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
 * GowlInputCapture -- the compositor-side, D-Bus-free state machine behind
 * the InputCapture portal.  It owns the set of capturable zones (derived
 * from monitor geometry), the installed pointer barriers, and the
 * enabled/active state, and contains the pure geometry logic that decides
 * when a pointer-motion segment crosses a barrier.  When active it diverts
 * input events to a registered sink instead of the focused client.
 *
 * It deliberately links no wlroots, no libeis, and no D-Bus: the
 * compositor feeds it cursor motion and input events through plain C
 * calls, and the portal binary drives it over the private Wayland
 * protocol.  This keeps all of it unit-testable with no compositor.
 */

#include <gio/gio.h>

#include "gowl-input-capture.h"

struct _GowlInputCapture {
	GObject parent_instance;

	GList  *zones;          /* owned list of GowlInputZone*           */
	guint   zone_set;       /* bumped whenever zones change           */

	GList  *barriers;       /* owned list of GowlInputBarrier*        */

	gboolean enabled;       /* portal Enable/Disable                  */
	gboolean active;        /* currently capturing (barrier crossed)  */
	guint32  activation_id; /* monotonic, last activation             */

	GowlInputSinkFunc       sink;
	gpointer                sink_data;

	GowlInputActivationFunc activation_cb;
	gpointer                activation_data;
};

G_DEFINE_TYPE(GowlInputCapture, gowl_input_capture, G_TYPE_OBJECT)

static void
gowl_input_capture_finalize(GObject *object)
{
	GowlInputCapture *self = GOWL_INPUT_CAPTURE(object);

	g_list_free_full(self->zones, (GDestroyNotify)gowl_input_zone_free);
	g_list_free_full(self->barriers, (GDestroyNotify)gowl_input_barrier_free);

	G_OBJECT_CLASS(gowl_input_capture_parent_class)->finalize(object);
}

static void
gowl_input_capture_class_init(GowlInputCaptureClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = gowl_input_capture_finalize;
}

static void
gowl_input_capture_init(GowlInputCapture *self)
{
	self->zones         = NULL;
	self->zone_set      = 0;
	self->barriers      = NULL;
	self->enabled       = FALSE;
	self->active        = FALSE;
	self->activation_id = 0;
	self->sink          = NULL;
	self->sink_data     = NULL;
	self->activation_cb = NULL;
	self->activation_data = NULL;
}

/**
 * gowl_input_capture_new:
 *
 * Creates a new #GowlInputCapture with no zones, no barriers, disabled.
 *
 * Returns: (transfer full): a new #GowlInputCapture.
 */
GowlInputCapture *
gowl_input_capture_new(void)
{
	return (GowlInputCapture *)g_object_new(GOWL_TYPE_INPUT_CAPTURE, NULL);
}

/**
 * gowl_input_capture_get_capabilities:
 * @self: a #GowlInputCapture
 *
 * Returns the kinds of input this machine can divert.  gowl drives both
 * the pointer and keyboard pipelines, so this is always
 * KEYBOARD | POINTER (TOUCHSCREEN is not supported).
 *
 * Returns: the #GowlInputCaptureCapability bitmask.
 */
GowlInputCaptureCapability
gowl_input_capture_get_capabilities(GowlInputCapture *self)
{
	g_return_val_if_fail(GOWL_IS_INPUT_CAPTURE(self),
	                     GOWL_INPUT_CAPTURE_CAP_NONE);

	return GOWL_INPUT_CAPTURE_CAP_KEYBOARD | GOWL_INPUT_CAPTURE_CAP_POINTER;
}

/**
 * gowl_input_capture_set_zones:
 * @self: a #GowlInputCapture
 * @zones: (element-type GowlInputZone) (transfer none): the new zone list
 *
 * Replaces the capturable zones with a deep copy of @zones and bumps the
 * zone_set counter.  Setting zones clears any installed barriers, since a
 * barrier validated against the old layout may no longer lie on an edge.
 */
void
gowl_input_capture_set_zones(GowlInputCapture *self, GList *zones)
{
	GList *l;

	g_return_if_fail(GOWL_IS_INPUT_CAPTURE(self));

	g_list_free_full(self->zones, (GDestroyNotify)gowl_input_zone_free);
	self->zones = NULL;
	for (l = zones; l != NULL; l = l->next)
		self->zones = g_list_prepend(self->zones,
			gowl_input_zone_copy((const GowlInputZone *)l->data));
	self->zones = g_list_reverse(self->zones);

	/* Barriers are layout-relative; invalidate them on a layout change. */
	g_list_free_full(self->barriers,
	                 (GDestroyNotify)gowl_input_barrier_free);
	self->barriers = NULL;

	self->zone_set++;
}

/**
 * gowl_input_capture_get_zones:
 * @self: a #GowlInputCapture
 *
 * Returns the current zones.
 *
 * Returns: (element-type GowlInputZone) (transfer full): a deep copy of
 *          the zone list; free with
 *          g_list_free_full(list, (GDestroyNotify)gowl_input_zone_free).
 */
GList *
gowl_input_capture_get_zones(GowlInputCapture *self)
{
	GList *out = NULL, *l;

	g_return_val_if_fail(GOWL_IS_INPUT_CAPTURE(self), NULL);

	for (l = self->zones; l != NULL; l = l->next)
		out = g_list_prepend(out,
			gowl_input_zone_copy((const GowlInputZone *)l->data));
	return g_list_reverse(out);
}

/**
 * gowl_input_capture_get_zone_set:
 * @self: a #GowlInputCapture
 *
 * Returns the current zone_set id, which increments each time the zones
 * change.  Reported to the portal so a client can detect stale barriers.
 *
 * Returns: the zone_set counter.
 */
guint
gowl_input_capture_get_zone_set(GowlInputCapture *self)
{
	g_return_val_if_fail(GOWL_IS_INPUT_CAPTURE(self), 0);

	return self->zone_set;
}

/* ------------------------------------------------------------------ *
 * Barrier validation
 * ------------------------------------------------------------------ */

/* TRUE if [a,b] (a <= b assumed by caller) covers [c,d] fully. */
static gboolean
range_covers(gint a, gint b, gint c, gint d)
{
	return a <= c && d <= b;
}

/*
 * A barrier is valid when it is axis-aligned (classify != INVALID) and it
 * lies exactly on the OUTER boundary of the zone union while being fully
 * contained within a single zone's edge span.  "Outer boundary" means the
 * barrier sits on one of a zone's four edges AND no other zone abuts that
 * edge from the outside (otherwise it would be interior to the union, and
 * crossing it would just move between two captured monitors).
 *
 * We implement this directly against the zone rectangles: for a vertical
 * barrier at x == X spanning [y1,y2], it must coincide with the left or
 * right edge of some zone Z, lie within Z's y-span, and there must be no
 * zone on the far side of that edge overlapping the barrier's y-range.
 * Horizontal barriers are the transpose.
 */

static gboolean
zone_on_far_side_vertical(GList *zones, const GowlInputZone *owner,
                          gint edge_x, gboolean owner_is_left_edge,
                          gint y_lo, gint y_hi)
{
	GList *l;

	for (l = zones; l != NULL; l = l->next) {
		const GowlInputZone *z = (const GowlInputZone *)l->data;
		gint zx, zr, zy, zb;

		if (z == owner)
			continue;
		zx = z->x;
		/*
		 * Fix: CWE-190 Integer Overflow — z->width and z->height are
		 * guint.  Casting a guint value > G_MAXINT to gint produces a
		 * negative number, silently corrupting the right/bottom edge
		 * comparison and making otherwise-invalid barriers pass
		 * validation.  Clamp to G_MAXINT before the cast; values that
		 * large cannot correspond to any real monitor, so the clamp is
		 * safe and the zone simply has no matching edge in that
		 * direction.
		 */
		zr = z->x + (gint)MIN(z->width,  (guint)G_MAXINT);
		zy = z->y;
		zb = z->y + (gint)MIN(z->height, (guint)G_MAXINT);

		/* No vertical overlap with the barrier span -> not relevant. */
		if (zb <= y_lo || zy >= y_hi)
			continue;

		if (owner_is_left_edge) {
			/* Far side is to the LEFT of edge_x: a neighbour whose
			 * right edge touches edge_x sits outside. */
			if (zr == edge_x)
				return TRUE;
		} else {
			/* Owner's right edge; far side is to the RIGHT. */
			if (zx == edge_x)
				return TRUE;
		}
	}
	return FALSE;
}

static gboolean
zone_on_far_side_horizontal(GList *zones, const GowlInputZone *owner,
                            gint edge_y, gboolean owner_is_top_edge,
                            gint x_lo, gint x_hi)
{
	GList *l;

	for (l = zones; l != NULL; l = l->next) {
		const GowlInputZone *z = (const GowlInputZone *)l->data;
		gint zx, zr, zy, zb;

		if (z == owner)
			continue;
		zx = z->x;
		/* Fix: CWE-190 — clamp guint dimension before cast, same as
		 * zone_on_far_side_vertical above. */
		zr = z->x + (gint)MIN(z->width,  (guint)G_MAXINT);
		zy = z->y;
		zb = z->y + (gint)MIN(z->height, (guint)G_MAXINT);

		if (zr <= x_lo || zx >= x_hi)
			continue;

		if (owner_is_top_edge) {
			if (zb == edge_y)
				return TRUE;
		} else {
			if (zy == edge_y)
				return TRUE;
		}
	}
	return FALSE;
}

static gboolean
barrier_is_valid(GList *zones, const GowlInputBarrier *b)
{
	GowlInputBarrierOrientation o = gowl_input_barrier_classify(b);
	GList *l;

	if (o == GOWL_INPUT_BARRIER_INVALID)
		return FALSE;
	if (zones == NULL)
		return FALSE;

	if (o == GOWL_INPUT_BARRIER_VERTICAL) {
		gint x = b->x1;
		gint y_lo = MIN(b->y1, b->y2);
		gint y_hi = MAX(b->y1, b->y2);

		for (l = zones; l != NULL; l = l->next) {
			const GowlInputZone *z = (const GowlInputZone *)l->data;
			gint zx = z->x;
			/* Fix: CWE-190 — clamp guint dimension before cast, same
			 * as zone_on_far_side_vertical. */
			gint zr = z->x + (gint)MIN(z->width,  (guint)G_MAXINT);
			gint zy = z->y;
			gint zb = z->y + (gint)MIN(z->height, (guint)G_MAXINT);

			if (!range_covers(zy, zb, y_lo, y_hi))
				continue;
			if (x == zx) {
				if (!zone_on_far_side_vertical(zones, z, x,
				                               TRUE, y_lo, y_hi))
					return TRUE;
			} else if (x == zr) {
				if (!zone_on_far_side_vertical(zones, z, x,
				                               FALSE, y_lo, y_hi))
					return TRUE;
			}
		}
		return FALSE;
	} else {
		gint y = b->y1;
		gint x_lo = MIN(b->x1, b->x2);
		gint x_hi = MAX(b->x1, b->x2);

		for (l = zones; l != NULL; l = l->next) {
			const GowlInputZone *z = (const GowlInputZone *)l->data;
			gint zx = z->x;
			/* Fix: CWE-190 — clamp guint dimension before cast. */
			gint zr = z->x + (gint)MIN(z->width,  (guint)G_MAXINT);
			gint zy = z->y;
			gint zb = z->y + (gint)MIN(z->height, (guint)G_MAXINT);

			if (!range_covers(zx, zr, x_lo, x_hi))
				continue;
			if (y == zy) {
				if (!zone_on_far_side_horizontal(zones, z, y,
				                                 TRUE, x_lo, x_hi))
					return TRUE;
			} else if (y == zb) {
				if (!zone_on_far_side_horizontal(zones, z, y,
				                                 FALSE, x_lo, x_hi))
					return TRUE;
			}
		}
		return FALSE;
	}
}

/**
 * gowl_input_capture_set_barriers:
 * @self: a #GowlInputCapture
 * @barriers: (element-type GowlInputBarrier) (transfer none): requested
 *   barriers
 * @accepted_ids: (out caller-allocates) (nullable) (element-type guint32):
 *   if non-%NULL, the ids of the barriers that were accepted are appended
 * @error: (nullable): return location for a #GError
 *
 * Validates and installs @barriers, replacing any previously installed
 * set.  Each barrier must be axis-aligned (non-zero id, horizontal or
 * vertical), lie on the outer boundary of the zone union, and be fully
 * contained within one zone's edge span; invalid or interior barriers are
 * rejected.  Duplicate ids keep only the first occurrence.  A barrier set
 * that contains at least one valid barrier succeeds with the accepted
 * subset; if every barrier is rejected the call fails with @error set and
 * no barriers are installed.
 *
 * Returns: %TRUE if at least one barrier was installed.
 */
gboolean
gowl_input_capture_set_barriers(
	GowlInputCapture *self,
	GList            *barriers,
	GArray           *accepted_ids,
	GError          **error
){
	GList *installed = NULL, *l;
	GHashTable *seen;
	guint n_accepted = 0;

	g_return_val_if_fail(GOWL_IS_INPUT_CAPTURE(self), FALSE);

	seen = g_hash_table_new(g_direct_hash, g_direct_equal);

	for (l = barriers; l != NULL; l = l->next) {
		const GowlInputBarrier *b = (const GowlInputBarrier *)l->data;

		/* Skip duplicate ids (keep the first). */
		if (b->id != 0
		    && g_hash_table_contains(seen, GUINT_TO_POINTER(b->id)))
			continue;

		if (!barrier_is_valid(self->zones, b))
			continue;

		g_hash_table_add(seen, GUINT_TO_POINTER(b->id));
		installed = g_list_prepend(installed, gowl_input_barrier_copy(b));
		if (accepted_ids != NULL)
			g_array_append_val(accepted_ids, b->id);
		n_accepted++;
	}

	g_hash_table_destroy(seen);

	if (n_accepted == 0) {
		g_list_free_full(installed,
		                 (GDestroyNotify)gowl_input_barrier_free);
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		                    "no valid pointer barriers");
		return FALSE;
	}

	g_list_free_full(self->barriers,
	                 (GDestroyNotify)gowl_input_barrier_free);
	self->barriers = g_list_reverse(installed);

	return TRUE;
}

/**
 * gowl_input_capture_n_barriers:
 * @self: a #GowlInputCapture
 *
 * Returns the number of currently installed barriers.
 *
 * Returns: the barrier count.
 */
guint
gowl_input_capture_n_barriers(GowlInputCapture *self)
{
	g_return_val_if_fail(GOWL_IS_INPUT_CAPTURE(self), 0);

	return g_list_length(self->barriers);
}

/* ------------------------------------------------------------------ *
 * Enable / disable / active state
 * ------------------------------------------------------------------ */

/**
 * gowl_input_capture_enable:
 * @self: a #GowlInputCapture
 *
 * Arms capture: once enabled, crossing an installed barrier activates
 * diversion.  Idempotent.
 */
void
gowl_input_capture_enable(GowlInputCapture *self)
{
	g_return_if_fail(GOWL_IS_INPUT_CAPTURE(self));

	self->enabled = TRUE;
}

/**
 * gowl_input_capture_disable:
 * @self: a #GowlInputCapture
 *
 * Disarms capture and, if currently active, deactivates it (restoring
 * local input).  Idempotent.
 */
void
gowl_input_capture_disable(GowlInputCapture *self)
{
	g_return_if_fail(GOWL_IS_INPUT_CAPTURE(self));

	if (self->active)
		gowl_input_capture_deactivate(self);
	self->enabled = FALSE;
}

/**
 * gowl_input_capture_release:
 * @self: a #GowlInputCapture
 *
 * Fully tears down a session's capture state: deactivates, disables, and
 * drops all installed barriers.  Zones are left intact (they reflect the
 * monitor layout, not session state).
 */
void
gowl_input_capture_release(GowlInputCapture *self)
{
	g_return_if_fail(GOWL_IS_INPUT_CAPTURE(self));

	if (self->active)
		gowl_input_capture_deactivate(self);
	self->enabled = FALSE;
	g_list_free_full(self->barriers,
	                 (GDestroyNotify)gowl_input_barrier_free);
	self->barriers = NULL;
}

/**
 * gowl_input_capture_is_enabled:
 * @self: a #GowlInputCapture
 *
 * Returns: %TRUE if capture is armed.
 */
gboolean
gowl_input_capture_is_enabled(GowlInputCapture *self)
{
	g_return_val_if_fail(GOWL_IS_INPUT_CAPTURE(self), FALSE);

	return self->enabled;
}

/**
 * gowl_input_capture_is_active:
 * @self: a #GowlInputCapture
 *
 * Returns: %TRUE if capture is currently diverting input.
 */
gboolean
gowl_input_capture_is_active(GowlInputCapture *self)
{
	g_return_val_if_fail(GOWL_IS_INPUT_CAPTURE(self), FALSE);

	return self->active;
}

/**
 * gowl_input_capture_set_sink:
 * @self: a #GowlInputCapture
 * @sink: (nullable) (scope notified): the diversion callback, or %NULL
 * @user_data: data passed to @sink
 *
 * Registers the callback that receives diverted input events while active.
 */
void
gowl_input_capture_set_sink(
	GowlInputCapture  *self,
	GowlInputSinkFunc  sink,
	gpointer           user_data
){
	g_return_if_fail(GOWL_IS_INPUT_CAPTURE(self));

	self->sink      = sink;
	self->sink_data = user_data;
}

/**
 * gowl_input_capture_set_activation_callback:
 * @self: a #GowlInputCapture
 * @cb: (nullable) (scope notified): the activation callback, or %NULL
 * @user_data: data passed to @cb
 *
 * Registers the callback fired on activation and deactivation.
 */
void
gowl_input_capture_set_activation_callback(
	GowlInputCapture        *self,
	GowlInputActivationFunc  cb,
	gpointer                 user_data
){
	g_return_if_fail(GOWL_IS_INPUT_CAPTURE(self));

	self->activation_cb   = cb;
	self->activation_data = user_data;
}

/* ------------------------------------------------------------------ *
 * Crossing detection
 * ------------------------------------------------------------------ */

/*
 * Does the motion segment (px,py)->(cx,cy) cross the vertical line x == bx
 * within the barrier's y-span [ylo,yhi]?  We require an actual sign change
 * in (x - bx) so that merely starting or ending on the line does not
 * count, and we interpolate the crossing y to test the span.  This is
 * robust to high-speed motion that leaps fully past the barrier in one
 * delta (segment intersection, not point-on-line).
 */
static gboolean
seg_crosses_vertical(gdouble px, gdouble py, gdouble cx, gdouble cy,
                     gint bx, gint ylo, gint yhi, gdouble *out_y)
{
	gdouble dx, t, y;

	if ((px < bx && cx < bx) || (px > bx && cx > bx))
		return FALSE;          /* both on the same side */
	if (px == cx)
		return FALSE;          /* parallel to the barrier */
	if ((px < bx) == (cx < bx))
		return FALSE;          /* no strict side change */

	dx = cx - px;
	t = ((gdouble)bx - px) / dx;
	y = py + t * (cy - py);

	if (y < ylo || y > yhi)
		return FALSE;
	if (out_y != NULL)
		*out_y = y;
	return TRUE;
}

static gboolean
seg_crosses_horizontal(gdouble px, gdouble py, gdouble cx, gdouble cy,
                       gint by, gint xlo, gint xhi, gdouble *out_x)
{
	gdouble dy, t, x;

	if ((py < by && cy < by) || (py > by && cy > by))
		return FALSE;
	if (py == cy)
		return FALSE;
	if ((py < by) == (cy < by))
		return FALSE;

	dy = cy - py;
	t = ((gdouble)by - py) / dy;
	x = px + t * (cx - px);

	if (x < xlo || x > xhi)
		return FALSE;
	if (out_x != NULL)
		*out_x = x;
	return TRUE;
}

static void
do_activate(GowlInputCapture *self, gdouble x, gdouble y, guint32 barrier_id)
{
	self->active = TRUE;
	self->activation_id++;
	if (self->activation_cb != NULL)
		self->activation_cb(self, self->activation_id, x, y,
		                    barrier_id, TRUE, self->activation_data);
}

/**
 * gowl_input_capture_check_crossing:
 * @self: a #GowlInputCapture
 * @prev_x: previous cursor x, layout-relative
 * @prev_y: previous cursor y, layout-relative
 * @cur_x: current cursor x, layout-relative
 * @cur_y: current cursor y, layout-relative
 *
 * Tests the motion segment (@prev_x,@prev_y)->(@cur_x,@cur_y) against the
 * installed barriers.  When capture is enabled but not yet active and the
 * segment crosses a barrier, this activates capture (bumping the
 * activation id and firing the activation callback with the crossing
 * point and barrier id) and returns %TRUE.  Does nothing and returns
 * %FALSE when capture is disabled, already active, or no barrier is
 * crossed.
 *
 * Returns: %TRUE if this call activated capture.
 */
gboolean
gowl_input_capture_check_crossing(
	GowlInputCapture *self,
	gdouble           prev_x,
	gdouble           prev_y,
	gdouble           cur_x,
	gdouble           cur_y
){
	GList *l;

	g_return_val_if_fail(GOWL_IS_INPUT_CAPTURE(self), FALSE);

	if (!self->enabled || self->active)
		return FALSE;

	for (l = self->barriers; l != NULL; l = l->next) {
		const GowlInputBarrier *b = (const GowlInputBarrier *)l->data;
		GowlInputBarrierOrientation o = gowl_input_barrier_classify(b);
		gdouble cross = 0.0;

		if (o == GOWL_INPUT_BARRIER_VERTICAL) {
			gint ylo = MIN(b->y1, b->y2);
			gint yhi = MAX(b->y1, b->y2);
			if (seg_crosses_vertical(prev_x, prev_y, cur_x, cur_y,
			                         b->x1, ylo, yhi, &cross)) {
				do_activate(self, (gdouble)b->x1, cross, b->id);
				return TRUE;
			}
		} else if (o == GOWL_INPUT_BARRIER_HORIZONTAL) {
			gint xlo = MIN(b->x1, b->x2);
			gint xhi = MAX(b->x1, b->x2);
			if (seg_crosses_horizontal(prev_x, prev_y, cur_x, cur_y,
			                           b->y1, xlo, xhi, &cross)) {
				do_activate(self, cross, (gdouble)b->y1, b->id);
				return TRUE;
			}
		}
	}

	return FALSE;
}

/**
 * gowl_input_capture_deactivate:
 * @self: a #GowlInputCapture
 *
 * Ends the current capture activation (e.g. the user pressed the release
 * keybind), restoring local input, and fires the activation callback with
 * @activated = %FALSE.  Capture stays enabled, so re-crossing a barrier
 * re-activates.  A no-op when not active.
 */
void
gowl_input_capture_deactivate(GowlInputCapture *self)
{
	g_return_if_fail(GOWL_IS_INPUT_CAPTURE(self));

	if (!self->active)
		return;

	self->active = FALSE;
	if (self->activation_cb != NULL)
		self->activation_cb(self, self->activation_id, 0.0, 0.0,
		                    0, FALSE, self->activation_data);
}

/**
 * gowl_input_capture_emit:
 * @self: a #GowlInputCapture
 * @event: (not nullable): the input event to divert
 *
 * Hands @event to the registered sink, if capture is active and a sink is
 * set.  Called by the compositor input hooks for each event that should
 * be diverted away from the focused client.  A no-op when inactive or no
 * sink is registered.
 */
void
gowl_input_capture_emit(GowlInputCapture *self, const GowlInputEvent *event)
{
	g_return_if_fail(GOWL_IS_INPUT_CAPTURE(self));
	g_return_if_fail(event != NULL);

	if (!self->active || self->sink == NULL)
		return;

	self->sink(self, event, self->sink_data);
}
