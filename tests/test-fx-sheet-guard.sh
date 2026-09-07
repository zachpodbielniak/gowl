#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: a capture that hides the client layers must hide the effect
# sheets too.
#
# A GowlFxSheet is a direct child of scene->tree -- a SIBLING of the
# layer trees, not inside one -- so that an effect can park it above or
# below whole layers.  The cost is that hiding "every client layer" does
# NOT hide it, and a sheet is not decoration: it is an opaque,
# monitor-sized picture of the desktop WITH its windows in it, left there
# by the cube, expo and switcher while they own the output.
#
# The bug that motivated this: blur rebuilds its wallpaper backdrop by
# capturing the output with every client layer hidden.  Do that during a
# tag switch, while the cube's sheet is parked, and the capture is the
# sheet -- so the tag being LEFT gets blurred into the backdrop and
# cached as the current one.  On screen: a translucent terminal on tag 2
# showing ghosts of the chat window from tag 4.
#
# It is a source guard rather than a unit test because building a sheet
# needs a live compositor, an output and a renderer; the invariant is
# "these two calls travel together", which is exactly a source property.

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
fail=0
checked=0

# Every file that hides a client layer.
for f in $(grep -rl "gowl_fx_vis_hide_layer" "$root/src" "$root/modules" \
	2>/dev/null | grep -v "gowl-fx-capture.c"); do
	rel=${f#"$root"/}

	# Only care about sweeps that hide the WINDOW layers -- an effect
	# hiding one specific layer for its own reasons is not a capture.
	if ! grep -q "GOWL_SCENE_LAYER_TILE" "$f"; then
		continue
	fi
	checked=$((checked + 1))

	if ! grep -q "gowl_fx_vis_hide_sheets" "$f"; then
		echo "FAIL: $rel hides the client layers but not the effect"
		echo "      sheets.  A sheet is a sibling of the layer trees and"
		echo "      holds a picture of the desktop WITH its windows, so"
		echo "      this capture will pick up whatever an effect last"
		echo "      parked -- including the tag you just left."
		echo "      Add gowl_fx_vis_hide_sheets(vis)."
		fail=1
	fi
done

# The helper itself must still exist and still walk the live sheets.
if ! grep -q "gowl_fx_sheet_live" "$root/src/fx/gowl-fx-capture.c"; then
	echo "FAIL: gowl_fx_vis_hide_sheets no longer consults the live-sheet"
	echo "      registry, so it hides nothing"
	fail=1
fi

# And the registry must be kept up to date at both ends.
if ! grep -q "fx_live_sheets = g_list_prepend" "$root/src/fx/gowl-fx-sheet.c" \
	|| ! grep -q "fx_live_sheets = g_list_remove" "$root/src/fx/gowl-fx-sheet.c"; then
	echo "FAIL: the live-sheet registry is not maintained on both create"
	echo "      and free; a freed sheet left in it is a use-after-free in"
	echo "      the next capture"
	fail=1
fi

if [ "$checked" -eq 0 ]; then
	echo "SKIP: no capture sites found"
	exit 0
fi

if [ "$fail" -eq 0 ]; then
	echo "PASS: fx-sheet guard ($checked capture site(s) hide sheets too)"
fi

exit $fail
