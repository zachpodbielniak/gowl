#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: the tag transitions stay good citizens among the effect modules.
#
# Every transition module sorts ahead of the animation module so it can
# swallow the reveal fade of a switch it is about to animate.  That
# position is only safe because the shared host claims NOTHING else: if
# its client_event ever returned TRUE more broadly, every module behind
# the transitions -- window animations first -- would silently stop
# receiving events, and the symptom is an absence, not a crash.
# tests/test-tag-transition.c checks the call; this checks the source,
# and the rules a new transition module has to keep:
#
#   - no GL in a module (src/fx owns all of it, AGENTS.md);
#   - no shared scene layer switched off for a run (layers are global,
#     tags per-output: it would blank the other screen);
#   - its name is in the selector's canonical tour, or Super+: would put
#     it wherever it happened to load.

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
host="$root/src/fx/gowl-fx-transition.c"
macro="$root/src/fx/gowl-fx-transition-module.h"
registry="$root/src/core/gowl-tag-transition.c"
fail=0

for f in "$host" "$macro" "$registry"; do
	if [ ! -f "$f" ]; then
		echo "FAIL: $f is missing"
		exit 1
	fi
done

body_of() {
	awk -v fn="$1" '
		$0 ~ "^" fn "\\(" { inside = 1 }
		inside { print }
		inside && /^}/ { exit }
	' "$2"
}

# 1. The host claims only the reveal, and only conditionally.
ce=$(body_of gowl_fx_transition_host_client_event "$host")
if ! printf '%s\n' "$ce" | grep -q 'GOWL_SCENE_EFFECT_REVEAL'; then
	echo "FAIL: the transition host's client_event no longer tests for REVEAL"
	fail=1
fi
if printf '%s\n' "$ce" | grep -qE '^[[:space:]]*return TRUE;'; then
	echo "FAIL: the transition host's client_event returns TRUE"
	echo "      unconditionally; it would swallow every window event"
	fail=1
fi

# 2. Transitions are asked before the default-priority providers.
if ! grep -qE '#define GOWL_FX_TRANSITION_PRIORITY +\(-[0-9]+\)' "$macro"; then
	echo "FAIL: GOWL_FX_TRANSITION_PRIORITY is no longer ahead of the"
	echo "      default; the transitions could not swallow the reveal"
	fail=1
fi

# 3. The host asks the arbiter before starting.
if ! body_of host_wants "$host" | grep -q 'gowl_compositor_tag_transition_is'; then
	echo "FAIL: the transition host starts without asking"
	echo "      gowl_compositor_tag_transition_is(); every loaded"
	echo "      transition would animate every switch at once"
	fail=1
fi

# 4. Per module.
order=$(awk '/canonical_order\[\] = \{/,/NULL/' "$registry")
for src in "$root"/modules/*/*.c; do
	grep -q 'GOWL_FX_DEFINE_TRANSITION_MODULE' "$src" || continue
	rel=${src#"$root"/}

	if grep -nE '\bgl[A-Z][A-Za-z0-9]*[[:space:]]*\(|#include <GLES' "$src" \
	   >/dev/null; then
		echo "FAIL: $rel calls GL directly; drawing goes through gowl_fx_*"
		fail=1
	fi
	if grep -nE 'wlr_scene_node_set_enabled' "$src" >/dev/null; then
		echo "FAIL: $rel toggles scene nodes itself; a transition takes"
		echo "      the output with the host's sheet, never by hiding a"
		echo "      shared layer"
		fail=1
	fi
	name=$(sed -nE 's/^[[:space:]]*\.name[[:space:]]*=[[:space:]]*"([a-z0-9_-]+)".*/\1/p' \
	       "$src" | head -n 1)
	if [ -z "$name" ]; then
		echo "FAIL: $rel defines a transition without a \`.name = \"...\"'"
		fail=1
	elif ! printf '%s\n' "$order" | grep -q "\"$name\""; then
		echo "FAIL: $rel's transition \"$name\" is not in the canonical"
		echo "      tour in src/core/gowl-tag-transition.c"
		fail=1
	fi
done

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "PASS: tag transitions claim only their reveal and stay GL-free"
exit 0
