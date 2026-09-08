#!/bin/sh
# gowl - source guard for the screenshot path
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Four properties of the screenshot path that no unit test reaches, and
# that all fail quietly -- the button keeps working, it just stops doing
# one of the things it claims to do, or it takes the session with it.

set -e
# An inherited CDPATH makes `cd' echo the resolved directory.
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
fail=0

fail() {
	echo "FAIL: $1" >&2
	fail=1
}

# 1. The compositor's own screenshot module must stay reachable.
#
# gowl_recording_provider_start() had no callers anywhere in the tree
# for its whole life: the module implemented every capture mode and
# nothing could invoke it.  The screenshot module was in exactly the
# same state -- an interface, an implementation, and no way in.  An
# interface with no callers rots quietly, so this is the check that
# says somebody is still using it.
grep -rq "gowl_screenshot_provider_capture" modules/bar/ ||
	fail "nothing in the bar calls gowl_screenshot_provider_capture; the screenshot module is unreachable again"

grep -rq "gowl_module_manager_get_screenshot_provider" modules/ ||
	fail "no module looks the screenshot provider up; the getter has no callers"

# 2. `copy-to-clipboard' must be read, not merely parsed.
#
# The module parsed this setting, defaulted it to TRUE, and then never
# looked at it again -- so every screenshot went to disk and nowhere
# else, and the configuration said otherwise.  Reading a setting into a
# struct field is not using it.
shot="modules/screenshot/gowl-module-screenshot.c"
if grep -q "copy_to_clipboard" "$shot"; then
	# Uses that are NOT the declaration and NOT the two assignments
	# that fill it in from config and defaults.
	uses=$(grep -c "self->copy_to_clipboard" "$shot" || true)
	writes=$(grep -c "self->copy_to_clipboard[[:space:]]*=" "$shot" || true)
	if [ "$uses" -le "$writes" ]; then
		fail "$shot stores copy_to_clipboard but never reads it; the setting does nothing"
	fi
fi

# 3. The clipboard must not be written from the compositor thread.
#
# wlr_data_source::send hands us a pipe fd and runs on the compositor
# thread.  The text source writes its whole payload there, which is
# fine for a line of text and a desktop freeze for a screenshot: a
# client that reads a megabyte slowly holds the compositor for as long
# as it likes, and one that never reads at all holds it forever.
#
# So the bytes source must hand the fd to the main loop instead.  The
# symptom of getting this wrong is not a crash -- it is a session that
# hangs when something pastes.
seat="src/core/gowl-seat.c"
send_body=$(awk '
	/^gowl_bytes_source_send\(/ { inside = 1 }
	inside { print }
	inside && /^}/ { exit }' "$seat")
if [ -z "$send_body" ]; then
	fail "$seat has no gowl_bytes_source_send; a guard naming a function that does not exist checks nothing"
else
	echo "$send_body" | grep -q "g_unix_fd_add" ||
		fail "gowl_bytes_source_send does not hand the fd to the main loop; a slow reader now blocks the compositor"
	echo "$send_body" | grep -q "g_unix_set_fd_nonblocking" ||
		fail "gowl_bytes_source_send does not set the fd non-blocking; the first write of a large payload blocks"
fi

# 4. An async capture must not hold the plugin by raw pointer.
#
# An area selection completes whenever the user finishes dragging.  Bar
# plugins hot-reload, so that may be long after the plugin the callback
# belongs to was destroyed.  A raw pointer is a use-after-free that
# only shows up if somebody reloads mid-drag, which nobody does on
# purpose and everybody does eventually.
desk="modules/bar/bar-plugins-desktop.c"
if grep -q "gowl_screenshot_provider_capture" "$desk"; then
	grep -q "g_weak_ref_init" "$desk" ||
		fail "$desk starts an async capture without taking a weak ref on the plugin"
	grep -q "g_weak_ref_get" "$desk" ||
		fail "$desk never resolves the weak ref; the completion path is using something it did not check is alive"
fi

[ "$fail" -eq 0 ] || exit 1
echo "PASS: screenshot source guards"
