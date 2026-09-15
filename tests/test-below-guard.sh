#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: the two things behaviour tests cannot reach.
#
# 1. THE MAP PATH CALLS THE OPT-OUT.  Whether a window gets effects is
#    decided in gowl_compositor_apply_fx_optout(), which is unit-tested
#    on its own (tests/test-fx-optout.c) and tested against the real
#    blur module (tests/test-blur-nodes.c).  What neither can test is
#    that on_client_map() actually CALLS it -- a mapped window needs a
#    real Wayland client, which no test rig here has.  Delete the call
#    and every test still passes while the feature is dead.
#
# 2. THE SHIPPED KEYS.  Super+Alt+b and Super+Ctrl+Alt+b are the whole
#    user-visible contract, and there are two copies of the defaults
#    that have drifted before: data/default-config.yaml and the one
#    src/main.c embeds for a session with no config file at all.
#
# cmacs --gowl carries a third copy in lisp/cmacs/cmacs-gowl.el, which
# its own ERT suite asserts.

set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fail=0

comp="$root/src/core/gowl-compositor.c"
yaml="$root/data/default-config.yaml"
main="$root/src/main.c"

# --- the opt-out is wired into the map path ---
if [ -f "$comp" ]; then
	if ! grep -q 'gowl_compositor_apply_fx_optout(self, c);' "$comp"; then
		echo "FAIL: nothing in the compositor calls gowl_compositor_apply_fx_optout();"
		echo "      GOWL_NO_FX and the Steam exclusion are dead"
		fail=1
	fi
else
	echo "FAIL: src/core/gowl-compositor.c is missing"
	fail=1
fi

# --- the keys, in both copies of the defaults ---
if [ -f "$yaml" ]; then
	if ! grep -q '^[[:space:]]*"Super+Alt+b":' "$yaml"; then
		echo "FAIL: default-config.yaml does not bind Super+Alt+b"
		fail=1
	fi
	if ! grep -q 'action:[[:space:]]*toggle_below,' "$yaml"; then
		echo "FAIL: default-config.yaml does not use the toggle_below action"
		fail=1
	fi
	if ! grep -q '^[[:space:]]*"Super+Ctrl+Alt+b":' "$yaml"; then
		echo "FAIL: default-config.yaml does not bind Super+Ctrl+Alt+b"
		fail=1
	fi
	if ! grep -q 'action:[[:space:]]*toggle_below_all,' "$yaml"; then
		echo "FAIL: default-config.yaml does not use the toggle_below_all action"
		fail=1
	fi
	if ! grep -q '^no-fx-apps:' "$yaml"; then
		echo "FAIL: default-config.yaml no longer documents no-fx-apps"
		fail=1
	fi
else
	echo "FAIL: data/default-config.yaml is missing"
	fail=1
fi

if [ -f "$main" ]; then
	if ! grep -q 'Super+Alt+b\\": { action: toggle_below }' "$main"; then
		echo "FAIL: the config embedded in main.c does not bind Super+Alt+b"
		echo "      to toggle_below"
		fail=1
	fi
	if ! grep -q 'Super+Ctrl+Alt+b\\": { action: toggle_below_all }' "$main"; then
		echo "FAIL: the config embedded in main.c does not bind"
		echo "      Super+Ctrl+Alt+b to toggle_below_all"
		fail=1
	fi
fi

if [ "$fail" -ne 0 ]; then
	echo "below guard FAILED"
	exit 1
fi
echo "below guard PASSED (the map path opts windows out; Super+Alt+b and"
echo "                    Super+Ctrl+Alt+b are bound in both defaults)"
exit 0
