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
 * GowlCaptureWlroots -- the wlroots-backed GowlCaptureProvider.
 *
 * Monitor capture (always): wlr-screencopy + the output image-capture
 * source manager.  Works on every supported wlroots (>= 0.19).
 *
 * Window capture (wlroots >= 0.20 only, GOWL_HAVE_CAPTURE_WINDOW): the
 * ext-image-copy-capture + ext-foreign-toplevel-list stack plus the
 * foreign-toplevel image-capture-source manager that bridges a window's
 * scene tree to a capture source.  This is the protocol set
 * xdg-desktop-portal-wlr 0.8.2 needs to enumerate and capture individual
 * windows (the "use system window capture" option).  On 0.19 these
 * functions do not exist in libwlroots, so the whole window path is
 * compiled out and the provider simply reports MONITOR-only.
 *
 * The window path keeps a hash table mapping each GowlClient to its
 * wlr_ext_foreign_toplevel_handle_v1.  Handles are created on map,
 * updated on title/app_id change, and destroyed on unmap.  When a client
 * requests to capture a toplevel, we look up the owning client's scene
 * tree and hand wlroots a scene-node capture source for it.
 */

#include "gowl-capture-wlroots.h"
#include "gowl-core-private.h"
#include "gowl-wlroots-compat.h"
#include "core/gowl-client.h"

#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>

#if GOWL_HAVE_CAPTURE_WINDOW
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#endif

struct _GowlCaptureWlroots {
	GObject parent_instance;

	/* Borrowed: the compositor owns us. */
	GowlCompositor *compositor;

#if GOWL_HAVE_CAPTURE_WINDOW
	struct wlr_ext_foreign_toplevel_list_v1               *toplevel_list;
	struct wlr_ext_image_copy_capture_manager_v1          *copy_capture;
	struct wlr_ext_output_image_capture_source_manager_v1 *output_source;
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1
	                                                      *toplevel_source;

	struct wl_listener toplevel_capture_request;

	/* GowlClient* -> struct wlr_ext_foreign_toplevel_handle_v1* */
	GHashTable *handles;
#endif
};

static void gowl_capture_wlroots_iface_init(GowlCaptureProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlCaptureWlroots, gowl_capture_wlroots, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_CAPTURE_PROVIDER,
	                      gowl_capture_wlroots_iface_init))

/* ------------------------------------------------------------------ *
 * Window-capture machinery (wlroots >= 0.20)
 * ------------------------------------------------------------------ */
#if GOWL_HAVE_CAPTURE_WINDOW

/*
 * A client asked to capture a specific toplevel.  Find the GowlClient
 * whose foreign-toplevel handle matches the request, build a capture
 * source from that client's scene tree, and accept.  If we cannot
 * resolve it, leave the request unaccepted (wlroots replies with a
 * stopped source to the client).
 */
static void
on_toplevel_capture_request(struct wl_listener *listener, void *data)
{
	GowlCaptureWlroots *self;
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request
		*request;
	GHashTableIter iter;
	gpointer key, value;
	GowlClient *client = NULL;

	self = wl_container_of(listener, self, toplevel_capture_request);
	request = data;

	/* Reverse-map the requested handle back to its GowlClient. */
	g_hash_table_iter_init(&iter, self->handles);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (value == request->toplevel_handle) {
			client = (GowlClient *)key;
			break;
		}
	}

	if (client != NULL) {
		struct wlr_scene_tree *tree = gowl_client_get_scene(client);
		if (tree != NULL && self->compositor != NULL) {
			struct wlr_ext_image_capture_source_v1 *source;

			source = wlr_ext_image_capture_source_v1_create_with_scene_node(
				&tree->node,
				self->compositor->event_loop,
				self->compositor->allocator,
				self->compositor->renderer);
			if (source != NULL) {
				wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(
					request, source);
				return;
			}
		}
	}

	g_debug("capture: could not resolve toplevel capture request");
}

/* Create (or refresh) the foreign-toplevel handle for a client. */
static void
window_add(GowlCaptureWlroots *self, GowlClient *client)
{
	struct wlr_ext_foreign_toplevel_handle_v1_state state;
	struct wlr_ext_foreign_toplevel_handle_v1 *handle;

	if (self->toplevel_list == NULL || client == NULL)
		return;
	if (g_hash_table_contains(self->handles, client))
		return;

	state.title  = gowl_client_get_title(client);
	state.app_id = gowl_client_get_app_id(client);

	handle = wlr_ext_foreign_toplevel_handle_v1_create(
		self->toplevel_list, &state);
	if (handle == NULL)
		return;

	g_hash_table_insert(self->handles, client, handle);
}

static void
window_update(GowlCaptureWlroots *self, GowlClient *client)
{
	struct wlr_ext_foreign_toplevel_handle_v1 *handle;
	struct wlr_ext_foreign_toplevel_handle_v1_state state;

	if (client == NULL)
		return;

	handle = g_hash_table_lookup(self->handles, client);
	if (handle == NULL) {
		/* Not yet tracked (e.g. title set before map finished); add. */
		window_add(self, client);
		return;
	}

	state.title  = gowl_client_get_title(client);
	state.app_id = gowl_client_get_app_id(client);
	wlr_ext_foreign_toplevel_handle_v1_update_state(handle, &state);
}

static void
window_remove(GowlCaptureWlroots *self, GowlClient *client)
{
	struct wlr_ext_foreign_toplevel_handle_v1 *handle;

	if (client == NULL)
		return;

	handle = g_hash_table_lookup(self->handles, client);
	if (handle != NULL) {
		wlr_ext_foreign_toplevel_handle_v1_destroy(handle);
		g_hash_table_remove(self->handles, client);
	}
}

#endif /* GOWL_HAVE_CAPTURE_WINDOW */

/* ------------------------------------------------------------------ *
 * GowlCaptureProvider implementation
 * ------------------------------------------------------------------ */

static GowlCaptureCapability
impl_get_capabilities(GowlCaptureProvider *provider)
{
	(void)provider;
#if GOWL_HAVE_CAPTURE_WINDOW
	return GOWL_CAPTURE_CAP_MONITOR | GOWL_CAPTURE_CAP_WINDOW;
#else
	return GOWL_CAPTURE_CAP_MONITOR;
#endif
}

static gboolean
impl_create_globals(GowlCaptureProvider *provider, gpointer display)
{
	struct wl_display *wl_display = display;
#if GOWL_HAVE_CAPTURE_WINDOW
	GowlCaptureWlroots *self = GOWL_CAPTURE_WLROOTS(provider);
#else
	(void)provider;
#endif

	g_return_val_if_fail(wl_display != NULL, FALSE);

	/* Monitor capture: always.  wlr-screencopy is the legacy path
	 * (still used by older portal backends and grim); the output
	 * image-capture source manager is the modern path. */
	wlr_screencopy_manager_v1_create(wl_display);
	wlr_ext_output_image_capture_source_manager_v1_create(wl_display, 1);

#if GOWL_HAVE_CAPTURE_WINDOW
	/* Window capture: enumerate toplevels + the image-copy-capture
	 * manager + the foreign-toplevel source bridge. */
	self->toplevel_list =
		wlr_ext_foreign_toplevel_list_v1_create(wl_display, 1);
	self->copy_capture =
		wlr_ext_image_copy_capture_manager_v1_create(wl_display, 1);
	self->toplevel_source =
		wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(
			wl_display, 1);

	if (self->toplevel_source != NULL) {
		self->toplevel_capture_request.notify =
			on_toplevel_capture_request;
		wl_signal_add(&self->toplevel_source->events.new_request,
		              &self->toplevel_capture_request);
	}
#endif

	return TRUE;
}

static GList *
impl_list_sources(GowlCaptureProvider *provider)
{
	GowlCaptureWlroots *self = GOWL_CAPTURE_WLROOTS(provider);
	GList *sources = NULL;
	GList *l;

	if (self->compositor == NULL)
		return NULL;

	/* Monitors: one source per output. */
	for (l = self->compositor->monitors; l != NULL; l = l->next) {
		GowlMonitor *m = (GowlMonitor *)l->data;
		const gchar *name;

		if (m->wlr_output == NULL)
			continue;
		name = gowl_monitor_get_name(m);
		sources = g_list_prepend(sources,
			gowl_capture_source_new(GOWL_CAPTURE_SOURCE_MONITOR,
			                        name, name, NULL));
	}

#if GOWL_HAVE_CAPTURE_WINDOW
	/* Windows: one source per tracked toplevel. */
	{
		GHashTableIter iter;
		gpointer key, value;

		g_hash_table_iter_init(&iter, self->handles);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			GowlClient *c = (GowlClient *)key;
			struct wlr_ext_foreign_toplevel_handle_v1 *h = value;

			sources = g_list_prepend(sources,
				gowl_capture_source_new(
					GOWL_CAPTURE_SOURCE_WINDOW,
					h->identifier,
					gowl_client_get_title(c),
					gowl_client_get_app_id(c)));
		}
	}
#endif

	return g_list_reverse(sources);
}

static void
impl_add_window(GowlCaptureProvider *provider, gpointer client)
{
#if GOWL_HAVE_CAPTURE_WINDOW
	GowlCaptureWlroots *self = GOWL_CAPTURE_WLROOTS(provider);
	GowlClient *c = client;

	if (c == NULL)
		return;
	window_add(self, c);

	{
		GowlCaptureSource *src = gowl_capture_source_new(
			GOWL_CAPTURE_SOURCE_WINDOW, NULL,
			gowl_client_get_title(c), gowl_client_get_app_id(c));
		g_signal_emit_by_name(provider, "source-added", src);
		gowl_capture_source_free(src);
	}
#else
	(void)provider;
	(void)client;
#endif
}

static void
impl_remove_window(GowlCaptureProvider *provider, gpointer client)
{
#if GOWL_HAVE_CAPTURE_WINDOW
	GowlCaptureWlroots *self = GOWL_CAPTURE_WLROOTS(provider);
	GowlClient *c = client;

	if (c == NULL || !g_hash_table_contains(self->handles, c))
		return;

	{
		GowlCaptureSource *src = gowl_capture_source_new(
			GOWL_CAPTURE_SOURCE_WINDOW, NULL,
			gowl_client_get_title(c), gowl_client_get_app_id(c));
		g_signal_emit_by_name(provider, "source-removed", src);
		gowl_capture_source_free(src);
	}

	window_remove(self, c);
#else
	(void)provider;
	(void)client;
#endif
}

static void
impl_update_window(GowlCaptureProvider *provider, gpointer client)
{
#if GOWL_HAVE_CAPTURE_WINDOW
	window_update(GOWL_CAPTURE_WLROOTS(provider), client);
#else
	(void)provider;
	(void)client;
#endif
}

static void
gowl_capture_wlroots_iface_init(GowlCaptureProviderInterface *iface)
{
	iface->get_capabilities = impl_get_capabilities;
	iface->create_globals   = impl_create_globals;
	iface->list_sources     = impl_list_sources;
	iface->add_window       = impl_add_window;
	iface->remove_window    = impl_remove_window;
	iface->update_window    = impl_update_window;
}

/* ------------------------------------------------------------------ *
 * GObject lifecycle
 * ------------------------------------------------------------------ */

static void
gowl_capture_wlroots_finalize(GObject *object)
{
#if GOWL_HAVE_CAPTURE_WINDOW
	GowlCaptureWlroots *self = GOWL_CAPTURE_WLROOTS(object);

	/* Handles are owned by wlroots and torn down with the display; we
	 * just drop our tracking table.  The capture-request listener is
	 * removed automatically when the manager's global is destroyed. */
	if (self->handles != NULL) {
		g_hash_table_destroy(self->handles);
		self->handles = NULL;
	}
#endif

	G_OBJECT_CLASS(gowl_capture_wlroots_parent_class)->finalize(object);
}

static void
gowl_capture_wlroots_class_init(GowlCaptureWlrootsClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = gowl_capture_wlroots_finalize;
}

static void
gowl_capture_wlroots_init(GowlCaptureWlroots *self)
{
#if GOWL_HAVE_CAPTURE_WINDOW
	self->handles = g_hash_table_new(g_direct_hash, g_direct_equal);
#else
	(void)self;
#endif
}

GowlCaptureProvider *
gowl_capture_wlroots_new(gpointer compositor)
{
	GowlCaptureWlroots *self;

	self = g_object_new(GOWL_TYPE_CAPTURE_WLROOTS, NULL);
	self->compositor = (GowlCompositor *)compositor;

	return GOWL_CAPTURE_PROVIDER(self);
}
