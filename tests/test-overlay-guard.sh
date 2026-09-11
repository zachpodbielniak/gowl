#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: two call sites the scratchpad depends on.  Both are on paths that
# need a live wlroots surface, so no unit test can reach them.
#
#   * on_client_unmap() must hand an adopted overlay back.  A toplevel can
#     unmap and map again without being destroyed, and the map path keeps
#     whatever overlay state the client had: an adopted window coming back
#     would be invisible, on no tag, with nothing left to show it.
#
#   * The focus_stack action must step through
#     gowl_compositor_stack_neighbour(), which keeps a shown scratchpad's
#     windows cycling among themselves.  Stepping by visibility alone lands
#     on the tiles beneath it, and focus leaving the panel rolls it away:
#     Super+j would close the scratchpad.
#
# tests/test-overlay-adopt.c covers what those functions do; this covers
# that they are called.

set -e
# An inherited CDPATH makes `cd' print the directory it resolved, which
# would land in $root alongside pwd's output.
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
src="$root/src/core/gowl-compositor.c"
fail=0

# The code from one pattern to the next, with comments removed so prose
# that names a function does not count as a call to it.
code_between() {
	sed -n "/$1/,/$2/p" "$src" \
		| sed 's,/\*.*\*/,,' \
		| grep -vE '^[[:space:]]*(\*|/\*)' || true
}

if ! code_between '^on_client_unmap(' '^}' \
	| grep -qE '\bgowl_compositor_release_overlay[[:space:]]*\('
then
	echo "FAIL: on_client_unmap() no longer releases adopted overlays;"
	echo "      an adopted window that unmaps and maps again would come"
	echo "      back hidden, on no tag, with nothing to show it"
	fail=1
fi

if ! code_between 'case GOWL_ACTION_FOCUS_STACK:' 'case GOWL_ACTION_SET_MFACT:' \
	| grep -qE '\bgowl_compositor_stack_neighbour[[:space:]]*\('
then
	echo "FAIL: the focus_stack action no longer steps through"
	echo "      gowl_compositor_stack_neighbour(); Super+j on a shown"
	echo "      scratchpad would land on the tiles beneath and close it"
	fail=1
fi

if [ "$fail" -eq 0 ]; then
	echo "PASS: the unmap and focus-stack overlay call sites are in place"
fi
exit "$fail"
