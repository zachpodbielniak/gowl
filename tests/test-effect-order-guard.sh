#!/bin/sh
# gowl - source guard for scene-effect dispatch order
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# One rule, learned the expensive way: a scene-effect module must not
# decide whether to do its job by checking whether it is FIRST.
#
# gowl-effects.c dispatches per event -- every provider is offered each
# one and the first to claim it owns it -- so being at index 0 means
# nothing about whether a module should act.  Every shipped effect
# module also carries the same default priority, and which of them lands
# at index 0 is decided by a sort that has no reason to prefer any of
# them.
#
# The animation module had exactly this check.  Adding unrelated modules
# to the default set moved it off index 0, and it quietly stopped every
# window animation: pops, reveals and settling jiggles all gone, with no
# error, no warning, and nothing in the config to explain it.

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
fail=0

fail() {
	echo "FAIL: $1" >&2
	fail=1
}

# Comments stripped: the fix for this carries a comment that names the
# function, and a plain grep would be satisfied by the explanation of
# why the call is gone.
for f in modules/*/gowl-module-*.c; do
	code=$(sed -e 's://.*::' "$f" | awk 'BEGIN{c=0} {line=$0
		while (1) {
			if (c) { i=index(line,"*/"); if (!i) { line=""; break }
				line=substr(line,i+2); c=0; continue }
			i=index(line,"/*"); if (!i) break
			j=index(substr(line,i+2),"*/")
			if (!j) { line=substr(line,1,i-1); c=1; break }
			line=substr(line,1,i-1) substr(line,i+2+j+1) }
		print line}')

	echo "$code" | grep -q "gowl_module_manager_get_scene_effect(" || continue

	# Using it to FIND the top provider is fine; comparing it against
	# yourself to decide whether to act is the bug.
	if echo "$code" | grep -qE "gowl_module_manager_get_scene_effect\([^)]*\) *(!=|==)"; then
		fail "$f gates itself on being the first scene-effect provider; it will silently switch off when another module sorts ahead of it"
	fi
done

[ "$fail" -eq 0 ] || exit 1
echo "PASS: scene-effect order guard"
