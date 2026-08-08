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
 * Unit tests for the pure keyboard-focus and close-routing rules.
 *
 * These cover two field defects reported 2026-08-08 by Ben Doty:
 *
 *   1. The KILL_CLIENT keybind sent the XDG close unconditionally,
 *      NULL-dereferencing inside wlroots for an X11 client and taking
 *      the whole `emacs --gowl' session down with it.
 *
 *   2. Keyboard-interactive layer surfaces (wofi) were granted the
 *      keyboard and then silently lost it to a later focus change,
 *      ending up visible, on top, and deaf.
 */

#include "core/gowl-focus-rules.h"
#include <glib.h>
#include <string.h>

/* Layer numbers, spelled out so the tests read like the protocol.
 * Kept local: the header only promises GOWL_LAYER_KEYBOARD_MIN. */
#define LAYER_BACKGROUND (0)
#define LAYER_BOTTOM     (1)
#define LAYER_TOP        (2)
#define LAYER_OVERLAY    (3)

/* keyboard_interactive values from zwlr_layer_surface_v1. */
#define KI_NONE      (0)
#define KI_EXCLUSIVE (1)
#define KI_ON_DEMAND (2)

/* -----------------------------------------------------------
 * Bug 1 -- close routing
 * ----------------------------------------------------------- */

static void
test_close_route_truth_table(void)
{
	/* Native Wayland client: xdg_toplevel set, no xwayland surface. */
	g_assert_cmpint(gowl_close_route_for(FALSE, TRUE), ==,
	                GOWL_CLOSE_ROUTE_XDG);

	/* X11 client: xwayland surface set, xdg_toplevel NULL.  This is
	 * the crashing case -- it must NOT route to XDG. */
	g_assert_cmpint(gowl_close_route_for(TRUE, FALSE), ==,
	                GOWL_CLOSE_ROUTE_XWAYLAND);
	g_assert_cmpint(gowl_close_route_for(TRUE, FALSE), !=,
	                GOWL_CLOSE_ROUTE_XDG);

	/* Unmapped / mid-teardown: neither pointer.  No-op, no fault. */
	g_assert_cmpint(gowl_close_route_for(FALSE, FALSE), ==,
	                GOWL_CLOSE_ROUTE_NONE);

	/* Both set is impossible in practice; X11 wins, because the XDG
	 * branch is the one that would crash on a stale pointer. */
	g_assert_cmpint(gowl_close_route_for(TRUE, TRUE), ==,
	                GOWL_CLOSE_ROUTE_XWAYLAND);
}

/* -----------------------------------------------------------
 * Bug 2 -- which layer surfaces own the keyboard
 * ----------------------------------------------------------- */

static void
test_layer_takes_keyboard_layers(void)
{
	/* Mapped + keyboard-interactive at TOP or above: owns it.  This
	 * is a launcher (wofi --show drun). */
	g_assert_true(gowl_layer_takes_keyboard(FALSE, TRUE, KI_EXCLUSIVE,
	                                        LAYER_TOP));
	g_assert_true(gowl_layer_takes_keyboard(FALSE, TRUE, KI_EXCLUSIVE,
	                                        LAYER_OVERLAY));

	/* Below TOP: bars and wallpapers never take the keyboard away
	 * from a window, even if they ask for it. */
	g_assert_false(gowl_layer_takes_keyboard(FALSE, TRUE, KI_EXCLUSIVE,
	                                         LAYER_BOTTOM));
	g_assert_false(gowl_layer_takes_keyboard(FALSE, TRUE, KI_EXCLUSIVE,
	                                         LAYER_BACKGROUND));

	/* The constant the compositor asserts against really is TOP. */
	g_assert_cmpint(GOWL_LAYER_KEYBOARD_MIN, ==, LAYER_TOP);
}

static void
test_layer_takes_keyboard_interactivity(void)
{
	/* keyboard_interactive = 0: a bar or wallpaper.  Never takes
	 * focus from a client, at any layer. */
	g_assert_false(gowl_layer_takes_keyboard(FALSE, TRUE, KI_NONE,
	                                         LAYER_TOP));
	g_assert_false(gowl_layer_takes_keyboard(FALSE, TRUE, KI_NONE,
	                                         LAYER_OVERLAY));

	/* Both EXCLUSIVE (1) and ON_DEMAND (2) ask for the keyboard.
	 * An on-demand surface still needs the grab held while it is up
	 * -- dropping it is exactly the reported failure. */
	g_assert_true(gowl_layer_takes_keyboard(FALSE, TRUE, KI_EXCLUSIVE,
	                                        LAYER_TOP));
	g_assert_true(gowl_layer_takes_keyboard(FALSE, TRUE, KI_ON_DEMAND,
	                                        LAYER_TOP));
}

static void
test_layer_takes_keyboard_lifecycle(void)
{
	/* Unmapped: no grab.  This is what releases the keyboard when a
	 * launcher goes away. */
	g_assert_false(gowl_layer_takes_keyboard(FALSE, FALSE, KI_EXCLUSIVE,
	                                         LAYER_TOP));
	g_assert_false(gowl_layer_takes_keyboard(FALSE, FALSE, KI_EXCLUSIVE,
	                                         LAYER_OVERLAY));

	/* Session locked: the lock client owns input; no layer surface
	 * may take the keyboard, however loudly it asks. */
	g_assert_false(gowl_layer_takes_keyboard(TRUE, TRUE, KI_EXCLUSIVE,
	                                         LAYER_OVERLAY));
	g_assert_false(gowl_layer_takes_keyboard(TRUE, TRUE, KI_ON_DEMAND,
	                                         LAYER_TOP));

	/* Locked outranks everything, including an unmapped surface --
	 * no combination of the other inputs flips it back on. */
	g_assert_false(gowl_layer_takes_keyboard(TRUE, FALSE, KI_NONE,
	                                         LAYER_BACKGROUND));
}

/* -----------------------------------------------------------
 * Bug 2 -- the focus gate itself
 * ----------------------------------------------------------- */

static void
test_focus_allow_baseline(void)
{
	/* Nothing in the way: an ordinary window takes focus.  The
	 * regression case -- an xdg-toplevel must still be focusable
	 * when no layer surface is mapped. */
	g_assert_cmpint(gowl_focus_decide(FALSE, FALSE, FALSE, FALSE, FALSE),
	                ==, GOWL_FOCUS_ALLOW);

	/* A focus *clear* (target NULL -> target_embedded FALSE) is also
	 * allowed when nothing holds a grab. */
	g_assert_cmpint(gowl_focus_decide(FALSE, FALSE, FALSE, FALSE, TRUE),
	                ==, GOWL_FOCUS_ALLOW);
}

static void
test_focus_layer_grab_blocks(void)
{
	/* The core of bug 2: a launcher holds the keyboard and a client
	 * asks for focus.  Refused -- whatever the request came from.
	 * The compositor cannot tell a keybind from a pointer enter from
	 * an arrange from a host embedder here, and must refuse all of
	 * them identically. */
	g_assert_cmpint(gowl_focus_decide(FALSE, FALSE, TRUE, FALSE, FALSE),
	                ==, GOWL_FOCUS_DENY_LAYER_GRAB);

	/* A focus clear is refused too.  Clearing focus out from under a
	 * launcher leaves it visible and deaf just as surely as pointing
	 * the keyboard at another window does. */
	g_assert_cmpint(gowl_focus_decide(FALSE, FALSE, TRUE, FALSE, TRUE),
	                ==, GOWL_FOCUS_DENY_LAYER_GRAB);

	/* The layer grab outranks an X11 popup grab: whichever popup is
	 * up, the launcher keeps the keyboard, and the reported reason
	 * names the layer. */
	g_assert_cmpint(gowl_focus_decide(FALSE, FALSE, TRUE, TRUE, FALSE),
	                ==, GOWL_FOCUS_DENY_LAYER_GRAB);
}

static void
test_focus_guard_precedence(void)
{
	/* Locked outranks everything below it. */
	g_assert_cmpint(gowl_focus_decide(TRUE, TRUE, TRUE, TRUE, FALSE),
	                ==, GOWL_FOCUS_DENY_LOCKED);

	/* Session locked with no other guard set: still refused.  A
	 * layer surface must not take focus while locked, and neither
	 * must a client. */
	g_assert_cmpint(gowl_focus_decide(TRUE, FALSE, FALSE, FALSE, FALSE),
	                ==, GOWL_FOCUS_DENY_LOCKED);

	/* Embedded clients (cmacs --gowl app buffers) are host-driven
	 * and never take the keyboard -- checked before the grabs. */
	g_assert_cmpint(gowl_focus_decide(FALSE, TRUE, TRUE, TRUE, FALSE),
	                ==, GOWL_FOCUS_DENY_EMBEDDED);
	g_assert_cmpint(gowl_focus_decide(FALSE, TRUE, FALSE, FALSE, FALSE),
	                ==, GOWL_FOCUS_DENY_EMBEDDED);
}

static void
test_focus_exclusive_client_preserved(void)
{
	/* Pre-existing behaviour, kept: an X11 override-redirect popup
	 * holding a grab blocks other clients... */
	g_assert_cmpint(gowl_focus_decide(FALSE, FALSE, FALSE, TRUE, FALSE),
	                ==, GOWL_FOCUS_DENY_EXCLUSIVE_CLIENT);

	/* ...but focusing the popup itself just re-asserts its own grab
	 * and is allowed. */
	g_assert_cmpint(gowl_focus_decide(FALSE, FALSE, FALSE, TRUE, TRUE),
	                ==, GOWL_FOCUS_ALLOW);

	/* An inactive exclusive_focus (popup no longer wants focus)
	 * blocks nothing. */
	g_assert_cmpint(gowl_focus_decide(FALSE, FALSE, FALSE, FALSE, FALSE),
	                ==, GOWL_FOCUS_ALLOW);
}

static void
test_focus_decide_is_total(void)
{
	gboolean locked, embedded, layer, excl, is_excl;
	GowlFocusDecision d;

	/* Exhaustive sweep of the 32-row truth table: the gate must
	 * always return one of the defined decisions, and must allow
	 * exactly when no guard applies. */
	for (locked = FALSE; locked <= TRUE; locked++)
	for (embedded = FALSE; embedded <= TRUE; embedded++)
	for (layer = FALSE; layer <= TRUE; layer++)
	for (excl = FALSE; excl <= TRUE; excl++)
	for (is_excl = FALSE; is_excl <= TRUE; is_excl++) {
		gboolean expect_allow;

		d = gowl_focus_decide(locked, embedded, layer, excl,
		                      is_excl);

		expect_allow = !locked && !embedded && !layer
		               && (!excl || is_excl);

		if (expect_allow)
			g_assert_cmpint(d, ==, GOWL_FOCUS_ALLOW);
		else
			g_assert_cmpint(d, !=, GOWL_FOCUS_ALLOW);

		/* Every decision has a distinct, non-empty name. */
		g_assert_nonnull(gowl_focus_decision_to_string(d));
		g_assert_cmpint(
			strlen(gowl_focus_decision_to_string(d)), >, 0);
	}
}

static void
test_focus_decision_names(void)
{
	g_assert_cmpstr(gowl_focus_decision_to_string(GOWL_FOCUS_ALLOW),
	                ==, "allow");

	/* Refusal reasons all say why, so a "my launcher is deaf" report
	 * is one G_MESSAGES_DEBUG=gowl run away from an answer. */
	g_assert_nonnull(strstr(
		gowl_focus_decision_to_string(GOWL_FOCUS_DENY_LAYER_GRAB),
		"layer"));
	g_assert_nonnull(strstr(
		gowl_focus_decision_to_string(GOWL_FOCUS_DENY_LOCKED),
		"locked"));
	g_assert_nonnull(strstr(
		gowl_focus_decision_to_string(GOWL_FOCUS_DENY_EMBEDDED),
		"embedded"));
	g_assert_nonnull(strstr(
		gowl_focus_decision_to_string(
			GOWL_FOCUS_DENY_EXCLUSIVE_CLIENT),
		"grab"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/focus-rules/close-route-truth-table",
	                test_close_route_truth_table);

	g_test_add_func("/focus-rules/layer-takes-keyboard/layers",
	                test_layer_takes_keyboard_layers);
	g_test_add_func("/focus-rules/layer-takes-keyboard/interactivity",
	                test_layer_takes_keyboard_interactivity);
	g_test_add_func("/focus-rules/layer-takes-keyboard/lifecycle",
	                test_layer_takes_keyboard_lifecycle);

	g_test_add_func("/focus-rules/decide/allow-baseline",
	                test_focus_allow_baseline);
	g_test_add_func("/focus-rules/decide/layer-grab-blocks",
	                test_focus_layer_grab_blocks);
	g_test_add_func("/focus-rules/decide/guard-precedence",
	                test_focus_guard_precedence);
	g_test_add_func("/focus-rules/decide/exclusive-client-preserved",
	                test_focus_exclusive_client_preserved);
	g_test_add_func("/focus-rules/decide/total",
	                test_focus_decide_is_total);
	g_test_add_func("/focus-rules/decide/names",
	                test_focus_decision_names);

	return g_test_run();
}
