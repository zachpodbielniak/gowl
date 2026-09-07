#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: the portal backend must serve BOTH directions of libei.
#
# A libei client declares itself sender or receiver at connect time, and
# the EIS implementation takes the opposite role:
#
#   receiver client -> a software KVM sharing THIS machine's input
#                      (InputCapture).  We create the device and push at it.
#   sender client   -> a software KVM DRIVING this machine from another
#                      (RemoteDesktop).  It pushes at us and we forward
#                      the events into the compositor.
#
# The backend used to disconnect senders on sight, which is why KVM server
# mode worked and client mode did not.  Nothing about that failure is
# visible: the D-Bus handshake succeeds, ConnectToEIS hands over a working
# fd, and the connection is dropped the instant libei says which way round
# it wants to be.  The client then sits there doing nothing, with no error
# anywhere.
#
# This is a source guard rather than a unit test because the machine has
# libeis (the server half) but not libei (the client half): there is no
# way to build a real sender client here to connect with.

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
eis="$root/tools/xdg-desktop-portal-gowl/portal-eis.c"
wl="$root/tools/xdg-desktop-portal-gowl/portal-wayland.c"
fail=0

if [ ! -f "$eis" ]; then
	echo "SKIP: $eis is missing (portal backend not in this tree)"
	exit 0
fi

# 1. A sender must not be hung up on.  The old code was
#    `if (eis_client_is_sender(client)) { eis_client_disconnect(client); }'
#    -- so the check is for a disconnect guarded by the sender test, not
#    for the sender test itself, which the code still legitimately uses to
#    decide which slot a client goes in.
if awk '/eis_client_is_sender/ { hot = 3 }
        hot > 0 { print; hot-- }' "$eis" | grep -q 'eis_client_disconnect'; then
	echo "FAIL: portal-eis.c disconnects a client on the strength of"
	echo "      eis_client_is_sender().  That is KVM client mode; it will"
	echo "      fail silently with no error on either side."
	fail=1
fi

# 2. Every event a sender client produces has to be handled, or that part
#    of the input simply vanishes -- keys arrive but the mouse does not,
#    or scrolling does nothing, with nothing logged.
for ev in EIS_EVENT_DEVICE_START_EMULATING \
          EIS_EVENT_POINTER_MOTION \
          EIS_EVENT_POINTER_MOTION_ABSOLUTE \
          EIS_EVENT_BUTTON_BUTTON \
          EIS_EVENT_SCROLL_DELTA \
          EIS_EVENT_SCROLL_DISCRETE \
          EIS_EVENT_KEYBOARD_KEY \
          EIS_EVENT_FRAME; do
	if ! grep -q "case $ev:" "$eis"; then
		echo "FAIL: portal-eis.c does not handle $ev; a sender client's"
		echo "      corresponding input is silently dropped"
		fail=1
	fi
done

# 3. The injection device needs regions or a client cannot address an
#    absolute position on it.  Both devices build them from the zones.
if ! grep -q 'eis_device_new_region' "$eis"; then
	echo "FAIL: portal-eis.c builds no device regions; absolute pointer"
	echo "      positioning has no coordinate space in either direction"
	fail=1
fi

# 4. Injection entry points must tolerate a missing inject object.  The
#    portal accepts an EIS connection before it knows whether the
#    compositor gave it anywhere to send events, and taking the process
#    down would also lose the capture direction that was working.
if [ -f "$wl" ]; then
	for fn in portal_wayland_inject_rel_motion \
	          portal_wayland_inject_button \
	          portal_wayland_inject_key \
	          portal_wayland_inject_frame; do
		body=$(awk -v fn="$fn" '
			$0 ~ "^" fn "\\(" { inside = 1 }
			inside { print }
			inside && /^}/ { exit }
		' "$wl")
		if ! printf '%s\n' "$body" | grep -q 'self->inject == NULL'; then
			echo "FAIL: $fn does not check for a missing inject object"
			fail=1
		fi
	done
fi

if [ "$fail" -ne 0 ]; then
	echo "portal guard FAILED"
	exit 1
fi
echo "portal guard PASSED (both libei directions served, injection guarded)"
exit 0
