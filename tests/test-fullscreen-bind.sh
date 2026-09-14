#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: both f keys are the focused WINDOW, not the tag's layout.
#
# Super+f used to select the float LAYOUT, which is a mode change for the
# whole tag made while reaching for fullscreen -- the more expensive of
# the two mistakes, and one that is not obvious afterwards because the
# windows simply stop tiling.  It became toggle_fullscreen and the float
# layout moved to Super+Shift+f.
#
# Super+Shift+f has since become toggle_float, which is the per-window
# operation people actually reach for, and the float layout now has no
# key of its own at all.  That is a deliberate trade and this guard is
# what keeps it honest: the layout has to stay REACHABLE -- registered,
# so the cycle keys find it -- even with nothing bound directly to it.
# Losing the binding is the decision; losing the layout would be a bug.
#
# There are two copies of the shipped defaults and they have drifted
# before: data/default-config.yaml, and the one src/main.c embeds for a
# session with no config file at all.  Both are checked, because a fix
# to one of them is a fix to half the users.
#
# cmacs --gowl carries its own copy in lisp/cmacs/cmacs-gowl.el, asserted
# by cmacs-gowl-test-default-keybinds-navigation-and-modes.

set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fail=0

yaml="$root/data/default-config.yaml"
main="$root/src/main.c"

# --- the YAML defaults ---
if [ -f "$yaml" ]; then
	if ! grep -q '^[[:space:]]*"Super+f":[[:space:]]*{[[:space:]]*action:[[:space:]]*toggle_fullscreen' "$yaml"; then
		echo "FAIL: default-config.yaml does not bind Super+f to toggle_fullscreen"
		fail=1
	fi
	if grep -q '^[[:space:]]*"Super+f":[[:space:]]*{[[:space:]]*action:[[:space:]]*set_layout' "$yaml"; then
		echo "FAIL: default-config.yaml still binds Super+f to a layout"
		fail=1
	fi
	if ! grep -q '^[[:space:]]*"Super+Shift+f":[[:space:]]*{[[:space:]]*action:[[:space:]]*toggle_float' "$yaml"; then
		echo "FAIL: default-config.yaml does not bind Super+Shift+f to toggle_float"
		fail=1
	fi
	if grep -q '^[[:space:]]*"Super+Shift+f":[[:space:]]*{[[:space:]]*action:[[:space:]]*set_layout' "$yaml"; then
		echo "FAIL: default-config.yaml still binds Super+Shift+f to a layout"
		fail=1
	fi
else
	echo "FAIL: data/default-config.yaml is missing"
	fail=1
fi

# --- the copy main.c embeds ---
if [ -f "$main" ]; then
	if ! grep -q 'Super+f\\": { action: toggle_fullscreen' "$main"; then
		echo "FAIL: the config embedded in main.c does not bind Super+f to"
		echo "      toggle_fullscreen"
		fail=1
	fi
	if ! grep -q 'Super+Shift+f\\": { action: toggle_float' "$main"; then
		echo "FAIL: the config embedded in main.c does not bind"
		echo "      Super+Shift+f to toggle_float"
		fail=1
	fi
fi

# --- and the float layout is still there to be cycled to ---
#
# The point of taking its key away is that it is rarely what anybody
# wants, not that it should stop existing.  A build that dropped the
# module would pass every check above and leave the cycle keys stepping
# over a layout that is simply gone.
if [ ! -f "$root/modules/float/gowl-layout-float.c" ]; then
	echo "FAIL: the float layout is gone; nothing can cycle to it"
	fail=1
fi
if ! grep -q '"float"' "$root/modules/float/gowl-module-float.c"; then
	echo "FAIL: the float module no longer registers a layout named float"
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	echo "fullscreen bind guard FAILED"
	exit 1
fi
echo "fullscreen bind guard PASSED (Super+f fullscreens, Super+Shift+f floats;"
echo "                              the float layout is cycled to)"
exit 0
