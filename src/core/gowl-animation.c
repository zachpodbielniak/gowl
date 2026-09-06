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

/* Geometry is a compositor-side transform of a snapshot of the client's
 * surface tree. The client receives only the final configure. Entrance,
 * exit and layout motion have separate timing; opacity never overshoots. */

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
	 * settles quickly enough to leave time for the jiggle rather than a
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

static gdouble
jiggle_strength(GowlCompositor *self, GowlClient *c)
{
	if (c->isembedded || c->isfullscreen || self->config == NULL)
		return 0.0;
	return gowl_config_get_animation_jiggle_strength(self->config);
}

/* A bounded impulse, independent of travel distance on a large monitor.
 * Squash along one axis while stretching the other. The snapshot and
 * frame share this rectangle, including hit-testing through the snapshot. */
static void
jiggle_configure(GowlCompositor *self, GowlClient *c,
                 const struct wlr_box *from, const struct wlr_box *to)
{
	gdouble dx = (gdouble)to->x - from->x;
	gdouble dy = (gdouble)to->y - from->y;
	gdouble dw = (gdouble)to->width - from->width;
	gdouble dh = (gdouble)to->height - from->height;
	gdouble strength = jiggle_strength(self, c);
	gdouble amount = MIN(1.0, (fabs(dx) + fabs(dy) + fabs(dw) + fabs(dh)) / 100.0);
	gboolean horizontal = fabs(dx) + fabs(dw) >= fabs(dy) + fabs(dh);

	c->anim_jiggle[0] = CLAMP(dx * 0.08, -12.0, 12.0) * strength;
	c->anim_jiggle[1] = CLAMP(dy * 0.08, -12.0, 12.0) * strength;
	c->anim_jiggle[2] = c->anim_ghost != NULL
		? MIN(24.0, to->width * 0.04) * amount * strength * (horizontal ? 1.0 : -0.7)
		: 0.0;
	c->anim_jiggle[3] = c->anim_ghost != NULL
		? MIN(24.0, to->height * 0.04) * amount * strength * (horizontal ? -0.7 : 1.0)
		: 0.0;
}

static void
jiggle_apply(struct wlr_box *box, const gdouble impulse[4],
             gdouble t, gint cycles, guint bw)
{
	gdouble wave;
	gint dw, dh;

	/* Exact endpoints and a decaying envelope: no residual displacement
	 * or perpetual timer once the window has settled. */
	if (t <= 0.0 || t >= 1.0
	    || (impulse[0] == 0.0 && impulse[1] == 0.0
	        && impulse[2] == 0.0 && impulse[3] == 0.0))
		return;
	wave = sin(2.0 * G_PI * cycles * t) * (1.0 - t) * (1.0 - t);
	dw = MAX(1 + 2 * (gint)bw - box->width, (gint)lround(impulse[2] * wave));
	dh = MAX(1 + 2 * (gint)bw - box->height, (gint)lround(impulse[3] * wave));
	box->x += (gint)lround(impulse[0] * wave) - dw / 2;
	box->y += (gint)lround(impulse[1] * wave) - dh / 2;
	box->width += dw;
	box->height += dh;
}

/* The half both entrances share. */
static void
fade_in_start(GowlCompositor *self, GowlClient *c)
{
	gint duration;

	if (!gowl_animation_enabled(self) || c->scene == NULL)
		return;

	duration = open_duration(self);
	if (duration <= 0)
		return;

	c->anim_opening = TRUE;
	c->anim_open_start_us = g_get_monotonic_time();
	c->anim_open_dur_us = (gint64)MIN(duration, 120) * 1000;

	/* Start invisible.  Set here rather than left to the first tick so
	 * that the frame between mapping and the first tick does not show
	 * the window at full opacity --- one frame of pop is still a pop. */
	gowl_client_set_anim_alpha(c, 0.0f);
}

void
gowl_animation_open_start(GowlCompositor *self, GowlClient *c)
{
	struct wlr_box from;
	gdouble scale;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(c != NULL);

	if (c->isembedded || open_duration(self) <= 0)
		return;

	/* A centered pop has no residual rise to reappear after the
	 * geometry finishes. Its clock belongs to the opening gesture. */
	scale = gowl_config_get_animation_popin_scale(self->config);
	from = c->geom;
	from.width = MAX(1, (gint)lround(from.width * scale));
	from.height = MAX(1, (gint)lround(from.height * scale));
	from.x += (c->geom.width - from.width) / 2;
	from.y += (c->geom.height - from.height) / 2;
	gowl_animation_start(self, c, &from, &c->geom);
	if (c->anim_active) {
		c->anim_pop = TRUE;
		c->anim_dur_us = (gint64)open_duration(self) * 1000;
		/* The entrance wobbles around its centre, without drifting. */
		c->anim_jiggle[0] = c->anim_jiggle[1] = 0.0;
		c->anim_jiggle[2] = MIN(24.0, c->geom.width * 0.04) * jiggle_strength(self, c);
		c->anim_jiggle[3] = -MIN(24.0, c->geom.height * 0.04) * jiggle_strength(self, c);
	}
	fade_in_start(self, c);
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

	fade_in_start(self, c);
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
open_tick(GowlClient *c, gint64 now_us)
{
	gdouble t;

	if (!c->anim_opening)
		return FALSE;
	t = c->anim_open_dur_us > 0
		? (gdouble)(now_us - c->anim_open_start_us) / c->anim_open_dur_us
		: 1.0;
	if (t >= 1.0) {
		gowl_animation_open_cancel(c);
		return FALSE;
	}
	gowl_client_set_anim_alpha(c,
		(gfloat)gowl_curve_eval("almost-linear", t));
	return TRUE;
}

/* Defined below, next to the geometry animation they belong to. */
static void ghost_release (GowlClient *c);

/* ── Closing ─────────────────────────────────────────────────────── */

/* A closing snapshot owns its buffers independently of the client. */
struct _GowlCloseAnim {
	GowlSceneSnapshot *snapshot;
	GowlMonitor *mon;
	gint64 start_us, dur_us;
	gint x, y, w, h;
	gfloat alpha;
	gdouble target_scale;
	gdouble jiggle[4];
};

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

	gowl_scene_snapshot_free(a->snapshot);

	g_free(a);
}

static void
surface_size(GowlClient *c, gint *width, gint *height)
{
	struct wlr_surface *surface = gowl_client_get_wlr_surface(c);

	*width = c->geom.width - 2 * (gint)c->bw;
	*height = c->geom.height - 2 * (gint)c->bw;
	if (c->xdg_toplevel != NULL) {
		*width = c->xdg_toplevel->base->geometry.width;
		*height = c->xdg_toplevel->base->geometry.height;
	} else if (surface != NULL) {
		*width = surface->current.width;
		*height = surface->current.height;
	}
}

struct wlr_surface *
gowl_animation_surface_at(GowlClient *c, gdouble x, gdouble y,
                          gdouble *sx, gdouble *sy)
{
	struct wlr_surface *surface;
	gint cx, cy, width, height;
	gdouble local_x, local_y;

	if (c->anim_ghost == NULL || c->scene == NULL
	    || !wlr_scene_node_coords(&c->scene->node, &cx, &cy))
		return NULL;
	surface = gowl_client_get_wlr_surface(c);
	if (surface == NULL)
		return NULL;
	surface_size(c, &width, &height);
	local_x = (x - cx - c->bw) * width
		/ MAX(1, c->anim_cur.width - 2 * (gint)c->bw);
	local_y = (y - cy - c->bw) * height
		/ MAX(1, c->anim_cur.height - 2 * (gint)c->bw);
	if (c->xdg_toplevel != NULL) {
		local_x += c->xdg_toplevel->base->geometry.x;
		local_y += c->xdg_toplevel->base->geometry.y;
		return wlr_xdg_surface_surface_at(c->xdg_toplevel->base,
		                                  local_x, local_y, sx, sy);
	}
	return wlr_surface_surface_at(surface, local_x, local_y, sx, sy);
}

static bool
close_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	return false;
}

static void
close_disable_input(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	buffer->point_accepts_input = close_accepts_input;
}

void
gowl_animation_close_start(GowlCompositor *self, GowlClient *c)
{
	GowlCloseAnim *a;
	struct wlr_scene_tree *parent;
	struct wlr_box vis;
	gint duration, width, height, x, y;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(c != NULL);

	if (!gowl_animation_enabled(self) || c->scene == NULL
	    || !c->anim_placed || c->isembedded
	    || !wlr_scene_node_coords(&c->scene->node, &x, &y))
		return;
	duration = close_duration(self);
	if (duration <= 0)
		return;

	/* Above retiling neighbours, but still below panels and the lock.
	 * Fullscreen ghosts stay in their original fullscreen layer. */
	parent = self->layers[c->isfullscreen ? GOWL_SCENE_LAYER_FS
	                                    : GOWL_SCENE_LAYER_FLOAT];
	if (parent == NULL)
		return;

	vis = c->anim_active ? c->anim_cur : c->geom;
	a = g_new0(GowlCloseAnim, 1);
	a->w = vis.width - 2 * (gint)c->bw;
	a->h = vis.height - 2 * (gint)c->bw;
	if (c->anim_ghost != NULL) {
		/* Transfer exactly the picture currently on screen, including
		 * a partially completed opening. No flash to a newer buffer. */
		a->snapshot = c->anim_ghost;
		c->anim_ghost = NULL;
		wlr_scene_node_reparent(&a->snapshot->tree->node, parent);
	} else {
		surface_size(c, &width, &height);
		a->snapshot = gowl_scene_snapshot_new(parent, c->scene_surface,
		                                       width, height);
	}
	if (a->snapshot == NULL || a->w <= 0 || a->h <= 0) {
		close_anim_free(a);
		return;
	}

	/* Convert layout coordinates to the chosen layer's coordinates. */
	wlr_scene_node_coords(&parent->node, &width, &height);
	a->x = x - width + (gint)c->bw;
	a->y = y - height + (gint)c->bw;
	a->mon = c->mon;
	a->start_us = g_get_monotonic_time();
	a->dur_us = (gint64)duration * 1000;
	a->alpha = c->alpha * c->anim_alpha;
	a->target_scale = gowl_config_get_animation_popin_scale(self->config);
	a->jiggle[2] = MIN(32.0, a->w * 0.06) * jiggle_strength(self, c);
	a->jiggle[3] = -MIN(32.0, a->h * 0.06) * jiggle_strength(self, c);
	gowl_scene_snapshot_set_opacity(a->snapshot, a->alpha);
	gowl_scene_snapshot_resize(a->snapshot, a->w, a->h);
	wlr_scene_node_set_position(&a->snapshot->tree->node, a->x, a->y);
	wlr_scene_node_raise_to_top(&a->snapshot->tree->node);
	/* A departing picture must not intercept clicks on surviving floaters. */
	wlr_scene_node_for_each_buffer(&a->snapshot->tree->node,
	                                close_disable_input, NULL);
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
close_tick(GowlCompositor *self, GowlMonitor *m, gint64 now_us)
{
	gboolean live = FALSE;
	GList *l;

	l = self->close_anims;
	while (l != NULL) {
		GowlCloseAnim *a = (GowlCloseAnim *)l->data;
		GList *next = l->next;
		gdouble t, e, scale;
		gint w, h;
		struct wlr_box box;

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

		/* A linear shrink remains visible through the exit. Reusing
		 * the move's ease-out made it almost disappear in one frame. */
		e = gowl_curve_eval("almost-linear", t);
		gowl_scene_snapshot_set_opacity(a->snapshot, a->alpha * (1.0 - e));
		scale = 1.0 - t * (1.0 - a->target_scale);
		w = MAX(1, (gint)lround(a->w * scale));
		h = MAX(1, (gint)lround(a->h * scale));
		box = (struct wlr_box){ a->x + (a->w - w) / 2,
		                         a->y + (a->h - h) / 2, w, h };
		jiggle_apply(&box, a->jiggle, t, 1, 0);
		gowl_scene_snapshot_resize(a->snapshot, box.width, box.height);
		wlr_scene_node_set_position(&a->snapshot->tree->node,
		                            box.x, box.y);

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
		gowl_scene_snapshot_resize(
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
	gint width, height;

	if (c->isembedded || c->scene_surface == NULL)
		return FALSE;
	surface_size(c, &width, &height);
	c->anim_ghost = gowl_scene_snapshot_new(c->scene, c->scene_surface,
	                                        width, height);
	if (c->anim_ghost == NULL)
		return FALSE;
	wlr_scene_node_set_position(&c->anim_ghost->tree->node, c->bw, c->bw);
	gowl_scene_snapshot_set_opacity(c->anim_ghost, c->alpha * c->anim_alpha);
	wlr_scene_node_set_enabled(&c->scene_surface->node, FALSE);
	return TRUE;
}

static void
ghost_release(GowlClient *c)
{
	g_clear_pointer(&c->anim_ghost, gowl_scene_snapshot_free);
	if (c->scene_surface != NULL) {
		wlr_scene_node_set_enabled(&c->scene_surface->node, TRUE);
		/* The live tree was disabled during the fade and could not be
		 * reached by wlroots' visible-buffer iterator. */
		gowl_client_set_anim_alpha(c, c->anim_alpha);
	}
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

	/* Arrange and configure acknowledgements may repeat a destination.
	 * Keeping the original clock prevents a held key or busy client
	 * from indefinitely extending the settling tail. */
	if (c->anim_active && wlr_box_equal(&c->anim_to, to))
		return;
	start = c->anim_active ? c->anim_cur : *from;

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

	/* Resizing and squash/stretch both need a snapshot. With jiggle
	 * disabled a pure move keeps the live surface instead. */
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
	if (!resized && jiggle_strength(self, c) > 0.0 && c->anim_ghost == NULL)
		ghost_capture(c); /* No buffer: jiggle_configure keeps the size fixed. */

	c->anim_active = TRUE;
	c->anim_pop = FALSE;
	c->anim_from = start;
	c->anim_to = *to;
	c->anim_cur = start;
	c->anim_start_us = g_get_monotonic_time();
	c->anim_dur_us = (gint64)duration * 1000;
	jiggle_configure(self, c, &start, to);

	/* Hold everything at the start rect; the frame loop walks it. */
	wlr_scene_node_set_position(&c->scene->node, start.x, start.y);
	gowl_compositor_apply_frame_geometry(self, c, start.width,
	                                     start.height);
	if (c->anim_ghost != NULL)
		gowl_scene_snapshot_resize(
			c->anim_ghost,
			MAX(1, start.width - 2 * (gint)c->bw),
			MAX(1, start.height - 2 * (gint)c->bw));
}

void
gowl_animation_settle(GowlCompositor *self, GowlClient *c,
                       const struct wlr_box *grab)
{
	gint x, y;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(c != NULL && grab != NULL);

	if (!gowl_animation_enabled(self) || jiggle_strength(self, c) <= 0.0
	    || c->scene == NULL || !c->anim_placed || c->anim_active
	    || wlr_box_equal(grab, &c->geom)
	    || !wlr_scene_node_coords(&c->scene->node, &x, &y))
		return;
	if (!ghost_capture(c))
		return;

	/* The pointer already placed the window. Only the visual impulse
	 * rings down; no client configure or layout change accompanies it. */
	c->anim_from = c->anim_to = c->anim_cur = c->geom;
	c->anim_active = TRUE;
	c->anim_pop = FALSE;
	c->anim_start_us = g_get_monotonic_time();
	c->anim_dur_us = (gint64)gowl_config_get_animation_duration(self->config) * 1000;
	jiggle_configure(self, c, grab, &c->geom);
	geometry_apply(self, c, &c->geom);
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

	if (close_tick(self, m, now_us))
		live = TRUE;

	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;
		gdouble t, e;

		if (c->scene == NULL)
			continue;

		/* Opening runs alongside a move rather than instead of it: a
		 * window can be re-tiled while it is still fading in. */
		if (open_tick(c, now_us) && client_on(c, m))
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

		e = gowl_curve_eval(c->anim_pop
			? gowl_config_get_animation_curve_open(self->config) : curve, t);
		c->anim_cur = lerp_box(&c->anim_from, &c->anim_to, e);
		jiggle_apply(&c->anim_cur, c->anim_jiggle, t, 2, c->bw);
		geometry_apply(self, c, &c->anim_cur);

		if (client_on(c, m))
			live = TRUE;
	}

	return live;
}
