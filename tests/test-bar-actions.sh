#!/bin/sh
# gowl - source guard: every panel control reaches a handler
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# A panel control is wired by matching STRINGS: the builder passes an id
# to gowl_bar_panel_add_toggle() and the action handler compares
# item_id against it.  Nothing checks that the two agree.  A typo, a
# renamed control or a handler that was never written all produce the
# same thing -- a control that draws, highlights, clicks, and does
# nothing at all -- with no warning at build time and none at runtime.
#
# So: every literal id a plugin file emits must be answered somewhere in
# that same file, and every id it answers must be one it can actually
# emit.  The second half catches the more interesting mistake, which is
# a handler left behind pointing at a control that no longer exists.
#
# Ids built from a format string ("dev:%u") are checked by their static
# prefix, which is what a g_str_has_prefix handler matches on.

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
fail=0

# Which item kinds are actually clickable, taken from the renderer
# rather than assumed: it is the only thing that decides, and a guard
# that keeps its own list would drift from it silently.
interactive=" $(grep -ohE 'pass_emit_hit\(p, [^,]+, [^,]+, GOWL_BAR_ITEM_[A-Z_]+' \
	src/barkit/gowl-bar-panel-render.c \
	| sed 's/.*GOWL_BAR_ITEM_//' | sort -u | tr '\n' ' ')"

for f in modules/bar/bar-plugins-*.c; do
	# Ids handed to a panel builder.  The id is the argument after the
	# panel, so take the second quoted string on the call line.
	#
	# add_hero is deliberately absent: its second argument is an ICON,
	# not an id, and a hero carries no action.  Including it reported
	# every glyph in the tree as an unhandled control.
	emitted=$( { grep -ohE \
		'gowl_bar_panel_add_(toggle|buttons|slider|row)\(panel, *"[^"]+"' "$f" \
		| sed 's/.*panel, *"\([^"]*\)".*/\1/'
	    # An id can also be attached after the fact, which is how a
	    # control built by a shared helper gets named.  Only count it
	    # when the item's KIND actually emits a hit rect: a calendar
	    # grid carries an id and is not clickable, and demanding a
	    # handler for it would be demanding dead code.
	    awk -v kinds="$interactive" '
		/gowl_bar_panel_item_new\(GOWL_BAR_ITEM_/ {
			k = $0
			sub(/.*GOWL_BAR_ITEM_/, "", k)
			sub(/[^A-Z_].*/, "", k)
			last_kind = k
		}
		# The add_* builders establish a kind too, and do not go
		# through item_new -- a hero named after the fact is how the
		# audio panel gets its mute switch.
		/gowl_bar_panel_add_(hero|row|toggle|slider|buttons)\(/ {
			k = $0
			sub(/.*gowl_bar_panel_add_/, "", k)
			sub(/\(.*/, "", k)
			last_kind = toupper(k)
		}
		/gowl_bar_panel_item_set_id\(/ {
			if (index(kinds, " " last_kind " ") == 0) next
			line = $0
			sub(/.*item_set_id\([a-z_]+, *"/, "", line)
			sub(/".*/, "", line)
			print line
		}' "$f"
	  } | sed 's/%.*//' | sort -u)

	# Ids the action handler answers.
	handled=$(grep -ohE \
		'(g_strcmp0|g_str_has_prefix)\(item_id, *"[^"]*"' "$f" \
		| sed 's/.*item_id, *"\([^"]*\)".*/\1/' \
		| sed 's/%.*//' | sort -u)

	[ -n "$emitted" ] || continue

	for id in $emitted; do
		echo "$handled" | grep -qxF "$id" || {
			# Allow a prefix handler to answer a longer id.
			matched=no
			for h in $handled; do
				case "$id" in
				"$h"*) matched=yes ;;
				esac
			done
			[ "$matched" = yes ] || {
				echo "FAIL: $f emits panel control '$id' that no handler answers" >&2
				echo "      The control will draw, hover and click, and do nothing." >&2
				fail=1
			}
		}
	done

	for id in $handled; do
		matched=no
		for e in $emitted; do
			case "$e" in
			"$id"*) matched=yes ;;
			esac
		done
		# A handler may legitimately answer an id built entirely at
		# run time; those appear as a bare prefix ending in ':'.
		case "$id" in
		*:) matched=yes ;;
		esac
		[ "$matched" = yes ] || {
			echo "FAIL: $f answers panel id '$id' that it never emits" >&2
			echo "      Either the control was renamed or removed and this" >&2
			echo "      handler was left behind." >&2
			fail=1
		}
	done
done

[ "$fail" -eq 0 ] || exit 1
echo "PASS: every panel control reaches a handler"
