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
 * test-input-remap-analog.c - The arithmetic behind gamepad remapping.
 *
 * Plain functions of plain values: normalising raw axis readings
 * (signed, unsigned-centred and trigger ranges), the deadzone and its
 * rescaling, the press/release hysteresis that makes a stick direction
 * one press, combining held keys into one stick position (opposites
 * cancel, diagonals on the unit circle), the speed curve with its
 * acceleration cap, and the inverse mapping a virtual pad reports.
 */

#include <glib.h>
#include <math.h>

#include "boxed/gowl-input-remap-analog.h"

#define EPS (1e-6)

static void
test_normalize_signed(void)
{
	GowlRemapAbsInfo a = { -32768, 32767, 128, 16 };

	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_normalize(&a, -32768, TRUE), -1.0, EPS);
	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_normalize(&a, 32767, TRUE), 1.0, EPS);
	/* the midpoint of an asymmetric range is -0.5: still ~0 */
	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_normalize(&a, 0, TRUE), 0.0, 1e-4);
	/* out of range clamps */
	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_normalize(&a, 99999, TRUE), 1.0, EPS);
}

static void
test_normalize_unsigned_centred(void)
{
	/* a cheap pad: 0..255 resting at 127.5 */
	GowlRemapAbsInfo a = { 0, 255, 15, 0 };

	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_normalize(&a, 0, TRUE), -1.0, EPS);
	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_normalize(&a, 255, TRUE), 1.0, EPS);
	g_assert_cmpfloat(fabs(gowl_remap_analog_normalize(&a, 128, TRUE)), <, 0.01);
}

static void
test_normalize_trigger(void)
{
	GowlRemapAbsInfo a = { 0, 1023, 0, 0 };

	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_normalize(&a, 0, FALSE), 0.0, EPS);
	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_normalize(&a, 1023, FALSE), 1.0, EPS);
	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_normalize(&a, 1023 / 2, FALSE),
	                               511.0 / 1023.0, EPS);
	/* a broken range is 0, never a division by zero */
	a.max = a.min;
	g_assert_cmpfloat(gowl_remap_analog_normalize(&a, 5, FALSE), ==, 0.0);
}

static void
test_deadzone(void)
{
	GowlRemapAbsInfo a = { -32768, 32767, 4096, 16 };

	/* inside: nothing; at the edge: 0; beyond: rescaled to reach 1 */
	g_assert_cmpfloat(gowl_remap_analog_deadzone(0.1, 0.2), ==, 0.0);
	g_assert_cmpfloat(gowl_remap_analog_deadzone(-0.2, 0.2), ==, 0.0);
	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_deadzone(0.6, 0.2), 0.5, EPS);
	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_deadzone(-0.6, 0.2), -0.5, EPS);
	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_deadzone(1.0, 0.2), 1.0, EPS);
	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_deadzone(0.5, 0.0), 0.5, EPS);
	/* the default comes from flat, floored at 0.05 */
	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_default_deadzone(&a, TRUE),
	                               4096.0 / 32767.5, 1e-4);
	a.flat = 0;
	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_default_deadzone(&a, TRUE),
	                               0.05, EPS);
}

static void
test_hysteresis(void)
{
	gboolean down;
	guint presses;
	guint i;
	/* a stick wobbling around the press threshold */
	static const gdouble wobble[] = {
		0.0, 0.3, 0.49, 0.51, 0.45, 0.52, 0.40, 0.36, 0.34, 0.2, 0.0
	};

	down = FALSE;
	presses = 0;
	for (i = 0; i < G_N_ELEMENTS(wobble); i++) {
		gboolean now = gowl_remap_analog_hysteresis(down, wobble[i], 0.5, 0.35);

		if (now && !down)
			presses++;
		down = now;
		/* still held between the thresholds once pressed */
		if (wobble[i] == 0.40)
			g_assert_true(down);
	}
	g_assert_cmpuint(presses, ==, 1);
	g_assert_false(down);
	/* pushed the other way is never this direction */
	g_assert_false(gowl_remap_analog_hysteresis(FALSE, -0.9, 0.5, 0.35));
	g_assert_false(gowl_remap_analog_hysteresis(TRUE, -0.9, 0.5, 0.35));
}

static void
test_combine(void)
{
	gdouble x;
	gdouble y;

	/* D: right */
	gowl_remap_analog_combine(1.0, 0.0, 0.0, 0.0, &x, &y);
	g_assert_cmpfloat_with_epsilon(x, 1.0, EPS);
	g_assert_cmpfloat_with_epsilon(y, 0.0, EPS);
	/* W+D: up-right ON the unit circle, not the corner */
	gowl_remap_analog_combine(1.0, 0.0, 0.0, 1.0, &x, &y);
	g_assert_cmpfloat_with_epsilon(x, M_SQRT1_2, EPS);
	g_assert_cmpfloat_with_epsilon(y, -M_SQRT1_2, EPS);
	g_assert_cmpfloat_with_epsilon(sqrt(x * x + y * y), 1.0, EPS);
	/* A+D: cancel */
	gowl_remap_analog_combine(1.0, 1.0, 0.0, 0.0, &x, &y);
	g_assert_cmpfloat(x, ==, 0.0);
	/* a half deflection stays half, inside the circle */
	gowl_remap_analog_combine(0.5, 0.0, 0.5, 0.0, &x, &y);
	g_assert_cmpfloat_with_epsilon(x, 0.5, EPS);
	g_assert_cmpfloat_with_epsilon(y, 0.5, EPS);
}

static void
test_curve_and_speed(void)
{
	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_curve(0.5, 2.0), 0.25, EPS);
	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_curve(-0.5, 2.0), -0.25, EPS);
	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_curve(0.5, 1.0), 0.5, EPS);
	g_assert_cmpfloat_with_epsilon(gowl_remap_analog_curve(1.0, 3.0), 1.0, EPS);

	g_assert_cmpfloat_with_epsilon(
		gowl_remap_analog_speed(1.0, 2.0, 1000.0, 0.0, 5.0), 1000.0, EPS);
	g_assert_cmpfloat_with_epsilon(
		gowl_remap_analog_speed(-0.5, 2.0, 1000.0, 0.0, 0.0), -250.0, EPS);
	/* acceleration grows with time held, and is capped at 4x */
	g_assert_cmpfloat_with_epsilon(
		gowl_remap_analog_speed(1.0, 1.0, 100.0, 1.0, 1.0), 200.0, EPS);
	g_assert_cmpfloat_with_epsilon(
		gowl_remap_analog_speed(1.0, 1.0, 100.0, 1.0, 100.0), 400.0, EPS);
}

static void
test_to_raw(void)
{
	g_assert_cmpint(gowl_remap_analog_to_raw(-1.0, -32768, 32767), ==, -32768);
	g_assert_cmpint(gowl_remap_analog_to_raw(1.0, -32768, 32767), ==, 32767);
	g_assert_cmpint(ABS(gowl_remap_analog_to_raw(0.0, -32768, 32767)), <=, 1);
	g_assert_cmpint(gowl_remap_analog_to_raw(1.0, 0, 255), ==, 255);
	g_assert_cmpint(gowl_remap_analog_to_raw(0.0, 0, 255), ==, 0);
	g_assert_cmpint(gowl_remap_analog_to_raw(-1.0, 0, 255), ==, 0);
	g_assert_cmpint(gowl_remap_analog_to_raw(1.0, -1, 1), ==, 1);
	g_assert_cmpint(gowl_remap_analog_to_raw(-1.0, -1, 1), ==, -1);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/input-remap/analog/normalize-signed", test_normalize_signed);
	g_test_add_func("/input-remap/analog/normalize-unsigned-centred",
	                test_normalize_unsigned_centred);
	g_test_add_func("/input-remap/analog/normalize-trigger", test_normalize_trigger);
	g_test_add_func("/input-remap/analog/deadzone", test_deadzone);
	g_test_add_func("/input-remap/analog/hysteresis", test_hysteresis);
	g_test_add_func("/input-remap/analog/combine", test_combine);
	g_test_add_func("/input-remap/analog/curve-and-speed", test_curve_and_speed);
	g_test_add_func("/input-remap/analog/to-raw", test_to_raw);
	return g_test_run();
}
