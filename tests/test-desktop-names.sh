#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: every session .desktop must advertise a desktop name that
# Chromium recognises, and must advertise it LAST.
#
# Chromium -- and therefore every Electron app -- chooses its password
# store by splitting XDG_CURRENT_DESKTOP on ':' and comparing each name
# against a fixed list.  A session naming none of them resolves to
# DESKTOP_ENVIRONMENT_OTHER, and key_storage_util_linux then picks the
# PLAINTEXT backend instead of libsecret.  What the user sees is an app
# saying it cannot save the session and they will have to log in again
# -- including for secrets it had already stored under a session that
# did report a known desktop, because the backend changed under them.
#
# The ordering half matters just as much.  xdg-desktop-portal resolves
# <desktop>-portals.conf and UseIn= by FIRST match over the same list,
# so gowl's own names have to come before the compatibility one or the
# InputCapture/RemoteDesktop routing silently moves to another backend.
#
# Nothing at runtime can catch either half: both fail quietly, in a
# different process, long after the session started.

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
fail=0
checked=0

# base/nix/xdg_util.cc: the names Chromium maps to a real desktop.
# Everything else is DESKTOP_ENVIRONMENT_OTHER.
known="GNOME KDE XFCE Unity X-Cinnamon Pantheon UKUI LXQt Deepin"

# The names gowl needs to win a first-match lookup.
ours="gowl wlroots"

for f in "$root"/data/*.desktop; do
	[ -f "$f" ] || continue
	names=$(sed -n 's/^DesktopNames=//p' "$f")
	[ -n "$names" ] || continue
	checked=$((checked + 1))
	base=$(basename "$f")

	# Split on ':' and ';' -- the spec allows either as a separator.
	list=$(echo "$names" | tr ':;' '  ')

	hit=""
	for n in $list; do
		for k in $known; do
			if [ "$n" = "$k" ]; then
				hit="$n"
				break
			fi
		done
		[ -n "$hit" ] && break
	done

	if [ -z "$hit" ]; then
		echo "FAIL: $base DesktopNames=$names names no desktop Chromium"
		echo "      knows, so every Electron app falls back to the"
		echo "      plaintext password store and loses saved logins."
		echo "      Append one of: $known"
		fail=1
		continue
	fi

	# The compatibility name must not out-rank gowl's own names.
	for o in $ours; do
		case " $list " in
		*" $o "*) ;;
		*) continue ;;
		esac
		pos_ours=0
		pos_hit=0
		i=0
		for n in $list; do
			i=$((i + 1))
			[ "$n" = "$o" ] && [ "$pos_ours" -eq 0 ] && pos_ours=$i
			[ "$n" = "$hit" ] && [ "$pos_hit" -eq 0 ] && pos_hit=$i
		done
		if [ "$pos_ours" -gt "$pos_hit" ]; then
			echo "FAIL: $base lists '$hit' before '$o'.  xdg-desktop-portal"
			echo "      resolves <desktop>-portals.conf and UseIn by first"
			echo "      match, so InputCapture/RemoteDesktop would leave the"
			echo "      gowl backend.  Put the compatibility name last."
			fail=1
		fi
	done
done

if [ "$checked" -eq 0 ]; then
	echo "SKIP: no session .desktop files with DesktopNames"
	exit 0
fi

if [ "$fail" -eq 0 ]; then
	echo "PASS: desktop-names guard ($checked session file(s) checked)"
fi

exit $fail
