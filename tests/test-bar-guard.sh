#!/bin/sh
#
# gowl - source guard for the bar's containment invariants
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Four properties that no unit test can reach, and that all fail
# silently -- the bar keeps working right up until the day it takes the
# session with it.

set -e

cd "$(dirname "$0")/.."

fail() {
	echo "FAIL: $1" >&2
	exit 1
}

# 1. Every plugin entry point the bar calls runs under the fault guard.
#
# Bar plugins are in-process, so an unguarded call into one is a
# potential logout.  These are the five surfaces a plugin exposes; each
# must have a guarded call site in the module.
for what in bar_poll_body bar_work_body bar_click_body \
            bar_build_panel_body bar_panel_action_body; do
	grep -q "gowl_bar_guard_call($what" modules/bar/gowl-module-bar.c ||
		fail "the bar module no longer runs $what under the fault guard"
done

# 2. Unloading a plugin must not unmap its code.
#
# A GType, a queued worker or a pending callback can still point into a
# module the registry has dropped.  Keeping it mapped costs tens of
# kilobytes; closing it costs a crash that looks like it came from
# somewhere else entirely.
grep -q "g_module_make_resident" src/barkit/gowl-bar-registry.c ||
	fail "the bar registry no longer keeps plugin modules resident"
if grep -q "g_module_close" src/barkit/gowl-bar-registry.c; then
	fail "the bar registry closes plugin modules; see gowl_bar_registry_unload"
fi

# 3. Panels and toasts float above fullscreen windows.
#
# The bar itself belongs on the TOP layer with the other panels, but a
# dropdown or a notification on that layer disappears behind a
# fullscreen window -- which is exactly when a critical toast matters
# most.
grep -q "GOWL_SCENE_LAYER_TOP" modules/bar/gowl-module-bar.c ||
	fail "the bar surface is no longer created on the TOP scene layer"
overlay_uses=$(grep -c "GOWL_SCENE_LAYER_OVERLAY" modules/bar/gowl-module-bar.c || true)
[ "$overlay_uses" -ge 2 ] ||
	fail "panels and toasts must both use the OVERLAY scene layer"

# 4. A shipped plugin never hard-codes a colour.
#
# Plugins name theme roles so one palette setting restyles the whole
# bar, third-party widgets included.  A hex literal in a plugin is a
# widget that stays Mocha-blue when the user switches to Latte.
for f in modules/bar/bar-plugins-*.c; do
	if grep -n '"#[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]' "$f" >/dev/null; then
		grep -n '"#[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]' "$f" >&2
		fail "$f hard-codes a colour; name a GowlBarColor role instead"
	fi
done

# 5. The shipped layout is replaced, not added to.
#
# A configuration written before regions existed sets only `widgets'.
# If the shipped centre clock survives that, the bar shows the time
# twice -- and if the left is cleared without putting the tag row and
# title back, upgrading silently costs every such configuration both.
grep -q "gowl_bar_layout_config_kind" modules/bar/gowl-module-bar.c ||
	fail "the bar no longer replaces its shipped layout on first configure"
grep -q 'GOWL_BAR_CONFIG_LEGACY' modules/bar/gowl-module-bar.c ||
	fail "the bar no longer restores tags+title for a pre-regions config"

# 6. Blocking work stays off the compositor's dispatch thread.
#
# That thread holds the lock every editor primitive needs, so a
# subprocess spawned from a poll, a draw or a panel action freezes the
# editor as well as the bar.  The synchronous helpers are only legal
# from an async poll or from queued work.
grep -q "g_spawn_sync" modules/bar/gowl-module-bar.c ||
	fail "bar_run_argv no longer exists; plugins have no safe spawn helper"
for f in modules/bar/bar-plugins-*.c; do
	if grep -q "g_spawn_sync\|g_spawn_command_line_sync" "$f"; then
		fail "$f spawns synchronously; use bar_run_argv from poll_async"
	fi
done

echo "PASS: bar source guards"
