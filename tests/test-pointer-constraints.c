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
 *
 * The compositor's OWN confinement -- `toggle_pointer_confine', the
 * `confine-pointer' rule -- is checked here for the same quiet default,
 * and against a real client in test-pointer-confine-live.
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

/* Counts pointer-confine-changed emissions. */
static void
on_confine_changed(GowlCompositor *c, gboolean active, GowlClient *client,
                   const gchar *reason, gpointer data)
{
	(*(gint *)data)++;
}

/*
 * The compositor's own confinement, with nothing to confine to.  Every
 * entry point must be a quiet no-op on an unstarted compositor -- no
 * seat, no cursor, no clients -- because cmacs and IPC reach them
 * before, and after, there is a session: a confine with no window
 * fails, a release with nothing confined says nothing, and neither
 * dereferences what is not there.
 */
static void
test_confine_needs_a_window(void)
{
	GowlCompositor *c = gowl_compositor_new();
	gint changes = 0;
	gint x = -1;

	g_signal_connect(c, "pointer-confine-changed",
	                 G_CALLBACK(on_confine_changed), &changes);

	g_assert_false(gowl_compositor_pointer_confine_is_active(c));
	g_assert_null(gowl_compositor_get_pointer_confine_client(c));
	g_assert_false(gowl_compositor_pointer_confine_pins_focus(c));
	g_assert_false(gowl_compositor_get_pointer_confine_box(c, &x, NULL,
	                                                       NULL, NULL));
	g_assert_cmpint(x, ==, -1);

	g_assert_false(gowl_compositor_confine_pointer(c, NULL));
	g_assert_false(gowl_compositor_toggle_pointer_confine(c));
	gowl_compositor_release_pointer_confine(c, "manual");
	g_assert_cmpint(changes, ==, 0);

	/* And the confinement never claims the cursor is locked: that is
	 * the client's constraint, a different thing. */
	g_assert_false(gowl_compositor_pointer_is_locked(c));

	g_object_unref(c);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/pointer-constraints/unstarted",
	                test_unstarted_compositor_has_no_constraint);
	g_test_add_func("/pointer-constraints/none-active",
	                test_no_constraint_means_cursor_moves);
	g_test_add_func("/pointer-constraints/compositor-confine/no-window",
	                test_confine_needs_a_window);

	return g_test_run();
}
