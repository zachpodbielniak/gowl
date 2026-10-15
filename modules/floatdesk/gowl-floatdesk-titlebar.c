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
 * gowl-floatdesk-titlebar.c - title bars on a floating desktop.
 *
 * A floating window on a floating workspace gets a bar across the top
 * of its frame: the title, and minimize / maximize / close on the
 * right.  Dragging the bar moves the window; dropping it at the left or
 * right edge of the output snaps it to that half, at the top edge
 * maximizes it; double-clicking toggles maximize.  The window's borders
 * resize it.
 *
 * The bar is INSIDE the frame: the compositor keeps
 * GowlClient.deco_top pixels above the content for it (set by the
 * module's pre-arrange), so the blur behind a window, its shadow, its
 * rounded corners and its animations all include the bar without
 * knowing it exists.  The bar itself is a scene buffer, a child of the
 * window's scene tree, drawn with cairo and pango in the session
 * palette's colours, redrawn only when what it shows changes.
 *
 * A window that draws its own title bar (client-side decorations: an
 * xdg_toplevel that never negotiated xdg-decoration) gets none -- two
 * title bars stacked is worse than one.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-floatdesk"

#include <math.h>
#include <string.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>

#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-frame-sink.h"
#include "module/gowl-module-manager.h"
#include "interfaces/gowl-client-decorator.h"

#include "gowl-floatdesk-private.h"

/* Double-click window, in microseconds. */
#define TITLEBAR_DOUBLE_CLICK_US (400 * 1000)

/* How far outside the frame a press still grabs its edge. */
#define TITLEBAR_EDGE_OUTSIDE (6)

/* The three buttons, right to left. */
typedef enum {
	TB_HIT_NONE = 0,
	TB_HIT_TITLE,
	TB_HIT_MINIMIZE,
	TB_HIT_MAXIMIZE,
	TB_HIT_CLOSE
} TitlebarHit;

struct _FloatdeskTitlebar {
	GowlModuleFloatdesk     *self;
	GowlClient              *client;
	struct wlr_scene_buffer *buffer;
	struct wl_listener       node_destroy;

	/* What the current buffer shows, to redraw only on a change. */
	gint     width;
	gint     height;
	gdouble  scale;
	gboolean focused;
	gboolean maximized;
	gchar   *title;
};

/* ---------------------------------------------------------------- */

/*
 * titlebar_table:
 * @self: the module
 *
 * Returns: (transfer none): client -> title bar, created on first use
 */
static GHashTable *
titlebar_table(GowlModuleFloatdesk *self)
{
	if (self->titlebar_nodes == NULL)
		self->titlebar_nodes = g_hash_table_new(g_direct_hash,
		                                        g_direct_equal);
	return self->titlebar_nodes;
}

static void
titlebar_free(FloatdeskTitlebar *tb)
{
	wl_list_remove(&tb->node_destroy.link);
	g_free(tb->title);
	g_free(tb);
}

/*
 * on_node_destroy:
 *
 * The bar's scene node went away with its window's scene tree (unmap):
 * forget it without touching the node again.
 */
static void
on_node_destroy(
	struct wl_listener *listener,
	void               *data
){
	FloatdeskTitlebar *tb = wl_container_of(listener, tb, node_destroy);

	(void)data;
	if (tb->self->titlebar_nodes != NULL)
		g_hash_table_remove(tb->self->titlebar_nodes, tb->client);
	titlebar_free(tb);
}

gboolean
floatdesk_titlebar_wanted(
	GowlModuleFloatdesk *self,
	GowlClient          *c
){
	if (!self->titlebars || c->scene == NULL || !c->isfloating
	    || c->isfullscreen || c->isoverlay || c->isunmanaged
	    || c->isembedded || c->isminimized)
		return FALSE;
	/* Client-side decorations: the window has a title bar already. */
	if (c->xdg_toplevel != NULL && c->decoration == NULL)
		return FALSE;
	return floatdesk_client_on_floating(self, c);
}

void
floatdesk_titlebar_drop(
	GowlModuleFloatdesk *self,
	GowlClient          *c
){
	FloatdeskTitlebar *tb;

	if (self->titlebar_nodes == NULL || c == NULL)
		return;
	tb = g_hash_table_lookup(self->titlebar_nodes, c);
	if (tb == NULL)
		return;
	g_hash_table_remove(self->titlebar_nodes, c);
	/* Taking the listener off first: destroying the node fires it. */
	wl_list_remove(&tb->node_destroy.link);
	wl_list_init(&tb->node_destroy.link);
	wlr_scene_node_destroy(&tb->buffer->node);
	titlebar_free(tb);
}

void
floatdesk_titlebar_drop_all(GowlModuleFloatdesk *self)
{
	GList *clients, *l;

	if (self->titlebar_nodes == NULL)
		return;
	clients = g_hash_table_get_keys(self->titlebar_nodes);
	for (l = clients; l != NULL; l = l->next)
		floatdesk_titlebar_drop(self, l->data);
	g_list_free(clients);
}

/*
 * corner_radius:
 * @self: the module
 *
 * Returns: the decorator's corner radius (roundcorners), 0 without one --
 *   the bar's top corners follow the frame's
 */
static gint
corner_radius(GowlModuleFloatdesk *self)
{
	GowlClientDecorator *dec;

	if (self->compositor == NULL || self->compositor->module_mgr == NULL)
		return 0;
	dec = (GowlClientDecorator *)gowl_module_manager_get_decorator(
		self->compositor->module_mgr);
	return dec != NULL ? MAX(0, gowl_client_decorator_get_corner_radius(dec))
	                   : 0;
}

/*
 * is_focused:
 * @self: the module
 * @c: a window
 *
 * Returns: %TRUE if @c has the keyboard
 */
static gboolean
is_focused(
	GowlModuleFloatdesk *self,
	GowlClient          *c
){
	return self->compositor->selmon != NULL
	       && focustop_public(self->compositor, self->compositor->selmon) == c;
}

/*
 * draw_button:
 * @cr: cairo, in logical units
 * @theme: the session theme
 * @role: the button's colour
 * @cx: centre
 * @cy: centre
 * @r: radius
 * @hit: which button, for its glyph
 * @focused: an unfocused window's buttons are muted
 */
static void
draw_button(
	cairo_t            *cr,
	const GowlBarTheme *theme,
	GowlBarColor        role,
	gdouble             cx,
	gdouble             cy,
	gdouble             r,
	TitlebarHit         hit,
	gboolean            focused
){
	gdouble g = r * 0.45;

	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
	gowl_bar_theme_cairo_set_alpha(theme, cr,
		focused ? role : GOWL_BAR_COLOR_OVERLAY, focused ? 1.0 : 0.6);
	cairo_fill(cr);

	gowl_bar_theme_cairo_set_alpha(theme, cr, GOWL_BAR_COLOR_CRUST, 0.85);
	cairo_set_line_width(cr, MAX(1.0, r * 0.18));
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	switch (hit) {
	case TB_HIT_CLOSE:
		cairo_move_to(cr, cx - g, cy - g);
		cairo_line_to(cr, cx + g, cy + g);
		cairo_move_to(cr, cx + g, cy - g);
		cairo_line_to(cr, cx - g, cy + g);
		break;
	case TB_HIT_MAXIMIZE:
		cairo_rectangle(cr, cx - g, cy - g, 2 * g, 2 * g);
		break;
	case TB_HIT_MINIMIZE:
		cairo_move_to(cr, cx - g, cy);
		cairo_line_to(cr, cx + g, cy);
		break;
	case TB_HIT_NONE:
	case TB_HIT_TITLE:
	default:
		break;
	}
	cairo_stroke(cr);
}

/*
 * titlebar_render:
 * @self: the module
 * @tb: the bar
 *
 * Draws the bar at its current size into a fresh buffer.
 */
static void
titlebar_render(
	GowlModuleFloatdesk *self,
	FloatdeskTitlebar   *tb
){
	cairo_surface_t      *surface;
	cairo_t              *cr;
	PangoLayout          *layout;
	PangoFontDescription *font;
	struct wlr_buffer    *buffer;
	const GowlBarTheme   *theme = self->theme;
	gint                  pw, ph, radius, text_w, text_h;
	gdouble               w = tb->width, h = tb->height, r, step;

	if (theme == NULL || tb->width <= 0 || tb->height <= 0)
		return;
	pw = (gint)ceil(w * tb->scale);
	ph = (gint)ceil(h * tb->scale);
	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return;
	}
	cr = cairo_create(surface);
	cairo_scale(cr, tb->scale, tb->scale);

	/* The background, its top corners following a rounded frame's. */
	radius = MAX(0, corner_radius(self) - (gint)tb->client->bw);
	cairo_new_path(cr);
	if (radius > 0) {
		cairo_move_to(cr, 0, h);
		cairo_line_to(cr, 0, radius);
		cairo_arc(cr, radius, radius, radius, G_PI, 1.5 * G_PI);
		cairo_line_to(cr, w - radius, 0);
		cairo_arc(cr, w - radius, radius, radius, 1.5 * G_PI, 2 * G_PI);
		cairo_line_to(cr, w, h);
		cairo_close_path(cr);
	} else {
		cairo_rectangle(cr, 0, 0, w, h);
	}
	gowl_bar_theme_cairo_set(theme, cr, tb->focused ? GOWL_BAR_COLOR_SURFACE
	                                                : GOWL_BAR_COLOR_MANTLE);
	cairo_fill(cr);

	/* A hairline separating the bar from the content. */
	gowl_bar_theme_cairo_set_alpha(theme, cr, GOWL_BAR_COLOR_CRUST, 0.6);
	cairo_rectangle(cr, 0, h - 1, w, 1);
	cairo_fill(cr);

	/* Buttons: close, maximize, minimize from the right, one bar
	 * height each. */
	r = MAX(4.0, h * 0.22);
	step = h;
	draw_button(cr, theme, GOWL_BAR_COLOR_RED, w - step * 0.5, h * 0.5, r,
	            TB_HIT_CLOSE, tb->focused);
	draw_button(cr, theme, GOWL_BAR_COLOR_GREEN, w - step * 1.5, h * 0.5, r,
	            TB_HIT_MAXIMIZE, tb->focused);
	draw_button(cr, theme, GOWL_BAR_COLOR_YELLOW, w - step * 2.5, h * 0.5, r,
	            TB_HIT_MINIMIZE, tb->focused);

	/* The title, ellipsized into what the buttons leave. */
	layout = pango_cairo_create_layout(cr);
	font = pango_font_description_from_string(
		gowl_bar_theme_get_font(theme) != NULL
		? gowl_bar_theme_get_font(theme) : "sans 10");
	pango_layout_set_font_description(layout, font);
	pango_font_description_free(font);
	pango_layout_set_text(layout, tb->title != NULL ? tb->title : "", -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_width(layout,
		MAX(1, (gint)((w - step * 3 - 16) * PANGO_SCALE)));
	pango_layout_get_pixel_size(layout, &text_w, &text_h);
	gowl_bar_theme_cairo_set(theme, cr, tb->focused ? GOWL_BAR_COLOR_TEXT
	                                                : GOWL_BAR_COLOR_SUBTEXT);
	cairo_move_to(cr, 12, (h - text_h) / 2.0);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);

	cairo_destroy(cr);
	cairo_surface_flush(surface);
	buffer = gowl_raw_buffer_create(cairo_image_surface_get_data(surface),
		cairo_image_surface_get_width(surface),
		cairo_image_surface_get_height(surface),
		cairo_image_surface_get_stride(surface));
	cairo_surface_destroy(surface);
	if (buffer == NULL)
		return;
	wlr_scene_buffer_set_buffer(tb->buffer, buffer);
	wlr_buffer_drop(buffer);
	wlr_scene_buffer_set_dest_size(tb->buffer, tb->width, tb->height);
}

void
floatdesk_titlebar_sync(
	GowlModuleFloatdesk *self,
	GowlClient          *c
){
	FloatdeskTitlebar *tb;
	gint               width, height;
	gdouble            scale = 1.0;
	gboolean           focused, maximized, redraw;

	if (c == NULL || c->scene == NULL || c->deco_top <= 0
	    || c->isfullscreen || self->compositor == NULL) {
		floatdesk_titlebar_drop(self, c);
		return;
	}

	width = (c->frame.width > 0 ? c->frame.width : c->geom.width)
		- 2 * (gint)c->bw;
	height = c->deco_top;
	if (width <= 0)
		return;
	if (c->mon != NULL && c->mon->wlr_output != NULL)
		scale = MAX(1.0, c->mon->wlr_output->scale);
	focused = is_focused(self, c);
	maximized = floatdesk_is_maximized(c);

	tb = g_hash_table_lookup(titlebar_table(self), c);
	if (tb == NULL) {
		tb = g_new0(FloatdeskTitlebar, 1);
		tb->self = self;
		tb->client = c;
		tb->buffer = wlr_scene_buffer_create(c->scene, NULL);
		if (tb->buffer == NULL) {
			g_free(tb);
			return;
		}
		tb->node_destroy.notify = on_node_destroy;
		wl_signal_add(&tb->buffer->node.events.destroy,
		              &tb->node_destroy);
		g_hash_table_insert(titlebar_table(self), c, tb);
		redraw = TRUE;
	} else {
		redraw = tb->width != width || tb->height != height
		         || tb->scale != scale || tb->focused != focused
		         || tb->maximized != maximized
		         || g_strcmp0(tb->title, c->title) != 0;
	}

	wlr_scene_node_set_position(&tb->buffer->node, (gint)c->bw, (gint)c->bw);
	/* Above the content and any decorator frame drawn into the tree. */
	wlr_scene_node_raise_to_top(&tb->buffer->node);
	if (!redraw)
		return;

	tb->width = width;
	tb->height = height;
	tb->scale = scale;
	tb->focused = focused;
	tb->maximized = maximized;
	g_free(tb->title);
	tb->title = g_strdup(c->title);
	titlebar_render(self, tb);
}

/* ---------------------------------------------------------------- */
/* Input                                                             */
/* ---------------------------------------------------------------- */

/*
 * titlebar_at:
 * @self: the module
 * @lx: the pointer, layout coordinates
 * @ly: the pointer
 * @local_x: (out): the pointer across the bar
 * @local_y: (out): the pointer down the bar
 *
 * Asks the scene graph, so a bar covered by another window is not hit.
 *
 * Returns: (transfer none) (nullable): the bar under the pointer
 */
static FloatdeskTitlebar *
titlebar_at(
	GowlModuleFloatdesk *self,
	gdouble              lx,
	gdouble              ly,
	gdouble             *local_x,
	gdouble             *local_y
){
	struct wlr_scene_node *node;
	GHashTableIter         iter;
	gpointer               value;

	if (self->titlebar_nodes == NULL
	    || g_hash_table_size(self->titlebar_nodes) == 0)
		return NULL;
	node = wlr_scene_node_at(&self->compositor->scene->tree.node, lx, ly,
	                         local_x, local_y);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER)
		return NULL;
	g_hash_table_iter_init(&iter, self->titlebar_nodes);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		FloatdeskTitlebar *tb = value;

		if (&tb->buffer->node == node)
			return tb;
	}
	return NULL;
}

/*
 * hit_of:
 * @tb: a bar
 * @x: the pointer across it
 *
 * Returns: which part of the bar @x is in
 */
static TitlebarHit
hit_of(
	FloatdeskTitlebar *tb,
	gdouble            x
){
	gdouble step = tb->height;

	if (x >= tb->width - step)
		return TB_HIT_CLOSE;
	if (x >= tb->width - 2 * step)
		return TB_HIT_MAXIMIZE;
	if (x >= tb->width - 3 * step)
		return TB_HIT_MINIMIZE;
	return TB_HIT_TITLE;
}

/*
 * begin_title_drag:
 * @self: the module
 * @c: the window
 *
 * A maximized or snapped window dragged by its bar first goes back to
 * its own size, under the pointer at the same fraction across, so the
 * window comes away from the edge rather than the edge moving.
 */
static void
begin_title_drag(
	GowlModuleFloatdesk *self,
	GowlClient          *c
){
	FloatdeskRestore *r;
	gdouble           cx = self->compositor->wlr_cursor->x;
	gdouble           cy = self->compositor->wlr_cursor->y;

	r = g_object_get_data(G_OBJECT(c), FLOATDESK_RESTORE_KEY);
	if (r != NULL && c->geom.width > 0) {
		struct wlr_box old = c->geom;
		struct wlr_box box = r->box;
		gdouble        frac = (cx - old.x) / (gdouble)old.width;

		box.x = (gint)(cx - frac * box.width);
		box.y = (gint)(cy - MIN(cy - old.y, (gdouble)c->deco_top / 2.0
		                                   + c->bw));
		g_object_set_data(G_OBJECT(c), FLOATDESK_RESTORE_KEY, NULL);
		gowl_compositor_place_client(self->compositor, c, box.x, box.y,
		                             box.width, box.height);
		floatdesk_titlebar_sync(self, c);
	}
	gowl_compositor_begin_move(self->compositor, c);
}

/*
 * client_of_node:
 * @self: the module
 * @node: a scene node
 *
 * Returns: (transfer none) (nullable): the window whose tree @node is
 */
static GowlClient *
client_of_node(
	GowlModuleFloatdesk   *self,
	struct wlr_scene_node *node
){
	GList *l;

	for (l = self->compositor->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if (c->scene != NULL && &c->scene->node == node)
			return c;
	}
	return NULL;
}

/*
 * edge_resize_at:
 * @self: the module
 * @lx: the pointer
 * @ly: the pointer
 *
 * A press on a floating window's border, or just outside it, starts a
 * resize from that edge or corner.  The topmost window wins: the FLOAT
 * layer is walked from its top child down, and a press INSIDE a window
 * stops the walk, since that press belongs to the window.
 *
 * Returns: %TRUE if a resize started
 */
static gboolean
edge_resize_at(
	GowlModuleFloatdesk *self,
	gdouble              lx,
	gdouble              ly
){
	struct wlr_scene_tree *layer;
	struct wlr_scene_node *node;

	layer = self->compositor->layers[GOWL_SCENE_LAYER_FLOAT];
	if (layer == NULL)
		return FALSE;
	wl_list_for_each_reverse(node, &layer->children, link) {
		GowlClient     *c;
		struct wlr_box  f;
		gint            out = TITLEBAR_EDGE_OUTSIDE;
		gint            in;
		guint32         edges = 0;

		/* Which window this node is, by asking the client list rather
		 * than trusting node->data: other things live in this layer
		 * (an effect's ghost, a module's sheet), and their data is not
		 * a GObject to type-check. */
		if (!node->enabled)
			continue;
		c = client_of_node(self, node);
		if (c == NULL || !c->isfloating || c->isfullscreen
		    || !floatdesk_client_on_floating(self, c))
			continue;
		f = c->frame.width > 0 ? c->frame : c->geom;
		in = MAX(2, (gint)c->bw);
		if (lx < f.x - out || lx >= f.x + f.width + out
		    || ly < f.y - out || ly >= f.y + f.height + out)
			continue;
		/* Inside the frame, clear of the border: the window's. */
		if (lx >= f.x + in && lx < f.x + f.width - in
		    && ly >= f.y + in && ly < f.y + f.height - in)
			return FALSE;
		if (lx < f.x + in)
			edges |= WLR_EDGE_LEFT;
		else if (lx >= f.x + f.width - in)
			edges |= WLR_EDGE_RIGHT;
		if (ly < f.y + in)
			edges |= WLR_EDGE_TOP;
		else if (ly >= f.y + f.height - in)
			edges |= WLR_EDGE_BOTTOM;
		if (edges == 0)
			return FALSE;
		gowl_compositor_focus_client(self->compositor, c, TRUE);
		g_object_set_data(G_OBJECT(c), FLOATDESK_RESTORE_KEY, NULL);
		return gowl_compositor_begin_resize(self->compositor, c, edges);
	}
	return FALSE;
}

gboolean
floatdesk_titlebar_button(
	GowlModuleFloatdesk *self,
	guint                button,
	gboolean             pressed
){
	FloatdeskTitlebar *tb;
	GowlClient        *c;
	gdouble            x = 0, y = 0;
	gdouble            lx, ly;
	gint64             now;
	g_autofree gchar  *reply = NULL;

	if (!pressed || self->compositor->wlr_cursor == NULL)
		return FALSE;
	lx = self->compositor->wlr_cursor->x;
	ly = self->compositor->wlr_cursor->y;

	tb = titlebar_at(self, lx, ly, &x, &y);
	if (tb == NULL) {
		if (button == BTN_LEFT)
			return edge_resize_at(self, lx, ly);
		return FALSE;
	}
	c = tb->client;
	if (button != BTN_LEFT) {
		/* Middle sends the window to the back; right is swallowed
		 * so it does not fall through to whatever is underneath. */
		if (button == BTN_MIDDLE)
			wlr_scene_node_lower_to_bottom(&c->scene->node);
		return TRUE;
	}

	switch (hit_of(tb, x)) {
	case TB_HIT_CLOSE:
		gowl_client_close(c);
		return TRUE;
	case TB_HIT_MAXIMIZE:
		gowl_compositor_focus_client(self->compositor, c, TRUE);
		floatdesk_maximize_toggle(self, c);
		return TRUE;
	case TB_HIT_MINIMIZE:
		reply = floatdesk_minimize(self, c);
		return TRUE;
	case TB_HIT_TITLE:
	case TB_HIT_NONE:
	default:
		break;
	}

	gowl_compositor_focus_client(self->compositor, c, TRUE);
	now = g_get_monotonic_time();
	if (self->last_click_client == c
	    && now - self->last_click_us < TITLEBAR_DOUBLE_CLICK_US) {
		self->last_click_client = NULL;
		floatdesk_maximize_toggle(self, c);
		return TRUE;
	}
	self->last_click_client = c;
	self->last_click_us = now;
	begin_title_drag(self, c);
	return TRUE;
}

/*
 * snap_idle_cb:
 *
 * Applies a drop-at-an-edge snap after the core has finished ending the
 * grab -- which re-assigns the window's output and would otherwise run
 * after the snap and undo it.
 */
static void
snap_idle_cb(void *data)
{
	GowlModuleFloatdesk *self = data;
	GowlClient          *c = self->snap_client;

	self->snap_idle = NULL;
	self->snap_client = NULL;
	if (c == NULL || self->compositor == NULL
	    || !floatdesk_client_on_floating(self, c))
		return;
	floatdesk_snap(self, c, self->snap_kind);
}

/*
 * monitor_at:
 * @self: the module
 * @lx: a layout point
 * @ly: a layout point
 *
 * Returns: (transfer none) (nullable): the output containing the point
 */
static GowlMonitor *
monitor_at(
	GowlModuleFloatdesk *self,
	gdouble              lx,
	gdouble              ly
){
	GList *l;

	for (l = self->compositor->monitors; l != NULL; l = l->next) {
		GowlMonitor *m = l->data;

		if (wlr_box_contains_point(&m->m, lx, ly))
			return m;
	}
	return NULL;
}

void
floatdesk_titlebar_release(
	GowlModuleFloatdesk *self,
	guint                button
){
	GowlCompositor   *comp = self->compositor;
	GowlClient       *c;
	GowlMonitor      *m;
	GowlFloatdeskSnap kind;

	(void)button;
	if (comp->cursor_mode != GOWL_CURSOR_MODE_MOVE
	    || comp->grabbed_client == NULL)
		return;
	c = comp->grabbed_client;
	if (!floatdesk_client_on_floating(self, c))
		return;

	m = monitor_at(self, comp->wlr_cursor->x, comp->wlr_cursor->y);
	kind = m != NULL
		? gowl_floatdesk_snap_at(m->m.x, m->m.y, m->m.width, m->m.height,
		                         comp->wlr_cursor->x, comp->wlr_cursor->y,
		                         self->snap_threshold)
		: GOWL_FLOATDESK_SNAP_NONE;
	if (kind == GOWL_FLOATDESK_SNAP_NONE) {
		/* Moved by hand: whatever size it has now is its own. */
		g_object_set_data(G_OBJECT(c), FLOATDESK_RESTORE_KEY, NULL);
		return;
	}
	self->snap_client = c;
	self->snap_kind = kind;
	self->snap_monitor = m;
	if (self->snap_idle == NULL)
		self->snap_idle = wl_event_loop_add_idle(
			gowl_compositor_get_event_loop(comp), snap_idle_cb, self);
}
