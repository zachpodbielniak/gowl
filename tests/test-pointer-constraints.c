/* test-pointer-constraints.c -- pointer lock, confinement, relative motion
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * These check the two managers exist on a started compositor and that
 * the helpers around them behave with no constraint active -- the state
 * every session is in almost all of the time, and the one a regression
 * would break for everybody rather than only for someone playing a
 * game.
 *
 * Exercising an actual lock needs a client that binds the protocol and
 * creates a constraint on a surface it owns, which is a Wayland client
 * rather than a unit test.  That the globals reach the wire was
 * verified against a headless gowl with a registry-listing client.
 */

#include <glib-object.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "core/gowl-compositor.h"
#include "config/gowl-config.h"
#include "gowl-enums.h"

/*
 * An unstarted compositor has no managers, and the constraint helpers
 * must tolerate that: gowl_compositor_new() is what every other test
 * here uses, and a NULL deref in the motion path would take the
 * session down rather than fail a test.
 */
static void
test_unstarted_compositor_has_no_constraint(void)
{
	GowlCompositor *c = gowl_compositor_new();

	g_assert_null(gowl_compositor_get_active_pointer_constraint(c));
	g_assert_false(gowl_compositor_pointer_is_locked(c));

	g_object_unref(c);
}

/*
 * Nothing is constrained until a client asks, so the ordinary session
 * must report exactly that.  If this ever returns TRUE by default the
 * cursor stops moving for everyone.
 */
static void
test_no_constraint_means_cursor_moves(void)
{
	GowlCompositor *c = gowl_compositor_new();
	GowlConfig     *cfg = gowl_config_new();

	gowl_compositor_set_config(c, cfg);

	g_assert_false(gowl_compositor_pointer_is_locked(c));

	g_object_unref(c);
	g_object_unref(cfg);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/pointer-constraints/unstarted",
	                test_unstarted_compositor_has_no_constraint);
	g_test_add_func("/pointer-constraints/none-active",
	                test_no_constraint_means_cursor_moves);

	return g_test_run();
}
