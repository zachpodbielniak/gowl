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

#include "portal-wayland.h"

#include <gio/gio.h>
#include <glib-unix.h>
#include <wayland-client.h>

#include "gowl-input-capture-v1-client-protocol.h"

struct _PortalWayland {
	PortalEis                       *eis;     /* not owned */

	struct wl_display               *display;
	struct wl_registry              *registry;
	struct zgowl_input_capture_manager_v1 *manager;
	struct zgowl_input_capture_v1   *capture;
	struct zgowl_input_inject_v1    *inject;

	guint                            fd_source;

	/* get_zones reply accumulation */
	GArray                          *zones;     /* PortalZone */
	guint32                          zone_set;
	gboolean                         zones_done;

	PortalActivationFunc             activation_cb;
	gpointer                         activation_data;
	PortalZonesChangedFunc           zones_changed_cb;
	gpointer                         zones_changed_data;
};

/* ---------------------------------------------------------------
 * Capture session event listener
 * --------------------------------------------------------------- */

static void
cap_capabilities(void *data, struct zgowl_input_capture_v1 *cap,
                 uint32_t capabilities)
{
	(void)data;
	(void)cap;
	(void)capabilities;
}

static void
cap_zone(void *data, struct zgowl_input_capture_v1 *cap,
         uint32_t width, uint32_t height, int32_t x, int32_t y)
{
	PortalWayland *self = data;
	PortalZone z;

	(void)cap;
	z.width = width;
	z.height = height;
	z.x = x;
	z.y = y;
	g_array_append_val(self->zones, z);
}

static void
cap_zones_done(void *data, struct zgowl_input_capture_v1 *cap,
               uint32_t zone_set)
{
	PortalWayland *self = data;

	(void)cap;
	self->zone_set = zone_set;
	self->zones_done = TRUE;
}

static void
cap_zones_changed(void *data, struct zgowl_input_capture_v1 *cap,
                  uint32_t zone_set)
{
	PortalWayland *self = data;

	(void)cap;
	if (self->zones_changed_cb != NULL)
		self->zones_changed_cb(zone_set, self->zones_changed_data);
}

static void
cap_barrier_status(void *data, struct zgowl_input_capture_v1 *cap,
                   uint32_t id, uint32_t accepted)
{
	(void)data;
	(void)cap;
	(void)id;
	(void)accepted;
	/* Accepted/rejected status is consumed synchronously in
	 * set_barriers via a roundtrip; nothing to accumulate here for the
	 * minimal backend. */
}

static void
cap_set_barriers_done(void *data, struct zgowl_input_capture_v1 *cap)
{
	(void)data;
	(void)cap;
}

static void
cap_activated(void *data, struct zgowl_input_capture_v1 *cap,
              uint32_t activation_id, wl_fixed_t cursor_x,
              wl_fixed_t cursor_y, uint32_t barrier_id)
{
	PortalWayland *self = data;

	(void)cap;
	/* Begin the EIS emulation burst and notify the D-Bus layer. */
	portal_eis_start(self->eis);
	if (self->activation_cb != NULL)
		self->activation_cb(activation_id,
			wl_fixed_to_double(cursor_x),
			wl_fixed_to_double(cursor_y),
			barrier_id, TRUE, self->activation_data);
}

static void
cap_deactivated(void *data, struct zgowl_input_capture_v1 *cap,
                uint32_t activation_id)
{
	PortalWayland *self = data;

	(void)cap;
	portal_eis_stop(self->eis);
	if (self->activation_cb != NULL)
		self->activation_cb(activation_id, 0.0, 0.0, 0, FALSE,
			self->activation_data);
}

static void
cap_disabled(void *data, struct zgowl_input_capture_v1 *cap)
{
	(void)data;
	(void)cap;
}

static void
cap_rel_motion(void *data, struct zgowl_input_capture_v1 *cap,
               uint32_t time, wl_fixed_t dx, wl_fixed_t dy)
{
	PortalWayland *self = data;

	(void)cap;
	(void)time;
	portal_eis_rel_motion(self->eis, wl_fixed_to_double(dx),
		wl_fixed_to_double(dy));
	portal_eis_frame(self->eis);
}

static void
cap_button(void *data, struct zgowl_input_capture_v1 *cap,
           uint32_t time, uint32_t button, uint32_t state)
{
	PortalWayland *self = data;

	(void)cap;
	(void)time;
	portal_eis_button(self->eis, button, state != 0);
	portal_eis_frame(self->eis);
}

static void
cap_axis(void *data, struct zgowl_input_capture_v1 *cap,
         uint32_t time, uint32_t axis, wl_fixed_t value, int32_t discrete)
{
	PortalWayland *self = data;

	(void)cap;
	(void)time;
	(void)discrete;
	portal_eis_scroll(self->eis, axis, wl_fixed_to_double(value));
	portal_eis_frame(self->eis);
}

static void
cap_key(void *data, struct zgowl_input_capture_v1 *cap,
        uint32_t time, uint32_t key, uint32_t state)
{
	PortalWayland *self = data;

	(void)cap;
	(void)time;
	portal_eis_key(self->eis, key, state != 0);
	portal_eis_frame(self->eis);
}

static void
cap_modifiers(void *data, struct zgowl_input_capture_v1 *cap,
              uint32_t depressed, uint32_t latched, uint32_t locked,
              uint32_t group)
{
	PortalWayland *self = data;

	(void)cap;
	portal_eis_modifiers(self->eis, depressed, latched, locked, group);
}

static const struct zgowl_input_capture_v1_listener capture_listener = {
	.capabilities      = cap_capabilities,
	.zone              = cap_zone,
	.zones_done        = cap_zones_done,
	.zones_changed     = cap_zones_changed,
	.barrier_status    = cap_barrier_status,
	.set_barriers_done = cap_set_barriers_done,
	.activated         = cap_activated,
	.deactivated       = cap_deactivated,
	.disabled          = cap_disabled,
	.rel_motion        = cap_rel_motion,
	.button            = cap_button,
	.axis              = cap_axis,
	.key               = cap_key,
	.modifiers         = cap_modifiers,
};

/* ---------------------------------------------------------------
 * Registry
 * --------------------------------------------------------------- */

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
                const char *interface, uint32_t version)
{
	PortalWayland *self = data;

	(void)version;
	if (g_strcmp0(interface,
	              zgowl_input_capture_manager_v1_interface.name) == 0) {
		self->manager = wl_registry_bind(registry, name,
			&zgowl_input_capture_manager_v1_interface, 1);
	}
}

static void
registry_global_remove(void *data, struct wl_registry *registry,
                       uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global        = registry_global,
	.global_remove = registry_global_remove,
};

/* ---------------------------------------------------------------
 * GLib main-loop integration of the wl_display fd
 * --------------------------------------------------------------- */

static gboolean
on_wayland_fd(gint fd, GIOCondition cond, gpointer data)
{
	PortalWayland *self = data;

	(void)fd;
	(void)cond;

	if (wl_display_dispatch(self->display) < 0)
		return G_SOURCE_REMOVE;
	wl_display_flush(self->display);
	return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------- */

PortalWayland *
portal_wayland_new(PortalEis *eis, GError **error)
{
	PortalWayland *self;
	int fd;

	self = g_new0(PortalWayland, 1);
	self->eis = eis;
	self->zones = g_array_new(FALSE, FALSE, sizeof(PortalZone));

	self->display = wl_display_connect(NULL);
	if (self->display == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
			"could not connect to the Wayland display");
		g_array_unref(self->zones);
		g_free(self);
		return NULL;
	}

	self->registry = wl_display_get_registry(self->display);
	wl_registry_add_listener(self->registry, &registry_listener, self);
	wl_display_roundtrip(self->display);

	if (self->manager == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"compositor does not implement gowl-input-capture "
			"(is this gowl / cmacs --gowl?)");
		wl_display_disconnect(self->display);
		g_array_unref(self->zones);
		g_free(self);
		return NULL;
	}

	self->capture = zgowl_input_capture_manager_v1_create_capture(
		self->manager);
	zgowl_input_capture_v1_add_listener(self->capture, &capture_listener,
		self);
	self->inject = zgowl_input_capture_manager_v1_create_inject(
		self->manager);
	wl_display_roundtrip(self->display);

	fd = wl_display_get_fd(self->display);
	self->fd_source = g_unix_fd_add(fd, G_IO_IN, on_wayland_fd, self);

	return self;
}

void
portal_wayland_free(PortalWayland *self)
{
	if (self == NULL)
		return;

	if (self->fd_source != 0)
		g_source_remove(self->fd_source);
	if (self->inject != NULL)
		zgowl_input_inject_v1_destroy(self->inject);
	if (self->capture != NULL)
		zgowl_input_capture_v1_destroy(self->capture);
	if (self->manager != NULL)
		zgowl_input_capture_manager_v1_destroy(self->manager);
	if (self->registry != NULL)
		wl_registry_destroy(self->registry);
	if (self->display != NULL)
		wl_display_disconnect(self->display);
	if (self->zones != NULL)
		g_array_unref(self->zones);
	g_free(self);
}

void
portal_wayland_set_activation_callback(PortalWayland       *self,
                                       PortalActivationFunc cb,
                                       gpointer             user_data)
{
	self->activation_cb = cb;
	self->activation_data = user_data;
}

void
portal_wayland_set_zones_changed_callback(PortalWayland          *self,
                                          PortalZonesChangedFunc  cb,
                                          gpointer                user_data)
{
	self->zones_changed_cb = cb;
	self->zones_changed_data = user_data;
}

gboolean
portal_wayland_get_zones(PortalWayland *self, PortalZone **zones_out,
                         guint *n_zones_out, guint32 *zone_set_out)
{
	g_array_set_size(self->zones, 0);
	self->zones_done = FALSE;

	zgowl_input_capture_v1_get_zones(self->capture);

	/*
	 * Fix: CWE-835 Loop with Unreachable Exit Condition — if the
	 * compositor is connected but never sends a zones_done event (e.g.
	 * due to a bug or a deliberately slow compositor), the original
	 * unbounded loop would block the portal process indefinitely,
	 * preventing any further D-Bus method handling.  Cap the number of
	 * roundtrips: each roundtrip delivers all currently-buffered events
	 * from the compositor, so a well-behaved compositor will complete
	 * within the first roundtrip; a few extra are allowed for transient
	 * backpressure.  If zones_done is still not set after the limit,
	 * treat it as a compositor error and return FALSE so the D-Bus
	 * caller receives a failure response instead of hanging.
	 */
	{
		gint attempts = 0;
		const gint max_attempts = 8;

		while (!self->zones_done) {
			if (wl_display_roundtrip(self->display) < 0)
				return FALSE;
			if (++attempts >= max_attempts)
				return FALSE;
		}
	}

	if (zones_out != NULL)
		*zones_out = (PortalZone *)self->zones->data;
	if (n_zones_out != NULL)
		*n_zones_out = self->zones->len;
	if (zone_set_out != NULL)
		*zone_set_out = self->zone_set;
	return TRUE;
}

void
portal_wayland_add_barrier(PortalWayland *self, guint32 id,
                           int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
	zgowl_input_capture_v1_add_barrier(self->capture, id, x1, y1, x2, y2);
}

void
portal_wayland_set_barriers(PortalWayland *self, guint32 zone_set)
{
	zgowl_input_capture_v1_set_barriers(self->capture, zone_set);
	wl_display_roundtrip(self->display);
}

void
portal_wayland_enable(PortalWayland *self)
{
	zgowl_input_capture_v1_enable(self->capture);
	wl_display_flush(self->display);
}

void
portal_wayland_disable(PortalWayland *self)
{
	zgowl_input_capture_v1_disable(self->capture);
	wl_display_flush(self->display);
}

void
portal_wayland_release(PortalWayland *self, guint32 activation_id,
                       gboolean has_position, double x, double y)
{
	zgowl_input_capture_v1_release(self->capture, activation_id,
		has_position ? 1 : 0,
		wl_fixed_from_double(x), wl_fixed_from_double(y));
	wl_display_flush(self->display);
}

void
portal_wayland_inject_rel_motion(PortalWayland *self, double dx, double dy)
{
	zgowl_input_inject_v1_pointer_motion(self->inject,
		wl_fixed_from_double(dx), wl_fixed_from_double(dy));
}

void
portal_wayland_inject_abs_motion(PortalWayland *self, double nx, double ny)
{
	zgowl_input_inject_v1_pointer_motion_absolute(self->inject,
		wl_fixed_from_double(nx), wl_fixed_from_double(ny));
}

void
portal_wayland_inject_button(PortalWayland *self, uint32_t button,
                             gboolean pressed)
{
	zgowl_input_inject_v1_button(self->inject, button, pressed ? 1 : 0);
}

void
portal_wayland_inject_axis(PortalWayland *self, uint32_t axis, double value)
{
	zgowl_input_inject_v1_axis(self->inject, axis,
		wl_fixed_from_double(value));
}

void
portal_wayland_inject_key(PortalWayland *self, uint32_t keycode,
                          gboolean pressed)
{
	zgowl_input_inject_v1_key(self->inject, keycode, pressed ? 1 : 0);
}

void
portal_wayland_inject_frame(PortalWayland *self)
{
	zgowl_input_inject_v1_frame(self->inject);
	wl_display_flush(self->display);
}
