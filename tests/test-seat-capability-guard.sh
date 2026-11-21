#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: the seat's capabilities are decided in exactly one place.
#
# They used to be computed twice -- in the new-input handler and again
# in the input-remap core -- each from "is a physical keyboard plugged
# in".  Neither knew about virtual keyboards, so a machine with no
# hardware keyboard never advertised wl_keyboard: keys from an on-screen
# keyboard or wtype reached no client, and toolkit clients never enabled
# their text input.  Teaching one copy and not the other would have been
# worse than teaching neither: the capability would come and go with
# whichever ran last.
#
# So gowl_compositor_update_seat_capabilities() is the only caller of
# wlr_seat_set_capabilities() in src/, the rule it applies counts
# virtual keyboards and the forced setting, and the events that change
# the answer all call it.  A unit test cannot see a second copy of the
# rule: it would compile, and pass every test that does not happen to
# exercise the path it sits on.

set -e
# An inherited CDPATH makes `cd' echo the resolved directory.
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
comp="$root/src/core/gowl-compositor.c"
text="$root/src/core/gowl-text-input.c"
remap="$root/src/core/gowl-input-remap-core.c"
fail=0

# 1. One caller, in the compositor.
callers=$(grep -rlE '\bwlr_seat_set_capabilities[[:space:]]*\(' "$root/src" \
	--include='*.c' | sort)
if [ "$callers" != "$comp" ]; then
	echo "FAIL: wlr_seat_set_capabilities() must be called only from"
	echo "      gowl_compositor_update_seat_capabilities() in"
	echo "      src/core/gowl-compositor.c.  Callers found:"
	printf '%s\n' "$callers" | sed 's/^/        /'
	fail=1
fi
n=$(grep -cE '\bwlr_seat_set_capabilities[[:space:]]*\(' "$comp" || true)
if [ "$n" != 1 ]; then
	echo "FAIL: gowl-compositor.c calls wlr_seat_set_capabilities()"
	echo "      $n times; the one call belongs in"
	echo "      gowl_compositor_update_seat_capabilities()."
	fail=1
fi

# 2. The rule counts what it must.
rule=$(sed -n '/^gowl_compositor_seat_has_keyboard(/,/^}/p' "$comp")
for need in gowl_text_input_has_virtual_keyboard \
	gowl_input_remap_core_has_claimed_keyboard \
	gowl_compositor_get_force_seat_keyboard \
	wlr_kb_group; do
	if ! printf '%s\n' "$rule" | grep -q "$need"; then
		echo "FAIL: gowl_compositor_seat_has_keyboard() no longer"
		echo "      consults $need."
		fail=1
	fi
done

# 3. Everything that changes the answer asks for it to be re-decided.
count_updates() {
	grep -cE '\bgowl_compositor_update_seat_capabilities[[:space:]]*\(' "$1" \
		|| true
}
# a virtual keyboard arriving, and one leaving
if [ "$(count_updates "$text")" -lt 2 ]; then
	echo "FAIL: gowl-text-input.c must update the seat's capabilities"
	echo "      both when a virtual keyboard is created and when one is"
	echo "      destroyed."
	fail=1
fi
# a remapper claiming or releasing a device
if [ "$(count_updates "$remap")" -lt 1 ]; then
	echo "FAIL: gowl-input-remap-core.c no longer updates the seat's"
	echo "      capabilities after re-deciding its claims."
	fail=1
fi
# a device arriving, the config being replaced, startup, the setter, and
# the definition itself
if [ "$(count_updates "$comp")" -lt 5 ]; then
	echo "FAIL: gowl-compositor.c must update the seat's capabilities on"
	echo "      new input, in set_config, at start (a headless seat gets"
	echo "      no hotplug to do it) and in the force setter."
	fail=1
fi

[ "$fail" = 0 ] && echo "PASS: the seat's capabilities are decided in one place"
exit $fail
