#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: every portal interface data/gowl-portals.conf routes must land
# on a backend that actually implements it.
#
# portals.conf entries name backends, not interfaces.  Both
# `default=wlr' and `org.freedesktop.impl.portal.Secret=gnome-keyring'
# are a LIST that xdg-desktop-portal SEARCHES for a backend whose
# Interfaces= declares the interface being asked for -- and `default' is
# not one fallback backend, it is the list every interface without a
# line of its own resolves through.  Nothing validates the pairing: the
# frontend reads the file, finds no backend, and moves on.
#
# An interface left with no implementation is strictly worse than a
# portal that is not installed.  A missing portal answers with an error
# the application can report; an unrouted interface is never asked and
# never answers, and not a word of it is logged anywhere.
#
# org.freedesktop.impl.portal.Settings is the case that taught us this.
# It serves org.freedesktop.appearance color-scheme, so leaving it
# unrouted does not present as a portal fault at all -- it presents as
# every application set to "match system" coming up light-themed in a
# dark session, which reads as a theme or toolkit bug and gets chased
# there instead.
#
# Checked against gowl's own data/gowl.portal plus whatever is installed
# under the XDG data dirs, because a routing line can only be wrong
# relative to the backends that exist.  A machine with no installed
# backends -- a CI container -- cannot answer the question, and says so
# rather than passing.

set -e
CDPATH=
root=$(cd "$(dirname "$0")/.." && pwd)
conf="$root/data/gowl-portals.conf"
fail=0
checked=0

if [ ! -f "$conf" ]; then
	echo "SKIP: $conf is missing (no portal routing in this tree)"
	exit 0
fi

# Where an installed backend's .portal file can live.  /usr/share and
# /usr/local/share are appended unconditionally: a session that exported
# a trimmed XDG_DATA_DIRS would otherwise hide the system backends and
# turn a real routing hole into a skip.
portal_dirs=""
for d in $(printf '%s' "${XDG_DATA_DIRS:-}:/usr/local/share:/usr/share" \
           | tr ':' ' '); do
	if [ -d "$d/xdg-desktop-portal/portals" ]; then
		portal_dirs="$portal_dirs $d/xdg-desktop-portal/portals"
	fi
done

# Resolve a backend name to the .portal file that describes it.  gowl's
# own copy in data/ wins over an installed one on purpose: this tree is
# the source of truth for what the gowl backend implements, and the
# installed file is frequently an older build.
portal_file () {
	if [ -f "$root/data/$1.portal" ]; then
		printf '%s\n' "$root/data/$1.portal"
		return 0
	fi
	for _d in $portal_dirs; do
		if [ -f "$_d/$1.portal" ]; then
			printf '%s\n' "$_d/$1.portal"
			return 0
		fi
	done
	return 1
}

# Interfaces= is ';'-separated and conventionally trailing-';'.
portal_ifaces () {
	sed -n 's/^Interfaces=//p' "$1" | tr ';' ' '
}

# Every backend we can see, so the check knows which interfaces this
# machine is actually capable of serving.
backends="gowl"
installed=0
for d in $portal_dirs; do
	for f in "$d"/*.portal; do
		[ -f "$f" ] || continue
		installed=$((installed + 1))
		backends="$backends $(basename "$f" .portal)"
	done
done
backends=$(printf '%s' "$backends" | tr ' ' '\n' | sed '/^$/d' | sort -u \
           | tr '\n' ' ')

if [ "$installed" -eq 0 ]; then
	echo "SKIP: no .portal files under the XDG data dirs, so there is no"
	echo "      way to tell which interface any backend implements.  This"
	echo "      check needs an installed xdg-desktop-portal."
	exit 0
fi

# The interfaces some available backend implements -- i.e. the ones this
# session can be asked for and must therefore be able to route.  An
# interface nothing here implements is not a routing question.
available=""
for b in $backends; do
	if f=$(portal_file "$b"); then
		available="$available $(portal_ifaces "$f")"
	fi
done
# Kept space-separated, not newline-separated: `case " $available "' is
# how membership is tested below, and sort's newlines would never match.
available=$(printf '%s' "$available" | tr ' ' '\n' | sed '/^$/d' | sort -u \
            | tr '\n' ' ')

# Does any backend in a ';'-separated list implement $1?  A name that
# resolves to no .portal file is simply not a candidate: x-d-p skips it
# and carries on down the list.
routed () {
	_iface=$1
	for _b in $(printf '%s' "$2" | tr ';' ' '); do
		# `*' means "any backend"; portals.conf(5) allows it as a value.
		if [ "$_b" = "*" ]; then
			_cand=$backends
		else
			_cand=$_b
		fi
		for _c in $_cand; do
			if _f=$(portal_file "$_c"); then
				for _i in $(portal_ifaces "$_f"); do
					if [ "$_i" = "$_iface" ]; then
						return 0
					fi
				done
			fi
		done
	done
	return 1
}

# Strip comments and anything outside [preferred], then squeeze the
# whitespace out so each surviving line is a bare key=value.
entries=$(awk '
	/^[[:space:]]*[#;]/ { next }
	/^[[:space:]]*\[/ {
		section = $0
		gsub(/[[:space:]]/, "", section)
		next
	}
	section == "[preferred]" && index($0, "=") {
		line = $0
		gsub(/[[:space:]]/, "", line)
		if (line != "") { print line }
	}
' "$conf")

default=""
have_default=0
explicit=""
globkeys=""

for e in $entries; do
	key=${e%%=*}
	val=${e#*=}

	if [ "$key" = "default" ]; then
		default=$val
		have_default=1
		continue
	fi

	# `none' is a deliberate "this interface gets no backend".
	if [ "$val" = "none" ]; then
		explicit="$explicit $key"
		continue
	fi

	# portals.conf(5) allows a glob key.  Record it so the default
	# sweep below does not blame `default' for an interface one of
	# these covers, but say plainly that it was not verified.
	case $key in
	*'*'*)
		globkeys="$globkeys $key"
		echo "NOTE: $key is a glob; its routing is not checked here"
		continue
		;;
	esac

	explicit="$explicit $key"
	checked=$((checked + 1))

	if routed "$key" "$val"; then
		continue
	fi

	# Unroutable, but only a bug if this machine has something to
	# route to.  Where nothing installed implements the interface
	# there is no better line to write, and the file is not at fault.
	case " $available " in
	*" $key "*)
		echo "FAIL: $key=$val -- no backend in that list declares it"
		echo "      in its Interfaces=, though one installed here does,"
		echo "      so the interface is left with NO implementation."
		echo "      It will not error; it will simply never answer,"
		echo "      and nothing will log it."
		fail=1
		;;
	*)
		echo "NOTE: $key=$val -- no backend installed here implements"
		echo "      it, so the routing cannot be checked on this machine"
		;;
	esac
done

# Everything with no line of its own resolves through `default', so the
# same question has to be asked of every remaining interface a backend
# on this machine implements.
if [ "$have_default" -eq 0 ]; then
	echo "NOTE: no default= line; unlisted interfaces fall back to the"
	echo "      legacy UseIn= matching, which is not checkable here"
else
	dropped=""
	for i in $available; do
		case " $explicit " in
		*" $i "*) continue ;;
		esac
		skip=0
		for g in $globkeys; do
			# Deliberate glob match against a glob key.
			# shellcheck disable=SC2254
			case $i in
			$g) skip=1 ;;
			esac
		done
		[ "$skip" -eq 1 ] && continue
		checked=$((checked + 1))
		if routed "$i" "$default"; then
			continue
		fi
		dropped="$dropped $i"
	done

	if [ -n "$dropped" ]; then
		echo "FAIL: default=$default implements none of these, and they"
		echo "      have no line of their own, so each is left with NO"
		echo "      backend even though one is installed on this machine:"
		for i in $dropped; do
			echo "        $i"
		done
		echo "      An unrouted interface fails silently -- no error"
		echo "      reaches the application, and nothing is logged."
		case " $dropped " in
		*" org.freedesktop.impl.portal.Settings "*)
			echo "      Settings in particular serves"
			echo "      org.freedesktop.appearance color-scheme, so the"
			echo "      symptom is not a portal error at all: every app"
			echo "      set to \"match system\" comes up light-themed."
			echo "      Route it to gtk;gnome."
			;;
		esac
		echo "      Make default= a list that reaches a backend for each,"
		echo "      or give the interface an explicit line."
		fail=1
	fi
fi

if [ "$checked" -eq 0 ]; then
	echo "SKIP: $conf routes nothing"
	exit 0
fi

if [ "$fail" -eq 0 ]; then
	echo "PASS: portal-routing guard ($checked interface(s) checked)"
fi

exit $fail
