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
 * Two more uses of the same selection, which save no file at all:
 *
 *   - screenshot-ocr:   drag a region, read its text with tesseract
 *                       and put the text on the clipboard
 *   - screenshot-color: click a pixel, put its colour on the clipboard
 *                       as #rrggbb
 *
 * Configuration (YAML):
 *   modules:
 *     screenshot:
 *       enabled: true
 *       save-directory: ~/Pictures/Screenshots
 *       filename-format: screenshot_%Y%m%d_%H%M%S
 *       copy-to-clipboard: true
 *       ocr-command: tesseract      # given IMAGE stdout -l LANG
 *       ocr-language: eng           # tesseract's -l: eng, deu, eng+deu
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-screenshot"

#include <glib-object.h>
#include <glib/gstdio.h>
#include <gmodule.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
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
#include "interfaces/gowl-ipc-handler.h"
#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"
#include "core/gowl-seat.h"
#include "boxed/gowl-capture-result.h"
#include "ipc/gowl-ipc.h"
#include "util/gowl-subprocess.h"

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
	struct wlr_scene_rect *sel_dim;       /* armed indicator, whole layout */
	struct wlr_scene_rect *sel_fill;      /* semi-transparent fill */
	struct wlr_scene_rect *sel_border[4]; /* top, bottom, left, right */

	/* Window picking: click the window to capture rather than taking
	   whatever happens to be focused. */
	gboolean    picking;
	gpointer    pick_client;              /* GowlClient*, unowned */

	/* Async completion */
	GowlScreenshotCallback finish_cb;
	gpointer    finish_data;

	/* What the area selection is FOR: a picture (the default), or the
	   text in it.  An OCR selection saves no file. */
	gboolean    for_ocr;
	GowlSubprocess *ocr_proc;     /* tesseract, while it reads */
	gchar      *ocr_image;        /* its temporary input, unlinked after */

	/* Colour picking: the next left click samples one pixel. */
	gboolean    color_picking;

	/* Configuration */
	gchar      *save_directory;
	gchar      *filename_format;
	gboolean    copy_to_clipboard;
	gchar      *ocr_command;
	gchar      *ocr_language;
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
static void screenshot_ipc_init        (GowlIpcHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleScreenshot, gowl_module_screenshot,
    GOWL_TYPE_MODULE,
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCREENSHOT_PROVIDER, screenshot_provider_init)
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, screenshot_startup_init)
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, screenshot_shutdown_init)
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_KEYBIND_HANDLER, screenshot_keybind_init)
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_MOUSE_HANDLER, screenshot_mouse_init)
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER, screenshot_ipc_init))

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

/*
 * The bounding box of every monitor.  Used for the dim wash, which has
 * to cover the whole desktop rather than one output --- a selection
 * spanning two screens is perfectly ordinary.
 */
static void
layout_extent(GowlModuleScreenshot *self, gint *x, gint *y, gint *w, gint *h)
{
	GList *monitors, *l;
	gint   x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	gboolean first = TRUE;

	*x = *y = 0;
	*w = *h = 0;

	if (self->compositor == NULL)
		return;

	monitors = gowl_compositor_get_monitors(self->compositor);
	for (l = monitors; l != NULL; l = l->next) {
		gint mx, my, mw, mh;

		gowl_monitor_get_geometry(GOWL_MONITOR(l->data),
		                          &mx, &my, &mw, &mh);
		if (mw <= 0 || mh <= 0)
			continue;
		if (first) {
			x0 = mx; y0 = my; x1 = mx + mw; y1 = my + mh;
			first = FALSE;
			continue;
		}
		if (mx < x0) x0 = mx;
		if (my < y0) y0 = my;
		if (mx + mw > x1) x1 = mx + mw;
		if (my + mh > y1) y1 = my + mh;
	}

	if (first)
		return;

	*x = x0;
	*y = y0;
	*w = x1 - x0;
	*h = y1 - y0;
}

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

	/*
	 * A dim wash over the whole layout, shown the INSTANT the mode
	 * arms.  Without it nothing was drawn until the first press, so
	 * an armed selection and a keybind that did nothing looked
	 * identical -- and since the input that sets the anchor never
	 * arrived, it was always the second one.
	 *
	 * It is destroyed before any capture, so it never appears in the
	 * image.
	 */
	{
		float dim_color[4] = { 0.0f, 0.0f, 0.0f, 0.25f };
		gint  lx, ly, lw, lh;

		layout_extent(self, &lx, &ly, &lw, &lh);
		self->sel_dim = wlr_scene_rect_create(overlay, lw, lh,
		                                      dim_color);
		wlr_scene_node_set_position(&self->sel_dim->node, lx, ly);
		wlr_scene_node_set_enabled(&self->sel_dim->node, TRUE);
	}

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

	if (self->sel_dim != NULL) {
		wlr_scene_node_destroy(&self->sel_dim->node);
		self->sel_dim = NULL;
	}

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

/*
 * Put the capture on the clipboard, if the config asks for it.
 *
 * `copy-to-clipboard' was parsed and defaulted to TRUE from the day
 * this module was written and then never read by anything, so every
 * screenshot went to disk and nowhere else.
 *
 * The PNG is read back from the file we just wrote rather than encoded
 * a second time in memory: cairo's writer takes a path, the bytes are
 * still in the page cache, and one encode is enough.  The clipboard
 * itself is gowl's own selection -- no wl-copy, no second process
 * holding the offer, and the write to the pasting client is drained by
 * the main loop rather than on this thread.
 */
static void
copy_result_to_clipboard(GowlModuleScreenshot *self, const gchar *path)
{
	g_autofree gchar  *contents = NULL;
	g_autoptr(GBytes)  bytes = NULL;
	g_autoptr(GError)  error = NULL;
	GowlSeat          *seat;
	gsize              length = 0;

	if (!self->copy_to_clipboard || path == NULL || self->compositor == NULL)
		return;

	seat = gowl_compositor_get_seat(self->compositor);
	if (seat == NULL)
		return;

	if (!g_file_get_contents(path, &contents, &length, &error)) {
		g_warning("gowl-screenshot: cannot read back '%s': %s",
		          path, error->message);
		return;
	}

	bytes = g_bytes_new_take(g_steal_pointer(&contents), length);
	gowl_seat_set_clipboard_bytes(seat, bytes, "image/png");
}

static void
deliver_result(GowlModuleScreenshot *self, GowlCaptureResult *result)
{
	if (!gowl_capture_result_is_cancelled(result))
		copy_result_to_clipboard(self,
			gowl_capture_result_get_path(result));

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

/*
 * Window picking.
 *
 * `Window' used to mean whatever happened to be focused, which is
 * almost never the window you want a picture of: clicking the bar
 * button, or pressing the keybind, focuses or is dispatched from
 * somewhere else entirely, and you get a shot of the thing you were
 * using to ask for the shot.  So the mode now arms, highlights the
 * window under the pointer, and captures the one you click.
 */
static void
update_pick_highlight(GowlModuleScreenshot *self)
{
	GowlClient *c = self->pick_client;
	gint        x, y, w, h;

	if (self->sel_fill == NULL)
		return;

	if (c == NULL) {
		wlr_scene_node_set_enabled(&self->sel_fill->node, FALSE);
		return;
	}

	gowl_client_get_geometry(c, &x, &y, &w, &h);
	if (w < 1 || h < 1) {
		wlr_scene_node_set_enabled(&self->sel_fill->node, FALSE);
		return;
	}

	wlr_scene_rect_set_size(self->sel_fill, w, h);
	wlr_scene_node_set_position(&self->sel_fill->node, x, y);
	wlr_scene_node_set_enabled(&self->sel_fill->node, TRUE);
}

static void
finish_window_pick(GowlModuleScreenshot *self, gboolean cancelled)
{
	GowlCaptureResult *result;
	GowlClient        *client = self->pick_client;
	GBytes            *data = NULL;
	gint               w = 0, h = 0;

	self->picking = FALSE;
	self->pick_client = NULL;
	destroy_overlay(self);
	g_signal_emit(self, screenshot_signals[SIGNAL_SELECTION_ACTIVE],
	              0, FALSE);

	if (cancelled || client == NULL) {
		deliver_result(self,
			gowl_capture_result_new(NULL, 0, 0, 0, NULL, TRUE));
		return;
	}

	/* The overlay is gone by now, so the highlight is not in the
	   picture -- the same reason the area path destroys it first. */
	data = gowl_compositor_screenshot_client(self->compositor, client,
	                                         &w, &h, NULL);
	if (data != NULL) {
		g_autofree gchar *path = generate_filename(self);

		gowl_compositor_save_png(data, w, h, path, NULL);
		result = gowl_capture_result_new(data, w, h, w * 4, path,
		                                 FALSE);
		g_bytes_unref(data);
	} else {
		result = gowl_capture_result_new(NULL, 0, 0, 0, NULL, TRUE);
	}
	deliver_result(self, result);
}

/* ----------------------------------------------------------------
 * Reading text (OCR) and colours off the screen
 *
 * Both put text on the clipboard rather than an image, so both say
 * what they put there -- a toast, and an IPC event a script or cmacs
 * can follow -- because a clipboard you cannot see is a clipboard you
 * do not trust.
 * ---------------------------------------------------------------- */

/* A toast (bar-notify when the bar is loaded) and the compositor's
   toast signal, the macro module's two routes. */
static void
grab_notify(
	GowlModuleScreenshot *self,
	const gchar          *summary,
	const gchar          *body
){
	g_autofree gchar *s = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *line = NULL;

	if (self->compositor == NULL)
		return;
	s = g_strdelimit(g_strdup(summary), "|\n", ' ');
	b = g_strdelimit(g_strdup(body != NULL ? body : ""), "|\n", ' ');
	line = g_strdup_printf("bar-notify %s|%s", s, b);
	g_free(gowl_compositor_run_command(self->compositor, line));
	g_signal_emit_by_name(self->compositor, "toast-requested",
	                      gowl_compositor_get_selected_monitor(self->compositor),
	                      s);
}

static void G_GNUC_PRINTF(2, 3)
grab_event(
	GowlModuleScreenshot *self,
	const gchar          *format,
	...
){
	GowlIpc *ipc;
	g_autofree gchar *msg = NULL;
	va_list ap;

	if (self->compositor == NULL)
		return;
	ipc = gowl_compositor_get_ipc(self->compositor);
	if (ipc == NULL)
		return;
	va_start(ap, format);
	msg = g_strdup_vprintf(format, ap);
	va_end(ap);
	gowl_ipc_push_event(ipc, "EVENT screenshot %s", msg);
}

static void
ocr_forget_image(
	GowlModuleScreenshot *self
){
	if (self->ocr_image != NULL) {
		g_unlink(self->ocr_image);
		g_clear_pointer(&self->ocr_image, g_free);
	}
}

/* tesseract finished: its stdout is the text. */
static void
on_ocr_done(
	gint         status,
	const gchar *out,
	const gchar *err,
	gpointer     data
){
	GowlModuleScreenshot *self = data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *body = NULL;
	GowlSeat *seat;
	glong chars;

	self->ocr_proc = NULL;
	ocr_forget_image(self);

	if (status == -1 || !g_spawn_check_wait_status(status, &error)) {
		g_autofree gchar *why = g_strdup_printf("%s%s%s",
			status == -1 ? "it took too long" : error->message,
			err != NULL && *err != '\0' ? ": " : "",
			err != NULL ? err : "");

		g_warning("gowl-screenshot: OCR failed: %s", why);
		grab_notify(self, "Text from screen", why);
		grab_event(self, "ocr-failed");
		return;
	}

	/* tesseract ends every page with a form feed and blank lines */
	text = g_strdelimit(g_strdup(out != NULL ? out : ""), "\f", '\n');
	g_strstrip(text);
	if (*text == '\0') {
		grab_notify(self, "Text from screen", "no text found there");
		grab_event(self, "ocr 0");
		return;
	}
	seat = gowl_compositor_get_seat(self->compositor);
	if (seat != NULL)
		gowl_seat_set_clipboard(seat, text);
	chars = g_utf8_strlen(text, -1);
	body = g_strdup_printf("%ld character%s copied", chars,
	                       chars == 1 ? "" : "s");
	grab_notify(self, "Text from screen", body);
	grab_event(self, "ocr %ld", chars);
}

/*
 * ocr_region:
 *
 * The OCR half of an area selection: the region goes to a private
 * temporary PNG (never the screenshots folder), tesseract reads it on
 * the side, and on_ocr_done() takes it from there.
 */
static void
ocr_region(
	GowlModuleScreenshot *self,
	gint                  x,
	gint                  y,
	gint                  w,
	gint                  h
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) data = NULL;
	g_autoptr(GPtrArray) argv = NULL;
	g_auto(GStrv) command = NULL;
	struct wl_event_loop *loop;
	gint out_w;
	gint out_h;
	gint fd;
	guint i;

	data = gowl_compositor_screenshot_region(self->compositor, NULL,
	           x, y, w, h, &out_w, &out_h, &error);
	if (data == NULL) {
		grab_notify(self, "Text from screen",
		            error != NULL ? error->message : "capture failed");
		return;
	}

	/* $XDG_RUNTIME_DIR: per-user, 0700, in memory -- a picture of
	   what was on the screen should not land anywhere more public */
	self->ocr_image = g_build_filename(g_get_user_runtime_dir(),
	                                   "gowl-ocr-XXXXXX.png", NULL);
	fd = g_mkstemp_full(self->ocr_image, O_RDWR, 0600);
	if (fd < 0) {
		g_autofree gchar *why = g_strdup_printf("cannot create %s: %s",
			self->ocr_image, g_strerror(errno));

		g_clear_pointer(&self->ocr_image, g_free);
		grab_notify(self, "Text from screen", why);
		return;
	}
	close(fd);
	if (!gowl_compositor_save_png(data, out_w, out_h, self->ocr_image,
	                              &error)) {
		ocr_forget_image(self);
		grab_notify(self, "Text from screen", error->message);
		return;
	}

	if (!g_shell_parse_argv(self->ocr_command, NULL, &command, &error)) {
		ocr_forget_image(self);
		grab_notify(self, "Text from screen", error->message);
		return;
	}
	argv = g_ptr_array_new();
	for (i = 0; command[i] != NULL; i++)
		g_ptr_array_add(argv, command[i]);
	g_ptr_array_add(argv, self->ocr_image);
	g_ptr_array_add(argv, (gpointer)"stdout");
	g_ptr_array_add(argv, (gpointer)"-l");
	g_ptr_array_add(argv, self->ocr_language);
	g_ptr_array_add(argv, NULL);

	loop = gowl_compositor_get_event_loop(self->compositor);
	self->ocr_proc = gowl_subprocess_spawn(loop,
		(const gchar * const *)argv->pdata, 60000, on_ocr_done, self,
		&error);
	if (self->ocr_proc == NULL) {
		g_autofree gchar *why = g_strdup_printf("%s -- is tesseract "
			"installed? (the tesseract package, plus a language "
			"pack such as tesseract-langpack-eng)", error->message);

		ocr_forget_image(self);
		grab_notify(self, "Text from screen", why);
		return;
	}
	grab_event(self, "ocr-reading");
}

/* Ends colour picking: the overlay goes before the sample is taken, so
   the frame is never what gets sampled. */
static void
finish_color_pick(
	GowlModuleScreenshot *self,
	gboolean              cancelled
){
	g_autoptr(GBytes) data = NULL;
	g_autofree gchar *hex = NULL;
	g_autofree gchar *body = NULL;
	const guint8 *px;
	gsize len = 0;
	gint out_w = 0;
	gint out_h = 0;
	GowlSeat *seat;

	self->color_picking = FALSE;
	destroy_overlay(self);
	g_signal_emit(self, screenshot_signals[SIGNAL_SELECTION_ACTIVE],
	              0, FALSE);
	if (cancelled)
		return;

	data = gowl_compositor_screenshot_region(self->compositor, NULL,
	           (gint)self->sel_current_x, (gint)self->sel_current_y, 1, 1,
	           &out_w, &out_h, NULL);
	px = data != NULL ? g_bytes_get_data(data, &len) : NULL;
	if (px == NULL || len < 4) {
		grab_notify(self, "Colour", "could not read that pixel");
		return;
	}
	/* The capture is ARGB8888 little-endian: B, G, R, A in memory (see
	   gowl_compositor_save_png()).  A scaled output gives more than one
	   device pixel for the one layout pixel; the first is the one
	   under the hot spot. */
	hex = g_strdup_printf("#%02x%02x%02x", px[2], px[1], px[0]);
	seat = gowl_compositor_get_seat(self->compositor);
	if (seat != NULL)
		gowl_seat_set_clipboard(seat, hex);
	body = g_strdup_printf("%s copied (rgb %u, %u, %u)", hex,
	                       px[2], px[1], px[0]);
	grab_notify(self, "Colour", body);
	grab_event(self, "color %s", hex);
}

/* The armed look for the colour picker: a thin frame round the layout,
   no dim -- a dim wash would change every colour on the screen. */
static void
create_color_overlay(
	GowlModuleScreenshot *self
){
	struct wlr_scene_tree *overlay;
	float frame[4] = { 0.95f, 0.75f, 0.2f, 0.9f };
	gint lx, ly, lw, lh;
	gint bw = 3;
	gint i;

	overlay = gowl_compositor_get_scene_layer(self->compositor,
	                                          GOWL_SCENE_LAYER_OVERLAY);
	if (overlay == NULL)
		return;
	layout_extent(self, &lx, &ly, &lw, &lh);
	for (i = 0; i < 4; i++)
		self->sel_border[i] = wlr_scene_rect_create(overlay,
			(i < 2) ? lw : bw, (i < 2) ? bw : lh, frame);
	wlr_scene_node_set_position(&self->sel_border[0]->node, lx, ly);
	wlr_scene_node_set_position(&self->sel_border[1]->node, lx,
	                            ly + lh - bw);
	wlr_scene_node_set_position(&self->sel_border[2]->node, lx, ly);
	wlr_scene_node_set_position(&self->sel_border[3]->node,
	                            lx + lw - bw, ly);
}

static void
finish_area_selection(GowlModuleScreenshot *self)
{
	GBytes *data;
	GowlCaptureResult *result;
	gint x, y, w, h, out_w, out_h;

	if (!self->anchor_set) {
		/* Cancelled before anchor was placed */
		self->for_ocr = FALSE;
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

	if (self->for_ocr) {
		/* Text, not a picture: no file, no image on the clipboard,
		   and nothing for a provider caller -- OCR is IPC-only. */
		self->for_ocr = FALSE;
		if (w >= 1 && h >= 1)
			ocr_region(self, x, y, w, h);
		return;
	}

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
			/*
			 * No client named: arm the picker rather than
			 * falling back to the focused window.  Whatever
			 * asked for this -- a keybind, a bar button -- is
			 * itself why something else is focused, so the
			 * focused window is the one answer that is almost
			 * certainly wrong.
			 */
			self->picking = TRUE;
			self->pick_client = NULL;
			create_overlay(self);
			g_signal_emit(self,
			              screenshot_signals[SIGNAL_SELECTION_ACTIVE],
			              0, TRUE);
			return;
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

/* Picking counts as selecting: both are interactive modes with an
   overlay up, and every caller asks this to find out whether one is in
   progress. */
static gboolean
screenshot_is_selecting(GowlScreenshotProvider *provider)
{
	GowlModuleScreenshot *self = GOWL_MODULE_SCREENSHOT(provider);

	return self->selecting || self->picking || self->color_picking;
}

static void
screenshot_cancel(GowlScreenshotProvider *provider)
{
	GowlModuleScreenshot *self = GOWL_MODULE_SCREENSHOT(provider);
	GowlCaptureResult *result;

	if (self->picking) {
		finish_window_pick(self, TRUE);
		return;
	}
	if (self->color_picking) {
		finish_color_pick(self, TRUE);
		return;
	}

	if (!self->selecting)
		return;

	self->selecting = FALSE;
	self->anchor_set = FALSE;
	if (self->for_ocr) {
		/* nobody asked for a picture, so nobody is told it was
		   cancelled */
		self->for_ocr = FALSE;
		destroy_overlay(self);
		g_signal_emit(self, screenshot_signals[SIGNAL_SELECTION_ACTIVE],
		              0, FALSE);
		return;
	}
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

	if (self->selecting || self->color_picking) {
		self->selecting = FALSE;
		self->anchor_set = FALSE;
		self->for_ocr = FALSE;
		self->color_picking = FALSE;
		destroy_overlay(self);
	}
	/* tesseract is watched on the compositor's event loop, which is
	   about to go: stop it while the loop is still there. */
	if (self->ocr_proc != NULL) {
		gowl_subprocess_cancel(self->ocr_proc);
		self->ocr_proc = NULL;
	}
	ocr_forget_image(self);

	self->compositor = NULL;
}

static void
screenshot_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = screenshot_on_shutdown;
}

/* ----------------------------------------------------------------
 * GowlIpcHandler interface
 * ---------------------------------------------------------------- */

/*
 * The module's entry points, reachable from a keybind
 * (`{ action: ipc_command, arg: "screenshot-area" }'), from the IPC
 * socket, and from an embedder.  A module .so cannot export a function
 * for the compositor to call, so it exports a name instead --- the
 * same shape expo and the switcher use.
 *
 * This is what makes Super+Shift+S possible without any of the
 * keybind, the CLI or cmacs knowing that this module exists.
 */
static gchar *
screenshot_handle_command(GowlIpcHandler *handler, const gchar *command,
                          const gchar *args)
{
	GowlModuleScreenshot *self = GOWL_MODULE_SCREENSHOT(handler);
	GowlCaptureMode       mode;
	gpointer              client = NULL;

	if (command == NULL || self->compositor == NULL)
		return NULL;

	if (g_strcmp0(command, "screenshot-cancel") == 0) {
		if (!self->selecting)
			return g_strdup("ERROR no selection in progress");
		screenshot_cancel(GOWL_SCREENSHOT_PROVIDER(self));
		return g_strdup("OK selection cancelled");
	}

	if (g_strcmp0(command, "screenshot-ocr") == 0) {
		if (self->selecting || self->picking || self->color_picking)
			return g_strdup("ERROR a selection is already in progress");
		if (self->ocr_proc != NULL)
			return g_strdup("ERROR still reading the last region");
		self->for_ocr = TRUE;
		self->selecting = TRUE;
		self->anchor_set = FALSE;
		create_overlay(self);
		g_signal_emit(self, screenshot_signals[SIGNAL_SELECTION_ACTIVE],
		              0, TRUE);
		return g_strdup("OK drag over the text, Escape to cancel");
	}
	if (g_strcmp0(command, "screenshot-color") == 0
	    || g_strcmp0(command, "screenshot-colour") == 0) {
		if (self->selecting || self->picking || self->color_picking)
			return g_strdup("ERROR a selection is already in progress");
		self->color_picking = TRUE;
		create_color_overlay(self);
		g_signal_emit(self, screenshot_signals[SIGNAL_SELECTION_ACTIVE],
		              0, TRUE);
		return g_strdup("OK click a pixel, Escape to cancel");
	}

	if (g_strcmp0(command, "screenshot-area") == 0
	    || g_strcmp0(command, "screenshot-select") == 0) {
		/* Starting a second rubber band while one is up would
		   strand the first one's callback, and the user cannot
		   see two selections at once anyway. */
		if (self->selecting)
			return g_strdup("ERROR a selection is already in progress");
		mode = GOWL_CAPTURE_MODE_AREA;
	} else if (g_strcmp0(command, "screenshot-window") == 0) {
		/* NULL client: arm the picker.  Naming the focused window
		   here would photograph whatever the keybind was pressed
		   from, which is the one window nobody wants a picture of. */
		if (self->picking)
			return g_strdup("ERROR a window pick is already in progress");
		mode = GOWL_CAPTURE_MODE_WINDOW;
	} else if (g_strcmp0(command, "screenshot-screen") == 0
	           || g_strcmp0(command, "screenshot") == 0) {
		mode = GOWL_CAPTURE_MODE_DESKTOP;
	} else if (g_strcmp0(command, "screenshot-all") == 0) {
		mode = GOWL_CAPTURE_MODE_ALL;
	} else {
		return NULL;
	}

	/*
	 * No callback: the module saves the file and sets the clipboard
	 * itself, and an area capture completes long after this returns.
	 * The reply says what was STARTED, which for the area mode is all
	 * that can honestly be said at this point.
	 */
	screenshot_capture(GOWL_SCREENSHOT_PROVIDER(self), mode,
	                   (args != NULL && *args != '\0') ? args : NULL,
	                   client, NULL, NULL);

	if (mode == GOWL_CAPTURE_MODE_AREA)
		return g_strdup("OK drag to select, Escape to cancel");
	if (mode == GOWL_CAPTURE_MODE_WINDOW)
		return g_strdup("OK click a window, Escape to cancel");
	return g_strdup("OK capture taken");
}

static void
screenshot_ipc_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = screenshot_handle_command;
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

	if (self->picking) {
		if (pressed && keysym == XKB_KEY_Escape)
			finish_window_pick(self, TRUE);
		return TRUE;
	}
	if (self->color_picking) {
		if (pressed && keysym == XKB_KEY_Escape)
			finish_color_pick(self, TRUE);
		return TRUE;
	}

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

	if (self->picking) {
		if (button == 0x111 && state == 1)          /* BTN_RIGHT */
			finish_window_pick(self, TRUE);
		else if (button == 0x110 && state == 0)     /* BTN_LEFT up */
			finish_window_pick(self, self->pick_client == NULL);
		/* Every button is consumed while picking: a press that
		   reached the client under the cursor would raise or focus
		   the very window we are about to photograph. */
		return TRUE;
	}
	if (self->color_picking) {
		/* Sampled on release, like the window pick: the press is
		   consumed too, so nothing under it is clicked. */
		if (button == 0x111 && state == 1)          /* BTN_RIGHT */
			finish_color_pick(self, TRUE);
		else if (button == 0x110 && state == 0)     /* BTN_LEFT up */
			finish_color_pick(self, FALSE);
		return TRUE;
	}

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

	if (self->picking) {
		GowlClient *c;

		self->sel_current_x = x;
		self->sel_current_y = y;
		c = gowl_compositor_client_at(self->compositor, x, y);
		if (c != self->pick_client) {
			self->pick_client = c;
			update_pick_highlight(self);
		}
		return TRUE;
	}

	if (self->color_picking) {
		self->sel_current_x = x;
		self->sel_current_y = y;
		return TRUE;
	}

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

	if (self->selecting || self->color_picking) {
		self->selecting = FALSE;
		self->anchor_set = FALSE;
		self->for_ocr = FALSE;
		self->color_picking = FALSE;
		destroy_overlay(self);
	}
	if (self->ocr_proc != NULL) {
		gowl_subprocess_cancel(self->ocr_proc);
		self->ocr_proc = NULL;
	}
	ocr_forget_image(self);
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

	val = g_hash_table_lookup(settings, "ocr-command");
	if (val != NULL && *val != '\0') {
		g_free(self->ocr_command);
		self->ocr_command = g_strdup(val);
	}

	val = g_hash_table_lookup(settings, "ocr-language");
	if (val != NULL && *val != '\0') {
		g_free(self->ocr_language);
		self->ocr_language = g_strdup(val);
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
	g_free(self->ocr_command);
	g_free(self->ocr_language);
	/* ocr_proc: cancelled by the shutdown handler and by deactivate,
	   while the event loop it is watched on still exists.  Touching it
	   here could reach a loop already destroyed. */
	ocr_forget_image(self);
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
	self->ocr_command      = g_strdup("tesseract");
	self->ocr_language     = g_strdup("eng");

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
