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

# 5. A shipped keybind must name a command something answers.
#
# `ipc_command' dispatches by NAME, and a name nothing handles is not
# an error: run_command finds no taker, the key is consumed because it
# matched a bind, and nothing happens.  So a renamed IPC command leaves
# Super+Shift+S silently inert -- the worst possible failure for a
# keybind, because it looks exactly like the key not being pressed.
for arg in $(grep -oE 'action: ipc_command, arg: "screenshot[a-z-]*"' \
		data/default-config.yaml | grep -oE '"screenshot[a-z-]*"' \
		| tr -d '"' | sort -u); do
	grep -q "\"$arg\"" "$shot" ||
		fail "default-config.yaml binds ipc_command $arg, which $shot does not handle"
done

# And the reverse for the one the user actually presses: the shipped
# config must still carry it.  Dropping the bind is a silent loss of a
# documented default.
grep -q 'Super+Shift+s.*ipc_command.*screenshot-area' data/default-config.yaml ||
	fail "default-config.yaml no longer binds Super+Shift+s to screenshot-area"

# 6. The mouse-handler interface must stay connected to the cursor.
#
# This is the bug that made the whole feature look broken.  Modules
# implement GowlMouseHandler; the manager has dispatch functions for
# it; and NOTHING in the compositor called them.  handle_motion had no
# callers anywhere in the tree, and dispatch_button had none either --
# so every module implementing the interface was writing functions that
# could never run.
#
# The screenshot selection armed correctly, drew nothing (there is
# nothing to draw before the first press), and then waited forever for
# a press that could not arrive.  From the outside that is
# indistinguishable from a keybind that does nothing.
#
# Checked against the source with comments stripped, and for a CALL
# rather than a mention: the hunk that fixes this carries a comment
# naming both functions, so a plain grep would be satisfied by the
# prose that explains the call after the call itself was deleted.
comp_code=$(sed -e 's://.*::' "$root/src/core/gowl-compositor.c" \
	| awk 'BEGIN{c=0} {line=$0
		while (1) {
			if (c) { i=index(line,"*/"); if (!i) { line=""; break }
				line=substr(line,i+2); c=0; continue }
			i=index(line,"/*"); if (!i) break
			j=index(substr(line,i+2),"*/")
			if (!j) { line=substr(line,1,i-1); c=1; break }
			line=substr(line,1,i-1) substr(line,i+2+j+1) }
		print line}')

echo "$comp_code" | grep -q "gowl_module_manager_dispatch_motion(" ||
	fail "the compositor never CALLS gowl_module_manager_dispatch_motion; every GowlMouseHandler::handle_motion is dead code again"
echo "$comp_code" | grep -q "gowl_module_manager_dispatch_button(" ||
	fail "the compositor never CALLS gowl_module_manager_dispatch_button; an interactive module can arm but never finish"

# 7. An armed interactive mode must be visible before the first click.
#
# The overlay used to be drawn only once an anchor was set, so an armed
# selection looked exactly like nothing having happened -- which is
# precisely how the dead dispatch above went unnoticed.  The dim wash
# is the feedback that says the mode is on.
overlay_body=$(awk '
	/^create_overlay\(/ { inside = 1 }
	inside { print }
	inside && /^}/ { exit }' "$shot")
if [ -z "$overlay_body" ]; then
	fail "$shot has no create_overlay; this guard would check nothing"
else
	echo "$overlay_body" | grep -q "sel_dim" ||
		fail "create_overlay draws nothing until the first press; an armed selection is invisible"
fi

# 8. Nothing the module drew may end up in the picture.
#
# Every capture path destroys the overlay first.  Forgetting it in one
# path produces a screenshot with a blue rubber band across it, which
# is obvious once seen and easy to not see in review.
for fn in finish_area_selection finish_window_pick; do
	fn_body=$(awk -v f="$fn" '
		$0 ~ "^" f "\\(" { inside = 1 }
		inside { print }
		inside && /^}/ { exit }' "$shot")
	if [ -z "$fn_body" ]; then
		fail "$shot has no $fn; this guard would check nothing"
		continue
	fi
	echo "$fn_body" | grep -q "destroy_overlay" ||
		fail "$fn captures without destroying the overlay; the selection UI lands in the image"
done

# 9. The bar must close its panel before capturing.
#
# A capture renders the scene as it stands, so a screen capture started
# from an open dropdown photographs the dropdown.
if grep -q "shot_action" "$desk"; then
	action_body=$(awk '
		/^shot_action\(/ { inside = 1 }
		inside { print }
		inside && /^}/ { exit }' "$desk")
	echo "$action_body" | grep -q "close_panel" ||
		fail "$desk captures without closing the panel; the dropdown ends up in the screenshot"
fi

# 10. Annotation must always land somewhere.
#
# satty and swappy are optional and cmacs may be built without imgedit,
# so every one of those can be absent.  The chain must still end in
# something that opens the capture -- an "Annotate" button that does
# nothing on a machine missing two optional tools is worse than no
# button, because the capture is already saved and the user is left
# wondering whether it worked.
if grep -q "shot_annotate" "$desk"; then
	awk '/^shot_annotate\(GowlBarPlugin/,/^}/' "$desk" \
		| grep -q "cmacs-screenshot-annotate" ||
		fail "$desk has no final fallback for annotation; with satty and swappy absent the button does nothing"
fi

[ "$fail" -eq 0 ] || exit 1
echo "PASS: screenshot source guards"
