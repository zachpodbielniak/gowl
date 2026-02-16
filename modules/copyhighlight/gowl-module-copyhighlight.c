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
#define G_LOG_DOMAIN "gowl-copyhighlight"

#include <glib-object.h>
#include <gmodule.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>

#include "module/gowl-module.h"
#include "core/gowl-compositor.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"

/**
 * GowlModuleCopyhighlight:
 *
 * Copy-on-highlight module.  When text is selected (highlighted) in
 * any client window, the primary selection is automatically synced to
 * the system clipboard so that Ctrl+V paste works in addition to the
 * traditional middle-click paste.
 *
 * Internally, a proxy #wlr_data_source is created that wraps the
 * primary selection source.  When a client reads from the clipboard,
 * the proxy forwards the read to the original primary selection source.
 *
 * This module implements #GowlStartupHandler and #GowlShutdownHandler
 * to manage its lifecycle.
 */

/* -----------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------- */

#define GOWL_TYPE_MODULE_COPYHIGHLIGHT (gowl_module_copyhighlight_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleCopyhighlight, gowl_module_copyhighlight,
                     GOWL, MODULE_COPYHIGHLIGHT, GowlModule)

/**
 * CopyhighlightSource:
 *
 * Proxy wlr_data_source that wraps a wlr_primary_selection_source.
 * The @base field must be first so wl_container_of() works correctly.
 */
typedef struct {
	struct wlr_data_source base;
	struct wlr_primary_selection_source *psel_source;
	struct wl_listener psel_destroy;
	GowlModuleCopyhighlight *module;
} CopyhighlightSource;

struct _GowlModuleCopyhighlight {
	GowlModule       parent_instance;
	GowlCompositor  *compositor;
	struct wlr_seat *seat;
	struct wl_listener set_psel;
	CopyhighlightSource *active_proxy;
};

static void copyhighlight_startup_init(GowlStartupHandlerInterface *iface);
static void copyhighlight_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleCopyhighlight, gowl_module_copyhighlight,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		copyhighlight_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER,
		copyhighlight_shutdown_init))

/* -----------------------------------------------------------
 * Proxy wlr_data_source implementation
 * ----------------------------------------------------------- */

/**
 * proxy_send:
 *
 * Forwards a clipboard read request to the wrapped primary selection
 * source.  If the original source has been destroyed, the fd is closed
 * to prevent a file descriptor leak.
 */
static void
proxy_send(
	struct wlr_data_source *source,
	const char             *mime_type,
	int32_t                 fd
){
	CopyhighlightSource *proxy;

	proxy = wl_container_of(source, proxy, base);

	if (proxy->psel_source != NULL) {
		wlr_primary_selection_source_send(
			proxy->psel_source, mime_type, (int)fd);
	} else {
		close((int)fd);
	}
}

/**
 * proxy_destroy:
 *
 * Cleans up the proxy data source.  Frees duplicated MIME type strings,
 * removes the listener on the original primary selection source, and
 * clears the module's active_proxy if it still points here.
 */
static void
proxy_destroy(struct wlr_data_source *source)
{
	CopyhighlightSource *proxy;
	char **mime;

	proxy = wl_container_of(source, proxy, base);

	/* free duplicated MIME type strings */
	wl_array_for_each(mime, &source->mime_types) {
		g_free(*mime);
	}

	wl_list_remove(&proxy->psel_destroy.link);

	/* clear module back-reference if this is the active proxy */
	if (proxy->module != NULL && proxy->module->active_proxy == proxy) {
		proxy->module->active_proxy = NULL;
	}

	g_free(proxy);
}

static const struct wlr_data_source_impl proxy_impl = {
	.send    = proxy_send,
	.destroy = proxy_destroy,
};

/* -----------------------------------------------------------
 * Primary selection source destroy handler
 * ----------------------------------------------------------- */

/**
 * on_psel_source_destroy:
 *
 * Called when the original primary selection source is destroyed
 * (e.g. the selecting client closes).  Invalidates the proxy's
 * reference to the source and destroys the proxy, which clears
 * the clipboard.
 */
static void
on_psel_source_destroy(struct wl_listener *listener, void *data)
{
	CopyhighlightSource *proxy;

	(void)data;

	proxy = wl_container_of(listener, proxy, psel_destroy);
	proxy->psel_source = NULL;

	wlr_data_source_destroy(&proxy->base);
}

/* -----------------------------------------------------------
 * Seat primary-selection-changed handler
 * ----------------------------------------------------------- */

/**
 * on_set_primary_selection:
 *
 * Called after the primary selection on the seat changes.  Creates a
 * proxy wlr_data_source that wraps the new primary selection source
 * and installs it as the clipboard selection.
 */
static void
on_set_primary_selection(struct wl_listener *listener, void *data)
{
	GowlModuleCopyhighlight *self;
	struct wlr_primary_selection_source *psel_src;
	CopyhighlightSource *proxy;
	char **mime;
	uint32_t serial;

	(void)data;

	self = wl_container_of(listener, self, set_psel);
	psel_src = self->seat->primary_selection_source;

	/* primary selection cleared -- leave clipboard as-is */
	if (psel_src == NULL)
		return;

	/* allocate and initialise the proxy data source */
	proxy = (CopyhighlightSource *)g_malloc0(sizeof(CopyhighlightSource));
	wlr_data_source_init(&proxy->base, &proxy_impl);
	proxy->psel_source = psel_src;
	proxy->module = self;

	/* deep-copy MIME types from the primary selection source */
	wl_array_for_each(mime, &psel_src->mime_types) {
		char *dup;
		char **slot;

		dup = g_strdup(*mime);
		slot = (char **)wl_array_add(
			&proxy->base.mime_types, sizeof(char *));
		*slot = dup;
	}

	/* listen for original source destruction */
	proxy->psel_destroy.notify = on_psel_source_destroy;
	wl_signal_add(&psel_src->events.destroy, &proxy->psel_destroy);

	/*
	 * NULL out active_proxy BEFORE calling wlr_seat_set_selection().
	 * That call may destroy the previous clipboard source, which may
	 * be our old proxy.  By clearing active_proxy first, the old
	 * proxy's destroy handler will not corrupt module state.
	 */
	self->active_proxy = NULL;

	serial = wl_display_next_serial(self->seat->display);
	wlr_seat_set_selection(self->seat, &proxy->base, serial);

	self->active_proxy = proxy;

	g_debug("copyhighlight: synced primary selection to clipboard "
	        "(%d MIME types)", (int)(psel_src->mime_types.size / sizeof(char *)));
}

/* -----------------------------------------------------------
 * GowlModule virtual methods
 * ----------------------------------------------------------- */

static gboolean
copyhighlight_activate(GowlModule *mod)
{
	(void)mod;
	g_debug("copyhighlight: activated");
	return TRUE;
}

static void
copyhighlight_deactivate(GowlModule *mod)
{
	(void)mod;
	g_debug("copyhighlight: deactivated");
}

static const gchar *
copyhighlight_get_name(GowlModule *mod)
{
	(void)mod;
	return "copyhighlight";
}

static const gchar *
copyhighlight_get_description(GowlModule *mod)
{
	(void)mod;
	return "Sync primary selection to clipboard on text highlight";
}

static const gchar *
copyhighlight_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/* -----------------------------------------------------------
 * GowlStartupHandler
 * ----------------------------------------------------------- */

/**
 * copyhighlight_on_startup:
 *
 * Obtains the wlr_seat from the compositor and registers a listener
 * on the set_primary_selection event to sync selections to clipboard.
 */
static void
copyhighlight_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleCopyhighlight *self;

	self = GOWL_MODULE_COPYHIGHLIGHT(handler);
	self->compositor = GOWL_COMPOSITOR(compositor);
	self->seat = gowl_compositor_get_wlr_seat(self->compositor);

	if (self->seat == NULL) {
		g_warning("copyhighlight: seat not available, module disabled");
		return;
	}

	self->set_psel.notify = on_set_primary_selection;
	wl_signal_add(&self->seat->events.set_primary_selection,
	              &self->set_psel);

	g_debug("copyhighlight: listening for primary selection changes");
}

static void
copyhighlight_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = copyhighlight_on_startup;
}

/* -----------------------------------------------------------
 * GowlShutdownHandler
 * ----------------------------------------------------------- */

/**
 * copyhighlight_on_shutdown:
 *
 * Removes the primary selection listener and destroys any active
 * proxy data source.
 */
static void
copyhighlight_on_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	GowlModuleCopyhighlight *self;

	(void)compositor;
	self = GOWL_MODULE_COPYHIGHLIGHT(handler);

	if (self->seat != NULL) {
		wl_list_remove(&self->set_psel.link);
	}

	if (self->active_proxy != NULL) {
		CopyhighlightSource *proxy;

		proxy = self->active_proxy;
		self->active_proxy = NULL;
		wlr_data_source_destroy(&proxy->base);
	}

	g_debug("copyhighlight: shutdown");
}

static void
copyhighlight_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = copyhighlight_on_shutdown;
}

/* -----------------------------------------------------------
 * GObject lifecycle
 * ----------------------------------------------------------- */

static void
gowl_module_copyhighlight_class_init(GowlModuleCopyhighlightClass *klass)
{
	GowlModuleClass *mod_class;

	mod_class = GOWL_MODULE_CLASS(klass);

	mod_class->activate        = copyhighlight_activate;
	mod_class->deactivate      = copyhighlight_deactivate;
	mod_class->get_name        = copyhighlight_get_name;
	mod_class->get_description = copyhighlight_get_description;
	mod_class->get_version     = copyhighlight_get_version;
}

static void
gowl_module_copyhighlight_init(GowlModuleCopyhighlight *self)
{
	self->compositor  = NULL;
	self->seat        = NULL;
	self->active_proxy = NULL;
}

/* -----------------------------------------------------------
 * Shared-object entry point
 * ----------------------------------------------------------- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_COPYHIGHLIGHT;
}
