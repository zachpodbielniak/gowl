#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: the input recorder's taps must stay wired to every input hook,
# and the injection path must stay unwired from all of them.
#
# tests/test-input-recorder.c drives the recorder through plain C calls,
# so it proves the ring, the deadline and the suppression policy and
# proves nothing about whether any of it is *reached*.  Delete the call
# in on_cursor_button and every one of those tests still passes while
# clicks stop being recorded -- a recorder that is correct in isolation
# and connected to nobody.
#
# The second half is the more important one, and it changed shape.
#
# The taps used to live on the wlr_cursor and keyboard listeners because
# the injection helpers bypassed those entirely.  That bypass WAS the
# bug: a software KVM could type and click, but never reach a compositor
# keybind or the bar, because neither was consulted for injected input.
# Injection now goes through the same compositor_handle_key() and
# compositor_handle_button() the real devices do.
#
# So "injection is untapped" can no longer be enforced by keeping it away
# from the tap.  It is enforced at the tap: every recording_note() in a
# shared decision function must be guarded by !synthetic.  Without that
# gowl records its own injected input, and a synthesiser reading the
# trace replays its own output -- which looks exactly like it worked.

set -e
# An inherited CDPATH makes `cd' echo the resolved directory.
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
comp="$root/src/core/gowl-compositor.c"
fail=0

# Body of a static function, comments stripped, from its definition to
# the closing brace in column 1.  Prose mentioning a call must not count
# as one.
# A name that does not exist yields an empty body, and every check on it
# then passes vacuously.  That is not hypothetical: this guard spent its
# whole life checking gowl_compositor_inject_pointer_button,
# _inject_pointer_axis and _inject_keyboard_key, none of which have ever
# existed -- the functions are _inject_button, _inject_axis and
# _inject_key.  Half the second half was testing nothing.
require_fn() {
	if ! grep -qE "^$1\\(" "$comp"; then
		echo "FAIL: no function named $1 in gowl-compositor.c"
		echo "      A guard that names a function which does not exist"
		echo "      passes on an empty body and checks nothing."
		fail=1
		return 1
	fi
	return 0
}

body() {
	awk -v fn="$1" '
		$0 ~ "^" fn "\\(" { inside = 1 }
		inside { print }
		inside && /^}/ { exit }
	' "$comp" | sed 's,/\*.*\*/,,' | grep -vE '^[[:space:]]*[*#]'
}

# Every hook that sees a real input event needs a tap.  Motion goes
# through recording_note_motion(), which fills the absolute position in
# from the cursor after it moved.
check_tap() {
	fn=$1
	call=$2

	if ! body "$fn" | grep -qE "\\b${call}[[:space:]]*\("; then
		echo "FAIL: $fn no longer calls $call()"
		echo "      That input kind stops being recorded, and every"
		echo "      test in test-input-recorder.c still passes."
		fail=1
	fi
}

check_tap compositor_handle_key    recording_note
check_tap on_kb_modifiers          recording_note
check_tap compositor_handle_button recording_note
check_tap on_cursor_axis           recording_note
check_tap on_cursor_motion         recording_note_motion
check_tap on_cursor_motion_abs     recording_note_motion

# A tap in a function injection also flows through must be conditional on
# !synthetic, or gowl records what it just injected.
for fn in compositor_handle_key compositor_handle_button
do
	require_fn "$fn" || continue
	if ! body "$fn" | grep -qE 'input_recorder != NULL && !synthetic'; then
		echo "FAIL: $fn taps the recorder without checking !synthetic."
		echo "      Injected input reaches this function, so gowl would"
		echo "      record its own injections and a synthesiser reading"
		echo "      the trace would replay its own output."
		fail=1
	fi
done

# The injection helpers must stay untapped.  Checked by function rather
# than by counting call sites, because the point is *which* functions
# record, not how many calls exist.
for fn in gowl_compositor_inject_pointer_motion \
	  gowl_compositor_inject_pointer_motion_absolute \
	  gowl_compositor_inject_button \
	  gowl_compositor_inject_axis \
	  gowl_compositor_inject_key \
	  gowl_compositor_motionnotify
do
	require_fn "$fn" || continue
	if body "$fn" | grep -qE '\brecording_note(_motion)?[[:space:]]*\('; then
		echo "FAIL: $fn records."
		echo "      Both the real and the synthetic path reach it, so"
		echo "      gowl would record its own injected input and a"
		echo "      synthesiser would be replaying its own output."
		echo "      Tap the wlr_cursor/keyboard listeners instead."
		fail=1
	fi
done

# The escape hatch is the only way out for somebody who did not start
# the recording and has no token.  It is not reachable from any test.
require_fn compositor_handle_key
if ! body compositor_handle_key | grep -q 'gowl_input_recorder_force_stop'; then
	echo "FAIL: compositor_handle_key no longer force-stops a recording."
	echo "      Super+Shift+Escape is the guaranteed way out of being"
	echo "      recorded; without it the only way to stop is to hold"
	echo "      the token."
	fail=1
fi

# The indicator must be raised from the recorder's own state change, not
# from whichever caller happened to notice.  A recording running with no
# indicator is the one state this feature must never reach.
if ! grep -q 'recording_indicator_sync' "$comp"; then
	echo "FAIL: the recording indicator is never synced."
	fail=1
fi
if ! awk '/^on_recording_changed\(/,/^}/' "$comp" \
	| grep -q 'recording_indicator_sync'; then
	echo "FAIL: on_recording_changed() no longer syncs the indicator."
	echo "      A recording with no visible frame is a keylogger."
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	echo "input-recording guard FAILED"
	exit 1
fi
echo "input-recording guard PASSED (taps wired, injection untapped)"
exit 0
