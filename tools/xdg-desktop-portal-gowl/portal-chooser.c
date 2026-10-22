/*
 * gowl - GObject Wayland Compositor
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
 */

/*
 * portal-chooser.c - one line out of a list, chosen by a human.
 *
 * Inside gowl the list goes on the menu card the session already uses
 * for everything else (Super+space), so a screen-share prompt looks
 * like the rest of the desktop and is driven by the same keys.  The
 * card is asked for with the `menu-choose' text command; its answer
 * comes back later as `EVENT menu chosen TOKEN INDEX' (or `cancelled'),
 * on a connection that subscribed to `menu' events BEFORE asking, so
 * the answer cannot slip past between the two.
 *
 * Outside gowl -- or with the menu module not loaded -- the external
 * dmenu-style programs are what is left, exactly as before.
 */

#include "portal-chooser.h"
#include "portal-gowl.h"

#include <gio/gio.h>
#include <string.h>

/* How long a human gets by default.  A chooser nobody answers is
 * withdrawn, and the request reads as cancelled. */
#define CHOOSER_TIMEOUT_S (300)

/* The line as a chooser can show it: no tabs, no newlines. */
static gchar *
clean_line(const gchar *line)
{
	gchar *out = g_strdup(line != NULL ? line : "");

	g_strdelimit(out, "\t\n\r", ' ');
	return out;
}

static guint
chooser_timeout_ms(void)
{
	const gchar *env = g_getenv("GOWL_PORTAL_CHOOSER_TIMEOUT");
	guint64 s = 0;

	if (env != NULL
	    && g_ascii_string_to_unsigned(env, 10, 1, 86400, &s, NULL))
		return (guint)s * 1000u;
	return CHOOSER_TIMEOUT_S * 1000u;
}

/* ---------------------------------------------------------------
 * gowl's menu card
 * --------------------------------------------------------------- */

static PortalChooserResult
run_gowl_menu(const gchar *prompt, const gchar *const *lines, guint *index)
{
	g_autoptr(PortalGowl) gowl = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) cmd = g_string_new("menu-choose ");
	g_autoptr(JsonNode) sub = NULL;
	g_autofree gchar *reply = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *chosen_prefix = NULL;
	g_autofree gchar *event = NULL;
	g_autofree gchar *want = NULL;
	JsonObject *params;
	JsonArray *names;
	guint i;

	gowl = portal_gowl_connect(&error);
	if (gowl == NULL) {
		g_debug("chooser: no gowl menu: %s", error->message);
		return PORTAL_CHOOSER_UNAVAILABLE;
	}

	/* Subscribed first, so the answer cannot arrive unheard. */
	params = json_object_new();
	names = json_array_new();
	json_array_add_string_element(names, "menu");
	json_object_set_array_member(params, "events", names);
	sub = portal_gowl_call(gowl, "events.subscribe", params, &error);
	if (sub == NULL) {
		g_debug("chooser: cannot subscribe: %s", error->message);
		return PORTAL_CHOOSER_UNAVAILABLE;
	}

	{
		g_autofree gchar *p = clean_line(prompt);

		g_string_append(cmd, p);
	}
	for (i = 0; lines[i] != NULL; i++) {
		g_autofree gchar *l = clean_line(lines[i]);

		g_string_append_c(cmd, '\t');
		g_string_append(cmd, l);
	}
	reply = portal_gowl_command(gowl, cmd->str, &error);
	if (reply == NULL) {
		/* `unknown command' is the menu module not being loaded,
		 * which is a configuration and not a failure. */
		g_debug("chooser: gowl menu declined: %s", error->message);
		return PORTAL_CHOOSER_UNAVAILABLE;
	}
	if (!g_str_has_prefix(reply, "OK ")) {
		g_debug("chooser: gowl menu answered `%s'", reply);
		return PORTAL_CHOOSER_UNAVAILABLE;
	}
	token = g_strdup(reply + 3);
	g_strstrip(token);

	/* Both outcomes carry the token; listen for either. */
	want = g_strdup("EVENT menu ");
	for (;;) {
		g_autofree gchar *cancelled = NULL;

		g_clear_pointer(&event, g_free);
		g_clear_error(&error);
		event = portal_gowl_wait_event_text(gowl, want,
		                                    chooser_timeout_ms(), &error);
		if (event == NULL) {
			g_message("chooser: no answer from the menu (%s); "
			          "withdrawing it", error->message);
			g_free(portal_gowl_command(gowl, "menu-close", NULL));
			return PORTAL_CHOOSER_CANCELLED;
		}
		chosen_prefix = g_strdup_printf("EVENT menu chosen %s ", token);
		cancelled = g_strdup_printf("EVENT menu cancelled %s", token);
		if (g_str_has_prefix(event, chosen_prefix)) {
			guint64 n = 0;

			/* Never trusted to be in range: the index indexes
			 * OUR array, and the socket is not us. */
			if (!g_ascii_string_to_unsigned(
				    g_strchomp(event + strlen(chosen_prefix)), 10, 0,
				    G_MAXUINT, &n, NULL)
			    || n >= g_strv_length((gchar **)lines))
				return PORTAL_CHOOSER_CANCELLED;
			*index = (guint)n;
			return PORTAL_CHOOSER_CHOSEN;
		}
		g_clear_pointer(&chosen_prefix, g_free);
		if (g_strcmp0(g_strchomp(event), cancelled) == 0)
			return PORTAL_CHOOSER_CANCELLED;
		/* Somebody else's choice: keep waiting for ours. */
	}
}

/* ---------------------------------------------------------------
 * dmenu-style programs
 * --------------------------------------------------------------- */

/* The menu program to pipe the list through, or NULL. */
static gchar *
dmenu_command(const gchar *prompt)
{
	const gchar *candidates[] = {
		"bemenu", "fuzzel", "wofi", "rofi", "dmenu", NULL
	};
	g_autofree gchar *quoted = g_shell_quote(prompt);
	g_autofree gchar *quoted_sp = NULL;
	gsize i;

	{
		g_autofree gchar *with = g_strdup_printf("%s: ", prompt);

		quoted_sp = g_shell_quote(with);
	}
	for (i = 0; candidates[i] != NULL; i++) {
		g_autofree gchar *path = g_find_program_in_path(candidates[i]);

		if (path == NULL)
			continue;
		/* Each of these reads lines on stdin and prints the chosen one,
		 * given the flag that makes it a picker rather than a runner. */
		if (g_strcmp0(candidates[i], "bemenu") == 0)
			return g_strdup_printf("%s -p %s -l 15", path, quoted);
		if (g_strcmp0(candidates[i], "fuzzel") == 0)
			return g_strdup_printf("%s --dmenu --prompt %s", path,
			                       quoted_sp);
		if (g_strcmp0(candidates[i], "wofi") == 0)
			return g_strdup_printf("%s --dmenu --prompt %s", path,
			                       quoted);
		if (g_strcmp0(candidates[i], "rofi") == 0)
			return g_strdup_printf("%s -dmenu -p %s", path, quoted);
		return g_strdup_printf("%s -p %s", path, quoted);
	}
	return NULL;
}

static PortalChooserResult
run_command(const gchar *cmd, const gchar *const *lines, guint *index)
{
	g_auto(GStrv) argv = NULL;
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) menu = g_string_new(NULL);
	g_autofree gchar *out = NULL;
	guint i;

	if (!g_shell_parse_argv(cmd, NULL, &argv, &error)) {
		g_warning("chooser: '%s' is unparseable: %s", cmd, error->message);
		return PORTAL_CHOOSER_UNAVAILABLE;
	}
	for (i = 0; lines[i] != NULL; i++) {
		g_autofree gchar *l = clean_line(lines[i]);

		g_string_append_printf(menu, "%s\n", l);
	}
	proc = g_subprocess_newv((const gchar * const *)argv,
		G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE,
		&error);
	if (proc == NULL) {
		g_warning("chooser: could not run '%s': %s", cmd, error->message);
		return PORTAL_CHOOSER_UNAVAILABLE;
	}
	if (!g_subprocess_communicate_utf8(proc, menu->str, NULL, &out, NULL,
	                                   &error)) {
		g_warning("chooser: '%s' failed: %s", cmd, error->message);
		return PORTAL_CHOOSER_UNAVAILABLE;
	}
	/* An empty answer from a chooser that ran is the human pressing
	 * Escape, which is a cancellation and not an error. */
	if (out == NULL || *out == '\0')
		return PORTAL_CHOOSER_CANCELLED;
	g_strchomp(out);
	for (i = 0; lines[i] != NULL; i++) {
		g_autofree gchar *l = clean_line(lines[i]);

		if (g_strcmp0(l, out) == 0) {
			*index = i;
			return PORTAL_CHOOSER_CHOSEN;
		}
	}
	/* Typed text that is on no line: rofi and fuzzel allow it.  Not a
	 * choice, and not something to guess at. */
	g_message("chooser: '%s' is not one of the choices", out);
	return PORTAL_CHOOSER_CANCELLED;
}

PortalChooserResult
portal_chooser_run(const gchar *prompt, const gchar *const *lines,
                   guint *index)
{
	const gchar *env = g_getenv("GOWL_PORTAL_CHOOSER");
	g_autofree gchar *cmd = NULL;
	PortalChooserResult result;

	g_return_val_if_fail(lines != NULL, PORTAL_CHOOSER_UNAVAILABLE);
	g_return_val_if_fail(index != NULL, PORTAL_CHOOSER_UNAVAILABLE);

	if (lines[0] == NULL)
		return PORTAL_CHOOSER_UNAVAILABLE;
	if (prompt == NULL)
		prompt = "Choose";

	/* `gowl' names the menu card and nothing else; any other value is
	 * a command, and an explicit one is never second-guessed. */
	if (env != NULL && *env != '\0' && g_strcmp0(env, "gowl") != 0)
		return run_command(env, lines, index);

	result = run_gowl_menu(prompt, lines, index);
	if (result != PORTAL_CHOOSER_UNAVAILABLE
	    || g_strcmp0(env, "gowl") == 0)
		return result;

	cmd = dmenu_command(prompt);
	if (cmd == NULL)
		return PORTAL_CHOOSER_UNAVAILABLE;
	return run_command(cmd, lines, index);
}
