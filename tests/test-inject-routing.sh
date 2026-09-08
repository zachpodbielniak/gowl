#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: injected input must go through the compositor's own decision,
# never straight to the seat.
#
# A key or click handed to wlr_seat_*_notify_* arrives at whatever is
# focused, and at nothing else.  None of the compositor's own handling
# runs: not configured keybinds, not module keybinds, not the embedder
# intercept, not the bar's hit test, not focus-follows-click.
#
# That is not a subtle degradation.  It meant a software KVM driving
# this machine through the RemoteDesktop portal could type into
# applications but could not use one compositor binding -- Super+Return,
# the tag keys, the switcher all did nothing, because the keys went
# straight past them into the focused window.  The bar was not clickable
# at all from the remote pointer, while working perfectly from the
# physical one, because a bar draws scene buffers the compositor
# hit-tests itself.
#
# Nothing at runtime catches it: the input arrives, applications respond,
# and only the compositor's own surfaces are dead.
#
# Checked as source because it needs a seat, a keyboard group and a
# loaded bar module to observe -- none of which the test suite builds.

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
comp="$root/src/core/gowl-compositor.c"
fail=0

# Body of a function, from its definition to the closing brace in
# column 1, comments stripped.
body() {
	awk -v fn="$1" '
		$0 ~ "^" fn "\\(" { inside = 1 }
		inside { print }
		inside && /^}/ { exit }
	' "$comp" | sed 's,/\*.*\*/,,' | grep -vE '^[[:space:]]*[*#]'
}

# A name that does not exist yields an empty body and every check on it
# passes vacuously, so existence is checked first.
require_fn() {
	if ! grep -qE "^$1\(" "$comp"; then
		echo "FAIL: no function named $1 in gowl-compositor.c"
		fail=1
		return 1
	fi
	return 0
}

# 1. The injectors route through the shared decision.
if require_fn gowl_compositor_inject_key; then
	if ! body gowl_compositor_inject_key \
			| grep -q 'compositor_handle_key'; then
		echo "FAIL: gowl_compositor_inject_key does not go through"
		echo "      compositor_handle_key().  Injected keys will reach"
		echo "      the focused client and no keybind will fire."
		fail=1
	fi
	if body gowl_compositor_inject_key \
			| grep -q 'wlr_seat_keyboard_notify_key'; then
		echo "FAIL: gowl_compositor_inject_key sends to the seat"
		echo "      directly, bypassing every keybind check."
		fail=1
	fi
fi

if require_fn gowl_compositor_inject_button; then
	if ! body gowl_compositor_inject_button \
			| grep -q 'compositor_handle_button'; then
		echo "FAIL: gowl_compositor_inject_button does not go through"
		echo "      compositor_handle_button().  The bar, the tag boxes"
		echo "      and focus-follows-click will not see the click."
		fail=1
	fi
	if body gowl_compositor_inject_button \
			| grep -q 'wlr_seat_pointer_notify_button'; then
		echo "FAIL: gowl_compositor_inject_button sends to the seat"
		echo "      directly, bypassing the bar hit test."
		fail=1
	fi
fi

# 2. Injected motion moves the real cursor through the shared path, so
#    that a subsequent click hit-tests where the pointer actually is.
if require_fn gowl_compositor_inject_pointer_motion; then
	if ! body gowl_compositor_inject_pointer_motion \
			| grep -q 'gowl_compositor_motionnotify'; then
		echo "FAIL: injected motion does not call motionnotify, so the"
		echo "      cursor and the hit tests disagree about where the"
		echo "      pointer is."
		fail=1
	fi
fi

# 3. The shared decisions must still forward what nothing claimed, or
#    injection would type into a void.
if require_fn compositor_handle_key; then
	body compositor_handle_key | grep -q 'wlr_seat_keyboard_notify_key' || {
		echo "FAIL: compositor_handle_key never forwards to a client;"
		echo "      unclaimed keys would be swallowed."
		fail=1
	}
fi

# -- An injected scroll must not claim a wheel it cannot back up ------
#
# axis_source WHEEL tells the client the device has notches and that
# axis_value120 is the authoritative amount.  Sending WHEEL with a
# discrete value of zero therefore says "scroll by exactly nothing" to
# any client that believes the source, while a client that falls back to
# the continuous value scrolls normally.
#
# That is what made injected scrolling work in Chromium and GTK and do
# nothing at all in Firefox, on the same machine, from the same KVM,
# with no error anywhere.  The source must be DERIVED from the discrete
# amount, never asserted.
inject_axis_body=$(awk '
	/^gowl_compositor_inject_axis\(/ { inside = 1 }
	inside { print }
	inside && /^}/ { exit }' "$comp")

if [ -z "$inject_axis_body" ]; then
	echo "FAIL: no gowl_compositor_inject_axis in gowl-compositor.c"
	echo "      A guard naming a function that does not exist checks nothing."
	fail=1
else
	if ! echo "$inject_axis_body" | grep -q "WL_POINTER_AXIS_SOURCE_CONTINUOUS"; then
		echo "FAIL: injected scroll never reports a continuous source"
		echo "      A delta with no notches behind it IS continuous; calling"
		echo "      it a wheel promises a discrete amount there is none of."
		fail=1
	fi
	if ! echo "$inject_axis_body" | grep -qE "discrete != 0|discrete == 0"; then
		echo "FAIL: injected scroll does not derive its source from the discrete amount"
		fail=1
	fi
fi

# The portal must carry libei's discrete amount rather than dividing it
# away.  libei counts in 120ths, the same unit the wire uses; converting
# it to a continuous value and dropping it is what left the compositor
# announcing a wheel with nothing to put in it.
eis="$root/tools/xdg-desktop-portal-gowl/portal-eis.c"
if [ -f "$eis" ] && grep -q "EIS_EVENT_SCROLL_DISCRETE" "$eis"; then
	if ! awk '/EIS_EVENT_SCROLL_DISCRETE/,/break;/' "$eis" | grep -q "v120"; then
		echo "FAIL: $eis discards libei's v120 scroll amount"
		echo "      The compositor is then left with no discrete value to send."
		fail=1
	fi
fi

if [ "$fail" -eq 0 ]; then
	echo "inject-routing guard PASSED (injection takes the real path)"
fi
exit $fail
