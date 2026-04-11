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
#include <sys/statvfs.h>

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
		sscanf(hex, "#%2x%2x%2x%2x", &r, &g, &b, &a);
	} else if (strlen(hex) == 7) {
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
 * Widget types
 * ---------------------------------------------------------------- */

typedef enum {
	BAR_WIDGET_CPU,
	BAR_WIDGET_MEMORY,
	BAR_WIDGET_DISK,
	BAR_WIDGET_BATTERY,
	BAR_WIDGET_CLOCK,
	BAR_WIDGET_COUNT
} BarWidgetType;

#define BAR_MAX_WIDGETS 16

typedef struct {
	BarWidgetType type;
	gdouble       color[4];  /* (0,0,0,0) = use fg_color */
	gboolean      has_color;
	gchar        *param;     /* type-specific: disk mount path */
} BarWidget;

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
	gchar   *font_desc;

	/* Title colorization */
	gchar   *title_delimiters;            /* chars that split segments */
	gdouble  title_delimiter_color[4];    /* color for delimiter chars */
	gdouble  title_palette[8][4];         /* segment color cycle */
	gint     title_palette_size;          /* 0 = disabled, use fg_color */

	/* Widgets */
	BarWidget widgets[BAR_MAX_WIDGETS];
	gint      n_widgets;

	/* State */
	GHashTable *surfaces;
	gpointer    compositor;
	gchar      *custom_title;
	gulong      focus_handler_id;
	gulong      client_added_id;
	gulong      client_removed_id;
	struct wl_event_source *tick_timer;

	/* Cached system data */
	glong   prev_cpu_idle;
	glong   prev_cpu_total;
	gint    cached_cpu_pct;
	time_t  cpu_read_time;
	glong   cached_mem_used_mb;
	glong   cached_mem_total_mb;
	time_t  mem_read_time;
	glong   cached_disk_free_gb;
	glong   cached_disk_total_gb;
	time_t  disk_read_time;
	gint    cached_battery;
	time_t  bat_read_time;
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
 * System data readers
 * ---------------------------------------------------------------- */

static void
read_cpu(GowlModuleBar *self)
{
	FILE *f;
	glong user, nice, sys, idle, iowait, irq, softirq, steal;
	glong total, diff_idle, diff_total;
	time_t now;

	now = time(NULL);
	if (now - self->cpu_read_time < 2)
		return;
	self->cpu_read_time = now;

	f = fopen("/proc/stat", "r");
	if (f == NULL)
		return;

	if (fscanf(f, "cpu %ld %ld %ld %ld %ld %ld %ld %ld",
	           &user, &nice, &sys, &idle, &iowait,
	           &irq, &softirq, &steal) != 8) {
		fclose(f);
		return;
	}
	fclose(f);

	total = user + nice + sys + idle + iowait + irq + softirq + steal;
	diff_idle  = (idle + iowait) - self->prev_cpu_idle;
	diff_total = total - self->prev_cpu_total;

	if (diff_total > 0)
		self->cached_cpu_pct = (gint)(100 * (diff_total - diff_idle) / diff_total);
	else
		self->cached_cpu_pct = 0;

	self->prev_cpu_idle  = idle + iowait;
	self->prev_cpu_total = total;
}

static void
read_memory(GowlModuleBar *self)
{
	FILE *f;
	char line[256];
	glong total_kb, avail_kb, used_kb;
	time_t now;

	now = time(NULL);
	if (now - self->mem_read_time < 5)
		return;
	self->mem_read_time = now;

	total_kb = 0;
	avail_kb = 0;

	f = fopen("/proc/meminfo", "r");
	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, "MemTotal:", 9) == 0)
			sscanf(line + 9, " %ld", &total_kb);
		else if (strncmp(line, "MemAvailable:", 13) == 0)
			sscanf(line + 13, " %ld", &avail_kb);
	}
	fclose(f);

	used_kb = total_kb - avail_kb;
	self->cached_mem_used_mb  = used_kb / 1024;
	self->cached_mem_total_mb = total_kb / 1024;
}

static void
read_disk(GowlModuleBar *self)
{
	struct statvfs st;
	time_t now;

	now = time(NULL);
	if (now - self->disk_read_time < 30)
		return;
	self->disk_read_time = now;

	if (statvfs("/", &st) != 0)
		return;

	self->cached_disk_free_gb  = (glong)((st.f_bavail * st.f_frsize) /
	                             (1024UL * 1024UL * 1024UL));
	self->cached_disk_total_gb = (glong)((st.f_blocks * st.f_frsize) /
	                             (1024UL * 1024UL * 1024UL));
}

static void
read_battery(GowlModuleBar *self)
{
	FILE *f;
	gint pct;
	time_t now;

	now = time(NULL);
	if (now - self->bat_read_time < 60)
		return;
	self->bat_read_time = now;

	f = fopen("/sys/class/power_supply/BAT0/capacity", "r");
	if (f == NULL)
		f = fopen("/sys/class/power_supply/BAT1/capacity", "r");
	if (f == NULL) {
		self->cached_battery = -1;
		return;
	}

	if (fscanf(f, "%d", &pct) != 1)
		pct = -1;
	fclose(f);
	self->cached_battery = pct;
}

static void
read_all_data(GowlModuleBar *self)
{
	gint i;

	for (i = 0; i < self->n_widgets; i++) {
		switch (self->widgets[i].type) {
		case BAR_WIDGET_CPU:     read_cpu(self);     break;
		case BAR_WIDGET_MEMORY:  read_memory(self);  break;
		case BAR_WIDGET_DISK:    read_disk(self);    break;
		case BAR_WIDGET_BATTERY: read_battery(self); break;
		default: break;
		}
	}
}

/* ----------------------------------------------------------------
 * Widget text formatting
 * ---------------------------------------------------------------- */

static void
widget_text(GowlModuleBar *self, BarWidget *w, char *buf, gsize bufsz)
{
	time_t now;
	struct tm *tm;

	switch (w->type) {
	case BAR_WIDGET_CPU:
		snprintf(buf, bufsz, "CPU %d%%", self->cached_cpu_pct);
		break;
	case BAR_WIDGET_MEMORY:
		if (self->cached_mem_used_mb >= 1024)
			snprintf(buf, bufsz, "MEM %.1fG",
			         (gdouble)self->cached_mem_used_mb / 1024.0);
		else
			snprintf(buf, bufsz, "MEM %ldM", self->cached_mem_used_mb);
		break;
	case BAR_WIDGET_DISK:
		{
			const gchar *path;
			struct statvfs st;
			glong free_gb;

			path = (w->param != NULL) ? w->param : "/";
			if (statvfs(path, &st) == 0) {
				free_gb = (glong)((st.f_bavail * st.f_frsize) /
				          (1024UL * 1024UL * 1024UL));
				snprintf(buf, bufsz, "%s %ldG", path, free_gb);
			} else {
				snprintf(buf, bufsz, "%s ?", path);
			}
		}
		break;
	case BAR_WIDGET_BATTERY:
		if (self->cached_battery >= 0)
			snprintf(buf, bufsz, "BAT %d%%", self->cached_battery);
		else
			buf[0] = '\0';
		break;
	case BAR_WIDGET_CLOCK:
		now = time(NULL);
		tm = localtime(&now);
		strftime(buf, bufsz, "%a %b %d  %H:%M", tm);
		break;
	default:
		buf[0] = '\0';
		break;
	}
}

/* ----------------------------------------------------------------
 * Widget config parsing
 * ---------------------------------------------------------------- */

static BarWidgetType
widget_type_from_name(const gchar *name)
{
	if (strcmp(name, "cpu") == 0)     return BAR_WIDGET_CPU;
	if (strcmp(name, "memory") == 0)  return BAR_WIDGET_MEMORY;
	if (strcmp(name, "mem") == 0)     return BAR_WIDGET_MEMORY;
	if (strcmp(name, "disk") == 0)    return BAR_WIDGET_DISK;
	if (strcmp(name, "battery") == 0) return BAR_WIDGET_BATTERY;
	if (strcmp(name, "bat") == 0)     return BAR_WIDGET_BATTERY;
	if (strcmp(name, "clock") == 0)   return BAR_WIDGET_CLOCK;
	if (strcmp(name, "time") == 0)    return BAR_WIDGET_CLOCK;
	return BAR_WIDGET_COUNT; /* invalid */
}

static const gchar *
widget_color_key(BarWidgetType type)
{
	switch (type) {
	case BAR_WIDGET_CPU:     return "cpu-color";
	case BAR_WIDGET_MEMORY:  return "memory-color";
	case BAR_WIDGET_DISK:    return "disk-color";
	case BAR_WIDGET_BATTERY: return "battery-color";
	case BAR_WIDGET_CLOCK:   return "clock-color";
	default: return NULL;
	}
}

static void
parse_widget_list(GowlModuleBar *self, const gchar *spec)
{
	gchar **parts;
	gint i;

	parts = g_strsplit(spec, " ", -1);
	self->n_widgets = 0;

	/* Free old widget params */
	for (i = 0; i < self->n_widgets; i++)
		g_free(self->widgets[i].param);

	self->n_widgets = 0;

	for (i = 0; parts[i] != NULL && self->n_widgets < BAR_MAX_WIDGETS; i++) {
		BarWidgetType t;
		gchar *name;
		gchar *param;
		gchar *colon;

		if (parts[i][0] == '\0')
			continue;

		/* Support "disk:/home" syntax for parameterized widgets */
		name  = parts[i];
		param = NULL;
		colon = strchr(name, ':');
		if (colon != NULL) {
			*colon = '\0';
			param = colon + 1;
		}

		t = widget_type_from_name(name);
		if (t == BAR_WIDGET_COUNT)
			continue;

		self->widgets[self->n_widgets].type = t;
		self->widgets[self->n_widgets].has_color = FALSE;
		self->widgets[self->n_widgets].param =
			(param != NULL && param[0] != '\0') ? g_strdup(param) : NULL;
		self->n_widgets++;
	}
	g_strfreev(parts);
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
	PangoRectangle ink, logical;
	GowlClient *focused;
	const gchar *title;
	gint right_x;
	gint i;
	char wtext[128];
	gint separator_w;

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

	/* Measure separator width */
	pango_layout_set_text(layout, "  ", -1);
	pango_layout_get_pixel_extents(layout, &ink, &logical);
	separator_w = logical.width;

	/* Left: title */
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

	/* Colorize title: split on delimiters, cycle palette colors */
	if (self->title_palette_size > 0 && self->title_delimiters != NULL) {
		PangoAttrList *attrs;
		gint seg_idx;
		gint pos;
		gint len;

		attrs = pango_attr_list_new();
		seg_idx = 0;
		len = (gint)strlen(title);

		for (pos = 0; pos < len; pos++) {
			PangoAttribute *attr;
			const gdouble *c;

			if (strchr(self->title_delimiters, title[pos]) != NULL) {
				/* Delimiter character */
				c = self->title_delimiter_color;
			} else {
				/* Regular character — find end of segment */
				gint start = pos;
				while (pos < len &&
				       strchr(self->title_delimiters, title[pos]) == NULL)
					pos++;
				c = self->title_palette[seg_idx % self->title_palette_size];
				attr = pango_attr_foreground_new(
					(guint16)(c[0] * 65535),
					(guint16)(c[1] * 65535),
					(guint16)(c[2] * 65535));
				attr->start_index = (guint)start;
				attr->end_index   = (guint)pos;
				pango_attr_list_insert(attrs, attr);
				seg_idx++;
				pos--; /* re-process delimiter in next iteration */
				continue;
			}
			/* Single delimiter char */
			attr = pango_attr_foreground_new(
				(guint16)(c[0] * 65535),
				(guint16)(c[1] * 65535),
				(guint16)(c[2] * 65535));
			attr->start_index = (guint)pos;
			attr->end_index   = (guint)(pos + 1);
			pango_attr_list_insert(attrs, attr);
		}
		pango_layout_set_attributes(layout, attrs);
		pango_attr_list_unref(attrs);
	} else {
		cairo_set_source_rgba(cr, self->fg_color[0], self->fg_color[1],
		                      self->fg_color[2], self->fg_color[3]);
	}

	cairo_move_to(cr, padding, text_y);
	/* When using pango attributes, set cairo source to white so
	 * the attribute colors are used directly. */
	if (self->title_palette_size > 0)
		cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
	pango_cairo_show_layout(cr, layout);
	pango_layout_set_attributes(layout, NULL);

	/* Right: widgets, rendered right-to-left */
	right_x = width - padding;
	pango_layout_set_width(layout, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);

	/* Read fresh system data */
	read_all_data(self);

	for (i = self->n_widgets - 1; i >= 0; i--) {
		const gdouble *c;

		widget_text(self, &self->widgets[i], wtext, sizeof(wtext));
		if (wtext[0] == '\0')
			continue;

		/* Use per-widget color if set, otherwise fg_color */
		c = self->widgets[i].has_color ? self->widgets[i].color
		                               : self->fg_color;
		cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);

		pango_layout_set_text(layout, wtext, -1);
		pango_layout_get_pixel_extents(layout, &ink, &logical);
		right_x -= logical.width;
		cairo_move_to(cr, right_x, text_y);
		pango_cairo_show_layout(cr, layout);

		/* Add separator between widgets */
		right_x -= separator_w;
	}

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

	buf = bar_render(self, mon_w, self->bar_height);

	surface = g_new0(BarSurface, 1);
	surface->scene_buf = wlr_scene_buffer_create(top_layer, &buf->base);
	surface->width  = mon_w;
	surface->height = self->bar_height;
	surface->mon_x  = mon_x;
	surface->mon_y  = mon_y;

	wlr_scene_node_set_position(&surface->scene_buf->node, mon_x, mon_y);
	wlr_buffer_drop(&buf->base);

	g_hash_table_insert(self->surfaces, g_strdup(name), surface);
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
bar_tick(void *data)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(data);
	bar_redraw_all(self);

	if (self->tick_timer != NULL)
		wl_event_source_timer_update(self->tick_timer, 2 * 1000);

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

	if (self->tick_timer != NULL) {
		wl_event_source_remove(self->tick_timer);
		self->tick_timer = NULL;
	}

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
	return "Compositor status bar with configurable system widgets";
}

static const gchar *
bar_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.2.0";
}

static void
bar_configure(GowlModule *mod, gpointer config)
{
	GowlModuleBar *self;
	GHashTable *settings;
	const gchar *val;
	gint i;

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
		gdouble sz = g_ascii_strtod(val, NULL);
		g_free(self->font_desc);
		self->font_desc = g_strdup_printf("monospace %.0f", sz);
	}

	val = (const gchar *)g_hash_table_lookup(settings, "title");
	if (val != NULL) {
		g_free(self->custom_title);
		self->custom_title = g_strdup(val);
	}

	/* Title colorization */
	val = (const gchar *)g_hash_table_lookup(settings, "title-delimiters");
	if (val != NULL) {
		g_free(self->title_delimiters);
		self->title_delimiters = g_strdup(val);
	}

	val = (const gchar *)g_hash_table_lookup(settings, "title-delimiter-color");
	if (val != NULL)
		parse_hex_color(val, self->title_delimiter_color);

	val = (const gchar *)g_hash_table_lookup(settings, "title-palette");
	if (val != NULL) {
		gchar **colors = g_strsplit(val, " ", -1);
		gint ci;

		self->title_palette_size = 0;
		for (ci = 0; colors[ci] != NULL && ci < 8; ci++) {
			if (colors[ci][0] == '\0')
				continue;
			parse_hex_color(colors[ci],
			                self->title_palette[self->title_palette_size]);
			self->title_palette_size++;
		}
		g_strfreev(colors);
	}

	val = (const gchar *)g_hash_table_lookup(settings, "widgets");
	if (val != NULL)
		parse_widget_list(self, val);

	/* Per-widget colors */
	for (i = 0; i < self->n_widgets; i++) {
		const gchar *key = widget_color_key(self->widgets[i].type);
		if (key != NULL) {
			val = (const gchar *)g_hash_table_lookup(settings, key);
			if (val != NULL) {
				parse_hex_color(val, self->widgets[i].color);
				self->widgets[i].has_color = TRUE;
			}
		}
	}

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

	if (monitor != NULL) {
		GowlMonitor *mon = GOWL_MONITOR(monitor);
		const gchar *name = gowl_monitor_get_name(mon);
		gint mon_x, mon_y, mon_w, mon_h;
		BarSurface *surface;

		gowl_monitor_get_geometry(mon, &mon_x, &mon_y, &mon_w, &mon_h);
		surface = (BarSurface *)g_hash_table_lookup(self->surfaces, name);

		if (surface == NULL || surface->width != mon_w
		    || surface->mon_x != mon_x || surface->mon_y != mon_y) {
			bar_create_surface(self, mon);
			return;
		}
	}

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

	self->focus_handler_id =
		g_signal_connect(compositor, "focus-changed",
		                 G_CALLBACK(bar_on_focus_changed), self);
	self->client_added_id =
		g_signal_connect(compositor, "client-added",
		                 G_CALLBACK(bar_on_client_changed), self);
	self->client_removed_id =
		g_signal_connect(compositor, "client-removed",
		                 G_CALLBACK(bar_on_client_changed), self);

	monitors = gowl_compositor_get_monitors(comp);
	for (l = monitors; l != NULL; l = l->next)
		bar_create_surface(self, GOWL_MONITOR(l->data));

	/* 2-second tick timer for system data updates */
	loop = wl_display_get_event_loop(
		gowl_compositor_get_wl_display(comp));
	if (loop != NULL) {
		self->tick_timer = wl_event_loop_add_timer(loop,
			bar_tick, self);
		if (self->tick_timer != NULL)
			wl_event_source_timer_update(self->tick_timer, 2 * 1000);
	}

	monitors = gowl_compositor_get_monitors(comp);
	for (l = monitors; l != NULL; l = l->next)
		gowl_compositor_arrangelayers(comp, GOWL_MONITOR(l->data));
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

	if (self->tick_timer != NULL) {
		wl_event_source_remove(self->tick_timer);
		self->tick_timer = NULL;
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

	{
		gint i;
		for (i = 0; i < self->n_widgets; i++)
			g_free(self->widgets[i].param);
	}
	g_free(self->font_desc);
	g_free(self->custom_title);
	g_free(self->title_delimiters);
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
	self->title_delimiters   = NULL;  /* disabled by default */
	self->title_palette_size = 0;
	/* dim delimiter color default */
	self->title_delimiter_color[0] = 0.45;
	self->title_delimiter_color[1] = 0.46;
	self->title_delimiter_color[2] = 0.50;
	self->title_delimiter_color[3] = 1.0;

	self->surfaces  = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                        g_free, NULL);
	self->compositor       = NULL;
	self->custom_title     = NULL;
	self->focus_handler_id = 0;
	self->client_added_id  = 0;
	self->client_removed_id = 0;
	self->tick_timer       = NULL;

	/* Default widgets: cpu memory disk battery clock */
	self->n_widgets = 5;
	self->widgets[0].type = BAR_WIDGET_CPU;
	self->widgets[0].has_color = FALSE;
	self->widgets[0].param = NULL;
	self->widgets[1].type = BAR_WIDGET_MEMORY;
	self->widgets[1].has_color = FALSE;
	self->widgets[1].param = NULL;
	self->widgets[2].type = BAR_WIDGET_DISK;
	self->widgets[2].has_color = FALSE;
	self->widgets[2].param = NULL;
	self->widgets[3].type = BAR_WIDGET_BATTERY;
	self->widgets[3].has_color = FALSE;
	self->widgets[3].param = NULL;
	self->widgets[4].type = BAR_WIDGET_CLOCK;
	self->widgets[4].has_color = FALSE;
	self->widgets[4].param = NULL;

	/* Zero cached data */
	self->prev_cpu_idle  = 0;
	self->prev_cpu_total = 0;
	self->cached_cpu_pct = 0;
	self->cpu_read_time  = 0;
	self->cached_mem_used_mb  = 0;
	self->cached_mem_total_mb = 0;
	self->mem_read_time  = 0;
	self->cached_disk_free_gb  = 0;
	self->cached_disk_total_gb = 0;
	self->disk_read_time = 0;
	self->cached_battery = -1;
	self->bat_read_time  = 0;
}

/* ----------------------------------------------------------------
 * Shared-object entry point
 * ---------------------------------------------------------------- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_BAR;
}
