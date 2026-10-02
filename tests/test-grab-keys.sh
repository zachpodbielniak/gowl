#!/bin/sh
# gowl - GObject Wayland Compositor
# Copyright (C) 2026  Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Guard: the macro recorder, voice, OCR, colour picker and clipboard
# menu -- their default keys, their opt-in, and the rules that keep them
# from leaking or from blocking the desktop.
#
#   * The six keys in data/default-config.yaml, each bound once, to the
#     command it is documented as.  cmacs --gowl carries its own copy in
#     lisp/cmacs/cmacs-gowl.el, asserted by its ERT suite.
#   * Opt-in standalone: the macro, screenshot and clipboard modules are
#     off in the shipped config, so the keys are inert until enabled.
#   * The clipboard entry in menu.yaml is private, and the model honours
#     it: a private provider is never searched, and a row pasted from it
#     is never written to "recent".
#   * A private recording publishes no token.
#   * The OCR image goes to $XDG_RUNTIME_DIR, never the screenshots
#     folder, and children are watched on the compositor's event loop
#     (gowl_subprocess_*): GSubprocess / g_child_watch_add complete on
#     the default main context, which under cmacs is Emacs's thread.
#   * gowl-stt is ShellCheck-clean, when ShellCheck is installed.

set -e

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
fail=0
yaml="$root/data/default-config.yaml"
menu="$root/data/menu.yaml"

say () {
	echo "FAIL: $1"
	fail=1
}

# --- the six keys, exactly once each ---
check_bind () {
	key="$1"
	arg="$2"
	n=$(grep -c "^[[:space:]]*\"$key\":" "$yaml" || true)
	if [ "$n" != 1 ]; then
		say "$key is bound $n times in default-config.yaml (want 1)"
		return
	fi
	if ! grep -q "^[[:space:]]*\"$key\":[[:space:]]*{[[:space:]]*action:[[:space:]]*ipc_command,[[:space:]]*arg:[[:space:]]*\"$arg\"" "$yaml"; then
		say "$key is not ipc_command \"$arg\" in default-config.yaml"
	fi
}
check_bind "Super+Alt+Shift+s" "screenshot-ocr"
check_bind "Super+Alt+c" "screenshot-color"
check_bind "Super+Alt+r" "macro-record"
check_bind "Super+Alt+Shift+r" "macro-run last-recording"
check_bind "Super+Alt+m" "macro-voice"
check_bind "Super+Alt+v" "menu-open clipboard"

# --- opt-in: the modules behind them are off by default ---
for m in macro screenshot clipboard; do
	if ! awk -v m="$m" '
		$0 ~ "^  " m ":[[:space:]]*$" { inside = 1; next }
		inside && /^  [a-z]/ { exit }
		inside && /^    enabled:[[:space:]]*false/ { found = 1; exit }
		END { exit !found }' "$yaml"; then
		say "modules.$m is not enabled: false in default-config.yaml"
	fi
done

# --- the clipboard menu is private, and the model keeps it so ---
if ! awk '
	/^  - id: clipboard[[:space:]]*$/ { inside = 1; next }
	inside && /^  - id:/ { exit }
	inside && /^    provider: clipboard/ { p = 1 }
	inside && /^    private: true/ { q = 1 }
	END { exit !(p && q) }' "$menu"; then
	say "menu.yaml's clipboard entry is not provider: clipboard, private: true"
fi
c="$root/src/menu/gowl-menu.c"
grep -q 'c->provider != NULL && !c->is_private' "$c" \
	|| say "search_walk no longer skips a private provider's rows"
grep -q 'GOWL_MENU_RESULT_NONE && !parent->is_private' "$c" \
	|| say "activating a private row is written to the menu history"
grep -q '|| parent->is_private)' "$c" \
	|| say "\"recent\" can show a private provider's row"

# --- a private recording publishes no token ---
r="$root/src/core/gowl-input-recorder.c"
grep -q 'return self->owner != NULL ? NULL : self->token;' "$r" \
	|| say "public_token() no longer hides a private recording's token"
if grep -n 'json_builder_add_string_value(b, self->token)' "$r" >/dev/null; then
	say "a recorder payload writes the raw token"
fi

# --- OCR image placement; children on the event loop ---
s="$root/modules/screenshot/gowl-module-screenshot.c"
grep -q 'g_get_user_runtime_dir()' "$s" \
	|| say "the OCR image is no longer written to \$XDG_RUNTIME_DIR"
for f in "$s" "$root/modules/macro/gowl-module-macro.c"; do
	if grep -nE 'g_subprocess_|g_child_watch_add|g_spawn_async' "$f" >/dev/null; then
		say "$(basename "$f") spawns outside gowl_subprocess_*"
	fi
done

# --- gowl-stt ---
stt="$root/tools/gowl-stt/gowl-stt"
[ -x "$stt" ] || say "tools/gowl-stt/gowl-stt is missing or not executable"
if command -v shellcheck >/dev/null 2>&1; then
	shellcheck "$stt" || say "gowl-stt is not ShellCheck-clean"
fi

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "ok: grab keys, opt-in, private clipboard, private recordings, OCR placement"
