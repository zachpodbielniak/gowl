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

# 2b. A wheel notch leaves as a notch.  The capture protocol carries the
#     discrete amount (120ths, the unit libei shares); the portal used to
#     take it off the wire and throw it away, sending every scroll as a
#     continuous delta -- the ingress bug seen from the other end, where
#     the remote compositor then announced a wheel with no wheel amount
#     and clients that trust the source scrolled by nothing.
if ! awk '/^portal_eis_scroll\(/,/^}/' "$eis" | sed 's,/\*.*\*/,,' \
		| grep -vE '^[[:space:]]*\*' | grep -q 'eis_device_scroll_discrete('; then
	echo "FAIL: portal_eis_scroll() never calls eis_device_scroll_discrete;"
	echo "      the receiver gets every wheel notch as a continuous delta"
	fail=1
fi
if awk '/^cap_axis\(/,/^}/' "$wl" | grep -q '(void)discrete'; then
	echo "FAIL: portal-wayland.c cap_axis discards the discrete amount"
	echo "      the compositor sent"
	fail=1
fi

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

# Guard: the ScreenCast backend must capture WINDOWS, not just outputs.
#
# The whole reason gowl serves ScreenCast at all is that
# xdg-desktop-portal-wlr cannot share a single window: wlr-screencopy
# addresses outputs.  A backend that bound only the output source
# manager would build, run, and pass every other check while quietly
# being a worse portal-wlr -- so the toplevel source manager, the
# toplevel list and WINDOW in AvailableSourceTypes are asserted here.
sc="$root/tools/xdg-desktop-portal-gowl/portal-capture.c"
if [ -f "$sc" ]; then
	for sym in ext_foreign_toplevel_image_capture_source_manager_v1_create_source \
	           ext_output_image_capture_source_manager_v1_create_source \
	           ext_image_copy_capture_manager_v1_create_session; do
		if ! grep -q "$sym" "$sc"; then
			echo "FAIL: portal-capture.c never calls $sym"
			fail=1
		fi
	done
else
	echo "FAIL: the ScreenCast capture backend is missing"
	fail=1
fi

sccast="$root/tools/xdg-desktop-portal-gowl/portal-screencast.c"
if [ -f "$sccast" ]; then
	if ! grep -q 'SOURCE_TYPE_WINDOW' "$sccast"; then
		echo "FAIL: portal-screencast.c does not offer window sources"
		fail=1
	fi
	# The routing has to name the interface, or the frontend never asks.
	if ! grep -q 'org.freedesktop.impl.portal.ScreenCast' \
	     "$root/data/gowl.portal"; then
		echo "FAIL: gowl.portal does not declare ScreenCast"
		fail=1
	fi
fi

# Guard: what ScreenCast ADVERTISES, it does.
#
# A version number and a cursor-mode bitmask are promises an application
# acts on without checking.  Version 4 promises restore (persist_mode and
# restore_data in Start's results), 6 promises pipewire-serial, and the
# METADATA cursor bit (4) promises SPA_META_Cursor buffers.  The backend
# once said version 4 while implementing none of restore; an app that
# asked to persist was simply asked again every call, with nothing to
# say why.
if [ -f "$sccast" ]; then
	version=$(sed -n 's/^#define SCREENCAST_VERSION \([0-9]*\).*/\1/p' "$sccast")
	if [ -z "$version" ]; then
		echo "FAIL: portal-screencast.c has no SCREENCAST_VERSION"
		fail=1
	else
		if [ "$version" -ge 4 ]; then
			for key in "\"persist_mode\"" "\"restore_data\""; do
				if ! grep -q "$key" "$sccast"; then
					echo "FAIL: ScreenCast says v$version but never handles $key"
					fail=1
				fi
			done
		fi
		if [ "$version" -ge 6 ] && ! grep -q '"pipewire-serial"' "$sccast"; then
			echo "FAIL: ScreenCast says v$version but sends no pipewire-serial"
			fail=1
		fi
	fi
	modes=$(sed -n 's/^#define CURSOR_MODES_AVAILABLE (\(.*\))$/\1/p' "$sccast")
	case "$modes" in
	*METADATA*)
		if ! grep -q 'SPA_META_Cursor' \
		     "$root/tools/xdg-desktop-portal-gowl/portal-pw.c"; then
			echo "FAIL: METADATA cursors are advertised but portal-pw.c"
			echo "      produces no SPA_META_Cursor"
			fail=1
		fi
		;;
	"")
		echo "FAIL: portal-screencast.c has no CURSOR_MODES_AVAILABLE"
		fail=1
		;;
	esac
	if ! grep -q 'AvailableCursorModes.*' "$sccast" \
	   || ! awk '/"AvailableCursorModes"/ { hot = 4 } hot > 0 { print; hot-- }' \
	        "$sccast" | grep -q 'CURSOR_MODES_AVAILABLE'; then
		echo "FAIL: AvailableCursorModes is not CURSOR_MODES_AVAILABLE;"
		echo "      the property and the SelectSources check can disagree"
		fail=1
	fi
fi

# Guard: with PW_STREAM_FLAG_ALLOC_BUFFERS the BACKEND allocates the
# buffer memory in add_buffer.  It used to expect a ready-made memfd,
# find none, and return: every cast negotiated and delivered no frame.
pw="$root/tools/xdg-desktop-portal-gowl/portal-pw.c"
if [ -f "$pw" ] && grep -q 'PW_STREAM_FLAG_ALLOC_BUFFERS' "$pw" \
   && ! awk '/^on_stream_add_buffer\(/,/^}/' "$pw" | grep -q 'memfd_create('; then
	echo "FAIL: portal-pw.c asks to allocate buffers but add_buffer makes none"
	fail=1
fi

# Guard: grants are a standing permission to record the screen, and are
# written owner-only.
restore="$root/tools/xdg-desktop-portal-gowl/portal-restore.c"
if [ -f "$restore" ] && ! grep -q 'G_FILE_SET_CONTENTS_CONSISTENT, 0600' "$restore"; then
	echo "FAIL: portal-restore.c does not write the grants file 0600"
	fail=1
fi

# Guard: Screenshot is served, declared and routed.
ss="$root/tools/xdg-desktop-portal-gowl/portal-screenshot.c"
if [ -f "$ss" ]; then
	for sym in "'Screenshot'" "'PickColor'" "'AvailableTargets'" \
	           portal_capture_grab; do
		if ! grep -q "$sym" "$ss"; then
			echo "FAIL: portal-screenshot.c lacks $sym"
			fail=1
		fi
	done
	if ! grep -q 'org.freedesktop.impl.portal.Screenshot' \
	     "$root/data/gowl.portal"; then
		echo "FAIL: gowl.portal does not declare Screenshot"
		fail=1
	fi
	if ! grep -q '^org.freedesktop.impl.portal.Screenshot=gowl' \
	     "$root/data/gowl-portals.conf"; then
		echo "FAIL: gowl-portals.conf does not route Screenshot to gowl"
		fail=1
	fi
else
	echo "FAIL: the Screenshot backend is missing"
	fail=1
fi

# Guard: the chooser order.  An explicit $GOWL_PORTAL_CHOOSER beats the
# menu card, and the card beats the dmenu family -- inside gowl the card
# is the session's own menu, and a bemenu that happens to be installed
# should not pop up instead of it.
ch="$root/tools/xdg-desktop-portal-gowl/portal-chooser.c"
if [ -f "$ch" ]; then
	body=$(awk '/^begin\(/,/^}/' "$ch")
	env_at=$(printf '%s\n' "$body" | grep -n 'dmenu_start(task, env)' | head -1 | cut -d: -f1)
	menu_at=$(printf '%s\n' "$body" | grep -n 'menu_start(' | head -1 | cut -d: -f1)
	dmenu_at=$(printf '%s\n' "$body" | grep -n 'dmenu_command(' | head -1 | cut -d: -f1)
	if [ -z "$env_at" ] || [ -z "$menu_at" ] || [ -z "$dmenu_at" ] \
	   || [ "$env_at" -gt "$menu_at" ] || [ "$menu_at" -gt "$dmenu_at" ]; then
		echo "FAIL: the chooser's begin() is not env, then menu card, then dmenu"
		fail=1
	fi
	# Nothing waits on a human synchronously: the portal's one main
	# loop feeds every running cast.  A blocking wait froze the cast in
	# somebody else's call for as long as a person took to choose.
	for f in "$ch" "$sccast" "$ss"; do
		[ -f "$f" ] || continue
		if sed 's,/\*.*\*/,,' "$f" \
		     | grep -E 'g_subprocess_communicate(_utf8)?\(|g_subprocess_wait(_check)?\(|portal_gowl_wait_event' \
		     >/dev/null; then
			echo "FAIL: $(basename "$f") waits on a human synchronously"
			fail=1
		fi
	done
	for f in "$sccast" "$ss"; do
		if [ -f "$f" ] && ! grep -q 'portal_request_new' "$f"; then
			echo "FAIL: $(basename "$f") exports no Request object, so the"
			echo "      frontend cannot withdraw a pending pick"
			fail=1
		fi
	done
	if ! grep -q 'events.subscribe' "$ch" || ! grep -q 'menu-choose ' "$ch"; then
		echo "FAIL: the menu chooser must subscribe before menu-choose,"
		echo "      or the answer can arrive unheard"
		fail=1
	fi
else
	echo "FAIL: portal-chooser.c is missing"
	fail=1
fi

# Guard: the top-level rule rebuilds the portal on ANY of its sources.
# It used to name four files, and the tests ran against a stale binary.
if ! grep -q 'PORTAL_SOURCES := ' "$root/Makefile" \
   || ! grep -q 'tools/xdg-desktop-portal-gowl/\*\.c' "$root/Makefile"; then
	echo "FAIL: the Makefile's portal rule does not depend on every source"
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	echo "portal guard FAILED"
	exit 1
fi
echo "portal guard PASSED (both libei directions served, injection guarded,"
echo "                     ScreenCast captures windows as well as outputs,"
echo "                     advertises only what it does; Screenshot routed;"
echo "                     chooser order, asynchronous; grants 0600;"
echo "                     cast buffers allocated)"
exit 0
