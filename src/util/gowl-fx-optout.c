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

#include "gowl-fx-optout.h"

#include <string.h>
#include <unistd.h>

/*
 * How far up the process tree to look.
 *
 * A Steam game sits three or four hops under the client (game ->
 * reaper -> steam.sh -> steam), and a Flatpak adds a couple more.
 * Sixteen is well past anything real and is the guard against a /proc
 * that answers nonsense: the loop also stops at pid 1 and at the
 * compositor's own pid, but a corrupted `PPid:' could otherwise walk
 * for as long as it kept naming new processes.
 */
#define GOWL_FX_OPTOUT_MAX_HOPS (16)

/*
 * Environment variables Steam stamps into what it launches.
 *
 * SteamAppId and SteamGameId are on every game; SteamClientLaunch is on
 * things started from the client that are not games (the browser, the
 * friends window); STEAM_COMPAT_DATA_PATH is Proton's prefix, which
 * catches a Windows game whose own process has lost the first two.
 */
/*
 * Names that never get effects, whatever the config says.
 *
 * `no-fx-apps' ADDS to this rather than replacing it, so a user who
 * writes their own list cannot accidentally put the bubbles back on a
 * game.  Each is matched against the window's app_id and against the
 * command name of the process and its ancestors:
 *
 *   steam           the launcher, and everything under it
 *   steamwebhelper  the Chromium that draws the client's own windows
 *   mutter-devkit   a nested GNOME session, which has a desktop of its
 *                   own behind its windows and wants nothing behind
 *                   the frame it draws
 */
static const gchar *const builtin_names[] = {
	"steam",
	"steamwebhelper",
	"mutter-devkit",
	NULL
};

static const gchar *const steam_vars[] = {
	"SteamAppId",
	"SteamGameId",
	"SteamClientLaunch",
	"STEAM_COMPAT_DATA_PATH",
	NULL
};

/*
 * One KEY=VALUE entry of an environment block, split.
 *
 * Returns the value for @key, or NULL when @entry names something else.
 * The comparison on the key is case-SENSITIVE, because environment
 * variable names are.
 */
static const gchar *
entry_value_for(
	const gchar *entry,
	const gchar *key
){
	gsize n;

	n = strlen(key);
	if (strncmp(entry, key, n) != 0)
		return NULL;
	if (entry[n] != '=')
		return NULL;
	return entry + n + 1;
}

/*
 * Walk a NUL-separated environment block.
 *
 * /proc/PID/environ is not guaranteed to be NUL-terminated at the end,
 * and a process that rewrote its own environ (which is what setproctitle
 * does) can leave garbage in it, so the walk is bounded by @len and each
 * entry's length is measured inside what is left rather than with a
 * plain strlen() that could run off the buffer.
 */
typedef gboolean (*GowlFxEnvironFunc)(const gchar *entry, gpointer user_data);

static gboolean
environ_foreach(
	const gchar       *blob,
	gsize              len,
	GowlFxEnvironFunc  func,
	gpointer           user_data
){
	gsize i;

	if (blob == NULL || len == 0)
		return FALSE;

	i = 0;
	while (i < len) {
		const gchar *entry = blob + i;
		gsize        n     = strnlen(entry, len - i);

		if (n > 0 && func(entry, user_data))
			return TRUE;
		if (n == len - i)
			break;   /* unterminated tail: nothing more to read */
		i += n + 1;
	}
	return FALSE;
}

static gboolean
match_no_fx(
	const gchar *entry,
	gpointer     user_data
){
	const gchar *value;

	(void)user_data;

	value = entry_value_for(entry, "GOWL_NO_FX");
	if (value == NULL)
		return FALSE;

	/*
	 * `1' and `true' only.  An empty GOWL_NO_FX= is what unsetting it
	 * in a wrapper script looks like, and `0'/`false' has to mean what
	 * it says or the variable could never be turned back off for one
	 * child of a process that has it on.
	 */
	return g_str_equal(value, "1")
	       || g_ascii_strcasecmp(value, "true") == 0;
}

static gboolean
match_steam(
	const gchar *entry,
	gpointer     user_data
){
	guint i;

	(void)user_data;

	for (i = 0; steam_vars[i] != NULL; i++) {
		const gchar *value = entry_value_for(entry, steam_vars[i]);

		if (value != NULL && *value != '\0')
			return TRUE;
	}
	return FALSE;
}

/**
 * gowl_fx_optout_environ_no_fx:
 * @blob: (nullable): a NUL-separated environment block
 * @len: its length in bytes
 *
 * Returns: %TRUE when GOWL_NO_FX is set to 1 or true
 */
gboolean
gowl_fx_optout_environ_no_fx(
	const gchar *blob,
	gsize        len
){
	return environ_foreach(blob, len, match_no_fx, NULL);
}

/**
 * gowl_fx_optout_environ_steam:
 * @blob: (nullable): a NUL-separated environment block
 * @len: its length in bytes
 *
 * Returns: %TRUE when the environment is one Steam handed out
 */
gboolean
gowl_fx_optout_environ_steam(
	const gchar *blob,
	gsize        len
){
	return environ_foreach(blob, len, match_steam, NULL);
}

/*
 * @name against the built-in list and the configured one.
 *
 * Everything that asks "is this one of the names" asks this, so the
 * built-ins apply wherever the config's list does and in the same way.
 */
static gboolean
name_matches(
	const gchar *name,
	const gchar *list
){
	guint i;

	if (name == NULL || *name == '\0')
		return FALSE;

	for (i = 0; builtin_names[i] != NULL; i++) {
		if (g_ascii_strcasecmp(builtin_names[i], name) == 0)
			return TRUE;
	}
	return gowl_fx_optout_name_listed(name, list);
}

/**
 * gowl_fx_optout_name_listed:
 * @name: (nullable): an app_id or a command name
 * @list: (nullable): a comma-separated list
 *
 * Returns: %TRUE when @name is in @list
 */
gboolean
gowl_fx_optout_name_listed(
	const gchar *name,
	const gchar *list
){
	g_auto(GStrv) parts = NULL;
	guint i;

	if (name == NULL || *name == '\0' || list == NULL || *list == '\0')
		return FALSE;

	parts = g_strsplit(list, ",", -1);
	for (i = 0; parts[i] != NULL; i++) {
		const gchar *entry = g_strstrip(parts[i]);

		if (*entry == '\0')
			continue;
		if (g_ascii_strcasecmp(entry, name) == 0)
			return TRUE;
	}
	return FALSE;
}

/*
 * The command name of a process, two ways.
 *
 * /proc/PID/comm is the kernel's, and it is TRUNCATED TO 15 CHARACTERS
 * -- which is a real limit here, not a theoretical one: `xdg-desktop-portal'
 * and the like do not fit.  argv[0]'s basename is not truncated but can
 * be a full path, an interpreter, or absent altogether, so both are
 * tried and either may match.
 *
 * Both are read straight rather than through GowlProcessInfo, which
 * joins cmdline's NUL-separated arguments with spaces and so loses
 * where argv[0] ends.
 */
static gboolean
pid_name_listed(
	pid_t        pid,
	const gchar *list
){
	g_autofree gchar *comm_path = NULL;
	g_autofree gchar *comm = NULL;
	g_autofree gchar *cmd_path = NULL;
	g_autofree gchar *cmdline = NULL;
	gsize len = 0;

	comm_path = g_strdup_printf("/proc/%d/comm", (gint)pid);
	if (g_file_get_contents(comm_path, &comm, NULL, NULL)) {
		g_strstrip(comm);
		if (name_matches(comm, list))
			return TRUE;
	}

	cmd_path = g_strdup_printf("/proc/%d/cmdline", (gint)pid);
	if (g_file_get_contents(cmd_path, &cmdline, &len, NULL) && len > 0) {
		/* argv[0] is the first NUL-terminated string in there. */
		g_autofree gchar *base = g_path_get_basename(cmdline);

		if (name_matches(base, list))
			return TRUE;
	}

	return FALSE;
}

/*
 * The parent of a process, from /proc/PID/status.
 *
 * Deliberately NOT /proc/PID/stat: its second field is the command name
 * in parentheses and a process is free to put ')' and spaces in it, so
 * every naive field-counting parse of that file is wrong for a program
 * that wants it to be.  `PPid:' in status has no such hole.
 *
 * Returns 0 when the parent cannot be determined.
 */
static pid_t
pid_parent(pid_t pid)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *status = NULL;
	const gchar *p;

	path = g_strdup_printf("/proc/%d/status", (gint)pid);
	if (!g_file_get_contents(path, &status, NULL, NULL))
		return 0;

	p = status;
	while (p != NULL && *p != '\0') {
		if (g_str_has_prefix(p, "PPid:")) {
			return (pid_t)g_ascii_strtoll(p + 5, NULL, 10);
		}
		p = strchr(p, '\n');
		if (p != NULL)
			p++;
	}
	return 0;
}

/**
 * gowl_fx_optout_for_pid:
 * @pid: the process behind a window
 * @app_id: (nullable): the window's app_id
 * @list: (nullable): comma-separated extra names
 *
 * Returns: %TRUE when the window should get no effects
 */
gboolean
gowl_fx_optout_for_pid(
	pid_t        pid,
	const gchar *app_id,
	const gchar *list
){
	pid_t self_pid;
	pid_t cur;
	guint hop;

	/* The app_id needs no /proc at all, so it answers even for a
	 * client whose credentials we never got. */
	if (name_matches(app_id, list))
		return TRUE;

	if (pid <= 0)
		return FALSE;

	{
		g_autofree gchar *env_path = NULL;
		g_autofree gchar *blob = NULL;
		gsize len = 0;

		env_path = g_strdup_printf("/proc/%d/environ", (gint)pid);
		if (g_file_get_contents(env_path, &blob, &len, NULL)) {
			if (gowl_fx_optout_environ_no_fx(blob, len))
				return TRUE;
			if (gowl_fx_optout_environ_steam(blob, len))
				return TRUE;
		}
	}

	/*
	 * Up the tree.  Stopping at our own pid matters: gowl is embedded
	 * in cmacs and cmacs is started from a shell, so without the stop
	 * a name that happened to match anywhere above the compositor
	 * would switch the effects off for every window on the desktop.
	 */
	self_pid = getpid();
	cur = pid;
	for (hop = 0; hop < GOWL_FX_OPTOUT_MAX_HOPS; hop++) {
		if (cur <= 1 || cur == self_pid)
			break;
		if (pid_name_listed(cur, list))
			return TRUE;
		cur = pid_parent(cur);
		if (cur <= 0)
			break;
	}

	return FALSE;
}
