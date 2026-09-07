#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: an image that outlives the frame must live in a buffer we own.
#
# gowl_fx_capture_to_buffer() hands back a slot of the OUTPUT's
# swapchain.  That is fine to present and drop inside one frame, and
# wrong to keep.  Keeping one takes a buffer permanently out of the
# rotation the output needs, and -- the part that bites -- ties the
# content to a pool the compositor keeps drawing the live desktop into.
# If the slot is ever handed back out (a swapchain recreated on a mode
# or format change, a lock dropped, a scanout path that does not consult
# the lock) what you are holding stops being your capture and becomes a
# photograph of the desktop with its windows in it.
#
# The blur backdrop did exactly that: it captured the wallpaper with
# every window hidden, then held the output's buffer for as long as the
# tag set did not change.  The reported symptom was a translucent window
# on one tag showing another tag's application through it, intermittently
# -- which is what a stale desktop frame looks like when it reappears
# underneath a translucent window.
#
# GowlFxSheet already allocated its own swapchain.  This keeps the rest
# of the tree honest.

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
fail=0

# 1. Nothing may retain the output's buffer.  Any caller at all is
#    suspect, so this reports them for review rather than allowing a
#    list of blessed ones: there is currently no legitimate caller.
callers=$(grep -rln "gowl_fx_capture_to_buffer" "$root/src" "$root/modules" \
	2>/dev/null | grep -v "src/fx/gowl-fx-capture.c" | grep -v "src/fx/gowl-fx.h" || true)
if [ -n "$callers" ]; then
	echo "FAIL: gowl_fx_capture_to_buffer has callers:"
	echo "$callers" | sed 's/^/        /'
	echo "      It returns a slot of the OUTPUT's swapchain.  If the"
	echo "      image must outlive the frame, allocate your own buffer"
	echo "      with wlr_swapchain_create() as GowlFxSheet and the blur"
	echo "      backdrop do."
	fail=1
fi

# 2. The blur backdrop keeps its buffer across frames, so it must own
#    one.
blur="$root/modules/blur/gowl-module-blur.c"
if [ -f "$blur" ]; then
	if ! grep -q "wlr_swapchain_create" "$blur"; then
		echo "FAIL: the blur backdrop no longer allocates its own"
		echo "      buffer.  It holds the backdrop for as long as the"
		echo "      tag set is unchanged, so a borrowed output buffer"
		echo "      will eventually show the live desktop through every"
		echo "      translucent window."
		fail=1
	fi
	if ! grep -q "wlr_swapchain_destroy" "$blur"; then
		echo "FAIL: the blur backdrop allocates a swapchain and never"
		echo "      destroys it; one per monitor leaks on unplug."
		fail=1
	fi
fi

if [ "$fail" -eq 0 ]; then
	echo "fx-buffer-ownership guard PASSED (retained images own their buffers)"
fi
exit $fail
