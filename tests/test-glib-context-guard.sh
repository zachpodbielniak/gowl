#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: nothing in the compositor puts work on GLib's default main
# context.
#
# That context is not the compositor's, in either way gowl runs.
# Standalone, gowl's main() runs wl_display_run() and nothing iterates
# GLib's default context, so a callback registered there never fires.
# Under `cmacs --gowl', the compositor runs on cmacs's dispatch thread,
# under cmacs's gowl lock, while the default context belongs to Emacs's
# pgtk loop on the editor's thread.  A callback there runs beside the
# compositor with no lock held, and that is a crash: the scene, the
# client list and the wl_event_loop itself are single-threaded.
#
# Two did this.  The output frame backoff (`monitor_frame_retry') used
# g_timeout_add(): standalone, an output that had backed off stopped
# asking for frames; under cmacs, the retry raced on_monitor_destroy().
# The clipboard module drained the selection pipe with g_unix_fd_add()
# and g_timeout_add(): standalone the history was never written; under
# cmacs, clip_store() rewrote the index on Emacs's thread while the IPC
# commands read it on the dispatch thread.
#
# Defer with the compositor's own loop instead: wl_event_loop_add_timer,
# wl_event_loop_add_idle, wl_event_loop_add_fd on
# gowl_compositor_get_event_loop().  Code that runs a GLib loop of its
# own (the tray, notifyd, logind, the D-Bus modules) attaches sources to
# THAT context with g_source_attach(source, ctx), which is allowed.
# g_timeout_add() and friends always use the global default context,
# whatever context the calling thread has pushed, so they are refused
# everywhere here.
#
# Not scanned: src/bar (gowlbar, a separate program with its own
# GMainLoop) and tools/ (separate programs).  tests/test-bar-guard.sh
# guard 7 is the older, plugin-only form of this check.

set -e
# An inherited CDPATH makes `cd' echo the resolved directory, which
# would land in $root alongside pwd's output and break every path here.
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
fail=0

# Every compositor-side C file: the library and the modules.
files=$(cd "$root" && find src modules -name '*.c' -not -path 'src/bar/*' | sort)

# Strip /* */ comments (single- and multi-line) and preprocessor lines,
# so prose explaining why a call is gone does not trip the guard.
strip() {
	sed -e ':a' -e 's,/\*[^*]*\*\+\([^/*][^*]*\*\+\)*/,,g' \
	    -e '/\/\*/{N;ba' -e '}' "$1" \
		| grep -vE '^[[:space:]]*#'
}

forbidden='\bg_(timeout_add|timeout_add_full|timeout_add_seconds|timeout_add_seconds_full|idle_add|idle_add_full|idle_add_once|timeout_add_once|unix_fd_add|unix_fd_add_full|io_add_watch|io_add_watch_full|child_watch_add|child_watch_add_full|unix_signal_add|unix_signal_add_full)[[:space:]]*\('
attach_null='\bg_source_attach[[:space:]]*\([^;]*,[[:space:]]*NULL[[:space:]]*\)'

checked=0
for f in $files; do
	checked=$((checked + 1))
	bad=$(strip "$root/$f" | grep -nE "$forbidden|$attach_null" || true)
	if [ -n "$bad" ]; then
		echo "$f:" >&2
		echo "$bad" >&2
		echo "FAIL: $f puts work on GLib's default main context;" \
		     "use the compositor's wl_event_loop" >&2
		fail=1
	fi
done

# Sanity: the walk found the tree.  Zero files would pass vacuously.
if [ "$checked" -lt 100 ]; then
	echo "FAIL: only $checked files scanned; the file list is wrong" >&2
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "ok: $checked compositor files keep off GLib's default context"
