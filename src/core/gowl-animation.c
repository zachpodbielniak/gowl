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
 * gowl-animation.c -- sliding windows into place
 *
 * What is animated is the scene node's POSITION, and only that.  The
 * client is configured to its final size the moment the layout decides
 * it, and the node slides to meet it.
 *
 * Animating the size too would look better and cost far more: every
 * intermediate size is a configure the client has to acknowledge and
 * re-render for, so a 200 ms resize becomes a dozen round trips per
 * window per move, and a client that renders slowly falls visibly
 * behind its own frame.  Position is a compositor-side transform of
 * something already rendered, so it is free.
 *
 * The state lives on the client rather than in a list on the
 * compositor.  A client destroyed mid-slide takes its animation with
 * it, which a separate list would have to be told about.
 */

#include "gowl-animation.h"
#include "gowl-core-private.h"
#include "gowl-compositor.h"

#include <math.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>

/* ── Curves ──────────────────────────────────────────────────────── */

typedef struct {
	const gchar *name;
	gdouble x1, y1, x2, y2;
} GowlCurveDef;

/*
 * The same curves Omarchy configures Hyprland with, by the same names
 * in kebab-case.  Endpoints are fixed at (0,0) and (1,1), so these two
 * control points are the whole definition.
 */
static const GowlCurveDef curves[] = {
	{ "linear",            0.00, 0.00, 1.00, 1.00 },
	{ "ease-out-quint",    0.23, 1.00, 0.32, 1.00 },
	{ "ease-in-out-cubic", 0.65, 0.05, 0.36, 1.00 },
	{ "almost-linear",     0.50, 0.50, 0.75, 1.00 },
	{ "quick",             0.15, 0.00, 0.10, 1.00 },

	/*
	 * Leaves almost the whole distance in the first third and then
	 * settles, which at 180 ms reads as decisive rather than as a
	 * window being dragged.  The default, because quint spends longer
	 * in the middle and at these durations that middle feels like lag.
	 */
	{ "ease-out-expo",     0.16, 1.00, 0.30, 1.00 },

	/*
	 * These two overshoot: y1 above 1 carries the window PAST its
	 * target before it comes back.  Not the default, and worth
	 * knowing why --- in a tiling layout windows are adjacent, so an
	 * overshoot briefly puts one on top of its neighbour.  It looks
	 * great with gaps and wrong without them.
	 */
	{ "ease-out-back",     0.34, 1.56, 0.64, 1.00 },
	{ "spring",            0.16, 1.24, 0.30, 1.00 },
};

#define GOWL_CURVE_DEFAULT 5        /* ease-out-expo */

/*
 * How far below its final position a window starts.  Small on purpose:
 * a big offset is a slide, and a slide from anywhere but the window's
 * own neighbourhood is the corner-sweep bug wearing a different hat.
 * This is a nudge that the fade does most of the work of hiding.
 */
#define GOWL_ANIM_OPEN_RISE (24)

static gdouble
bezier_axis(gdouble t, gdouble p1, gdouble p2)
{
	gdouble mt = 1.0 - t;

	/* Cubic Bézier with P0 = 0 and P3 = 1. */
	return 3.0 * mt * mt * t * p1
	     + 3.0 * mt * t * t * p2
	     + t * t * t;
}

/*
 * A Bézier is parameterised by its own t, not by x, so evaluating
 * "y at x = 0.4" means solving for the t whose x is 0.4 first.  Bisection
 * rather than Newton: these curves can have a near-zero derivative at
 * the ends (ease-out-quint does), where Newton diverges, and 24
 * bisection steps resolve far finer than a pixel.
 */
static gdouble
bezier_solve(gdouble x, gdouble x1, gdouble x2)
{
	gdouble lo = 0.0, hi = 1.0, mid = x;
	gint i;

	for (i = 0; i < 24; i++) {
		gdouble cx;

		mid = 0.5 * (lo + hi);
		cx = bezier_axis(mid, x1, x2);
		if (cx < x)
			lo = mid;
		else
			hi = mid;
	}
	return mid;
}

gdouble
gowl_curve_eval(const gchar *name, gdouble t)
{
	const GowlCurveDef *c = &curves[GOWL_CURVE_DEFAULT];
	gsize i;
	gdouble u;

	if (t <= 0.0)
		return 0.0;
	if (t >= 1.0)
		return 1.0;

	if (name != NULL) {
		for (i = 0; i < G_N_ELEMENTS(curves); i++) {
			if (g_ascii_strcasecmp(curves[i].name, name) == 0) {
				c = &curves[i];
				break;
			}
		}
	}

	/* Linear needs no solve, and it is the curve most likely to be
	 * chosen by someone who wants the cost down. */
	if (c->x1 == 0.0 && c->y1 == 0.0 && c->x2 == 1.0 && c->y2 == 1.0)
		return t;

	u = bezier_solve(t, c->x1, c->x2);
	return bezier_axis(u, c->y1, c->y2);
}

/* ── Configuration ───────────────────────────────────────────────── */

gboolean
gowl_animation_enabled(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);

	if (self->config == NULL)
		return FALSE;
	if (!gowl_config_get_animations(self->config))
		return FALSE;

	return gowl_config_get_animation_duration(self->config) > 0;
}

/*
 * How long a move should take, and how long an open should.  Opening
 * gets its own duration because the two are not the same gesture: a
 * re-tile is a correction and wants to be over quickly, while an open
 * is an arrival and can afford the extra beat that makes it read as
 * one.  Either falls back to the single `animation-duration'.
 */
static gint
open_duration(GowlCompositor *self)
{
	gint d;

	if (self->config == NULL)
		return 0;

	d = gowl_config_get_animation_duration_open(self->config);
	if (d < 0)
		d = gowl_config_get_animation_duration(self->config);

	return d;
}

/* ── Opening ─────────────────────────────────────────────────────── */

/* The half both entrances share. */
static void
fade_in_start(GowlCompositor *self, GowlClient *c, gboolean rise)
{
	gint duration;

	if (!gowl_animation_enabled(self) || c->scene == NULL)
		return;

	duration = open_duration(self);
	if (duration <= 0)
		return;

	c->anim_open_rise = rise;
	c->anim_opening = TRUE;
	c->anim_open_start_us = g_get_monotonic_time();
	c->anim_open_dur_us = (gint64)duration * 1000;

	/* Start invisible.  Set here rather than left to the first tick so
	 * that the frame between mapping and the first tick does not show
	 * the window at full opacity --- one frame of pop is still a pop. */
	gowl_client_set_anim_alpha(c, 0.0f);
}

void
gowl_animation_open_start(GowlCompositor *self, GowlClient *c)
{
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(c != NULL);

	fade_in_start(self, c, TRUE);
}

void
gowl_animation_reveal_start(GowlCompositor *self, GowlClient *c)
{
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(c != NULL);

	/* Already fading for some other reason; leave it be rather than
	 * restarting it from zero, which on a fast tag switch would keep
	 * a window permanently half-transparent. */
	if (c->anim_opening)
		return;

	fade_in_start(self, c, FALSE);
}

void
gowl_animation_open_cancel(GowlClient *c)
{
	if (c == NULL || !c->anim_opening)
		return;

	c->anim_opening = FALSE;
	gowl_client_set_anim_alpha(c, 1.0f);
}

/*
 * Advances one client's open animation.  Returns TRUE while it is
 * still running.
 */
static gboolean
open_tick(GowlCompositor *self, GowlClient *c, gint64 now_us,
          const gchar *curve)
{
	gdouble t, e;
	gint rise;

	if (!c->anim_opening)
		return FALSE;

	if (c->anim_open_dur_us <= 0) {
		gowl_animation_open_cancel(c);
		return FALSE;
	}

	t = (gdouble)(now_us - c->anim_open_start_us)
		/ (gdouble)c->anim_open_dur_us;
	if (t < 0.0)
		t = 0.0;
	if (t >= 1.0) {
		gowl_animation_open_cancel(c);
		/* The rise is over, so put the node exactly where the layout
		 * wants it --- unless a move animation has since taken over,
		 * which owns the position from then on. */
		if (!c->anim_active)
			wlr_scene_node_set_position(&c->scene->node,
			                            c->geom.x, c->geom.y);
		return FALSE;
	}

	e = gowl_curve_eval(curve, t);
	gowl_client_set_anim_alpha(c, (gfloat)e);

	/*
	 * The rise, in the same easing.  A move animation started
	 * mid-open owns the position, so the rise steps aside rather than
	 * fighting it for the same node --- the fade carries on either
	 * way, which is the half that does most of the work.
	 */
	if (c->anim_open_rise && !c->anim_active) {
		rise = (gint)lround((1.0 - e) * (gdouble)GOWL_ANIM_OPEN_RISE);
		wlr_scene_node_set_position(&c->scene->node,
		                            c->geom.x, c->geom.y + rise);
	}

	return TRUE;
}

/* Defined below, next to the geometry animation they belong to. */
static void ghost_release (GowlClient *c);

/* ── Closing ─────────────────────────────────────────────────────── */

/*
 * A window that has gone, still on screen.
 *
 * There is no wlr_scene_node_snapshot in wlroots 0.20, so the way to
 * keep a closed window visible is to hold on to the last thing it
 * drew: lock its buffer, hang a standalone scene buffer off the same
 * layer, and let the client die.  A locked buffer outlives the surface,
 * the role object and the client, which is exactly the guarantee this
 * needs.
 *
 * The snapshot is the toplevel's own surface and nothing else, so a
 * window whose decorations or content live in SUBSURFACES loses them
 * for the duration of the fade.  That is also what makes the shrink
 * below safe: scaling one buffer is a supported operation, where
 * scaling a tree of them would pull a window apart --- which is why the
 * open animation has no scale and this one does.
 */
struct _GowlCloseAnim {
	struct wlr_scene_buffer *node;
	struct wlr_buffer       *buffer;   /* locked */
	GowlMonitor             *mon;      /* NULL once its output is gone */
	gint64                   start_us;
	gint64                   dur_us;
	gint                     x, y;     /* where the window was */
	gint                     w, h;     /* and how big */
};

/*
 * How far a closing window shrinks, as a fraction of its size.  Small:
 * the fade does most of the work and a big shrink reads as the window
 * being sucked away rather than dismissed.
 */
#define GOWL_ANIM_CLOSE_SHRINK (0.06)

/* How far it sinks, in pixels, over the same time. */
#define GOWL_ANIM_CLOSE_SINK (12)

static gint
close_duration(GowlCompositor *self)
{
	gint d;

	if (self->config == NULL)
		return 0;

	d = gowl_config_get_animation_duration_close(self->config);
	if (d < 0)
		d = gowl_config_get_animation_duration(self->config);

	return d;
}

static void
close_anim_free(GowlCloseAnim *a)
{
	if (a == NULL)
		return;

	if (a->node != NULL)
		wlr_scene_node_destroy(&a->node->node);
	if (a->buffer != NULL)
		wlr_buffer_unlock(a->buffer);

	g_free(a);
}

void
gowl_animation_close_start(GowlCompositor *self, GowlClient *c)
{
	GowlCloseAnim *a;
	struct wlr_surface *surface;
	struct wlr_scene_tree *parent;
	gint duration;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(c != NULL);

	if (!gowl_animation_enabled(self) || c->scene == NULL)
		return;

	/* A window that never got placed never appeared, so there is
	 * nothing to fade out. */
	if (!c->anim_placed || c->isembedded)
		return;

	duration = close_duration(self);
	if (duration <= 0)
		return;

	surface = gowl_client_get_wlr_surface(c);
	/*
	 * No buffer, no snapshot.  This is not an error: unmap fires for a
	 * committed NULL buffer as well as for a destroyed role object,
	 * and in the first case the client has already thrown away the
	 * thing we would have kept.  Such a window just disappears, which
	 * is what every window did before this existed.
	 */
	if (surface == NULL || surface->buffer == NULL)
		return;

	/*
	 * Above the tiling layer, not in it.
	 *
	 * Closing a window in a tiling layout immediately re-tiles the
	 * others into the space it left, so a ghost left among them is
	 * drawn over by the very window expanding into its place --- it
	 * animates perfectly and is invisible the whole time, which is
	 * exactly what happened the first time this was tested under a
	 * real session.  The float layer is above the tiles and below the
	 * bars, fullscreen and the lock screen, which is where something
	 * that was on top and is leaving belongs.
	 */
	parent = self->layers[GOWL_SCENE_LAYER_FLOAT];
	if (parent == NULL)
		return;

	a = g_new0(GowlCloseAnim, 1);
	a->buffer = wlr_buffer_lock(&surface->buffer->base);
	a->node = wlr_scene_buffer_create(parent, a->buffer);
	if (a->node == NULL) {
		close_anim_free(a);
		return;
	}

	/*
	 * Where the window actually was.  Mid-morph that is the rect the
	 * animation last drew, not the one the layout last decided --- a
	 * window closed while it is still resizing would otherwise leave a
	 * ghost at its destination size in its current position.
	 */
	{
		struct wlr_box vis = c->anim_active ? c->anim_cur : c->geom;

		a->x = c->scene->node.x + (gint)c->bw;
		a->y = c->scene->node.y + (gint)c->bw;
		a->w = vis.width - 2 * (gint)c->bw;
		a->h = vis.height - 2 * (gint)c->bw;
	}
	if (a->w <= 0 || a->h <= 0) {
		close_anim_free(a);
		return;
	}

	a->mon = c->mon;
	a->start_us = g_get_monotonic_time();
	a->dur_us = (gint64)duration * 1000;

	wlr_scene_node_set_position(&a->node->node, a->x, a->y);
	wlr_scene_buffer_set_dest_size(a->node, a->w, a->h);
	/* On top of its siblings: the window was the thing being looked
	 * at, and having it slide behind whatever is re-tiling into its
	 * place looks like a glitch rather than a dismissal. */
	wlr_scene_node_raise_to_top(&a->node->node);

	self->close_anims = g_list_prepend(self->close_anims, a);
}

void
gowl_animation_close_finish_all(GowlCompositor *self)
{
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	g_list_free_full(self->close_anims, (GDestroyNotify)close_anim_free);
	self->close_anims = NULL;
}

void
gowl_animation_close_forget_monitor(GowlCompositor *self, GowlMonitor *m)
{
	GList *l;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	/*
	 * An output going away takes its snapshots with it rather than
	 * leaving them pointing at freed memory.  They are a decoration
	 * measured in milliseconds; ending them early on a screen that no
	 * longer exists costs nothing.
	 */
	l = self->close_anims;
	while (l != NULL) {
		GowlCloseAnim *a = (GowlCloseAnim *)l->data;
		GList *next = l->next;

		if (a->mon == m) {
			self->close_anims =
				g_list_delete_link(self->close_anims, l);
			close_anim_free(a);
		}
		l = next;
	}
}

/*
 * Advances every closing window.  Returns TRUE while one is still
 * fading on @m.
 */
static gboolean
close_tick(GowlCompositor *self, GowlMonitor *m, gint64 now_us,
           const gchar *curve)
{
	gboolean live = FALSE;
	GList *l;

	l = self->close_anims;
	while (l != NULL) {
		GowlCloseAnim *a = (GowlCloseAnim *)l->data;
		GList *next = l->next;
		gdouble t, e, scale;
		gint w, h;

		t = a->dur_us > 0
			? (gdouble)(now_us - a->start_us) / (gdouble)a->dur_us
			: 1.0;

		if (t >= 1.0) {
			self->close_anims =
				g_list_delete_link(self->close_anims, l);
			close_anim_free(a);
			l = next;
			continue;
		}
		if (t < 0.0)
			t = 0.0;

		/*
		 * Clamped, because an overshoot curve would drive the opacity
		 * negative and the scale past the window's own size --- a
		 * closing window briefly growing is not the effect anyone
		 * configured an overshoot for.
		 */
		e = gowl_curve_eval(curve, t);
		if (e < 0.0) e = 0.0;
		if (e > 1.0) e = 1.0;

		wlr_scene_buffer_set_opacity(a->node, (float)(1.0 - e));

		scale = 1.0 - e * GOWL_ANIM_CLOSE_SHRINK;
		w = (gint)lround((gdouble)a->w * scale);
		h = (gint)lround((gdouble)a->h * scale);
		if (w < 1) w = 1;
		if (h < 1) h = 1;
		wlr_scene_buffer_set_dest_size(a->node, w, h);

		/* Shrink about the centre, and sink a little. */
		wlr_scene_node_set_position(
			&a->node->node,
			a->x + (a->w - w) / 2,
			a->y + (a->h - h) / 2
				+ (gint)lround(e * (gdouble)GOWL_ANIM_CLOSE_SINK));

		if (m == NULL || a->mon == NULL || a->mon == m)
			live = TRUE;

		l = next;
	}

	return live;
}

/* Interpolate a whole rect, so position and size arrive together. */
static struct wlr_box
lerp_box(const struct wlr_box *a, const struct wlr_box *b, gdouble e)
{
	struct wlr_box out;

	out.x = a->x + (gint)lround((gdouble)(b->x - a->x) * e);
	out.y = a->y + (gint)lround((gdouble)(b->y - a->y) * e);
	out.width = a->width
		+ (gint)lround((gdouble)(b->width - a->width) * e);
	out.height = a->height
		+ (gint)lround((gdouble)(b->height - a->height) * e);

	/* A zero or negative extent is not a window; wlr_scene_rect and
	 * wlr_scene_buffer both reject it. */
	out.width = MAX(1, out.width);
	out.height = MAX(1, out.height);

	return out;
}

/*
 * Puts the window on screen at @box: the container where the rect says,
 * the borders sized to it, and the frozen snapshot stretched to fill
 * what is inside them.  Everything the eye reads as "the window" moves
 * from one call.
 */
static void
geometry_apply(GowlCompositor *self, GowlClient *c,
               const struct wlr_box *box)
{
	wlr_scene_node_set_position(&c->scene->node, box->x, box->y);
	gowl_compositor_apply_frame_geometry(self, c, box->width, box->height);

	if (c->anim_ghost != NULL)
		wlr_scene_buffer_set_dest_size(
			c->anim_ghost,
			MAX(1, box->width - 2 * (gint)c->bw),
			MAX(1, box->height - 2 * (gint)c->bw));
}

/*
 * Lands the window on its final rect and hands it back to the real
 * surface, which has had the whole animation to render at that size.
 */
static void
geometry_finish(GowlCompositor *self, GowlClient *c)
{
	c->anim_active = FALSE;
	c->anim_cur = c->anim_to;

	geometry_apply(self, c, &c->anim_to);
	ghost_release(c);
}

/* ── Driving ─────────────────────────────────────────────────────── */

/*
 * Freezes how the client looks right now, so the animation has
 * something it can resize.  Returns FALSE when there is nothing to
 * freeze, in which case the caller falls back to moving without
 * resizing.
 */
static gboolean
ghost_capture(GowlClient *c)
{
	struct wlr_surface *surface;

	/*
	 * Never for an embedded client.  Under `emacs --gowl' that is the
	 * editor's own frame, and the snapshot works by HIDING the real
	 * surface for the duration --- a mechanism whose failure mode is
	 * an invisible editor, on the one client whose embedding is
	 * managed outside the compositor and which a headless test cannot
	 * render to check.  It takes the instant path instead: its size
	 * changes without animating, which is what every window did before
	 * this existed.
	 */
	if (c->isembedded)
		return FALSE;

	surface = gowl_client_get_wlr_surface(c);
	if (surface == NULL || surface->buffer == NULL)
		return FALSE;

	c->anim_ghost_buffer = wlr_buffer_lock(&surface->buffer->base);
	c->anim_ghost = wlr_scene_buffer_create(c->scene,
	                                        c->anim_ghost_buffer);
	if (c->anim_ghost == NULL) {
		g_clear_pointer(&c->anim_ghost_buffer, wlr_buffer_unlock);
		return FALSE;
	}

	/* Inside the client's own tree, so it inherits the animated
	 * container position and only its size has to be driven. */
	wlr_scene_node_set_position(&c->anim_ghost->node, (gint)c->bw,
	                            (gint)c->bw);
	wlr_scene_node_raise_to_top(&c->anim_ghost->node);

	/* The real surface is about to be configured to its final size.
	 * Hide it until the ghost has finished morphing into that size,
	 * so the two are never on screen disagreeing about how big the
	 * window is. */
	if (c->scene_surface != NULL)
		wlr_scene_node_set_enabled(&c->scene_surface->node, FALSE);

	return TRUE;
}

static void
ghost_release(GowlClient *c)
{
	if (c->anim_ghost != NULL) {
		wlr_scene_node_destroy(&c->anim_ghost->node);
		c->anim_ghost = NULL;
	}
	g_clear_pointer(&c->anim_ghost_buffer, wlr_buffer_unlock);

	if (c->scene_surface != NULL)
		wlr_scene_node_set_enabled(&c->scene_surface->node, TRUE);
}

void
gowl_animation_start(GowlCompositor *self, GowlClient *c,
                      const struct wlr_box *from, const struct wlr_box *to)
{
	struct wlr_box start;
	gint duration;
	gboolean moved, resized;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(c != NULL);
	g_return_if_fail(from != NULL && to != NULL);

	if (!gowl_animation_enabled(self) || c->scene == NULL)
		return;

	start = *from;

	moved = start.x != to->x || start.y != to->y;
	resized = start.width != to->width || start.height != to->height;

	/* Nothing to animate.  Checked here rather than at every call
	 * site, because a layout re-runs on every arrange and most clients
	 * do not move on most of them. */
	if (!moved && !resized) {
		gowl_animation_cancel(c);
		return;
	}

	duration = gowl_config_get_animation_duration(self->config);
	if (duration <= 0) {
		gowl_animation_cancel(c);
		return;
	}

	/* Retarget from wherever the window actually is, not from the
	 * caller's idea of "before".  A second layout change mid-flight
	 * should bend the path, not teleport the window back to restart
	 * it. */
	if (c->anim_active)
		start = c->anim_cur;

	/*
	 * A resize needs a snapshot to stretch; a pure move does not, and
	 * skipping it there is worth doing --- it is the common case for a
	 * window being pushed along a stack, it costs no locked buffer,
	 * and it keeps the window's live content on screen instead of
	 * freezing it.
	 */
	if (resized && c->anim_ghost == NULL && !ghost_capture(c)) {
		/*
		 * Nothing to stretch --- the client has not drawn yet.
		 * Animating the size anyway would move the borders while the
		 * live surface sat at its final size inside them, which is
		 * precisely the mismatch this animation exists to remove.
		 * Take the instant path instead.
		 */
		gowl_animation_cancel(c);
		return;
	}

	c->anim_active = TRUE;
	c->anim_from = start;
	c->anim_to = *to;
	c->anim_cur = start;
	c->anim_start_us = g_get_monotonic_time();
	c->anim_dur_us = (gint64)duration * 1000;

	/* Hold everything at the start rect; the frame loop walks it. */
	wlr_scene_node_set_position(&c->scene->node, start.x, start.y);
	gowl_compositor_apply_frame_geometry(self, c, start.width,
	                                     start.height);
	if (c->anim_ghost != NULL)
		wlr_scene_buffer_set_dest_size(
			c->anim_ghost,
			MAX(1, start.width - 2 * (gint)c->bw),
			MAX(1, start.height - 2 * (gint)c->bw));
}

void
gowl_animation_cancel(GowlClient *c)
{
	if (c == NULL)
		return;

	c->anim_active = FALSE;
	/* The snapshot holds a client buffer and hides the real surface;
	 * leaving either behind is a window that never comes back. */
	ghost_release(c);
}

/*
 * Whether a client counts towards the asking monitor's liveness.  Every
 * client is still advanced on every tick --- the maths is time-based
 * and idempotent, so two monitors asking gives one answer --- but only
 * the ones actually on this output decide whether it keeps redrawing.
 * A NULL monitor means "anywhere", which is what a test wants.
 */
static gboolean
client_on(GowlClient *c, GowlMonitor *m)
{
	return m == NULL || c->mon == m;
}

gboolean
gowl_animation_tick(GowlCompositor *self, GowlMonitor *m, gint64 now_us)
{
	GList *l;
	gboolean live = FALSE;
	const gchar *curve;

	/* The frame handler runs before the compositor is fully torn down,
	 * so a NULL here is a shutdown race rather than a caller bug. */
	if (self == NULL)
		return FALSE;
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);

	curve = self->config != NULL
		? gowl_config_get_animation_curve(self->config)
		: NULL;

	if (close_tick(self, m, now_us, curve))
		live = TRUE;

	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;
		gdouble t, e;

		if (c->scene == NULL)
			continue;

		/* Opening runs alongside a move rather than instead of it: a
		 * window can be re-tiled while it is still fading in. */
		if (open_tick(self, c, now_us, curve) && client_on(c, m))
			live = TRUE;

		if (!c->anim_active)
			continue;

		if (c->anim_dur_us <= 0) {
			geometry_finish(self, c);
			continue;
		}

		t = (gdouble)(now_us - c->anim_start_us) / (gdouble)c->anim_dur_us;
		if (t >= 1.0) {
			geometry_finish(self, c);
			continue;
		}
		if (t < 0.0)
			t = 0.0;

		e = gowl_curve_eval(curve, t);
		c->anim_cur = lerp_box(&c->anim_from, &c->anim_to, e);
		geometry_apply(self, c, &c->anim_cur);

		if (client_on(c, m))
			live = TRUE;
	}

	return live;
}
