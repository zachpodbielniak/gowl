/* test-inject-keyboard.c -- injected keys, and the modifiers they carry
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A key injected by the RemoteDesktop portal (a software KVM driving this
 * machine from another keyboard) goes straight to the focused surface.
 * What a client believes about held MODIFIERS arrives in a separate
 * message, and for a long time nothing on the injection path sent it: the
 * keystrokes landed, the Shift did not, and a remote keyboard could type
 * only lower case and could not press Ctrl-C.
 *
 * The fix is three lines of xkb bookkeeping in
 * gowl_compositor_inject_key(), and all three are the kind that look
 * right while being wrong.  This file exercises exactly that sequence
 * against a real US keymap -- no compositor, no seat, no display -- so
 * the arithmetic is checked rather than the plumbing.
 */

#include <glib.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

static struct xkb_context *ctx;
static struct xkb_keymap  *keymap;

static void
setup_keymap(void)
{
	struct xkb_rule_names names;

	memset(&names, 0, sizeof(names));
	names.layout = "us";

	ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	g_assert_nonnull(ctx);
	keymap = xkb_keymap_new_from_names(ctx, &names,
	                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
	g_assert_nonnull(keymap);
}

static void
teardown_keymap(void)
{
	xkb_keymap_unref(keymap);
	xkb_context_unref(ctx);
}

/*
 * Exactly what gowl_compositor_inject_key() does to the keyboard's xkb
 * state for one key.  Kept as its own function so the test is unmistakably
 * about that sequence and not about something adjacent to it.
 */
static void
inject(struct xkb_state *state, uint32_t evdev_keycode, gboolean pressed)
{
	xkb_state_update_key(state, evdev_keycode + 8,
	                     pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
}

static xkb_mod_mask_t
depressed(struct xkb_state *state)
{
	return xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED);
}

/*
 * THE +8.
 *
 * evdev and xkb number keys eight apart, and libei -- which is what a KVM
 * client speaks -- reports evdev.  An injection path that forwards the
 * number unchanged does not fail: it types a DIFFERENT LETTER, silently,
 * for every key.  KEY_A is 30 and must reach xkb as 38.
 */
static void
test_keycode_offset(void)
{
	struct xkb_state *state = xkb_state_new(keymap);

	g_assert_cmpint(xkb_state_key_get_one_sym(state, KEY_A + 8), ==,
	                XKB_KEY_a);
	g_assert_cmpint(xkb_state_key_get_one_sym(state, KEY_Z + 8), ==,
	                XKB_KEY_z);
	g_assert_cmpint(xkb_state_key_get_one_sym(state, KEY_1 + 8), ==,
	                XKB_KEY_1);

	/* And the un-offset value is a real but wrong key, which is why the
	 * mistake survives testing by hand: something appears. */
	g_assert_cmpint(xkb_state_key_get_one_sym(state, KEY_A), !=,
	                XKB_KEY_a);

	xkb_state_unref(state);
}

/* An injected Shift has to actually shift the key that follows it. */
static void
test_injected_shift_shifts(void)
{
	struct xkb_state *state = xkb_state_new(keymap);
	xkb_mod_index_t   shift = xkb_keymap_mod_get_index(keymap,
	                                                   XKB_MOD_NAME_SHIFT);

	g_assert_cmpint(shift, !=, XKB_MOD_INVALID);
	g_assert_cmpuint(depressed(state), ==, 0);
	g_assert_cmpint(xkb_state_key_get_one_sym(state, KEY_A + 8), ==,
	                XKB_KEY_a);

	inject(state, KEY_LEFTSHIFT, TRUE);

	/* The mask the compositor sends to clients... */
	g_assert_cmpuint(depressed(state) & (1u << shift), !=, 0);
	/* ...and the letter it now produces. */
	g_assert_cmpint(xkb_state_key_get_one_sym(state, KEY_A + 8), ==,
	                XKB_KEY_A);

	inject(state, KEY_LEFTSHIFT, FALSE);
	g_assert_cmpuint(depressed(state) & (1u << shift), ==, 0);
	g_assert_cmpint(xkb_state_key_get_one_sym(state, KEY_A + 8), ==,
	                XKB_KEY_a);

	xkb_state_unref(state);
}

/* Ctrl too, since that is what a KVM user reaches for first. */
static void
test_injected_ctrl_and_alt(void)
{
	struct xkb_state *state = xkb_state_new(keymap);
	xkb_mod_index_t   ctrl = xkb_keymap_mod_get_index(keymap,
	                                                  XKB_MOD_NAME_CTRL);
	xkb_mod_index_t   alt = xkb_keymap_mod_get_index(keymap,
	                                                 XKB_MOD_NAME_ALT);

	inject(state, KEY_LEFTCTRL, TRUE);
	g_assert_cmpuint(depressed(state) & (1u << ctrl), !=, 0);
	inject(state, KEY_LEFTALT, TRUE);
	g_assert_cmpuint(depressed(state) & (1u << alt), !=, 0);
	/* Both at once, not one replacing the other. */
	g_assert_cmpuint(depressed(state) & (1u << ctrl), !=, 0);

	inject(state, KEY_LEFTCTRL, FALSE);
	inject(state, KEY_LEFTALT, FALSE);
	g_assert_cmpuint(depressed(state), ==, 0);

	xkb_state_unref(state);
}

/*
 * Injected and physical modifiers share one state, on purpose: a Shift
 * held on the local keyboard must apply to a letter arriving from the
 * remote one, and the reverse.  Releasing one must not clear the other.
 */
static void
test_local_and_remote_modifiers_compose(void)
{
	struct xkb_state *state = xkb_state_new(keymap);
	xkb_mod_index_t   shift = xkb_keymap_mod_get_index(keymap,
	                                                   XKB_MOD_NAME_SHIFT);

	/* "Physical" left Shift, "injected" right Shift -- the same call,
	 * which is the point: the state does not distinguish them. */
	inject(state, KEY_LEFTSHIFT, TRUE);
	inject(state, KEY_RIGHTSHIFT, TRUE);
	g_assert_cmpuint(depressed(state) & (1u << shift), !=, 0);

	inject(state, KEY_LEFTSHIFT, FALSE);
	/* Still shifted: the other one is still down. */
	g_assert_cmpuint(depressed(state) & (1u << shift), !=, 0);
	g_assert_cmpint(xkb_state_key_get_one_sym(state, KEY_A + 8), ==,
	                XKB_KEY_A);

	inject(state, KEY_RIGHTSHIFT, FALSE);
	g_assert_cmpuint(depressed(state) & (1u << shift), ==, 0);

	xkb_state_unref(state);
}

/*
 * The masks a client actually receives.
 *
 * A Wayland client is told four numbers -- depressed, latched, locked and
 * group -- and it is easy to send only the first, because it is the one
 * that is obviously changing.  Each of the others is load-bearing for
 * something: locked carries Caps Lock, group carries the layout, and a
 * client given only "depressed" behaves as though the other three were
 * always zero.
 */
static void
test_all_four_masks_are_derivable(void)
{
	struct xkb_state *state = xkb_state_new(keymap);

	inject(state, KEY_LEFTSHIFT, TRUE);

	/* Serializing each of them is defined at any point, which is what
	 * lets the injection path publish a complete modifier state after
	 * every key rather than tracking one by hand. */
	g_assert_cmpuint(xkb_state_serialize_mods(state,
		XKB_STATE_MODS_DEPRESSED), !=, 0);
	g_assert_cmpuint(xkb_state_serialize_mods(state,
		XKB_STATE_MODS_LATCHED), ==, 0);
	g_assert_cmpuint(xkb_state_serialize_mods(state,
		XKB_STATE_MODS_LOCKED), ==, 0);
	g_assert_cmpuint(xkb_state_serialize_layout(state,
		XKB_STATE_LAYOUT_EFFECTIVE), ==, 0);

	inject(state, KEY_LEFTSHIFT, FALSE);
	g_assert_cmpuint(xkb_state_serialize_mods(state,
		XKB_STATE_MODS_DEPRESSED), ==, 0);

	xkb_state_unref(state);
}

/* Caps Lock is a LOCKED modifier, not a depressed one; a portal that
 * only serialized the depressed mask would drop it. */
static void
test_locked_modifiers_are_reported_separately(void)
{
	struct xkb_state *state = xkb_state_new(keymap);
	xkb_mod_index_t   caps = xkb_keymap_mod_get_index(keymap,
	                                                  XKB_MOD_NAME_CAPS);

	inject(state, KEY_CAPSLOCK, TRUE);
	inject(state, KEY_CAPSLOCK, FALSE);

	g_assert_cmpuint(xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED)
	                 & (1u << caps), !=, 0);
	/* And it really does capitalise, so the mask is not merely set. */
	g_assert_cmpint(xkb_state_key_get_one_sym(state, KEY_A + 8), ==,
	                XKB_KEY_A);

	xkb_state_unref(state);
}

int
main(int argc, char **argv)
{
	int status;

	g_test_init(&argc, &argv, NULL);
	setup_keymap();

	g_test_add_func("/inject-keyboard/keycode-offset", test_keycode_offset);
	g_test_add_func("/inject-keyboard/shift", test_injected_shift_shifts);
	g_test_add_func("/inject-keyboard/ctrl-alt", test_injected_ctrl_and_alt);
	g_test_add_func("/inject-keyboard/compose",
	                test_local_and_remote_modifiers_compose);
	g_test_add_func("/inject-keyboard/masks",
	                test_all_four_masks_are_derivable);
	g_test_add_func("/inject-keyboard/locked",
	                test_locked_modifiers_are_reported_separately);

	status = g_test_run();
	teardown_keymap();
	return status;
}
