#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: every virtual gamepad the inputremap module creates through
# /dev/uinput is destroyed again -- when no rule declares it, when the
# module is disabled, when the compositor goes and when the module is
# finalized.
#
# A uinput device outlives nothing by itself: it lives exactly as long
# as its fd.  But a pad left behind while the module is off is a
# controller a game still sees, frozen in whatever state it was last
# in -- a stick held left, a trigger pulled -- with nothing that can
# release it.  test-input-remap-gamepad checks /proc/bus/input/devices
# before and after at runtime where /dev/uinput is writable; this
# checks the structure everywhere, including on machines where that
# test skips.
#
# The reader side is held too: the core must never read one of gowl's
# own pads back as an input (a feedback loop), and the module must not
# create a device anywhere but vpad_create().

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
vpad="$root/modules/inputremap/gowl-inputremap-vpad.c"
mod="$root/modules/inputremap/gowl-module-inputremap.c"
pad="$root/src/core/gowl-input-remap-pad.c"
fail=0

body() {
	awk -v fn="$2" '
		$0 ~ "^" fn "\\(" { inside = 1 }
		inside { print }
		inside && /^}/ { exit }
	' "$1" | sed 's,/\*.*\*/,,' | grep -vE '^[[:space:]]*[*#]'
}

need() {
	# need FILE FUNCTION PATTERN MESSAGE
	if ! grep -qE "^$2\\(" "$1"; then
		echo "FAIL: no function $2 in $(basename "$1")"
		fail=1
		return
	fi
	if ! body "$1" "$2" | grep -qE "$3"; then
		echo "FAIL: $4"
		fail=1
	fi
}

# 1. Exactly one place creates a device, and the device is destroyed
#    before its fd is closed.
creates=$(grep -vE '^[[:space:]]*[*/]' "$vpad" | grep -c 'UI_DEV_CREATE' || true)
if [ "$creates" -ne 1 ]; then
	echo "FAIL: UI_DEV_CREATE appears $creates times in the vpad code;"
	echo "      vpad_create() is the one place a device is made."
	fail=1
fi
need "$vpad" vpad_destroy_device 'UI_DEV_DESTROY' \
	"vpad_destroy_device() no longer sends UI_DEV_DESTROY."
need "$vpad" vpad_free 'vpad_destroy_device' \
	"vpad_free() no longer destroys the device it frees."

# 2. Every way out of the module takes the pads with it.
need "$mod" ir_deactivate 'gowl_inputremap_vpads_destroy_all' \
	"disabling the module leaves its virtual pads plugged in."
need "$mod" ir_display_destroyed 'gowl_inputremap_vpads_destroy_all' \
	"the compositor going away leaves the virtual pads plugged in."
need "$mod" gowl_module_inputremap_finalize 'gowl_inputremap_vpads_free' \
	"finalizing the module leaks its virtual pads."
need "$mod" ir_reevaluate 'gowl_inputremap_vpads_sync' \
	"a rule change no longer destroys pads no rule declares."

# 3. The sync runs AFTER the core re-decided the devices, so a release
#    made by that re-decision reaches the pad it pressed.
order=$(body "$mod" ir_reevaluate | grep -nE 'input_remap_reevaluate|vpads_sync' \
	| cut -d: -f2 | tr -d '[:space:]')
case "$order" in
	*input_remap_reevaluate*vpads_sync*) ;;
	*)
		echo "FAIL: ir_reevaluate must re-decide claims before it"
		echo "      destroys pads; the other way round strands a held"
		echo "      button on a pad that is about to vanish."
		fail=1
		;;
esac

# 4. The reader never takes one of gowl's pads as an input.
need "$pad" pad_discover 'GOWL_INPUT_REMAP_PAD_NAME_PREFIX' \
	"the controller reader no longer skips gowl's own virtual pads."

# 5. Virtual pads exist only on the compositor's loop, never GLib's
#    default context.
if grep -vE '^[[:space:]]*[*/]' "$vpad" \
	| grep -qE 'g_timeout_add|g_idle_add|g_unix_fd_add|g_io_add_watch'; then
	echo "FAIL: the vpad code registers on GLib's default context."
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	echo "input-remap uinput guard FAILED"
	exit 1
fi
echo "input-remap uinput guard PASSED (virtual pads always destroyed, never read back)"
exit 0
