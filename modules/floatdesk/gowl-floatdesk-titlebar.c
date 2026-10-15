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

/* The three buttons, left to right: close, minimize, maximize. */
typedef enum {
	TB_HIT_NONE = 0,
	TB_HIT_TITLE,
	TB_HIT_MINIMIZE,
	TB_HIT_MAXIMIZE,
	TB_HIT_CLOSE
} TitlebarHit;

/* How many bar-heights the buttons take, from the left edge. */
#define TITLEBAR_BUTTONS (3)

/*
 * A title bar is five scene nodes in a tree of its own, so that a resize
 * moves and stretches nodes and redraws NOTHING:
 *
 *   left     the buttons and the top-left corner -- a buffer whose size
 *            does not depend on the window's width
 *   middle   a solid rect, stretched to fill
 *   right    the top-right corner, a buffer the corner radius wide
 *   line     the hairline under the bar, a rect
 *   title    the title text at its own size, centred on the bar
 *
 * Drawing the whole bar as one buffer meant a cairo + pango render and a
 * texture upload on every motion event of a resize, which is what made
 * resizing on a floating desktop lag.  Only a change of focus, title,
 * height or scale redraws now -- and the title, when a narrower window
 * has to ellipsize it, once the grab ends (until then it is cropped):
 * the core places the frame once more when a resize ends, and that is
 * the sync that lays the title out again.
 */
struct _FloatdeskTitlebar {
	GowlModuleFloatdesk     *self;
	GowlClient              *client;
	struct wlr_scene_tree   *tree;
	struct wlr_scene_buffer *left;
	struct wlr_scene_rect   *middle;
	struct wlr_scene_buffer *right;
	struct wlr_scene_rect   *line;
	struct wlr_scene_buffer *title;
	struct wl_listener       node_destroy;

	/* What the caps show, to redraw them only on a change. */
	gint     height;
	gdouble  scale;
	gboolean focused;
	gint     radius;
	gboolean caps_drawn;

	/* What the title buffer shows. */
	gchar   *title_text;
	gboolean title_focused;
	gint     title_height;
	gdouble  title_scale;
	gint     title_avail;      /* the width it was ellipsized to */
	gint     title_w;          /* its drawn size, logical */
	gint     title_h;
	gboolean title_ellipsized;
	gboolean title_drawn;

	/* The bar's current width, for hit testing. */
	gint     width;
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
	g_free(tb->title_text);
	g_free(tb);
}

/*
 * on_node_destroy:
 *
 * The bar's tree went away with its window's scene tree (unmap):
 * forget it without touching the nodes again.
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
	/* Taking the listener off first: destroying the tree fires it. */
	wl_list_remove(&tb->node_destroy.link);
	wl_list_init(&tb->node_destroy.link);
	wlr_scene_node_destroy(&tb->tree->node);
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
 * background_role:
 * @focused: the window has the keyboard
 *
 * Returns: the bar's colour role
 */
static GowlBarColor
background_role(gboolean focused)
{
	return focused ? GOWL_BAR_COLOR_SURFACE : GOWL_BAR_COLOR_MANTLE;
}

/*
 * role_to_rect:
 * @theme: the session theme
 * @role: a colour role
 * @alpha: extra opacity
 * @out: (out): premultiplied RGBA, the way a wlr_scene_rect wants it
 */
static void
role_to_rect(
	const GowlBarTheme *theme,
	GowlBarColor        role,
	gdouble             alpha,
	float               out[4]
){
	const gdouble *c = gowl_bar_theme_color(theme, role);
	gdouble        a = c[3] * alpha;

	out[0] = (float)(c[0] * a);
	out[1] = (float)(c[1] * a);
	out[2] = (float)(c[2] * a);
	out[3] = (float)a;
}

/*
 * new_surface:
 * @w: logical width
 * @h: logical height
 * @scale: the output scale
 * @cr_out: (out): a cairo context scaled to logical units
 *
 * Returns: (transfer full) (nullable): the image, or %NULL if cairo could
 *   not make one
 */
static cairo_surface_t *
new_surface(
	gint      w,
	gint      h,
	gdouble   scale,
	cairo_t **cr_out
){
	cairo_surface_t *surface;

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		MAX(1, (gint)ceil(w * scale)), MAX(1, (gint)ceil(h * scale)));
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	*cr_out = cairo_create(surface);
	cairo_scale(*cr_out, scale, scale);
	return surface;
}

/*
 * push:
 * @sb: the scene buffer
 * @surface: (transfer full): the finished image
 * @w: logical width
 * @h: logical height
 */
static void
push(
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
	cairo_surface_destroy(surface);
	if (buffer == NULL)
		return;
	wlr_scene_buffer_set_buffer(sb, buffer);
	wlr_buffer_drop(buffer);
	wlr_scene_buffer_set_dest_size(sb, w, h);
}

/*
 * render_caps:
 * @self: the module
 * @tb: the bar
 *
 * The left cap (top-left corner, then close, minimize, maximize, one bar
 * height each) and the right cap (the top-right corner).  Neither depends
 * on the window's width.
 */
static void
render_caps(
	GowlModuleFloatdesk *self,
	FloatdeskTitlebar   *tb
){
	const GowlBarTheme *theme = self->theme;
	cairo_surface_t    *surface;
	cairo_t            *cr = NULL;
	gdouble             h = tb->height;
	gdouble             r = MAX(4.0, h * 0.22);
	gint                lw = TITLEBAR_BUTTONS * tb->height;
	gint                rad = MIN(tb->radius, tb->height);

	/* Left: the corner, then the buttons. */
	surface = new_surface(lw, tb->height, tb->scale, &cr);
	if (surface != NULL) {
		cairo_new_path(cr);
		if (rad > 0) {
			cairo_move_to(cr, 0, h);
			cairo_line_to(cr, 0, rad);
			cairo_arc(cr, rad, rad, rad, G_PI, 1.5 * G_PI);
			cairo_line_to(cr, lw, 0);
			cairo_line_to(cr, lw, h);
			cairo_close_path(cr);
		} else {
			cairo_rectangle(cr, 0, 0, lw, h);
		}
		gowl_bar_theme_cairo_set(theme, cr, background_role(tb->focused));
		cairo_fill(cr);
		draw_button(cr, theme, GOWL_BAR_COLOR_RED, h * 0.5, h * 0.5, r,
		            TB_HIT_CLOSE, tb->focused);
		draw_button(cr, theme, GOWL_BAR_COLOR_YELLOW, h * 1.5, h * 0.5, r,
		            TB_HIT_MINIMIZE, tb->focused);
		draw_button(cr, theme, GOWL_BAR_COLOR_GREEN, h * 2.5, h * 0.5, r,
		            TB_HIT_MAXIMIZE, tb->focused);
		cairo_destroy(cr);
		push(tb->left, surface, lw, tb->height);
	}

	/* Right: the other top corner, or nothing at all. */
	wlr_scene_node_set_enabled(&tb->right->node, rad > 0);
	if (rad > 0) {
		surface = new_surface(rad, tb->height, tb->scale, &cr);
		if (surface != NULL) {
			cairo_new_path(cr);
			cairo_move_to(cr, 0, 0);
			cairo_arc(cr, 0, rad, rad, 1.5 * G_PI, 2 * G_PI);
			cairo_line_to(cr, rad, h);
			cairo_line_to(cr, 0, h);
			cairo_close_path(cr);
			gowl_bar_theme_cairo_set(theme, cr,
			                         background_role(tb->focused));
			cairo_fill(cr);
			cairo_destroy(cr);
			push(tb->right, surface, rad, tb->height);
		}
	}
}

/*
 * render_title:
 * @self: the module
 * @tb: the bar
 * @avail: the widest the title may be, ellipsized past it
 *
 * The title at its own size, so it is centred by moving the node.
 */
static void
render_title(
	GowlModuleFloatdesk *self,
	FloatdeskTitlebar   *tb,
	gint                 avail
){
	const GowlBarTheme   *theme = self->theme;
	cairo_surface_t      *probe, *surface;
	cairo_t              *cr = NULL;
	PangoLayout          *layout;
	PangoFontDescription *font;
	const gchar          *text = tb->title_text != NULL ? tb->title_text : "";
	gint                  tw = 0, th = 0;

	tb->title_avail = avail;
	tb->title_drawn = TRUE;
	if (*text == '\0' || avail <= 0) {
		tb->title_w = tb->title_h = 0;
		tb->title_ellipsized = FALSE;
		wlr_scene_node_set_enabled(&tb->title->node, FALSE);
		return;
	}

	/* Measure on a scratch context first, to size the image. */
	probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cr = cairo_create(probe);
	layout = pango_cairo_create_layout(cr);
	font = pango_font_description_from_string(
		gowl_bar_theme_get_font(theme) != NULL
		? gowl_bar_theme_get_font(theme) : "sans 10");
	pango_layout_set_font_description(layout, font);
	pango_font_description_free(font);
	pango_layout_set_text(layout, text, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_width(layout, avail * PANGO_SCALE);
	pango_layout_get_pixel_size(layout, &tw, &th);
	tb->title_ellipsized = pango_layout_is_ellipsized(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(probe);

	tw = MAX(1, MIN(tw, avail));
	th = MAX(1, MIN(th, tb->height));
	surface = new_surface(tw, th, tb->title_scale, &cr);
	if (surface == NULL) {
		g_object_unref(layout);
		return;
	}
	pango_cairo_update_layout(cr, layout);
	gowl_bar_theme_cairo_set(theme, cr, tb->title_focused
	                         ? GOWL_BAR_COLOR_TEXT : GOWL_BAR_COLOR_SUBTEXT);
	cairo_move_to(cr, 0, 0);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
	cairo_destroy(cr);
	tb->title_w = tw;
	tb->title_h = th;
	push(tb->title, surface, tw, th);
	wlr_scene_buffer_set_source_box(tb->title, NULL);
	wlr_scene_node_set_enabled(&tb->title->node, TRUE);
}

/*
 * titlebar_create:
 * @self: the module
 * @c: the window
 *
 * Returns: (transfer none) (nullable): a new bar in @c's scene tree
 */
static FloatdeskTitlebar *
titlebar_create(
	GowlModuleFloatdesk *self,
	GowlClient          *c
){
	FloatdeskTitlebar *tb;
	static const float none[4] = { 0, 0, 0, 0 };

	tb = g_new0(FloatdeskTitlebar, 1);
	tb->self = self;
	tb->client = c;
	tb->tree = wlr_scene_tree_create(c->scene);
	if (tb->tree == NULL) {
		g_free(tb);
		return NULL;
	}
	/* Bottom to top: the background pieces, the line, the title. */
	tb->left = wlr_scene_buffer_create(tb->tree, NULL);
	tb->middle = wlr_scene_rect_create(tb->tree, 0, 0, none);
	tb->right = wlr_scene_buffer_create(tb->tree, NULL);
	tb->line = wlr_scene_rect_create(tb->tree, 0, 1, none);
	tb->title = wlr_scene_buffer_create(tb->tree, NULL);
	if (tb->left == NULL || tb->middle == NULL || tb->right == NULL
	    || tb->line == NULL || tb->title == NULL) {
		wlr_scene_node_destroy(&tb->tree->node);
		g_free(tb);
		return NULL;
	}
	tb->node_destroy.notify = on_node_destroy;
	wl_signal_add(&tb->tree->node.events.destroy, &tb->node_destroy);
	g_hash_table_insert(titlebar_table(self), c, tb);
	return tb;
}

void
floatdesk_titlebar_sync(
	GowlModuleFloatdesk *self,
	GowlClient          *c
){
	FloatdeskTitlebar *tb;
	gint               width, height, lw, rad, avail, tx;
	gdouble            scale = 1.0;
	gboolean           focused, grabbed;
	float              color[4];

	if (c == NULL || c->scene == NULL || c->deco_top <= 0
	    || c->isfullscreen || self->compositor == NULL
	    || self->theme == NULL) {
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
	grabbed = gowl_compositor_client_is_grabbed(self->compositor, c);

	tb = g_hash_table_lookup(titlebar_table(self), c);
	if (tb == NULL) {
		tb = titlebar_create(self, c);
		if (tb == NULL)
			return;
	}
	tb->width = width;

	/* The caps: only when what they show changed. */
	rad = MAX(0, corner_radius(self) - (gint)c->bw);
	if (!tb->caps_drawn || tb->height != height || tb->scale != scale
	    || tb->focused != focused || tb->radius != rad) {
		tb->height = height;
		tb->scale = scale;
		tb->focused = focused;
		tb->radius = rad;
		tb->caps_drawn = TRUE;
		render_caps(self, tb);
		role_to_rect(self->theme, background_role(focused), 1.0, color);
		wlr_scene_rect_set_color(tb->middle, color);
		role_to_rect(self->theme, GOWL_BAR_COLOR_CRUST, 0.6, color);
		wlr_scene_rect_set_color(tb->line, color);
	}

	/* The geometry: cheap, every time. */
	lw = TITLEBAR_BUTTONS * height;
	rad = MIN(tb->radius, height);
	wlr_scene_node_set_position(&tb->tree->node, (gint)c->bw, (gint)c->bw);
	wlr_scene_node_set_position(&tb->left->node, 0, 0);
	wlr_scene_node_set_position(&tb->middle->node, lw, 0);
	wlr_scene_rect_set_size(tb->middle, MAX(0, width - lw - rad), height);
	wlr_scene_node_set_position(&tb->right->node, MAX(lw, width - rad), 0);
	wlr_scene_node_set_position(&tb->line->node, 0, height - 1);
	wlr_scene_rect_set_size(tb->line, width, 1);

	/*
	 * The title, centred on the whole bar.  Room is kept for the
	 * buttons on BOTH sides, so the centre stays the bar's centre and
	 * the text never runs under a button.
	 */
	avail = MAX(0, width - 2 * (lw + 8));
	if (!tb->title_drawn || g_strcmp0(tb->title_text, c->title) != 0
	    || tb->title_focused != focused || tb->title_height != height
	    || tb->title_scale != scale) {
		g_free(tb->title_text);
		tb->title_text = g_strdup(c->title);
		tb->title_focused = focused;
		tb->title_height = height;
		tb->title_scale = scale;
		render_title(self, tb, avail);
	} else if (avail != tb->title_avail && !grabbed
	           && (tb->title_ellipsized || avail < tb->title_w)) {
		/* Narrower or wider than it was ellipsized for, and the drag
		 * is over: lay it out again for the room it has. */
		render_title(self, tb, avail);
	}
	if (tb->title_w > 0) {
		gint shown = MIN(tb->title_w, avail);

		/* Mid-drag and too wide: crop rather than redraw. */
		if (shown < tb->title_w && shown > 0) {
			struct wlr_fbox src = { 0, 0, shown * tb->title_scale,
			                        tb->title_h * tb->title_scale };

			wlr_scene_buffer_set_source_box(tb->title, &src);
			wlr_scene_buffer_set_dest_size(tb->title, shown,
			                               tb->title_h);
		} else if (shown > 0) {
			wlr_scene_buffer_set_source_box(tb->title, NULL);
			wlr_scene_buffer_set_dest_size(tb->title, tb->title_w,
			                               tb->title_h);
		}
		wlr_scene_node_set_enabled(&tb->title->node, shown > 0);
		tx = (width - shown) / 2;
		wlr_scene_node_set_position(&tb->title->node, MAX(lw + 8, tx),
		                            (height - tb->title_h) / 2);
	}

	/* Above the content and any decorator frame drawn into the tree. */
	wlr_scene_node_raise_to_top(&tb->tree->node);
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
	gdouble                nx, ny;

	if (self->titlebar_nodes == NULL
	    || g_hash_table_size(self->titlebar_nodes) == 0)
		return NULL;
	node = wlr_scene_node_at(&self->compositor->scene->tree.node, lx, ly,
	                         &nx, &ny);
	if (node == NULL || node->parent == NULL)
		return NULL;
	g_hash_table_iter_init(&iter, self->titlebar_nodes);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		FloatdeskTitlebar *tb = value;
		gint               tx, ty;

		if (node->parent != tb->tree)
			continue;
		/* Any of the bar's pieces: answer in the bar's coordinates. */
		if (!wlr_scene_node_coords(&tb->tree->node, &tx, &ty))
			return NULL;
		*local_x = lx - tx;
		*local_y = ly - ty;
		return tb;
	}
	return NULL;
}

/*
 * hit_of:
 * @tb: a bar
 * @x: the pointer across it
 *
 * Returns: which part of the bar @x is in: close, minimize, maximize
 *   from the left edge, one bar height each, then the title
 */
static TitlebarHit
hit_of(
	FloatdeskTitlebar *tb,
	gdouble            x
){
	gdouble step = tb->height;

	if (x < 0)
		return TB_HIT_NONE;
	if (x < step)
		return TB_HIT_CLOSE;
	if (x < 2 * step)
		return TB_HIT_MINIMIZE;
	if (x < 3 * step)
		return TB_HIT_MAXIMIZE;
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
