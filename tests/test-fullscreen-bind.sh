#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: Super+f fullscreens the focused WINDOW.
#
# It used to select the float LAYOUT, which is a mode change for the
# whole tag made while reaching for fullscreen -- the more expensive of
# the two mistakes, and one that is not obvious afterwards because the
# windows simply stop tiling.  The float layout kept a key of its own on
# Super+Shift+f.
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
	# The float layout must still be reachable, or this trade just
	# loses it.
	if ! grep -q '"Super+Shift+f":.*set_layout.*float' "$yaml"; then
		echo "FAIL: default-config.yaml does not bind the float layout to Super+Shift+f"
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
	if ! grep -q 'Super+Shift+f\\": { action: set_layout, arg: \\"float' "$main"; then
		echo "FAIL: the config embedded in main.c does not bind the float"
		echo "      layout to Super+Shift+f"
		fail=1
	fi
fi

if [ "$fail" -ne 0 ]; then
	echo "fullscreen bind guard FAILED"
	exit 1
fi
echo "fullscreen bind guard PASSED (Super+f fullscreens; float on Super+Shift+f)"
exit 0
