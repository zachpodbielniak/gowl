#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: the raw wlroots close and focus entry points must have exactly
# ONE call site each, inside the guarded wrappers that pick the right
# protocol for the client at hand.
#
# This exists because the duplicate cost a user their whole desktop.  The
# KILL_CLIENT keybind grew its own copy of the close call, without the
# `is this an X11 client?' check that gowl_client_close() had all along.
# For an XWayland client `xdg_toplevel' is NULL, so the keybind
# NULL-dereferenced inside wlroots and SIGSEGVd the compositor -- which
# under `emacs --gowl' *is* the session, so every application in it died
# too.  A unit test cannot catch a second copy appearing somewhere else
# in 8000 lines of compositor; this can.
#
# Reported by Ben Doty, 2026-08-08.  See tests/test-focus-rules.c for the
# routing truth table itself.

set -e
# An inherited CDPATH makes `cd' echo the resolved directory, which
# would land in $root alongside pwd's output and break every path here.
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
fail=0

# Count real call sites: strip /* */ comment bodies and preprocessor
# lines so prose mentioning a function does not trip the guard.
count_calls() {
	symbol=$1
	shift
	sed 's,/\*.*\*/,,' "$@" 2>/dev/null \
		| grep -vE '^[[:space:]]*[*#]' \
		| grep -cE "\\b${symbol}[[:space:]]*\(" || true
}

check_single_call() {
	symbol=$1
	owner=$2
	total=$(count_calls "$symbol" "$root"/src/*.c "$root"/src/*/*.c)
	owned=$(count_calls "$symbol" "$root/$owner")

	if [ "$total" -ne 1 ] || [ "$owned" -ne 1 ]; then
		echo "FAIL: $symbol has $total call site(s) ($owned in $owner)"
		echo "      expected exactly 1, inside $owner"
		grep -rnE "\\b${symbol}[[:space:]]*\(" "$root/src" \
			| grep -vE '^\S+:[0-9]+:[[:space:]]*[*#]' || true
		echo "      route close requests through gowl_client_close()"
		echo "      -- it picks XDG vs XWayland via"
		echo "      gowl_close_route_for() and cannot crash on an"
		echo "      X11 client."
		fail=1
	fi
}

# 1. Both close primitives live only in gowl_client_close().
check_single_call wlr_xdg_toplevel_send_close src/core/gowl-client.c
check_single_call wlr_xwayland_surface_close  src/core/gowl-client.c

# 2. The focus gate must actually be consulted.  If someone deletes the
#    gowl_focus_decide() call from gowl_compositor_focus_client(), every
#    guard -- session lock, embedded clients, layer grab, X11 popup grab
#    -- silently stops applying, and the unit tests still pass because
#    the rules module itself is untouched.
if ! grep -q 'gowl_focus_decide' "$root/src/core/gowl-compositor.c"; then
	echo "FAIL: gowl_compositor_focus_client() no longer calls"
	echo "      gowl_focus_decide() -- all focus guards are bypassed"
	fail=1
fi

# 3. Seat keyboard focus may only be moved from the four vetted places:
#    the two branches of gowl_compositor_focus_client() (which is gated
#    by gowl_focus_decide), the arrangelayers hand-off that establishes
#    the layer grab, and the session-lock surface (which outranks it).
#    Anything else moving seat focus bypasses the grab -- that is
#    exactly how a launcher ends up visible but deaf.
enters=$(count_calls wlr_seat_keyboard_notify_enter \
	"$root"/src/*.c "$root"/src/*/*.c)
if [ "$enters" -gt 4 ]; then
	echo "FAIL: $enters wlr_seat_keyboard_notify_enter call sites"
	echo "      (expected at most 4: two in focus_client, one in"
	echo "      arrangelayers, one for the session-lock surface)."
	echo "      New seat-focus paths -- in gowl or in an embedder --"
	echo "      must consult"
	echo "      gowl_compositor_has_exclusive_keyboard_layer() first."
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	echo "close/focus guard FAILED"
	exit 1
fi
echo "close/focus guard PASSED (single close route, focus gate intact)"
exit 0
