#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: both shipped configs bind Super+Ctrl+s to scratchpad-remove.
#
# It is the key people reach for to take a window back out of the
# scratchpad, and an unbound Super chord is not swallowed: it goes to the
# focused window, where a terminal takes it as Ctrl+S -- XOFF -- and stops
# echoing.  The window looks dead, it is still in the scratchpad, and the
# key that should have brought it back is what froze it.
# Super+Ctrl+Shift+s does the same and stays bound beside it.
#
# Both copies: data/default-config.yaml, and the one src/main.c embeds
# for when no config file is found.

set -e
# An inherited CDPATH makes `cd' print the directory it resolved.
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
fail=0

# The YAML puts the value on the line after the key (or on the same one).
if ! grep -A1 '^[[:space:]]*"Super+Ctrl+s":' "$root/data/default-config.yaml" \
	| grep -q 'arg: "scratchpad-remove"'
then
	echo "FAIL: data/default-config.yaml does not bind Super+Ctrl+s to"
	echo "      scratchpad-remove; a terminal in the scratchpad would get"
	echo "      Ctrl+S (XOFF) and freeze"
	fail=1
fi

if ! grep -F '\"Super+Ctrl+s\"' "$root/src/main.c" \
	| grep -qF 'arg: \"scratchpad-remove\"'
then
	echo "FAIL: the config src/main.c embeds does not bind Super+Ctrl+s"
	echo "      to scratchpad-remove"
	fail=1
fi

if [ "$fail" -eq 0 ]; then
	echo "PASS: both shipped configs bind Super+Ctrl+s to scratchpad-remove"
fi
exit "$fail"
