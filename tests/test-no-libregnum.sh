#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: gowl must NEVER depend on libregnum / graylib / raylib.  External
# producers (e.g. cmacs rendering libregnum screensavers) only ever hand gowl
# raw pixel spans through the GowlFrameSink API; no engine symbols, headers, or
# build deps may leak in.  This script fails the build the moment that breaks,
# so it is a regression test, not just a review note.

set -e
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/release"
fail=0

# 1. No lrg_*/grl_* symbols anywhere in the gowl static / shared library.
for lib in "$out/libgowl.a" "$out"/libgowl.so*; do
	[ -e "$lib" ] || continue
	if nm "$lib" 2>/dev/null | grep -Eq '\b(lrg_|grl_)[a-z]'; then
		echo "FAIL: libregnum/graylib symbols found in $(basename "$lib")"
		nm "$lib" 2>/dev/null | grep -E '\b(lrg_|grl_)[a-z]' | head
		fail=1
	fi
done

# 2. No lrg_*/grl_* symbols in any built module .so.
for so in "$out"/modules/*.so "$out"/*.so; do
	[ -e "$so" ] || continue
	if nm -D "$so" 2>/dev/null | grep -Eq '\b(lrg_|grl_)[a-z]'; then
		echo "FAIL: libregnum/graylib symbols found in $(basename "$so")"
		fail=1
	fi
done

# 3. No forbidden engine #include in the raw-frame boundary source.
if grep -nE '^[[:space:]]*#[[:space:]]*include.*(libregnum|lrg-|graylib|raylib|<GL/)' \
		"$root/src/core/gowl-frame-sink.c" \
		"$root/src/core/gowl-frame-sink.h" 2>/dev/null; then
	echo "FAIL: forbidden engine #include in gowl-frame-sink"
	fail=1
fi

# 4. No libregnum/graylib/raylib in the gowl pkg-config dependency set.
if grep -nE 'DEPS[A-Z_]*[[:space:]]*[:+]?=.*\b(libregnum|graylib|raylib)\b' \
		"$root/config.mk" 2>/dev/null; then
	echo "FAIL: libregnum/graylib/raylib in gowl config.mk dependency set"
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	echo "no-libregnum guard FAILED"
	exit 1
fi
echo "no-libregnum guard PASSED (gowl carries no libregnum/graylib/raylib)"
exit 0
