#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: the cube must stay a GOOD CITIZEN of the single-scene-effect-owner
# rule, not a beneficiary of it.
#
# gowl dispatches scene presentation to exactly one provider -- the first
# active one by priority -- so a module that takes the top slot and does
# not pass on what it does not handle silently switches every other
# presentation module off.  The cube takes the top slot on purpose (it has
# to be asked first so it can decline), which makes the delegation in
# every one of its hooks the thing standing between "cube plus window
# animations" and "cube instead of window animations".  A unit test cannot
# reach that: it is a property of the source, and the symptom of losing it
# is not a failure but a quiet absence.
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

# 1. Every scene-effect hook the cube installs must consult the provider
#    below it.  The list is taken from the interface, so a hook added to
#    GowlSceneEffect later and wired up here without delegating fails.
for hook in cube_client_event cube_get_geometry cube_alpha_changed \
            cube_surface_at cube_frame cube_frame_done \
            cube_monitor_removed cube_finish; do
	if ! grep -qE "iface->[a-z_]+[[:space:]]*=[[:space:]]*$hook;" "$src"; then
		echo "FAIL: $hook is not installed on GowlSceneEffect any more;"
		echo "      update this guard or restore the hook"
		fail=1
		continue
	fi
	# Body: from the definition line to the next closing brace in column 1.
	body=$(awk -v fn="$hook" '
		$0 ~ "^" fn "\\(" { inside = 1 }
		inside { print }
		inside && /^}/ { exit }
	' "$src")
	if ! printf '%s\n' "$body" | grep -q 'cube_next'; then
		echo "FAIL: $hook does not delegate to the next scene-effect provider;"
		echo "      with the cube loaded, that hook stops reaching the"
		echo "      animation module entirely"
		fail=1
	fi
done

# 2. The delegation has to be the priority-ordered chain, not a hard-coded
#    reach for a module by name.
if ! grep -q 'gowl_module_manager_get_scene_effect_after' "$src"; then
	echo "FAIL: the cube no longer chains through the module manager"
	fail=1
fi

# 3. The cube must be asked before the default-priority providers, or it
#    never gets the chance to decline and hand over.
if ! grep -qE '#define GOWL_CUBE_PRIORITY +\(-[0-9]+\)' "$src"; then
	echo "FAIL: the cube's priority is no longer ahead of the default;"
	echo "      it would be dispatched after the animation module and"
	echo "      never own a frame"
	fail=1
fi

# 4. Taking the output for a rotation must not touch a shared scene layer.
#    Captures may (they are transient and never presented), so the check is
#    scoped to cube_take_output.
take=$(awk '
	/^cube_take_output\(/ { inside = 1 }
	inside { print }
	inside && /^}/ { exit }
' "$src")
if printf '%s\n' "$take" | grep -q 'set_enabled(&self->layers'; then
	echo "FAIL: cube_take_output disables a shared scene layer; the layers"
	echo "      are global and tags are per-monitor, so this blanks every"
	echo "      OTHER output for the length of the rotation"
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	echo "cube guard FAILED"
	exit 1
fi
echo "cube guard PASSED (delegation intact, no shared layer taken)"
exit 0
