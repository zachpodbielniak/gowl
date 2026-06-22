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
 * Tests for the InputCapture mechanism: the GowlInputZone and
 * GowlInputBarrier boxed types, and the GowlInputCapture state machine
 * (zone enumeration, barrier validation, crossing detection, the
 * enable/active state machine, and event diversion to a sink).  All run
 * with no compositor, no wlroots, and no libeis -- the state machine is
 * pure logic driven through plain C calls.
 */

#include <glib-object.h>

#include "boxed/gowl-input-zone.h"
#include "boxed/gowl-input-barrier.h"
#include "core/gowl-input-capture.h"

/* ------------------------------------------------------------------ *
 * GowlInputZone boxed type
 * ------------------------------------------------------------------ */

static void
test_zone_new(void)
{
	GowlInputZone *z;

	z = gowl_input_zone_new(1920, 1080, 0, 0, "DP-1");
	g_assert_nonnull(z);
	g_assert_cmpuint(z->width, ==, 1920);
	g_assert_cmpuint(z->height, ==, 1080);
	g_assert_cmpint(z->x, ==, 0);
	g_assert_cmpint(z->y, ==, 0);
	g_assert_cmpstr(z->output, ==, "DP-1");
	gowl_input_zone_free(z);
}

static void
test_zone_negative_origin(void)
{
	GowlInputZone *z;

	/* A monitor to the left of the primary has negative layout x. */
	z = gowl_input_zone_new(2560, 1440, -2560, -120, "HDMI-A-1");
	g_assert_cmpint(z->x, ==, -2560);
	g_assert_cmpint(z->y, ==, -120);
	gowl_input_zone_free(z);
}

static void
test_zone_copy_and_equals(void)
{
	GowlInputZone *a, *b;

	a = gowl_input_zone_new(800, 600, 10, 20, "X");
	b = gowl_input_zone_copy(a);
	g_assert_true(b != a);
	g_assert_true(b->output != a->output);   /* deep copy */
	g_assert_true(gowl_input_zone_equals(a, b));
	gowl_input_zone_free(a);
	gowl_input_zone_free(b);
}

static void
test_zone_equals_differs(void)
{
	GowlInputZone *a, *b, *c;

	a = gowl_input_zone_new(800, 600, 0, 0, "X");
	b = gowl_input_zone_new(800, 600, 0, 0, "Y");   /* output differs */
	c = gowl_input_zone_new(801, 600, 0, 0, "X");   /* width differs  */
	g_assert_false(gowl_input_zone_equals(a, b));
	g_assert_false(gowl_input_zone_equals(a, c));
	gowl_input_zone_free(a);
	gowl_input_zone_free(b);
	gowl_input_zone_free(c);
}

static void
test_zone_null_output(void)
{
	GowlInputZone *a, *b;

	a = gowl_input_zone_new(100, 100, 0, 0, NULL);
	b = gowl_input_zone_new(100, 100, 0, 0, NULL);
	g_assert_null(a->output);
	g_assert_true(gowl_input_zone_equals(a, b));
	gowl_input_zone_free(a);
	gowl_input_zone_free(b);
}

static void
test_zone_contains_point(void)
{
	GowlInputZone *z = gowl_input_zone_new(100, 100, 10, 10, NULL);

	g_assert_true(gowl_input_zone_contains_point(z, 10, 10));   /* TL incl. */
	g_assert_true(gowl_input_zone_contains_point(z, 109, 109));
	g_assert_false(gowl_input_zone_contains_point(z, 110, 50)); /* right excl. */
	g_assert_false(gowl_input_zone_contains_point(z, 50, 110)); /* bottom excl.*/
	g_assert_false(gowl_input_zone_contains_point(z, 9, 50));
	gowl_input_zone_free(z);
}

static void
test_zone_free_null(void)
{
	gowl_input_zone_free(NULL);   /* safe no-op */
}

static void
test_zone_gtype(void)
{
	g_assert_true(G_TYPE_IS_BOXED(GOWL_TYPE_INPUT_ZONE));
}

/* ------------------------------------------------------------------ *
 * GowlInputBarrier boxed type + classification
 * ------------------------------------------------------------------ */

static void
test_barrier_classify_horizontal(void)
{
	GowlInputBarrier *b = gowl_input_barrier_new(1, 0, 100, 500, 100);

	g_assert_cmpint(gowl_input_barrier_classify(b), ==,
	                GOWL_INPUT_BARRIER_HORIZONTAL);
	gowl_input_barrier_free(b);
}

static void
test_barrier_classify_vertical(void)
{
	GowlInputBarrier *b = gowl_input_barrier_new(1, 100, 0, 100, 500);

	g_assert_cmpint(gowl_input_barrier_classify(b), ==,
	                GOWL_INPUT_BARRIER_VERTICAL);
	gowl_input_barrier_free(b);
}

static void
test_barrier_classify_diagonal_invalid(void)
{
	GowlInputBarrier *b = gowl_input_barrier_new(1, 0, 0, 500, 500);

	g_assert_cmpint(gowl_input_barrier_classify(b), ==,
	                GOWL_INPUT_BARRIER_INVALID);
	gowl_input_barrier_free(b);
}

static void
test_barrier_classify_zero_length_invalid(void)
{
	GowlInputBarrier *b = gowl_input_barrier_new(1, 50, 50, 50, 50);

	g_assert_cmpint(gowl_input_barrier_classify(b), ==,
	                GOWL_INPUT_BARRIER_INVALID);
	gowl_input_barrier_free(b);
}

static void
test_barrier_classify_zero_id_invalid(void)
{
	/* id 0 is reserved/invalid per the portal spec, even if axis-aligned. */
	GowlInputBarrier *b = gowl_input_barrier_new(0, 100, 0, 100, 500);

	g_assert_cmpint(gowl_input_barrier_classify(b), ==,
	                GOWL_INPUT_BARRIER_INVALID);
	gowl_input_barrier_free(b);
}

static void
test_barrier_copy_and_equals(void)
{
	GowlInputBarrier *a, *b, *c;

	a = gowl_input_barrier_new(7, 100, 0, 100, 500);
	b = gowl_input_barrier_copy(a);
	c = gowl_input_barrier_new(8, 100, 0, 100, 500);
	g_assert_true(gowl_input_barrier_equals(a, b));
	g_assert_false(gowl_input_barrier_equals(a, c));   /* id differs */
	gowl_input_barrier_free(a);
	gowl_input_barrier_free(b);
	gowl_input_barrier_free(c);
}

static void
test_barrier_free_null(void)
{
	gowl_input_barrier_free(NULL);   /* safe no-op */
}

static void
test_barrier_gtype(void)
{
	g_assert_true(G_TYPE_IS_BOXED(GOWL_TYPE_INPUT_BARRIER));
}

/* ------------------------------------------------------------------ *
 * Zone enumeration on the state machine
 * ------------------------------------------------------------------ */

/* Build a layout: two side-by-side 1920x1080 monitors. */
static GList *
two_monitor_layout(void)
{
	GList *zones = NULL;

	zones = g_list_append(zones,
		gowl_input_zone_new(1920, 1080, 0, 0, "DP-1"));
	zones = g_list_append(zones,
		gowl_input_zone_new(1920, 1080, 1920, 0, "DP-2"));
	return zones;
}

static void
free_zone_list(GList *zones)
{
	g_list_free_full(zones, (GDestroyNotify)gowl_input_zone_free);
}

static void
test_capture_caps(void)
{
	GowlInputCapture *c = gowl_input_capture_new();

	g_assert_cmpint(gowl_input_capture_get_capabilities(c), ==,
	                (GOWL_INPUT_CAPTURE_CAP_KEYBOARD
	                 | GOWL_INPUT_CAPTURE_CAP_POINTER));
	g_object_unref(c);
}

static void
test_capture_zones_empty_initially(void)
{
	GowlInputCapture *c = gowl_input_capture_new();
	GList *z = gowl_input_capture_get_zones(c);

	g_assert_null(z);
	g_assert_cmpuint(gowl_input_capture_get_zone_set(c), ==, 0);
	g_object_unref(c);
}

static void
test_capture_set_zones_single(void)
{
	GowlInputCapture *c = gowl_input_capture_new();
	GList *in = NULL, *out;

	in = g_list_append(in, gowl_input_zone_new(1920, 1080, 0, 0, "DP-1"));
	gowl_input_capture_set_zones(c, in);
	free_zone_list(in);

	out = gowl_input_capture_get_zones(c);
	g_assert_cmpuint(g_list_length(out), ==, 1);
	g_assert_cmpuint(gowl_input_capture_get_zone_set(c), ==, 1);
	free_zone_list(out);
	g_object_unref(c);
}

static void
test_capture_zone_set_increments(void)
{
	GowlInputCapture *c = gowl_input_capture_new();
	GList *in = two_monitor_layout();

	gowl_input_capture_set_zones(c, in);
	g_assert_cmpuint(gowl_input_capture_get_zone_set(c), ==, 1);
	gowl_input_capture_set_zones(c, in);
	g_assert_cmpuint(gowl_input_capture_get_zone_set(c), ==, 2);
	free_zone_list(in);
	g_object_unref(c);
}

static void
test_capture_zones_deep_copied(void)
{
	GowlInputCapture *c = gowl_input_capture_new();
	GList *in = two_monitor_layout();
	GList *out;

	gowl_input_capture_set_zones(c, in);
	free_zone_list(in);   /* free the input; machine must own a copy */

	out = gowl_input_capture_get_zones(c);
	g_assert_cmpuint(g_list_length(out), ==, 2);
	g_assert_cmpstr(((GowlInputZone *)out->data)->output, ==, "DP-1");
	free_zone_list(out);
	g_object_unref(c);
}

/* ------------------------------------------------------------------ *
 * Barrier validation through the state machine
 * ------------------------------------------------------------------ */

static gboolean
set_one_barrier(GowlInputCapture *c, guint id,
                gint x1, gint y1, gint x2, gint y2)
{
	GList *bs = NULL;
	gboolean ok;

	bs = g_list_append(bs, gowl_input_barrier_new(id, x1, y1, x2, y2));
	ok = gowl_input_capture_set_barriers(c, bs, NULL, NULL);
	g_list_free_full(bs, (GDestroyNotify)gowl_input_barrier_free);
	return ok;
}

static void
test_barrier_reject_when_no_zones(void)
{
	GowlInputCapture *c = gowl_input_capture_new();

	/* Empty monitor list -> every barrier rejected (spec). */
	g_assert_false(set_one_barrier(c, 1, 0, 0, 0, 1080));
	g_assert_cmpuint(gowl_input_capture_n_barriers(c), ==, 0);
	g_object_unref(c);
}

static void
test_barrier_accept_outer_left_edge(void)
{
	GowlInputCapture *c = gowl_input_capture_new();
	GList *in = two_monitor_layout();

	gowl_input_capture_set_zones(c, in);
	free_zone_list(in);

	/* Left edge of the union (x == 0), full height of DP-1 -> outer. */
	g_assert_true(set_one_barrier(c, 1, 0, 0, 0, 1080));
	g_assert_cmpuint(gowl_input_capture_n_barriers(c), ==, 1);
	g_object_unref(c);
}

static void
test_barrier_accept_outer_right_edge(void)
{
	GowlInputCapture *c = gowl_input_capture_new();
	GList *in = two_monitor_layout();

	gowl_input_capture_set_zones(c, in);
	free_zone_list(in);

	/* Right edge of the union (x == 3840), full height of DP-2. */
	g_assert_true(set_one_barrier(c, 1, 3840, 0, 3840, 1080));
	g_object_unref(c);
}

static void
test_barrier_reject_interior_seam(void)
{
	GowlInputCapture *c = gowl_input_capture_new();
	GList *in = two_monitor_layout();

	gowl_input_capture_set_zones(c, in);
	free_zone_list(in);

	/* x == 1920 is the seam between DP-1 and DP-2: interior to the
	 * union, NOT an outer boundary -> rejected. */
	g_assert_false(set_one_barrier(c, 1, 1920, 0, 1920, 1080));
	g_assert_cmpuint(gowl_input_capture_n_barriers(c), ==, 0);
	g_object_unref(c);
}

static void
test_barrier_reject_diagonal(void)
{
	GowlInputCapture *c = gowl_input_capture_new();
	GList *in = two_monitor_layout();

	gowl_input_capture_set_zones(c, in);
	free_zone_list(in);
	g_assert_false(set_one_barrier(c, 1, 0, 0, 1920, 1080));
	g_object_unref(c);
}

static void
test_barrier_reject_zero_id(void)
{
	GowlInputCapture *c = gowl_input_capture_new();
	GList *in = two_monitor_layout();

	gowl_input_capture_set_zones(c, in);
	free_zone_list(in);
	g_assert_false(set_one_barrier(c, 0, 0, 0, 0, 1080));
	g_object_unref(c);
}

static void
test_barrier_partial_accept(void)
{
	GowlInputCapture *c = gowl_input_capture_new();
	GList *in = two_monitor_layout();
	GList *bs = NULL;
	GArray *accepted;
	gboolean ok;

	gowl_input_capture_set_zones(c, in);
	free_zone_list(in);

	/* One valid (outer left edge) + one invalid (interior seam). */
	bs = g_list_append(bs, gowl_input_barrier_new(10, 0, 0, 0, 1080));
	bs = g_list_append(bs, gowl_input_barrier_new(11, 1920, 0, 1920, 1080));

	accepted = g_array_new(FALSE, FALSE, sizeof(guint32));
	ok = gowl_input_capture_set_barriers(c, bs, accepted, NULL);
	g_assert_true(ok);
	g_assert_cmpuint(accepted->len, ==, 1);
	g_assert_cmpuint(g_array_index(accepted, guint32, 0), ==, 10);
	g_assert_cmpuint(gowl_input_capture_n_barriers(c), ==, 1);

	g_array_unref(accepted);
	g_list_free_full(bs, (GDestroyNotify)gowl_input_barrier_free);
	g_object_unref(c);
}

static void
test_barrier_all_invalid_fails_with_error(void)
{
	GowlInputCapture *c = gowl_input_capture_new();
	GList *in = two_monitor_layout();
	GError *err = NULL;
	gboolean ok;

	gowl_input_capture_set_zones(c, in);
	free_zone_list(in);

	ok = set_one_barrier(c, 0, 0, 0, 0, 0);   /* invalid */
	g_assert_false(ok);

	/* Same with the error out-param populated. */
	{
		GList *bs = NULL;
		bs = g_list_append(bs,
			gowl_input_barrier_new(1, 0, 0, 1920, 1080)); /* diagonal */
		ok = gowl_input_capture_set_barriers(c, bs, NULL, &err);
		g_assert_false(ok);
		g_assert_nonnull(err);
		g_clear_error(&err);
		g_list_free_full(bs, (GDestroyNotify)gowl_input_barrier_free);
	}
	g_object_unref(c);
}

static void
test_barrier_duplicate_ids_dedup(void)
{
	GowlInputCapture *c = gowl_input_capture_new();
	GList *in = two_monitor_layout();
	GList *bs = NULL;

	gowl_input_capture_set_zones(c, in);
	free_zone_list(in);

	/* Two valid barriers with the same id -> only the first installs. */
	bs = g_list_append(bs, gowl_input_barrier_new(5, 0, 0, 0, 1080));
	bs = g_list_append(bs, gowl_input_barrier_new(5, 3840, 0, 3840, 1080));
	g_assert_true(gowl_input_capture_set_barriers(c, bs, NULL, NULL));
	g_assert_cmpuint(gowl_input_capture_n_barriers(c), ==, 1);
	g_list_free_full(bs, (GDestroyNotify)gowl_input_barrier_free);
	g_object_unref(c);
}

static void
test_barrier_set_zones_clears_barriers(void)
{
	GowlInputCapture *c = gowl_input_capture_new();
	GList *in = two_monitor_layout();

	gowl_input_capture_set_zones(c, in);
	g_assert_true(set_one_barrier(c, 1, 0, 0, 0, 1080));
	g_assert_cmpuint(gowl_input_capture_n_barriers(c), ==, 1);

	/* A layout change must invalidate barriers. */
	gowl_input_capture_set_zones(c, in);
	g_assert_cmpuint(gowl_input_capture_n_barriers(c), ==, 0);
	free_zone_list(in);
	g_object_unref(c);
}

/* ------------------------------------------------------------------ *
 * Crossing detection + activation state machine
 * ------------------------------------------------------------------ */

typedef struct {
	guint   activations;
	guint   deactivations;
	guint32 last_activation_id;
	guint32 last_barrier_id;
	gdouble last_x;
	gdouble last_y;
} ActCounter;

static void
on_activation(GowlInputCapture *c, guint32 activation_id,
              gdouble x, gdouble y, guint32 barrier_id,
              gboolean activated, gpointer data)
{
	ActCounter *ac = data;

	(void)c;
	if (activated) {
		ac->activations++;
		ac->last_activation_id = activation_id;
		ac->last_barrier_id = barrier_id;
		ac->last_x = x;
		ac->last_y = y;
	} else {
		ac->deactivations++;
	}
}

/* A single capture with a left-edge vertical barrier (id 1) on a
 * single 1920x1080 monitor, enabled, with an activation counter. */
static GowlInputCapture *
make_armed_left_barrier(ActCounter *ac)
{
	GowlInputCapture *c = gowl_input_capture_new();
	GList *in = NULL;

	in = g_list_append(in, gowl_input_zone_new(1920, 1080, 0, 0, "DP-1"));
	gowl_input_capture_set_zones(c, in);
	free_zone_list(in);

	g_assert_true(set_one_barrier(c, 1, 0, 0, 0, 1080));
	gowl_input_capture_set_activation_callback(c, on_activation, ac);
	gowl_input_capture_enable(c);
	return c;
}

static void
test_cross_not_when_disabled(void)
{
	ActCounter ac = { 0 };
	GowlInputCapture *c = make_armed_left_barrier(&ac);

	gowl_input_capture_disable(c);
	/* Move across x==0 from +5 to -5; disabled -> no activation. */
	g_assert_false(gowl_input_capture_check_crossing(c, 5, 500, -5, 500));
	g_assert_cmpuint(ac.activations, ==, 0);
	g_assert_false(gowl_input_capture_is_active(c));
	g_object_unref(c);
}

static void
test_cross_activates_and_reports(void)
{
	ActCounter ac = { 0 };
	GowlInputCapture *c = make_armed_left_barrier(&ac);

	g_assert_true(gowl_input_capture_check_crossing(c, 5, 500, -5, 500));
	g_assert_true(gowl_input_capture_is_active(c));
	g_assert_cmpuint(ac.activations, ==, 1);
	g_assert_cmpuint(ac.last_barrier_id, ==, 1);
	g_assert_cmpuint(ac.last_activation_id, ==, 1);
	g_assert_cmpfloat(ac.last_x, ==, 0.0);
	/* Crossing y interpolated; horizontal move keeps y == 500. */
	g_assert_cmpfloat(ac.last_y, ==, 500.0);
	g_object_unref(c);
}

static void
test_cross_only_once_while_active(void)
{
	ActCounter ac = { 0 };
	GowlInputCapture *c = make_armed_left_barrier(&ac);

	g_assert_true(gowl_input_capture_check_crossing(c, 5, 500, -5, 500));
	/* Already active: a second crossing must not re-activate. */
	g_assert_false(gowl_input_capture_check_crossing(c, 5, 500, -5, 500));
	g_assert_cmpuint(ac.activations, ==, 1);
	g_object_unref(c);
}

static void
test_cross_touch_without_crossing(void)
{
	ActCounter ac = { 0 };
	GowlInputCapture *c = make_armed_left_barrier(&ac);

	/* Starts on the line (x==0) but moves away (to +5): no sign change. */
	g_assert_false(gowl_input_capture_check_crossing(c, 0, 500, 5, 500));
	g_assert_cmpuint(ac.activations, ==, 0);
	g_object_unref(c);
}

static void
test_cross_parallel_no_cross(void)
{
	ActCounter ac = { 0 };
	GowlInputCapture *c = make_armed_left_barrier(&ac);

	/* Motion parallel to the vertical barrier (constant x). */
	g_assert_false(gowl_input_capture_check_crossing(c, 5, 0, 5, 1080));
	g_assert_cmpuint(ac.activations, ==, 0);
	g_object_unref(c);
}

static void
test_cross_outside_span(void)
{
	ActCounter ac = { 0 };
	GowlInputCapture *c = make_armed_left_barrier(&ac);

	/* Crosses x==0 but at y==2000, outside the barrier's [0,1080] span. */
	g_assert_false(gowl_input_capture_check_crossing(c, 5, 2000, -5, 2000));
	g_assert_cmpuint(ac.activations, ==, 0);
	g_object_unref(c);
}

static void
test_cross_high_speed_leap(void)
{
	ActCounter ac = { 0 };
	GowlInputCapture *c = make_armed_left_barrier(&ac);

	/* One huge delta that leaps fully past the barrier (segment, not
	 * point-on-line, must still detect it). */
	g_assert_true(gowl_input_capture_check_crossing(c, 50, 500, -300, 540));
	g_assert_cmpuint(ac.activations, ==, 1);
	g_object_unref(c);
}

static void
test_cross_horizontal_barrier(void)
{
	ActCounter ac = { 0 };
	GowlInputCapture *c = gowl_input_capture_new();
	GList *in = NULL;

	in = g_list_append(in, gowl_input_zone_new(1920, 1080, 0, 0, "DP-1"));
	gowl_input_capture_set_zones(c, in);
	free_zone_list(in);

	/* Top edge horizontal barrier y==0, span x in [0,1920]. */
	g_assert_true(set_one_barrier(c, 2, 0, 0, 1920, 0));
	gowl_input_capture_set_activation_callback(c, on_activation, &ac);
	gowl_input_capture_enable(c);

	/* Move up across y==0 from +5 to -5. */
	g_assert_true(gowl_input_capture_check_crossing(c, 960, 5, 960, -5));
	g_assert_cmpuint(ac.last_barrier_id, ==, 2);
	g_assert_cmpfloat(ac.last_y, ==, 0.0);
	g_assert_cmpfloat(ac.last_x, ==, 960.0);
	g_object_unref(c);
}

static void
test_deactivate_keeps_enabled_recross(void)
{
	ActCounter ac = { 0 };
	GowlInputCapture *c = make_armed_left_barrier(&ac);

	g_assert_true(gowl_input_capture_check_crossing(c, 5, 500, -5, 500));
	g_assert_cmpuint(ac.activations, ==, 1);

	gowl_input_capture_deactivate(c);
	g_assert_false(gowl_input_capture_is_active(c));
	g_assert_true(gowl_input_capture_is_enabled(c));
	g_assert_cmpuint(ac.deactivations, ==, 1);

	/* Still enabled -> re-crossing re-activates with a new id. */
	g_assert_true(gowl_input_capture_check_crossing(c, 5, 500, -5, 500));
	g_assert_cmpuint(ac.activations, ==, 2);
	g_assert_cmpuint(ac.last_activation_id, ==, 2);
	g_object_unref(c);
}

static void
test_disable_while_active_deactivates(void)
{
	ActCounter ac = { 0 };
	GowlInputCapture *c = make_armed_left_barrier(&ac);

	g_assert_true(gowl_input_capture_check_crossing(c, 5, 500, -5, 500));
	gowl_input_capture_disable(c);
	g_assert_false(gowl_input_capture_is_active(c));
	g_assert_false(gowl_input_capture_is_enabled(c));
	g_assert_cmpuint(ac.deactivations, ==, 1);
	g_object_unref(c);
}

static void
test_enable_idempotent(void)
{
	ActCounter ac = { 0 };
	GowlInputCapture *c = make_armed_left_barrier(&ac);

	gowl_input_capture_enable(c);   /* already enabled */
	g_assert_true(gowl_input_capture_is_enabled(c));
	/* enable while active is a no-op and must not deactivate. */
	g_assert_true(gowl_input_capture_check_crossing(c, 5, 500, -5, 500));
	gowl_input_capture_enable(c);
	g_assert_true(gowl_input_capture_is_active(c));
	g_object_unref(c);
}

static void
test_disable_when_not_enabled_noop(void)
{
	GowlInputCapture *c = gowl_input_capture_new();

	gowl_input_capture_disable(c);   /* never enabled -> safe no-op */
	g_assert_false(gowl_input_capture_is_enabled(c));
	g_object_unref(c);
}

static void
test_deactivate_when_inactive_noop(void)
{
	ActCounter ac = { 0 };
	GowlInputCapture *c = make_armed_left_barrier(&ac);

	gowl_input_capture_deactivate(c);   /* not active */
	g_assert_cmpuint(ac.deactivations, ==, 0);
	g_object_unref(c);
}

static void
test_release_drops_barriers(void)
{
	ActCounter ac = { 0 };
	GowlInputCapture *c = make_armed_left_barrier(&ac);

	g_assert_true(gowl_input_capture_check_crossing(c, 5, 500, -5, 500));
	gowl_input_capture_release(c);
	g_assert_false(gowl_input_capture_is_active(c));
	g_assert_false(gowl_input_capture_is_enabled(c));
	g_assert_cmpuint(gowl_input_capture_n_barriers(c), ==, 0);
	g_assert_cmpuint(ac.deactivations, ==, 1);
	g_object_unref(c);
}

static void
test_cross_correct_barrier_id_multi(void)
{
	ActCounter ac = { 0 };
	GowlInputCapture *c = gowl_input_capture_new();
	GList *in = two_monitor_layout();
	GList *bs = NULL;

	gowl_input_capture_set_zones(c, in);
	free_zone_list(in);

	/* Left outer edge id 100, right outer edge id 200. */
	bs = g_list_append(bs, gowl_input_barrier_new(100, 0, 0, 0, 1080));
	bs = g_list_append(bs, gowl_input_barrier_new(200, 3840, 0, 3840, 1080));
	g_assert_true(gowl_input_capture_set_barriers(c, bs, NULL, NULL));
	g_list_free_full(bs, (GDestroyNotify)gowl_input_barrier_free);

	gowl_input_capture_set_activation_callback(c, on_activation, &ac);
	gowl_input_capture_enable(c);

	/* Cross the right edge. */
	g_assert_true(gowl_input_capture_check_crossing(c, 3835, 500,
	                                                3845, 500));
	g_assert_cmpuint(ac.last_barrier_id, ==, 200);
	g_object_unref(c);
}

/* ------------------------------------------------------------------ *
 * Event diversion to the sink
 * ------------------------------------------------------------------ */

typedef struct {
	guint           count;
	GowlInputEvent  last;
} SinkRecorder;

static void
on_sink(GowlInputCapture *c, const GowlInputEvent *ev, gpointer data)
{
	SinkRecorder *r = data;

	(void)c;
	r->count++;
	r->last = *ev;
}

static void
test_emit_only_when_active(void)
{
	SinkRecorder rec = { 0 };
	ActCounter ac = { 0 };
	GowlInputCapture *c = make_armed_left_barrier(&ac);
	GowlInputEvent ev = { 0 };

	gowl_input_capture_set_sink(c, on_sink, &rec);

	ev.type = GOWL_INPUT_EVENT_REL_MOTION;
	ev.dx = 3.0;
	ev.dy = -2.0;

	/* Not active yet -> emit is a no-op. */
	gowl_input_capture_emit(c, &ev);
	g_assert_cmpuint(rec.count, ==, 0);

	/* Activate, then emit reaches the sink. */
	g_assert_true(gowl_input_capture_check_crossing(c, 5, 500, -5, 500));
	gowl_input_capture_emit(c, &ev);
	g_assert_cmpuint(rec.count, ==, 1);
	g_assert_cmpint(rec.last.type, ==, GOWL_INPUT_EVENT_REL_MOTION);
	g_assert_cmpfloat(rec.last.dx, ==, 3.0);
	g_assert_cmpfloat(rec.last.dy, ==, -2.0);
	g_object_unref(c);
}

static void
test_emit_no_sink_safe(void)
{
	ActCounter ac = { 0 };
	GowlInputCapture *c = make_armed_left_barrier(&ac);
	GowlInputEvent ev = { 0 };

	/* Active but no sink registered -> emit must not crash. */
	g_assert_true(gowl_input_capture_check_crossing(c, 5, 500, -5, 500));
	ev.type = GOWL_INPUT_EVENT_KEY;
	ev.keycode = 30;
	ev.state = 1;
	gowl_input_capture_emit(c, &ev);
	g_object_unref(c);
}

static void
test_emit_event_kinds_roundtrip(void)
{
	SinkRecorder rec = { 0 };
	ActCounter ac = { 0 };
	GowlInputCapture *c = make_armed_left_barrier(&ac);
	GowlInputEvent ev = { 0 };

	gowl_input_capture_set_sink(c, on_sink, &rec);
	g_assert_true(gowl_input_capture_check_crossing(c, 5, 500, -5, 500));

	ev.type = GOWL_INPUT_EVENT_BUTTON;
	ev.button = 0x110;   /* BTN_LEFT */
	ev.state = 1;
	gowl_input_capture_emit(c, &ev);
	g_assert_cmpint(rec.last.type, ==, GOWL_INPUT_EVENT_BUTTON);
	g_assert_cmpuint(rec.last.button, ==, 0x110);

	ev.type = GOWL_INPUT_EVENT_AXIS;
	ev.axis = 0;
	ev.value = 15.0;
	ev.discrete = 1;
	gowl_input_capture_emit(c, &ev);
	g_assert_cmpint(rec.last.type, ==, GOWL_INPUT_EVENT_AXIS);
	g_assert_cmpfloat(rec.last.value, ==, 15.0);
	g_assert_cmpint(rec.last.discrete, ==, 1);

	ev.type = GOWL_INPUT_EVENT_MODIFIERS;
	ev.mods_depressed = 0x1;   /* Shift */
	gowl_input_capture_emit(c, &ev);
	g_assert_cmpint(rec.last.type, ==, GOWL_INPUT_EVENT_MODIFIERS);
	g_assert_cmpuint(rec.last.mods_depressed, ==, 0x1);

	g_assert_cmpuint(rec.count, ==, 3);
	g_object_unref(c);
}

static void
test_capture_gtype_is_object(void)
{
	g_assert_true(g_type_is_a(GOWL_TYPE_INPUT_CAPTURE, G_TYPE_OBJECT));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	/* Zone boxed */
	g_test_add_func("/inputcapture/zone/new", test_zone_new);
	g_test_add_func("/inputcapture/zone/negative-origin",
	                test_zone_negative_origin);
	g_test_add_func("/inputcapture/zone/copy-equals",
	                test_zone_copy_and_equals);
	g_test_add_func("/inputcapture/zone/equals-differs",
	                test_zone_equals_differs);
	g_test_add_func("/inputcapture/zone/null-output", test_zone_null_output);
	g_test_add_func("/inputcapture/zone/contains-point",
	                test_zone_contains_point);
	g_test_add_func("/inputcapture/zone/free-null", test_zone_free_null);
	g_test_add_func("/inputcapture/zone/gtype", test_zone_gtype);

	/* Barrier boxed + classify */
	g_test_add_func("/inputcapture/barrier/classify-horizontal",
	                test_barrier_classify_horizontal);
	g_test_add_func("/inputcapture/barrier/classify-vertical",
	                test_barrier_classify_vertical);
	g_test_add_func("/inputcapture/barrier/classify-diagonal",
	                test_barrier_classify_diagonal_invalid);
	g_test_add_func("/inputcapture/barrier/classify-zero-length",
	                test_barrier_classify_zero_length_invalid);
	g_test_add_func("/inputcapture/barrier/classify-zero-id",
	                test_barrier_classify_zero_id_invalid);
	g_test_add_func("/inputcapture/barrier/copy-equals",
	                test_barrier_copy_and_equals);
	g_test_add_func("/inputcapture/barrier/free-null", test_barrier_free_null);
	g_test_add_func("/inputcapture/barrier/gtype", test_barrier_gtype);

	/* Zones on the machine */
	g_test_add_func("/inputcapture/caps", test_capture_caps);
	g_test_add_func("/inputcapture/zones/empty-initial",
	                test_capture_zones_empty_initially);
	g_test_add_func("/inputcapture/zones/set-single",
	                test_capture_set_zones_single);
	g_test_add_func("/inputcapture/zones/zone-set-increments",
	                test_capture_zone_set_increments);
	g_test_add_func("/inputcapture/zones/deep-copied",
	                test_capture_zones_deep_copied);

	/* Barrier validation */
	g_test_add_func("/inputcapture/barrier/reject-no-zones",
	                test_barrier_reject_when_no_zones);
	g_test_add_func("/inputcapture/barrier/accept-left-edge",
	                test_barrier_accept_outer_left_edge);
	g_test_add_func("/inputcapture/barrier/accept-right-edge",
	                test_barrier_accept_outer_right_edge);
	g_test_add_func("/inputcapture/barrier/reject-interior-seam",
	                test_barrier_reject_interior_seam);
	g_test_add_func("/inputcapture/barrier/reject-diagonal",
	                test_barrier_reject_diagonal);
	g_test_add_func("/inputcapture/barrier/reject-zero-id",
	                test_barrier_reject_zero_id);
	g_test_add_func("/inputcapture/barrier/partial-accept",
	                test_barrier_partial_accept);
	g_test_add_func("/inputcapture/barrier/all-invalid-error",
	                test_barrier_all_invalid_fails_with_error);
	g_test_add_func("/inputcapture/barrier/duplicate-ids",
	                test_barrier_duplicate_ids_dedup);
	g_test_add_func("/inputcapture/barrier/set-zones-clears",
	                test_barrier_set_zones_clears_barriers);

	/* Crossing + activation */
	g_test_add_func("/inputcapture/cross/not-when-disabled",
	                test_cross_not_when_disabled);
	g_test_add_func("/inputcapture/cross/activates",
	                test_cross_activates_and_reports);
	g_test_add_func("/inputcapture/cross/only-once-active",
	                test_cross_only_once_while_active);
	g_test_add_func("/inputcapture/cross/touch-no-cross",
	                test_cross_touch_without_crossing);
	g_test_add_func("/inputcapture/cross/parallel",
	                test_cross_parallel_no_cross);
	g_test_add_func("/inputcapture/cross/outside-span",
	                test_cross_outside_span);
	g_test_add_func("/inputcapture/cross/high-speed-leap",
	                test_cross_high_speed_leap);
	g_test_add_func("/inputcapture/cross/horizontal",
	                test_cross_horizontal_barrier);
	g_test_add_func("/inputcapture/cross/multi-barrier-id",
	                test_cross_correct_barrier_id_multi);

	/* State machine */
	g_test_add_func("/inputcapture/state/deactivate-recross",
	                test_deactivate_keeps_enabled_recross);
	g_test_add_func("/inputcapture/state/disable-while-active",
	                test_disable_while_active_deactivates);
	g_test_add_func("/inputcapture/state/enable-idempotent",
	                test_enable_idempotent);
	g_test_add_func("/inputcapture/state/disable-not-enabled",
	                test_disable_when_not_enabled_noop);
	g_test_add_func("/inputcapture/state/deactivate-inactive",
	                test_deactivate_when_inactive_noop);
	g_test_add_func("/inputcapture/state/release",
	                test_release_drops_barriers);

	/* Diversion */
	g_test_add_func("/inputcapture/emit/only-when-active",
	                test_emit_only_when_active);
	g_test_add_func("/inputcapture/emit/no-sink-safe",
	                test_emit_no_sink_safe);
	g_test_add_func("/inputcapture/emit/event-kinds",
	                test_emit_event_kinds_roundtrip);
	g_test_add_func("/inputcapture/gtype", test_capture_gtype_is_object);

	return g_test_run();
}
