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
 * Server-side glue for the gowl-private input-capture protocol.  Binds
 * the zgowl_input_capture_manager_v1 global to:
 *
 *   * a GowlInputCapture state machine, one per capture session, attached
 *     to the compositor so its input hooks divert input when active; the
 *     session registers itself as the machine's sink + activation
 *     callback and forwards captured events / activation to its
 *     wl_resource as protocol events, and
 *   * the compositor's input-injection API for the inject object
 *     (RemoteDesktop ingress).
 *
 * This file links no D-Bus and no libeis: the xdg-desktop-portal-gowl
 * binary speaks this protocol as a Wayland client and owns all the
 * freedesktop machinery.  Everything here runs on the compositor thread.
 */

#include "gowl-input-capture-protocol.h"
#include "gowl-input-capture-v1-protocol.h"

#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-input-capture.h"
#include "boxed/gowl-input-zone.h"
#include "boxed/gowl-input-barrier.h"

#include <wayland-server-core.h>

/* ---------------------------------------------------------------
 * State
 * --------------------------------------------------------------- */

struct _GowlInputCaptureProtocol {
	gpointer          compositor;   /* GowlCompositor *        */
	struct wl_global *global;
};

/* One capture session: owns a GowlInputCapture attached to the
 * compositor while alive, plus the staged (not-yet-installed) barriers. */
typedef struct {
	GowlInputCaptureProtocol *proto;
	struct wl_resource       *resource;
	GowlInputCapture         *capture;
	GList                    *staged;     /* GowlInputBarrier* (pending) */
} GowlCaptureSession;

/* ---------------------------------------------------------------
 * GowlInputCapture callbacks -> protocol events
 * --------------------------------------------------------------- */

static void
session_on_input(GowlInputCapture     *capture,
                 const GowlInputEvent *ev,
                 gpointer              user_data)
{
	GowlCaptureSession *s = user_data;

	(void)capture;

	switch (ev->type) {
	case GOWL_INPUT_EVENT_REL_MOTION:
		zgowl_input_capture_v1_send_rel_motion(
			s->resource, ev->time_msec,
			wl_fixed_from_double(ev->dx),
			wl_fixed_from_double(ev->dy));
		break;
	case GOWL_INPUT_EVENT_BUTTON:
		zgowl_input_capture_v1_send_button(
			s->resource, ev->time_msec, ev->button, ev->state);
		break;
	case GOWL_INPUT_EVENT_AXIS:
		zgowl_input_capture_v1_send_axis(
			s->resource, ev->time_msec, ev->axis,
			wl_fixed_from_double(ev->value), ev->discrete);
		break;
	case GOWL_INPUT_EVENT_KEY:
		zgowl_input_capture_v1_send_key(
			s->resource, ev->time_msec, ev->keycode, ev->state);
		break;
	case GOWL_INPUT_EVENT_MODIFIERS:
		zgowl_input_capture_v1_send_modifiers(
			s->resource, ev->mods_depressed, ev->mods_latched,
			ev->mods_locked, ev->mods_group);
		break;
	default:
		break;
	}
}

static void
session_on_activation(GowlInputCapture *capture,
                      guint32           activation_id,
                      gdouble           x,
                      gdouble           y,
                      guint32           barrier_id,
                      gboolean          activated,
                      gpointer          user_data)
{
	GowlCaptureSession *s = user_data;

	(void)capture;

	if (activated)
		zgowl_input_capture_v1_send_activated(
			s->resource, activation_id,
			wl_fixed_from_double(x), wl_fixed_from_double(y),
			barrier_id);
	else
		zgowl_input_capture_v1_send_deactivated(
			s->resource, activation_id);
}

/* ---------------------------------------------------------------
 * Capture session requests
 * --------------------------------------------------------------- */

static GList *
build_zones(GowlInputCaptureProtocol *proto)
{
	GList *mons, *l, *zones = NULL;

	/* gowl_compositor_get_monitors() is (transfer none): the returned
	 * GList IS the compositor's internal monitor list, owned by the
	 * compositor.  We must NOT free or modify it -- doing so frees the
	 * live list out from under the compositor (and the bar, which walks
	 * it every tick), causing a use-after-free crash on the dispatch
	 * thread.  Only iterate. */
	mons = gowl_compositor_get_monitors((GowlCompositor *)proto->compositor);
	for (l = mons; l != NULL; l = l->next) {
		GowlMonitor *mon = (GowlMonitor *)l->data;
		gint x, y, w, h;

		gowl_monitor_get_geometry(mon, &x, &y, &w, &h);
		if (w <= 0 || h <= 0)
			continue;
		zones = g_list_append(zones,
			gowl_input_zone_new((guint)w, (guint)h, x, y,
			                    gowl_monitor_get_name(mon)));
	}
	return zones;
}

static void
capture_get_zones(struct wl_client   *client,
                  struct wl_resource *resource)
{
	GowlCaptureSession *s = wl_resource_get_user_data(resource);
	GList *zones, *l;

	(void)client;

	/* Fix: CWE-476 — wl_resource_get_user_data() can return NULL if the
	 * resource was created without user data or if the destroy handler
	 * already ran (wl_resource_set_implementation guarantees the pointer
	 * during the object's lifetime, but defensive NULL-guards are required
	 * for all Wayland request handlers in the compositor thread). */
	if (s == NULL)
		return;

	zones = build_zones(s->proto);
	gowl_input_capture_set_zones(s->capture, zones);

	for (l = zones; l != NULL; l = l->next) {
		GowlInputZone *z = (GowlInputZone *)l->data;
		zgowl_input_capture_v1_send_zone(resource, z->width, z->height,
		                                 z->x, z->y);
	}
	zgowl_input_capture_v1_send_zones_done(
		resource, gowl_input_capture_get_zone_set(s->capture));

	g_list_free_full(zones, (GDestroyNotify)gowl_input_zone_free);
}

static void
capture_add_barrier(struct wl_client   *client,
                    struct wl_resource *resource,
                    uint32_t            id,
                    int32_t             x1,
                    int32_t             y1,
                    int32_t             x2,
                    int32_t             y2)
{
	GowlCaptureSession *s = wl_resource_get_user_data(resource);

	(void)client;

	/* Fix: CWE-476 — NULL-guard consistent with capture_get_zones. */
	if (s == NULL)
		return;

	s->staged = g_list_append(s->staged,
		gowl_input_barrier_new(id, x1, y1, x2, y2));
}

static void
capture_set_barriers(struct wl_client   *client,
                     struct wl_resource *resource,
                     uint32_t            zone_set)
{
	GowlCaptureSession *s = wl_resource_get_user_data(resource);
	GArray *accepted;
	GHashTable *acc_set;
	GList *l;
	guint i;

	(void)client;
	(void)zone_set;   /* validated against the live layout */

	/* Fix: CWE-476 — NULL-guard consistent with capture_get_zones. */
	if (s == NULL)
		return;

	accepted = g_array_new(FALSE, FALSE, sizeof(guint32));
	gowl_input_capture_set_barriers(s->capture, s->staged, accepted, NULL);

	/* Report status for every staged barrier id (accepted or not). */
	acc_set = g_hash_table_new(g_direct_hash, g_direct_equal);
	for (i = 0; i < accepted->len; i++)
		g_hash_table_add(acc_set,
			GUINT_TO_POINTER(g_array_index(accepted, guint32, i)));

	for (l = s->staged; l != NULL; l = l->next) {
		GowlInputBarrier *b = (GowlInputBarrier *)l->data;
		gboolean ok = g_hash_table_contains(acc_set,
			GUINT_TO_POINTER(b->id));
		zgowl_input_capture_v1_send_barrier_status(resource, b->id,
			ok ? 1 : 0);
	}
	zgowl_input_capture_v1_send_set_barriers_done(resource);

	g_hash_table_destroy(acc_set);
	g_array_unref(accepted);

	g_list_free_full(s->staged, (GDestroyNotify)gowl_input_barrier_free);
	s->staged = NULL;
}

static void
capture_enable(struct wl_client   *client,
               struct wl_resource *resource)
{
	GowlCaptureSession *s = wl_resource_get_user_data(resource);

	(void)client;

	/* Fix: CWE-476 — NULL-guard consistent with capture_get_zones. */
	if (s == NULL)
		return;

	gowl_input_capture_enable(s->capture);
}

static void
capture_disable(struct wl_client   *client,
                struct wl_resource *resource)
{
	GowlCaptureSession *s = wl_resource_get_user_data(resource);

	(void)client;

	/* Fix: CWE-476 — NULL-guard consistent with capture_get_zones. */
	if (s == NULL)
		return;

	gowl_input_capture_disable(s->capture);
	zgowl_input_capture_v1_send_disabled(resource);
}

static void
capture_release(struct wl_client   *client,
                struct wl_resource *resource,
                uint32_t            activation_id,
                uint32_t            has_position,
                wl_fixed_t          x,
                wl_fixed_t          y)
{
	GowlCaptureSession *s = wl_resource_get_user_data(resource);

	(void)client;
	(void)activation_id;

	/* Fix: CWE-476 — NULL-guard consistent with capture_get_zones. */
	if (s == NULL)
		return;

	/* Optionally warp the cursor to the requested release point (the
	 * point, in layout coordinates, the remote left off at). */
	if (has_position)
		gowl_compositor_warp_cursor(
			(GowlCompositor *)s->proto->compositor,
			wl_fixed_to_double(x), wl_fixed_to_double(y));

	gowl_input_capture_deactivate(s->capture);
}

static void
capture_destroy(struct wl_client   *client,
                struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zgowl_input_capture_v1_interface capture_impl = {
	.destroy      = capture_destroy,
	.get_zones    = capture_get_zones,
	.add_barrier  = capture_add_barrier,
	.set_barriers = capture_set_barriers,
	.enable       = capture_enable,
	.disable      = capture_disable,
	.release      = capture_release,
};

static void
capture_resource_destroy(struct wl_resource *resource)
{
	GowlCaptureSession *s = wl_resource_get_user_data(resource);

	if (s == NULL)
		return;

	/* Detach from the compositor before destroying the machine. */
	if (gowl_compositor_get_input_capture(
		    (GowlCompositor *)s->proto->compositor) == s->capture)
		gowl_compositor_set_input_capture(
			(GowlCompositor *)s->proto->compositor, NULL);

	g_list_free_full(s->staged, (GDestroyNotify)gowl_input_barrier_free);
	g_clear_object(&s->capture);
	g_free(s);
}

/* ---------------------------------------------------------------
 * Inject object requests (RemoteDesktop ingress)
 * --------------------------------------------------------------- */

static void
inject_pointer_motion(struct wl_client   *client,
                      struct wl_resource *resource,
                      wl_fixed_t          dx,
                      wl_fixed_t          dy)
{
	GowlInputCaptureProtocol *p = wl_resource_get_user_data(resource);

	(void)client;
	gowl_compositor_inject_pointer_motion((GowlCompositor *)p->compositor,
		wl_fixed_to_double(dx), wl_fixed_to_double(dy));
}

static void
inject_pointer_motion_absolute(struct wl_client   *client,
                               struct wl_resource *resource,
                               wl_fixed_t          x,
                               wl_fixed_t          y)
{
	GowlInputCaptureProtocol *p = wl_resource_get_user_data(resource);

	(void)client;
	gowl_compositor_inject_pointer_motion_absolute(
		(GowlCompositor *)p->compositor,
		wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void
inject_button(struct wl_client   *client,
              struct wl_resource *resource,
              uint32_t            button,
              uint32_t            state)
{
	GowlInputCaptureProtocol *p = wl_resource_get_user_data(resource);

	(void)client;
	gowl_compositor_inject_button((GowlCompositor *)p->compositor,
		button, state != 0);
}

static void
inject_axis(struct wl_client   *client,
            struct wl_resource *resource,
            uint32_t            axis,
            wl_fixed_t          value)
{
	GowlInputCaptureProtocol *p = wl_resource_get_user_data(resource);

	(void)client;
	gowl_compositor_inject_axis((GowlCompositor *)p->compositor,
		axis == 1, wl_fixed_to_double(value));
}

static void
inject_key(struct wl_client   *client,
           struct wl_resource *resource,
           uint32_t            keycode,
           uint32_t            state)
{
	GowlInputCaptureProtocol *p = wl_resource_get_user_data(resource);

	(void)client;
	gowl_compositor_inject_key((GowlCompositor *)p->compositor,
		keycode, state != 0);
}

static void
inject_frame(struct wl_client   *client,
             struct wl_resource *resource)
{
	(void)client;
	(void)resource;
	/* Events take effect immediately; frame is a batching hint only. */
}

static void
inject_destroy(struct wl_client   *client,
               struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zgowl_input_inject_v1_interface inject_impl = {
	.destroy                 = inject_destroy,
	.pointer_motion          = inject_pointer_motion,
	.pointer_motion_absolute = inject_pointer_motion_absolute,
	.button                  = inject_button,
	.axis                    = inject_axis,
	.key                     = inject_key,
	.frame                   = inject_frame,
};

/* ---------------------------------------------------------------
 * Manager requests
 * --------------------------------------------------------------- */

static void
manager_create_capture(struct wl_client   *client,
                       struct wl_resource *resource,
                       uint32_t            id)
{
	GowlInputCaptureProtocol *proto = wl_resource_get_user_data(resource);
	GowlCaptureSession       *s;
	struct wl_resource       *res;

	res = wl_resource_create(client, &zgowl_input_capture_v1_interface,
	                         wl_resource_get_version(resource), id);
	if (res == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	s = g_new0(GowlCaptureSession, 1);
	s->proto    = proto;
	s->resource = res;
	s->capture  = gowl_input_capture_new();
	s->staged   = NULL;

	gowl_input_capture_set_sink(s->capture, session_on_input, s);
	gowl_input_capture_set_activation_callback(s->capture,
		session_on_activation, s);

	/* Attach so the compositor input hooks consult this machine. */
	gowl_compositor_set_input_capture(
		(GowlCompositor *)proto->compositor, s->capture);

	wl_resource_set_implementation(res, &capture_impl, s,
	                               capture_resource_destroy);

	zgowl_input_capture_v1_send_capabilities(res,
		gowl_input_capture_get_capabilities(s->capture));
}

static void
manager_create_inject(struct wl_client   *client,
                      struct wl_resource *resource,
                      uint32_t            id)
{
	GowlInputCaptureProtocol *proto = wl_resource_get_user_data(resource);
	struct wl_resource       *res;

	res = wl_resource_create(client, &zgowl_input_inject_v1_interface,
	                         wl_resource_get_version(resource), id);
	if (res == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(res, &inject_impl, proto, NULL);
}

static void
manager_destroy(struct wl_client   *client,
                struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zgowl_input_capture_manager_v1_interface manager_impl = {
	.destroy        = manager_destroy,
	.create_capture = manager_create_capture,
	.create_inject  = manager_create_inject,
};

static void
manager_bind(struct wl_client *client, void *data,
             uint32_t version, uint32_t id)
{
	GowlInputCaptureProtocol *proto = data;
	struct wl_resource       *resource;

	resource = wl_resource_create(client,
		&zgowl_input_capture_manager_v1_interface,
		(gint)version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, proto, NULL);
}

/* ---------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------- */

GowlInputCaptureProtocol *
gowl_input_capture_protocol_register(gpointer            compositor,
                                     struct wl_display  *display)
{
	GowlInputCaptureProtocol *self;

	g_return_val_if_fail(compositor != NULL, NULL);
	g_return_val_if_fail(display != NULL, NULL);

	self = g_new0(GowlInputCaptureProtocol, 1);
	self->compositor = compositor;
	self->global = wl_global_create(display,
		&zgowl_input_capture_manager_v1_interface, 1, self,
		manager_bind);
	if (self->global == NULL) {
		g_free(self);
		return NULL;
	}
	return self;
}

void
gowl_input_capture_protocol_unregister(GowlInputCaptureProtocol *self)
{
	if (self == NULL)
		return;
	if (self->global != NULL)
		wl_global_destroy(self->global);
	g_free(self);
}
