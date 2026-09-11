#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: every listener gowl_compositor_start() puts on a wlroots object
# is taken back off by remove_compositor_listeners(), and that runs before
# finalize destroys anything.
#
# wlroots asserts that a signal has no listeners left when the object
# owning it is destroyed, and an assert is an abort.  A listener added in
# start and forgotten in teardown took the process down in
# wlr_xwayland_destroy() -- under cmacs, from `gowl-stop', that is Emacs.
# tests/test-compositor-teardown.c finalizes a real compositor, but it
# only meets the objects its headless environment creates; this checks the
# list itself, so a manager that exists only on real hardware is covered.
#
# The capture provider is the one other owner of a listener on a global:
# impl_create_globals() adds it, and the provider's finalize -- run from
# the compositor's dispose, while the display still exists -- takes it
# off.  Its comment used to say wlroots did that for us; it does not.

set -e
# An inherited CDPATH makes `cd' print the directory it resolved, which
# would land in $root alongside pwd's output.
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
src="$root/src/core/gowl-compositor.c"
fail=0

# The body of a top-level function, comments removed so that prose
# naming a listener does not count.
body() {
	sed -n "/^$1(/,/^}/p" "$src" \
		| sed 's,/\*.*\*/,,' \
		| grep -vE '^[[:space:]]*(\*|/\*)' || true
}

# The listener argument of each LISTEN()/wl_signal_add() in start: the
# &self->field that is followed by a comma or a paren, never by ->.
added=$(body gowl_compositor_start | tr '\n' ' ' \
	| grep -oE '(LISTEN|wl_signal_add)\([^;]*' \
	| grep -oE '&self->[a-z_0-9]+[[:space:]]*[,)]' \
	| sed -E 's/&self->([a-z_0-9]+).*/\1/' | sort -u)
removed=$(body remove_compositor_listeners \
	| grep -oE 'listener_remove\(&self->[a-z_0-9]+\)' \
	| sed -E 's/.*->([a-z_0-9]+)\)/\1/' | sort -u)

count=$(printf '%s\n' "$added" | grep -c . || true)
if [ "$count" -lt 20 ]; then
	echo "FAIL: found only $count listeners added in gowl_compositor_start();"
	echo "      the extraction in this guard no longer matches the source"
	exit 1
fi

for l in $added; do
	if ! printf '%s\n' "$removed" | grep -qx "$l"; then
		echo "FAIL: &self->$l is added in gowl_compositor_start() but"
		echo "      remove_compositor_listeners() never takes it off"
		fail=1
	fi
done

# The capture provider's listeners, added in impl_create_globals(), come
# off in its finalize.
cap="$root/src/core/gowl-capture-wlroots.c"
cap_added=$(sed -n '/^impl_create_globals(/,/^}/p' "$cap" \
	| sed 's,/\*.*\*/,,' | grep -vE '^[[:space:]]*(\*|/\*)' | tr '\n' ' ' \
	| grep -oE 'wl_signal_add\([^;]*' \
	| grep -oE '&self->[a-z_0-9]+[[:space:]]*[,)]' \
	| sed -E 's/&self->([a-z_0-9]+).*/\1/' | sort -u)
cap_removed=$(sed -n '/^gowl_capture_wlroots_finalize(/,/^}/p' "$cap" \
	| grep -oE 'wl_list_remove\(&self->[a-z_0-9]+\.link\)' \
	| sed -E 's/.*->([a-z_0-9]+)\.link\)/\1/' | sort -u)
if [ -z "$cap_added" ]; then
	echo "FAIL: found no listener in impl_create_globals(); the extraction"
	echo "      in this guard no longer matches gowl-capture-wlroots.c"
	fail=1
fi
for l in $cap_added; do
	if ! printf '%s\n' "$cap_removed" | grep -qx "$l"; then
		echo "FAIL: &self->$l is added in impl_create_globals() but"
		echo "      gowl_capture_wlroots_finalize() never takes it off"
		fail=1
	fi
done

# And the removal comes first in finalize, before any destroy.
fin=$(body gowl_compositor_finalize)
first_remove=$(printf '%s\n' "$fin" | grep -n 'remove_compositor_listeners(' \
	| head -1 | cut -d: -f1)
first_destroy=$(printf '%s\n' "$fin" \
	| grep -nE 'wl_display_destroy_clients\(|wlr_xwayland_destroy|wlr_keyboard_group_destroy\(|wlr_backend_destroy\(|wl_display_destroy\(' \
	| head -1 | cut -d: -f1)
if [ -z "$first_remove" ] || [ -z "$first_destroy" ] \
	|| [ "$first_remove" -gt "$first_destroy" ]; then
	echo "FAIL: gowl_compositor_finalize() must call"
	echo "      remove_compositor_listeners() before it destroys anything"
	fail=1
fi

if [ "$fail" -eq 0 ]; then
	echo "PASS: all $count compositor listeners, and the capture"
	echo "      provider's, come off before teardown"
fi
exit "$fail"
