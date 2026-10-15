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

/*
 * gowl-floatdesk-dock.c - the floating desktop's dock.
 *
 * One dock per output, shown only while that output views a floating
 * workspace.  It lists the pinned applications, then every other
 * running one -- windows grouped by application through the catalog
 * (gowl-floatdesk-apps.c), a minimized window always among them -- and
 * an "all apps" button at the end.
 *
 *   left click      raise and focus the app's window (restoring it if
 *                   minimized); again: the next one, current workspace
 *                   first, then minimized, then other workspaces; an app
 *                   with no window is launched, and pulses until its
 *                   first window maps
 *   middle click    a new window
 *   right click     keep in dock / remove, new window, quit
 *   drag            reorder; dragging a running app pins it where it
 *                   lands; dragging from the apps grid pins too
 *   right click on the apps button: position and auto-hide
 *
 * It is drawn by the compositor itself (a scene buffer in the TOP
 * layer, menus and the grid in OVERLAY) rather than being a layer-shell
 * client: it has to know about minimized windows, workspace modes and
 * maximize state that no protocol carries.  Hidden under a fullscreen
 * window; under a maximized one, or with auto-hide on, it slides off
 * its edge and comes back when the pointer touches that edge.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-floatdesk"

#include <math.h>
#include <string.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/util/box.h>

#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-frame-sink.h"
#include "barkit/gowl-bar-icon.h"

#include "gowl-floatdesk-private.h"

#define DOCK_PAD        (8)      /* around each icon */
#define DOCK_OUTER      (6)      /* inside the dock's edge */
#define DOCK_MARGIN     (8)      /* from the output's edge */
#define DOCK_SEPARATOR  (12)     /* before the apps button */
#define DOCK_EDGE_HIT   (2)      /* the edge that reveals a hidden dock */
#define DOCK_DRAG_START (6)      /* pixels before a press becomes a drag */
#define DOCK_TICK_MS    (16)
#define DOCK_SLIDE_US   (160 * 1000)
#define DOCK_LAUNCH_US  (15 * G_USEC_PER_SEC)
#define MENU_WIDTH      (220)
#define MENU_ROW        (30)
#define GRID_CELL       (104)
#define GRID_ICON       (48)
#define GRID_PAD        (24)

/* What a press is being held on. */
typedef enum {
	PRESS_NONE = 0,
	PRESS_ITEM,
	PRESS_GRID_BUTTON,
	PRESS_GRID_APP,
	PRESS_MENU
} PressKind;

typedef struct {
	gchar                  *key;
	gchar                  *label;
	gchar                  *icon;
	gboolean                pinned;
	gboolean                launchable;
	GPtrArray              *clients;     /* GowlClient*, cycle order */
	guint                   minimized;
	gboolean                focused;
	gboolean                launching;
} DockItem;

typedef struct {
	gchar   *label;
	gchar   *action;
	gboolean checked;
} MenuEntry;

struct _FloatdeskDock {
	GowlModuleFloatdesk     *self;
	GowlMonitor             *mon;
	struct wlr_scene_buffer *buffer;
	GPtrArray               *items;      /* DockItem* */
	guint                    npins;
	gboolean                 vertical;
	struct wlr_box           shown;      /* layout coords, fully shown */
	gint                     slot;
	gboolean                 visible;    /* shown at all on this view */
	gboolean                 hiding;     /* auto-hide or maximized */
	gdouble                  reveal;     /* 0 off-screen .. 1 shown */
	gboolean                 want_reveal;
	gint64                   anim_from_us;
	gdouble                  anim_from;
	gboolean                 pointer_near;
	gint                     hover;

	/* a press in progress */
	PressKind                press;
	gint                     press_index;
	gdouble                  press_x;
	gdouble                  press_y;
	gboolean                 dragging;
	gint                     drop_index;
	gchar                   *drag_key;
	struct wlr_scene_buffer *drag_icon;

	/* a menu */
	struct wlr_scene_buffer *menu;
	struct wlr_box           menu_rect;
	GPtrArray               *menu_entries; /* MenuEntry* */
	gchar                   *menu_key;     /* NULL: the dock's own menu */

	/* the apps grid */
	struct wlr_scene_buffer *grid;
	struct wlr_box           grid_rect;
	GPtrArray               *grid_apps;    /* GowlFloatdeskApp*, catalog's */
	gint                     grid_cols;
	gint                     grid_scroll;  /* rows scrolled */
};

/* ================================================================ */
/* Small helpers                                                     */
/* ================================================================ */

static void
item_free(gpointer data)
{
	DockItem *it = data;

	g_free(it->key);
	g_free(it->label);
	g_free(it->icon);
	g_ptr_array_unref(it->clients);
	g_free(it);
}

static void
menu_entry_free(gpointer data)
{
	MenuEntry *e = data;

	g_free(e->label);
	g_free(e->action);
	g_free(e);
}

static GHashTable *
dock_table(GowlModuleFloatdesk *self)
{
	if (self->docks == NULL)
		self->docks = g_hash_table_new(g_direct_hash, g_direct_equal);
	return self->docks;
}

static GowlFloatdeskCatalog *
catalog(GowlModuleFloatdesk *self)
{
	if (self->catalog == NULL) {
		self->catalog = gowl_floatdesk_catalog_new();
		gowl_floatdesk_catalog_load_system(self->catalog);
	}
	return self->catalog;
}

/*
 * client_listed:
 * @c: a window
 *
 * Returns: %TRUE for a window the dock shows: managed, mapped, ours
 */
static gboolean
client_listed(GowlClient *c)
{
	return c->scene != NULL && !c->isoverlay && !c->isunmanaged
	       && !c->isembedded;
}

static gdouble
output_scale(GowlMonitor *m)
{
	return (m != NULL && m->wlr_output != NULL)
		? MAX(1.0, m->wlr_output->scale) : 1.0;
}

/*
 * push_buffer:
 * @sb: the scene buffer
 * @surface: a finished cairo image
 * @w: the logical width
 * @h: the logical height
 */
static void
push_buffer(
	struct wlr_scene_buffer *sb,
	cairo_surface_t         *surface,
	gint                     w,
	gint                     h
){
	struct wlr_buffer *buffer;

	cairo_surface_flush(surface);
	buffer = gowl_raw_buffer_create(cairo_image_surface_get_data(surface),
		cairo_image_surface_get_width(surface),
		cairo_image_surface_get_height(surface),
		cairo_image_surface_get_stride(surface));
	if (buffer == NULL)
		return;
	wlr_scene_buffer_set_buffer(sb, buffer);
	wlr_buffer_drop(buffer);
	wlr_scene_buffer_set_dest_size(sb, w, h);
}

/*
 * draw_icon:
 * @self: the module
 * @cr: cairo, logical units
 * @icon: (nullable): icon name or path
 * @label: (nullable): for the letter tile when there is no icon
 * @x: top-left
 * @y: top-left
 * @size: the square
 * @scale: the output scale, for the pixel size looked up
 * @alpha: faded or not
 */
static void
draw_icon(
	GowlModuleFloatdesk *self,
	cairo_t             *cr,
	const gchar         *icon,
	const gchar         *label,
	gdouble              x,
	gdouble              y,
	gint                 size,
	gdouble              scale,
	gdouble              alpha
){
	cairo_surface_t *img = NULL;
	gint             px = (gint)ceil(size * scale);

	if (self->icons == NULL)
		self->icons = gowl_bar_icon_cache_new();
	if (icon != NULL && *icon != '\0')
		img = gowl_bar_icon_lookup(self->icons, icon, NULL, px);

	if (img != NULL) {
		gdouble iw = cairo_image_surface_get_width(img);

		cairo_save(cr);
		cairo_translate(cr, x, y);
		cairo_scale(cr, size / MAX(1.0, iw), size / MAX(1.0, iw));
		cairo_set_source_surface(cr, img, 0, 0);
		cairo_paint_with_alpha(cr, alpha);
		cairo_restore(cr);
		return;
	}

	/* No icon: a rounded tile with the name's first letter. */
	{
		PangoLayout          *layout;
		PangoFontDescription *font;
		gchar                 letter[8] = { 0 };
		gint                  tw, th;

		gowl_bar_cairo_rounded_rect(cr, x, y, size, size, size * 0.22);
		gowl_bar_theme_cairo_set_alpha(self->theme, cr,
		                               GOWL_BAR_COLOR_ACCENT, alpha);
		cairo_fill(cr);
		if (label != NULL && *label != '\0') {
			gunichar ch = g_unichar_toupper(g_utf8_get_char(label));

			g_unichar_to_utf8(ch, letter);
		}
		layout = pango_cairo_create_layout(cr);
		font = pango_font_description_from_string("sans bold");
		pango_font_description_set_absolute_size(font,
			size * 0.5 * PANGO_SCALE);
		pango_layout_set_font_description(layout, font);
		pango_font_description_free(font);
		pango_layout_set_text(layout, letter, -1);
		pango_layout_get_pixel_size(layout, &tw, &th);
		gowl_bar_theme_cairo_set_alpha(self->theme, cr,
		                               GOWL_BAR_COLOR_CRUST, alpha);
		cairo_move_to(cr, x + (size - tw) / 2.0, y + (size - th) / 2.0);
		pango_cairo_show_layout(cr, layout);
		g_object_unref(layout);
	}
}

/* ================================================================ */
/* The model                                                         */
/* ================================================================ */

/*
 * client_rank:
 * @m: the output the dock is on
 * @c: a window
 *
 * The cycle order a click walks: windows on the workspace in view first,
 * minimized ones second, everything else after.
 */
static gint
client_rank(
	GowlMonitor *m,
	GowlClient  *c
){
	if (c->isminimized)
		return 1;
	if (c->mon == m && (c->tags & m->tagset[m->seltags]) != 0)
		return 0;
	return 2;
}

/*
 * build_items:
 * @dock: the dock
 *
 * Pins in order, then the rest of the running applications in the order
 * their first window appears in the client list.
 */
static void
build_items(FloatdeskDock *dock)
{
	GowlModuleFloatdesk  *self = dock->self;
	GowlFloatdeskCatalog *cat = catalog(self);
	GHashTable           *by_key;
	g_auto(GStrv)         pins = NULL;
	GowlClient           *focused;
	GList                *l;
	guint                 i;
	gint64                now = g_get_monotonic_time();

	g_ptr_array_set_size(dock->items, 0);
	by_key = g_hash_table_new(g_str_hash, g_str_equal);
	focused = self->compositor->selmon != NULL
		? focustop_public(self->compositor, self->compositor->selmon)
		: NULL;

	pins = floatdesk_dock_pins(self);
	for (i = 0; pins[i] != NULL; i++) {
		const GowlFloatdeskApp *app;
		DockItem               *it;

		if (g_hash_table_contains(by_key, pins[i]))
			continue;
		app = gowl_floatdesk_catalog_lookup(cat, pins[i]);
		if (app == NULL)
			app = gowl_floatdesk_catalog_resolve(cat, pins[i]);
		it = g_new0(DockItem, 1);
		it->key = g_strdup(app != NULL ? app->id : pins[i]);
		it->label = g_strdup(app != NULL ? app->name : pins[i]);
		it->icon = g_strdup(app != NULL && app->icon != NULL
		                    ? app->icon : pins[i]);
		it->pinned = TRUE;
		it->launchable = app != NULL && app->exec != NULL;
		it->clients = g_ptr_array_new();
		g_ptr_array_add(dock->items, it);
		g_hash_table_insert(by_key, it->key, it);
	}
	dock->npins = dock->items->len;

	for (l = self->compositor->clients; l != NULL; l = l->next) {
		GowlClient       *c = l->data;
		g_autofree gchar *key = NULL;
		DockItem         *it;

		if (!client_listed(c))
			continue;
		key = gowl_floatdesk_catalog_key(cat, c->app_id);
		it = g_hash_table_lookup(by_key, key);
		if (it == NULL) {
			const GowlFloatdeskApp *app;

			app = gowl_floatdesk_catalog_resolve(cat, c->app_id);
			it = g_new0(DockItem, 1);
			it->key = g_steal_pointer(&key);
			it->label = g_strdup(app != NULL ? app->name
			                     : (c->app_id != NULL ? c->app_id
			                        : (c->title != NULL ? c->title
			                           : "?")));
			it->icon = g_strdup(app != NULL && app->icon != NULL
			                    ? app->icon : c->app_id);
			it->launchable = app != NULL && app->exec != NULL;
			it->clients = g_ptr_array_new();
			g_ptr_array_add(dock->items, it);
			g_hash_table_insert(by_key, it->key, it);
		}
		g_ptr_array_add(it->clients, c);
		if (c->isminimized)
			it->minimized++;
		if (c == focused)
			it->focused = TRUE;
	}

	/* Order each app's windows for the click cycle; a stable sort, so
	 * windows of equal rank keep the client list's order. */
	for (i = 0; i < dock->items->len; i++) {
		DockItem *it = g_ptr_array_index(dock->items, i);
		guint     a, b;

		for (a = 1; a < it->clients->len; a++) {
			for (b = a; b > 0; b--) {
				GowlClient *x = g_ptr_array_index(it->clients, b - 1);
				GowlClient *y = g_ptr_array_index(it->clients, b);

				if (client_rank(dock->mon, x) <= client_rank(dock->mon, y))
					break;
				it->clients->pdata[b - 1] = y;
				it->clients->pdata[b] = x;
			}
		}
		if (self->launching != NULL) {
			gpointer deadline = g_hash_table_lookup(self->launching,
			                                        it->key);

			it->launching = deadline != NULL && it->clients->len == 0
				&& (gint64)GPOINTER_TO_SIZE(deadline) > now;
		}
	}
	g_hash_table_destroy(by_key);
}

/*
 * layout_dock:
 * @dock: the dock
 *
 * Where the dock sits fully shown, from its position and item count.
 */
static void
layout_dock(FloatdeskDock *dock)
{
	GowlModuleFloatdesk *self = dock->self;
	const gchar         *pos = floatdesk_dock_position(self);
	struct wlr_box      *w = &dock->mon->w;
	gint                 n = (gint)dock->items->len;
	gint                 along, across;

	dock->slot = self->dock_icon_size + 2 * DOCK_PAD;
	dock->vertical = g_strcmp0(pos, "left") == 0
	                 || g_strcmp0(pos, "right") == 0;
	along = 2 * DOCK_OUTER + n * dock->slot
	        + (n > 0 ? DOCK_SEPARATOR : 0) + dock->slot;
	across = 2 * DOCK_OUTER + dock->slot;

	if (!dock->vertical) {
		dock->shown.width = MIN(along, w->width);
		dock->shown.height = across;
		dock->shown.x = w->x + (w->width - dock->shown.width) / 2;
		dock->shown.y = g_strcmp0(pos, "top") == 0
			? w->y + DOCK_MARGIN
			: w->y + w->height - across - DOCK_MARGIN;
	} else {
		dock->shown.width = across;
		dock->shown.height = MIN(along, w->height);
		dock->shown.y = w->y + (w->height - dock->shown.height) / 2;
		dock->shown.x = g_strcmp0(pos, "left") == 0
			? w->x + DOCK_MARGIN
			: w->x + w->width - across - DOCK_MARGIN;
	}
}

/*
 * slot_box:
 * @dock: the dock
 * @index: an item, or items->len for the apps button
 * @out: (out): its box, relative to the dock
 */
static void
slot_box(
	FloatdeskDock  *dock,
	gint            index,
	struct wlr_box *out
){
	gint offset = DOCK_OUTER + index * dock->slot;

	if (index >= (gint)dock->items->len && dock->items->len > 0)
		offset += DOCK_SEPARATOR;
	if (!dock->vertical)
		*out = (struct wlr_box){ offset, DOCK_OUTER, dock->slot, dock->slot };
	else
		*out = (struct wlr_box){ DOCK_OUTER, offset, dock->slot, dock->slot };
}

/*
 * current_box:
 * @dock: the dock
 * @out: (out): where the dock is drawn now, sliding included
 */
static void
current_box(
	FloatdeskDock  *dock,
	struct wlr_box *out
){
	const gchar *pos = floatdesk_dock_position(dock->self);
	gdouble      hide = 1.0 - dock->reveal;
	gint         dist;

	*out = dock->shown;
	if (!dock->vertical) {
		dist = dock->shown.height + DOCK_MARGIN + 2;
		out->y += (gint)(hide * (g_strcmp0(pos, "top") == 0 ? -dist : dist));
	} else {
		dist = dock->shown.width + DOCK_MARGIN + 2;
		out->x += (gint)(hide * (g_strcmp0(pos, "left") == 0 ? -dist : dist));
	}
}

/*
 * index_at:
 * @dock: the dock
 * @lx: a layout point
 * @ly: a layout point
 *
 * Returns: the item under the point, items->len for the apps button, -1
 *   for none
 */
static gint
index_at(
	FloatdeskDock *dock,
	gdouble        lx,
	gdouble        ly
){
	struct wlr_box box;
	gint           i;

	if (!dock->visible || dock->reveal <= 0.01)
		return -1;
	current_box(dock, &box);
	if (!wlr_box_contains_point(&box, lx, ly))
		return -1;
	for (i = 0; i <= (gint)dock->items->len; i++) {
		struct wlr_box s;

		slot_box(dock, i, &s);
		s.x += box.x;
		s.y += box.y;
		if (wlr_box_contains_point(&s, lx, ly))
			return i;
	}
	return -1;
}

/*
 * drop_index_at:
 * @dock: the dock
 * @lx: the pointer
 * @ly: the pointer
 *
 * Returns: the position among the PINS a drag would land at
 */
static gint
drop_index_at(
	FloatdeskDock *dock,
	gdouble        lx,
	gdouble        ly
){
	struct wlr_box box;
	gdouble        along;
	gint           idx;

	current_box(dock, &box);
	along = dock->vertical ? ly - box.y : lx - box.x;
	idx = (gint)floor((along - DOCK_OUTER + dock->slot / 2.0) / dock->slot);
	return CLAMP(idx, 0, (gint)dock->npins);
}

/* ================================================================ */
/* Drawing                                                           */
/* ================================================================ */

static void
dock_render(FloatdeskDock *dock)
{
	GowlModuleFloatdesk *self = dock->self;
	cairo_surface_t     *surface;
	cairo_t             *cr;
	gdouble              scale = output_scale(dock->mon);
	gint                 w = dock->shown.width, h = dock->shown.height;
	gint                 i, isz = self->dock_icon_size;
	gdouble              pulse;

	if (self->theme == NULL || w <= 0 || h <= 0)
		return;
	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		(gint)ceil(w * scale), (gint)ceil(h * scale));
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return;
	}
	cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);

	gowl_bar_cairo_rounded_rect(cr, 0.5, 0.5, w - 1, h - 1, 14);
	gowl_bar_theme_cairo_set_alpha(self->theme, cr, GOWL_BAR_COLOR_MANTLE,
	                               0.92);
	cairo_fill_preserve(cr);
	gowl_bar_theme_cairo_set_alpha(self->theme, cr, GOWL_BAR_COLOR_SURFACE,
	                               0.9);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);

	pulse = 0.55 + 0.45 * sin(g_get_monotonic_time() / 120000.0);

	for (i = 0; i < (gint)dock->items->len; i++) {
		DockItem      *it = g_ptr_array_index(dock->items, i);
		struct wlr_box s;
		gdouble        alpha = 1.0;
		guint          running = it->clients->len - it->minimized;
		guint          dots, d;

		/* A drag is under way: leave the dragged item's slot dim. */
		if (dock->dragging && g_strcmp0(dock->drag_key, it->key) == 0)
			alpha = 0.3;
		else if (it->launching)
			alpha = pulse;
		else if (it->clients->len == 0)
			alpha = 0.45;

		slot_box(dock, i, &s);
		if (i == dock->hover && !dock->dragging) {
			gowl_bar_cairo_rounded_rect(cr, s.x + 2, s.y + 2,
			                            s.width - 4, s.height - 4, 10);
			gowl_bar_theme_cairo_set_alpha(self->theme, cr,
			                               GOWL_BAR_COLOR_SURFACE, 0.8);
			cairo_fill(cr);
		}
		draw_icon(self, cr, it->icon, it->label, s.x + DOCK_PAD,
		          s.y + DOCK_PAD, isz, scale, alpha);

		/* Running windows as dots along the edge facing the screen
		 * edge; a minimized one as a ring; the focused app's in the
		 * accent colour. */
		dots = MIN(3u, it->clients->len);
		for (d = 0; d < dots; d++) {
			gdouble cx, cy;
			gdouble off = ((gdouble)d - (dots - 1) / 2.0) * 7.0;

			if (!dock->vertical) {
				cx = s.x + s.width / 2.0 + off;
				cy = s.y + s.height - 3.5;
			} else {
				cx = s.x + s.width - 3.5;
				cy = s.y + s.height / 2.0 + off;
			}
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, 2.2, 0, 2 * G_PI);
			gowl_bar_theme_cairo_set(self->theme, cr, it->focused
				? GOWL_BAR_COLOR_ACCENT : GOWL_BAR_COLOR_TEXT);
			if (d < running) {
				cairo_fill(cr);
			} else {
				cairo_set_line_width(cr, 1.0);
				cairo_stroke(cr);
			}
		}
	}

	/* The insertion point of a drag. */
	if (dock->dragging && dock->drop_index >= 0) {
		struct wlr_box s;

		slot_box(dock, dock->drop_index, &s);
		gowl_bar_theme_cairo_set(self->theme, cr, GOWL_BAR_COLOR_ACCENT);
		if (!dock->vertical)
			cairo_rectangle(cr, s.x - 1.5, s.y + 4, 3, s.height - 8);
		else
			cairo_rectangle(cr, s.x + 4, s.y - 1.5, s.width - 8, 3);
		cairo_fill(cr);
	}

	/* The separator and the apps button: a 3x3 grid of dots. */
	{
		struct wlr_box s;
		gint           r, c;

		slot_box(dock, (gint)dock->items->len, &s);
		if (dock->items->len > 0) {
			gowl_bar_theme_cairo_set_alpha(self->theme, cr,
			                               GOWL_BAR_COLOR_OVERLAY, 0.7);
			if (!dock->vertical)
				cairo_rectangle(cr, s.x - DOCK_SEPARATOR / 2.0 - 0.5,
				                s.y + 10, 1, s.height - 20);
			else
				cairo_rectangle(cr, s.x + 10,
				                s.y - DOCK_SEPARATOR / 2.0 - 0.5,
				                s.width - 20, 1);
			cairo_fill(cr);
		}
		if (dock->hover == (gint)dock->items->len || dock->grid != NULL) {
			gowl_bar_cairo_rounded_rect(cr, s.x + 2, s.y + 2,
			                            s.width - 4, s.height - 4, 10);
			gowl_bar_theme_cairo_set_alpha(self->theme, cr,
			                               GOWL_BAR_COLOR_SURFACE, 0.8);
			cairo_fill(cr);
		}
		gowl_bar_theme_cairo_set(self->theme, cr, GOWL_BAR_COLOR_TEXT);
		for (r = 0; r < 3; r++) {
			for (c = 0; c < 3; c++) {
				cairo_new_path(cr);
				cairo_arc(cr,
				          s.x + s.width / 2.0 + (c - 1) * isz * 0.25,
				          s.y + s.height / 2.0 + (r - 1) * isz * 0.25,
				          isz * 0.06, 0, 2 * G_PI);
				cairo_fill(cr);
			}
		}
	}

	cairo_destroy(cr);
	push_buffer(dock->buffer, surface, w, h);
	cairo_surface_destroy(surface);
}

/*
 * dock_place:
 * @dock: the dock
 *
 * Puts the drawn dock at its current slide position, or hides it.
 */
static void
dock_place(FloatdeskDock *dock)
{
	struct wlr_box box;

	if (!dock->visible || dock->reveal <= 0.001) {
		wlr_scene_node_set_enabled(&dock->buffer->node, FALSE);
		return;
	}
	current_box(dock, &box);
	wlr_scene_node_set_position(&dock->buffer->node, box.x, box.y);
	wlr_scene_node_set_enabled(&dock->buffer->node, TRUE);
	wlr_scene_node_raise_to_top(&dock->buffer->node);
}

/* ================================================================ */
/* Menus                                                             */
/* ================================================================ */

static void
menu_close(FloatdeskDock *dock)
{
	if (dock->menu != NULL) {
		wlr_scene_node_destroy(&dock->menu->node);
		dock->menu = NULL;
	}
	g_clear_pointer(&dock->menu_entries, g_ptr_array_unref);
	g_clear_pointer(&dock->menu_key, g_free);
}

static void
menu_add(
	FloatdeskDock *dock,
	const gchar   *label,
	const gchar   *action,
	gboolean       checked
){
	MenuEntry *e = g_new0(MenuEntry, 1);

	e->label = g_strdup(label);
	e->action = g_strdup(action);
	e->checked = checked;
	g_ptr_array_add(dock->menu_entries, e);
}

static void
menu_render(FloatdeskDock *dock)
{
	GowlModuleFloatdesk *self = dock->self;
	cairo_surface_t     *surface;
	cairo_t             *cr;
	gdouble              scale = output_scale(dock->mon);
	gint                 w = dock->menu_rect.width, h = dock->menu_rect.height;
	guint                i;

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		(gint)ceil(w * scale), (gint)ceil(h * scale));
	cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);
	gowl_bar_cairo_rounded_rect(cr, 0.5, 0.5, w - 1, h - 1, 10);
	gowl_bar_theme_cairo_set_alpha(self->theme, cr, GOWL_BAR_COLOR_BASE,
	                               0.97);
	cairo_fill_preserve(cr);
	gowl_bar_theme_cairo_set(self->theme, cr, GOWL_BAR_COLOR_SURFACE);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);

	for (i = 0; i < dock->menu_entries->len; i++) {
		MenuEntry            *e = g_ptr_array_index(dock->menu_entries, i);
		PangoLayout          *layout;
		PangoFontDescription *font;
		gint                  tw, th;
		gdouble               y = 6 + i * MENU_ROW;

		if (e->action == NULL) {
			/* A heading. */
			gowl_bar_theme_cairo_set(self->theme, cr,
			                         GOWL_BAR_COLOR_SUBTEXT);
		} else {
			gowl_bar_theme_cairo_set(self->theme, cr,
			                         GOWL_BAR_COLOR_TEXT);
		}
		layout = pango_cairo_create_layout(cr);
		font = pango_font_description_from_string(
			gowl_bar_theme_get_font(self->theme) != NULL
			? gowl_bar_theme_get_font(self->theme) : "sans 10");
		pango_layout_set_font_description(layout, font);
		pango_font_description_free(font);
		pango_layout_set_text(layout, e->label, -1);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
		pango_layout_set_width(layout, (w - 44) * PANGO_SCALE);
		pango_layout_get_pixel_size(layout, &tw, &th);
		cairo_move_to(cr, 30, y + (MENU_ROW - th) / 2.0);
		pango_cairo_show_layout(cr, layout);
		g_object_unref(layout);
		if (e->checked) {
			gowl_bar_theme_cairo_set(self->theme, cr,
			                         GOWL_BAR_COLOR_ACCENT);
			cairo_new_path(cr);
			cairo_arc(cr, 16, y + MENU_ROW / 2.0, 4, 0, 2 * G_PI);
			cairo_fill(cr);
		}
	}
	cairo_destroy(cr);
	push_buffer(dock->menu, surface, w, h);
	cairo_surface_destroy(surface);
}

/*
 * menu_open:
 * @dock: the dock
 * @key: (nullable): the app the menu is for; %NULL for the dock's own
 * @lx: where the click was
 * @ly: where the click was
 */
static void
menu_open(
	FloatdeskDock *dock,
	const gchar   *key,
	gdouble        lx,
	gdouble        ly
){
	GowlModuleFloatdesk *self = dock->self;
	struct wlr_box      *area = &dock->mon->m;

	menu_close(dock);
	dock->menu_entries = g_ptr_array_new_with_free_func(menu_entry_free);
	dock->menu_key = g_strdup(key);

	if (key != NULL) {
		DockItem *it = NULL;
		guint     i;

		for (i = 0; i < dock->items->len; i++) {
			DockItem *x = g_ptr_array_index(dock->items, i);

			if (g_strcmp0(x->key, key) == 0)
				it = x;
		}
		if (it == NULL) {
			menu_close(dock);
			return;
		}
		menu_add(dock, it->label, NULL, FALSE);
		menu_add(dock, it->pinned ? "Remove from dock" : "Keep in dock",
		         it->pinned ? "unpin" : "pin", it->pinned);
		if (it->launchable)
			menu_add(dock, "New window", "new", FALSE);
		if (it->clients->len > 0)
			menu_add(dock, it->clients->len > 1 ? "Quit all windows"
			                                    : "Quit", "quit", FALSE);
	} else {
		const gchar *pos = floatdesk_dock_position(self);

		menu_add(dock, "Dock position", NULL, FALSE);
		menu_add(dock, "Bottom", "pos:bottom", g_strcmp0(pos, "bottom") == 0);
		menu_add(dock, "Left", "pos:left", g_strcmp0(pos, "left") == 0);
		menu_add(dock, "Right", "pos:right", g_strcmp0(pos, "right") == 0);
		menu_add(dock, "Top", "pos:top", g_strcmp0(pos, "top") == 0);
		menu_add(dock, "Auto-hide", "autohide",
		         floatdesk_dock_autohide(self));
	}

	dock->menu_rect.width = MENU_WIDTH;
	dock->menu_rect.height = 12 + (gint)dock->menu_entries->len * MENU_ROW;
	dock->menu_rect.x = CLAMP((gint)lx - MENU_WIDTH / 2, area->x + 4,
		area->x + area->width - MENU_WIDTH - 4);
	/* Open away from the dock's edge. */
	dock->menu_rect.y = (gint)ly - dock->menu_rect.height - 8;
	if (dock->menu_rect.y < area->y + 4)
		dock->menu_rect.y = (gint)ly + 8;
	dock->menu_rect.y = CLAMP(dock->menu_rect.y, area->y + 4,
		area->y + area->height - dock->menu_rect.height - 4);

	dock->menu = wlr_scene_buffer_create(
		self->compositor->layers[GOWL_SCENE_LAYER_OVERLAY], NULL);
	if (dock->menu == NULL) {
		menu_close(dock);
		return;
	}
	menu_render(dock);
	wlr_scene_node_set_position(&dock->menu->node, dock->menu_rect.x,
	                            dock->menu_rect.y);
	wlr_scene_node_raise_to_top(&dock->menu->node);
}

/*
 * menu_choose:
 * @dock: the dock
 * @row: the row clicked
 */
static void
menu_choose(
	FloatdeskDock *dock,
	gint           row
){
	GowlModuleFloatdesk *self = dock->self;
	MenuEntry           *e;
	g_autofree gchar    *key = NULL;
	g_autofree gchar    *action = NULL;
	g_autofree gchar    *reply = NULL;

	if (dock->menu_entries == NULL || row < 0
	    || row >= (gint)dock->menu_entries->len)
		return;
	e = g_ptr_array_index(dock->menu_entries, row);
	if (e->action == NULL)
		return;
	action = g_strdup(e->action);
	key = g_strdup(dock->menu_key);
	menu_close(dock);

	if (g_strcmp0(action, "pin") == 0) {
		floatdesk_dock_pin(self, key, -1);
		floatdesk_save(self);
	} else if (g_strcmp0(action, "unpin") == 0) {
		floatdesk_dock_unpin(self, key);
		floatdesk_save(self);
	} else if (g_strcmp0(action, "new") == 0) {
		reply = floatdesk_dock_launch(self, key);
	} else if (g_strcmp0(action, "quit") == 0) {
		guint i;

		for (i = 0; i < dock->items->len; i++) {
			DockItem *it = g_ptr_array_index(dock->items, i);
			guint     j;

			if (g_strcmp0(it->key, key) != 0)
				continue;
			for (j = 0; j < it->clients->len; j++)
				gowl_client_close(g_ptr_array_index(it->clients, j));
		}
	} else if (g_str_has_prefix(action, "pos:")) {
		gowl_floatdesk_state_set_dock_position(self->state, action + 4);
		floatdesk_save(self);
	} else if (g_strcmp0(action, "autohide") == 0) {
		gowl_floatdesk_state_set_dock_autohide(self->state,
			floatdesk_dock_autohide(self) ? 0 : 1);
		floatdesk_save(self);
	}
	floatdesk_dock_refresh_all(self);
}

/* ================================================================ */
/* The apps grid                                                     */
/* ================================================================ */

static void
grid_close(FloatdeskDock *dock)
{
	if (dock->grid != NULL) {
		wlr_scene_node_destroy(&dock->grid->node);
		dock->grid = NULL;
	}
	g_clear_pointer(&dock->grid_apps, g_ptr_array_unref);
	dock->grid_scroll = 0;
}

/*
 * grid_rows_visible:
 * @dock: the dock
 *
 * Returns: how many rows of cells fit in the panel
 */
static gint
grid_rows_visible(FloatdeskDock *dock)
{
	return MAX(1, (dock->grid_rect.height - 2 * GRID_PAD) / GRID_CELL);
}

static void
grid_render(FloatdeskDock *dock)
{
	GowlModuleFloatdesk *self = dock->self;
	cairo_surface_t     *surface;
	cairo_t             *cr;
	gdouble              scale = output_scale(dock->mon);
	gint                 w = dock->grid_rect.width, h = dock->grid_rect.height;
	gint                 rows = grid_rows_visible(dock);
	guint                i;

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		(gint)ceil(w * scale), (gint)ceil(h * scale));
	cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);
	gowl_bar_cairo_rounded_rect(cr, 0.5, 0.5, w - 1, h - 1, 18);
	gowl_bar_theme_cairo_set_alpha(self->theme, cr, GOWL_BAR_COLOR_BASE,
	                               0.96);
	cairo_fill_preserve(cr);
	gowl_bar_theme_cairo_set(self->theme, cr, GOWL_BAR_COLOR_SURFACE);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);

	for (i = 0; dock->grid_apps != NULL && i < dock->grid_apps->len; i++) {
		const GowlFloatdeskApp *app = g_ptr_array_index(dock->grid_apps, i);
		PangoLayout            *layout;
		PangoFontDescription   *font;
		gint                    row = (gint)i / dock->grid_cols - dock->grid_scroll;
		gint                    col = (gint)i % dock->grid_cols;
		gdouble                 x, y;
		gint                    tw, th;

		if (row < 0 || row >= rows)
			continue;
		x = GRID_PAD + col * GRID_CELL;
		y = GRID_PAD + row * GRID_CELL;
		draw_icon(self, cr, app->icon, app->name,
		          x + (GRID_CELL - GRID_ICON) / 2.0, y + 10, GRID_ICON,
		          scale, 1.0);
		layout = pango_cairo_create_layout(cr);
		font = pango_font_description_from_string(
			gowl_bar_theme_get_font(self->theme) != NULL
			? gowl_bar_theme_get_font(self->theme) : "sans 9");
		pango_layout_set_font_description(layout, font);
		pango_font_description_free(font);
		pango_layout_set_text(layout, app->name, -1);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
		pango_layout_set_width(layout, (GRID_CELL - 8) * PANGO_SCALE);
		pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
		pango_layout_get_pixel_size(layout, &tw, &th);
		gowl_bar_theme_cairo_set(self->theme, cr, GOWL_BAR_COLOR_TEXT);
		cairo_move_to(cr, x + 4, y + 10 + GRID_ICON + 8);
		pango_cairo_show_layout(cr, layout);
		g_object_unref(layout);
	}
	cairo_destroy(cr);
	push_buffer(dock->grid, surface, w, h);
	cairo_surface_destroy(surface);
}

static void
grid_open(FloatdeskDock *dock)
{
	GowlModuleFloatdesk *self = dock->self;
	struct wlr_box      *area = &dock->mon->w;

	menu_close(dock);
	grid_close(dock);

	/* Re-read the installed applications: the grid is where a newly
	 * installed one is looked for. */
	g_clear_pointer(&self->catalog, gowl_floatdesk_catalog_free);
	dock->grid_apps = gowl_floatdesk_catalog_list(catalog(self));

	dock->grid_rect.width = MIN(area->width - 80, 8 * GRID_CELL + 2 * GRID_PAD);
	dock->grid_cols = MAX(1, (dock->grid_rect.width - 2 * GRID_PAD) / GRID_CELL);
	dock->grid_rect.width = dock->grid_cols * GRID_CELL + 2 * GRID_PAD;
	dock->grid_rect.height = MIN(area->height - 160,
		(gint)((dock->grid_apps->len + dock->grid_cols - 1)
		       / dock->grid_cols) * GRID_CELL + 2 * GRID_PAD);
	dock->grid_rect.height = MAX(dock->grid_rect.height,
	                             GRID_CELL + 2 * GRID_PAD);
	dock->grid_rect.x = area->x + (area->width - dock->grid_rect.width) / 2;
	dock->grid_rect.y = area->y + (area->height - dock->grid_rect.height) / 2;

	dock->grid = wlr_scene_buffer_create(
		self->compositor->layers[GOWL_SCENE_LAYER_OVERLAY], NULL);
	if (dock->grid == NULL) {
		grid_close(dock);
		return;
	}
	grid_render(dock);
	wlr_scene_node_set_position(&dock->grid->node, dock->grid_rect.x,
	                            dock->grid_rect.y);
	wlr_scene_node_raise_to_top(&dock->grid->node);
}

/*
 * grid_app_at:
 * @dock: the dock
 * @lx: a layout point
 * @ly: a layout point
 *
 * Returns: (transfer none) (nullable): the application in the cell
 */
static const GowlFloatdeskApp *
grid_app_at(
	FloatdeskDock *dock,
	gdouble        lx,
	gdouble        ly
){
	gint col, row, idx;

	if (dock->grid == NULL || dock->grid_apps == NULL)
		return NULL;
	lx -= dock->grid_rect.x + GRID_PAD;
	ly -= dock->grid_rect.y + GRID_PAD;
	if (lx < 0 || ly < 0)
		return NULL;
	col = (gint)(lx / GRID_CELL);
	row = (gint)(ly / GRID_CELL);
	if (col >= dock->grid_cols || row >= grid_rows_visible(dock))
		return NULL;
	idx = (row + dock->grid_scroll) * dock->grid_cols + col;
	if (idx < 0 || idx >= (gint)dock->grid_apps->len)
		return NULL;
	return g_ptr_array_index(dock->grid_apps, idx);
}

/* ================================================================ */
/* Life of a dock                                                    */
/* ================================================================ */

static void
drag_icon_close(FloatdeskDock *dock)
{
	if (dock->drag_icon != NULL) {
		wlr_scene_node_destroy(&dock->drag_icon->node);
		dock->drag_icon = NULL;
	}
}

static void
press_reset(FloatdeskDock *dock)
{
	dock->press = PRESS_NONE;
	dock->press_index = -1;
	dock->dragging = FALSE;
	dock->drop_index = -1;
	g_clear_pointer(&dock->drag_key, g_free);
	drag_icon_close(dock);
}

static void
dock_free(FloatdeskDock *dock)
{
	press_reset(dock);
	menu_close(dock);
	grid_close(dock);
	if (dock->buffer != NULL)
		wlr_scene_node_destroy(&dock->buffer->node);
	g_ptr_array_unref(dock->items);
	g_free(dock);
}

static FloatdeskDock *
dock_for(
	GowlModuleFloatdesk *self,
	GowlMonitor         *m
){
	FloatdeskDock *dock;

	dock = g_hash_table_lookup(dock_table(self), m);
	if (dock != NULL)
		return dock;
	dock = g_new0(FloatdeskDock, 1);
	dock->self = self;
	dock->mon = m;
	dock->items = g_ptr_array_new_with_free_func(item_free);
	dock->hover = -1;
	dock->press_index = -1;
	dock->drop_index = -1;
	dock->reveal = 0.0;
	dock->buffer = wlr_scene_buffer_create(
		self->compositor->layers[GOWL_SCENE_LAYER_TOP], NULL);
	if (dock->buffer == NULL) {
		g_ptr_array_unref(dock->items);
		g_free(dock);
		return NULL;
	}
	wlr_scene_node_set_enabled(&dock->buffer->node, FALSE);
	g_hash_table_insert(dock_table(self), m, dock);
	return dock;
}

void
floatdesk_dock_monitor_gone(
	GowlModuleFloatdesk *self,
	GowlMonitor         *m
){
	FloatdeskDock *dock;

	if (self->docks == NULL)
		return;
	dock = g_hash_table_lookup(self->docks, m);
	if (dock == NULL)
		return;
	g_hash_table_remove(self->docks, m);
	dock_free(dock);
}

void
floatdesk_dock_destroy_all(GowlModuleFloatdesk *self)
{
	GList *mons, *l;

	if (self->docks == NULL)
		return;
	mons = g_hash_table_get_keys(self->docks);
	for (l = mons; l != NULL; l = l->next)
		floatdesk_dock_monitor_gone(self, l->data);
	g_list_free(mons);
}

static gboolean dock_tick_needed (GowlModuleFloatdesk *self);

/*
 * dock_tick:
 *
 * One frame of the slide and of the launch pulse, for every dock.
 * Stops itself when nothing is moving.
 */
static int
dock_tick(void *data)
{
	GowlModuleFloatdesk *self = data;
	GHashTableIter       iter;
	gpointer             value;
	gint64               now = g_get_monotonic_time();

	if (self->compositor == NULL || self->docks == NULL)
		return 0;
	g_hash_table_iter_init(&iter, self->docks);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		FloatdeskDock *dock = value;
		gdouble        target = dock->want_reveal ? 1.0 : 0.0;
		gboolean       pulsing = FALSE;
		guint          i;

		if (dock->reveal != target) {
			gdouble t = (now - dock->anim_from_us) / (gdouble)DOCK_SLIDE_US;

			t = CLAMP(t, 0.0, 1.0);
			/* Ease out. */
			t = 1.0 - (1.0 - t) * (1.0 - t);
			dock->reveal = dock->anim_from + (target - dock->anim_from) * t;
			if (fabs(dock->reveal - target) < 0.01 || t >= 1.0)
				dock->reveal = target;
			dock_place(dock);
		}
		for (i = 0; i < dock->items->len; i++) {
			DockItem *it = g_ptr_array_index(dock->items, i);

			if (it->launching)
				pulsing = TRUE;
		}
		if (pulsing && dock->visible) {
			/* The launch deadline may have passed. */
			build_items(dock);
			dock_render(dock);
		}
	}
	if (dock_tick_needed(self)) {
		wl_event_source_timer_update(self->dock_timer, DOCK_TICK_MS);
		return 0;
	}
	return 0;
}

static gboolean
dock_tick_needed(GowlModuleFloatdesk *self)
{
	GHashTableIter iter;
	gpointer       value;

	if (self->docks == NULL)
		return FALSE;
	g_hash_table_iter_init(&iter, self->docks);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		FloatdeskDock *dock = value;
		guint          i;

		if (dock->reveal != (dock->want_reveal ? 1.0 : 0.0))
			return TRUE;
		for (i = 0; dock->visible && i < dock->items->len; i++)
			if (((DockItem *)g_ptr_array_index(dock->items, i))->launching)
				return TRUE;
	}
	return FALSE;
}

static void
dock_tick_start(GowlModuleFloatdesk *self)
{
	if (self->compositor == NULL)
		return;
	if (self->dock_timer == NULL)
		self->dock_timer = wl_event_loop_add_timer(
			gowl_compositor_get_event_loop(self->compositor),
			dock_tick, self);
	if (self->dock_timer != NULL && dock_tick_needed(self))
		wl_event_source_timer_update(self->dock_timer, DOCK_TICK_MS);
}

/*
 * set_want_reveal:
 * @dock: the dock
 * @want: slide in or out
 */
static void
set_want_reveal(
	FloatdeskDock *dock,
	gboolean       want
){
	if (dock->want_reveal == want)
		return;
	dock->want_reveal = want;
	dock->anim_from = dock->reveal;
	dock->anim_from_us = g_get_monotonic_time();
	dock_tick_start(dock->self);
}

/*
 * dock_has_attention:
 * @dock: the dock
 *
 * Returns: %TRUE when something keeps a hiding dock out: the pointer at
 *   its edge or over it, a menu, the grid, a drag
 */
static gboolean
dock_has_attention(FloatdeskDock *dock)
{
	return dock->pointer_near || dock->menu != NULL || dock->grid != NULL
	       || dock->press != PRESS_NONE;
}

void
floatdesk_dock_refresh(
	GowlModuleFloatdesk *self,
	GowlMonitor         *m
){
	FloatdeskDock *dock;
	gboolean       fullscreen = FALSE, maximized = FALSE, show;
	GList         *l;

	if (self->compositor == NULL || m == NULL || m->wlr_output == NULL)
		return;

	show = self->dock_enabled && !self->compositor->locked
	       && floatdesk_view_floating(self, m);
	for (l = self->compositor->clients; show && l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if (c->mon != m || c->isoverlay || c->tags == 0
		    || (c->tags & m->tagset[m->seltags]) == 0)
			continue;
		if (c->isfullscreen)
			fullscreen = TRUE;
		else if (floatdesk_is_maximized(c))
			maximized = TRUE;
	}
	if (fullscreen)
		show = FALSE;

	dock = self->docks != NULL ? g_hash_table_lookup(self->docks, m) : NULL;
	if (!show) {
		if (dock != NULL) {
			press_reset(dock);
			menu_close(dock);
			grid_close(dock);
			dock->visible = FALSE;
			dock->reveal = 0.0;
			dock->want_reveal = FALSE;
			dock_place(dock);
		}
		return;
	}

	dock = dock_for(self, m);
	if (dock == NULL)
		return;
	if (!dock->visible) {
		/* Coming onto a floating view: start in, unless it hides. */
		dock->visible = TRUE;
		dock->reveal = 0.0;
		dock->anim_from = 0.0;
		dock->anim_from_us = g_get_monotonic_time();
		dock->want_reveal = FALSE;
	}
	dock->hiding = floatdesk_dock_autohide(self) || maximized;
	build_items(dock);
	layout_dock(dock);
	dock_render(dock);
	set_want_reveal(dock, !dock->hiding || dock_has_attention(dock));
	if (dock->reveal == (dock->want_reveal ? 1.0 : 0.0))
		dock_place(dock);
	dock_tick_start(self);
}

void
floatdesk_dock_refresh_all(GowlModuleFloatdesk *self)
{
	GList *l;

	if (self->compositor == NULL)
		return;
	for (l = self->compositor->monitors; l != NULL; l = l->next)
		floatdesk_dock_refresh(self, l->data);
}

/* ================================================================ */
/* Actions                                                           */
/* ================================================================ */

/*
 * find_item:
 * @self: the module
 * @key: an app key
 * @dock_out: (out) (optional): the dock it was found in
 *
 * Returns: (transfer none) (nullable): the item, from the selected
 *   output's dock first
 */
static DockItem *
find_item(
	GowlModuleFloatdesk *self,
	const gchar         *key,
	FloatdeskDock      **dock_out
){
	FloatdeskDock *dock;
	guint          i;

	dock = self->docks != NULL && self->compositor->selmon != NULL
		? g_hash_table_lookup(self->docks, self->compositor->selmon) : NULL;
	if (dock == NULL && self->compositor->selmon != NULL) {
		dock = dock_for(self, self->compositor->selmon);
		if (dock != NULL)
			build_items(dock);
	}
	if (dock == NULL)
		return NULL;
	if (dock_out != NULL)
		*dock_out = dock;
	for (i = 0; i < dock->items->len; i++) {
		DockItem *it = g_ptr_array_index(dock->items, i);

		if (g_strcmp0(it->key, key) == 0)
			return it;
	}
	return NULL;
}

gchar *
floatdesk_dock_launch(
	GowlModuleFloatdesk *self,
	const gchar         *key
){
	const GowlFloatdeskApp *app;
	g_auto(GStrv)           argv = NULL;
	GError                 *error = NULL;

	if (key == NULL || *key == '\0')
		return g_strdup("ERROR floatdesk-dock-launch takes an app id");
	app = gowl_floatdesk_catalog_lookup(catalog(self), key);
	if (app == NULL)
		app = gowl_floatdesk_catalog_resolve(catalog(self), key);
	if (app == NULL || app->exec == NULL || *app->exec == '\0')
		return g_strdup_printf("ERROR no launchable application \"%s\"",
		                       key);

	if (app->terminal) {
		g_autofree gchar *id = g_strconcat(GOWL_FLOATDESK_TUI_PREFIX,
		                                   app->id, NULL);

		argv = gowl_floatdesk_terminal_argv(self->terminal, id, app->exec,
		                                    &error);
	} else if (!g_shell_parse_argv(app->exec, NULL, &argv, &error)) {
		argv = NULL;
	}
	if (argv == NULL || !g_spawn_async(NULL, argv, NULL,
	                                    G_SPAWN_SEARCH_PATH, NULL, NULL,
	                                    NULL, &error)) {
		g_autofree gchar *msg = g_strdup_printf("Could not start %s: %s",
			app->name, error != NULL ? error->message : "?");

		g_warning("floatdesk: %s", msg);
		floatdesk_toast(self, NULL, msg);
		g_clear_error(&error);
		return g_strdup_printf("ERROR %s", msg);
	}

	/* Pulse until its first window maps (floatdesk_dock_client_mapped)
	 * or the deadline passes. */
	if (self->launching == NULL)
		self->launching = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                        g_free, NULL);
	g_hash_table_replace(self->launching, g_strdup(app->id),
		GSIZE_TO_POINTER((gsize)(g_get_monotonic_time() + DOCK_LAUNCH_US)));
	floatdesk_dock_refresh_all(self);
	return g_strdup_printf("OK launched %s", app->id);
}

void
floatdesk_dock_client_mapped(
	GowlModuleFloatdesk *self,
	GowlClient          *c
){
	g_autofree gchar *key = NULL;

	if (self->launching == NULL || g_hash_table_size(self->launching) == 0)
		return;
	key = gowl_floatdesk_catalog_key(catalog(self), c->app_id);
	g_hash_table_remove(self->launching, key);
}

gchar *
floatdesk_dock_activate(
	GowlModuleFloatdesk *self,
	const gchar         *key
){
	DockItem   *it;
	GowlClient *focused, *target;
	gint        index = -1, next;
	guint       i;

	if (key == NULL || *key == '\0')
		return g_strdup("ERROR floatdesk-dock-activate takes an app id");
	it = find_item(self, key, NULL);
	if (it == NULL || it->clients->len == 0)
		return floatdesk_dock_launch(self, key);

	focused = self->compositor->selmon != NULL
		? focustop_public(self->compositor, self->compositor->selmon)
		: NULL;
	for (i = 0; i < it->clients->len; i++)
		if (g_ptr_array_index(it->clients, i) == focused)
			index = (gint)i;
	next = gowl_floatdesk_cycle_next(it->clients->len, index);
	target = g_ptr_array_index(it->clients, next);

	if (target->isminimized)
		gowl_compositor_unminimize_client(self->compositor, target,
		                                  NULL, 0);
	else
		gowl_compositor_show_client(self->compositor, target);
	if (target->scene != NULL)
		wlr_scene_node_raise_to_top(&target->scene->node);
	return g_strdup_printf("OK focused %u", target->id);
}

gboolean
floatdesk_dock_pin(
	GowlModuleFloatdesk *self,
	const gchar         *key,
	gint                 index
){
	g_auto(GStrv) pins = floatdesk_dock_pins(self);
	GPtrArray    *out;
	guint         i, n = g_strv_length(pins);
	gint          at = -1;

	if (key == NULL || *key == '\0')
		return FALSE;
	for (i = 0; i < n; i++)
		if (g_strcmp0(pins[i], key) == 0)
			at = (gint)i;
	/* Already there and no new place asked for: nothing to do. */
	if (at >= 0 && index < 0)
		return FALSE;

	out = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < n; i++)
		if ((gint)i != at)
			g_ptr_array_add(out, g_strdup(pins[i]));
	if (index < 0 || index > (gint)out->len)
		index = (gint)out->len;
	g_ptr_array_insert(out, index, g_strdup(key));
	g_ptr_array_add(out, NULL);
	gowl_floatdesk_state_set_dock_pins(self->state,
	                                   (const gchar *const *)out->pdata);
	g_ptr_array_unref(out);
	floatdesk_dock_refresh_all(self);
	return TRUE;
}

gboolean
floatdesk_dock_unpin(
	GowlModuleFloatdesk *self,
	const gchar         *key
){
	g_auto(GStrv) pins = floatdesk_dock_pins(self);
	GPtrArray    *out;
	gboolean      found = FALSE;
	guint         i;

	out = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; pins[i] != NULL; i++) {
		if (g_strcmp0(pins[i], key) == 0) {
			found = TRUE;
			continue;
		}
		g_ptr_array_add(out, g_strdup(pins[i]));
	}
	if (found) {
		g_ptr_array_add(out, NULL);
		gowl_floatdesk_state_set_dock_pins(self->state,
		                                   (const gchar *const *)out->pdata);
	}
	g_ptr_array_unref(out);
	if (found)
		floatdesk_dock_refresh_all(self);
	return found;
}

void
floatdesk_dock_toggle_grid(GowlModuleFloatdesk *self)
{
	FloatdeskDock *dock;

	if (self->compositor == NULL || self->compositor->selmon == NULL)
		return;
	dock = dock_for(self, self->compositor->selmon);
	if (dock == NULL)
		return;
	if (dock->grid != NULL)
		grid_close(dock);
	else
		grid_open(dock);
	floatdesk_dock_refresh(self, self->compositor->selmon);
}

gchar *
floatdesk_dock_status(GowlModuleFloatdesk *self)
{
	FloatdeskDock *dock = NULL;
	GString       *items;
	guint          i;

	if (self->compositor->selmon != NULL) {
		dock = dock_for(self, self->compositor->selmon);
		if (dock != NULL)
			build_items(dock);
	}
	items = g_string_new(NULL);
	for (i = 0; dock != NULL && i < dock->items->len; i++) {
		DockItem *it = g_ptr_array_index(dock->items, i);

		if (items->len > 0)
			g_string_append_c(items, ';');
		/* key:pinned:windows:minimized:launching */
		g_string_append_printf(items, "%s:%d:%u:%u:%d", it->key,
		                       it->pinned ? 1 : 0, it->clients->len,
		                       it->minimized, it->launching ? 1 : 0);
	}
	return g_strdup_printf("OK position=%s autohide=%d shown=%d items=%s",
	                       floatdesk_dock_position(self),
	                       floatdesk_dock_autohide(self) ? 1 : 0,
	                       dock != NULL && dock->visible ? 1 : 0,
	                       (gchar *)g_string_free(items, FALSE));
}

/* ================================================================ */
/* Input                                                             */
/* ================================================================ */

/*
 * dock_under:
 * @self: the module
 * @lx: the pointer
 * @ly: the pointer
 *
 * Returns: (transfer none) (nullable): the dock of the output under the
 *   pointer
 */
static FloatdeskDock *
dock_under(
	GowlModuleFloatdesk *self,
	gdouble              lx,
	gdouble              ly
){
	GHashTableIter iter;
	gpointer       value;

	if (self->docks == NULL)
		return NULL;
	g_hash_table_iter_init(&iter, self->docks);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		FloatdeskDock *dock = value;

		if (wlr_box_contains_point(&dock->mon->m, lx, ly))
			return dock;
	}
	return NULL;
}

/*
 * dock_with_popup:
 * @self: the module
 *
 * Returns: (transfer none) (nullable): the dock with a menu, the grid or
 *   a press open -- wherever the pointer has gone since
 */
static FloatdeskDock *
dock_with_popup(GowlModuleFloatdesk *self)
{
	GHashTableIter iter;
	gpointer       value;

	if (self->docks == NULL)
		return NULL;
	g_hash_table_iter_init(&iter, self->docks);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		FloatdeskDock *dock = value;

		if (dock->menu != NULL || dock->grid != NULL
		    || dock->press != PRESS_NONE)
			return dock;
	}
	return NULL;
}

static bool
drag_passthrough(
	struct wlr_scene_buffer *buffer,
	double                  *sx,
	double                  *sy
){
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

/*
 * show_drag_icon:
 * @dock: the dock
 * @icon: (nullable): the icon name
 * @label: (nullable): the app name
 * @lx: the pointer
 * @ly: the pointer
 */
static void
show_drag_icon(
	FloatdeskDock *dock,
	const gchar   *icon,
	const gchar   *label,
	gdouble        lx,
	gdouble        ly
){
	GowlModuleFloatdesk *self = dock->self;
	gint                 size = self->dock_icon_size;

	if (dock->drag_icon == NULL) {
		cairo_surface_t *surface;
		cairo_t         *cr;
		gdouble          scale = output_scale(dock->mon);

		dock->drag_icon = wlr_scene_buffer_create(
			self->compositor->layers[GOWL_SCENE_LAYER_OVERLAY], NULL);
		if (dock->drag_icon == NULL)
			return;
		surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
			(gint)ceil(size * scale), (gint)ceil(size * scale));
		cr = cairo_create(surface);
		cairo_scale(cr, scale, scale);
		draw_icon(self, cr, icon, label, 0, 0, size, scale, 0.85);
		cairo_destroy(cr);
		push_buffer(dock->drag_icon, surface, size, size);
		cairo_surface_destroy(surface);
		/* The pointer sees through it, to the dock under it. */
		dock->drag_icon->point_accepts_input = drag_passthrough;
	}
	wlr_scene_node_set_position(&dock->drag_icon->node,
	                            (gint)lx - size / 2, (gint)ly - size / 2);
	wlr_scene_node_raise_to_top(&dock->drag_icon->node);
}

gboolean
floatdesk_dock_motion(
	GowlModuleFloatdesk *self,
	gdouble              lx,
	gdouble              ly
){
	FloatdeskDock *popup = dock_with_popup(self);
	FloatdeskDock *dock;
	GHashTableIter iter;
	gpointer       value;

	/* Every dock: whether the pointer is at its edge or over it, for
	 * auto-hide, and which item it is over. */
	if (self->docks != NULL) {
		g_hash_table_iter_init(&iter, self->docks);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			FloatdeskDock *d = value;
			const gchar   *pos = floatdesk_dock_position(self);
			struct wlr_box box, *m = &d->mon->m;
			gboolean       near = FALSE;
			gint           hover;

			if (!d->visible)
				continue;
			if (wlr_box_contains_point(m, lx, ly)) {
				if (g_strcmp0(pos, "bottom") == 0)
					near = ly >= m->y + m->height - DOCK_EDGE_HIT;
				else if (g_strcmp0(pos, "top") == 0)
					near = ly < m->y + DOCK_EDGE_HIT;
				else if (g_strcmp0(pos, "left") == 0)
					near = lx < m->x + DOCK_EDGE_HIT;
				else
					near = lx >= m->x + m->width - DOCK_EDGE_HIT;
				current_box(d, &box);
				/* Once out, keep it out while the pointer is on it. */
				if (d->reveal > 0.5 && wlr_box_contains_point(&box, lx, ly))
					near = TRUE;
			}
			d->pointer_near = near;
			if (d->hiding)
				set_want_reveal(d, dock_has_attention(d));
			hover = index_at(d, lx, ly);
			if (hover != d->hover) {
				d->hover = hover;
				dock_render(d);
			}
		}
	}

	if (popup == NULL || popup->press == PRESS_NONE)
		return FALSE;

	/* A press under way: past the threshold it is a drag. */
	dock = popup;
	if (!dock->dragging
	    && (fabs(lx - dock->press_x) > DOCK_DRAG_START
	        || fabs(ly - dock->press_y) > DOCK_DRAG_START)) {
		if (dock->press == PRESS_ITEM && dock->press_index >= 0
		    && dock->press_index < (gint)dock->items->len) {
			DockItem *it = g_ptr_array_index(dock->items,
			                                 dock->press_index);

			dock->dragging = TRUE;
			dock->drag_key = g_strdup(it->key);
			show_drag_icon(dock, it->icon, it->label, lx, ly);
		} else if (dock->press == PRESS_GRID_APP) {
			const GowlFloatdeskApp *app;

			app = gowl_floatdesk_catalog_lookup(catalog(self),
			                                    dock->drag_key);
			dock->dragging = TRUE;
			/* Out of the way, so the dock under it can be reached. */
			grid_close(dock);
			show_drag_icon(dock, app != NULL ? app->icon : NULL,
			               app != NULL ? app->name : dock->drag_key,
			               lx, ly);
		}
	}
	if (dock->dragging) {
		gint drop = -1;

		if (dock->drag_icon != NULL)
			wlr_scene_node_set_position(&dock->drag_icon->node,
				(gint)lx - self->dock_icon_size / 2,
				(gint)ly - self->dock_icon_size / 2);
		if (dock_under(self, lx, ly) == dock && index_at(dock, lx, ly) >= 0)
			drop = drop_index_at(dock, lx, ly);
		if (drop != dock->drop_index) {
			dock->drop_index = drop;
			dock_render(dock);
		}
		return TRUE;
	}
	return FALSE;
}

/*
 * finish_drag:
 * @dock: the dock
 *
 * A drop on the dock pins the dragged app at the insertion point --
 * moving a pin, or pinning a running app or one from the grid.  A drop
 * anywhere else does nothing.
 */
static void
finish_drag(FloatdeskDock *dock)
{
	GowlModuleFloatdesk *self = dock->self;
	g_autofree gchar    *key = g_strdup(dock->drag_key);
	gint                 drop = dock->drop_index;

	press_reset(dock);
	if (key != NULL && drop >= 0) {
		/* Moving a pin later in the list: the slot it leaves shifts
		 * every later index down by one. */
		g_auto(GStrv) pins = floatdesk_dock_pins(self);
		guint         i;

		for (i = 0; pins[i] != NULL; i++)
			if (g_strcmp0(pins[i], key) == 0 && (gint)i < drop)
				drop--;
		floatdesk_dock_pin(self, key, drop);
		floatdesk_save(self);
	}
	floatdesk_dock_refresh_all(self);
}

gboolean
floatdesk_dock_button(
	GowlModuleFloatdesk *self,
	guint                button,
	gboolean             pressed
){
	FloatdeskDock *dock;
	gdouble        lx, ly;
	gint           index;

	if (self->compositor->wlr_cursor == NULL)
		return FALSE;
	lx = self->compositor->wlr_cursor->x;
	ly = self->compositor->wlr_cursor->y;

	/* A press already held belongs to the dock it started on. */
	dock = dock_with_popup(self);
	if (dock != NULL && dock->press != PRESS_NONE && !pressed) {
		PressKind kind = dock->press;
		gint      pidx = dock->press_index;

		if (dock->dragging) {
			finish_drag(dock);
			return TRUE;
		}
		if (kind == PRESS_ITEM) {
			index = index_at(dock, lx, ly);
			if (index == pidx && pidx >= 0
			    && pidx < (gint)dock->items->len) {
				DockItem         *it = g_ptr_array_index(dock->items, pidx);
				g_autofree gchar *key = g_strdup(it->key);
				g_autofree gchar *reply = NULL;

				press_reset(dock);
				if (button == BTN_MIDDLE)
					reply = floatdesk_dock_launch(self, key);
				else
					reply = floatdesk_dock_activate(self, key);
				floatdesk_dock_refresh_all(self);
				return TRUE;
			}
		} else if (kind == PRESS_GRID_BUTTON) {
			press_reset(dock);
			if (index_at(dock, lx, ly) == (gint)dock->items->len) {
				if (dock->grid != NULL)
					grid_close(dock);
				else
					grid_open(dock);
			}
			floatdesk_dock_refresh(self, dock->mon);
			return TRUE;
		} else if (kind == PRESS_GRID_APP) {
			g_autofree gchar *key = g_strdup(dock->drag_key);
			g_autofree gchar *reply = NULL;

			press_reset(dock);
			grid_close(dock);
			reply = floatdesk_dock_launch(self, key);
			floatdesk_dock_refresh(self, dock->mon);
			return TRUE;
		} else if (kind == PRESS_MENU) {
			gint row;

			press_reset(dock);
			row = ((gint)ly - dock->menu_rect.y - 6) / MENU_ROW;
			if (dock->menu != NULL
			    && wlr_box_contains_point(&dock->menu_rect, lx, ly))
				menu_choose(dock, row);
			return TRUE;
		}
		press_reset(dock);
		floatdesk_dock_refresh(self, dock->mon);
		return TRUE;
	}
	if (!pressed)
		return FALSE;

	/* An open menu: a press in it is a choice, anywhere else closes it
	 * and goes on to whatever was clicked. */
	dock = dock_with_popup(self);
	if (dock != NULL && dock->menu != NULL) {
		if (wlr_box_contains_point(&dock->menu_rect, lx, ly)) {
			dock->press = PRESS_MENU;
			dock->press_x = lx;
			dock->press_y = ly;
			return TRUE;
		}
		menu_close(dock);
		floatdesk_dock_refresh(self, dock->mon);
	}

	/* The grid: a press on an app may become a click (launch) or a drag
	 * (pin); a press outside the grid and the dock closes it. */
	dock = dock_with_popup(self);
	if (dock != NULL && dock->grid != NULL) {
		const GowlFloatdeskApp *app = grid_app_at(dock, lx, ly);

		if (wlr_box_contains_point(&dock->grid_rect, lx, ly)) {
			if (app != NULL && button == BTN_LEFT) {
				dock->press = PRESS_GRID_APP;
				dock->press_x = lx;
				dock->press_y = ly;
				g_free(dock->drag_key);
				dock->drag_key = g_strdup(app->id);
			} else if (app != NULL && button == BTN_RIGHT) {
				floatdesk_dock_pin(self, app->id, -1);
				floatdesk_save(self);
			}
			return TRUE;
		}
		if (index_at(dock, lx, ly) < 0) {
			grid_close(dock);
			floatdesk_dock_refresh(self, dock->mon);
			return TRUE;
		}
	}

	dock = dock_under(self, lx, ly);
	if (dock == NULL)
		return FALSE;
	index = index_at(dock, lx, ly);
	if (index < 0)
		return FALSE;

	if (index == (gint)dock->items->len) {
		if (button == BTN_RIGHT) {
			menu_open(dock, NULL, lx, ly);
		} else if (button == BTN_LEFT) {
			dock->press = PRESS_GRID_BUTTON;
			dock->press_index = index;
			dock->press_x = lx;
			dock->press_y = ly;
		}
		return TRUE;
	}
	if (button == BTN_RIGHT) {
		DockItem *it = g_ptr_array_index(dock->items, index);

		menu_open(dock, it->key, lx, ly);
		return TRUE;
	}
	dock->press = PRESS_ITEM;
	dock->press_index = index;
	dock->press_x = lx;
	dock->press_y = ly;
	return TRUE;
}

gboolean
floatdesk_dock_axis(
	GowlModuleFloatdesk *self,
	gdouble              delta
){
	FloatdeskDock *dock = dock_with_popup(self);
	gint           rows, total;

	if (dock == NULL || dock->grid == NULL || dock->grid_apps == NULL
	    || self->compositor->wlr_cursor == NULL
	    || !wlr_box_contains_point(&dock->grid_rect,
	                               self->compositor->wlr_cursor->x,
	                               self->compositor->wlr_cursor->y))
		return FALSE;
	rows = grid_rows_visible(dock);
	total = ((gint)dock->grid_apps->len + dock->grid_cols - 1)
	        / dock->grid_cols;
	dock->grid_scroll = CLAMP(dock->grid_scroll + (delta > 0 ? 1 : -1), 0,
	                          MAX(0, total - rows));
	grid_render(dock);
	return TRUE;
}

gboolean
floatdesk_dock_key(
	GowlModuleFloatdesk *self,
	guint                keysym,
	gboolean             pressed
){
	FloatdeskDock *dock = dock_with_popup(self);

	if (dock == NULL || keysym != XKB_KEY_Escape)
		return FALSE;
	/* Swallow the release too, or the window under the grid gets a
	 * lone Escape release. */
	if (pressed) {
		press_reset(dock);
		menu_close(dock);
		grid_close(dock);
		floatdesk_dock_refresh(self, dock->mon);
	}
	return TRUE;
}
