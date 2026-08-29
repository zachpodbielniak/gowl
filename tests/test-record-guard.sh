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
# The second half is the more important one.  The taps live on the
# wlr_cursor and keyboard listeners precisely because the injection
# helpers bypass those and drive wlr_seat_*_notify_* directly.  A tap
# added to gowl_compositor_motionnotify() -- which both the real and the
# synthetic path call -- would start recording gowl's own synthetic
# input, and a synthesiser reading that trace would be replaying its own
# output.  It would look like it worked.

set -e
# An inherited CDPATH makes `cd' echo the resolved directory.
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
comp="$root/src/core/gowl-compositor.c"
fail=0

# Body of a static function, comments stripped, from its definition to
# the closing brace in column 1.  Prose mentioning a call must not count
# as one.
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

check_tap on_kb_key            recording_note
check_tap on_kb_modifiers      recording_note
check_tap on_cursor_button     recording_note
check_tap on_cursor_axis       recording_note
check_tap on_cursor_motion     recording_note_motion
check_tap on_cursor_motion_abs recording_note_motion

# The injection helpers must stay untapped.  Checked by function rather
# than by counting call sites, because the point is *which* functions
# record, not how many calls exist.
for fn in gowl_compositor_inject_pointer_motion \
	  gowl_compositor_inject_pointer_motion_absolute \
	  gowl_compositor_inject_pointer_button \
	  gowl_compositor_inject_pointer_axis \
	  gowl_compositor_inject_keyboard_key \
	  gowl_compositor_motionnotify
do
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
if ! body on_kb_key | grep -q 'gowl_input_recorder_force_stop'; then
	echo "FAIL: on_kb_key no longer force-stops a recording."
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
