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
#define G_LOG_DOMAIN "gowl-movestack"

#include <glib-object.h>
#include <gmodule.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-keybind-handler.h"
#include "interfaces/gowl-startup-handler.h"

/**
 * GowlModuleMovestack:
 *
 * Movestack module.  Provides key bindings to move the focused
 * client up or down in the tiling stack order.  The compositor
 * dispatches key events to this handler; if the configured
 * move-up or move-down keybind matches, the focused client is
 * swapped with its neighbour in the client list.
 *
 * This module registers as a GowlKeybindHandler so the compositor
 * can dispatch key events to it.  The actual stack reordering
 * requires compositor integration (accessing the client list and
 * calling arrange), which is performed when the compositor invokes
 * the keybind handler.
 */

#define GOWL_TYPE_MODULE_MOVESTACK (gowl_module_movestack_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleMovestack, gowl_module_movestack,
                     GOWL, MODULE_MOVESTACK, GowlModule)

struct _GowlModuleMovestack {
	GowlModule parent_instance;
	gpointer   compositor;
};

static void movestack_keybind_init(GowlKeybindHandlerInterface *iface);
static void movestack_startup_init(GowlStartupHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleMovestack, gowl_module_movestack,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_KEYBIND_HANDLER,
		movestack_keybind_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		movestack_startup_init))

/* --- GowlModule virtual methods --- */

static gboolean
movestack_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static const gchar *
movestack_get_name(GowlModule *mod)
{
	(void)mod;
	return "movestack";
}

static const gchar *
movestack_get_description(GowlModule *mod)
{
	(void)mod;
	return "Move focused client up/down in the tiling stack";
}

static const gchar *
movestack_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/* --- GowlStartupHandler --- */

static void
movestack_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleMovestack *self;

	self = GOWL_MODULE_MOVESTACK(handler);
	self->compositor = compositor;

	g_debug("movestack: startup, compositor reference stored");
}

static void
movestack_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = movestack_on_startup;
}

/* --- GowlKeybindHandler --- */

/**
 * movestack_handle_key:
 *
 * Key event handler.  When the compositor integrates module-based
 * keybind dispatch, this handler will intercept the configured
 * move-up/move-down key combos and reorder the client list.
 *
 * Returns: %TRUE if the key was consumed, %FALSE to pass through
 */
static gboolean
movestack_handle_key(
	GowlKeybindHandler *handler,
	guint               modifiers,
	guint               keysym,
	gboolean            pressed
){
	(void)handler;
	(void)modifiers;
	(void)keysym;
	(void)pressed;

	/*
	 * Movestack algorithm:
	 * 1. Get focused client from compositor
	 * 2. Find its position in self->compositor->clients GList
	 * 3. Based on direction (+1/-1):
	 *    - Swap the GList link with the next/prev visible tiling client
	 * 4. Call gowl_compositor_arrange(self->compositor, client->mon)
	 * 5. Return TRUE to consume the keybind
	 *
	 * Implementation deferred until compositor exposes keybind
	 * dispatch through the module manager to this handler.
	 */

	return FALSE;
}

static void
movestack_keybind_init(GowlKeybindHandlerInterface *iface)
{
	iface->handle_key = movestack_handle_key;
}

/* --- GObject lifecycle --- */

static void
gowl_module_movestack_class_init(GowlModuleMovestackClass *klass)
{
	GowlModuleClass *mod_class;

	mod_class = GOWL_MODULE_CLASS(klass);

	mod_class->activate        = movestack_activate;
	mod_class->get_name        = movestack_get_name;
	mod_class->get_description = movestack_get_description;
	mod_class->get_version     = movestack_get_version;
}

static void
gowl_module_movestack_init(GowlModuleMovestack *self)
{
	self->compositor = NULL;
}

/* --- Shared-object entry point --- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_MOVESTACK;
}
