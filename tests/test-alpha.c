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

#include "gowl.h"

#include <math.h>

/* ---- Client alpha accessor tests ---- */

static void
test_alpha_client_default(void)
{
	GowlClient *c;

	c = gowl_client_new();
	g_assert_true(fabsf(gowl_client_get_alpha(c) - 1.0f) < 0.001f);
	g_object_unref(c);
}

static void
test_alpha_client_set_get(void)
{
	GowlClient *c;

	c = gowl_client_new();
	gowl_client_set_alpha(c, 0.5f);
	g_assert_true(fabsf(gowl_client_get_alpha(c) - 0.5f) < 0.001f);
	g_object_unref(c);
}

static void
test_alpha_client_clamp_low(void)
{
	GowlClient *c;

	c = gowl_client_new();
	gowl_client_set_alpha(c, -0.5f);
	g_assert_true(fabsf(gowl_client_get_alpha(c) - 0.0f) < 0.001f);
	g_object_unref(c);
}

static void
test_alpha_client_clamp_high(void)
{
	GowlClient *c;

	c = gowl_client_new();
	gowl_client_set_alpha(c, 2.0f);
	g_assert_true(fabsf(gowl_client_get_alpha(c) - 1.0f) < 0.001f);
	g_object_unref(c);
}

static void
test_alpha_client_zero(void)
{
	GowlClient *c;

	c = gowl_client_new();
	gowl_client_set_alpha(c, 0.0f);
	g_assert_true(fabsf(gowl_client_get_alpha(c) - 0.0f) < 0.001f);
	g_object_unref(c);
}

static void
test_alpha_client_one(void)
{
	GowlClient *c;

	c = gowl_client_new();
	gowl_client_set_alpha(c, 0.3f);
	gowl_client_set_alpha(c, 1.0f);
	g_assert_true(fabsf(gowl_client_get_alpha(c) - 1.0f) < 0.001f);
	g_object_unref(c);
}

/* ---- Pinned opacity ---- */

/* A new window is unpinned, so focus dimming governs it. */
static void
test_alpha_pinned_default(void)
{
	GowlClient *c;

	c = gowl_client_new();
	g_assert_true(gowl_client_get_pinned_alpha(c) == 0.0f);
	g_assert_true(fabsf(gowl_client_focus_alpha(c, TRUE, 0.9f, 0.7f)
	                    - 0.9f) < 0.001f);
	g_assert_true(fabsf(gowl_client_focus_alpha(c, FALSE, 0.9f, 0.7f)
	                    - 0.7f) < 0.001f);
	g_object_unref(c);
}

/* Pinning applies at once, and the focus decision then always answers
 * the pin -- focused or not.  This is the flash: the alpha module used
 * to set the focus value here and something else put the pin back. */
static void
test_alpha_pinned_wins_over_focus(void)
{
	GowlClient *c;

	c = gowl_client_new();
	gowl_client_set_alpha(c, 0.9f);
	gowl_client_set_pinned_alpha(c, 1.0f);
	g_assert_true(fabsf(gowl_client_get_alpha(c) - 1.0f) < 0.001f);
	g_assert_true(fabsf(gowl_client_focus_alpha(c, TRUE, 0.9f, 0.7f)
	                    - 1.0f) < 0.001f);
	g_assert_true(fabsf(gowl_client_focus_alpha(c, FALSE, 0.9f, 0.7f)
	                    - 1.0f) < 0.001f);

	/* A partial pin holds too. */
	gowl_client_set_pinned_alpha(c, 0.6f);
	g_assert_true(fabsf(gowl_client_focus_alpha(c, TRUE, 0.9f, 0.7f)
	                    - 0.6f) < 0.001f);
	g_object_unref(c);
}

/* Unpinning keeps the current opacity until the next focus change and
 * hands the decision back to the focus values. */
static void
test_alpha_unpin(void)
{
	GowlClient *c;

	c = gowl_client_new();
	gowl_client_set_pinned_alpha(c, 0.6f);
	gowl_client_set_pinned_alpha(c, 0.0f);
	g_assert_true(gowl_client_get_pinned_alpha(c) == 0.0f);
	g_assert_true(fabsf(gowl_client_get_alpha(c) - 0.6f) < 0.001f);
	g_assert_true(fabsf(gowl_client_focus_alpha(c, FALSE, 0.9f, 0.7f)
	                    - 0.7f) < 0.001f);
	g_object_unref(c);
}

/* Out-of-range pins: above 1 clamps, negative unpins. */
static void
test_alpha_pinned_clamp(void)
{
	GowlClient *c;

	c = gowl_client_new();
	gowl_client_set_pinned_alpha(c, 3.0f);
	g_assert_true(fabsf(gowl_client_get_pinned_alpha(c) - 1.0f) < 0.001f);
	gowl_client_set_pinned_alpha(c, -1.0f);
	g_assert_true(gowl_client_get_pinned_alpha(c) == 0.0f);
	g_object_unref(c);
}

/* A plain set_alpha (what focus dimming itself does) never pins. */
static void
test_alpha_set_does_not_pin(void)
{
	GowlClient *c;

	c = gowl_client_new();
	gowl_client_set_alpha(c, 0.5f);
	g_assert_true(gowl_client_get_pinned_alpha(c) == 0.0f);
	g_object_unref(c);
}

/* ---- Main ---- */

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	/* Client alpha accessors */
	g_test_add_func("/alpha/client/default", test_alpha_client_default);
	g_test_add_func("/alpha/client/set-get", test_alpha_client_set_get);
	g_test_add_func("/alpha/client/clamp-low", test_alpha_client_clamp_low);
	g_test_add_func("/alpha/client/clamp-high", test_alpha_client_clamp_high);
	g_test_add_func("/alpha/client/zero", test_alpha_client_zero);
	g_test_add_func("/alpha/client/one", test_alpha_client_one);

	/* Pinned opacity */
	g_test_add_func("/alpha/pinned/default", test_alpha_pinned_default);
	g_test_add_func("/alpha/pinned/wins-over-focus",
	                test_alpha_pinned_wins_over_focus);
	g_test_add_func("/alpha/pinned/unpin", test_alpha_unpin);
	g_test_add_func("/alpha/pinned/clamp", test_alpha_pinned_clamp);
	g_test_add_func("/alpha/pinned/set-does-not-pin",
	                test_alpha_set_does_not_pin);

	return g_test_run();
}
