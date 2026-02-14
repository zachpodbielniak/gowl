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
#define G_LOG_DOMAIN "gowl-swallow"

#include <glib-object.h>
#include <gmodule.h>
#include <string.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-swallow-handler.h"
#include "interfaces/gowl-startup-handler.h"

/**
 * GOWL_SWALLOW_MAX_TERMINALS:
 *
 * Maximum number of terminal app_ids that can be configured
 * as swallow-capable.
 */
#define GOWL_SWALLOW_MAX_TERMINALS (16)

/**
 * GOWL_SWALLOW_MAX_PAIRS:
 *
 * Maximum number of simultaneous swallow pairs (parent/child)
 * tracked at once.
 */
#define GOWL_SWALLOW_MAX_PAIRS (64)

/**
 * SwallowPair:
 *
 * Tracks a single swallow relationship.  When a child window
 * spawned from a terminal maps, the terminal (parent) is hidden
 * and the child takes its place.  On child unmap, the parent
 * is restored.
 *
 * @parent: the terminal client that was swallowed
 * @child:  the spawned client that replaced it
 */
typedef struct {
	gpointer parent;
	gpointer child;
} SwallowPair;

/**
 * GowlModuleSwallow:
 *
 * Window swallowing module.  When a GUI application is launched
 * from a terminal, the terminal window is hidden and the new
 * application takes its place in the tiling layout.  When the
 * application closes, the terminal is restored.
 *
 * Swallow detection uses PID ancestry: if the new client's PID
 * is a descendant of a terminal's PID, the terminal is swallowed.
 * On Linux this is checked via /proc/PID/stat.
 */

#define GOWL_TYPE_MODULE_SWALLOW (gowl_module_swallow_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleSwallow, gowl_module_swallow,
                     GOWL, MODULE_SWALLOW, GowlModule)

struct _GowlModuleSwallow {
	GowlModule   parent_instance;
	gpointer     compositor;
	gchar       *terminal_ids[GOWL_SWALLOW_MAX_TERMINALS];
	gint         n_terminals;
	SwallowPair  pairs[GOWL_SWALLOW_MAX_PAIRS];
	gint         n_pairs;
};

static void swallow_handler_init(GowlSwallowHandlerInterface *iface);
static void swallow_startup_init(GowlStartupHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleSwallow, gowl_module_swallow,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SWALLOW_HANDLER,
		swallow_handler_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		swallow_startup_init))

/* --- GowlModule virtual methods --- */

static gboolean
swallow_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static const gchar *
swallow_get_name(GowlModule *mod)
{
	(void)mod;
	return "swallow";
}

static const gchar *
swallow_get_description(GowlModule *mod)
{
	(void)mod;
	return "Window swallowing for terminal-spawned applications";
}

static const gchar *
swallow_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/**
 * swallow_configure:
 * @mod: the module
 * @config: a %NULL-terminated array of terminal app_id strings
 *
 * Configures the list of terminal app_ids that are eligible
 * for swallowing.  Common values: "st-256color", "kitty",
 * "foot", "Alacritty".
 */
static void
swallow_configure(GowlModule *mod, gpointer config)
{
	GowlModuleSwallow *self;
	gchar **ids;
	gint i;

	self = GOWL_MODULE_SWALLOW(mod);

	/* Free previous terminal IDs */
	for (i = 0; i < self->n_terminals; i++)
		g_free(self->terminal_ids[i]);
	self->n_terminals = 0;

	if (config == NULL)
		return;

	/* config is expected to be a NULL-terminated gchar** array */
	ids = (gchar **)config;
	for (i = 0; ids[i] != NULL && self->n_terminals < GOWL_SWALLOW_MAX_TERMINALS; i++) {
		self->terminal_ids[self->n_terminals] = g_strdup(ids[i]);
		self->n_terminals++;
	}

	g_debug("swallow: configured %d terminal app_ids", self->n_terminals);
}

/* --- GowlStartupHandler --- */

static void
swallow_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleSwallow *self;

	self = GOWL_MODULE_SWALLOW(handler);
	self->compositor = compositor;

	/* Default terminal app_ids if none configured */
	if (self->n_terminals == 0) {
		self->terminal_ids[0] = g_strdup("st-256color");
		self->terminal_ids[1] = g_strdup("kitty");
		self->terminal_ids[2] = g_strdup("foot");
		self->n_terminals = 3;
	}

	g_debug("swallow: startup, tracking %d terminal app_ids",
	        self->n_terminals);
}

static void
swallow_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = swallow_on_startup;
}

/* --- GowlSwallowHandler --- */

/**
 * swallow_should_swallow:
 * @handler: the swallow handler
 * @parent: candidate parent client (potential terminal)
 * @child: newly mapped client
 *
 * Determines whether @parent should be swallowed by @child.
 * Checks two conditions:
 *   1. @parent's app_id matches a configured terminal app_id
 *   2. @child's PID is a descendant of @parent's PID
 *
 * PID ancestry is checked by walking /proc/PID/stat ppid fields
 * upward until we find the parent's PID or reach init (PID 1).
 *
 * Returns: %TRUE if @parent should be swallowed
 */
static gboolean
swallow_should_swallow(
	GowlSwallowHandler *handler,
	gpointer             parent,
	gpointer             child
){
	(void)handler;
	(void)parent;
	(void)child;

	/*
	 * Swallow detection algorithm:
	 *
	 * 1. Get parent app_id from compositor client structure
	 * 2. Check if app_id is in terminal_ids list
	 * 3. If not a terminal, return FALSE
	 * 4. Get child PID and parent PID from the client structures
	 * 5. Walk /proc/<child_pid>/stat:
	 *    - Read field 4 (ppid)
	 *    - If ppid == parent_pid, return TRUE
	 *    - If ppid <= 1, return FALSE (reached init)
	 *    - Set child_pid = ppid and repeat
	 *
	 * Implementation deferred until compositor exposes client
	 * PID and app_id through the module interface.
	 */

	return FALSE;
}

/**
 * swallow_swallow:
 * @handler: the swallow handler
 * @parent: the terminal client to hide
 * @child: the child client taking the terminal's place
 *
 * Performs the swallow operation.  Hides @parent and positions
 * @child in @parent's tiling slot.  Records the pair so that
 * unswallow can restore the terminal later.
 */
static void
swallow_swallow(
	GowlSwallowHandler *handler,
	gpointer             parent,
	gpointer             child
){
	GowlModuleSwallow *self;

	self = GOWL_MODULE_SWALLOW(handler);

	if (self->n_pairs >= GOWL_SWALLOW_MAX_PAIRS) {
		g_warning("swallow: max pairs (%d) reached", GOWL_SWALLOW_MAX_PAIRS);
		return;
	}

	/* Record the swallow pair */
	self->pairs[self->n_pairs].parent = parent;
	self->pairs[self->n_pairs].child  = child;
	self->n_pairs++;

	/*
	 * When compositor integration is complete:
	 * 1. Save parent's geometry and tiling position
	 * 2. Hide parent (set hidden flag, remove from scene)
	 * 3. Place child in parent's position in the client list
	 * 4. Call arrange() to re-tile
	 */

	g_debug("swallow: swallowed parent, %d active pairs", self->n_pairs);
}

/**
 * swallow_unswallow:
 * @handler: the swallow handler
 * @parent: the terminal client to restore
 *
 * Restores a swallowed terminal.  Called when the child client
 * that triggered the swallow unmaps (closes).  The terminal is
 * made visible again in its original tiling position.
 */
static void
swallow_unswallow(
	GowlSwallowHandler *handler,
	gpointer             parent
){
	GowlModuleSwallow *self;
	gint i;

	self = GOWL_MODULE_SWALLOW(handler);

	/* Find and remove the swallow pair */
	for (i = 0; i < self->n_pairs; i++) {
		if (self->pairs[i].parent == parent) {
			/*
			 * When compositor integration is complete:
			 * 1. Show parent (clear hidden flag, add to scene)
			 * 2. Restore parent's geometry
			 * 3. Call arrange() to re-tile
			 */

			/* Remove pair by shifting remaining entries down */
			if (i < self->n_pairs - 1) {
				memmove(&self->pairs[i],
				        &self->pairs[i + 1],
				        (gulong)(self->n_pairs - i - 1) * sizeof(SwallowPair));
			}
			self->n_pairs--;

			g_debug("swallow: unswallowed parent, %d active pairs",
			        self->n_pairs);
			return;
		}
	}

	g_debug("swallow: unswallow called for unknown parent");
}

static void
swallow_handler_init(GowlSwallowHandlerInterface *iface)
{
	iface->should_swallow = swallow_should_swallow;
	iface->swallow        = swallow_swallow;
	iface->unswallow      = swallow_unswallow;
}

/* --- GObject lifecycle --- */

static void
swallow_finalize(GObject *obj)
{
	GowlModuleSwallow *self;
	gint i;

	self = GOWL_MODULE_SWALLOW(obj);

	/* Free terminal app_id strings */
	for (i = 0; i < self->n_terminals; i++)
		g_free(self->terminal_ids[i]);

	G_OBJECT_CLASS(gowl_module_swallow_parent_class)->finalize(obj);
}

static void
gowl_module_swallow_class_init(GowlModuleSwallowClass *klass)
{
	GObjectClass    *obj_class;
	GowlModuleClass *mod_class;

	obj_class = G_OBJECT_CLASS(klass);
	mod_class = GOWL_MODULE_CLASS(klass);

	obj_class->finalize = swallow_finalize;

	mod_class->activate        = swallow_activate;
	mod_class->get_name        = swallow_get_name;
	mod_class->get_description = swallow_get_description;
	mod_class->get_version     = swallow_get_version;
	mod_class->configure       = swallow_configure;
}

static void
gowl_module_swallow_init(GowlModuleSwallow *self)
{
	self->compositor  = NULL;
	self->n_terminals = 0;
	self->n_pairs     = 0;
	memset(self->terminal_ids, 0, sizeof(self->terminal_ids));
	memset(self->pairs, 0, sizeof(self->pairs));
}

/* --- Shared-object entry point --- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_SWALLOW;
}
