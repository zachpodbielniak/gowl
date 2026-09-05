#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: every input device type wlroots can report must be routed
# somewhere, and the `default:' arm of the new-input switch must not be
# a bare `break;'.
#
# This exists because that is exactly how tablets were lost.  The switch
# handled KEYBOARD, POINTER and SWITCH and dropped everything else into
# `default: break;', so a Wacom plugged into a gowl session did nothing
# at all -- not a stroke, not a click, not even cursor motion -- and
# nothing anywhere said so.  A silently ignored device produces no
# warning, no error and no log line; the only symptom is hardware that
# does not work, which is indistinguishable from a driver problem.
#
# A unit test cannot see this: the switch compiles and runs perfectly
# well while ignoring half the devices on the machine.

set -e
# An inherited CDPATH makes `cd' echo the resolved directory.
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
comp="$root/src/core/gowl-compositor.c"
fail=0

# 1. The tablet handler is reachable from the new-input path.
if ! grep -qE '\bgowl_tablet_new_device[[:space:]]*\(' "$comp"; then
	echo "FAIL: gowl_tablet_new_device() is never called from"
	echo "      gowl-compositor.c, so tablets reach no handler."
	fail=1
fi

# 2. The tablet-v2 global is created at startup.  Without it
#    gowl_tablet_new_device() refuses every device, so the call site
#    above alone is not enough.
if ! grep -qE '\bgowl_tablet_manager_init[[:space:]]*\(' "$comp"; then
	echo "FAIL: gowl_tablet_manager_init() is never called, so the"
	echo "      tablet-v2 global is never created and every tablet"
	echo "      is refused."
	fail=1
fi

# 3. The default arm of the device switch does something.  Extract the
#    lines between `switch (device->type)' and its closing brace, then
#    require that `default:' is followed by a call rather than only a
#    break.
awk '
	/switch[[:space:]]*\([[:space:]]*device->type[[:space:]]*\)/ { inswitch=1 }
	inswitch && /^[[:space:]]*default:/ { indefault=1; next }
	indefault && /^[[:space:]]*}/ { exit }
	indefault { print }
' "$comp" > /tmp/gowl-default-arm.$$ 2>/dev/null || true

if [ -s /tmp/gowl-default-arm.$$ ]; then
	# Strip comments and the break, and see whether anything is left.
	body=$(sed 's,/\*.*\*/,,' /tmp/gowl-default-arm.$$ \
		| grep -vE '^[[:space:]]*(\*|/\*|//)' \
		| grep -vE '^[[:space:]]*break;[[:space:]]*$' \
		| grep -vE '^[[:space:]]*$' || true)
	if [ -z "$body" ]; then
		echo "FAIL: the default arm of the device switch is a bare"
		echo "      break -- some device type is being dropped with"
		echo "      no handler and no warning.  That is how tablets"
		echo "      went unnoticed."
		fail=1
	fi
else
	echo "FAIL: could not find the default arm of the new-input switch"
	echo "      (did on_new_input change shape?).  This guard needs"
	echo "      updating rather than deleting."
	fail=1
fi
rm -f /tmp/gowl-default-arm.$$

if [ "$fail" -eq 0 ]; then
	echo "input-device guard PASSED (tablets routed, default arm acts)"
fi
exit $fail
