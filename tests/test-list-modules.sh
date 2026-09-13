#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: --list-modules must report what is on disk, not a list somebody
# maintains by hand.
#
# It used to be a table in src/main.c.  Tables drift, and this one did:
# by the time anybody looked it was missing six of the twelve layouts and
# a third of everything else, which made it look as though those layouts
# were not modules at all.  Nothing failed -- a stale list still prints.
# So the check is the only thing that can notice.
#
# Also checks the order (a list nobody can scan is barely better than a
# wrong one) and that --list-modules-ex names a file for every module,
# which is what makes a shadowed copy visible.

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/release"
gowl="$out/gowl"
moddir="$out/modules"
fail=0

if [ ! -x "$gowl" ] || [ ! -d "$moddir" ]; then
	echo "SKIP: no built gowl or module directory"
	exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# What is actually there.
ls "$moddir"/*.so 2>/dev/null |
	sed 's|.*/||; s|\.so$||' | sort > "$tmp/on-disk"

# What the compositor says is there.  The table starts after the dashed
# rule and ends at the blank line before "Searched".
"$gowl" --list-modules 2>/dev/null |
	sed -n '/^  ------/,/^$/p' | sed '1d;/^$/d' |
	awk '{print $1}' | sort > "$tmp/reported"

if ! cmp -s "$tmp/on-disk" "$tmp/reported"; then
	echo "FAIL: --list-modules does not match $moddir"
	echo "  only on disk:"
	comm -23 "$tmp/on-disk" "$tmp/reported" | sed 's/^/    /'
	echo "  only reported:"
	comm -13 "$tmp/on-disk" "$tmp/reported" | sed 's/^/    /'
	fail=1
fi

# Sorted, so a long list can be read.
"$gowl" --list-modules 2>/dev/null |
	sed -n '/^  ------/,/^$/p' | sed '1d;/^$/d' |
	awk '{print $1}' > "$tmp/order"
if ! LC_ALL=C sort -c "$tmp/order" 2>/dev/null; then
	echo "FAIL: --list-modules is not in sorted order"
	fail=1
fi

# Every module has a description: a blank column is a module nobody can
# choose between.
if "$gowl" --list-modules 2>/dev/null |
	sed -n '/^  ------/,/^$/p' | sed '1d;/^$/d' |
	awk 'NF < 2 { print; found = 1 } END { exit !found }'
then
	echo "FAIL: some modules have no description"
	fail=1
fi

# -ex names the file each one would load from.
n_mods=$(wc -l < "$tmp/on-disk")
n_paths=$("$gowl" --list-modules-ex 2>/dev/null |
	sed -n '/^  ------/,/^$/p' | sed '1d;/^$/d' |
	grep -c "$moddir/" || true)
if [ "$n_paths" -ne "$n_mods" ]; then
	echo "FAIL: --list-modules-ex named $n_paths paths for $n_mods modules"
	fail=1
fi

if [ "$fail" -eq 0 ]; then
	echo "PASS: --list-modules reports all $n_mods modules on disk, in order"
fi
exit $fail
