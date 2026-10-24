#!/bin/bash
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: docs/hotkey.org cannot drift from the examples it shows or from
# the module it describes.
#
#   1. docs/hotkey.org is generated from docs/hotkey.org.in: every
#      `@@FILE:PATH:LANG@@' line there becomes a src block holding PATH
#      verbatim (org-escaped).  The checked-in page must equal a fresh
#      generation, so an edited example is an edited page -- and the
#      examples are the files the test suite loads (test-hotkey-config.c),
#      so every block on the page is code a test runs.  A page that is
#      also readable on a forge, where #+INCLUDE is not expanded;
#   2. every data/example-hotkey* file and every cookbook case is on the
#      page;
#   3. every IPC verb hk_dispatch() answers, every action kind the model
#      reads, and every hook event, is mentioned on the page.
#
# Usage:
#   tests/test-hotkey-docs.sh            check (what `make test' runs)
#   tests/test-hotkey-docs.sh --write    regenerate docs/hotkey.org

set -euo pipefail
CDPATH=
root=$(cd -- "$(dirname -- "${0}")/.." >/dev/null && pwd)
src="${root}/docs/hotkey.org.in"
page="${root}/docs/hotkey.org"
mod="${root}/modules/hotkey/gowl-module-hotkey.c"
model="${root}/modules/hotkey/gowl-hotkey-model.c"
fail=0

# generate: print the page for docs/hotkey.org.in on stdout
generate () {
	local line path lang

	while IFS= read -r line || [[ -n "${line}" ]]
	do
		if [[ "${line}" =~ ^@@FILE:([^:]+):([a-z]+)@@$ ]]
		then
			path="${BASH_REMATCH[1]}"
			lang="${BASH_REMATCH[2]}"
			if [[ ! -f "${root}/${path}" ]]
			then
				echo "FAIL: ${path} named in docs/hotkey.org.in does not exist" >&2
				return 1
			fi
			printf '#+caption: =%s=\n#+begin_src %s\n' "${path}" "${lang}"
			# org escapes a src line starting with * or #+ by a comma
			sed -E 's/^(,*)(\*|#\+)/,\1\2/' "${root}/${path}"
			printf '#+end_src\n'
		else
			printf '%s\n' "${line}"
		fi
	done < "${src}"
}

if [[ ! -f "${src}" ]]
then
	echo "FAIL: docs/hotkey.org.in is missing"
	exit 1
fi

if [[ "${1:-}" == "--write" ]]
then
	generate > "${page}.tmp"
	mv -- "${page}.tmp" "${page}"
	echo "wrote docs/hotkey.org"
	exit 0
fi

# 1
if ! diff -u "${page}" <(generate) > /dev/null 2>&1
then
	diff -u "${page}" <(generate) | head -40 || true
	echo "FAIL: docs/hotkey.org is stale: run tests/test-hotkey-docs.sh --write (rule 1)"
	fail=1
fi

# 2
for f in "${root}"/data/example-hotkey* "${root}"/data/example-hotkey-cases/*
do
	[[ -d "${f}" ]] && continue
	rel="${f#"${root}"/}"
	if ! grep -qF "@@FILE:${rel}:" "${src}"
	then
		echo "FAIL: ${rel} is not shown in docs/hotkey.org (rule 2)"
		fail=1
	fi
done

# 3: IPC verbs -- the strings hk_dispatch() compares the verb against
verbs=$(awk '/^hk_dispatch\(/ { inside = 1 } inside { print } inside && /^}/ { exit }' "${mod}" \
	| grep -oE 'g_strcmp0\(verb, "[a-z-]+"\)' | sed -E 's/.*"([a-z-]+)".*/\1/' | sort -u)
if [[ -z "${verbs}" ]]
then
	echo "FAIL: found no IPC verbs in hk_dispatch() -- this guard needs updating (rule 3)"
	fail=1
fi
for v in ${verbs}
do
	if ! grep -qE "hotkey-${v}([^a-z-]|$)" "${src}"
	then
		echo "FAIL: IPC command hotkey-${v} is not documented (rule 3)"
		fail=1
	fi
done

# 3: action kinds -- action_keys[] in the model
kinds=$(grep -oE '\{ *"[a-z-]+", *HK_ACT_[A-Z_]+' "${model}" | sed -E 's/\{ *"([a-z-]+)".*/\1/' | sort -u)
if [[ -z "${kinds}" ]]
then
	echo "FAIL: found no action kinds in the model -- this guard needs updating (rule 3)"
	fail=1
fi
for k in ${kinds}
do
	if ! grep -qE "[={]${k}[:=]" "${src}"
	then
		echo "FAIL: action kind ${k} is not documented (rule 3)"
		fail=1
	fi
done

# 3: hook events -- hook_events[] in the model
events=$(awk '/hook_events\[\] = \{/ { inside = 1; next } inside && /\};/ { exit } inside { print }' "${model}" \
	| grep -oE '"[a-z-]+"' | tr -d '"' | sort -u)
if [[ -z "${events}" ]]
then
	echo "FAIL: found no hook events in the model -- this guard needs updating (rule 3)"
	fail=1
fi
for e in ${events}
do
	if ! grep -qE "=${e}=" "${src}"
	then
		echo "FAIL: hook event ${e} is not documented (rule 3)"
		fail=1
	fi
done

[[ "${fail}" == 0 ]] && echo "PASS: docs/hotkey.org matches its examples and the module"
exit "${fail}"
