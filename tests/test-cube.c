/* test-cube.c -- the desktop cube's rotation planner
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The planner is the half of the cube that can be wrong without crashing:
 * an off-by-one in the step count silently skips a tag on the way past,
 * and an envelope that is not exactly zero at both ends puts a visible
 * cut at the start and finish of every tag switch.  It is deliberately
 * free of wlroots and GL so all of that can be checked here rather than
 * by looking at a screen.
 *
 * The implementation is included rather than linked: it is one pure
 * translation unit and lives in a module .so that has no business being
 * loaded by a test.
 */

#include <glib.h>
#include <math.h>

#include "../modules/cube/gowl-cube-plan.c"

/* A tag view can hold several bits.  The lowest wins, so replacing a view
 * turns and merely adding a tag to one does not. */
static void
test_tag_index(void)
{
	g_assert_cmpint(gowl_cube_tag_index(1u << 0), ==, 0);
	g_assert_cmpint(gowl_cube_tag_index(1u << 8), ==, 8);
	g_assert_cmpint(gowl_cube_tag_index((1u << 1) | (1u << 3)), ==, 1);
	g_assert_cmpint(gowl_cube_tag_index(0), ==, -1);
}

/* The headline behaviour: one tag apart is one turn, three apart is three,
 * and every tag in between is on the itinerary rather than skipped. */
static void
test_step_count_and_itinerary(void)
{
	GowlCubePlan plan;
	gint j;

	g_assert_true(gowl_cube_plan_init(&plan, 0, 1, 4, 500, 150, 0));
	g_assert_cmpint(plan.steps, ==, 1);
	g_assert_cmpint(plan.dir, ==, 1);

	g_assert_true(gowl_cube_plan_init(&plan, 0, 3, 4, 500, 150, 0));
	g_assert_cmpint(plan.steps, ==, 3);
	g_assert_cmpint(plan.dir, ==, 1);
	for (j = 0; j <= plan.steps; j++)
		g_assert_cmpint(plan.tag[j], ==, j);

	/* Backwards visits the same tags in the other order. */
	g_assert_true(gowl_cube_plan_init(&plan, 3, 0, 4, 500, 150, 0));
	g_assert_cmpint(plan.steps, ==, 3);
	g_assert_cmpint(plan.dir, ==, -1);
	for (j = 0; j <= plan.steps; j++)
		g_assert_cmpint(plan.tag[j], ==, 3 - j);
}

/* There is no shortcut around the back: tags are a row, in the user's head
 * and in the bar, so 9 -> 1 is the eight-step trip. */
static void
test_no_wraparound(void)
{
	GowlCubePlan plan;

	g_assert_true(gowl_cube_plan_init(&plan, 8, 0, 4, 500, 150, 0));
	g_assert_cmpint(plan.steps, ==, 8);
	g_assert_cmpint(plan.tag[0], ==, 8);
	g_assert_cmpint(plan.tag[8], ==, 0);
}

/* Nothing to animate is not an error, it is a plan that declines. */
static void
test_refusals(void)
{
	GowlCubePlan plan;

	g_assert_false(gowl_cube_plan_init(&plan, 2, 2, 4, 500, 150, 0));
	g_assert_false(gowl_cube_plan_init(&plan, -1, 2, 4, 500, 150, 0));
	g_assert_false(gowl_cube_plan_init(&plan, 0, GOWL_CUBE_MAX_TAGS,
	                                   4, 500, 150, 0));
	/* A zero duration is how the config switches the cube off. */
	g_assert_false(gowl_cube_plan_init(&plan, 0, 1, 4, 0, 150, 0));
}

/* A longer journey takes longer, but sub-linearly and with a ceiling --
 * eight steps at full price would be a loading screen. */
static void
test_duration_grows_but_is_capped(void)
{
	GowlCubePlan one, three, eight;

	g_assert_true(gowl_cube_plan_init(&one, 0, 1, 4, 500, 150, 0));
	g_assert_true(gowl_cube_plan_init(&three, 0, 3, 4, 500, 150, 0));
	g_assert_true(gowl_cube_plan_init(&eight, 0, 8, 4, 500, 150, 0));

	g_assert_cmpint((gint)one.dur_us, ==, 500 * 1000);
	g_assert_cmpint((gint)three.dur_us, ==, (500 + 2 * 150) * 1000);
	g_assert_cmpint((gint)eight.dur_us, ==, 3 * 500 * 1000);

	/* Sub-linear: three steps must not cost three times one step. */
	g_assert_cmpint((gint)three.dur_us, <, 3 * (gint)one.dur_us);
}

/* An absurd face count from a config file bends to the nearest solid
 * rather than producing a two-sided one with no volume. */
static void
test_face_count_is_clamped(void)
{
	GowlCubePlan plan;

	g_assert_true(gowl_cube_plan_init(&plan, 0, 1, 1, 500, 150, 0));
	g_assert_cmpint(plan.faces, ==, GOWL_CUBE_MIN_FACES);

	g_assert_true(gowl_cube_plan_init(&plan, 0, 1, 99, 500, 150, 0));
	g_assert_cmpint(plan.faces, ==, GOWL_CUBE_MAX_FACES);
}

static void
test_progress_is_clamped(void)
{
	GowlCubePlan plan;

	g_assert_true(gowl_cube_plan_init(&plan, 0, 1, 4, 500, 150, 1000000));
	g_assert_cmpfloat(gowl_cube_plan_progress(&plan, 0), ==, 0.0);
	g_assert_cmpfloat(gowl_cube_plan_progress(&plan, 1000000), ==, 0.0);
	g_assert_cmpfloat(gowl_cube_plan_progress(&plan, 1250000), ==, 0.5);
	g_assert_cmpfloat(gowl_cube_plan_progress(&plan, 9000000), ==, 1.0);
}

/*
 * The landing has to be exact.  Slot j sits at dir * (j * face_angle -
 * rotation); if the finished rotation is not precisely steps face-angles
 * the destination desktop ends up a fraction of a degree off square, and
 * a full-screen texture sampled a fraction off square is visibly soft.
 */
static void
test_rotation_lands_square(void)
{
	GowlCubePlan plan;
	gdouble      face, residual;

	g_assert_true(gowl_cube_plan_init(&plan, 0, 3, 4, 500, 150, 0));
	face = gowl_cube_plan_face_angle(&plan);

	g_assert_cmpfloat(fabs(face - G_PI_2), <, 1e-12);
	g_assert_cmpfloat(gowl_cube_plan_rotation(&plan, 0.0), ==, 0.0);

	residual = (gdouble)plan.dir
	           * ((gdouble)plan.steps * face
	              - gowl_cube_plan_rotation(&plan, 1.0));
	g_assert_cmpfloat(fabs(residual), <, 1e-12);
}

/* Six sides means sixty degrees a step; the face angle is the only thing
 * the face count changes about the journey. */
static void
test_face_angle_follows_face_count(void)
{
	GowlCubePlan plan;

	g_assert_true(gowl_cube_plan_init(&plan, 0, 1, 6, 500, 150, 0));
	g_assert_cmpfloat(fabs(gowl_cube_plan_face_angle(&plan)
	                       - (2.0 * G_PI / 6.0)), <, 1e-12);
}

/*
 * The window is what keeps a slot and its own repeat --- they are `faces'
 * apart --- from ever being drawn in the same frame, which on a renderer
 * with no depth buffer would be two desktops fighting over one plane.
 */
static void
test_slot_window_is_narrow_and_tracks_the_front(void)
{
	GowlCubePlan plan;
	gint first, last;
	gdouble e;

	gint faces;

	for (faces = GOWL_CUBE_MIN_FACES; faces <= GOWL_CUBE_MAX_FACES; faces++) {
		g_assert_true(gowl_cube_plan_init(&plan, 0, 8, faces, 500, 150, 0));

		for (e = 0.0; e <= 1.0001; e += 0.02) {
			gint front;

			gowl_cube_plan_slot_window(&plan, e, &first, &last);

			/* Never more slots than the solid has sides. */
			g_assert_cmpint(last - first + 1, <=, plan.faces);
			/* Never more than the four the renderer sizes for. */
			g_assert_cmpint(last - first + 1, <=, 4);

			front = (gint)floor(e * (gdouble)plan.steps);
			g_assert_cmpint(first, <=, front);
			g_assert_cmpint(last, >=, front);
		}
	}
}

/*
 * Exactly zero at both ends.  This is the whole reason the rotation has no
 * visible cut: with the envelope at zero there is no pull-back and no
 * pitch, so the leading side projects to precisely the viewport and the
 * first and last frames are the flat desktop.
 */
static void
test_bump_is_zero_at_both_ends(void)
{
	gdouble t;

	g_assert_cmpfloat(gowl_cube_plan_bump(0.0), ==, 0.0);
	g_assert_cmpfloat(gowl_cube_plan_bump(1.0), ==, 0.0);
	g_assert_cmpfloat(gowl_cube_plan_bump(-1.0), ==, 0.0);
	g_assert_cmpfloat(gowl_cube_plan_bump(2.0), ==, 0.0);

	for (t = 0.05; t < 1.0; t += 0.05) {
		g_assert_cmpfloat(gowl_cube_plan_bump(t), >, 0.0);
		g_assert_cmpfloat(gowl_cube_plan_bump(t), <=, 1.0);
	}
	/* Peaks in the middle, where the solid is meant to be fully formed. */
	g_assert_cmpfloat(gowl_cube_plan_bump(0.5), >,
	                  gowl_cube_plan_bump(0.1));
	g_assert_cmpfloat(fabs(gowl_cube_plan_bump(0.5) - 1.0), <, 1e-9);

	/*
	 * And FLAT at the ends, not merely zero there.
	 *
	 * No frame lands on t = 1 exactly: at sixty frames a second and a
	 * half-second rotation the last one drawn is about t = 0.97, and the
	 * envelope's value THERE is what the user sees at the moment the
	 * effect is taken off screen.  An envelope that is zero at 1 but
	 * steep just before it ends the rotation with a jolt, which is how
	 * this started out.  Three per cent is invisible; twenty is not.
	 */
	g_assert_cmpfloat(gowl_cube_plan_bump(0.97), <, 0.03);
	g_assert_cmpfloat(gowl_cube_plan_bump(0.03), <, 0.03);
	/* Still meaningfully open a tenth of the way in, or the solid would
	 * spend the first fifth of the turn as a flat sheet. */
	g_assert_cmpfloat(gowl_cube_plan_bump(0.1), >, 0.15);
}

/*
 * Letting go of a half-finished swipe.
 *
 * The trip back is the SAME plan re-aimed, not a new one, because the
 * picture has to be continuous across the moment the fingers lift --- the
 * captured desktops, the direction and the sides all have to stay put.
 */
static void
test_reverse_runs_the_journey_back(void)
{
	GowlCubePlan plan;
	gint j;

	g_assert_true(gowl_cube_plan_init(&plan, 0, 3, 4, 500, 150, 0));
	gowl_cube_plan_reverse(&plan, 0.4, 1000000);

	/* Same itinerary, same direction: only the clock changed. */
	g_assert_cmpint(plan.steps, ==, 3);
	g_assert_cmpint(plan.dir, ==, 1);
	for (j = 0; j <= plan.steps; j++)
		g_assert_cmpint(plan.tag[j], ==, j);

	/* Starts from where the swipe got to and returns to nothing. */
	g_assert_cmpfloat(fabs(gowl_cube_plan_progress(&plan, 1000000) - 0.4),
	                  <, 1e-9);
	g_assert_cmpfloat(gowl_cube_plan_progress(&plan, 9000000), ==, 0.0);
	/* And monotonically, with no bounce past zero. */
	g_assert_cmpfloat(gowl_cube_plan_progress(&plan, 1000000 + plan.dur_us / 2),
	                  <, 0.4);
	g_assert_cmpfloat(gowl_cube_plan_progress(&plan, 1000000 + plan.dur_us / 2),
	                  >, 0.0);
}

/*
 * A rotation abandoned early should not take as long to undo as one
 * abandoned just short of the end --- but nor should it snap, which reads
 * as a glitch rather than as a decision.
 */
static void
test_reverse_duration_scales_with_a_floor(void)
{
	GowlCubePlan early, late;

	g_assert_true(gowl_cube_plan_init(&early, 0, 1, 4, 500, 150, 0));
	g_assert_true(gowl_cube_plan_init(&late, 0, 1, 4, 500, 150, 0));

	gowl_cube_plan_reverse(&early, 0.05, 0);
	gowl_cube_plan_reverse(&late, 0.9, 0);

	g_assert_cmpint((gint)early.dur_us, <, (gint)late.dur_us);
	/* The floor: never an instant snap. */
	g_assert_cmpint((gint)early.dur_us, >=, 90 * 1000);
	/* And never longer than the trip out. */
	g_assert_cmpint((gint)late.dur_us, <=, 500 * 1000);
}

/*
 * "Finished" cannot be "progress reached 1.0", because a reversed plan
 * finishes at 0.0.  Getting this wrong leaves a rewound rotation frozen
 * on the user's screen for ever, holding its captures.
 */
static void
test_finished_covers_both_directions(void)
{
	GowlCubePlan plan;

	g_assert_true(gowl_cube_plan_init(&plan, 0, 1, 4, 500, 150, 0));
	g_assert_false(gowl_cube_plan_finished(&plan, 0));
	g_assert_false(gowl_cube_plan_finished(&plan, 400000));
	g_assert_true(gowl_cube_plan_finished(&plan, 500000));
	g_assert_true(gowl_cube_plan_finished(&plan, 9000000));

	gowl_cube_plan_reverse(&plan, 0.5, 1000000);
	g_assert_false(gowl_cube_plan_finished(&plan, 1000000));
	g_assert_true(gowl_cube_plan_finished(&plan, 1000000 + plan.dur_us));
}

/* Reversing twice must not compound the shortening into a snap. */
static void
test_reverse_is_idempotent(void)
{
	GowlCubePlan plan;
	gint64 first;

	g_assert_true(gowl_cube_plan_init(&plan, 0, 2, 4, 500, 150, 0));
	gowl_cube_plan_reverse(&plan, 0.8, 0);
	first = plan.dur_us;
	gowl_cube_plan_reverse(&plan, 0.6, 10000);
	g_assert_cmpint((gint)plan.dur_us, ==, (gint)first);
	g_assert_cmpfloat(fabs(gowl_cube_plan_progress(&plan, 10000) - 0.6),
	                  <, 1e-9);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/cube/tag-index", test_tag_index);
	g_test_add_func("/cube/itinerary", test_step_count_and_itinerary);
	g_test_add_func("/cube/no-wraparound", test_no_wraparound);
	g_test_add_func("/cube/refusals", test_refusals);
	g_test_add_func("/cube/duration", test_duration_grows_but_is_capped);
	g_test_add_func("/cube/face-clamp", test_face_count_is_clamped);
	g_test_add_func("/cube/progress", test_progress_is_clamped);
	g_test_add_func("/cube/lands-square", test_rotation_lands_square);
	g_test_add_func("/cube/face-angle", test_face_angle_follows_face_count);
	g_test_add_func("/cube/slot-window",
	                test_slot_window_is_narrow_and_tracks_the_front);
	g_test_add_func("/cube/bump", test_bump_is_zero_at_both_ends);
	g_test_add_func("/cube/reverse", test_reverse_runs_the_journey_back);
	g_test_add_func("/cube/reverse-duration",
	                test_reverse_duration_scales_with_a_floor);
	g_test_add_func("/cube/finished", test_finished_covers_both_directions);
	g_test_add_func("/cube/reverse-idempotent", test_reverse_is_idempotent);

	return g_test_run();
}
