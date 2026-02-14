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
#define G_LOG_DOMAIN "gowl-scratchpad"

#include <glib-object.h>
#include <gmodule.h>
#include <string.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-scratchpad-handler.h"
#include "interfaces/gowl-keybind-handler.h"
#include "interfaces/gowl-startup-handler.h"

/**
 * GOWL_SCRATCHPAD_MAX_ENTRIES:
 *
 * Maximum number of scratchpad entries (named scratchpads).
 */
#define GOWL_SCRATCHPAD_MAX_ENTRIES (16)

/**
 * ScratchpadEntry:
 *
 * Tracks a named scratchpad.  Each entry has a name used for
 * matching (typically an app_id or window class) and a pointer
 * to the client when it is registered.
 *
 * @name:    identifier for this scratchpad (e.g. "terminal", "mixer")
 * @client:  the client assigned to this scratchpad, or %NULL
 * @visible: whether the scratchpad client is currently shown
 */
typedef struct {
	gchar    *name;
	gpointer  client;
	gboolean  visible;
} ScratchpadEntry;

/**
 * GowlModuleScratchpad:
 *
 * Scratchpad module.  Manages named scratchpad windows that can
 * be toggled on/off.  When toggled on, a scratchpad client is
 * made visible, floating, and focused.  When toggled off, the
 * client is hidden from the current view.
 *
 * The compositor dispatches scratchpad queries through the
 * GowlScratchpadHandler interface and key events through
 * GowlKeybindHandler.
 */

#define GOWL_TYPE_MODULE_SCRATCHPAD (gowl_module_scratchpad_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleScratchpad, gowl_module_scratchpad,
                     GOWL, MODULE_SCRATCHPAD, GowlModule)

struct _GowlModuleScratchpad {
	GowlModule       parent_instance;
	gpointer         compositor;
	ScratchpadEntry  entries[GOWL_SCRATCHPAD_MAX_ENTRIES];
	gint             n_entries;
};

static void scratchpad_handler_init(GowlScratchpadHandlerInterface *iface);
static void scratchpad_keybind_init(GowlKeybindHandlerInterface *iface);
static void scratchpad_startup_init(GowlStartupHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleScratchpad, gowl_module_scratchpad,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCRATCHPAD_HANDLER,
		scratchpad_handler_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_KEYBIND_HANDLER,
		scratchpad_keybind_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		scratchpad_startup_init))

/* --- GowlModule virtual methods --- */

static gboolean
scratchpad_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static const gchar *
scratchpad_get_name(GowlModule *mod)
{
	(void)mod;
	return "scratchpad";
}

static const gchar *
scratchpad_get_description(GowlModule *mod)
{
	(void)mod;
	return "Named scratchpad window management";
}

static const gchar *
scratchpad_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/* --- GowlStartupHandler --- */

static void
scratchpad_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleScratchpad *self;

	self = GOWL_MODULE_SCRATCHPAD(handler);
	self->compositor = compositor;

	g_debug("scratchpad: startup, compositor reference stored");
}

static void
scratchpad_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = scratchpad_on_startup;
}

/* --- GowlScratchpadHandler --- */

/**
 * scratchpad_find_entry:
 * @self: the scratchpad module
 * @name: scratchpad name to search for
 *
 * Finds a scratchpad entry by name.
 *
 * Returns: index into entries array, or -1 if not found
 */
static gint
scratchpad_find_entry(
	GowlModuleScratchpad *self,
	const gchar          *name
){
	gint i;

	for (i = 0; i < self->n_entries; i++) {
		if (self->entries[i].name != NULL &&
		    strcmp(self->entries[i].name, name) == 0)
			return i;
	}

	return -1;
}

/**
 * scratchpad_is_scratchpad:
 * @handler: the scratchpad handler
 * @client: the client to check
 *
 * Checks whether a given client is registered as a scratchpad.
 *
 * Returns: %TRUE if @client is a scratchpad window
 */
static gboolean
scratchpad_is_scratchpad(GowlScratchpadHandler *handler, gpointer client)
{
	GowlModuleScratchpad *self;
	gint i;

	self = GOWL_MODULE_SCRATCHPAD(handler);

	for (i = 0; i < self->n_entries; i++) {
		if (self->entries[i].client == client)
			return TRUE;
	}

	return FALSE;
}

/**
 * scratchpad_toggle_scratchpad:
 * @handler: the scratchpad handler
 * @name: the name of the scratchpad to toggle
 *
 * Toggles visibility of the named scratchpad.  If the scratchpad
 * client is currently visible, it is hidden.  If hidden, it is
 * made visible, set to floating, and focused.
 *
 * If no client is registered for @name, a new entry is created
 * (the compositor will assign a client when one matching the
 * name maps).
 */
static void
scratchpad_toggle_scratchpad(GowlScratchpadHandler *handler, const gchar *name)
{
	GowlModuleScratchpad *self;
	gint                  idx;

	self = GOWL_MODULE_SCRATCHPAD(handler);

	if (name == NULL)
		return;

	idx = scratchpad_find_entry(self, name);

	if (idx < 0) {
		/* Register a new scratchpad entry (no client yet) */
		if (self->n_entries >= GOWL_SCRATCHPAD_MAX_ENTRIES) {
			g_warning("scratchpad: max entries (%d) reached",
			          GOWL_SCRATCHPAD_MAX_ENTRIES);
			return;
		}
		self->entries[self->n_entries].name    = g_strdup(name);
		self->entries[self->n_entries].client  = NULL;
		self->entries[self->n_entries].visible = FALSE;
		self->n_entries++;

		g_debug("scratchpad: registered new entry '%s'", name);
		return;
	}

	/* Toggle visibility of existing entry */
	self->entries[idx].visible = !self->entries[idx].visible;

	/*
	 * When the compositor integrates scratchpad dispatch:
	 * - If visible: set client floating, move to current tag,
	 *   center on screen, focus it
	 * - If hidden: remove client from current tag view
	 *   (client remains alive but not displayed)
	 */
	g_debug("scratchpad: toggled '%s' -> %s",
	        name,
	        self->entries[idx].visible ? "visible" : "hidden");
}

static void
scratchpad_handler_init(GowlScratchpadHandlerInterface *iface)
{
	iface->is_scratchpad     = scratchpad_is_scratchpad;
	iface->toggle_scratchpad = scratchpad_toggle_scratchpad;
}

/* --- GowlKeybindHandler --- */

/**
 * scratchpad_handle_key:
 *
 * Key event handler for scratchpad toggle keybinds.
 *
 * Returns: %TRUE if the key was consumed, %FALSE to pass through
 */
static gboolean
scratchpad_handle_key(
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
	 * Scratchpad keybind dispatch:
	 * 1. Check if the key combo matches a configured scratchpad toggle
	 * 2. If so, call scratchpad_toggle_scratchpad() with the name
	 * 3. Return TRUE to consume the keybind
	 *
	 * Implementation deferred until compositor exposes keybind
	 * dispatch through the module manager.
	 */

	return FALSE;
}

static void
scratchpad_keybind_init(GowlKeybindHandlerInterface *iface)
{
	iface->handle_key = scratchpad_handle_key;
}

/* --- GObject lifecycle --- */

static void
scratchpad_finalize(GObject *obj)
{
	GowlModuleScratchpad *self;
	gint i;

	self = GOWL_MODULE_SCRATCHPAD(obj);

	/* Free all scratchpad entry names */
	for (i = 0; i < self->n_entries; i++)
		g_free(self->entries[i].name);

	G_OBJECT_CLASS(gowl_module_scratchpad_parent_class)->finalize(obj);
}

static void
gowl_module_scratchpad_class_init(GowlModuleScratchpadClass *klass)
{
	GObjectClass    *obj_class;
	GowlModuleClass *mod_class;

	obj_class = G_OBJECT_CLASS(klass);
	mod_class = GOWL_MODULE_CLASS(klass);

	obj_class->finalize = scratchpad_finalize;

	mod_class->activate        = scratchpad_activate;
	mod_class->get_name        = scratchpad_get_name;
	mod_class->get_description = scratchpad_get_description;
	mod_class->get_version     = scratchpad_get_version;
}

static void
gowl_module_scratchpad_init(GowlModuleScratchpad *self)
{
	self->compositor = NULL;
	self->n_entries  = 0;
	memset(self->entries, 0, sizeof(self->entries));
}

/* --- Shared-object entry point --- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_SCRATCHPAD;
}
