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

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-bar"

#include <glib-object.h>
#include <gmodule.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include <cairo.h>
#include <pango/pangocairo.h>

#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

#include "gowl-enums.h"
#include "module/gowl-module.h"
#include "interfaces/gowl-bar-provider.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"

/* ----------------------------------------------------------------
 * Custom wlr_buffer for bar pixel data
 * ---------------------------------------------------------------- */

typedef struct {
	struct wlr_buffer base;
	guchar *pixels;
	gsize   size;
	gint    stride;
} BarBuffer;

static void
bar_buffer_destroy(struct wlr_buffer *wlr_buf)
{
	BarBuffer *buf = wl_container_of(wlr_buf, buf, base);
	g_free(buf->pixels);
	g_free(buf);
}

static bool
bar_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buf,
                                 uint32_t flags,
                                 void **data,
                                 uint32_t *format,
                                 size_t *stride)
{
	BarBuffer *buf = wl_container_of(wlr_buf, buf, base);
	*data   = buf->pixels;
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)buf->stride;
	return true;
}

static void
bar_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buf)
{
	(void)wlr_buf;
}

static const struct wlr_buffer_impl bar_buffer_impl = {
	.destroy = bar_buffer_destroy,
	.begin_data_ptr_access = bar_buffer_begin_data_ptr_access,
	.end_data_ptr_access = bar_buffer_end_data_ptr_access,
};

/* ----------------------------------------------------------------
 * Hex color parsing
 * ---------------------------------------------------------------- */

static void
parse_hex_color(const gchar *hex, gdouble rgba[4])
{
	guint r, g, b, a;

	a = 255;
	if (hex == NULL || hex[0] != '#') {
		rgba[0] = rgba[1] = rgba[2] = 0.0;
		rgba[3] = 1.0;
		return;
	}

	if (strlen(hex) == 9) {
		/* #RRGGBBAA */
		sscanf(hex, "#%2x%2x%2x%2x", &r, &g, &b, &a);
	} else if (strlen(hex) == 7) {
		/* #RRGGBB */
		sscanf(hex, "#%2x%2x%2x", &r, &g, &b);
	} else {
		rgba[0] = rgba[1] = rgba[2] = 0.0;
		rgba[3] = 1.0;
		return;
	}

	rgba[0] = (gdouble)r / 255.0;
	rgba[1] = (gdouble)g / 255.0;
	rgba[2] = (gdouble)b / 255.0;
	rgba[3] = (gdouble)a / 255.0;
}

/* ----------------------------------------------------------------
 * Module type
 * ---------------------------------------------------------------- */

#define GOWL_TYPE_MODULE_BAR (gowl_module_bar_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleBar, gowl_module_bar,
                     GOWL, MODULE_BAR, GowlModule)

typedef struct {
	struct wlr_scene_buffer *scene_buf;
	gint width;
	gint height;
	gint mon_x;
	gint mon_y;
} BarSurface;

struct _GowlModuleBar {
	GowlModule parent_instance;

	/* Config */
	gint     bar_height;
	gdouble  bg_color[4];
	gdouble  fg_color[4];
	gchar   *font_desc;   /* pango font description string */

	/* State */
	GHashTable *surfaces;  /* monitor name -> BarSurface* */
	gpointer    compositor;
	gchar      *custom_title; /* set by Elisp, overrides focused client */
	gulong      focus_handler_id;
	gulong      client_added_id;
	gulong      client_removed_id;
	struct wl_event_source *clock_timer;
};

static void bar_provider_iface_init(GowlBarProviderInterface *iface);
static void bar_startup_init(GowlStartupHandlerInterface *iface);
static void bar_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleBar, gowl_module_bar,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_BAR_PROVIDER,
		bar_provider_iface_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		bar_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER,
		bar_shutdown_init))

/* ----------------------------------------------------------------
 * Battery reading
 * ---------------------------------------------------------------- */

static gint
read_battery_percent(void)
{
	FILE *f;
	gint pct;

	f = fopen("/sys/class/power_supply/BAT0/capacity", "r");
	if (f == NULL)
		f = fopen("/sys/class/power_supply/BAT1/capacity", "r");
	if (f == NULL)
		return -1;

	if (fscanf(f, "%d", &pct) != 1)
		pct = -1;
	fclose(f);
	return pct;
}

/* ----------------------------------------------------------------
 * Rendering
 * ---------------------------------------------------------------- */

static BarBuffer *
bar_render(GowlModuleBar *self, gint width, gint height)
{
	cairo_surface_t *cs;
	cairo_t *cr;
	PangoLayout *layout;
	PangoFontDescription *font;
	BarBuffer *buf;
	guchar *pixels;
	gint stride;
	gint padding;
	gint text_y;
	gint bat;
	time_t now;
	struct tm *tm;
	char time_buf[64];
	char right_buf[128];
	PangoRectangle ink, logical;
	GowlClient *focused;
	const gchar *title;

	cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	cr = cairo_create(cs);

	/* Background */
	cairo_set_source_rgba(cr, self->bg_color[0], self->bg_color[1],
	                      self->bg_color[2], self->bg_color[3]);
	cairo_paint(cr);

	/* Set up pango */
	layout = pango_cairo_create_layout(cr);
	font = pango_font_description_from_string(self->font_desc);
	pango_layout_set_font_description(layout, font);

	padding = 10;
	text_y = (height - pango_font_description_get_size(font) /
	          PANGO_SCALE) / 2;
	if (text_y < 2)
		text_y = 2;

	/* Left: title — use custom_title from Elisp if set,
	 * otherwise fall back to focused client title */
	cairo_set_source_rgba(cr, self->fg_color[0], self->fg_color[1],
	                      self->fg_color[2], self->fg_color[3]);

	if (self->custom_title != NULL && self->custom_title[0] != '\0') {
		title = self->custom_title;
	} else {
		focused = (self->compositor != NULL) ?
			gowl_compositor_get_focused_client(
				GOWL_COMPOSITOR(self->compositor)) : NULL;
		title = (focused != NULL) ? gowl_client_get_title(focused) : "cmacs";
		if (title == NULL)
			title = "cmacs";
	}

	pango_layout_set_text(layout, title, -1);
	pango_layout_set_width(layout, (width / 2 - padding * 2) * PANGO_SCALE);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	cairo_move_to(cr, padding, text_y);
	pango_cairo_show_layout(cr, layout);

	/* Right: battery + clock */
	now = time(NULL);
	tm = localtime(&now);
	strftime(time_buf, sizeof(time_buf), "%a %b %d  %H:%M", tm);

	bat = read_battery_percent();
	if (bat >= 0)
		snprintf(right_buf, sizeof(right_buf), "\xf0\x9f\x94\x8b %d%%   %s",
		         bat, time_buf);
	else
		snprintf(right_buf, sizeof(right_buf), "%s", time_buf);

	pango_layout_set_text(layout, right_buf, -1);
	pango_layout_set_width(layout, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
	pango_layout_get_pixel_extents(layout, &ink, &logical);
	cairo_move_to(cr, width - logical.width - padding, text_y);
	pango_cairo_show_layout(cr, layout);

	pango_font_description_free(font);
	g_object_unref(layout);

	/* Copy to pixel buffer */
	cairo_surface_flush(cs);
	stride = cairo_image_surface_get_stride(cs);
	pixels = (guchar *)g_malloc((gsize)(stride * height));
	memcpy(pixels, cairo_image_surface_get_data(cs), (gsize)(stride * height));
	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	/* Create wlr_buffer */
	buf = g_new0(BarBuffer, 1);
	buf->pixels = pixels;
	buf->size   = (gsize)(stride * height);
	buf->stride = stride;
	wlr_buffer_init(&buf->base, &bar_buffer_impl, width, height);

	return buf;
}

/* ----------------------------------------------------------------
 * Scene buffer management
 * ---------------------------------------------------------------- */

static void
bar_create_surface(GowlModuleBar *self, GowlMonitor *monitor)
{
	GowlCompositor *comp;
	struct wlr_scene_tree *top_layer;
	BarSurface *surface;
	BarBuffer *buf;
	const gchar *name;
	gint mon_x, mon_y, mon_w, mon_h;

	comp = GOWL_COMPOSITOR(self->compositor);
	name = gowl_monitor_get_name(monitor);
	gowl_monitor_get_geometry(monitor, &mon_x, &mon_y, &mon_w, &mon_h);

	if (mon_w <= 0 || mon_h <= 0)
		return;

	/* Remove existing surface for this monitor */
	surface = (BarSurface *)g_hash_table_lookup(self->surfaces, name);
	if (surface != NULL) {
		if (surface->scene_buf != NULL)
			wlr_scene_node_destroy(&surface->scene_buf->node);
		g_hash_table_remove(self->surfaces, name);
		g_free(surface);
	}

	top_layer = gowl_compositor_get_scene_layer(comp,
	                                            GOWL_SCENE_LAYER_TOP);
	if (top_layer == NULL)
		return;

	/* Render bar content */
	buf = bar_render(self, mon_w, self->bar_height);

	/* Create scene buffer on TOP layer */
	surface = g_new0(BarSurface, 1);
	surface->scene_buf = wlr_scene_buffer_create(top_layer, &buf->base);
	surface->width  = mon_w;
	surface->height = self->bar_height;
	surface->mon_x  = mon_x;
	surface->mon_y  = mon_y;

	/* Position at top of monitor */
	wlr_scene_node_set_position(&surface->scene_buf->node, mon_x, mon_y);

	/* Drop producer ref */
	wlr_buffer_drop(&buf->base);

	/* Store */
	g_hash_table_insert(self->surfaces, g_strdup(name), surface);

	g_debug("bar: created surface for monitor %s (%dx%d)",
	        name, mon_w, self->bar_height);
}

static void
bar_redraw_all(GowlModuleBar *self)
{
	GowlCompositor *comp;
	GList *monitors, *l;

	if (self->compositor == NULL)
		return;

	comp = GOWL_COMPOSITOR(self->compositor);
	monitors = gowl_compositor_get_monitors(comp);

	for (l = monitors; l != NULL; l = l->next) {
		GowlMonitor *mon = GOWL_MONITOR(l->data);
		const gchar *name = gowl_monitor_get_name(mon);
		BarSurface *surface;
		BarBuffer *buf;

		surface = (BarSurface *)g_hash_table_lookup(self->surfaces, name);
		if (surface == NULL || surface->scene_buf == NULL) {
			bar_create_surface(self, mon);
			continue;
		}

		/* Re-render and update the existing scene buffer */
		buf = bar_render(self, surface->width, surface->height);
		wlr_scene_buffer_set_buffer(surface->scene_buf, &buf->base);
		wlr_buffer_drop(&buf->base);
	}
}

static void
bar_destroy_all(GowlModuleBar *self)
{
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init(&iter, self->surfaces);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		BarSurface *surface = (BarSurface *)value;
		if (surface->scene_buf != NULL)
			wlr_scene_node_destroy(&surface->scene_buf->node);
		g_free(surface);
	}
	g_hash_table_remove_all(self->surfaces);
}

/* ----------------------------------------------------------------
 * Signal callbacks
 * ---------------------------------------------------------------- */

static void
bar_on_focus_changed(GowlCompositor *comp, GObject *client,
                     gpointer user_data)
{
	(void)comp;
	(void)client;
	bar_redraw_all(GOWL_MODULE_BAR(user_data));
}

static void
bar_on_client_changed(GowlCompositor *comp, GObject *client,
                      gpointer user_data)
{
	(void)comp;
	(void)client;
	bar_redraw_all(GOWL_MODULE_BAR(user_data));
}

static int
bar_clock_tick(void *data)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(data);
	bar_redraw_all(self);

	/* Re-arm timer for 60 seconds */
	if (self->clock_timer != NULL)
		wl_event_source_timer_update(self->clock_timer, 60 * 1000);

	return 0;
}

/* ----------------------------------------------------------------
 * GowlModule virtual methods
 * ---------------------------------------------------------------- */

static gboolean
bar_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
bar_deactivate(GowlModule *mod)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(mod);

	/* Disconnect signals */
	if (self->focus_handler_id != 0 && self->compositor != NULL) {
		g_signal_handler_disconnect(self->compositor,
		                            self->focus_handler_id);
		self->focus_handler_id = 0;
	}
	if (self->client_added_id != 0 && self->compositor != NULL) {
		g_signal_handler_disconnect(self->compositor,
		                            self->client_added_id);
		self->client_added_id = 0;
	}
	if (self->client_removed_id != 0 && self->compositor != NULL) {
		g_signal_handler_disconnect(self->compositor,
		                            self->client_removed_id);
		self->client_removed_id = 0;
	}

	/* Remove clock timer */
	if (self->clock_timer != NULL) {
		wl_event_source_remove(self->clock_timer);
		self->clock_timer = NULL;
	}

	/* Destroy all scene nodes */
	bar_destroy_all(self);

	self->compositor = NULL;
}

static const gchar *
bar_get_name(GowlModule *mod)
{
	(void)mod;
	return "bar";
}

static const gchar *
bar_get_description(GowlModule *mod)
{
	(void)mod;
	return "Compositor status bar with title and system info";
}

static const gchar *
bar_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

static void
bar_configure(GowlModule *mod, gpointer config)
{
	GowlModuleBar *self;
	GHashTable *settings;
	const gchar *val;

	self = GOWL_MODULE_BAR(mod);

	if (config == NULL)
		return;

	settings = (GHashTable *)config;

	val = (const gchar *)g_hash_table_lookup(settings, "height");
	if (val != NULL)
		self->bar_height = (gint)g_ascii_strtoll(val, NULL, 10);

	val = (const gchar *)g_hash_table_lookup(settings, "bg-color");
	if (val != NULL)
		parse_hex_color(val, self->bg_color);

	val = (const gchar *)g_hash_table_lookup(settings, "fg-color");
	if (val != NULL)
		parse_hex_color(val, self->fg_color);

	val = (const gchar *)g_hash_table_lookup(settings, "font");
	if (val != NULL) {
		g_free(self->font_desc);
		self->font_desc = g_strdup(val);
	}

	val = (const gchar *)g_hash_table_lookup(settings, "font-size");
	if (val != NULL) {
		gchar *desc;
		gdouble sz = g_ascii_strtod(val, NULL);

		g_free(self->font_desc);
		desc = g_strdup_printf("monospace %.0f", sz);
		self->font_desc = desc;
	}

	val = (const gchar *)g_hash_table_lookup(settings, "title");
	if (val != NULL) {
		g_free(self->custom_title);
		self->custom_title = g_strdup(val);
	}

	g_message("bar: configured height=%d font=%s",
	          self->bar_height, self->font_desc);

	/* Re-render if compositor is running */
	if (self->compositor != NULL)
		bar_redraw_all(self);
}

/* ----------------------------------------------------------------
 * GowlBarProvider
 * ---------------------------------------------------------------- */

static gint
bar_get_bar_height(GowlBarProvider *provider, gpointer monitor)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(provider);
	(void)monitor;
	return self->bar_height;
}

static void
bar_render_bar(GowlBarProvider *provider, gpointer monitor)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(provider);
	(void)monitor;
	bar_redraw_all(self);
}

static void
bar_provider_iface_init(GowlBarProviderInterface *iface)
{
	iface->get_bar_height = bar_get_bar_height;
	iface->render_bar     = bar_render_bar;
}

/* ----------------------------------------------------------------
 * GowlStartupHandler
 * ---------------------------------------------------------------- */

static void
bar_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(handler);
	GowlCompositor *comp = GOWL_COMPOSITOR(compositor);
	GList *monitors, *l;
	struct wl_event_loop *loop;

	self->compositor = compositor;

	/* Connect to compositor signals */
	self->focus_handler_id =
		g_signal_connect(compositor, "focus-changed",
		                 G_CALLBACK(bar_on_focus_changed), self);
	self->client_added_id =
		g_signal_connect(compositor, "client-added",
		                 G_CALLBACK(bar_on_client_changed), self);
	self->client_removed_id =
		g_signal_connect(compositor, "client-removed",
		                 G_CALLBACK(bar_on_client_changed), self);

	/* Create bar surfaces for all existing monitors */
	monitors = gowl_compositor_get_monitors(comp);
	for (l = monitors; l != NULL; l = l->next)
		bar_create_surface(self, GOWL_MONITOR(l->data));

	/* Start clock timer (60s) */
	loop = wl_display_get_event_loop(
		gowl_compositor_get_wl_display(comp));
	if (loop != NULL) {
		self->clock_timer = wl_event_loop_add_timer(loop,
			bar_clock_tick, self);
		if (self->clock_timer != NULL)
			wl_event_source_timer_update(self->clock_timer, 60 * 1000);
	}

	/* Trigger layout recalculation to account for bar height */
	monitors = gowl_compositor_get_monitors(comp);
	for (l = monitors; l != NULL; l = l->next)
		gowl_compositor_arrangelayers(comp, GOWL_MONITOR(l->data));

	g_debug("bar: startup, height=%d", self->bar_height);
}

static void
bar_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = bar_on_startup;
}

/* ----------------------------------------------------------------
 * GowlShutdownHandler
 * ---------------------------------------------------------------- */

static void
bar_on_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(handler);

	(void)compositor;

	if (self->focus_handler_id != 0 && self->compositor != NULL) {
		g_signal_handler_disconnect(self->compositor,
		                            self->focus_handler_id);
		self->focus_handler_id = 0;
	}
	if (self->client_added_id != 0 && self->compositor != NULL) {
		g_signal_handler_disconnect(self->compositor,
		                            self->client_added_id);
		self->client_added_id = 0;
	}
	if (self->client_removed_id != 0 && self->compositor != NULL) {
		g_signal_handler_disconnect(self->compositor,
		                            self->client_removed_id);
		self->client_removed_id = 0;
	}

	if (self->clock_timer != NULL) {
		wl_event_source_remove(self->clock_timer);
		self->clock_timer = NULL;
	}

	bar_destroy_all(self);
	self->compositor = NULL;
}

static void
bar_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = bar_on_shutdown;
}

/* ----------------------------------------------------------------
 * GObject lifecycle
 * ---------------------------------------------------------------- */

static void
gowl_module_bar_finalize(GObject *object)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(object);

	g_free(self->font_desc);
	g_free(self->custom_title);
	g_hash_table_unref(self->surfaces);

	G_OBJECT_CLASS(gowl_module_bar_parent_class)->finalize(object);
}

static void
gowl_module_bar_class_init(GowlModuleBarClass *klass)
{
	GObjectClass *obj_class;
	GowlModuleClass *mod_class;

	obj_class = G_OBJECT_CLASS(klass);
	obj_class->finalize = gowl_module_bar_finalize;

	mod_class = GOWL_MODULE_CLASS(klass);
	mod_class->activate        = bar_activate;
	mod_class->deactivate      = bar_deactivate;
	mod_class->get_name        = bar_get_name;
	mod_class->get_description = bar_get_description;
	mod_class->get_version     = bar_get_version;
	mod_class->configure       = bar_configure;
}

static void
gowl_module_bar_init(GowlModuleBar *self)
{
	self->bar_height = 28;
	/* Semi-transparent Catppuccin Mocha base */
	self->bg_color[0] = 0.118;
	self->bg_color[1] = 0.118;
	self->bg_color[2] = 0.180;
	self->bg_color[3] = 0.8;
	/* Catppuccin text */
	self->fg_color[0] = 0.804;
	self->fg_color[1] = 0.839;
	self->fg_color[2] = 0.957;
	self->fg_color[3] = 1.0;

	self->font_desc = g_strdup("monospace 13");
	self->surfaces  = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                        g_free, NULL);
	self->compositor       = NULL;
	self->custom_title     = NULL;
	self->focus_handler_id = 0;
	self->client_added_id  = 0;
	self->client_removed_id = 0;
	self->clock_timer      = NULL;
}

/* ----------------------------------------------------------------
 * Shared-object entry point
 * ---------------------------------------------------------------- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_BAR;
}
