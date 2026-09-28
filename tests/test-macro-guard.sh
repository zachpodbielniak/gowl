#!/bin/sh
#
# gowl - source guard for the macro system's containment invariants
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Properties that no test run can prove, because breaking them only
# shows on the day a macro misbehaves on someone's desktop -- as a
# frozen session instead of a notification.

set -e

cd -- "$(dirname "$0")/.." >/dev/null

fail() {
	echo "FAIL: $1" >&2
	exit 1
}

RUNNER=modules/macro/gowl-macro-runner.c
MODULE=modules/macro/gowl-module-macro.c
LOADER=modules/macro/gowl-macro-loader.c
DBUS=modules/macro/gowl-macro-dbus.c
API=src/macro/gowl-macro.c
INPUT=src/core/gowl-client-input.c

# 1. Macro code only ever runs under the fault guard.
#
# The body is called from exactly one place, body_trampoline, and that
# is only ever handed to gowl_fault_guard_call -- on the compositor
# thread and on workers alike.  A second call site is an unguarded
# macro: one NULL dereference and the session is gone.
calls=$(grep -c 'b->body(' "$RUNNER" || true)
[ "$calls" -eq 1 ] ||
	fail "a macro body is called outside body_trampoline in $RUNNER"
guarded=$(grep -c 'gowl_fault_guard_call(body_trampoline' "$RUNNER" || true)
[ "$guarded" -ge 2 ] ||
	fail "the timeline and the worker must both guard body_trampoline"
if grep -n 'body_trampoline(' "$RUNNER" | grep -v 'gowl_fault_guard_call\|^[0-9]*:body_trampoline(gpointer' >/dev/null; then
	fail "body_trampoline is called directly somewhere in $RUNNER"
fi
# Opening a macro runs its constructors: guarded too, with a budget.
grep -q 'gowl_fault_guard_call(open_body' "$LOADER" ||
	fail "the loader opens macro files without the fault guard"
grep -q 'gowl_fault_guard_call(info_body' "$LOADER" ||
	fail "the loader calls gowl_macro_info() without the fault guard"
if grep -n 'info()' "$LOADER" | grep -v 'c->info()' | grep -v '"' \
	| grep -v '/\*' >/dev/null; then
	fail "gowl_macro_info() is called outside info_body in $LOADER"
fi
# A registered C macro runs through the same runner.
grep -q 'gowl_macro_runner_start(self->runner, ctx, registered_body' "$MODULE" ||
	fail "registered C macros no longer go through the guarded runner"

# 2. The compositor thread never sleeps for a macro.
#
# Sleeping belongs to threaded macros, on their worker.  A g_usleep in
# the module or the API is the desktop standing still.
if grep -n 'g_usleep\|[^_]sleep (\|[^_]usleep(' "$RUNNER" "$MODULE" "$LOADER" "$DBUS" "$API" \
	| grep -v '^\s*\*' | grep -v '/\*' >/dev/null; then
	fail "something in the macro system sleeps; the timeline uses timers"
fi
grep -q 'if (!ctx->threaded)' "$API" ||
	fail "gowl_macro_sleep no longer refuses to run outside a worker"

# 3. A lock another thread takes is held only with the watchdog held off.
#
# A watchdog unwind inside a locked region leaves the lock taken for
# good, and the other side of every one of these is the compositor
# thread.  Every g_mutex_lock in the API must directly follow a
# gowl_fault_guard_hold.
awk '
	/g_mutex_lock\(/ {
		if (prev !~ /gowl_fault_guard_hold\(\)/) {
			printf "%s:%d: g_mutex_lock without a hold\n", FILENAME, FNR
			bad = 1
		}
	}
	{ if ($0 !~ /^[ \t]*$/) prev = $0 }
	END { exit bad }
' "$API" || fail "a lock in $API is taken without gowl_fault_guard_hold"
for fn in 'post(' 'host_invoke_sync('; do
	grep -A40 "^$fn" "$RUNNER" | grep -q 'gowl_fault_guard_hold' ||
		fail "$fn in $RUNNER takes a lock without gowl_fault_guard_hold"
done

# 4. Keys are never sent to a window while the screen is locked or
#    something holds the keyboard.
grep -q 'self->locked || gowl_compositor_keyboard_is_grabbed(self)' "$INPUT" ||
	fail "targeted key sends no longer refuse when locked or grabbed"
# ... and the only way in is target_begin, which checks.
for fn in gowl_compositor_send_key_to_client gowl_compositor_send_text_to_client; do
	sed -n "/^$fn(/,/^}/p" "$INPUT" | grep -q 'target_begin(' ||
		fail "$fn does not go through target_begin"
done
# The remapper's macro target is skipped while locked, like callbacks.
sed -n '/case GOWL_INPUT_REMAP_TARGET_MACRO:/,/return;/p' \
	src/core/gowl-input-remap-core.c | grep -q 'if (locked)' ||
	fail "the remapper runs macro targets while the screen is locked"

# 5. Opened macro code is never unmapped, and a script is never freed
#    under a worker.
grep -q 'g_module_make_resident' "$LOADER" ||
	fail "the loader no longer keeps macro modules resident"
if grep -q 'g_module_close' "$LOADER"; then
	fail "the loader closes macro modules; a queued step may point into one"
fi
grep -q 'g_ptr_array_new_with_free_func(script_free)' "$LOADER" ||
	fail "scripts are freed before the loader goes; see the retired list"

# 6. The D-Bus handler owns its invocation (transfer full); taking
#    another reference leaked one per call, each pinning the connection.
if grep -q 'g_object_ref(invocation)' "$DBUS"; then
	fail "$DBUS takes an extra reference on a method invocation"
fi

# 7. The watchdog signal is not JSC's.
#
# cmacs moves WebKit's GC signal to 40 (JSC_SIGNAL_FOR_GC); a macro
# watchdog on the same number would stop a macro every time WebKit
# collected garbage.
grep -q 'define WATCHDOG_RT_OFFSET (9)' src/util/gowl-fault-guard.c ||
	fail "the watchdog signal moved; check it against JSC_SIGNAL_FOR_GC=40"

# 8. The shipped examples only sleep when they are threaded.
for f in data/macros/*.c; do
	if grep -q 'gowl_macro_sleep' "$f" &&
	   ! grep -q '#define GOWL_MACRO_THREADED 1' "$f"; then
		fail "$f sleeps but is not threaded"
	fi
done

echo "macro guard: ok"
exit 0
