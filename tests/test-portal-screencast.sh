#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# End-to-end: the ScreenCast portal really casts.
#
# Starts a headless compositor and the portal backend on a private
# session bus, then drives the freedesktop handshake the way a video
# call does -- CreateSession, SelectSources, Start -- and checks that a
# PipeWire node id comes back.  A node id is the whole contract: it is
# what the application connects to, and producing one means the capture
# session opened, the compositor described the source, and the PipeWire
# stream negotiated a format.
#
# The private bus matters.  The backend takes
# org.freedesktop.impl.portal.desktop.gowl with REPLACE, so running this
# against the developer's own session bus would displace the portal
# their desktop is using.  dbus-run-session gives it a bus of its own.
#
# Skips rather than fails when PipeWire is not running or the tools are
# missing: this is an integration test of three daemons, and a machine
# without them has not broken anything.

set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
portal="$root/tools/xdg-desktop-portal-gowl/xdg-desktop-portal-gowl"
gowl="$root/build/release/gowl"

if [ ! -x "$portal" ] || [ ! -x "$gowl" ]; then
	echo "SKIP: portal or compositor not built"
	exit 0
fi
if ! command -v dbus-run-session >/dev/null 2>&1 \
   || ! command -v gdbus >/dev/null 2>&1; then
	echo "SKIP: dbus-run-session or gdbus is missing"
	exit 0
fi
if [ ! -S "${PIPEWIRE_RUNTIME_DIR:-${XDG_RUNTIME_DIR:-/run/user/$(id -u)}}/pipewire-0" ]; then
	echo "SKIP: PipeWire is not running"
	exit 0
fi

# PipeWire lives on the real runtime dir; the compositor gets a private
# one so its socket cannot collide with a live session's.
: "${PIPEWIRE_RUNTIME_DIR:=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}}"
export PIPEWIRE_RUNTIME_DIR
runtime=$(mktemp -d -p "${XDG_RUNTIME_DIR:-/tmp}" gowl-sc-test-XXXXXX)

comp_pid=""
portal_pid=""
cleanup () {
	[ -n "$portal_pid" ] && kill -TERM "$portal_pid" 2>/dev/null || true
	[ -n "$comp_pid" ] && kill -TERM "$comp_pid" 2>/dev/null || true
	rm -rf "$runtime"
}
trap cleanup EXIT INT TERM

GOWL_DISABLE_SYSTEMD=1 XDG_RUNTIME_DIR="$runtime" \
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
LD_LIBRARY_PATH="$root/build/release" \
	"$gowl" --no-c-config --config /dev/null >"$runtime/compositor.log" 2>&1 &
comp_pid=$!

# Wait for the compositor's socket rather than sleeping a guess.
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

# A chooser that takes the first offered source, so the handshake runs
# without a human.  The real one is a menu; see portal-screencast.c.
cat >"$runtime/chooser.sh" <<'EOF'
#!/bin/sh
head -1
EOF
chmod +x "$runtime/chooser.sh"

script=$(cat <<'SCRIPT'
set -e
"$PORTAL" >"$RUNTIME/portal.log" 2>&1 &
portal_pid=$!
# Wait by LISTING names, not by calling a method: a method call to a
# name nobody owns makes the bus ACTIVATE it from a service file, and
# this machine has an installed xdg-desktop-portal-gowl.  That copy
# would take the name from the one built here -- which is what the test
# is for -- and the symptom is a NoReply from a process that never had
# the session.
i=0
while [ $i -lt 100 ]; do
	if gdbus call --session --dest org.freedesktop.DBus \
	     --object-path /org/freedesktop/DBus \
	     --method org.freedesktop.DBus.ListNames 2>/dev/null \
	     | grep -q 'org.freedesktop.impl.portal.desktop.gowl'; then
		break
	fi
	i=$((i + 1))
	sleep 0.1
done
gdbus call --session --dest org.freedesktop.impl.portal.desktop.gowl \
	--object-path /org/freedesktop/portal/desktop \
	--method org.freedesktop.DBus.Properties.Get \
	org.freedesktop.impl.portal.ScreenCast AvailableSourceTypes \
	>"$RUNTIME/types.txt" 2>/dev/null || true
if [ ! -s "$RUNTIME/types.txt" ]; then
	echo "FAIL: the ScreenCast interface never appeared"
	cat "$RUNTIME/portal.log"
	exit 1
fi
# 1 = MONITOR, 2 = WINDOW.  Both, or the backend is no better than
# portal-wlr, which is the entire reason it exists.
case $(cat "$RUNTIME/types.txt") in
	*"uint32 3"*) : ;;
	*) echo "FAIL: AvailableSourceTypes is $(cat "$RUNTIME/types.txt"), wanted 3"
	   exit 1 ;;
esac

session=/org/freedesktop/portal/desktop/session/gowltest/s1
gdbus call --session --dest org.freedesktop.impl.portal.desktop.gowl \
	--object-path /org/freedesktop/portal/desktop \
	--method org.freedesktop.impl.portal.ScreenCast.CreateSession \
	/org/freedesktop/portal/desktop/request/gowltest/r1 "$session" \
	test.app "{}" >"$RUNTIME/create.txt"
grep -q "uint32 0" "$RUNTIME/create.txt" || {
	echo "FAIL: CreateSession: $(cat "$RUNTIME/create.txt")"
	cat "$RUNTIME/portal.log"; exit 1; }

# Monitors only: a headless compositor has an output and no windows.
gdbus call --session --dest org.freedesktop.impl.portal.desktop.gowl \
	--object-path /org/freedesktop/portal/desktop \
	--method org.freedesktop.impl.portal.ScreenCast.SelectSources \
	/org/freedesktop/portal/desktop/request/gowltest/r2 "$session" \
	test.app "{'types': <uint32 1>, 'cursor_mode': <uint32 1>}" \
	>"$RUNTIME/select.txt"
grep -q "uint32 0" "$RUNTIME/select.txt" || {
	echo "FAIL: SelectSources: $(cat "$RUNTIME/select.txt")"; exit 1; }

gdbus call --session --dest org.freedesktop.impl.portal.desktop.gowl \
	--object-path /org/freedesktop/portal/desktop \
	--method org.freedesktop.impl.portal.ScreenCast.Start \
	/org/freedesktop/portal/desktop/request/gowltest/r3 "$session" \
	test.app "" "{}" >"$RUNTIME/start.txt"
grep -q "uint32 0" "$RUNTIME/start.txt" || {
	echo "FAIL: Start: $(cat "$RUNTIME/start.txt")"
	cat "$RUNTIME/portal.log"; exit 1; }
# A stream, with a node id the application would connect to.
grep -q "'streams'" "$RUNTIME/start.txt" || {
	echo "FAIL: Start returned no streams: $(cat "$RUNTIME/start.txt")"
	exit 1; }
grep -q "'size'" "$RUNTIME/start.txt" || {
	echo "FAIL: the stream has no size: $(cat "$RUNTIME/start.txt")"
	exit 1; }

kill -TERM $portal_pid 2>/dev/null || true
echo "screencast portal PASSED (session, sources, a PipeWire stream)"
exit 0
SCRIPT
)

# XDG_DATA_DIRS is emptied into the throwaway tree on purpose: a private
# bus still reads the system's D-Bus service files, so an INSTALLED
# xdg-desktop-portal-gowl would be activated by the first method call and
# fight the one started here for the name.  Two backends, one of them
# built from other sources, is not what this is testing.
mkdir -p "$runtime/data"
PORTAL="$portal" RUNTIME="$runtime" \
XDG_RUNTIME_DIR="$runtime" WAYLAND_DISPLAY="$display" \
XDG_DATA_HOME="$runtime/data" XDG_DATA_DIRS="$runtime/data" \
GOWL_PORTAL_CHOOSER="$runtime/chooser.sh" \
	dbus-run-session -- sh -c "$script"
