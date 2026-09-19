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

#include <string.h>
#include <time.h>

#include <cairo.h>
#include <pango/pangocairo.h>

#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"
#include "config/gowl-config.h"
#include "core/gowl-layout-registry.h"

#include <linux/input-event-codes.h>

#include "bar-internal.h"

/**
 * SECTION:bar-plugins-core
 * @title: Core bar plugins
 * @short_description: the window-manager widgets and the generic ones
 *
 * Two groups.  The compositor-aware widgets --- the tag row, the
 * window title, the layout indicator --- are here because they are the
 * only shipped plugins that read compositor state directly.  The
 * generic ones --- a button, a toggle, a command, a spacer --- are here
 * because they are how a user adds something the shipped set does not
 * cover without writing any C at all.
 */

/* ----------------------------------------------------------------
 * Shared helpers
 * ---------------------------------------------------------------- */

static GowlCompositor *
core_compositor(void)
{
	const BarEnv *env = bar_env();

	if (env == NULL || env->compositor == NULL)
		return NULL;
	return GOWL_COMPOSITOR(env->compositor);
}

/* The `color' setting: a palette role, or a literal the user meant
   exactly.  One helper for every shipped widget, so they all accept
   the same spellings -- see bar_plugin_apply_color(). */
static void
core_apply_color_setting(GowlBarPlugin *plugin, GowlBarColor fallback)
{
	bar_plugin_apply_color(plugin,
	                       gowl_bar_plugin_get_setting(plugin, "color"),
	                       fallback);
}

/* Look up a colour setting as a literal RGBA, falling back to a theme
   role.  Used by the widgets that paint their own boxes. */
static void
core_color_or_role(GowlBarPlugin *plugin, const GowlBarTheme *theme,
                   const gchar *key, GowlBarColor fallback, gdouble out[4])
{
	const gchar *spec;
	GowlBarColor role;
	const gdouble *c;

	spec = gowl_bar_plugin_get_setting(plugin, key);
	if (spec != NULL && gowl_bar_color_parse(spec, out))
		return;
	if (spec != NULL && gowl_bar_theme_color_from_name(spec, &role)) {
		c = gowl_bar_theme_color(theme, role);
		memcpy(out, c, sizeof(gdouble) * 4);
		return;
	}
	c = gowl_bar_theme_color(theme, fallback);
	memcpy(out, c, sizeof(gdouble) * 4);
}

/* ----------------------------------------------------------------
 * tags -- the dwm-style tag row
 * ---------------------------------------------------------------- */

/*
 * What the tag row shows --- for ONE output.
 *
 * Not stored on the plugin.  One plugin object serves every screen, so
 * a reading taken once and kept would be whichever screen was asked
 * last; on a two-monitor desk both bars then showed the FOCUSED
 * screen's tags, which is not what a dwm tag row means.  It is read
 * fresh from gowl_bar_plugin_get_monitor() in the three places that
 * need it --- the signature, the measure and the draw --- each of which
 * the host serves the output it is asking about.
 */
typedef struct {
	guint32 selected;
	guint32 occupied;
	guint32 urgent;
	gint    tag_count;
} TagsView;

static void
tags_view(GowlBarPlugin *plugin, TagsView *out)
{
	GowlCompositor *comp;
	GowlMonitor *mon;
	GowlConfig *config;
	GList *clients, *l;

	memset(out, 0, sizeof(*out));
	out->tag_count = 9;

	comp = core_compositor();
	if (comp == NULL)
		return;

	/* Session-wide: every screen shows the same number of tags. */
	config = gowl_compositor_get_config(comp);
	if (config != NULL)
		out->tag_count = gowl_config_get_tag_count(config);

	mon = bar_plugin_monitor(plugin);
	if (mon == NULL)
		return;

	out->selected = gowl_monitor_get_tags(mon);

	clients = gowl_compositor_get_clients(comp);
	for (l = clients; l != NULL; l = l->next) {
		GowlClient *c = GOWL_CLIENT(l->data);
		guint32 ct;

		if (gowl_client_get_monitor(c) != mon)
			continue;
		ct = gowl_client_get_tags(c);
		out->occupied |= ct;
		if (gowl_client_get_urgent(c))
			out->urgent |= ct;
	}
}

static void
tags_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	(void)data;
	(void)settings;
	gowl_bar_plugin_set_visible(plugin,
		gowl_bar_plugin_get_setting_bool(plugin, "visible", TRUE));
}

/*
 * The only thing left that is the same on every screen.
 *
 * The module's tag hit-test divides the slot by this setting, and it
 * has to be published from somewhere; a poll is the honest place for a
 * value that does not vary by output.
 */
static void
tags_poll(GowlBarPlugin *plugin, gpointer data)
{
	GowlCompositor *comp;
	GowlConfig *config;
	g_autofree gchar *count_str = NULL;
	gint count;

	(void)data;

	comp = core_compositor();
	if (comp == NULL)
		return;

	config = gowl_compositor_get_config(comp);
	count = (config != NULL) ? gowl_config_get_tag_count(config) : 9;

	count_str = g_strdup_printf("%d", count);
	gowl_bar_plugin_set_setting(plugin, "tag-count", count_str);
}

/*
 * The tags of the output being asked about, as a string.
 *
 * This is what makes the row per-screen: the bar compares each screen's
 * surface against its own last signature, so naming this screen's tags
 * here repaints this screen when they move and leaves the other alone.
 * It replaces the state-in-the-label trick this widget used to rely on,
 * which could only ever carry one screen's answer.
 */
static void
tags_signature(GowlBarPlugin *plugin, gpointer data, GString *out)
{
	TagsView view;

	(void)data;

	tags_view(plugin, &view);
	g_string_append_printf(out, "%d:%u/%u/%u", view.tag_count,
	                       view.selected, view.occupied, view.urgent);
}

static gint
tags_measure(GowlBarPlugin *plugin, gpointer data, PangoLayout *layout,
             const GowlBarTheme *theme, gint height)
{
	TagsView view;

	(void)data;
	(void)layout;
	(void)theme;

	tags_view(plugin, &view);
	if (view.tag_count <= 0)
		return 0;
	/* Square boxes matching the bar height, exactly as dwm draws
	   them; the module's tag hit-test divides the slot the same way. */
	return view.tag_count * height;
}

static void
tags_draw(GowlBarPlugin *plugin, gpointer data, cairo_t *cr,
          PangoLayout *layout, const GowlBarTheme *theme, gint x, gint y,
          gint width, gint height, gboolean hovered, gboolean panel_open)
{
	TagsView view;
	PangoFontDescription *font;
	gdouble active_bg[4], active_fg[4], occupied_fg[4];
	gdouble urgent_bg[4], urgent_fg[4], empty_fg[4];
	gint box_w, i;

	(void)data;
	(void)hovered;
	(void)panel_open;
	(void)width;

	tags_view(plugin, &view);
	if (view.tag_count <= 0)
		return;

	core_color_or_role(plugin, theme, "active-bg", GOWL_BAR_COLOR_ACCENT,
	                   active_bg);
	core_color_or_role(plugin, theme, "active-fg", GOWL_BAR_COLOR_BASE,
	                   active_fg);
	core_color_or_role(plugin, theme, "occupied-fg", GOWL_BAR_COLOR_TEXT,
	                   occupied_fg);
	core_color_or_role(plugin, theme, "urgent-bg", GOWL_BAR_COLOR_RED,
	                   urgent_bg);
	core_color_or_role(plugin, theme, "urgent-fg", GOWL_BAR_COLOR_BASE,
	                   urgent_fg);
	core_color_or_role(plugin, theme, "empty-fg", GOWL_BAR_COLOR_MUTED,
	                   empty_fg);

	font = pango_font_description_from_string(
		gowl_bar_theme_get_font(theme));
	pango_layout_set_font_description(layout, font);
	pango_layout_set_width(layout, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
	pango_layout_set_attributes(layout, NULL);

	box_w = height;

	for (i = 0; i < view.tag_count; i++) {
		guint32 bit = (guint32)1u << i;
		gboolean sel = (view.selected & bit) != 0;
		gboolean urg = (view.urgent & bit) != 0;
		gboolean occ = (view.occupied & bit) != 0;
		const gdouble *fg;
		gchar label[8];
		PangoRectangle logical;
		gint bx;

		bx = x + i * box_w;

		if (urg) {
			cairo_set_source_rgba(cr, urgent_bg[0], urgent_bg[1],
			                      urgent_bg[2], urgent_bg[3]);
			cairo_rectangle(cr, bx, y, box_w, height);
			cairo_fill(cr);
			fg = urgent_fg;
		} else if (sel) {
			cairo_set_source_rgba(cr, active_bg[0], active_bg[1],
			                      active_bg[2], active_bg[3]);
			cairo_rectangle(cr, bx, y, box_w, height);
			cairo_fill(cr);
			fg = active_fg;
		} else if (occ) {
			fg = occupied_fg;
		} else {
			fg = empty_fg;
		}

		g_snprintf(label, sizeof(label), "%d", i + 1);
		pango_layout_set_text(layout, label, -1);
		pango_layout_get_pixel_extents(layout, NULL, &logical);
		cairo_set_source_rgba(cr, fg[0], fg[1], fg[2], fg[3]);
		cairo_move_to(cr, bx + (box_w - logical.width) / 2,
		              y + (height - logical.height) / 2);
		pango_cairo_show_layout(cr, layout);

		/* dwm's corner marker for a tag that has windows but is not
		   the one being viewed. */
		if (occ && !sel && !urg) {
			cairo_set_source_rgba(cr, fg[0], fg[1], fg[2], fg[3]);
			cairo_rectangle(cr, bx + 3.0, y + 3.0, 3.0, 3.0);
			cairo_fill(cr);
		}
	}

	pango_font_description_free(font);
}

static const GowlBarPluginVTable tags_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL,
	NULL, NULL, tags_configure,
	NULL, tags_poll, NULL,
	tags_measure, tags_draw,
	NULL, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL,
	tags_signature
};

/* ----------------------------------------------------------------
 * title -- the focused window's title
 * ---------------------------------------------------------------- */

typedef struct {
	gchar   *delimiters;
	gdouble  palette[8][4];
	gint     palette_size;
	gdouble  delimiter_color[4];
	gboolean have_palette;
} TitleData;

static gpointer
title_create(GowlBarPlugin *plugin)
{
	TitleData *td;

	(void)plugin;
	td = g_new0(TitleData, 1);
	td->delimiter_color[0] = 0.45;
	td->delimiter_color[1] = 0.46;
	td->delimiter_color[2] = 0.50;
	td->delimiter_color[3] = 1.0;
	return td;
}

static void
title_destroy(GowlBarPlugin *plugin, gpointer data)
{
	TitleData *td = data;

	(void)plugin;
	if (td == NULL)
		return;
	g_free(td->delimiters);
	g_free(td);
}

static void
title_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	TitleData *td = data;
	const gchar *value;

	(void)settings;

	g_free(td->delimiters);
	td->delimiters = g_strdup(gowl_bar_plugin_get_setting(plugin,
	                                                      "delimiters"));

	value = gowl_bar_plugin_get_setting(plugin, "delimiter-color");
	if (value != NULL)
		gowl_bar_color_parse(value, td->delimiter_color);

	core_apply_color_setting(plugin, GOWL_BAR_COLOR_TEXT);

	td->palette_size = 0;
	td->have_palette = FALSE;
	value = gowl_bar_plugin_get_setting(plugin, "palette");
	if (value != NULL && value[0] != '\0') {
		g_auto(GStrv) parts = NULL;
		gint i;

		parts = g_strsplit_set(value, " \t,", -1);
		for (i = 0; parts[i] != NULL && td->palette_size < 8; i++) {
			if (parts[i][0] == '\0')
				continue;
			if (gowl_bar_color_parse(parts[i],
			                         td->palette[td->palette_size]))
				td->palette_size++;
		}
		td->have_palette = (td->palette_size > 0 &&
		                    td->delimiters != NULL);
	}
}

static void
title_poll(GowlBarPlugin *plugin, gpointer data)
{
	GowlCompositor *comp;
	GowlClient *focused;
	const gchar *override;
	const gchar *title;

	(void)data;

	override = gowl_bar_plugin_get_setting(plugin, "text");
	if (override != NULL && override[0] != '\0') {
		gowl_bar_plugin_set_label(plugin, override);
		return;
	}

	comp = core_compositor();
	if (comp == NULL) {
		gowl_bar_plugin_set_label(plugin, NULL);
		return;
	}

	focused = gowl_compositor_get_focused_client(comp);
	title = (focused != NULL) ? gowl_client_get_title(focused) : NULL;
	if (title == NULL || title[0] == '\0')
		title = gowl_bar_plugin_get_setting(plugin, "fallback");
	gowl_bar_plugin_set_label(plugin, title);
}

static gint
title_measure(GowlBarPlugin *plugin, gpointer data, PangoLayout *layout,
              const GowlBarTheme *theme, gint height)
{
	gint natural, cap;

	(void)data;

	natural = gowl_bar_plugin_measure_label(plugin, layout, theme, height);
	/* A title is the one widget that can be arbitrarily long, and a
	   maximised terminal's title would otherwise push everything
	   else off the bar. */
	cap = gowl_bar_plugin_get_setting_int(plugin, "max-width", 480);
	if (cap > 0 && natural > cap)
		return cap;
	return natural;
}

static void
title_draw(GowlBarPlugin *plugin, gpointer data, cairo_t *cr,
           PangoLayout *layout, const GowlBarTheme *theme, gint x, gint y,
           gint width, gint height, gboolean hovered, gboolean panel_open)
{
	TitleData *td = data;
	PangoFontDescription *font;
	PangoAttrList *attrs;
	g_autofree gchar *label = NULL;
	PangoRectangle logical;
	gint pad, len, pos, seg;

	(void)hovered;
	(void)panel_open;

	label = gowl_bar_plugin_dup_label(plugin);
	if (label == NULL || label[0] == '\0')
		return;

	pad = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_ITEM_PAD);
	font = pango_font_description_from_string(
		gowl_bar_theme_get_font(theme));
	pango_layout_set_font_description(layout, font);
	pango_layout_set_width(layout, (width - 2 * pad) * PANGO_SCALE);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_text(layout, label, -1);

	if (!td->have_palette) {
		pango_layout_set_attributes(layout, NULL);
		gowl_bar_plugin_cairo_set_color(plugin, theme, cr);
		pango_layout_get_pixel_extents(layout, NULL, &logical);
		cairo_move_to(cr, x + pad, y + (height - logical.height) / 2);
		pango_cairo_show_layout(cr, layout);
		pango_font_description_free(font);
		return;
	}

	/* Colourised: split on the delimiter set and cycle the palette
	   over the segments, so `~/src/gowl/README.org' reads as parts
	   rather than as one long string. */
	attrs = pango_attr_list_new();
	len = (gint)strlen(label);
	seg = 0;
	for (pos = 0; pos < len; pos++) {
		PangoAttribute *attr;
		const gdouble *c;
		gint start;

		if (strchr(td->delimiters, label[pos]) != NULL) {
			c = td->delimiter_color;
			attr = pango_attr_foreground_new(
				(guint16)(c[0] * 65535.0),
				(guint16)(c[1] * 65535.0),
				(guint16)(c[2] * 65535.0));
			attr->start_index = (guint)pos;
			attr->end_index   = (guint)(pos + 1);
			pango_attr_list_insert(attrs, attr);
			continue;
		}

		start = pos;
		while (pos < len &&
		       strchr(td->delimiters, label[pos]) == NULL)
			pos++;
		c = td->palette[seg % td->palette_size];
		attr = pango_attr_foreground_new(
			(guint16)(c[0] * 65535.0),
			(guint16)(c[1] * 65535.0),
			(guint16)(c[2] * 65535.0));
		attr->start_index = (guint)start;
		attr->end_index   = (guint)pos;
		pango_attr_list_insert(attrs, attr);
		seg++;
		pos--;   /* the delimiter is handled on the next pass */
	}

	pango_layout_set_attributes(layout, attrs);
	pango_layout_get_pixel_extents(layout, NULL, &logical);
	/* Pango attribute colours replace the source, so the source has
	   to be opaque white or every segment comes out tinted. */
	cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
	cairo_move_to(cr, x + pad, y + (height - logical.height) / 2);
	pango_cairo_show_layout(cr, layout);
	pango_layout_set_attributes(layout, NULL);
	pango_attr_list_unref(attrs);
	pango_font_description_free(font);
}

static const GowlBarPluginVTable title_vtable = {
	sizeof(GowlBarPluginVTable),
	title_create, title_destroy,
	NULL, NULL, title_configure,
	NULL, title_poll, NULL,
	title_measure, title_draw,
	NULL, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL,
	NULL
};

/* ----------------------------------------------------------------
 * layout -- the active layout's symbol
 * ---------------------------------------------------------------- */

/*
 * The symbol of the output being asked about.
 *
 * Read on demand rather than parked in the plugin's label: the label is
 * shared by every screen, so on a second monitor the symbol was
 * whichever layout the FOCUSED screen was running --- and clicking it
 * cycled a layout the symbol was not describing.
 *
 * Borrowed from the monitor, which outlives the call.
 */
static const gchar *
layout_symbol(GowlBarPlugin *plugin)
{
	GowlMonitor *mon = bar_plugin_monitor(plugin);

	if (mon == NULL)
		return NULL;
	return gowl_monitor_get_layout_symbol(mon);
}

static void
layout_signature(GowlBarPlugin *plugin, gpointer data, GString *out)
{
	const gchar *symbol;

	(void)data;

	symbol = layout_symbol(plugin);
	g_string_append(out, (symbol != NULL) ? symbol : "");
}

static gint
layout_measure(GowlBarPlugin *plugin, gpointer data, PangoLayout *layout,
               const GowlBarTheme *theme, gint height)
{
	(void)data;

	return gowl_bar_plugin_measure_text(plugin, layout, theme, height,
	                                    layout_symbol(plugin));
}

static void
layout_draw(GowlBarPlugin *plugin, gpointer data, cairo_t *cr,
            PangoLayout *layout, const GowlBarTheme *theme, gint x, gint y,
            gint width, gint height, gboolean hovered, gboolean panel_open)
{
	(void)data;

	gowl_bar_plugin_draw_text(plugin, cr, layout, theme, x, y, width,
	                          height, hovered, panel_open,
	                          layout_symbol(plugin));
}

static gboolean
layout_click(GowlBarPlugin *plugin, gpointer data, guint button, gint x,
             gint y, guint modifiers)
{
	GowlCompositor *comp;
	GowlMonitor *mon;

	(void)data;
	(void)x;
	(void)y;
	(void)modifiers;

	comp = core_compositor();
	if (comp == NULL)
		return FALSE;

	/* The output the click landed on.  Clicking the layout symbol in
	   the second screen's bar used to cycle the FOCUSED screen's
	   layout, so the symbol you clicked did not change and a window
	   somewhere else moved. */
	mon = bar_plugin_monitor(plugin);
	if (mon == NULL)
		return FALSE;

	/* Left cycles forwards, right backwards --- the same convention
	   dwm's layout symbol has always had. */
	gowl_layout_cycle(comp, mon, (button == BTN_RIGHT) ? -1 : 1);
	return TRUE;
}

static const GowlBarPluginVTable layout_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL,
	NULL, NULL, NULL,
	NULL, NULL, NULL,
	layout_measure, layout_draw,
	layout_click, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL,
	layout_signature
};

/* ----------------------------------------------------------------
 * clock -- the time, with a calendar panel
 * ---------------------------------------------------------------- */

typedef struct {
	gint month_offset;   /* months away from today, for paging */
} ClockData;

static gpointer
clock_create(GowlBarPlugin *plugin)
{
	(void)plugin;
	return g_new0(ClockData, 1);
}

static void
clock_destroy(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	g_free(data);
}

static gint
clock_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	/* Once a second would repaint the bar sixty times a minute to
	   change a digit once -- unless the format shows seconds, in
	   which case anything slower shows them jumping in fives. */
	if (bar_strftime_has_seconds(
		    gowl_bar_plugin_get_setting(plugin, "format")))
		return 1;
	return 5;
}

static void
clock_poll(GowlBarPlugin *plugin, gpointer data)
{
	const gchar *format;
	time_t now;
	struct tm tm_buf;
	gchar buf[128];

	(void)data;

	format = gowl_bar_plugin_get_setting(plugin, "format");
	if (format == NULL || format[0] == '\0')
		format = "%a %b %d  %H:%M";

	now = time(NULL);
	localtime_r(&now, &tm_buf);
	if (strftime(buf, sizeof(buf), format, &tm_buf) == 0)
		buf[0] = '\0';
	gowl_bar_plugin_set_label(plugin, buf);
}

/* Build the month grid: a weekday header row, then six week rows, each
   preceded by its ISO week number. */
static void
clock_add_calendar(GowlBarPanel *panel, ClockData *cd)
{
	GowlBarPanelItem *grid;
	GDateTime *now, *shown, *cursor;
	const gchar *weekdays[7] = { "SUN", "MON", "TUE", "WED", "THU",
	                             "FRI", "SAT" };
	gint year, month, today_year, today_month, today_day;
	gint first_dow, row, col;
	GDateTime *first;

	now = g_date_time_new_now_local();
	shown = g_date_time_add_months(now, cd->month_offset);

	year        = g_date_time_get_year(shown);
	month       = g_date_time_get_month(shown);
	today_year  = g_date_time_get_year(now);
	today_month = g_date_time_get_month(now);
	today_day   = g_date_time_get_day_of_month(now);

	first = g_date_time_new_local(year, month, 1, 0, 0, 0.0);
	/* g_date_time_get_day_of_week is 1 = Monday; the grid starts on
	   Sunday, which is where the extra modulo comes from. */
	first_dow = g_date_time_get_day_of_week(first) % 7;

	grid = gowl_bar_panel_item_new(GOWL_BAR_ITEM_CALENDAR);
	gowl_bar_panel_item_set_id(grid, "day");

	/* Header row: the week-number gutter plus seven weekday names. */
	{
		GowlBarPanelItem *cell;
		gint i;

		cell = gowl_bar_panel_item_new(GOWL_BAR_ITEM_LABEL);
		gowl_bar_panel_item_set_title(cell, "W");
		gowl_bar_panel_item_add_child(grid, cell);

		for (i = 0; i < 7; i++) {
			cell = gowl_bar_panel_item_new(GOWL_BAR_ITEM_LABEL);
			gowl_bar_panel_item_set_title(cell, weekdays[i]);
			gowl_bar_panel_item_add_child(grid, cell);
		}
	}

	cursor = g_date_time_add_days(first, -first_dow);
	for (row = 0; row < 6; row++) {
		GowlBarPanelItem *cell;
		gchar buf[16];

		/* The ISO week the row is mostly in: a Sunday-first row's
		   Sunday belongs to the week BEFORE its other six days. */
		g_snprintf(buf, sizeof(buf), "%d", bar_iso_week_of_row(cursor));
		cell = gowl_bar_panel_item_new(GOWL_BAR_ITEM_LABEL);
		gowl_bar_panel_item_set_title(cell, buf);
		gowl_bar_panel_item_add_child(grid, cell);

		for (col = 0; col < 7; col++) {
			GDateTime *next;
			gboolean in_month, is_today;

			in_month = (g_date_time_get_month(cursor) == month &&
			            g_date_time_get_year(cursor) == year);
			is_today = (g_date_time_get_year(cursor) == today_year &&
			            g_date_time_get_month(cursor) == today_month &&
			            g_date_time_get_day_of_month(cursor) ==
			            today_day);

			g_snprintf(buf, sizeof(buf), "%d",
			           g_date_time_get_day_of_month(cursor));
			cell = gowl_bar_panel_item_new(GOWL_BAR_ITEM_LABEL);
			gowl_bar_panel_item_set_title(cell, buf);
			gowl_bar_panel_item_set_disabled(cell, !in_month);
			gowl_bar_panel_item_set_active(cell, is_today);
			/* Weekends get the accent so the shape of the month
			   is readable without counting columns. */
			if (in_month && (col == 0 || col == 6))
				gowl_bar_panel_item_set_color(cell,
					GOWL_BAR_COLOR_MAUVE);
			gowl_bar_panel_item_add_child(grid, cell);

			next = g_date_time_add_days(cursor, 1);
			g_date_time_unref(cursor);
			cursor = next;
		}
	}

	gowl_bar_panel_add(panel, grid);

	g_date_time_unref(cursor);
	g_date_time_unref(first);
	g_date_time_unref(shown);
	g_date_time_unref(now);
}

static GowlBarPanel *
clock_panel(GowlBarPlugin *plugin, gpointer data)
{
	ClockData *cd = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *row;
	GDateTime *now, *shown;
	g_autofree gchar *heading = NULL;
	g_autofree gchar *month_name = NULL;
	g_autofree gchar *progress_label = NULL;
	gdouble year_fraction;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 430);

	now   = g_date_time_new_now_local();
	shown = g_date_time_add_months(now, cd->month_offset);

	heading = g_date_time_format(now, "%B %-d");
	{
		g_autofree gchar *weekday = NULL;

		weekday = g_date_time_format(now, "%A");
		gowl_bar_panel_add_hero(panel, "\xef\x81\xb3", heading,
		                        weekday);
	}

	/* How far through the year we are: a small thing, and the one
	   readout a calendar can give you that a calendar cannot. */
	{
		GDateTime *year_end;
		gint days_in_year;

		year_end = g_date_time_new_local(g_date_time_get_year(now),
		                                 12, 31, 0, 0, 0.0);
		days_in_year = (year_end != NULL)
			? g_date_time_get_day_of_year(year_end) : 365;
		if (year_end != NULL)
			g_date_time_unref(year_end);

		year_fraction = (gdouble)(g_date_time_get_day_of_year(now) - 1)
		                / (gdouble)days_in_year;
	}

	progress_label = g_strdup_printf("%d", g_date_time_get_year(now));
	row = gowl_bar_panel_add_progress(panel, progress_label,
	                                  year_fraction);
	{
		g_autofree gchar *pct = NULL;

		pct = g_strdup_printf("%d%%", (gint)(year_fraction * 100.0));
		gowl_bar_panel_item_set_value(row, pct);
	}

	gowl_bar_panel_add_separator(panel);
	clock_add_calendar(panel, cd);

	row = gowl_bar_panel_add_buttons(panel, "month");
	gowl_bar_panel_add_button(row, "\xe2\x80\xb9", FALSE);
	month_name = g_date_time_format(shown, "%B %Y");
	gowl_bar_panel_add_button(row, month_name, TRUE);
	gowl_bar_panel_add_button(row, "\xe2\x80\xba", FALSE);

	g_date_time_unref(shown);
	g_date_time_unref(now);
	return panel;
}

static void
clock_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
             gint index, gdouble value, guint button)
{
	ClockData *cd = data;

	(void)plugin;
	(void)value;
	(void)button;

	if (g_strcmp0(item_id, "month") != 0)
		return;

	switch (index) {
	case 0:
		cd->month_offset--;
		break;
	case 1:
		cd->month_offset = 0;   /* the label doubles as "today" */
		break;
	case 2:
		cd->month_offset++;
		break;
	default:
		break;
	}
}

static const GowlBarPluginVTable clock_vtable = {
	sizeof(GowlBarPluginVTable),
	clock_create, clock_destroy,
	NULL, NULL, NULL,
	clock_interval, clock_poll, NULL,
	NULL, NULL,
	NULL, NULL,
	clock_panel, clock_action,
	NULL, NULL,
	NULL,
	NULL
};

/* ----------------------------------------------------------------
 * spacer -- fixed empty width
 * ---------------------------------------------------------------- */

static gint
spacer_measure(GowlBarPlugin *plugin, gpointer data, PangoLayout *layout,
               const GowlBarTheme *theme, gint height)
{
	(void)data;
	(void)layout;
	(void)theme;
	(void)height;
	return gowl_bar_plugin_get_setting_int(plugin, "param",
		gowl_bar_plugin_get_setting_int(plugin, "width", 16));
}

static void
spacer_draw(GowlBarPlugin *plugin, gpointer data, cairo_t *cr,
            PangoLayout *layout, const GowlBarTheme *theme, gint x, gint y,
            gint width, gint height, gboolean hovered, gboolean panel_open)
{
	(void)plugin;
	(void)data;
	(void)layout;
	(void)hovered;
	(void)panel_open;

	if (!gowl_bar_plugin_get_setting_bool(plugin, "rule", FALSE))
		return;

	/* A spacer can optionally draw a divider, which is the only way
	   to separate two groups inside one region. */
	gowl_bar_theme_cairo_set_alpha(theme, cr, GOWL_BAR_COLOR_OVERLAY, 0.4);
	cairo_rectangle(cr, x + width / 2, y + height / 4, 1, height / 2);
	cairo_fill(cr);
}

static const GowlBarPluginVTable spacer_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL,
	NULL, NULL, NULL,
	NULL, NULL, NULL,
	spacer_measure, spacer_draw,
	NULL, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL,
	NULL
};

/* ----------------------------------------------------------------
 * button -- a glyph that runs a command
 * ---------------------------------------------------------------- */

static void
button_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	const gchar *icon;
	const gchar *label;

	(void)data;
	(void)settings;

	icon = gowl_bar_plugin_get_setting(plugin, "icon");
	gowl_bar_plugin_set_icon(plugin, icon);

	label = gowl_bar_plugin_get_setting(plugin, "label");
	gowl_bar_plugin_set_label(plugin, label);

	/* With neither an icon nor a label there would be nothing to
	   click, so fall back to something visible rather than a
	   zero-width widget the user cannot find. */
	if ((icon == NULL || icon[0] == '\0') &&
	    (label == NULL || label[0] == '\0'))
		gowl_bar_plugin_set_icon(plugin, "\xe2\x97\x8f");

	gowl_bar_plugin_set_tooltip(plugin,
		gowl_bar_plugin_get_setting(plugin, "tooltip"));
	core_apply_color_setting(plugin, GOWL_BAR_COLOR_TEXT);
}

static gboolean
button_click(GowlBarPlugin *plugin, gpointer data, guint button, gint x,
             gint y, guint modifiers)
{
	const gchar *command;

	(void)data;
	(void)x;
	(void)y;
	(void)modifiers;

	/* `command' is the left button; `command-right' and
	   `command-middle' are optional, so one widget can carry three
	   related actions the way a tray icon does. */
	command = NULL;
	if (button == BTN_RIGHT)
		command = gowl_bar_plugin_get_setting(plugin, "command-right");
	else if (button == BTN_MIDDLE)
		command = gowl_bar_plugin_get_setting(plugin, "command-middle");
	if (command == NULL)
		command = gowl_bar_plugin_get_setting(plugin, "command");
	if (command == NULL)
		command = gowl_bar_plugin_get_setting(plugin, "param");

	if (command == NULL || command[0] == '\0')
		return FALSE;

	gowl_bar_plugin_spawn(plugin, command);
	return TRUE;
}

static const GowlBarPluginVTable button_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL,
	NULL, NULL, button_configure,
	NULL, NULL, NULL,
	NULL, NULL,
	button_click, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL,
	NULL
};

/* ----------------------------------------------------------------
 * toggle -- a stateful button
 * ---------------------------------------------------------------- */

typedef struct {
	gboolean active;
	gboolean pending;
} ToggleData;

static gpointer
toggle_create(GowlBarPlugin *plugin)
{
	(void)plugin;
	return g_new0(ToggleData, 1);
}

static void
toggle_destroy(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	g_free(data);
}

static gint
toggle_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	/* Only worth polling when there is a state command to poll. */
	if (gowl_bar_plugin_get_setting(plugin, "state-command") == NULL)
		return 3600;
	return 5;
}

/* Refresh the label and colour from the current state. */
static void
toggle_apply(GowlBarPlugin *plugin, ToggleData *td)
{
	const gchar *icon;
	const gchar *label;

	icon = gowl_bar_plugin_get_setting(plugin,
		td->active ? "icon-on" : "icon-off");
	if (icon == NULL)
		icon = gowl_bar_plugin_get_setting(plugin, "icon");
	gowl_bar_plugin_set_icon(plugin, icon);

	label = gowl_bar_plugin_get_setting(plugin,
		td->active ? "label-on" : "label-off");
	if (label == NULL)
		label = gowl_bar_plugin_get_setting(plugin, "label");
	gowl_bar_plugin_set_label(plugin, label);

	if (icon == NULL && label == NULL)
		gowl_bar_plugin_set_icon(plugin,
			td->active ? "\xe2\x97\x8f" : "\xe2\x97\x8b");

	/*
	 * Hover text.  A toggle is a single glyph whose meaning is entirely
	 * in the configuration -- `toggle:caffeine' renders a cup and
	 * nothing else -- so without this the one widget that most needs
	 * naming is the one that cannot be named.
	 */
	{
		const gchar *tip;

		tip = gowl_bar_plugin_get_setting(plugin,
			td->active ? "tooltip-on" : "tooltip-off");
		if (tip == NULL)
			tip = gowl_bar_plugin_get_setting(plugin, "tooltip");
		if (tip == NULL)
			tip = label;      /* better than the type's name */
		gowl_bar_plugin_set_tooltip(plugin, tip);
	}

	bar_plugin_apply_color(plugin,
		gowl_bar_plugin_get_setting(plugin,
			td->active ? "color-on" : "color-off"),
		td->active ? GOWL_BAR_COLOR_ACCENT : GOWL_BAR_COLOR_MUTED);
}

static void
toggle_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	(void)settings;
	toggle_apply(plugin, data);
}

static void
toggle_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	ToggleData *td = data;
	const gchar *state_cmd;
	g_autofree gchar *out = NULL;
	gboolean active;

	state_cmd = gowl_bar_plugin_get_setting(plugin, "state-command");
	if (state_cmd == NULL || state_cmd[0] == '\0')
		return;

	/* Exit status alone would be the obvious contract, but the shell
	   idioms people already have (`pgrep -x foo', `systemctl
	   is-active') communicate through output as often as status, so
	   any non-empty output that is not a falsey word counts as on. */
	out = bar_run_shell_line(state_cmd);
	active = (out != NULL && out[0] != '\0' &&
	          g_ascii_strcasecmp(out, "0") != 0 &&
	          g_ascii_strcasecmp(out, "no") != 0 &&
	          g_ascii_strcasecmp(out, "off") != 0 &&
	          g_ascii_strcasecmp(out, "inactive") != 0 &&
	          g_ascii_strcasecmp(out, "false") != 0);

	if (active != td->active || td->pending) {
		td->active  = active;
		td->pending = FALSE;
		toggle_apply(plugin, td);
	}
}

static gboolean
toggle_click(GowlBarPlugin *plugin, gpointer data, guint button, gint x,
             gint y, guint modifiers)
{
	ToggleData *td = data;
	const gchar *command;

	(void)x;
	(void)y;
	(void)modifiers;

	if (button != BTN_LEFT)
		return FALSE;

	command = gowl_bar_plugin_get_setting(plugin,
		td->active ? "command-off" : "command-on");
	if (command == NULL)
		command = gowl_bar_plugin_get_setting(plugin, "command");
	if (command == NULL || command[0] == '\0')
		return FALSE;

	gowl_bar_plugin_spawn(plugin, command);

	/* Flip optimistically so the widget answers the click at once;
	   the next state poll corrects it if the command failed. */
	td->active  = !td->active;
	td->pending = TRUE;
	toggle_apply(plugin, td);
	return TRUE;
}

static const GowlBarPluginVTable toggle_vtable = {
	sizeof(GowlBarPluginVTable),
	toggle_create, toggle_destroy,
	NULL, NULL, toggle_configure,
	toggle_interval, NULL, toggle_poll_async,
	NULL, NULL,
	toggle_click, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL,
	NULL
};

/* ----------------------------------------------------------------
 * cmd -- a command's first line of output
 * ---------------------------------------------------------------- */

static gint
cmd_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 10;
}

static void
cmd_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	const gchar *command;
	g_autofree gchar *out = NULL;

	(void)data;

	command = gowl_bar_plugin_get_setting(plugin, "param");
	if (command == NULL)
		command = gowl_bar_plugin_get_setting(plugin, "command");
	if (command == NULL || command[0] == '\0')
		return;

	out = bar_run_shell_line(command);
	/* Strip ANSI escapes rather than rendering them: the old bar
	   coloured them, but a widget that changes colour from a
	   script's output fights the palette, and the `color' setting
	   expresses the same intent properly. */
	bar_strip_ansi(out);
	gowl_bar_plugin_set_label(plugin, out);
}

static gboolean
cmd_click(GowlBarPlugin *plugin, gpointer data, guint button, gint x, gint y,
          guint modifiers)
{
	const gchar *command;

	(void)data;
	(void)x;
	(void)y;
	(void)modifiers;

	/* The same three slots a button has, so the two generic widgets
	   are configured the same way.  `on-click' is the left button's
	   name here because `command' is the polled one. */
	command = NULL;
	if (button == BTN_RIGHT)
		command = gowl_bar_plugin_get_setting(plugin, "command-right");
	else if (button == BTN_MIDDLE)
		command = gowl_bar_plugin_get_setting(plugin, "command-middle");
	if (command == NULL)
		command = gowl_bar_plugin_get_setting(plugin, "on-click");
	if (command == NULL || command[0] == '\0')
		return FALSE;

	gowl_bar_plugin_spawn(plugin, command);
	return TRUE;
}

static void
cmd_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	(void)data;
	(void)settings;
	gowl_bar_plugin_set_icon(plugin,
		gowl_bar_plugin_get_setting(plugin, "icon"));
	core_apply_color_setting(plugin, GOWL_BAR_COLOR_TEXT);
}

static const GowlBarPluginVTable cmd_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL,
	NULL, NULL, cmd_configure,
	cmd_interval, NULL, cmd_poll_async,
	NULL, NULL,
	cmd_click, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL,
	NULL
};

/* ----------------------------------------------------------------
 * label -- static text
 * ---------------------------------------------------------------- */

static void
label_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	const gchar *text;

	(void)data;
	(void)settings;

	text = gowl_bar_plugin_get_setting(plugin, "param");
	if (text == NULL)
		text = gowl_bar_plugin_get_setting(plugin, "text");
	gowl_bar_plugin_set_label(plugin, text);
	gowl_bar_plugin_set_icon(plugin,
		gowl_bar_plugin_get_setting(plugin, "icon"));
	core_apply_color_setting(plugin, GOWL_BAR_COLOR_SUBTEXT);
}

static const GowlBarPluginVTable label_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL,
	NULL, NULL, label_configure,
	NULL, NULL, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL,
	NULL
};

/* ----------------------------------------------------------------
 * Registration
 * ---------------------------------------------------------------- */

/**
 * bar_register_core_plugins:
 * @registry: the registry to populate
 *
 * Registers the window-manager widgets and the generic ones.
 */
void
bar_register_core_plugins(GowlBarRegistry *registry)
{
	gowl_bar_registry_register_vtable(registry, "tags", "Tags",
		"dwm-style tag row for the output it is drawn on",
		&tags_vtable);
	gowl_bar_registry_register_vtable(registry, "title", "Window title",
		"The focused window's title, optionally colourised",
		&title_vtable);
	gowl_bar_registry_register_vtable(registry, "layout", "Layout",
		"The active layout's symbol; click to cycle",
		&layout_vtable);
	gowl_bar_registry_register_vtable(registry, "clock", "Clock",
		"The time, with a calendar panel", &clock_vtable);
	gowl_bar_registry_register_vtable(registry, "spacer", "Spacer",
		"Fixed empty width, optionally a divider rule",
		&spacer_vtable);
	gowl_bar_registry_register_vtable(registry, "button", "Button",
		"A glyph that runs a command", &button_vtable);
	gowl_bar_registry_register_vtable(registry, "toggle", "Toggle",
		"A stateful button driven by a state command",
		&toggle_vtable);
	gowl_bar_registry_register_vtable(registry, "cmd", "Command",
		"The first line of a command's output", &cmd_vtable);
	gowl_bar_registry_register_vtable(registry, "label", "Label",
		"Static text", &label_vtable);

	gowl_bar_registry_register_alias(registry, "time", "clock");
	gowl_bar_registry_register_alias(registry, "workspaces", "tags");
	gowl_bar_registry_register_alias(registry, "window", "title");
	gowl_bar_registry_register_alias(registry, "text", "label");
}
