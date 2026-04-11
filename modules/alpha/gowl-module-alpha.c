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
#define G_LOG_DOMAIN "gowl-alpha"

#include <glib-object.h>
#include <gmodule.h>
#include <stdlib.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "core/gowl-compositor.h"
#include "core/gowl-client.h"

/**
 * GowlModuleAlpha:
 *
 * Per-client window opacity with automatic focus dimming.  When the
 * focused client changes, the previously focused client fades to
 * @unfocused_alpha and the newly focused client is set to
 * @focused_alpha.  Both values are configurable via the YAML
 * modules section or gowl_module_configure().
 *
 * Configuration keys:
 *   - "focused-alpha":   opacity for the focused window (default 1.0)
 *   - "unfocused-alpha": opacity for unfocused windows (default 0.8)
 */

#define GOWL_TYPE_MODULE_ALPHA (gowl_module_alpha_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleAlpha, gowl_module_alpha,
                     GOWL, MODULE_ALPHA, GowlModule)

struct _GowlModuleAlpha {
	GowlModule parent_instance;
	gpointer   compositor;        /* borrowed GowlCompositor* */
	gfloat     focused_alpha;     /* default 1.0 */
	gfloat     unfocused_alpha;   /* default 0.8 */
	gulong     focus_handler_id;  /* g_signal_connect handler */
	gpointer   prev_focused;      /* last focused GowlClient* */
};

static void alpha_startup_init(GowlStartupHandlerInterface *iface);
static void alpha_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleAlpha, gowl_module_alpha,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		alpha_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER,
		alpha_shutdown_init))

/* --- Focus change callback --- */

static void
alpha_on_focus_changed(GowlCompositor *comp,
                       GObject        *client_obj,
                       gpointer        user_data)
{
	GowlModuleAlpha *self = GOWL_MODULE_ALPHA(user_data);
	GowlClient *client;
	GowlClient *prev;

	(void)comp;

	prev = (GowlClient *)self->prev_focused;
	client = (client_obj != NULL) ? GOWL_CLIENT(client_obj) : NULL;

	/* Dim the previously focused client (skip embedded — they render
	 * on top of their parent, not over the wallpaper) */
	if (prev != NULL && prev != client
	    && !gowl_client_get_embedded(prev))
		gowl_client_set_alpha(prev, self->unfocused_alpha);

	/* Brighten the newly focused client */
	if (client != NULL && !gowl_client_get_embedded(client))
		gowl_client_set_alpha(client, self->focused_alpha);

	self->prev_focused = client;
}

/* --- Apply initial alpha to all existing clients --- */

static void
alpha_apply_initial(GowlModuleAlpha *self)
{
	GowlCompositor *comp;
	GowlClient *focused;
	GList *clients, *l;

	comp = (GowlCompositor *)self->compositor;
	if (comp == NULL)
		return;

	focused = gowl_compositor_get_focused_client(comp);
	clients = gowl_compositor_get_clients(comp);

	for (l = clients; l != NULL; l = l->next) {
		GowlClient *c = GOWL_CLIENT(l->data);

		/* Skip embedded clients — they render on top of their
		 * parent and should stay fully opaque. */
		if (gowl_client_get_embedded(c))
			continue;

		if (c == focused)
			gowl_client_set_alpha(c, self->focused_alpha);
		else
			gowl_client_set_alpha(c, self->unfocused_alpha);
	}

	self->prev_focused = focused;
}

/* --- Reset all clients to opaque --- */

static void
alpha_reset_all(GowlModuleAlpha *self)
{
	GowlCompositor *comp;
	GList *clients, *l;

	comp = (GowlCompositor *)self->compositor;
	if (comp == NULL)
		return;

	clients = gowl_compositor_get_clients(comp);

	for (l = clients; l != NULL; l = l->next)
		gowl_client_set_alpha(GOWL_CLIENT(l->data), 1.0f);
}

/* --- GowlModule virtual methods --- */

static gboolean
alpha_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
alpha_deactivate(GowlModule *mod)
{
	GowlModuleAlpha *self = GOWL_MODULE_ALPHA(mod);

	/* Disconnect the focus signal */
	if (self->focus_handler_id != 0 && self->compositor != NULL) {
		g_signal_handler_disconnect(self->compositor,
		                            self->focus_handler_id);
		self->focus_handler_id = 0;
	}

	/* Restore all clients to fully opaque */
	alpha_reset_all(self);

	self->prev_focused = NULL;
	self->compositor = NULL;
}

static const gchar *
alpha_get_name(GowlModule *mod)
{
	(void)mod;
	return "alpha";
}

static const gchar *
alpha_get_description(GowlModule *mod)
{
	(void)mod;
	return "Per-client window opacity with focus dimming";
}

static const gchar *
alpha_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

static void
alpha_configure(GowlModule *mod, gpointer config)
{
	GowlModuleAlpha *self;
	GHashTable *settings;
	const gchar *val;

	self = GOWL_MODULE_ALPHA(mod);

	if (config == NULL)
		return;

	settings = (GHashTable *)config;

	val = (const gchar *)g_hash_table_lookup(settings, "focused-alpha");
	if (val != NULL)
		self->focused_alpha = (gfloat)g_ascii_strtod(val, NULL);

	val = (const gchar *)g_hash_table_lookup(settings, "unfocused-alpha");
	if (val != NULL)
		self->unfocused_alpha = (gfloat)g_ascii_strtod(val, NULL);

	/* Clamp to valid range */
	if (self->focused_alpha < 0.0f)   self->focused_alpha = 0.0f;
	if (self->focused_alpha > 1.0f)   self->focused_alpha = 1.0f;
	if (self->unfocused_alpha < 0.0f) self->unfocused_alpha = 0.0f;
	if (self->unfocused_alpha > 1.0f) self->unfocused_alpha = 1.0f;

	g_message("alpha: configured focused=%.2f unfocused=%.2f",
	          self->focused_alpha, self->unfocused_alpha);

	/* Re-apply to existing clients if compositor is running */
	if (self->compositor != NULL)
		alpha_apply_initial(self);
}

/* --- GowlStartupHandler --- */

static void
alpha_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleAlpha *self = GOWL_MODULE_ALPHA(handler);

	self->compositor = compositor;

	/* Connect to focus-changed signal.  The signal passes a
	 * GObject* (GowlClient or NULL) as the first argument. */
	self->focus_handler_id =
		g_signal_connect(compositor, "focus-changed",
		                 G_CALLBACK(alpha_on_focus_changed), self);

	/* Apply initial alpha to any clients already present */
	alpha_apply_initial(self);

	g_debug("alpha: startup, focused=%.2f unfocused=%.2f",
	        self->focused_alpha, self->unfocused_alpha);
}

static void
alpha_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = alpha_on_startup;
}

/* --- GowlShutdownHandler --- */

static void
alpha_on_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	GowlModuleAlpha *self = GOWL_MODULE_ALPHA(handler);

	(void)compositor;

	/* Disconnect and reset */
	if (self->focus_handler_id != 0 && self->compositor != NULL) {
		g_signal_handler_disconnect(self->compositor,
		                            self->focus_handler_id);
		self->focus_handler_id = 0;
	}
	self->prev_focused = NULL;
	self->compositor = NULL;
}

static void
alpha_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = alpha_on_shutdown;
}

/* --- GObject lifecycle --- */

static void
gowl_module_alpha_class_init(GowlModuleAlphaClass *klass)
{
	GowlModuleClass *mod_class;

	mod_class = GOWL_MODULE_CLASS(klass);

	mod_class->activate        = alpha_activate;
	mod_class->deactivate      = alpha_deactivate;
	mod_class->get_name        = alpha_get_name;
	mod_class->get_description = alpha_get_description;
	mod_class->get_version     = alpha_get_version;
	mod_class->configure       = alpha_configure;
}

static void
gowl_module_alpha_init(GowlModuleAlpha *self)
{
	self->compositor       = NULL;
	self->focused_alpha    = 1.0f;
	self->unfocused_alpha  = 0.8f;
	self->focus_handler_id = 0;
	self->prev_focused     = NULL;
}

/* --- Shared-object entry point --- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_ALPHA;
}
