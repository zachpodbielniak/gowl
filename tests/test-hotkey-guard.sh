#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: the hotkey module's structural rules -- facts a unit test
# cannot hold for the NEXT edit:
#
#   1. nothing waits through GLib's default main context (g_timeout_add,
#      g_idle_add, g_child_watch_add, GSubprocess/GTask async): under
#      cmacs that context is Emacs's thread.  Timers are wl_event_loop
#      timers, a waited-for child is gowl_subprocess_spawn(), a C build
#      comes back through the module's eventfd.  A fire-and-forget spawn
#      (g_spawn_async without G_SPAWN_DO_NOT_REAP_CHILD) waits on nothing
#      -- GLib reaps it through an intermediate child -- and is allowed;
#   2. the module never drives the seat itself (wlr_seat_*_notify_*):
#      keys go through gowl_compositor_inject_key(), the compositor's own
#      key path, so binds, the recorder rules and the lock all apply;
#   3. code from a crispy build -- an action, a script's init or fini --
#      is only ever called inside the fault-guarded bodies, and a build is
#      only opened inside one: one NULL dereference must cost a call, not
#      the session;
#   4. yaml-glib is never handed a NULL GError**: it dereferences one on
#      malformed input (a crash on a user's typo);
#   5. switching the module off unregisters its JSON-RPC methods, which
#      otherwise point at a module that is gone;
#   6. a shell action's line goes to /bin/sh as one argument, as written:
#      nothing typed in a window is ever substituted into it (the values
#      reach the shell as GOWL_HOTKEY_* environment variables instead);
#   7. a hotkey's actions never run inside the key event that fired it:
#      the engine's FIRE is queued (hk_queue_fire) and run a loop turn
#      later;
#   8. the remap modifier overlay's listener on the keyboard group is
#      taken off on every way out -- deactivate, shutdown, the display
#      going, the compositor being finalized: wlroots aborts when an
#      object is destroyed with a listener still on it;
#   9. a custom form is filled with Lisp string literals (only " and \
#      escaped), never g_strescape(), whose octal escapes the Lisp reader
#      turns into raw bytes.

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
dir="$root/modules/hotkey"
mod="$dir/gowl-module-hotkey.c"
runner="$dir/gowl-hotkey-runner.c"
script="$dir/gowl-hotkey-script.c"
fail=0

# Source with comments removed (good enough for these patterns)
code() {
	sed 's,/\*.*\*/,,' "$@" | grep -vE '^[[:space:]]*\*'
}

# The body of function $2 in file $1
body() {
	awk -v f="$2" '$0 ~ "^"f"\\(" { inside = 1 } inside { print } inside && /^}/ { exit }' "$1"
}

if [ ! -f "$mod" ] || [ ! -f "$runner" ] || [ ! -f "$script" ]; then
	echo "FAIL: the hotkey module's sources are not where this guard looks"
	exit 1
fi

# 1
if code "$dir"/*.c | grep -nE 'g_timeout_add|g_idle_add|g_child_watch_add|g_subprocess_[a-z_]*_async|g_task_new|G_SPAWN_DO_NOT_REAP_CHILD' ; then
	echo "FAIL: the hotkey module waits through GLib's default context (rule 1)"
	fail=1
fi

# 2
if code "$dir"/*.c | grep -nE 'wlr_seat_(keyboard|pointer)_notify' ; then
	echo "FAIL: the hotkey module drives the seat directly (rule 2)"
	fail=1
fi

# 3: calls through a build's function pointers, and g_module_open, only
# in call_body() and open_body()
outside=$(code "$script" | awk '
	/^(call_body|open_body)\(/ { inside = 1 }
	!inside && /(->action|->init|->fini)\(|g_module_open\(/ { print NR": "$0 }
	inside && /^}/ { inside = 0 }')
if [ -n "$outside" ]; then
	printf '%s\n' "$outside"
	echo "FAIL: crispy code called outside the fault guard (rule 3)"
	fail=1
fi
if code "$mod" "$runner" | grep -nE 'g_module_open\(|HkCActionFunc\)\(|[^_]init\(\)' ; then
	echo "FAIL: the module calls a build itself instead of through hk_c_call_* (rule 3)"
	fail=1
fi
if ! body "$script" guarded | grep -q 'gowl_fault_guard_call'; then
	echo "FAIL: guarded() does not use the fault guard (rule 3)"
	fail=1
fi

# 4
if code "$dir"/*.c "$root/src/hotkey/gowl-hotkey.c" \
     | grep -nE 'yaml_parser_load_from_(data|file)\([^;]*, *NULL\)' ; then
	echo "FAIL: yaml-glib handed a NULL GError** (rule 4)"
	fail=1
fi

# 5
if ! body "$mod" hk_deactivate | grep -q 'methods_uninstall(self)'; then
	echo "FAIL: hk_deactivate() leaves the JSON-RPC methods registered (rule 5)"
	fail=1
fi
if ! body "$mod" methods_uninstall | grep -q 'gowl_ipc_unregister_method'; then
	echo "FAIL: methods_uninstall() unregisters nothing (rule 5)"
	fail=1
fi

# 6
sp=$(body "$runner" spawn_action)
if [ -z "$sp" ]; then
	echo "FAIL: no spawn_action() in $(basename "$runner")"
	fail=1
elif printf '%s\n' "$sp" | grep -nE 'substitute\(|g_strdup_printf\([^)]*a->text'; then
	echo "FAIL: a shell line is built from substituted text (rule 6)"
	fail=1
elif ! printf '%s\n' "$sp" | grep -q 'g_strdup(a->text)'; then
	echo "FAIL: spawn_action() no longer passes the line as written (rule 6)"
	fail=1
fi

# 7
po=$(body "$runner" hk_play_outputs)
if ! printf '%s\n' "$po" | grep -q 'hk_queue_fire(self, o->hotkey)'; then
	echo "FAIL: hk_play_outputs() does not queue a fired hotkey (rule 7)"
	fail=1
fi
if printf '%s\n' "$po" | grep -q 'hk_run_actions'; then
	echo "FAIL: hk_play_outputs() runs actions inside the key event (rule 7)"
	fail=1
fi

# 8
for f in hk_deactivate hk_on_shutdown hk_display_destroyed; do
	if ! body "$mod" "$f" | grep -q 'hk_overlay_detach(self)'; then
		echo "FAIL: $f() leaves the overlay listener on the group (rule 8)"
		fail=1
	fi
done
if ! body "$mod" on_compositor_gone | grep -q 'wl_list_remove(&self->group_mods.link)'; then
	echo "FAIL: on_compositor_gone() leaves the overlay listener (rule 8)"
	fail=1
fi
if ! code "$mod" | grep -q 'g_object_weak_ref(G_OBJECT(compositor), on_compositor_gone'; then
	echo "FAIL: on_compositor_gone() is not connected (rule 8)"
	fail=1
fi

# 9
if code "$dir"/*.c | grep -n 'g_strescape' ; then
	echo "FAIL: g_strescape() in the hotkey module (rule 9)"
	fail=1
fi

[ "$fail" = 0 ] && echo "PASS: the hotkey module keeps its structural rules"
exit $fail
