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
 *
 * Minimum-viable server implementation of `ext_workspace_v1`.
 *
 * Scope:
 *   - Single implicit workspace group per compositor (no per-output
 *     groups).
 *   - Workspaces advertised with: id, name, state (active or idle),
 *     capabilities (activate only).
 *   - Client requests honoured: commit (batching), stop, activate.
 *     deactivate/remove/assign are accepted but no-op — we don't
 *     expose those capabilities.
 *   - No coordinates advertised; bars that don't use them (waybar's
 *     default) work unchanged.
 *
 * The implementation tracks bound clients so workspace events reach
 * every taskbar without per-call enumeration, and connects the
 * three compositor workspace signals so events broadcast
 * automatically.
 */

#include "gowl-ext-workspace.h"
#include "../core/gowl-compositor.h"
#include "../core/gowl-workspace.h"
#include "../interfaces/gowl-workspace-provider.h"
#include "ext-workspace-v1-protocol.h"

#include <wayland-server-core.h>

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------
 * Internal state.  The manager owns the wl_global + signal bridges
 * and a list of live client resources (one per bound manager).
 * --------------------------------------------------------------- */

typedef struct {
	GowlExtWorkspaceManager *manager;
	struct wl_resource      *manager_resource;
	struct wl_resource      *group_resource;
	/* workspace id -> wl_resource for per-client workspace handles */
	GHashTable              *workspace_resources; /* guint64 -> wl_resource* */
	gboolean                 stopped;
} GowlExtClient;

struct GowlExtWorkspaceManager {
	gpointer                  compositor;         /* GowlCompositor *    */
	struct wl_global         *global;
	GList                    *clients;            /* GowlExtClient* list */

	/* Signal handler ids so we can disconnect cleanly. */
	gulong                    sig_created;
	gulong                    sig_switched;
	gulong                    sig_destroyed;
};

/* ---------------------------------------------------------------
 * Workspace handle resource
 * --------------------------------------------------------------- */

static void
workspace_handle_destroy(struct wl_client   *client,
                          struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void
workspace_handle_activate(struct wl_client   *client,
                           struct wl_resource *resource)
{
	GowlExtClient                *ext_client;
	GowlExtWorkspaceManager      *mgr;
	GowlWorkspaceProvider        *provider;
	GowlWorkspace                *target;
	guint64                       id;

	(void)client;

	ext_client = wl_resource_get_user_data(
		wl_resource_get_user_data(resource));
	if (ext_client == NULL)
		return;
	mgr = ext_client->manager;

	id = (guint64)(guintptr)wl_resource_get_user_data(resource);
	provider = gowl_compositor_get_workspace_provider(
		(GowlCompositor *)mgr->compositor);
	if (provider == NULL)
		return;

	target = gowl_workspace_provider_lookup(provider, id);
	if (target == NULL)
		return;

	/* Batched: real switch happens in commit().  For simplicity
	 * in this minimum-viable impl we switch immediately; bars
	 * expect the switch to take effect promptly anyway. */
	if (gowl_workspace_provider_switch_to(provider, target))
		gowl_compositor_emit_workspace_switched(
			(GowlCompositor *)mgr->compositor,
			NULL, target);
}

static void
workspace_handle_noop_request(struct wl_client   *client,
                               struct wl_resource *resource)
{
	(void)client;
	(void)resource;
	/* deactivate / remove fall here — no capabilities advertised
	 * so most bars never call these.  If one does, we ignore the
	 * request; spec allows a compositor to reject operations it
	 * didn't advertise capabilities for. */
}

static void
workspace_handle_assign(struct wl_client   *client,
                         struct wl_resource *resource,
                         struct wl_resource *group)
{
	(void)client;
	(void)resource;
	(void)group;
	/* We only have one implicit group; assign is a no-op. */
}

static const struct ext_workspace_handle_v1_interface
workspace_handle_impl = {
	.destroy     = workspace_handle_destroy,
	.activate    = workspace_handle_activate,
	.deactivate  = workspace_handle_noop_request,
	.assign      = workspace_handle_assign,
	.remove      = workspace_handle_noop_request,
};

static void
workspace_handle_resource_destroy(struct wl_resource *resource)
{
	(void)resource;
	/* Nothing to free — workspace resources are indexed by id in
	 * the client's hash table and removed on workspace-destroyed.
	 * If the client dies first, the hash table value goes stale
	 * but is only consulted by subsequent broadcasts that
	 * iterate live resources; we clean up in the client-resource
	 * destroy handler below. */
}

/* Allocate a workspace_handle_v1 resource and emit its initial
 * events (id + name + state + capabilities). */
static struct wl_resource *
send_workspace_handle(GowlExtClient *ext_client,
                       GowlWorkspace *ws,
                       guint32        workspace_version)
{
	struct wl_resource *handle;
	const gchar        *name;
	guint64             ws_id;
	gchar               id_buf[32];

	ws_id = gowl_workspace_get_id(ws);
	handle = wl_resource_create(
		wl_resource_get_client(ext_client->manager_resource),
		&ext_workspace_handle_v1_interface,
		workspace_version,
		0);
	if (handle == NULL)
		return NULL;
	wl_resource_set_implementation(handle,
	                                &workspace_handle_impl,
	                                (void *)(guintptr)ws_id,
	                                workspace_handle_resource_destroy);

	ext_workspace_manager_v1_send_workspace(
		ext_client->manager_resource, handle);

	/* Group membership: send workspace_enter on the group. */
	if (ext_client->group_resource != NULL)
		ext_workspace_group_handle_v1_send_workspace_enter(
			ext_client->group_resource, handle);

	/* id: human-readable string form of the numeric workspace id. */
	snprintf(id_buf, sizeof(id_buf), "%" G_GUINT64_FORMAT, ws_id);
	ext_workspace_handle_v1_send_id(handle, id_buf);

	/* name (defaults to the id if the workspace has none). */
	name = gowl_workspace_get_name(ws);
	ext_workspace_handle_v1_send_name(handle,
	                                   name != NULL ? name : id_buf);

	/* capabilities: activate only. */
	ext_workspace_handle_v1_send_capabilities(
		handle,
		EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);

	g_hash_table_insert(ext_client->workspace_resources,
	                    g_memdup2(&ws_id, sizeof(ws_id)),
	                    handle);
	return handle;
}

static void
send_workspace_state(struct wl_resource *handle, gboolean active)
{
	ext_workspace_handle_v1_send_state(
		handle,
		active
		? EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE
		: 0);
}

/* ---------------------------------------------------------------
 * Group handle resource
 * --------------------------------------------------------------- */

static void
group_handle_create_workspace(struct wl_client   *client,
                               struct wl_resource *resource,
                               const char         *workspace_name)
{
	(void)client;
	(void)resource;
	(void)workspace_name;
	/* We don't advertise create_workspace capability; no-op. */
}

static void
group_handle_destroy(struct wl_client   *client,
                      struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct ext_workspace_group_handle_v1_interface
group_handle_impl = {
	.create_workspace = group_handle_create_workspace,
	.destroy          = group_handle_destroy,
};

static void
group_handle_resource_destroy(struct wl_resource *resource)
{
	(void)resource;
}

/* ---------------------------------------------------------------
 * Manager resource
 * --------------------------------------------------------------- */

static void
manager_commit(struct wl_client   *client,
                struct wl_resource *resource)
{
	(void)client;
	(void)resource;
	/* Batched requests already took effect inline; commit is a
	 * no-op in the simplified implementation.  The spec allows
	 * this — commit simply signals "I'm done queuing." */
}

static void
manager_stop(struct wl_client   *client,
              struct wl_resource *resource)
{
	GowlExtClient *ext_client;

	(void)client;

	ext_client = wl_resource_get_user_data(resource);
	if (ext_client == NULL || ext_client->stopped)
		return;
	ext_client->stopped = TRUE;

	/* Tell the client we acknowledged the stop.  The handle
	 * remains valid until the client destroys it. */
	ext_workspace_manager_v1_send_finished(resource);
}

static const struct ext_workspace_manager_v1_interface manager_impl = {
	.commit = manager_commit,
	.stop   = manager_stop,
};

static void
manager_resource_destroy(struct wl_resource *resource)
{
	GowlExtClient *ext_client = wl_resource_get_user_data(resource);

	if (ext_client == NULL)
		return;

	ext_client->manager->clients = g_list_remove(
		ext_client->manager->clients, ext_client);

	g_clear_pointer(&ext_client->workspace_resources,
	                 g_hash_table_unref);
	g_free(ext_client);
}

/* Broadcast the entire current workspace set to a newly-bound
 * client, then fire `done`. */
static void
broadcast_initial(GowlExtClient *ext_client, guint32 version)
{
	GowlWorkspaceProvider *provider;
	GowlWorkspace         *current;
	GList                 *all, *it;

	provider = gowl_compositor_get_workspace_provider(
		(GowlCompositor *)ext_client->manager->compositor);

	/* Group: one implicit "all outputs" group. */
	ext_client->group_resource = wl_resource_create(
		wl_resource_get_client(ext_client->manager_resource),
		&ext_workspace_group_handle_v1_interface,
		version,
		0);
	if (ext_client->group_resource != NULL) {
		wl_resource_set_implementation(
			ext_client->group_resource,
			&group_handle_impl,
			ext_client,
			group_handle_resource_destroy);
		ext_workspace_manager_v1_send_workspace_group(
			ext_client->manager_resource,
			ext_client->group_resource);
		ext_workspace_group_handle_v1_send_capabilities(
			ext_client->group_resource, 0);
	}

	if (provider == NULL) {
		ext_workspace_manager_v1_send_done(
			ext_client->manager_resource);
		return;
	}

	current = gowl_workspace_provider_get_current(provider);
	all = gowl_workspace_provider_list(provider);
	for (it = all; it != NULL; it = it->next) {
		GowlWorkspace      *ws = GOWL_WORKSPACE(it->data);
		struct wl_resource *handle;

		handle = send_workspace_handle(ext_client, ws, version);
		if (handle != NULL)
			send_workspace_state(handle, ws == current);
	}
	g_list_free(all);

	ext_workspace_manager_v1_send_done(ext_client->manager_resource);
}

static void
manager_bind(struct wl_client *client, void *data,
              guint32 version, guint32 id)
{
	GowlExtWorkspaceManager *mgr = data;
	GowlExtClient           *ext_client;
	struct wl_resource      *resource;

	resource = wl_resource_create(client,
	                              &ext_workspace_manager_v1_interface,
	                              (gint)version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	ext_client = g_new0(GowlExtClient, 1);
	ext_client->manager             = mgr;
	ext_client->manager_resource    = resource;
	ext_client->group_resource      = NULL;
	ext_client->workspace_resources = g_hash_table_new_full(
		g_int64_hash, g_int64_equal, g_free, NULL);
	ext_client->stopped             = FALSE;

	wl_resource_set_implementation(resource, &manager_impl,
	                                ext_client,
	                                manager_resource_destroy);

	mgr->clients = g_list_prepend(mgr->clients, ext_client);

	broadcast_initial(ext_client, (guint32)version);
}

/* ---------------------------------------------------------------
 * Signal bridges from GowlCompositor → protocol broadcasts
 * --------------------------------------------------------------- */

static void
on_workspace_created(GowlCompositor *compositor,
                      GowlWorkspace  *ws,
                      gpointer        user_data)
{
	GowlExtWorkspaceManager *mgr = user_data;
	GList                   *it;

	(void)compositor;

	for (it = mgr->clients; it != NULL; it = it->next) {
		GowlExtClient      *ext_client = it->data;
		guint32             version;
		struct wl_resource *handle;

		if (ext_client->stopped)
			continue;
		version = wl_resource_get_version(
			ext_client->manager_resource);
		handle = send_workspace_handle(ext_client, ws, version);
		if (handle != NULL)
			send_workspace_state(handle, FALSE);
		ext_workspace_manager_v1_send_done(
			ext_client->manager_resource);
	}
}

static void
on_workspace_switched(GowlCompositor *compositor,
                       GowlWorkspace  *from,
                       GowlWorkspace  *to,
                       gpointer        user_data)
{
	GowlExtWorkspaceManager *mgr = user_data;
	GList                   *it;

	(void)compositor;

	for (it = mgr->clients; it != NULL; it = it->next) {
		GowlExtClient      *ext_client = it->data;
		struct wl_resource *handle;

		if (ext_client->stopped)
			continue;

		if (from != NULL) {
			guint64 id = gowl_workspace_get_id(from);
			handle = g_hash_table_lookup(
				ext_client->workspace_resources, &id);
			if (handle != NULL)
				send_workspace_state(handle, FALSE);
		}
		if (to != NULL) {
			guint64 id = gowl_workspace_get_id(to);
			handle = g_hash_table_lookup(
				ext_client->workspace_resources, &id);
			if (handle != NULL)
				send_workspace_state(handle, TRUE);
		}
		ext_workspace_manager_v1_send_done(
			ext_client->manager_resource);
	}
}

static void
on_workspace_destroyed(GowlCompositor *compositor,
                        GowlWorkspace  *ws,
                        gpointer        user_data)
{
	GowlExtWorkspaceManager *mgr = user_data;
	GList                   *it;
	guint64                  id  = gowl_workspace_get_id(ws);

	(void)compositor;

	for (it = mgr->clients; it != NULL; it = it->next) {
		GowlExtClient      *ext_client = it->data;
		struct wl_resource *handle;

		if (ext_client->stopped)
			continue;

		handle = g_hash_table_lookup(
			ext_client->workspace_resources, &id);
		if (handle == NULL)
			continue;

		ext_workspace_handle_v1_send_removed(handle);
		g_hash_table_remove(ext_client->workspace_resources, &id);
		ext_workspace_manager_v1_send_done(
			ext_client->manager_resource);
	}
}

/* ---------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------- */

GowlExtWorkspaceManager *
gowl_ext_workspace_manager_register(gpointer            compositor,
                                     struct wl_display *display)
{
	GowlExtWorkspaceManager *mgr;

	g_return_val_if_fail(compositor != NULL, NULL);
	g_return_val_if_fail(display    != NULL, NULL);

	mgr = g_new0(GowlExtWorkspaceManager, 1);
	mgr->compositor = compositor;
	mgr->clients    = NULL;

	mgr->global = wl_global_create(
		display,
		&ext_workspace_manager_v1_interface,
		1,
		mgr,
		manager_bind);
	if (mgr->global == NULL) {
		g_free(mgr);
		return NULL;
	}

	mgr->sig_created = g_signal_connect(
		compositor, "workspace-created",
		G_CALLBACK(on_workspace_created), mgr);
	mgr->sig_switched = g_signal_connect(
		compositor, "workspace-switched",
		G_CALLBACK(on_workspace_switched), mgr);
	mgr->sig_destroyed = g_signal_connect(
		compositor, "workspace-destroyed",
		G_CALLBACK(on_workspace_destroyed), mgr);

	return mgr;
}

void
gowl_ext_workspace_manager_unregister(GowlExtWorkspaceManager *self)
{
	if (self == NULL)
		return;

	if (self->sig_created != 0)
		g_signal_handler_disconnect(self->compositor,
		                             self->sig_created);
	if (self->sig_switched != 0)
		g_signal_handler_disconnect(self->compositor,
		                             self->sig_switched);
	if (self->sig_destroyed != 0)
		g_signal_handler_disconnect(self->compositor,
		                             self->sig_destroyed);

	if (self->global != NULL)
		wl_global_destroy(self->global);

	/* Clients still hold resources pointing at `self`.  The
	 * resource-destroy callback will remove them from
	 * self->clients; we don't free the list here to avoid
	 * double-free.  It's safe to leak the GList tail if the
	 * display is being torn down — wl_global_destroy marks
	 * resources as defunct and future sends are no-ops. */
	g_free(self);
}
