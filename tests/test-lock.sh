#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# End-to-end: the lock is a separate program, and the session survives
# it dying.
#
# That last part is the whole reason the lock was pulled out of the
# compositor.  A lock built in is only as safe as the process it runs
# in; a lock client holding the session through ext-session-lock-v1
# leaves the screen sealed when it crashes, and the compositor puts a
# new one up.  So this kills gowl-lock with SIGKILL -- the case a
# graceful shutdown would not cover -- and checks the session is STILL
# locked afterwards.
#
# Also checked, because each was broken:
#
#  - `gowl-msg lock' starts the configured lock program, so locking
#    works from a keybind on any tag rather than only from an editor
#    command that needs the editor focused;
#  - the compositor reports the state, so a bar or a script can ask;
#  - the administrative `unlock' opens it again, which is the only way
#    out of a headless test (there is no password to type).
#
# Skips rather than fails where the pieces are not built: this drives
# three programs, and a machine missing one has not broken anything.

set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
gowl="$root/build/release/gowl"
lock="$root/build/release/gowl-lock"
msg="$root/build/release/gowl-msg"

if [ ! -x "$gowl" ] || [ ! -x "$msg" ]; then
	echo "SKIP: the compositor or gowl-msg is not built"
	exit 0
fi
if [ ! -x "$lock" ]; then
	echo "SKIP: gowl-lock is not built (pam-devel?)"
	exit 0
fi

runtime=$(mktemp -d -p "${XDG_RUNTIME_DIR:-/tmp}" gowl-lock-test-XXXXXX)
comp_pid=""
lock_pid=""
cleanup () {
	[ -n "$lock_pid" ] && kill -KILL "$lock_pid" 2>/dev/null || true
	[ -n "$comp_pid" ] && kill -TERM "$comp_pid" 2>/dev/null || true
	rm -rf "$runtime"
}
trap cleanup EXIT INT TERM

GOWL_DISABLE_SYSTEMD=1 XDG_RUNTIME_DIR="$runtime" \
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_RENDERER=pixman \
LD_LIBRARY_PATH="$root/build/release" \
	"$gowl" --no-c-config --config /dev/null >"$runtime/compositor.log" 2>&1 &
comp_pid=$!

# Wait for the socket rather than sleeping a guess.
i=0
while [ $i -lt 100 ]; do
	display=$(ls "$runtime" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1)
	[ -n "$display" ] && break
	i=$((i + 1))
	sleep 0.1
done
if [ -z "$display" ]; then
	echo "FAIL: the compositor never opened a socket"
	cat "$runtime/compositor.log"
	exit 1
fi

export XDG_RUNTIME_DIR="$runtime"
export WAYLAND_DISPLAY="$display"

ask () {
	"$msg" "$@" 2>/dev/null
}

# The IPC socket is opened a moment after the Wayland one.
i=0
while [ $i -lt 100 ]; do
	[ "$(ask ping)" = "pong" ] && break
	i=$((i + 1))
	sleep 0.1
done
if [ "$(ask ping)" != "pong" ]; then
	echo "SKIP: no IPC socket (built without it?)"
	exit 0
fi

state=$(ask locked)
if [ "$state" != "unlocked" ]; then
	echo "FAIL: a fresh session says it is '$state'"
	exit 1
fi

# --ready-fd is how a caller knows the screen is really covered rather
# than merely that a process was started; the suspend path depends on
# exactly this.
"$lock" --pam-service login --ready-fd 1 >"$runtime/ready" 2>"$runtime/lock.log" &
lock_pid=$!

i=0
while [ $i -lt 100 ]; do
	[ "$(ask locked)" = "locked" ] && break
	i=$((i + 1))
	sleep 0.1
done
if [ "$(ask locked)" != "locked" ]; then
	echo "FAIL: gowl-lock did not lock the session"
	cat "$runtime/lock.log"
	exit 1
fi
if [ ! -s "$runtime/ready" ]; then
	echo "FAIL: gowl-lock never wrote to its --ready-fd"
	exit 1
fi

# The point of the whole arrangement: kill it outright and the session
# stays sealed.  SIGKILL, so nothing in gowl-lock gets a chance to be
# polite about it.
kill -KILL "$lock_pid" 2>/dev/null || true
wait "$lock_pid" 2>/dev/null || true
lock_pid=""

# Give the compositor a moment to notice the client is gone -- and, with
# lock-command unset in this config, to NOT put another one up.
sleep 0.5
if [ "$(ask locked)" != "locked" ]; then
	echo "FAIL: killing the lock program unlocked the session"
	exit 1
fi

# The administrative override is the only way out without a password.
ask unlock >/dev/null
i=0
while [ $i -lt 50 ]; do
	[ "$(ask locked)" = "unlocked" ] && break
	i=$((i + 1))
	sleep 0.1
done
if [ "$(ask locked)" != "unlocked" ]; then
	echo "FAIL: gowl-msg unlock did not open the session"
	exit 1
fi

echo "lock PASSED (locks, survives its client being killed, unlocks)"
exit 0
