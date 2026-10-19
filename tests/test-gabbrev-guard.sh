#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: the gabbrev module's structural rules -- facts a unit test
# cannot hold for the NEXT edit:
#
#   1. nothing waits through GLib's default main context (g_timeout_add,
#      g_idle_add, g_child_watch_add, GSubprocess/GTask async): under
#      cmacs that context is Emacs's thread, and typing from there races
#      the compositor.  Timers are wl_event_loop timers, subprocesses are
#      gowl_subprocess_spawn(), cross-thread answers are posted through
#      the module's eventfd;
#   2. the module never touches the seat's keyboard itself
#      (wlr_seat_*_notify_*): typing is gowl_compositor_send_key/
#      text_to_client, which honour the lock and the keyboard grabs and
#      reach X11 windows through X focus;
#   3. the key handler starts a job with `in_key' set, so the typing
#      happens a loop turn later -- never in the middle of delivering the
#      key that completed the trigger;
#   4. yaml-glib is never handed a NULL GError**: it dereferences one on
#      malformed input (a crash on a user's typo);
#   5. job_start() hands back a number, not the job: a job whose render
#      needs nothing finishes, and is freed, inside it.

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
dir="$root/modules/gabbrev"
mod="$dir/gowl-module-gabbrev.c"
fail=0

# Source with comments removed (good enough for these patterns)
code() {
	sed 's,/\*.*\*/,,' "$@" | grep -vE '^[[:space:]]*\*'
}

# 1
if code "$dir"/*.c | grep -nE 'g_timeout_add|g_idle_add|g_child_watch_add|g_subprocess_[a-z_]*_async|g_task_new|g_spawn_async' ; then
	echo "FAIL: gabbrev waits through GLib's default context (rule 1)"
	fail=1
fi

# 2
if code "$dir"/*.c | grep -nE 'wlr_seat_(keyboard|pointer)_notify' ; then
	echo "FAIL: gabbrev drives the seat directly (rule 2)"
	fail=1
fi

# 3
body=$(awk '/^gab_key_forwarded\(/ { inside = 1 } inside { print } inside && /^}/ { exit }' "$mod")
if [ -z "$body" ]; then
	echo "FAIL: no gab_key_forwarded() in $(basename "$mod")"
	fail=1
elif ! printf '%s\n' "$body" | grep -q 'self->in_key = TRUE;' \
     || ! printf '%s\n' "$body" | grep -q 'self->in_key = FALSE;'; then
	echo "FAIL: gab_key_forwarded() starts a job without in_key (rule 3)"
	fail=1
fi
if ! awk '/^kick_steps\(/ { inside = 1 } inside { print } inside && /^}/ { exit }' "$mod" \
     | grep -q 'self->in_key'; then
	echo "FAIL: kick_steps() ignores in_key (rule 3)"
	fail=1
fi

# 4
if code "$dir"/*.c "$root/src/gabbrev/gowl-gabbrev.c" \
     | grep -nE 'yaml_parser_load_from_(data|file)\([^;]*, *NULL\)' ; then
	echo "FAIL: yaml-glib handed a NULL GError** (rule 4)"
	fail=1
fi

# 5
if ! grep -qE '^static guint$' "$mod" || ! awk '/^job_start\(/ { print prev } { prev = $0 }' "$mod" | grep -q 'guint'; then
	echo "FAIL: job_start() must return the job's id, not the job (rule 5)"
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "gabbrev guard: ok"
