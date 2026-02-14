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

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-autostart"

#include <glib-object.h>
#include <gmodule.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"

/**
 * GowlModuleAutostart:
 *
 * Autostart module.  On compositor startup, spawns a configurable
 * list of shell commands asynchronously.  Commands that fail to
 * spawn are logged as warnings but do not prevent other commands
 * from running.
 *
 * Configuration (via configure()): expects a NULL-terminated
 * array of command strings (gchar **).
 */

#define GOWL_TYPE_MODULE_AUTOSTART (gowl_module_autostart_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleAutostart, gowl_module_autostart,
                     GOWL, MODULE_AUTOSTART, GowlModule)

struct _GowlModuleAutostart {
	GowlModule  parent_instance;
	gchar     **commands;
	gint        n_commands;
};

static void autostart_startup_init(GowlStartupHandlerInterface *iface);
static void autostart_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleAutostart, gowl_module_autostart,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		autostart_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER,
		autostart_shutdown_init))

/* --- GowlModule virtual methods --- */

static gboolean
autostart_activate(GowlModule *mod)
{
	(void)mod;
	g_debug("autostart: activated");
	return TRUE;
}

static void
autostart_deactivate(GowlModule *mod)
{
	(void)mod;
	g_debug("autostart: deactivated");
}

static const gchar *
autostart_get_name(GowlModule *mod)
{
	(void)mod;
	return "autostart";
}

static const gchar *
autostart_get_description(GowlModule *mod)
{
	(void)mod;
	return "Spawn commands on compositor startup";
}

static const gchar *
autostart_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/**
 * autostart_configure:
 *
 * Accepts a NULL-terminated string array (gchar **) of shell
 * commands to run at startup.
 */
static void
autostart_configure(GowlModule *mod, gpointer config)
{
	GowlModuleAutostart *self;
	gchar **cmds;
	gint i;

	self = GOWL_MODULE_AUTOSTART(mod);

	g_strfreev(self->commands);
	self->commands = NULL;
	self->n_commands = 0;

	if (config == NULL)
		return;

	cmds = (gchar **)config;
	for (i = 0; cmds[i] != NULL; i++)
		;
	self->n_commands = i;
	self->commands = g_strdupv(cmds);

	g_debug("autostart: configured with %d command(s)", self->n_commands);
}

/* --- GowlStartupHandler --- */

/**
 * autostart_on_startup:
 *
 * Spawns each configured command asynchronously using the shell.
 */
static void
autostart_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleAutostart *self;
	gint i;

	self = GOWL_MODULE_AUTOSTART(handler);
	(void)compositor;

	if (self->commands == NULL || self->n_commands == 0) {
		g_debug("autostart: no commands configured");
		return;
	}

	for (i = 0; i < self->n_commands; i++) {
		GError *error;

		error = NULL;
		g_debug("autostart: spawning '%s'", self->commands[i]);

		if (!g_spawn_command_line_async(self->commands[i], &error)) {
			g_warning("autostart: failed to spawn '%s': %s",
			          self->commands[i], error->message);
			g_error_free(error);
		}
	}
}

static void
autostart_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = autostart_on_startup;
}

/* --- GowlShutdownHandler --- */

static void
autostart_on_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	(void)handler;
	(void)compositor;
	g_debug("autostart: shutdown");
}

static void
autostart_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = autostart_on_shutdown;
}

/* --- GObject lifecycle --- */

static void
gowl_module_autostart_finalize(GObject *object)
{
	GowlModuleAutostart *self;

	self = GOWL_MODULE_AUTOSTART(object);
	g_strfreev(self->commands);

	G_OBJECT_CLASS(gowl_module_autostart_parent_class)->finalize(object);
}

static void
gowl_module_autostart_class_init(GowlModuleAutostartClass *klass)
{
	GObjectClass *object_class;
	GowlModuleClass *mod_class;

	object_class = G_OBJECT_CLASS(klass);
	mod_class = GOWL_MODULE_CLASS(klass);

	object_class->finalize = gowl_module_autostart_finalize;

	mod_class->activate        = autostart_activate;
	mod_class->deactivate      = autostart_deactivate;
	mod_class->get_name        = autostart_get_name;
	mod_class->get_description = autostart_get_description;
	mod_class->get_version     = autostart_get_version;
	mod_class->configure       = autostart_configure;
}

static void
gowl_module_autostart_init(GowlModuleAutostart *self)
{
	self->commands = NULL;
	self->n_commands = 0;
}

/* --- Shared-object entry point --- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_AUTOSTART;
}
