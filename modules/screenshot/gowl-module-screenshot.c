/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * GowlModuleScreenshot:
 *
 * A compositor module that captures screenshots in four modes:
 *
 *   - desktop:  capture the current (or named) monitor output
 *   - window:   capture a specific client surface
 *   - area:     interactive rubber-band region selection
 *   - all:      stitch all monitors into a single image
 *
 * Screenshots are saved as PNG files (via cairo) and optionally
 * copied to the Wayland clipboard.  The module implements
 * #GowlScreenshotProvider so that other modules (e.g. recording)
 * can reuse the area selection mechanism.
 *
 * Configuration (YAML):
 *   modules:
 *     screenshot:
 *       enabled: true
 *       save-directory: ~/Pictures/Screenshots
 *       filename-format: screenshot_%Y%m%d_%H%M%S
 *       copy-to-clipboard: true
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-screenshot"

#include <glib-object.h>
#include <gmodule.h>
#include <string.h>
#include <time.h>
#include <wordexp.h>

#include <wlr/types/wlr_scene.h>
#include <xkbcommon/xkbcommon.h>

#include "gowl-enums.h"
#include "module/gowl-module.h"
#include "interfaces/gowl-screenshot-provider.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "interfaces/gowl-keybind-handler.h"
#include "interfaces/gowl-mouse-handler.h"
#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"
#include "boxed/gowl-capture-result.h"

/* ----------------------------------------------------------------
 * Module type declaration
 * ---------------------------------------------------------------- */

#define GOWL_TYPE_MODULE_SCREENSHOT (gowl_module_screenshot_get_type())

G_DECLARE_FINAL_TYPE(GowlModuleScreenshot, gowl_module_screenshot,
                     GOWL, MODULE_SCREENSHOT, GowlModule)

struct _GowlModuleScreenshot {
	GowlModule  parent_instance;

	/* Borrowed references (set on startup, cleared on shutdown) */
	GowlCompositor *compositor;

	/* Interactive area selection state */
	gboolean    selecting;
	gboolean    anchor_set;       /* TRUE after first button press */
	gdouble     sel_start_x;
	gdouble     sel_start_y;
	gdouble     sel_current_x;
	gdouble     sel_current_y;

	/* Overlay scene rects for rubber-band visualization */
	struct wlr_scene_rect *sel_fill;      /* semi-transparent fill */
	struct wlr_scene_rect *sel_border[4]; /* top, bottom, left, right */

	/* Async completion */
	GowlScreenshotCallback finish_cb;
	gpointer    finish_data;

	/* Configuration */
	gchar      *save_directory;
	gchar      *filename_format;
	gboolean    copy_to_clipboard;
};

/* Signal IDs */
enum {
	SIGNAL_CAPTURE_STARTED,
	SIGNAL_CAPTURE_COMPLETE,
	SIGNAL_SELECTION_ACTIVE,
	N_SIGNALS
};

static guint screenshot_signals[N_SIGNALS] = { 0, };

/* ----------------------------------------------------------------
 * Interface forward declarations
 * ---------------------------------------------------------------- */

static void screenshot_provider_init   (GowlScreenshotProviderInterface *iface);
static void screenshot_startup_init    (GowlStartupHandlerInterface *iface);
static void screenshot_shutdown_init   (GowlShutdownHandlerInterface *iface);
static void screenshot_keybind_init    (GowlKeybindHandlerInterface *iface);
static void screenshot_mouse_init      (GowlMouseHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleScreenshot, gowl_module_screenshot,
    GOWL_TYPE_MODULE,
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCREENSHOT_PROVIDER, screenshot_provider_init)
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, screenshot_startup_init)
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, screenshot_shutdown_init)
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_KEYBIND_HANDLER, screenshot_keybind_init)
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_MOUSE_HANDLER, screenshot_mouse_init))

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

static gchar *
expand_path(const gchar *path)
{
	wordexp_t we;
	gchar *result;

	if (wordexp(path, &we, WRDE_NOCMD) != 0)
		return g_strdup(path);

	result = g_strdup(we.we_wordv[0]);
	wordfree(&we);
	return result;
}

static gchar *
generate_filename(GowlModuleScreenshot *self)
{
	g_autofree gchar *dir = NULL;
	gchar timebuf[64];
	time_t t;
	struct tm tm;

	dir = expand_path(self->save_directory);
	g_mkdir_with_parents(dir, 0755);

	t = time(NULL);
	localtime_r(&t, &tm);
	strftime(timebuf, sizeof(timebuf), self->filename_format, &tm);

	return g_strdup_printf("%s/%s.png", dir, timebuf);
}

/* ----------------------------------------------------------------
 * Overlay management for area selection
 * ---------------------------------------------------------------- */

static void
create_overlay(GowlModuleScreenshot *self)
{
	struct wlr_scene_tree *overlay;
	float fill_color[4]   = { 0.2f, 0.5f, 0.8f, 0.15f };
	float border_color[4] = { 0.2f, 0.5f, 0.8f, 0.8f };
	gint i;

	overlay = gowl_compositor_get_scene_layer(
	              self->compositor, GOWL_SCENE_LAYER_OVERLAY);
	if (overlay == NULL)
		return;

	self->sel_fill = wlr_scene_rect_create(overlay, 0, 0, fill_color);
	wlr_scene_node_set_enabled(&self->sel_fill->node, FALSE);

	for (i = 0; i < 4; i++) {
		self->sel_border[i] = wlr_scene_rect_create(overlay, 0, 0,
		                                             border_color);
		wlr_scene_node_set_enabled(&self->sel_border[i]->node, FALSE);
	}
}

static void
destroy_overlay(GowlModuleScreenshot *self)
{
	gint i;

	if (self->sel_fill != NULL) {
		wlr_scene_node_destroy(&self->sel_fill->node);
		self->sel_fill = NULL;
	}

	for (i = 0; i < 4; i++) {
		if (self->sel_border[i] != NULL) {
			wlr_scene_node_destroy(&self->sel_border[i]->node);
			self->sel_border[i] = NULL;
		}
	}
}

static void
update_overlay(GowlModuleScreenshot *self)
{
	gint x, y, w, h, bw;

	if (self->sel_fill == NULL || !self->anchor_set)
		return;

	/* Compute rectangle from anchor and current position */
	if (self->sel_current_x >= self->sel_start_x) {
		x = (gint)self->sel_start_x;
		w = (gint)(self->sel_current_x - self->sel_start_x);
	} else {
		x = (gint)self->sel_current_x;
		w = (gint)(self->sel_start_x - self->sel_current_x);
	}

	if (self->sel_current_y >= self->sel_start_y) {
		y = (gint)self->sel_start_y;
		h = (gint)(self->sel_current_y - self->sel_start_y);
	} else {
		y = (gint)self->sel_current_y;
		h = (gint)(self->sel_start_y - self->sel_current_y);
	}

	if (w < 1) w = 1;
	if (h < 1) h = 1;
	bw = 2; /* border width in pixels */

	/* Fill rect */
	wlr_scene_rect_set_size(self->sel_fill, w, h);
	wlr_scene_node_set_position(&self->sel_fill->node, x, y);
	wlr_scene_node_set_enabled(&self->sel_fill->node, TRUE);

	/* Top border */
	wlr_scene_rect_set_size(self->sel_border[0], w + bw * 2, bw);
	wlr_scene_node_set_position(&self->sel_border[0]->node,
	                            x - bw, y - bw);
	wlr_scene_node_set_enabled(&self->sel_border[0]->node, TRUE);

	/* Bottom border */
	wlr_scene_rect_set_size(self->sel_border[1], w + bw * 2, bw);
	wlr_scene_node_set_position(&self->sel_border[1]->node,
	                            x - bw, y + h);
	wlr_scene_node_set_enabled(&self->sel_border[1]->node, TRUE);

	/* Left border */
	wlr_scene_rect_set_size(self->sel_border[2], bw, h);
	wlr_scene_node_set_position(&self->sel_border[2]->node,
	                            x - bw, y);
	wlr_scene_node_set_enabled(&self->sel_border[2]->node, TRUE);

	/* Right border */
	wlr_scene_rect_set_size(self->sel_border[3], bw, h);
	wlr_scene_node_set_position(&self->sel_border[3]->node,
	                            x + w, y);
	wlr_scene_node_set_enabled(&self->sel_border[3]->node, TRUE);
}

/* ----------------------------------------------------------------
 * Core capture logic
 * ---------------------------------------------------------------- */

static void
deliver_result(GowlModuleScreenshot *self, GowlCaptureResult *result)
{
	g_signal_emit(self, screenshot_signals[SIGNAL_CAPTURE_COMPLETE],
	              0, result);

	if (self->finish_cb != NULL) {
		GowlScreenshotCallback cb = self->finish_cb;
		gpointer data = self->finish_data;

		self->finish_cb = NULL;
		self->finish_data = NULL;
		cb(result, data);
	} else {
		gowl_capture_result_free(result);
	}
}

static void
finish_area_selection(GowlModuleScreenshot *self)
{
	GBytes *data;
	GowlCaptureResult *result;
	gint x, y, w, h, out_w, out_h;

	if (!self->anchor_set) {
		/* Cancelled before anchor was placed */
		result = gowl_capture_result_new(NULL, 0, 0, 0, NULL, TRUE);
		self->selecting = FALSE;
		self->anchor_set = FALSE;
		destroy_overlay(self);
		g_signal_emit(self, screenshot_signals[SIGNAL_SELECTION_ACTIVE],
		              0, FALSE);
		deliver_result(self, result);
		return;
	}

	/* Compute selection rect */
	if (self->sel_current_x >= self->sel_start_x) {
		x = (gint)self->sel_start_x;
		w = (gint)(self->sel_current_x - self->sel_start_x);
	} else {
		x = (gint)self->sel_current_x;
		w = (gint)(self->sel_start_x - self->sel_current_x);
	}
	if (self->sel_current_y >= self->sel_start_y) {
		y = (gint)self->sel_start_y;
		h = (gint)(self->sel_current_y - self->sel_start_y);
	} else {
		y = (gint)self->sel_current_y;
		h = (gint)(self->sel_start_y - self->sel_current_y);
	}

	self->selecting = FALSE;
	self->anchor_set = FALSE;
	destroy_overlay(self);
	g_signal_emit(self, screenshot_signals[SIGNAL_SELECTION_ACTIVE],
	              0, FALSE);

	if (w < 1 || h < 1) {
		result = gowl_capture_result_new(NULL, 0, 0, 0, NULL, TRUE);
		deliver_result(self, result);
		return;
	}

	data = gowl_compositor_screenshot_region(self->compositor,
	           NULL, x, y, w, h, &out_w, &out_h, NULL);

	if (data != NULL) {
		g_autofree gchar *path = generate_filename(self);
		gowl_compositor_save_png(data, out_w, out_h, path, NULL);
		result = gowl_capture_result_new(data, out_w, out_h,
		                                 out_w * 4, path, FALSE);
		g_bytes_unref(data);
	} else {
		result = gowl_capture_result_new(NULL, 0, 0, 0, NULL, TRUE);
	}

	deliver_result(self, result);
}

static void
do_capture(GowlModuleScreenshot *self,
           GowlCaptureMode       mode,
           const gchar          *output_name,
           gpointer              client)
{
	GBytes *data = NULL;
	GowlCaptureResult *result;
	gint w = 0, h = 0;

	g_signal_emit(self, screenshot_signals[SIGNAL_CAPTURE_STARTED],
	              0, mode);

	switch (mode) {
	case GOWL_CAPTURE_MODE_DESKTOP:
		data = gowl_compositor_screenshot_output(self->compositor,
		           output_name, &w, &h, NULL);
		break;
	case GOWL_CAPTURE_MODE_WINDOW:
		if (client != NULL) {
			data = gowl_compositor_screenshot_client(
			           self->compositor,
			           GOWL_CLIENT(client), &w, &h, NULL);
		} else {
			GowlClient *focused;

			focused = gowl_compositor_get_focused_client(
			              self->compositor);
			if (focused != NULL) {
				data = gowl_compositor_screenshot_client(
				           self->compositor,
				           focused, &w, &h, NULL);
			}
		}
		break;
	case GOWL_CAPTURE_MODE_ALL:
		data = gowl_compositor_screenshot_all(self->compositor,
		           &w, &h, NULL);
		break;
	case GOWL_CAPTURE_MODE_AREA:
		/* Area is handled asynchronously */
		self->selecting = TRUE;
		self->anchor_set = FALSE;
		create_overlay(self);
		g_signal_emit(self, screenshot_signals[SIGNAL_SELECTION_ACTIVE],
		              0, TRUE);
		return;
	}

	if (data != NULL) {
		g_autofree gchar *path = generate_filename(self);
		gowl_compositor_save_png(data, w, h, path, NULL);
		result = gowl_capture_result_new(data, w, h,
		                                 w * 4, path, FALSE);
		g_bytes_unref(data);
	} else {
		result = gowl_capture_result_new(NULL, 0, 0, 0, NULL, TRUE);
	}

	deliver_result(self, result);
}

/* ----------------------------------------------------------------
 * GowlScreenshotProvider interface
 * ---------------------------------------------------------------- */

static void
screenshot_capture(GowlScreenshotProvider *provider,
                   GowlCaptureMode         mode,
                   const gchar            *output_name,
                   gpointer                client,
                   GowlScreenshotCallback  cb,
                   gpointer                user_data)
{
	GowlModuleScreenshot *self = GOWL_MODULE_SCREENSHOT(provider);

	self->finish_cb = cb;
	self->finish_data = user_data;

	do_capture(self, mode, output_name, client);
}

static gboolean
screenshot_is_selecting(GowlScreenshotProvider *provider)
{
	return GOWL_MODULE_SCREENSHOT(provider)->selecting;
}

static void
screenshot_cancel(GowlScreenshotProvider *provider)
{
	GowlModuleScreenshot *self = GOWL_MODULE_SCREENSHOT(provider);
	GowlCaptureResult *result;

	if (!self->selecting)
		return;

	self->selecting = FALSE;
	self->anchor_set = FALSE;
	destroy_overlay(self);
	g_signal_emit(self, screenshot_signals[SIGNAL_SELECTION_ACTIVE],
	              0, FALSE);

	result = gowl_capture_result_new(NULL, 0, 0, 0, NULL, TRUE);
	deliver_result(self, result);
}

static void
screenshot_provider_init(GowlScreenshotProviderInterface *iface)
{
	iface->capture      = screenshot_capture;
	iface->is_selecting = screenshot_is_selecting;
	iface->cancel       = screenshot_cancel;
}

/* ----------------------------------------------------------------
 * GowlStartupHandler interface
 * ---------------------------------------------------------------- */

static void
screenshot_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleScreenshot *self = GOWL_MODULE_SCREENSHOT(handler);

	self->compositor = GOWL_COMPOSITOR(compositor);
}

static void
screenshot_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = screenshot_on_startup;
}

/* ----------------------------------------------------------------
 * GowlShutdownHandler interface
 * ---------------------------------------------------------------- */

static void
screenshot_on_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	GowlModuleScreenshot *self = GOWL_MODULE_SCREENSHOT(handler);

	(void)compositor;

	if (self->selecting) {
		self->selecting = FALSE;
		self->anchor_set = FALSE;
		destroy_overlay(self);
	}

	self->compositor = NULL;
}

static void
screenshot_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = screenshot_on_shutdown;
}

/* ----------------------------------------------------------------
 * GowlKeybindHandler interface (Escape cancels selection)
 * ---------------------------------------------------------------- */

static gboolean
screenshot_handle_key(GowlKeybindHandler *handler,
                      guint               modifiers,
                      guint               keysym,
                      gboolean            pressed)
{
	GowlModuleScreenshot *self = GOWL_MODULE_SCREENSHOT(handler);

	(void)modifiers;

	if (!self->selecting)
		return FALSE;

	if (pressed && keysym == XKB_KEY_Escape) {
		screenshot_cancel(GOWL_SCREENSHOT_PROVIDER(self));
		return TRUE;
	}

	/* Consume all keys during selection */
	return TRUE;
}

static void
screenshot_keybind_init(GowlKeybindHandlerInterface *iface)
{
	iface->handle_key = screenshot_handle_key;
}

/* ----------------------------------------------------------------
 * GowlMouseHandler interface (rubber-band selection)
 * ---------------------------------------------------------------- */

static gboolean
screenshot_handle_button(GowlMouseHandler *handler,
                         guint             button,
                         guint             state,
                         guint             modifiers)
{
	GowlModuleScreenshot *self = GOWL_MODULE_SCREENSHOT(handler);

	(void)modifiers;

	if (!self->selecting)
		return FALSE;

	/* Button 1 (left click) */
	if (button == 0x110) { /* BTN_LEFT */
		if (state == 1) { /* pressed */
			self->anchor_set = TRUE;
			self->sel_start_x = self->sel_current_x;
			self->sel_start_y = self->sel_current_y;
		} else { /* released */
			if (self->anchor_set)
				finish_area_selection(self);
		}
		return TRUE;
	}

	/* Button 3 (right click) cancels */
	if (button == 0x111) { /* BTN_RIGHT */
		if (state == 1)
			screenshot_cancel(GOWL_SCREENSHOT_PROVIDER(self));
		return TRUE;
	}

	return TRUE;
}

static gboolean
screenshot_handle_motion(GowlMouseHandler *handler,
                         gdouble           x,
                         gdouble           y)
{
	GowlModuleScreenshot *self = GOWL_MODULE_SCREENSHOT(handler);

	if (!self->selecting)
		return FALSE;

	self->sel_current_x = x;
	self->sel_current_y = y;

	if (self->anchor_set)
		update_overlay(self);

	return TRUE;
}

static void
screenshot_mouse_init(GowlMouseHandlerInterface *iface)
{
	iface->handle_button = screenshot_handle_button;
	iface->handle_motion = screenshot_handle_motion;
}

/* ----------------------------------------------------------------
 * GowlModule virtual methods
 * ---------------------------------------------------------------- */

static gboolean
screenshot_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
screenshot_deactivate(GowlModule *mod)
{
	GowlModuleScreenshot *self = GOWL_MODULE_SCREENSHOT(mod);

	if (self->selecting) {
		self->selecting = FALSE;
		self->anchor_set = FALSE;
		destroy_overlay(self);
	}
}

static const gchar *
screenshot_get_name(GowlModule *mod)
{
	(void)mod;
	return "screenshot";
}

static const gchar *
screenshot_get_description(GowlModule *mod)
{
	(void)mod;
	return "Screenshot capture with interactive area selection";
}

static const gchar *
screenshot_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

static void
screenshot_configure(GowlModule *mod, gpointer config)
{
	GowlModuleScreenshot *self = GOWL_MODULE_SCREENSHOT(mod);
	GHashTable *settings;
	const gchar *val;

	if (config == NULL)
		return;

	settings = (GHashTable *)config;

	val = g_hash_table_lookup(settings, "save-directory");
	if (val != NULL) {
		g_free(self->save_directory);
		self->save_directory = g_strdup(val);
	}

	val = g_hash_table_lookup(settings, "filename-format");
	if (val != NULL) {
		g_free(self->filename_format);
		self->filename_format = g_strdup(val);
	}

	val = g_hash_table_lookup(settings, "copy-to-clipboard");
	if (val != NULL) {
		self->copy_to_clipboard =
			(g_ascii_strcasecmp(val, "true") == 0 ||
			 g_ascii_strcasecmp(val, "1") == 0 ||
			 g_ascii_strcasecmp(val, "yes") == 0);
	}
}

/* ----------------------------------------------------------------
 * GObject lifecycle
 * ---------------------------------------------------------------- */

static void
gowl_module_screenshot_finalize(GObject *object)
{
	GowlModuleScreenshot *self = GOWL_MODULE_SCREENSHOT(object);

	g_free(self->save_directory);
	g_free(self->filename_format);
	destroy_overlay(self);

	G_OBJECT_CLASS(gowl_module_screenshot_parent_class)->finalize(object);
}

static void
gowl_module_screenshot_class_init(GowlModuleScreenshotClass *klass)
{
	GObjectClass    *object_class = G_OBJECT_CLASS(klass);
	GowlModuleClass *module_class = GOWL_MODULE_CLASS(klass);

	object_class->finalize = gowl_module_screenshot_finalize;

	module_class->activate       = screenshot_activate;
	module_class->deactivate     = screenshot_deactivate;
	module_class->get_name       = screenshot_get_name;
	module_class->get_description = screenshot_get_description;
	module_class->get_version    = screenshot_get_version;
	module_class->configure      = screenshot_configure;

	/**
	 * GowlModuleScreenshot::capture-started:
	 * @self: the screenshot module
	 * @mode: the #GowlCaptureMode that was requested
	 *
	 * Emitted when a capture operation begins.
	 */
	screenshot_signals[SIGNAL_CAPTURE_STARTED] =
		g_signal_new("capture-started",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, GOWL_TYPE_CAPTURE_MODE);

	/**
	 * GowlModuleScreenshot::capture-complete:
	 * @self: the screenshot module
	 * @result: (transfer none): the #GowlCaptureResult
	 *
	 * Emitted when a capture completes (including cancellation).
	 */
	screenshot_signals[SIGNAL_CAPTURE_COMPLETE] =
		g_signal_new("capture-complete",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, GOWL_TYPE_CAPTURE_RESULT);

	/**
	 * GowlModuleScreenshot::selection-active:
	 * @self: the screenshot module
	 * @active: %TRUE when entering, %FALSE when leaving selection
	 *
	 * Emitted when interactive area selection starts or ends.
	 */
	screenshot_signals[SIGNAL_SELECTION_ACTIVE] =
		g_signal_new("selection-active",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void
gowl_module_screenshot_init(GowlModuleScreenshot *self)
{
	self->compositor       = NULL;
	self->selecting        = FALSE;
	self->anchor_set       = FALSE;
	self->sel_fill         = NULL;
	self->finish_cb        = NULL;
	self->finish_data      = NULL;
	self->save_directory   = g_strdup("~/Pictures/Screenshots");
	self->filename_format  = g_strdup("screenshot_%Y%m%d_%H%M%S");
	self->copy_to_clipboard = TRUE;

	memset(self->sel_border, 0, sizeof(self->sel_border));
}

/* ----------------------------------------------------------------
 * Module entry point
 * ---------------------------------------------------------------- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_SCREENSHOT;
}
