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

#ifndef GOWL_FX_OPTOUT_H
#define GOWL_FX_OPTOUT_H

#include <glib.h>
#include <sys/types.h>

G_BEGIN_DECLS

/*
 * Which windows get no eye candy.
 *
 * Three unrelated questions wear one answer here, because the thing a
 * user wants is one thing: "not on that".
 *
 *   THE ENVIRONMENT.  `GOWL_NO_FX=1' (or `true', in any case) in a
 *   process's environment turns the effects off for every window it
 *   maps.  It is read out of /proc/PID/environ rather than out of the
 *   compositor's own environment, which is what makes it INHERITED:
 *   exporting it in a shell, a .desktop file's `Env=', or a systemd
 *   unit covers that process and everything it launches, with no
 *   config change and no restart.
 *
 *   STEAM.  A game is the one window where a shader behind the frame
 *   costs frames the user paid a graphics card for, and Steam's own
 *   client is Chromium in a trench coat.  Steam stamps `SteamAppId' and
 *   friends into every process it starts, so the environment answers
 *   for the games; the process ANCESTRY answers for the client itself
 *   and for anything a game re-execs into.
 *
 *   A NAME LIST.  `steam', `steamwebhelper' and `mutter-devkit' are
 *   built in and cannot be configured away; `no-fx-apps' ADDS to them.
 *   A name is matched against the window's app_id AND against the
 *   command name of the process and each of its ancestors, so naming a
 *   launcher covers the whole session it launches -- which is the
 *   point for `mutter-devkit' and a nested GNOME.
 *
 * No wlroots and no GObject: the decision is testable on its own (see
 * tests/test-fx-optout.c), and only gowl_fx_optout_for_pid() touches
 * /proc.
 */

/**
 * gowl_fx_optout_environ_no_fx:
 * @blob: (nullable) (array length=len) (element-type guint8): a
 *   NUL-separated environment block, as /proc/PID/environ gives it
 * @len: length of @blob in bytes
 *
 * Whether @blob asks for effects to be switched off, i.e. contains
 * `GOWL_NO_FX' set to `1' or to `true' in any case.  Any other value --
 * including `0', the empty string and `false' -- does not.
 *
 * Returns: %TRUE when the environment opts out
 */
gboolean gowl_fx_optout_environ_no_fx (const gchar *blob,
                                       gsize        len);

/**
 * gowl_fx_optout_environ_steam:
 * @blob: (nullable) (array length=len) (element-type guint8): a
 *   NUL-separated environment block
 * @len: length of @blob in bytes
 *
 * Whether @blob belongs to a process Steam started.  Steam exports
 * `SteamAppId', `SteamGameId' and `SteamClientLaunch' into the games it
 * runs, and Proton adds `STEAM_COMPAT_DATA_PATH'; any of them set to a
 * non-empty value is taken as a yes.
 *
 * Returns: %TRUE when the environment is a Steam one
 */
gboolean gowl_fx_optout_environ_steam (const gchar *blob,
                                       gsize        len);

/**
 * gowl_fx_optout_name_listed:
 * @name: (nullable): an app_id or a process command name
 * @list: (nullable): a comma-separated list of names
 *
 * Case-insensitive membership test, ignoring whitespace around each
 * entry and skipping empty ones, so a hand-edited `no-fx-apps' with a
 * trailing comma behaves.  This is @list alone: the built-in names are
 * added by gowl_fx_optout_for_pid(), not here.
 *
 * Returns: %TRUE when @name is one of @list's entries
 */
gboolean gowl_fx_optout_name_listed   (const gchar *name,
                                       const gchar *list);

/**
 * gowl_fx_optout_for_pid:
 * @pid: the process behind a window, or -1 when it is unknown
 * @app_id: (nullable): the window's app_id (X11 class for XWayland)
 * @list: (nullable): comma-separated extra names, from `no-fx-apps'
 *
 * The whole decision: the environment of @pid, the Steam markers in it,
 * @app_id against the names, and the command name of @pid and of each
 * of its ancestors against the names -- where "the names" is @list plus
 * the built-in `steam', `steamwebhelper' and `mutter-devkit'.
 *
 * The ancestry walk stops at the compositor's own process, at pid 1 and
 * after a bounded number of hops, so a cycle or a hostile /proc cannot
 * hang the compositor.  A /proc that cannot be read is not an opt-out:
 * every failure answers %FALSE, which is the state the desktop was in
 * before this existed.
 *
 * Returns: %TRUE when the window should get no effects
 */
gboolean gowl_fx_optout_for_pid       (pid_t        pid,
                                       const gchar *app_id,
                                       const gchar *list);

G_END_DECLS

#endif /* GOWL_FX_OPTOUT_H */
