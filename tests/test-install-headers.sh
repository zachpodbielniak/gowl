#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: `make install-headers' must install EVERY public header, and
# must not stop early.
#
# The rule used to be one MKDIR_P/INSTALL_DATA pair per subdirectory,
# which failed in two ways that both looked like nothing at all:
#
#   * src/layout holds only .c files, so `install src/layout/*.h' died
#     on the unexpanded glob.  make stopped there, and every directory
#     listed after it -- src/ipc, src/util -- was silently never
#     installed.  In a container build the failure was swallowed
#     entirely by the surrounding `for dep in ...' loop.
#   * A new subdirectory needed its own two lines, and src/fx and
#     src/barkit never got them.  A bar plugin compiled against an
#     INSTALLED gowl therefore could not find gowl/barkit/*.h -- the
#     header set that IS the plugin contract.
#
# So this asserts the outcome rather than the recipe: install into a
# throwaway DESTDIR and check that every src/<dir>/*.h landed.

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
fail=0

dest=$(mktemp -d)
trap 'rm -rf "$dest"' EXIT

if ! make -C "$root" install-headers DESTDIR="$dest" PREFIX=/usr \
	>"$dest/.log" 2>&1
then
	echo "FAIL: make install-headers exited non-zero"
	tail -20 "$dest/.log"
	exit 1
fi

inc="$dest/usr/include/gowl"

# 1. Every subdirectory named in LIB_HDRS -- i.e. every directory whose
#    headers are part of the library -- is covered by HEADER_SUBDIRS,
#    and every header in it is installed.  Deriving the expectation
#    from LIB_HDRS rather than from HEADER_SUBDIRS is the point: adding
#    a subsystem to the library and forgetting the install list is
#    exactly the bug this catches.
#
#    src/bar (the standalone gowlbar client), src/protocols (generated)
#    and src/config.h (build config) are not in LIB_HDRS and are
#    correctly not installed.
want_dirs=$(sed -n '/^LIB_HDRS *:=/,/[^\\]$/p' "$root/Makefile" \
	| grep -oE 'src/[a-z]+/' | sed 's|src/||; s|/$||' | sort -u)

for sub in $want_dirs; do
	for h in "$root/src/$sub/"*.h; do
		[ -e "$h" ] || continue
		name=$(basename "$h")
		if [ ! -f "$inc/$sub/$name" ]; then
			echo "FAIL: header not installed: $sub/$name"
			echo "      (is '$sub' in HEADER_SUBDIRS in config.mk?)"
			fail=1
		fi
	done
done

# The master include and the types/enums/version headers it pulls in.
for name in gowl.h gowl-types.h gowl-enums.h gowl-version.h; do
	if [ ! -f "$inc/$name" ]; then
		echo "FAIL: top-level header not installed: $name"
		fail=1
	fi
done

# 2. The barkit contract specifically.  A plugin includes these by
#    name; a missing one is the difference between "plugins work on an
#    installed gowl" and "plugins work only in the dev tree".
for h in gowl-bar-plugin.h gowl-bar-plugin-proxy.h gowl-bar-registry.h \
         gowl-bar-panel.h gowl-bar-theme.h gowl-bar-host.h; do
	if [ ! -f "$inc/barkit/$h" ]; then
		echo "FAIL: barkit plugin contract header missing: $h"
		fail=1
	fi
done

# 3. A subdirectory with no headers must be skipped, not turned into an
#    empty directory -- and must not stop the rule.
if [ -d "$inc/layout" ] && [ -z "$(ls -A "$inc/layout" 2>/dev/null)" ]; then
	echo "FAIL: empty directory created for a headerless subdir (layout)"
	fail=1
fi

if [ "$fail" -eq 0 ]; then
	n=$(find "$inc" -name '*.h' | wc -l)
	echo "PASS: install-headers installed $n headers, none missing"
fi

exit $fail
