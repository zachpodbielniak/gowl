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

#ifndef BAR_INTERNAL_H
#define BAR_INTERNAL_H

#include <glib.h>

#include "barkit/gowl-bar-plugin.h"
#include "barkit/gowl-bar-plugin-proxy.h"
#include "barkit/gowl-bar-registry.h"

#include "bar-sysinfo.h"

G_BEGIN_DECLS

/**
 * BarEnv:
 * @sysinfo: the shared `/proc' reader
 * @compositor: (nullable): the #GowlCompositor, as an untyped pointer so
 *   plugin files need not pull in the compositor headers
 * @registry: the plugin registry
 *
 * What the shipped plugins need from the module that hosts them.
 *
 * A module-level singleton rather than a per-plugin closure: there is
 * exactly one bar module per compositor, the shipped plugins are
 * registered as vtables (which carry no user data of their own), and
 * threading an environment pointer through every callback would add a
 * parameter to fifty functions to express something that cannot vary.
 */
typedef struct {
	BarSysinfo      *sysinfo;
	gpointer         compositor;
	GowlBarRegistry *registry;
} BarEnv;

/**
 * bar_env:
 *
 * Returns: (transfer none) (nullable): the shared environment, or
 *   %NULL before the module has started
 */
const BarEnv *bar_env (void);

/**
 * bar_env_set:
 * @env: (nullable): the environment, or %NULL to clear it
 *
 * Called by the module on startup and shutdown.
 */
void bar_env_set (const BarEnv *env);

/* --- Registration entry points ------------------------------------ */

void bar_register_core_plugins    (GowlBarRegistry *registry);
void bar_register_system_plugins  (GowlBarRegistry *registry);
void bar_register_net_plugins     (GowlBarRegistry *registry);
void bar_register_desktop_plugins (GowlBarRegistry *registry);

/* --- Subprocess helpers ------------------------------------------- */

/**
 * bar_run_argv:
 * @argv: (array zero-terminated=1): the command and its arguments
 *
 * Runs a command and collects its standard output.  Blocking, so this
 * may only be called from a plugin's async poll or from
 * gowl_bar_plugin_queue_work() --- never from a poll, a draw or a
 * panel action, all of which run on the compositor's dispatch thread.
 *
 * Returns: (transfer full) (nullable): the output, or %NULL when the
 *   command could not be run or exited non-zero
 */
gchar *bar_run_argv (const gchar * const *argv);

/**
 * bar_run_argv_line:
 * @argv: (array zero-terminated=1): the command and its arguments
 *
 * As bar_run_argv(), but returns only the first line, trimmed.
 *
 * Returns: (transfer full) (nullable): the first line of output
 */
gchar *bar_run_argv_line (const gchar * const *argv);

/**
 * bar_run_shell_line:
 * @cmdline: a command line, parsed with shell quoting rules
 *
 * Returns: (transfer full) (nullable): the first line of output
 */
gchar *bar_run_shell_line (const gchar *cmdline);

/**
 * bar_spawn_shell:
 * @cmdline: a command line
 *
 * Launches @cmdline detached, without waiting.  Safe from any thread.
 */
void bar_spawn_shell (const gchar *cmdline);

/**
 * bar_have_command:
 * @name: an executable name
 *
 * Returns: %TRUE when @name is on `PATH'
 */
gboolean bar_have_command (const gchar *name);

/**
 * bar_app_command:
 * @binary: the native command name, e.g. "pavucontrol"
 * @flatpak_id: (nullable): its flatpak application id
 * @args: (nullable): arguments appended to whichever form is used
 *
 * Builds a command line that runs @binary if it is installed natively
 * and falls back to its flatpak if it is not.
 *
 * The choice is made by the SHELL, at launch, rather than probed here
 * on purpose.  A panel button runs on the compositor's dispatch thread
 * while it holds cmacs_gowl_mutex, so asking `flatpak info' which form
 * is present would block the editor for the length of a subprocess --
 * the same mistake that stopped windows mapping.  gowl_bar_plugin_spawn
 * is already detached, so the test costs nothing and stays correct if
 * the user installs or removes either form later.
 *
 * A user installation is preferred over a system one, matching how
 * these desktop helpers are normally installed.
 *
 * Returns: (transfer full): a command line for bar_spawn_shell()
 */
gchar *bar_app_command (const gchar *binary,
                        const gchar *flatpak_id,
                        const gchar *args);

/**
 * bar_expand_tilde:
 * @path: a path that may start with `~/'
 *
 * Returns: (transfer full): @path with any leading `~' expanded
 */
gchar *bar_expand_tilde (const gchar *path);

G_END_DECLS

#endif /* BAR_INTERNAL_H */
