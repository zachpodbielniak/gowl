/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The system tray, as a row of icons in the bar.
 *
 * The register is src/tray: who is there and what each one says about
 * itself.  This is the half that puts it on a screen and turns a click
 * into one of the three things a tray item can be told.
 *
 * THE CLICKS ARE NOT THE USUAL ONES, deliberately.  Most trays activate
 * on a single left click, which on a bar somebody is also using to
 * switch tags means opening Zoom by brushing past it.  Here:
 *
 *   double left    activate -- which for nearly every application means
 *                  "show me your window".  Two clicks because one is
 *                  too easy to do by accident on a strip of 22-pixel
 *                  targets
 *   right          the application's own menu
 *   middle         secondary activate, which is whatever the
 *                  application decided that means
 *   scroll         forwarded, which is how a volume applet works
 *
 * An item that sets `ItemIsMenu' says it has no Activate at all and
 * wants its menu instead; those get the menu from a double click too,
 * because the alternative is a double click that silently does nothing.
 *
 * WHAT HAPPENS TO THE WINDOW afterwards is not this file's business and
 * that is the point: Activate makes the application show a window, the
 * window maps, and gowl tiles it like any other.  An application that
 * should float is a windowrules entry, the same as everything else.
 */

#include "bar-internal.h"
#include "bar-tray-icon.h"

#include <string.h>

#include <linux/input-event-codes.h>

#include "tray/gowl-tray.h"

/* How long two clicks may be apart and still be one double click. */
#define TRAY_DOUBLE_CLICK_US (400000)

/* The gap between icons, and the smallest an icon may be drawn. */
#define TRAY_GAP     (6)
#define TRAY_MIN_PX  (12)

typedef struct {
	GHashTable *icons;        /* cache key -> cairo_surface_t* */

	/* The last draw's layout, which is also the hit test: a click
	 * arrives in widget coordinates and has to become an item. */
	GPtrArray  *shown;        /* GowlTrayItem*, this draw's order */
	gint        slot;         /* pixels per item, gap included */
	gint        icon_px;

	/* Double-click bookkeeping. */
	gint64      last_click_us;
	gint        last_click_slot;

	/* Which item's menu the panel is showing, and how far into it. */
	gchar      *menu_key;
	GArray     *menu_path;    /* gint ids, the drill-in trail */
} TrayData;

/* ── Layout ──────────────────────────────────────────────────────── */

/*
 * Which items are drawn.
 *
 * `Passive' means the application is saying "nothing to see": the
 * specification's own advice is not to show it.  Everything else is
 * shown, including items whose icon could not be resolved -- those get
 * a lettered placeholder, because an application that is running and
 * has no square is indistinguishable from one that has crashed.
 */
static void
tray_collect(TrayData *d)
{
	GPtrArray *items = gowl_tray_dup_items(gowl_tray_get_default());
	guint i;

	g_ptr_array_set_size(d->shown, 0);
	for (i = 0; items != NULL && i < items->len; i++) {
		GowlTrayItem *item = g_ptr_array_index(items, i);

		if (g_strcmp0(item->status, "Passive") == 0)
			continue;
		g_ptr_array_add(d->shown, gowl_tray_item_copy(item));
	}
	if (items != NULL)
		g_ptr_array_unref(items);
}

static gpointer
tray_create(GowlBarPlugin *plugin)
{
	TrayData *d = g_new0(TrayData, 1);

	(void)plugin;
	d->icons = bar_tray_icon_cache_new();
	d->shown = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_tray_item_free);
	d->menu_path = g_array_new(FALSE, FALSE, sizeof(gint));
	d->last_click_slot = -1;
	return d;
}

static void
tray_destroy(GowlBarPlugin *plugin, gpointer data)
{
	TrayData *d = data;

	(void)plugin;
	if (d == NULL)
		return;
	g_hash_table_unref(d->icons);
	g_ptr_array_unref(d->shown);
	g_array_unref(d->menu_path);
	g_free(d->menu_key);
	g_free(d);
}

static gint
tray_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	/*
	 * Never really polled.  The register tells the compositor when it
	 * changes and the bar's signature picks that up, so a tray that
	 * nothing is happening in costs nothing at all.
	 */
	return 3600;
}

static gint
tray_measure(GowlBarPlugin *plugin, gpointer data, PangoLayout *layout,
             const GowlBarTheme *theme, gint height)
{
	TrayData *d = data;
	gint icon;

	(void)plugin;
	(void)layout;
	(void)theme;

	tray_collect(d);
	if (d->shown->len == 0)
		return 0;

	/* Square, inset from the bar so icons do not touch its edges. */
	icon = MAX(TRAY_MIN_PX, height - 8);
	d->icon_px = icon;
	d->slot = icon + TRAY_GAP;
	return (gint)d->shown->len * d->slot - TRAY_GAP;
}

static void
tray_draw(GowlBarPlugin *plugin, gpointer data, cairo_t *cr,
          PangoLayout *layout, const GowlBarTheme *theme,
          gint x, gint y, gint width, gint height,
          gboolean hovered, gboolean panel_open)
{
	TrayData *d = data;
	guint i;

	(void)plugin;
	(void)width;
	(void)hovered;
	(void)panel_open;

	for (i = 0; i < d->shown->len; i++) {
		GowlTrayItem *item = g_ptr_array_index(d->shown, i);
		cairo_surface_t *icon = bar_tray_icon_for(d->icons, item,
		                                          d->icon_px);
		gint ix = x + (gint)i * d->slot;
		gint iy = y + (height - d->icon_px) / 2;

		if (icon != NULL) {
			cairo_save(cr);
			cairo_set_source_surface(cr, icon, ix, iy);
			cairo_paint(cr);
			cairo_restore(cr);
		} else {
			/*
			 * Nothing resolved.  A letter rather than a gap: the
			 * application IS running and its menu still works, and an
			 * empty space says the opposite.
			 */
			const gchar *from = (item->title != NULL && *item->title)
				? item->title : (item->id != NULL ? item->id : "?");
			gchar initial[8];
			gint tw = 0, th = 0;

			const gdouble *c;

			memset(initial, 0, sizeof(initial));
			g_utf8_strncpy(initial, from, 1);

			c = gowl_bar_theme_color(theme, GOWL_BAR_COLOR_OVERLAY);
			cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);
			cairo_set_line_width(cr, 1.0);
			cairo_rectangle(cr, ix + 0.5, iy + 0.5,
			                d->icon_px - 1, d->icon_px - 1);
			cairo_stroke(cr);

			pango_layout_set_text(layout, initial, -1);
			pango_layout_get_pixel_size(layout, &tw, &th);
			c = gowl_bar_theme_color(theme, GOWL_BAR_COLOR_TEXT);
			cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);
			cairo_move_to(cr, ix + (d->icon_px - tw) / 2.0,
			              iy + (d->icon_px - th) / 2.0);
			pango_cairo_show_layout(cr, layout);
		}

		/*
		 * An item asking for attention gets an underline.  Not a
		 * different icon -- the application may already have swapped
		 * its own -- and not a colour wash, which would fight whatever
		 * the icon's own colours are saying.
		 */
		if (g_strcmp0(item->status, "NeedsAttention") == 0) {
			const gdouble *c = gowl_bar_theme_color(theme,
			                                        GOWL_BAR_COLOR_RED);

			cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);
			cairo_rectangle(cr, ix, iy + d->icon_px + 1, d->icon_px, 2);
			cairo_fill(cr);
		}
	}
}

/*
 * What the bar folds into its change detection.
 *
 * Load bearing: the bar skips a screen's repaint when the signature has
 * not moved, and everything this widget draws is something other than
 * its own label.  Without the serial in here a tray icon would change
 * on the bus and never change on the screen.
 */
static void
tray_signature(GowlBarPlugin *plugin, gpointer data, GString *out)
{
	TrayData *d = data;

	(void)plugin;
	g_string_append_printf(out, "tray:%u:%u",
	                       gowl_tray_get_serial(gowl_tray_get_default()),
	                       d->shown != NULL ? d->shown->len : 0u);
}

/* ── Clicks ──────────────────────────────────────────────────────── */

static GowlTrayItem *
tray_item_at(TrayData *d, gint x)
{
	gint index;

	if (d->slot <= 0 || d->shown->len == 0 || x < 0)
		return NULL;
	index = x / d->slot;
	if (index < 0 || index >= (gint)d->shown->len)
		return NULL;
	/* The gap between two icons belongs to neither. */
	if (x - index * d->slot >= d->icon_px)
		return NULL;
	return g_ptr_array_index(d->shown, index);
}

static void
tray_open_menu_for(GowlBarPlugin *plugin, TrayData *d,
                   const GowlTrayItem *item)
{
	GowlBarHost *host = gowl_bar_plugin_get_host(plugin);

	g_free(d->menu_key);
	d->menu_key = g_strdup(item->key);
	g_array_set_size(d->menu_path, 0);

	/*
	 * Ask for the menu before opening the panel.  dbusmenu's AboutToShow
	 * is what tells an application to BUILD a menu it generates on
	 * demand, so a panel opened first shows an empty list once and the
	 * real thing on the second try.
	 */
	gowl_tray_menu_refresh(gowl_tray_get_default(), item->key);
	if (host != NULL)
		gowl_bar_host_open_panel(host, gowl_bar_plugin_get_id(plugin));
}

static gboolean
tray_click(GowlBarPlugin *plugin, gpointer data, guint button, gint x, gint y,
           guint modifiers)
{
	TrayData *d = data;
	GowlTrayItem *item = tray_item_at(d, x);
	gint64 now = g_get_monotonic_time();
	gint slot;

	(void)modifiers;
	if (item == NULL)
		return FALSE;
	slot = (d->slot > 0) ? x / d->slot : 0;

	if (button == BTN_RIGHT) {
		tray_open_menu_for(plugin, d, item);
		return TRUE;
	}
	if (button == BTN_MIDDLE) {
		gowl_tray_secondary_activate(gowl_tray_get_default(), item->key,
		                             x, y);
		return TRUE;
	}
	if (button != BTN_LEFT)
		return FALSE;

	if (d->last_click_slot == slot
	    && now - d->last_click_us < TRAY_DOUBLE_CLICK_US) {
		d->last_click_us = 0;
		d->last_click_slot = -1;
		/*
		 * An item that says it is a menu has no Activate to call, so
		 * the double click opens the menu instead.  A double click
		 * that silently does nothing is worse than the wrong one.
		 */
		if (item->item_is_menu && item->menu_path != NULL)
			tray_open_menu_for(plugin, d, item);
		else
			gowl_tray_activate(gowl_tray_get_default(), item->key, x, y);
		return TRUE;
	}

	d->last_click_us = now;
	d->last_click_slot = slot;
	/* Consumed even though nothing happened, so the host does not read
	 * the first of a double click as "open this widget's panel". */
	return TRUE;
}

static gboolean
tray_scroll(GowlBarPlugin *plugin, gpointer data, gdouble delta,
            gint discrete, guint modifiers)
{
	TrayData *d = data;
	GowlTrayItem *item;

	(void)plugin;
	(void)modifiers;
	/*
	 * A scroll carries no position, so it goes to the only item when
	 * there is one and is otherwise declined: guessing which of six
	 * icons the user meant to change the volume of is worse than doing
	 * nothing.
	 */
	if (d->shown->len != 1)
		return FALSE;
	item = g_ptr_array_index(d->shown, 0);
	gowl_tray_scroll(gowl_tray_get_default(), item->key,
	                 discrete != 0 ? discrete : (delta > 0 ? 1 : -1),
	                 "vertical");
	return TRUE;
}

/* ── The menu, as a panel ────────────────────────────────────────── */

/* Walk to the submenu the drill-in trail currently names. */
static GowlTrayMenuItem *
menu_at_path(GowlTrayMenuItem *root, GArray *path)
{
	GowlTrayMenuItem *node = root;
	guint i;

	for (i = 0; node != NULL && i < path->len; i++) {
		gint want = g_array_index(path, gint, i);
		GowlTrayMenuItem *next = NULL;
		guint c;

		if (node->children == NULL)
			return node;
		for (c = 0; c < node->children->len && next == NULL; c++) {
			GowlTrayMenuItem *kid = g_ptr_array_index(node->children, c);

			if (kid->id == want)
				next = kid;
		}
		node = next;
	}
	return node;
}

static GowlBarPanel *
tray_panel(GowlBarPlugin *plugin, gpointer data)
{
	TrayData *d = data;
	GowlBarPanel *panel;
	GowlTrayMenuItem *root = NULL;
	GowlTrayMenuItem *node;
	guint i;

	(void)plugin;
	panel = gowl_bar_panel_new();
	if (d->menu_key == NULL)
		return panel;

	root = gowl_tray_dup_menu(gowl_tray_get_default(), d->menu_key);
	node = menu_at_path(root, d->menu_path);
	if (node == NULL || node->children == NULL || node->children->len == 0) {
		/*
		 * Either it has not arrived yet or the application has none.
		 * Said out loud rather than shown as an empty box, because an
		 * empty box is what a broken tray looks like too.
		 */
		gowl_bar_panel_add_label(panel, root == NULL
			? "Asking the application for its menu..."
			: "This application offers no menu");
		if (root != NULL)
			gowl_tray_menu_item_free(root);
		return panel;
	}

	/* A way back out of a submenu, since a panel is a flat list and
	 * there is nowhere else to put the trail. */
	if (d->menu_path->len > 0) {
		GowlBarPanelItem *back =
			gowl_bar_panel_add_field(panel, "Back", NULL);

		gowl_bar_panel_item_set_id(back, "..");
		gowl_bar_panel_add_separator(panel);
	}

	for (i = 0; i < node->children->len; i++) {
		GowlTrayMenuItem *row = g_ptr_array_index(node->children, i);
		GowlBarPanelItem *pi;
		gchar *id;

		if (!row->visible)
			continue;
		if (row->is_separator) {
			gowl_bar_panel_add_separator(panel);
			continue;
		}

		pi = gowl_bar_panel_add_field(panel,
			row->label != NULL ? row->label : "", NULL);
		id = g_strdup_printf("%d", row->id);
		/* A disabled row keeps its text and loses its id, which is what
		 * makes it inert: the host reports nothing for an item with no
		 * id, so there is no way to act on it by accident. */
		if (row->enabled)
			gowl_bar_panel_item_set_id(pi, id);
		g_free(id);

		/* A checkmark or a radio, shown as the value rather than as a
		 * toggle row: a dbusmenu toggle is the application's state and
		 * flipping it is a click on the row like any other. */
		if (row->toggle_type != NULL) {
			gowl_bar_panel_item_set_value(pi,
				row->toggle_state == 1 ? "on"
				: (row->toggle_state == 0 ? "off" : "--"));
		}
		if (row->children != NULL && row->children->len > 0)
			gowl_bar_panel_item_set_badge(pi, ">");
	}
	gowl_tray_menu_item_free(root);
	return panel;
}

static void
tray_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
            gint index, gdouble value, guint button)
{
	TrayData *d = data;
	GowlTrayMenuItem *root = NULL;
	GowlTrayMenuItem *node, *row = NULL;
	gint id;
	guint i;

	(void)index;
	(void)value;
	(void)button;
	if (item_id == NULL || d->menu_key == NULL)
		return;

	if (g_strcmp0(item_id, "..") == 0) {
		if (d->menu_path->len > 0)
			g_array_set_size(d->menu_path, d->menu_path->len - 1);
		gowl_bar_plugin_request_panel_refresh(plugin);
		return;
	}

	id = (gint)g_ascii_strtoll(item_id, NULL, 10);
	root = gowl_tray_dup_menu(gowl_tray_get_default(), d->menu_key);
	node = menu_at_path(root, d->menu_path);
	if (node == NULL || node->children == NULL) {
		if (root != NULL)
			gowl_tray_menu_item_free(root);
		return;
	}
	for (i = 0; i < node->children->len && row == NULL; i++) {
		GowlTrayMenuItem *kid = g_ptr_array_index(node->children, i);

		if (kid->id == id)
			row = kid;
	}
	if (row == NULL) {
		gowl_tray_menu_item_free(root);
		return;
	}

	/* A row with children is a way in, not a thing to do. */
	if (row->children != NULL && row->children->len > 0) {
		g_array_append_val(d->menu_path, id);
		gowl_tray_menu_item_free(root);
		gowl_bar_plugin_request_panel_refresh(plugin);
		return;
	}

	gowl_tray_menu_item_free(root);
	gowl_tray_menu_clicked(gowl_tray_get_default(), d->menu_key, id);
}

static void
tray_panel_closed(GowlBarPlugin *plugin, gpointer data)
{
	TrayData *d = data;

	(void)plugin;
	/* The next right click chooses the item and the depth afresh; a
	 * panel that reopened three levels into the menu somebody left
	 * would be its own small mystery. */
	g_clear_pointer(&d->menu_key, g_free);
	g_array_set_size(d->menu_path, 0);
}

static const GowlBarPluginVTable tray_vtable = {
	sizeof(GowlBarPluginVTable),
	tray_create, tray_destroy,
	NULL, NULL, NULL,
	tray_interval, NULL, NULL,
	tray_measure, tray_draw,
	tray_click, tray_scroll,
	tray_panel, tray_action,
	NULL, tray_panel_closed,
	NULL,
	tray_signature
};

void
bar_register_tray_plugins(GowlBarRegistry *registry)
{
	gowl_bar_registry_register_vtable(registry, "tray", "System tray",
		"Icons for applications that live in the background",
		&tray_vtable);
	gowl_bar_registry_register_alias(registry, "systray", "tray");
}
