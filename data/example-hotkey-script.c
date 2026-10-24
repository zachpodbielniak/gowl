/*
 * gowl - Example hotkey script (the .ahk of gowl)
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
 * A crispy hotkey script: plain C the hotkey module compiles (and
 * caches) with crispy, opens, and calls gowl_hotkey_script_init() in.
 * Name it in the configuration:
 *
 *   modules:
 *     hotkey:
 *       enabled: true
 *       scripts: [~/.config/gowl/hotkeys.c]
 *
 * Save the file and the module rebuilds it and runs init again --
 * everything the old build registered is taken back first, so there is
 * no restart and nothing doubles up.  Every call into the script runs
 * under the fault guard: a crash costs that one call, not the session.
 *
 * Flags for the compiler go in a CRISPY_PARAMS line:
 *   #define CRISPY_PARAMS "$(pkg-config --cflags --libs libnotify)"
 */

#include <gowl/gowl.h>

/* How many times Super+Alt+c has been pressed since the script loaded */
static guint presses;

/* Counts, and says so in a toast. */
static void
script_count(
	GowlHotkeyCall *call,
	gpointer        data
){
	g_autofree gchar *line = NULL;

	(void)data;
	presses++;
	line = g_strdup_printf("bar-notify pressed %u time%s|", presses,
	                       presses == 1 ? "" : "s");
	g_free(gowl_hotkey_command(gowl_hotkey_call_get_compositor(call), line));
}

/* A context: true between nine and five on a weekday. */
static gboolean
script_working_hours(
	GowlHotkeyCall *call,
	gpointer        data
){
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	gint hour = g_date_time_get_hour(now);
	gint day = g_date_time_get_day_of_week(now);

	(void)call;
	(void)data;
	return day <= 5 && hour >= 9 && hour < 17;
}

/* Hands the current window to the scratchpad and back: a function a
 * YAML hotkey (or `hotkey-call stash') can call. */
static void
script_stash(
	GowlHotkeyCall *call,
	gpointer        data
){
	(void)data;
	g_free(gowl_hotkey_command(gowl_hotkey_call_get_compositor(call),
	                           "scratchpad-toggle"));
}

G_MODULE_EXPORT gboolean gowl_hotkey_script_init(void);
G_MODULE_EXPORT void     gowl_hotkey_script_fini(void);

/*
 * gowl_hotkey_script_init:
 *
 * Registers the script's hotkeys, functions and contexts.  Called on the
 * compositor thread, under the fault guard, each time the script is
 * (re)built.
 *
 * Returns: %TRUE when it set itself up
 */
G_MODULE_EXPORT gboolean
gowl_hotkey_script_init(void)
{
	GError *error = NULL;

	presses = 0;
	if (!gowl_hotkey_define("script-count", "#!c", script_count, NULL, NULL,
	                        &error)) {
		g_warning("hotkey script: %s", error->message);
		g_error_free(error);
		return FALSE;
	}
	gowl_hotkey_register_func("stash", script_stash, NULL, NULL);
	gowl_hotkey_register_context("working-hours", script_working_hours,
	                             NULL, NULL);
	/* YAML that uses them: Super+Escape stashes the window, and during
	 * working hours F12 opens the work chat */
	return gowl_hotkey_add_yaml("example-script",
		"hotkeys:\n"
		"  - {key: \"#Esc\", do: {call: stash}}\n"
		"  - {key: F12, context: working-hours, "
		"do: {run: [element-desktop]}}\n", NULL);
}

/*
 * gowl_hotkey_script_fini:
 *
 * Called before a rebuild replaces the script and when it is dropped.
 * The module takes back what init registered by itself; this is for the
 * script's own state.
 */
G_MODULE_EXPORT void
gowl_hotkey_script_fini(void)
{
	presses = 0;
}
