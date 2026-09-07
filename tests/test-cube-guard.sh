#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: the cube must stay a GOOD CITIZEN among the effect modules.
#
# gowl-effects.c hands consumable hooks to the first provider that claims
# them, and the cube sorts ahead of every other effect module so it can
# claim the one it needs.  That position is only safe because it claims
# almost nothing: if its client_event ever starts returning TRUE for
# events it does not handle, every module behind it -- window animations
# first -- silently stops receiving them.  tests/test-effects.c pins the
# dispatch semantics; this pins the cube's side of the bargain, which is a
# property of the source rather than of a call.
#
# Also checked: the cube must never switch off a SHARED scene layer for
# the duration of a rotation.  The layers are global and the tags are
# per-monitor, so hiding one to make room for the cube on one screen
# blanks the other.

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
src="$root/modules/cube/gowl-module-cube.c"
fail=0

if [ ! -f "$src" ]; then
	echo "FAIL: $src is missing"
	exit 1
fi

# Body of a function, from its definition line to the next brace in
# column one.
body_of() {
	awk -v fn="$1" '
		$0 ~ "^" fn "\\(" { inside = 1 }
		inside { print }
		inside && /^}/ { exit }
	' "$src"
}

# 1. The cube must be asked before the default-priority providers, or it
#    cannot claim the reveal it needs to suppress.
if ! grep -qE '#define GOWL_CUBE_PRIORITY +\(-[0-9]+\)' "$src"; then
	echo "FAIL: the cube's priority is no longer ahead of the default;"
	echo "      it would be asked after the animation module and could no"
	echo "      longer suppress the reveal it is about to animate"
	fail=1
fi

# 2. client_event must be conditional on the one event it owns.  A bare
#    `return TRUE' anywhere in it would swallow every client event.
ce=$(body_of cube_client_event)
if ! printf '%s\n' "$ce" | grep -q 'GOWL_SCENE_EFFECT_REVEAL'; then
	echo "FAIL: cube_client_event no longer tests for REVEAL"
	fail=1
fi
if printf '%s\n' "$ce" | grep -qE '^[[:space:]]*return TRUE;'; then
	echo "FAIL: cube_client_event returns TRUE unconditionally; with the"
	echo "      cube's priority that swallows every client event before"
	echo "      the animation module can see it"
	fail=1
fi

# 3. The cube must not implement the hooks that decide where a window is.
#    Those are consumable, it sorts first, and it has no opinion on them --
#    installing one would take them away from the animation module.
for hook in get_geometry surface_at alpha_changed; do
	if grep -qE "iface->$hook[[:space:]]*=" "$src"; then
		echo "FAIL: the cube installs iface->$hook; it sorts ahead of the"
		echo "      animation module, so it would answer for windows it"
		echo "      knows nothing about"
		fail=1
	fi
done

# 4. Taking the output for a rotation must not touch a shared scene layer.
#    Captures may (they are transient and never presented), so the check is
#    scoped to cube_take_output.
if body_of cube_take_output | grep -q 'set_enabled(&self->layers'; then
	echo "FAIL: cube_take_output disables a shared scene layer; the layers"
	echo "      are global and tags are per-monitor, so this blanks every"
	echo "      OTHER output for the length of the rotation"
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	echo "cube guard FAILED"
	exit 1
fi
echo "cube guard PASSED (claims only its own reveal, takes no shared layer)"
exit 0
