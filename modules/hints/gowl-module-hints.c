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
 * Window hints: every window wears a letter, and the letter focuses it.
 *
 * tmux's `C-b q' for a compositor.  One key puts a label and a coloured
 * tint over every visible window on every output; the next keystroke
 * focuses the window wearing it.
 *
 * WHY THIS EXISTS AND DIRECTION KEYS DO NOT REPLACE IT.  Every other way
 * of moving focus is RELATIVE: next window, window to the left, next in
 * the stack.  Relative motion is fine with three windows and useless
 * with twelve, because the cost of reaching a window grows with how far
 * away it is and you have to count the hops by eye first.  A hint is
 * absolute and the cost is flat: one keystroke, whichever corner of
 * whichever screen the window is in.  That is the whole argument, and it
 * is why this pays off on a large or a multi-output desk and barely
 * registers on a laptop with two windows.
 *
 * THE LABELS MUST NOT MOVE, and that is the one thing a naive version
 * gets wrong.  If a window's letter changes between one press and the
 * next -- because the list was built in creation order, or focus order,
 * or whatever the client list happened to hold -- then the feature is a
 * lookup table you have to READ every time, and reading it costs more
 * than counting hops did.  It is only faster than the alternative once
 * the letters have become muscle memory, so they are assigned by
 * POSITION and nothing else: the window in a given place on screen wears
 * the same letter today as it did yesterday, whatever order it was
 * opened in.  See hint_compare().
 *
 * The alphabet is the home row first for the same reason: the first
 * eight windows are reachable without moving a finger.
 *
 * WHAT IT DRAWS.  Per window, one scene buffer in the overlay layer: a
 * translucent scrim over the whole frame so the unlabelled content
 * recedes, a border in that window's own colour, and a large badge
 * carrying the letter.  Cairo into a raw buffer, which is what the
 * layout indicator does -- there is no GL here and nothing is captured,
 * so unlike the overview and the switcher this costs nothing to open and
 * cannot fail on a driver that will not compile a shader.
 *
 * COLOURS COME FROM THE PALETTE, cycled, so adjacent windows are told
 * apart at a glance and the whole thing matches whatever flavour is
 * configured.  Under the shipped Catppuccin Mocha that is mauve, green,
 * peach, blue and so on.
 *
 * IT SWALLOWS EVERY KEY WHILE IT IS UP.  A hint overlay that let an
 * unmatched keystroke through to the window underneath would be a
 * feature that occasionally types into your editor, which is worse than
 * not having it.  Escape cancels; anything that cannot begin a label
 * cancels; nothing reaches the surface below.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-hints"

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-client.h"
#include "core/gowl-frame-sink.h"
#include "config/gowl-config.h"
#include "boxed/gowl-color.h"
#include "boxed/gowl-palette.h"
#include "module/gowl-module-manager.h"
#include "util/gowl-backdrop-plan.h"
#include "interfaces/gowl-client-decorator.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-keybind-handler.h"
#include "interfaces/gowl-mouse-handler.h"
#include "interfaces/gowl-scene-effect.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "interfaces/gowl-startup-handler.h"

#include <cairo.h>
#include <math.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#include <gmodule.h>

/*
 * Two characters is as far as this goes, and the cap is a judgement
 * rather than a limit of the arithmetic.  A 26-letter alphabet gives 676
 * two-character labels, which is past any number of windows anybody has
 * on screen; three characters would be slower to type than the relative
 * motion it replaces, which is the whole point of the feature.
 */
#define GOWL_HINTS_MAX_LABEL 2

#define GOWL_TYPE_MODULE_HINTS (gowl_module_hints_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleHints, gowl_module_hints,
                     GOWL, MODULE_HINTS, GowlModule)

/**
 * GowlHint:
 *
 * One window's label and the overlay drawn for it.
 *
 * The client is REFERENCED.  A window closing while the overlay is up is
 * not a rare case -- the overlay can sit open indefinitely by default --
 * and a dangling pointer here would be dereferenced by the very next
 * keystroke.  Holding a reference keeps the object alive; whether it is
 * still worth focusing is asked separately, at selection time, because a
 * referenced client can still have been unmapped.
 */
typedef struct {
	GowlClient              *client;   /* referenced */
	struct wlr_scene_buffer *node;
	struct wlr_box           frame;    /* where it was drawn, layout px */
	gchar                    label[GOWL_HINTS_MAX_LABEL + 1];
	gdouble                  color[3];
} GowlHint;

/**
 * GowlHintsStyle:
 *
 * The look and the behaviour, read out of the config.
 */
typedef struct {
	gchar   *keys;           /* the alphabet, lowercase */
	gchar   *colors;         /* comma-separated palette names */
	gint     timeout_ms;     /* 0 waits for a keystroke */
	gint     size;           /* badge diameter, logical px */
	gint     border;         /* border width, logical px */
	gdouble  scrim;
	gboolean warp_pointer;
	gboolean current_output;
} GowlHintsStyle;

struct _GowlModuleHints {
	GowlModule  parent_instance;
	GowlCompositor *compositor;      /* weak pointer */
	struct wl_listener display_destroy;

	GArray     *hints;               /* GowlHint, valid while open */
	gboolean    open;
	gchar       typed[GOWL_HINTS_MAX_LABEL + 1];
	gint        width;               /* characters per label, 1 or 2 */
	struct wl_event_source *timer;
};

static void hints_effect_init(GowlSceneEffectInterface *iface);
static void hints_ipc_init(GowlIpcHandlerInterface *iface);
static void hints_key_init(GowlKeybindHandlerInterface *iface);
static void hints_mouse_init(GowlMouseHandlerInterface *iface);
static void hints_shutdown_init(GowlShutdownHandlerInterface *iface);
static void hints_startup_init(GowlStartupHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleHints, gowl_module_hints, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, hints_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER, hints_ipc_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_KEYBIND_HANDLER, hints_key_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_MOUSE_HANDLER, hints_mouse_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, hints_shutdown_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, hints_startup_init))

/* ── The look, out of the config ─────────────────────────────────── */

/*
 * The home row first, then the top row, then the bottom.
 *
 * Not alphabetical, and the difference matters more here than it looks:
 * the first eight windows land under fingers that are already resting on
 * them, which is most of what makes a hint faster than counting hops.
 * An alphabetical alphabet puts `a' and `b' next to each other on the
 * wrong hand and `q' -- a common first label -- under the weakest
 * finger.  The same order vimium and every other hint mode has arrived
 * at independently.
 */
#define GOWL_HINTS_DEFAULT_KEYS "asdfghjklqwertyuiopzxcvbnm"

/*
 * The colour cycle, as PALETTE NAMES rather than hex.
 *
 * Every one of these is a key the built-in flavours all define, so the
 * overlay follows whatever palette is configured without this module
 * knowing anything about Catppuccin -- and a config that defines its own
 * `mauve' gets its own mauve here.  The order is chosen for CONTRAST
 * between consecutive entries, because what the colours are for is
 * telling one window's badge from the badge of the window next to it.
 */
#define GOWL_HINTS_DEFAULT_COLORS \
	"mauve,green,peach,blue,pink,teal,yellow,red,sapphire,lavender,flamingo,sky"

static void
hints_style_clear(GowlHintsStyle *style)
{
	g_clear_pointer(&style->keys, g_free);
	g_clear_pointer(&style->colors, g_free);
}

static void
hints_read_style(GowlConfig *config, GowlHintsStyle *out)
{
	memset(out, 0, sizeof(*out));

	out->keys           = g_strdup(gowl_config_get_hints_keys(config));
	out->colors         = g_strdup(gowl_config_get_hints_colors(config));
	out->timeout_ms     = gowl_config_get_hints_timeout(config);
	out->size           = gowl_config_get_hints_size(config);
	out->border         = gowl_config_get_hints_border_width(config);
	out->scrim          = gowl_config_get_hints_scrim(config);
	out->warp_pointer   = gowl_config_get_hints_warp_pointer(config);
	out->current_output = gowl_config_get_hints_current_output(config);

	if (out->keys == NULL || *out->keys == '\0') {
		g_free(out->keys);
		out->keys = g_strdup(GOWL_HINTS_DEFAULT_KEYS);
	}
	if (out->colors == NULL || *out->colors == '\0') {
		g_free(out->colors);
		out->colors = g_strdup(GOWL_HINTS_DEFAULT_COLORS);
	}
}

/*
 * The nth colour of the cycle, resolved through the palette.
 *
 * A name the palette does not know falls back to the focus colour rather
 * than to white: a typo in `hints-colors' should give a badge that is
 * merely the wrong colour, not one that is invisible against a light
 * wallpaper.
 */
static void
hints_color_at(GowlCompositor *self, const gchar *spec, gint n, gdouble out[3])
{
	g_auto(GStrv) names = NULL;
	g_autofree gchar *hex = NULL;
	GowlColor *col = NULL;
	guint count;

	out[0] = self->focus_color[0];
	out[1] = self->focus_color[1];
	out[2] = self->focus_color[2];

	if (spec == NULL || self->config == NULL)
		return;
	names = g_strsplit(spec, ",", -1);
	count = g_strv_length(names);
	if (count == 0)
		return;

	hex = gowl_config_resolve_color(self->config,
	                                g_strstrip(names[n % (gint)count]));
	col = hex != NULL ? gowl_color_new_from_hex(hex) : NULL;
	if (col != NULL) {
		out[0] = col->r;
		out[1] = col->g;
		out[2] = col->b;
		gowl_color_free(col);
	}
}

/* ── Which windows, and in what order ────────────────────────────── */

/* The compositor's own visibility test, which is a macro in
 * gowl-compositor.c and so not reachable from here.  Kept in one place
 * rather than inlined twice. */
static gboolean
hints_visible_on(GowlClient *c, GowlMonitor *m)
{
	if (m == NULL || c->mon != m)
		return FALSE;
	if (c->isoverlay)
		return c->overlay_visible;
	return c->issticky || (c->tags & m->tagset[m->seltags]) != 0;
}

static gboolean
hints_eligible(GowlClient *c, GowlCompositor *self, gboolean current_output)
{
	if (c == NULL || c->scene == NULL || c->mon == NULL)
		return FALSE;
	if (!hints_visible_on(c, c->mon))
		return FALSE;
	if (current_output && c->mon != self->selmon)
		return FALSE;
	return TRUE;
}

static struct wlr_box
hints_drawn_frame(GowlClient *c)
{
	/*
	 * The frame, not the geometry, and the client struct says why at
	 * length: a scrolling layout keeps `geom' unclipped on purpose, so a
	 * window hanging off the side of the screen would get its badge
	 * drawn off-screen with it.  `frame' is where the window is actually
	 * put.
	 */
	if (c->frame.width > 0 && c->frame.height > 0)
		return c->frame;
	return c->geom;
}

/*
 * THE ORDER THE LABELS ARE HANDED OUT IN, and the whole feature rests on
 * it.
 *
 * By POSITION, never by anything about the client itself.  A window in a
 * given place on screen has to wear the same letter it wore last time or
 * the labels never become muscle memory, and a hint you have to read is
 * slower than the relative motion it replaces.
 *
 * Outputs first, left to right and then top to bottom, so the leftmost
 * screen's windows always come first.  Within an output, COLUMN order:
 * x before y.  A tiled screen's primary division is nearly always
 * vertical -- master beside stack -- so column order walks the layout the
 * way it is actually built, and the two agree anyway on the row-split
 * layouts where x ties.
 *
 * The x comparison has a TOLERANCE, and it is load bearing.  Two windows
 * meant to be in the same column can differ by a pixel of border or gap
 * arithmetic, and without a tolerance that pixel decides the whole
 * ordering -- so a window would swap letters with its neighbour when a
 * gap setting changed, which is exactly the instability the ordering
 * exists to prevent.  An eighth of the output is far wider than any
 * rounding and far narrower than a real column.
 */
static gint
hints_compare(gconstpointer a, gconstpointer b)
{
	const GowlHint *ha = a, *hb = b;
	const GowlMonitor *ma = ha->client->mon, *mb = hb->client->mon;
	gint tol;

	if (ma != mb) {
		if (ma->m.x != mb->m.x)
			return ma->m.x < mb->m.x ? -1 : 1;
		if (ma->m.y != mb->m.y)
			return ma->m.y < mb->m.y ? -1 : 1;
	}

	tol = MAX(1, ma->m.width / 8);
	if (ABS(ha->frame.x - hb->frame.x) >= tol)
		return ha->frame.x < hb->frame.x ? -1 : 1;
	if (ha->frame.y != hb->frame.y)
		return ha->frame.y < hb->frame.y ? -1 : 1;
	if (ha->frame.x != hb->frame.x)
		return ha->frame.x < hb->frame.x ? -1 : 1;

	/*
	 * Two windows in the same place, which floating windows really can
	 * be.  Broken by client id: it is monotonic and never reused, so the
	 * answer is at least STABLE for as long as both windows live, which
	 * is all this case can promise.
	 */
	return gowl_client_get_id(ha->client) < gowl_client_get_id(hb->client)
	       ? -1 : 1;
}

/* How many characters each label needs.  All labels are the same length,
 * which is what makes no label a prefix of another and lets a two-key
 * sequence be matched without a terminator. */
static gint
hints_label_width(gint count, gsize alphabet)
{
	if (alphabet == 0)
		return 0;
	if ((gsize)count <= alphabet)
		return 1;
	if ((gsize)count <= alphabet * alphabet)
		return 2;
	return 0;   /* more windows than two characters can name */
}

static void
hints_assign_label(GowlHint *hint, gint n, const gchar *keys, gint width)
{
	gsize alphabet = strlen(keys);

	memset(hint->label, 0, sizeof(hint->label));
	if (width == 1) {
		hint->label[0] = keys[n % (gint)alphabet];
	} else {
		hint->label[0] = keys[(n / (gint)alphabet) % (gint)alphabet];
		hint->label[1] = keys[n % (gint)alphabet];
	}
}

/* ── Drawing ─────────────────────────────────────────────────────── */

static bool
hints_passthrough(struct wlr_scene_buffer *buffer, double *x, double *y)
{
	/*
	 * The overlay takes no pointer input.  It is up for a keystroke, and
	 * a click during it should reach whatever is underneath and cancel,
	 * which the mouse handler does -- not be eaten by a rectangle that
	 * happens to be on top.
	 */
	return false;
}

/* The radius the window is actually drawn with, so the tint ends where
 * the window does.  The same arithmetic the backdrops use, including the
 * border correction: taking the decorator's number raw leaves a bright
 * nick inside each corner of a bordered window. */
static gdouble
hints_corner_radius(GowlCompositor *self, const struct wlr_box *frame,
                    guint border_width)
{
	gpointer dec;

	if (self->module_mgr == NULL)
		return 0.0;
	dec = gowl_module_manager_get_decorator(self->module_mgr);
	if (dec == NULL)
		return 0.0;
	return gowl_backdrop_corner_radius(
		gowl_client_decorator_get_corner_radius((GowlClientDecorator *)dec),
		(gint)border_width, frame->width, frame->height);
}

static void
hints_rounded_rect(cairo_t *cr, gdouble x, gdouble y, gdouble w, gdouble h,
                   gdouble r)
{
	r = MIN(r, MIN(w, h) * 0.5);
	if (r <= 0.5) {
		cairo_rectangle(cr, x, y, w, h);
		return;
	}
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r,     r, -G_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
	cairo_arc(cr, x + r,     y + h - r, r, G_PI / 2, G_PI);
	cairo_arc(cr, x + r,     y + r,     r, G_PI, 3 * G_PI / 2);
	cairo_close_path(cr);
}

/*
 * One window's overlay: a scrim, a border and a badge.
 *
 * Rendered at the OUTPUT's scale while every measurement stays in
 * logical pixels, which is the same split the layout indicator uses --
 * the badge is then crisp on a HiDPI panel without any of the arithmetic
 * below having to know the panel exists.
 */
static void
hints_draw(GowlModuleHints *mod, GowlCompositor *self, GowlHint *hint,
           const GowlHintsStyle *style, gboolean dim)
{
	cairo_surface_t *surface;
	cairo_t         *cr;
	struct wlr_buffer *buffer;
	cairo_text_extents_t ext;
	GowlMonitor *m = hint->client->mon;
	gdouble scale = m->wlr_output != NULL ? MAX(1.0, m->wlr_output->scale) : 1.0;
	gdouble w = hint->frame.width, h = hint->frame.height;
	gdouble radius, badge, font;
	g_autofree gchar *shown = NULL;

	if (w < 1.0 || h < 1.0)
		return;

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
	                                     (gint)ceil(w * scale),
	                                     (gint)ceil(h * scale));
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return;
	}
	cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);

	radius = hints_corner_radius(self, &hint->frame, hint->client->bw);

	/* The scrim, clipped to the window's own rounded shape. */
	hints_rounded_rect(cr, 0, 0, w, h, radius);
	cairo_set_source_rgba(cr, 0.04, 0.04, 0.06, style->scrim);
	cairo_fill_preserve(cr);

	/* The border, drawn INSIDE the path: a stroke centred on it would
	 * put half its width outside the window, where the scene buffer is
	 * clipped, and the overlay would look thinner than it is. */
	if (style->border > 0) {
		cairo_set_source_rgba(cr, hint->color[0], hint->color[1],
		                      hint->color[2], 0.92);
		cairo_set_line_width(cr, style->border * 2.0);
		cairo_save(cr);
		cairo_clip_preserve(cr);
		cairo_stroke_preserve(cr);
		cairo_restore(cr);
	}
	cairo_new_path(cr);

	/*
	 * The badge, capped against the window.  A small window -- a
	 * dropdown, a picture-in-picture -- must still be able to show its
	 * letter, and a badge drawn at the configured size over a 120-pixel
	 * window would be a coloured disc with nothing legible on it.
	 */
	badge = MIN((gdouble)style->size, MIN(w, h) * 0.62);
	badge = MAX(badge, 18.0);
	font  = badge * 0.58;

	cairo_arc(cr, w * 0.5, h * 0.5, badge * 0.5, 0, 2 * G_PI);
	cairo_set_source_rgba(cr, hint->color[0], hint->color[1], hint->color[2],
	                      dim ? 0.45 : 0.95);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, 0.04, 0.04, 0.06, 0.55);
	cairo_set_line_width(cr, MAX(1.0, badge * 0.04));
	cairo_stroke(cr);

	/* Upper case on screen, lower case on the keyboard: a capital is
	 * easier to pick out of a busy window at a glance, and nobody expects
	 * to hold shift for it. */
	shown = g_ascii_strup(hint->label, -1);
	cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
	                       CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, font);
	cairo_text_extents(cr, shown, &ext);
	cairo_move_to(cr, w * 0.5 - (ext.width * 0.5 + ext.x_bearing),
	              h * 0.5 - (ext.height * 0.5 + ext.y_bearing));
	cairo_set_source_rgb(cr, 0.06, 0.06, 0.09);
	cairo_show_text(cr, shown);

	cairo_destroy(cr);
	cairo_surface_flush(surface);

	buffer = gowl_raw_buffer_create(cairo_image_surface_get_data(surface),
	                                cairo_image_surface_get_width(surface),
	                                cairo_image_surface_get_height(surface),
	                                cairo_image_surface_get_stride(surface));
	cairo_surface_destroy(surface);
	if (buffer == NULL)
		return;

	if (hint->node == NULL) {
		hint->node = wlr_scene_buffer_create(
			self->layers[GOWL_SCENE_LAYER_OVERLAY], buffer);
		if (hint->node != NULL)
			hint->node->point_accepts_input = hints_passthrough;
	} else {
		wlr_scene_buffer_set_buffer(hint->node, buffer);
	}
	wlr_buffer_drop(buffer);
	if (hint->node == NULL)
		return;

	wlr_scene_buffer_set_dest_size(hint->node, hint->frame.width,
	                               hint->frame.height);
	wlr_scene_node_set_position(&hint->node->node, hint->frame.x,
	                            hint->frame.y);
	wlr_scene_node_raise_to_top(&hint->node->node);
}

/* ── Opening and closing ─────────────────────────────────────────── */

static void
hints_free_one(gpointer data)
{
	GowlHint *hint = data;

	if (hint->node != NULL) {
		wlr_scene_node_destroy(&hint->node->node);
		hint->node = NULL;
	}
	g_clear_object(&hint->client);
}

static void
hints_close(GowlModuleHints *mod)
{
	if (mod->hints != NULL)
		g_array_set_size(mod->hints, 0);
	memset(mod->typed, 0, sizeof(mod->typed));
	mod->open  = FALSE;
	mod->width = 0;
	if (mod->timer != NULL)
		wl_event_source_timer_update(mod->timer, 0);
}

static gint
hints_expire(gpointer data)
{
	hints_close(data);
	return 0;
}

/* Redraw every badge, dimming the ones a typed prefix has ruled out.
 * With single-character labels nothing is ever ruled out, so this is a
 * plain redraw; with two it is the feedback that says the first key
 * landed. */
static void
hints_render_all(GowlModuleHints *mod, GowlCompositor *self,
                 const GowlHintsStyle *style)
{
	gsize typed = strlen(mod->typed);
	guint i;

	for (i = 0; i < mod->hints->len; i++) {
		GowlHint *hint = &g_array_index(mod->hints, GowlHint, i);
		gboolean dim = typed > 0
		               && strncmp(hint->label, mod->typed, typed) != 0;

		hints_draw(mod, self, hint, style, dim);
	}
}

static gboolean
hints_open(GowlModuleHints *mod, GowlCompositor *self)
{
	GowlHintsStyle style;
	GList *l;
	guint  i;
	gint   width;

	if (self == NULL || self->config == NULL || self->locked)
		return FALSE;

	hints_close(mod);
	hints_read_style(self->config, &style);

	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;
		GowlHint    hint;

		if (!hints_eligible(c, self, style.current_output))
			continue;
		memset(&hint, 0, sizeof(hint));
		hint.client = g_object_ref(c);
		hint.frame  = hints_drawn_frame(c);
		g_array_append_val(mod->hints, hint);
	}

	if (mod->hints->len == 0) {
		hints_style_clear(&style);
		return FALSE;
	}

	/* Position order, and only then the labels: see hints_compare(). */
	g_array_sort(mod->hints, hints_compare);

	width = hints_label_width((gint)mod->hints->len, strlen(style.keys));
	if (width == 0) {
		/* More windows than two characters of this alphabet can name.
		 * Refusing is better than labelling some of them: a hint
		 * overlay where a third of the windows have no letter is a
		 * feature that has silently stopped working. */
		g_warning("hints: %u windows is more than the %zu-character "
		          "alphabet can label; widen `hints-keys'",
		          mod->hints->len, strlen(style.keys));
		g_array_set_size(mod->hints, 0);
		hints_style_clear(&style);
		return FALSE;
	}
	mod->width = width;

	for (i = 0; i < mod->hints->len; i++) {
		GowlHint *hint = &g_array_index(mod->hints, GowlHint, i);

		hints_assign_label(hint, (gint)i, style.keys, width);
		hints_color_at(self, style.colors, (gint)i, hint->color);
	}

	mod->open = TRUE;
	hints_render_all(mod, self, &style);

	/*
	 * The timeout is OFF by default, which is where this parts company
	 * with tmux.  `display-panes-time' is one second there and is the
	 * most complained-about thing about the feature: the labels vanish
	 * while you are still looking for the one you want, and the only
	 * recovery is to press the key again.  Waiting costs nothing --
	 * every key is swallowed anyway, so there is no state to be stuck in
	 * -- and Escape is right there.
	 */
	if (style.timeout_ms > 0 && mod->timer != NULL)
		wl_event_source_timer_update(mod->timer, style.timeout_ms);

	hints_style_clear(&style);
	return TRUE;
}

/*
 * Focus the window wearing @label.
 *
 * The client was referenced when the overlay opened, so it is alive --
 * but alive is not the same as still worth focusing.  A window that
 * unmapped while the overlay was up has no surface to give the keyboard
 * to, and focusing it would leave the seat pointing at nothing.
 */
/* Which window wears @label, by id; 0 for none.  Split out of
 * hints_select() because the IPC reply names the window and the overlay
 * -- labels and all -- is gone by the time the selection returns. */
static guint
hints_client_for(GowlModuleHints *mod, const gchar *label)
{
	guint i;

	for (i = 0; i < mod->hints->len; i++) {
		GowlHint *hint = &g_array_index(mod->hints, GowlHint, i);

		if (g_ascii_strcasecmp(hint->label, label) == 0)
			return gowl_client_get_id(hint->client);
	}
	return 0;
}

static gboolean
hints_select(GowlModuleHints *mod, GowlCompositor *self, const gchar *label)
{
	GowlClient *want = NULL;
	gboolean    warp;
	guint       i;

	for (i = 0; i < mod->hints->len && want == NULL; i++) {
		GowlHint *hint = &g_array_index(mod->hints, GowlHint, i);

		if (g_ascii_strcasecmp(hint->label, label) == 0)
			want = hint->client;
	}
	if (want == NULL)
		return FALSE;

	warp = self->config != NULL
	       && gowl_config_get_hints_warp_pointer(self->config);
	/*
	 * Alive is not the same as focusable.
	 *
	 * The client was referenced when the overlay opened, so it is still
	 * there -- but the overlay waits indefinitely by default, and in
	 * that time a window can lose its tree, its monitor or its surface
	 * while the object stays behind.  Focusing one that has no surface
	 * hands the seat a NULL to give the keyboard to, which wlroots
	 * asserts on; gowl_client_get_wlr_surface() is annotated nullable
	 * for exactly this reason.
	 */
	if (want->scene == NULL || want->mon == NULL
	    || gowl_client_get_wlr_surface(want) == NULL) {
		hints_close(mod);
		return FALSE;
	}

	/* Take the overlay down FIRST.  Focusing raises and re-places, and a
	 * badge still in the scene at that moment is a rectangle drawn over
	 * the window that has just come forward. */
	g_object_ref(want);
	hints_close(mod);

	/*
	 * focus_client, NOT show_client, and the difference matters.
	 *
	 * show_client() sets the monitor's tags to the chosen window's --
	 * which is right for jumping to a window that is not on screen, and
	 * wrong here for the same reason it is right there.  Everything the
	 * overlay labels is ALREADY visible, so switching the view could
	 * only hide the other windows it had just drawn badges on.
	 *
	 * The cross-output half comes free: focus_client() selects the
	 * client's monitor itself, so picking a window on the other screen
	 * moves the compositor's idea of which screen is current along with
	 * the keyboard.  Without that the next `Super+Return' would open a
	 * terminal on the screen that was left behind.
	 */
	gowl_compositor_focus_client(self, want, TRUE);

	/*
	 * And take the pointer with it, by default.
	 *
	 * `sloppyfocus' is on out of the box, so leaving the cursor where it
	 * was means the next nudge of the mouse hands focus straight back to
	 * whatever it is sitting over -- which makes the feature look broken
	 * in the one configuration most people are running.  Warping to the
	 * middle of the chosen window is what every other absolute-focus
	 * mechanism does for the same reason.
	 */
	if (warp) {
		struct wlr_box f = hints_drawn_frame(want);

		if (f.width > 0 && f.height > 0)
			gowl_compositor_warp_cursor(self, f.x + f.width / 2.0,
			                            f.y + f.height / 2.0);
	}
	g_object_unref(want);
	return TRUE;
}

/* ── Hooks ───────────────────────────────────────────────────────── */

static GowlCompositor *
hints_comp(GowlModuleHints *mod)
{
	return mod->compositor;
}

/*
 * Every key while the overlay is up, and nothing gets past.
 *
 * Modules are offered a key only after the user's own binds have
 * declined it, so this cannot shadow a configured bind -- which is also
 * why the labels are unmodified letters: a bare `a' is not something
 * anybody binds at the compositor level, so the overlay is reached
 * without fighting the config.
 */
static gboolean
hints_handle_key(GowlKeybindHandler *handler, guint modifiers, guint keysym,
                 gboolean pressed)
{
	GowlModuleHints *mod = GOWL_MODULE_HINTS(handler);
	GowlCompositor  *self = hints_comp(mod);
	gsize            typed;
	gchar            ch;
	guint            i;

	if (self == NULL || !mod->open)
		return FALSE;
	/* Releases are swallowed but do nothing: without this the release of
	 * the very key that opened the overlay would be read as a label. */
	if (!pressed)
		return TRUE;

	if (keysym == XKB_KEY_Escape || keysym == XKB_KEY_q) {
		hints_close(mod);
		return TRUE;
	}

	/* Backspace takes back a character of a two-key label rather than
	 * cancelling: a mistyped first key should not cost the whole
	 * overlay. */
	if (keysym == XKB_KEY_BackSpace) {
		typed = strlen(mod->typed);
		if (typed > 0) {
			GowlHintsStyle style;

			mod->typed[typed - 1] = '\0';
			hints_read_style(self->config, &style);
			hints_render_all(mod, self, &style);
			hints_style_clear(&style);
		}
		return TRUE;
	}

	if (keysym < 0x20 || keysym > 0x7e) {
		/* A modifier, a function key, anything that cannot begin a
		 * label.  Swallowed rather than acted on: the overlay is modal
		 * and a stray key must not reach the window under it. */
		return TRUE;
	}

	ch = g_ascii_tolower((gchar)keysym);
	typed = strlen(mod->typed);
	if (typed >= (gsize)mod->width)
		return TRUE;
	mod->typed[typed] = ch;
	mod->typed[typed + 1] = '\0';
	typed++;

	/* Nothing can match this prefix: cancel rather than sit there, which
	 * is what tells somebody who hit the wrong key that they did. */
	for (i = 0; i < mod->hints->len; i++) {
		GowlHint *hint = &g_array_index(mod->hints, GowlHint, i);

		if (strncmp(hint->label, mod->typed, typed) == 0)
			break;
	}
	if (i == mod->hints->len) {
		hints_close(mod);
		return TRUE;
	}

	if (typed >= (gsize)mod->width) {
		hints_select(mod, self, mod->typed);
		return TRUE;
	}

	{
		GowlHintsStyle style;

		hints_read_style(self->config, &style);
		hints_render_all(mod, self, &style);
		hints_style_clear(&style);
	}
	return TRUE;
}

/* A click cancels.  Somebody who reached for the mouse has answered the
 * question the overlay was asking. */
static gboolean
hints_handle_button(GowlMouseHandler *handler, guint button, guint state,
                    guint modifiers)
{
	GowlModuleHints *mod = GOWL_MODULE_HINTS(handler);

	if (!mod->open || state == 0)
		return FALSE;
	hints_close(mod);
	return TRUE;
}

static gchar *
hints_handle_command(GowlIpcHandler *handler, const gchar *command,
                     const gchar *args)
{
	GowlModuleHints *mod = GOWL_MODULE_HINTS(handler);
	GowlCompositor  *self = hints_comp(mod);
	guint            id;

	if (self == NULL || command == NULL)
		return NULL;

	if (g_strcmp0(command, "hints") == 0
	    || g_strcmp0(command, "hints-toggle") == 0) {
		if (mod->open) {
			hints_close(mod);
			return g_strdup("OK hints closed");
		}
		return g_strdup(hints_open(mod, self)
		                ? "OK hints shown" : "ERROR no windows to label");
	}

	if (g_strcmp0(command, "hints-show") == 0)
		return g_strdup(hints_open(mod, self)
		                ? "OK hints shown" : "ERROR no windows to label");

	if (g_strcmp0(command, "hints-hide") == 0) {
		hints_close(mod);
		return g_strdup("OK hints closed");
	}

	if (g_strcmp0(command, "hints-select") == 0) {
		if (!mod->open)
			return g_strdup("ERROR hints not shown");
		if (args == NULL || *args == '\0')
			return g_strdup("ERROR hints-select needs a label");
		id = hints_client_for(mod, args);
		if (id == 0)
			return g_strdup("ERROR no such hint");
		return hints_select(mod, self, args)
		       ? g_strdup_printf("OK hints selected %u", id)
		       : g_strdup("ERROR that window went away");
	}

	/*
	 * The labels as data, so an embedder can draw its own picker over
	 * the same assignment rather than inventing a second one that
	 * disagrees with what is on screen.
	 */
	if (g_strcmp0(command, "hints-list") == 0) {
		GString *out = g_string_new(NULL);
		guint    i;

		for (i = 0; i < mod->hints->len; i++) {
			GowlHint *hint = &g_array_index(mod->hints, GowlHint, i);

			g_string_append_printf(out, "%s %u %s\n", hint->label,
			                       gowl_client_get_id(hint->client),
			                       gowl_client_get_title(hint->client) != NULL
			                       ? gowl_client_get_title(hint->client) : "");
		}
		return g_string_free(out, FALSE);
	}

	return NULL;
}

/*
 * A window moved, resized or went away while the overlay was up.
 *
 * Rare and not impossible: the overlay waits indefinitely by default, and
 * a client can resize itself at any moment.  A badge left at the old
 * place is worse than no badge, because it points at the wrong window.
 *
 * Anything structural -- a window closing or unmapping -- takes the whole
 * overlay down instead of re-labelling: re-labelling would move every
 * letter after the missing one, which is the one thing the ordering
 * exists to prevent, and doing it under somebody's fingers mid-keystroke
 * is how a hint mode focuses the wrong window.
 */
static gboolean
hints_client_event(GowlSceneEffect *effect, GowlCompositor *self,
                   GowlClient *c, GowlSceneEffectEvent event,
                   const struct wlr_box *previous, gboolean interactive)
{
	GowlModuleHints *mod = GOWL_MODULE_HINTS(effect);

	if (!mod->open)
		return FALSE;
	switch (event) {
	case GOWL_SCENE_EFFECT_UNMAP:
	case GOWL_SCENE_EFFECT_DESTROY:
		hints_close(mod);
		break;
	default:
		break;
	}
	return FALSE;
}

static void
hints_client_placed(GowlSceneEffect *effect, GowlCompositor *self,
                    GowlClient *c, gboolean settled)
{
	GowlModuleHints *mod = GOWL_MODULE_HINTS(effect);
	GowlHintsStyle   style;
	gboolean         moved = FALSE;
	guint            i;

	if (!mod->open || self == NULL || self->config == NULL)
		return;

	for (i = 0; i < mod->hints->len; i++) {
		GowlHint      *hint = &g_array_index(mod->hints, GowlHint, i);
		struct wlr_box now;

		if (hint->client != c)
			continue;
		now = hints_drawn_frame(c);
		if (memcmp(&now, &hint->frame, sizeof(now)) != 0) {
			hint->frame = now;
			moved = TRUE;
		}
	}
	if (!moved)
		return;

	hints_read_style(self->config, &style);
	hints_render_all(mod, self, &style);
	hints_style_clear(&style);
}

static void
hints_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                      GowlMonitor *m)
{
	GowlModuleHints *mod = GOWL_MODULE_HINTS(effect);

	/* Every badge on that output has just lost the tree it was in, and
	 * the labels after it would all shift.  Down it goes. */
	if (mod->open)
		hints_close(mod);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

static void
hints_detach(GowlModuleHints *mod)
{
	hints_close(mod);
	if (mod->timer != NULL) {
		wl_event_source_remove(mod->timer);
		mod->timer = NULL;
	}
	if (mod->compositor != NULL) {
		g_object_remove_weak_pointer(G_OBJECT(mod->compositor),
		                             (gpointer *)&mod->compositor);
		mod->compositor = NULL;
	}
	wl_list_remove(&mod->display_destroy.link);
	wl_list_init(&mod->display_destroy.link);
}

static void
hints_display_destroyed(struct wl_listener *listener, void *data)
{
	GowlModuleHints *mod = wl_container_of(listener, mod, display_destroy);

	hints_detach(mod);
}

/*
 * The compositor arrives here rather than through a weak ref set from a
 * scene-effect hook, because the first thing that happens to this module
 * may well be an IPC command -- and none of its hooks fire until a
 * window moves.  A module whose only path to the compositor is a hook it
 * has not been given yet answers its first command with nothing.
 */
static void
hints_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleHints *mod = GOWL_MODULE_HINTS(handler);

	if (mod->compositor == compositor)
		return;
	hints_detach(mod);
	mod->compositor = compositor;
	g_object_add_weak_pointer(G_OBJECT(compositor),
	                          (gpointer *)&mod->compositor);
	mod->timer = wl_event_loop_add_timer(
		gowl_compositor_get_event_loop(compositor), hints_expire, mod);
	mod->display_destroy.notify = hints_display_destroyed;
	wl_display_add_destroy_listener(
		gowl_compositor_get_wl_display(compositor), &mod->display_destroy);
}

static void
hints_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	hints_close(GOWL_MODULE_HINTS(effect));
}

static void
hints_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_event    = hints_client_event;
	iface->client_placed   = hints_client_placed;
	iface->monitor_removed = hints_monitor_removed;
	iface->finish          = hints_finish;
}
static void hints_ipc_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = hints_handle_command;
}
static void hints_key_init(GowlKeybindHandlerInterface *iface)
{
	iface->handle_key = hints_handle_key;
}
static void hints_mouse_init(GowlMouseHandlerInterface *iface)
{
	iface->handle_button = hints_handle_button;
}
static void
hints_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	hints_detach(GOWL_MODULE_HINTS(handler));
}
static void hints_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = hints_shutdown;
}
static void hints_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = hints_startup;
}

/*
 * Above the animation module, so a window placed by an animation has
 * already been placed by the time client_placed reaches here and the
 * badge lands on the frame the window actually ended up with.
 */
#define GOWL_HINTS_PRIORITY (40)

static gboolean
hints_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_HINTS_PRIORITY);
	return TRUE;
}

static void
hints_deactivate(GowlModule *base)
{
	hints_detach(GOWL_MODULE_HINTS(base));
}

static const gchar *hints_name(GowlModule *m)    { return "hints"; }
static const gchar *hints_version(GowlModule *m) { return "0.1.0"; }
static const gchar *hints_description(GowlModule *m)
{
	return "Labels every window with a key that focuses it";
}

static void
hints_finalize(GObject *object)
{
	GowlModuleHints *mod = GOWL_MODULE_HINTS(object);

	hints_detach(mod);
	g_clear_pointer(&mod->hints, g_array_unref);
	G_OBJECT_CLASS(gowl_module_hints_parent_class)->finalize(object);
}

static void
gowl_module_hints_class_init(GowlModuleHintsClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = hints_activate;
	mod->deactivate      = hints_deactivate;
	mod->get_name        = hints_name;
	mod->get_description = hints_description;
	mod->get_version     = hints_version;
	G_OBJECT_CLASS(klass)->finalize = hints_finalize;
}

static void
gowl_module_hints_init(GowlModuleHints *mod)
{
	mod->hints = g_array_new(FALSE, TRUE, sizeof(GowlHint));
	g_array_set_clear_func(mod->hints, hints_free_one);
	wl_list_init(&mod->display_destroy.link);
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_HINTS_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_HINTS;
}
