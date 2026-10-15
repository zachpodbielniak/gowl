#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: input observers (the broadcast module) are invisible until one
# is loaded, see keys where they must, and deliver only through the
# compositor's targeted-delivery API.
#
# Structural facts no unit test can hold for the NEXT edit:
#
#   1. every GowlInputObserver hook in the compositor is behind
#      gowl_module_manager_has_input_observers(), so a session without
#      the module builds no event and calls nothing;
#   2. the filter hook runs after the lock screen and the confinement
#      escape (neither may be claimed) and BEFORE keyboard-shortcuts-
#      inhibit is consulted -- else the broadcast toggle dies exactly
#      when a fullscreen game has focus, which is when it is wanted;
#   3. the forwarded hook runs AFTER the key reached the focused window,
#      so a key a bind claimed is never repeated anywhere;
#   4. the module never touches the seat itself (wlr_seat_*_notify_*):
#      delivery is gowl_compositor_deliver_key/_button, which honour the
#      lock and the grabs and route X11 windows through X focus;
#   5. the module waits on nothing through GLib's default context
#      (g_timeout_add, g_idle_add, g_child_watch_add): under cmacs that
#      context is Emacs's thread, and a delivery from there races the
#      compositor.  Its delay queue is a wl_event_loop timer.

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
comp="$root/src/core/gowl-compositor.c"
mod="$root/modules/broadcast/gowl-module-broadcast.c"
engine="$root/modules/broadcast/gowl-broadcast-engine.c"
fail=0

# Body of a function, comments stripped, from its definition to the
# closing brace in column 1.
body() {
	awk -v fn="$2" '
		$0 ~ "^" fn "\\(" { inside = 1 }
		inside { print }
		inside && /^}/ { exit }
	' "$1" | sed 's,/\*.*\*/,,' | grep -vE '^[[:space:]]*[*#]'
}

require_fn() {
	if ! grep -qE "^$2\\(" "$1"; then
		echo "FAIL: no function named $2 in $(basename "$1")"
		echo "      A guard naming a function that does not exist"
		echo "      checks nothing."
		fail=1
		return 1
	fi
	return 0
}

line_of() {
	body "$1" "$2" | grep -nE "$3" | head -1 | cut -d: -f1 | grep . || echo 0
}

# 1. Every hook call is gated.
for hook in gowl_module_manager_filter_key gowl_module_manager_key_forwarded \
            gowl_module_manager_button_forwarded; do
	calls=$(grep -c "$hook(" "$comp" || true)
	if [ "$calls" -eq 0 ]; then
		echo "FAIL: $hook is never called; the observer hook is gone"
		fail=1
		continue
	fi
	# the gate must appear in the 40 lines above each call
	ungated=$(awk -v hook="$hook(" '
		{ line[NR] = $0 }
		index($0, hook) {
			ok = 0
			for (i = NR - 40; i < NR; i++)
				if (i > 0 && index(line[i], "gowl_module_manager_has_input_observers"))
					ok = 1
			if (!ok) print NR
		}
	' "$comp")
	if [ -n "$ungated" ]; then
		echo "FAIL: $hook called without has_input_observers() first"
		echo "      (gowl-compositor.c lines: $ungated).  Without an"
		echo "      observer the compositor must do no work for it."
		fail=1
	fi
done

# 2 and 3. Where the key hooks sit in compositor_handle_key.
if require_fn "$comp" compositor_handle_key; then
	locked=$(line_of "$comp" compositor_handle_key 'if \(self->locked\)')
	escape=$(line_of "$comp" compositor_handle_key 'pointer_confine_escape_pressed')
	filter=$(line_of "$comp" compositor_handle_key 'gowl_module_manager_filter_key')
	inhibit=$(line_of "$comp" compositor_handle_key 'gowl_shortcuts_inhibited')
	notify=$(line_of "$comp" compositor_handle_key 'wlr_seat_keyboard_notify_key\(self->wlr_seat,[[:space:]]*$')
	fwd=$(line_of "$comp" compositor_handle_key 'gowl_module_manager_key_forwarded')
	if [ "$filter" -le "$locked" ] || [ "$filter" -le "$escape" ]; then
		echo "FAIL: the observer filter runs before the lock screen or the"
		echo "      confinement escape; neither may be claimed by a module"
		fail=1
	fi
	if [ "$filter" -ge "$inhibit" ]; then
		echo "FAIL: the observer filter runs after shortcuts-inhibit is"
		echo "      consulted; the broadcast toggle would stop working"
		echo "      whenever a fullscreen game has focus"
		fail=1
	fi
	if [ "$notify" -eq 0 ] || [ "$fwd" -le "$notify" ]; then
		echo "FAIL: key_forwarded is not after the forward to the focused"
		echo "      window; a key a bind claimed could be repeated"
		fail=1
	fi
fi

# 4. The module reaches windows through the delivery API only.
if grep -nE 'wlr_seat_[a-z_]*notify_|wlr_seat_keyboard_enter|wlr_seat_pointer_enter' \
	"$mod" "$engine"; then
	echo "FAIL: the broadcast module talks to the seat itself; deliver"
	echo "      through gowl_compositor_deliver_key/_button, which honour"
	echo "      the lock and the grabs and know about X11"
	fail=1
fi

# 5. Nothing through GLib's default main context.
if grep -nE 'g_timeout_add|g_idle_add|g_child_watch_add' "$mod" "$engine"; then
	echo "FAIL: the broadcast module uses a GLib-default-context source;"
	echo "      under cmacs it would run on Emacs's thread.  Use a"
	echo "      wl_event_loop timer."
	fail=1
fi

if [ "$fail" -eq 0 ]; then
	echo "PASS: input observers are inert without a module, placed where"
	echo "      they must be, and broadcast delivers only through the core"
fi
exit "$fail"
