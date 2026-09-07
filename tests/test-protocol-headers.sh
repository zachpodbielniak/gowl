#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: every generated protocol header a wlroots header pulls in by
# bare name must be listed in PROTO_HDRS.
#
# wlroots 0.19's public headers include generated protocol headers
# directly -- wlr_pointer_constraints_v1.h does
#
#     #include "pointer-constraints-unstable-v1-protocol.h"
#
# so every consumer has to run wayland-scanner over the XML itself.
# 0.20 replaced those with `#include <wayland-protocols/...-enum.h>',
# which wayland-protocols now installs, so nothing has to be generated.
#
# That difference makes the bug invisible where it is introduced.  gowl
# picks the NEWEST wlroots present, so a developer box carrying both
# versions always compiles the 0.20 path and never notices a missing
# rule.  It surfaced only on Fedora 43, which has 0.19 and nothing
# else, as a fatal error in a header nobody had edited -- and only
# inside a container build, which is the slowest possible place to find
# out.
#
# So this checks EVERY installed wlroots version, not just the one that
# would be selected.

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
fail=0
checked=0

# What PROTO_HDRS says we generate.
generated=$(sed -n '/^PROTO_HDRS *:=/,/[^\\]$/p' "$root/rules.mk" \
	| grep -oE '[a-z0-9-]+-protocol\.h' | sort -u)

if [ -z "$generated" ]; then
	echo "FAIL: could not read PROTO_HDRS from rules.mk"
	exit 1
fi

for incdir in /usr/include/wlroots-* /usr/local/include/wlroots-*; do
	[ -d "$incdir" ] || continue
	ver=$(basename "$incdir")

	# Only the wlr headers gowl actually includes matter; wlroots
	# ships plenty it never touches.
	need=""
	for wlr in "$incdir"/wlr/types/*.h "$incdir"/wlr/*.h; do
		[ -f "$wlr" ] || continue
		rel="wlr/${wlr#"$incdir"/wlr/}"
		grep -rqF "$rel" "$root/src" "$root/modules" "$root/tools" \
			2>/dev/null || continue
		got=$(grep -oE '^#include "[a-z0-9-]+-protocol\.h"' "$wlr" \
			| sed 's/^#include "//; s/"$//')
		need="$need $got"
	done

	need=$(echo "$need" | tr ' ' '\n' | grep -v '^$' | sort -u)
	[ -n "$need" ] || continue
	checked=$((checked + 1))

	for h in $need; do
		if ! echo "$generated" | grep -qx "$h"; then
			echo "FAIL: $ver needs $h, which PROTO_HDRS does not generate"
			echo "      (add it to PROTO_HDRS and give it a rule in rules.mk)"
			fail=1
		fi
	done
done

if [ "$checked" -eq 0 ]; then
	echo "SKIP: no wlroots headers require generated protocol headers"
	exit 0
fi

if [ "$fail" -eq 0 ]; then
	echo "PASS: protocol-header guard ($checked wlroots version(s) checked)"
fi

exit $fail
