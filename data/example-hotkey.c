/*
 * gowl - Example C configuration: hotkey (AutoHotkey for gowl)
 * GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Four ways a C config extends the hotkey module (docs/hotkey.org, "The
 * C API"):
 *
 *   1. a hotkey that runs a C function, in one call:
 *      gowl_hotkey_define();
 *   2. a C function YAML hotkeys call by name: {call: NAME};
 *   3. a context -- a C predicate -- hotkeys name: context: NAME;
 *   4. a whole `hotkey:' section written right here, process-wide, so
 *      it survives a config reload: gowl_hotkey_add_yaml().
 *
 * Merge add_hotkeys() into ~/.config/gowl/config.c and call it from
 * gowl_config_init(); then `gowl --recompile'.  Everything is safe to
 * register before the module loads.  The module is opt-in:
 *
 *   modules:
 *     hotkey: { enabled: true }
 */

#include <gowl/gowl.h>

/* 1. Super+Alt+w: say which window has the keyboard, in a toast */
static void
example_which_window(
	GowlHotkeyCall *call,
	gpointer        data
){
	GowlClient *c = gowl_hotkey_call_get_focused(call);
	g_autofree gchar *line = NULL;

	(void)data;
	line = g_strdup_printf("bar-notify %s|%s",
	                       c != NULL ? gowl_client_get_app_id(c) : "nothing",
	                       c != NULL ? gowl_client_get_title(c) : "");
	g_free(gowl_hotkey_command(gowl_hotkey_call_get_compositor(call), line));
}

/* 2. `say': types its arguments, joined by spaces, and answers with how
 * many characters that was (hotkey-call say hello world -> OK 11) */
static void
example_say(
	GowlHotkeyCall *call,
	gpointer        data
){
	g_autofree gchar *text = g_strjoinv(" ",
		(gchar **)gowl_hotkey_call_get_args(call));
	g_autofree gchar *n = NULL;

	(void)data;
	gowl_hotkey_send_text(gowl_hotkey_call_get_compositor(call), text);
	n = g_strdup_printf("%ld", g_utf8_strlen(text, -1));
	gowl_hotkey_call_set_result(call, n);
}

/* 3. `floating': the focused window floats.  A context is asked while a
 * key is being decided, so it only reads -- quick and side-effect free. */
static gboolean
example_floating(
	GowlHotkeyCall *call,
	gpointer        data
){
	GowlClient *c = gowl_hotkey_call_get_focused(call);

	(void)data;
	return c != NULL && gowl_client_get_floating(c);
}

/*
 * add_hotkeys:
 *
 * Everything above, registered.
 *
 * Returns: %TRUE when the key and the inline YAML parsed
 */
static gboolean
add_hotkeys(void)
{
	GError *error = NULL;

	/* 1. */
	if (!gowl_hotkey_define("which-window", "#!w", example_which_window,
	                        NULL, NULL, &error)) {
		g_warning("hotkey example: %s", error->message);
		g_error_free(error);
		return FALSE;
	}

	/* 2. and 3. */
	gowl_hotkey_register_func("say", example_say, NULL, NULL);
	gowl_hotkey_register_context("floating", example_floating, NULL, NULL);

	/* 4. a section that uses them */
	if (!gowl_hotkey_add_yaml("example-c-config",
		    "hotkeys:\n"
		    "  - key: \"#!g\"\n"
		    "    do: {call: say, args: [good, morning]}\n"
		    "  # only on a floating window: Super+arrows move it\n"
		    "  - {key: \"#Left\", context: floating, "
		    "do: {window: move-by, arg: \"-50 0\"}}\n"
		    "  - {key: \"#Right\", context: floating, "
		    "do: {window: move-by, arg: \"50 0\"}}\n",
		    &error)) {
		g_warning("hotkey example: %s", error->message);
		g_error_free(error);
		return FALSE;
	}
	return TRUE;
}
