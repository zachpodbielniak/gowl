/* test-animation.c -- easing curves
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Curve math is checked here; test-animation-scene.c exercises the
 * animation clocks, geometry, opacity and snapshot lifetime on scene nodes.
 */

#include <glib.h>
#include <math.h>

#include "../modules/animation/gowl-animation.h"
#include "util/gowl-easing.h"

/* Every curve is pinned at both ends: a window must start where it was
 * and finish exactly where the layout put it, whatever the shape in
 * between.  An endpoint that is off by a fraction leaves windows a
 * pixel out of place after every move. */
static void
test_endpoints_are_exact(void)
{
	const char *names[] = { "linear", "ease-out-quint",
	                        "ease-in-out-cubic", "almost-linear",
	                        "quick", NULL };
	int i;

	for (i = 0; names[i] != NULL; i++) {
		g_assert_cmpfloat(gowl_curve_eval(names[i], 0.0), ==, 0.0);
		g_assert_cmpfloat(gowl_curve_eval(names[i], 1.0), ==, 1.0);
	}
}

/* Out-of-range input is clamped rather than extrapolated.  A tick that
 * arrives late must not push a window past its target. */
static void
test_out_of_range_is_clamped(void)
{
	g_assert_cmpfloat(gowl_curve_eval("ease-out-quint", -0.5), ==, 0.0);
	g_assert_cmpfloat(gowl_curve_eval("ease-out-quint", 1.5), ==, 1.0);
}

/* Linear is the identity, which is also the cheap path the evaluator
 * short-circuits rather than solving a Bézier for. */
static void
test_linear_is_identity(void)
{
	gdouble t;

	for (t = 0.1; t < 1.0; t += 0.1)
		g_assert_cmpfloat(fabs(gowl_curve_eval("linear", t) - t),
		                  <, 0.0001);
}

/* Every curve is monotonic: progress never goes backwards, or a window
 * would visibly reverse mid-slide. */
static void
test_curves_are_monotonic(void)
{
	/* Every curve that is not an overshoot curve.  `ease-out-back' and
	 * `spring' are excluded on purpose: they go past 1 and come back,
	 * which is the point of them, and there is a test above that fails
	 * if they ever stop doing it. */
	const char *names[] = { "linear", "ease-out-quint",
	                        "ease-in-out-cubic", "almost-linear",
	                        "quick", "ease-out-expo", NULL };
	int i;

	for (i = 0; names[i] != NULL; i++) {
		gdouble prev = -1.0, t;

		for (t = 0.0; t <= 1.0; t += 0.01) {
			gdouble v = gowl_curve_eval(names[i], t);

			g_assert_cmpfloat(v, >=, prev);
			g_assert_cmpfloat(v, >=, 0.0);
			g_assert_cmpfloat(v, <=, 1.0);
			prev = v;
		}
	}
}

/* ease-out-quint is most of the way there early: that is what makes a
 * slide feel responsive rather than floaty, and it is the property that
 * would be lost silently if the solver regressed. */
static void
test_ease_out_front_loads(void)
{
	g_assert_cmpfloat(gowl_curve_eval("ease-out-quint", 0.25), >, 0.6);
	g_assert_cmpfloat(gowl_curve_eval("ease-out-quint", 0.5), >, 0.85);
}

/* An unknown name falls back rather than returning garbage or zero --
 * a typo in a config should cost the chosen shape, not all motion. */
static void
test_unknown_curve_falls_back(void)
{
	g_assert_cmpfloat(gowl_curve_eval("no-such-curve", 0.5), >, 0.0);
	g_assert_cmpfloat(gowl_curve_eval(NULL, 0.5), >, 0.0);
g_assert_cmpfloat(gowl_curve_eval("no-such-curve", 0.5), ==,
	                  gowl_curve_eval("ease-out-expo", 0.5));
}

/* The two overshoot curves must actually overshoot -- that is the
 * whole reason they exist, and a cubic Bezier only does it when a
 * control point's y goes above 1.  A typo in the table would give a
 * curve that is merely a slightly different ease, with nothing to see
 * and nothing to fail. */
static void
test_overshoot_curves_overshoot(void)
{
	const char *names[] = { "ease-out-back", "spring", NULL };
	gboolean over;
	gdouble t;
	int i;

	for (i = 0; names[i] != NULL; i++) {
		over = FALSE;
		for (t = 0.0; t <= 1.0; t += 0.01) {
			if (gowl_curve_eval(names[i], t) > 1.0) {
				over = TRUE;
				break;
			}
		}
		g_assert_true(over);
	}
}

/* And the curves that are NOT overshoot curves must not, because a
 * window overshooting in a tiling layout lands on top of its
 * neighbour.  This is what keeps the default safe. */
static void
test_default_curves_stay_in_range(void)
{
	const char *names[] = { "linear", "ease-out-quint",
	                        "ease-in-out-cubic", "almost-linear",
	                        "quick", "ease-out-expo", NULL };
	gdouble t, v;
	int i;

	for (i = 0; names[i] != NULL; i++) {
		for (t = 0.0; t <= 1.0; t += 0.01) {
			v = gowl_curve_eval(names[i], t);
			g_assert_cmpfloat(v, >=, 0.0);
			g_assert_cmpfloat(v, <=, 1.0);
		}
	}
}

/* The default is expo, and it must be ahead of quint through the whole
 * first half -- that earlier departure is the reason it was made the
 * default, and it is the difference between "decisive" and "laggy" at
 * these durations. */
static void
test_default_leaves_faster_than_quint(void)
{
	gdouble t;

	for (t = 0.05; t <= 0.5; t += 0.05) {
		g_assert_cmpfloat(gowl_curve_eval("ease-out-expo", t), >,
		                  gowl_curve_eval("ease-out-quint", t));
	}
}

/*
 * The curve table lives in the core now, because the cube needed the same
 * names to mean the same thing and two tables would have drifted the
 * first time one of them gained a curve.  The animation module keeps
 * `gowl_curve_eval' as its own spelling of it; this asserts the two have
 * not come apart, which is the only way that refactor can go wrong
 * quietly -- windows and the cube would simply ease differently.
 */
static void
test_module_curve_matches_core_easing(void)
{
	const char *names[] = { "linear", "ease-out-quint", "ease-in-out-cubic",
	                        "almost-linear", "quick", "ease-out-expo",
	                        "ease-out-back", "spring",
	                        "no-such-curve", NULL };
	int i;
	gdouble t;

	for (i = 0; names[i] != NULL; i++) {
		for (t = 0.0; t <= 1.0001; t += 0.05) {
			g_assert_cmpfloat(fabs(gowl_curve_eval(names[i], t)
			                       - gowl_easing_eval(names[i], t)),
			                  <, 1e-12);
		}
	}

	/* Including the NULL case, which is how a caller asks for the
	 * default rather than for a named curve. */
	g_assert_cmpfloat(fabs(gowl_curve_eval(NULL, 0.4)
	                       - gowl_easing_eval(NULL, 0.4)), <, 1e-12);
}

/* A typo in a config file eases with the default rather than refusing,
 * so the only way a user finds out is by asking. */
static void
test_curve_names_are_reportable(void)
{
	g_assert_true(gowl_easing_name_is_known("ease-out-expo"));
	/* Case-insensitively, matching how the evaluator matches. */
	g_assert_true(gowl_easing_name_is_known("Ease-Out-Expo"));
	g_assert_false(gowl_easing_name_is_known("wobble"));
	g_assert_false(gowl_easing_name_is_known(NULL));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/animation/endpoints-exact", test_endpoints_are_exact);
	g_test_add_func("/animation/curve-matches-core-easing",
	                test_module_curve_matches_core_easing);
	g_test_add_func("/animation/curve-names-known",
	                test_curve_names_are_reportable);
	g_test_add_func("/animation/out-of-range-clamped",
	                test_out_of_range_is_clamped);
	g_test_add_func("/animation/linear-identity", test_linear_is_identity);
	g_test_add_func("/animation/monotonic", test_curves_are_monotonic);
	g_test_add_func("/animation/ease-out-front-loads",
	                test_ease_out_front_loads);
	g_test_add_func("/animation/overshoot-curves",
	                test_overshoot_curves_overshoot);
	g_test_add_func("/animation/default-curves-in-range",
	                test_default_curves_stay_in_range);
	g_test_add_func("/animation/default-leaves-faster",
	                test_default_leaves_faster_than_quint);
	g_test_add_func("/animation/unknown-falls-back",
	                test_unknown_curve_falls_back);

	return g_test_run();
}
