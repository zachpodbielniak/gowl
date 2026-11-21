#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: per-device input remapping stays invisible until it is used,
# and what it produces goes through the compositor's own decisions.
#
# The promise is that a session without the inputremap module behaves
# byte for byte as it did before the feature existed.  A unit test can
# show one path doing that; it cannot show that the NEXT edit to
# on_cursor_button kept the hook ahead of the handler, or that the remap
# core still bails out before it allocates anything.  These are
# structural facts, so they are checked on the source.
#
# The second half matters as much: a remapped key sent straight to the
# seat would skip keybinds, the embedder intercept and InputCapture --
# and the Super+Escape hatch with them.  The core must only ever reach
# the seat through compositor_handle_key() and compositor_handle_button().

set -e
# An inherited CDPATH makes `cd' echo the resolved directory.
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
comp="$root/src/core/gowl-compositor.c"
core="$root/src/core/gowl-input-remap-core.c"
fail=0

# Body of a function, comments stripped, from its definition to the
# closing brace in column 1.
body() {
	awk -v fn="$2" '
		$0 ~ "^" fn "\\(" { inside = 1 }
		inside { print }
		inside && /^}/ { exit }
	' "$1" | sed 's,/\*.*\*/,,' | grep -vE '^[[:space:]]*[*#]'
}

require_fn() {
	if ! grep -qE "^$2\\(" "$1"; then
		echo "FAIL: no function named $2 in $(basename "$1")"
		echo "      A guard naming a function that does not exist"
		echo "      checks nothing."
		fail=1
		return 1
	fi
	return 0
}

# Line number of the first match of $3 inside function $2 of file $1,
# or 0.
line_of() {
	body "$1" "$2" | grep -nE "$3" | head -1 | cut -d: -f1 | grep . || echo 0
}

# 1. A keyboard joins the group unless -- and only unless -- claimed.
if require_fn "$comp" create_keyboard; then
	if ! body "$comp" create_keyboard | tr '\n' ' ' \
		| grep -qE 'if \(!gowl_input_remap_core_try_claim\([^)]*\)\)[[:space:]]*wlr_keyboard_group_add_keyboard'; then
		echo "FAIL: create_keyboard no longer adds the keyboard to the"
		echo "      group exactly when gowl_input_remap_core_try_claim()"
		echo "      says no.  Without a remapper every keyboard must join."
		fail=1
	fi
fi

# 2. The pointer hooks run first and fall through to the old handling.
if require_fn "$comp" on_cursor_button; then
	hook=$(line_of "$comp" on_cursor_button 'gowl_input_remap_core_button[[:space:]]*\(')
	old=$(line_of "$comp" on_cursor_button 'compositor_handle_button[[:space:]]*\(')
	if [ "$hook" -eq 0 ] || [ "$old" -eq 0 ] || [ "$hook" -ge "$old" ]; then
		echo "FAIL: on_cursor_button must ask gowl_input_remap_core_button()"
		echo "      before compositor_handle_button(), and still call it."
		fail=1
	fi
fi
if require_fn "$comp" on_cursor_axis; then
	hook=$(line_of "$comp" on_cursor_axis 'gowl_input_remap_core_axis[[:space:]]*\(')
	old=$(line_of "$comp" on_cursor_axis 'compositor_handle_axis[[:space:]]*\(')
	if [ "$hook" -eq 0 ] || [ "$old" -eq 0 ] || [ "$hook" -ge "$old" ]; then
		echo "FAIL: on_cursor_axis must ask gowl_input_remap_core_axis()"
		echo "      first and still hand the scroll to"
		echo "      compositor_handle_axis() (where the recorder tap is)."
		fail=1
	fi
fi
if require_fn "$comp" on_cursor_motion; then
	hook=$(line_of "$comp" on_cursor_motion 'gowl_input_remap_core_motion[[:space:]]*\(')
	old=$(line_of "$comp" on_cursor_motion 'compositor_handle_motion_relative[[:space:]]*\(')
	if [ "$hook" -eq 0 ] || [ "$old" -eq 0 ] || [ "$hook" -ge "$old" ]; then
		echo "FAIL: on_cursor_motion must ask gowl_input_remap_core_motion()"
		echo "      first (a mouse driving a virtual stick) and still"
		echo "      hand the motion to compositor_handle_motion_relative()."
		fail=1
	fi
fi

# 3. Each hook in the core returns before touching state when nothing
#    is active or claimed.
if require_fn "$core" gowl_input_remap_core_try_claim; then
	if ! body "$core" gowl_input_remap_core_try_claim \
		| grep -qE 'if \(r == NULL'; then
		echo "FAIL: gowl_input_remap_core_try_claim must return at once"
		echo "      when no remapper is active."
		fail=1
	fi
fi
if require_fn "$core" claimed_pointer; then
	if ! body "$core" claimed_pointer \
		| grep -qE 'remap_devices == NULL'; then
		echo "FAIL: claimed_pointer must return NULL at once while no"
		echo "      device has ever been seen by a remapper."
		fail=1
	fi
fi
if require_fn "$core" gowl_compositor_input_remap_reevaluate; then
	if ! body "$core" gowl_compositor_input_remap_reevaluate \
		| grep -qE 'active_remapper\(self\) == NULL && self->remap_devices == NULL'; then
		echo "FAIL: gowl_compositor_input_remap_reevaluate must leave the"
		echo "      seat alone when there is nothing to re-decide."
		fail=1
	fi
fi

if require_fn "$core" gowl_input_remap_core_motion; then
	if ! body "$core" gowl_input_remap_core_motion \
		| grep -qE 'claimed_pointer\(self, dev\)'; then
		echo "FAIL: gowl_input_remap_core_motion must go through"
		echo "      claimed_pointer(), which is NULL at once while no"
		echo "      device has been seen by a remapper."
		fail=1
	fi
fi

# 3b. Game controllers: nothing under /dev/input is looked at without an
#     active remapper, and a controller is opened only when claimed (or
#     while the user runs identify mode).
pad="$root/src/core/gowl-input-remap-pad.c"
if require_fn "$pad" gowl_input_remap_pads_reevaluate; then
	if ! body "$pad" gowl_input_remap_pads_reevaluate \
		| grep -qE 'r == NULL && self->remap_pads == NULL'; then
		echo "FAIL: gowl_input_remap_pads_reevaluate must return before"
		echo "      scanning /dev/input when no remapper is active and"
		echo "      none ever was."
		fail=1
	fi
fi
calls=$(grep -vE '^[[:space:]]*[*/]' "$pad" | grep -cE '[^_a-z]pad_open[[:space:]]*\(' || true)
if [ "$calls" -ne 1 ] \
	|| ! body "$pad" pad_apply | tr '\n' ' ' \
		| grep -qE 'if \(want_open\(pad\)\)[[:space:]]*pad_open\(pad\)'; then
	echo "FAIL: gowl-input-remap-pad.c opens a controller outside the"
	echo "      claim/identify decision: pad_open() must have exactly one"
	echo "      call, in pad_apply(), behind want_open()."
	fail=1
fi
if grep -vE '^[[:space:]]*[*/]' "$pad" \
	| grep -qE 'g_timeout_add|g_idle_add|g_unix_fd_add|g_io_add_watch'; then
	echo "FAIL: gowl-input-remap-pad.c registers on GLib's default"
	echo "      context; controllers are read on the wl_event_loop."
	fail=1
fi
if grep -vE '^[[:space:]]*[*/]' "$pad" \
	| grep -qE 'wlr_seat_pointer_notify|wlr_seat_keyboard_notify|wlr_cursor_move'; then
	echo "FAIL: gowl-input-remap-pad.c drives the seat or the cursor"
	echo "      itself; stick motion and scroll go through"
	echo "      gowl_compositor_remap_handle_motion()/_axis()."
	fail=1
fi

# 4. Remapped output takes the compositor's decisions, as real input.
if require_fn "$comp" gowl_compositor_remap_handle_key; then
	if ! body "$comp" gowl_compositor_remap_handle_key \
		| grep -qE 'compositor_handle_key\([^;]*FALSE\)'; then
		echo "FAIL: gowl_compositor_remap_handle_key must go through"
		echo "      compositor_handle_key() as real (non-synthetic) input."
		fail=1
	fi
fi
if require_fn "$comp" gowl_compositor_remap_handle_button; then
	if ! body "$comp" gowl_compositor_remap_handle_button \
		| grep -qE 'compositor_handle_button\([^;]*FALSE\)'; then
		echo "FAIL: gowl_compositor_remap_handle_button must go through"
		echo "      compositor_handle_button() as real input."
		fail=1
	fi
fi
if require_fn "$comp" compositor_handle_key; then
	if ! body "$comp" compositor_handle_key | grep -q '!self->remap_feeding'; then
		echo "FAIL: compositor_handle_key arms key repeat for a remapped"
		echo "      key.  A remap is one press, one output."
		fail=1
	fi
fi
if grep -vE '^[[:space:]]*[*/]' "$core" \
	| grep -qE 'wlr_seat_keyboard_notify_key|wlr_seat_pointer_notify_button'; then
	echo "FAIL: gowl-input-remap-core.c sends input straight to the seat."
	echo "      That skips keybinds, the embedder intercept, InputCapture"
	echo "      and the Super+Escape hatch."
	fail=1
fi

# 5. The escape hatch is decided before any rule is consulted.
if require_fn "$core" on_remap_key; then
	if ! body "$core" on_remap_key | grep -q 'KEY_ESC'; then
		echo "FAIL: on_remap_key no longer recognises Super+Escape; a"
		echo "      claimed keyboard could swallow the escape hatch."
		fail=1
	fi
fi

if [ "$fail" -ne 0 ]; then
	echo "input-remap fast-path guard FAILED"
	exit 1
fi
echo "input-remap fast-path guard PASSED (hooks inert without a remapper, outputs go through the pipeline)"
exit 0
