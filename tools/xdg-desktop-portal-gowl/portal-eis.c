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

#include "portal-eis.h"

#include <glib-unix.h>
#include <libeis.h>
#include <time.h>

/*
 * The EIS server.  deskflow connects as a receiver libei client; this
 * server (the "sender" half) creates a virtual pointer+keyboard device
 * and streams the compositor-captured input to it.
 *
 * The libeis fd is watched on the GLib main loop (g_unix_fd_add); on
 * readiness we eis_dispatch() and drain eis_get_event() to drive the
 * client/seat/device lifecycle.  Captured events arrive via the
 * portal_eis_* entry points (called from the Wayland protocol handlers)
 * and are forwarded onto the live device.
 */

struct _PortalEis {
	struct eis        *ctx;
	guint              fd_source;     /* g_unix_fd_add id */

	struct eis_client *client;        /* the connected receiver client */
	struct eis_seat   *seat;
	struct eis_device *device;        /* pointer+keyboard virtual device */

	gboolean           device_ready;  /* resumed + emulating */
	guint32            sequence;
};

static uint64_t
now_usec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

/* Tear down the per-client device/seat state (client gone or device
 * closed). */
static void
drop_device(PortalEis *self)
{
	if (self->device != NULL) {
		eis_device_unref(self->device);
		self->device = NULL;
	}
	if (self->seat != NULL) {
		eis_seat_unref(self->seat);
		self->seat = NULL;
	}
	self->device_ready = FALSE;
}

/* Create the pointer+keyboard device on a freshly-bound seat. */
static void
create_device(PortalEis *self)
{
	struct eis_device *dev;

	if (self->seat == NULL)
		return;

	dev = eis_seat_new_device(self->seat);
	eis_device_configure_name(dev, "gowl captured input");
	eis_device_configure_type(dev, EIS_DEVICE_TYPE_VIRTUAL);
	eis_device_configure_capability(dev, EIS_DEVICE_CAP_POINTER);
	eis_device_configure_capability(dev, EIS_DEVICE_CAP_BUTTON);
	eis_device_configure_capability(dev, EIS_DEVICE_CAP_SCROLL);
	eis_device_configure_capability(dev, EIS_DEVICE_CAP_KEYBOARD);
	eis_device_add(dev);
	eis_device_resume(dev);

	self->device = dev;
}

static void
handle_event(PortalEis *self, struct eis_event *event)
{
	enum eis_event_type type = eis_event_get_type(event);

	switch (type) {
	case EIS_EVENT_CLIENT_CONNECT: {
		struct eis_client *client = eis_event_get_client(event);

		/* deskflow's InputCapture path is a receiver; reject senders. */
		if (eis_client_is_sender(client)) {
			eis_client_disconnect(client);
			break;
		}
		if (self->client != NULL) {
			/* One client at a time for a session. */
			eis_client_disconnect(client);
			break;
		}
		self->client = client;
		eis_client_connect(client);

		self->seat = eis_client_new_seat(client, "gowl-capture");
		eis_seat_configure_capability(self->seat,
			EIS_DEVICE_CAP_POINTER);
		eis_seat_configure_capability(self->seat,
			EIS_DEVICE_CAP_BUTTON);
		eis_seat_configure_capability(self->seat,
			EIS_DEVICE_CAP_SCROLL);
		eis_seat_configure_capability(self->seat,
			EIS_DEVICE_CAP_KEYBOARD);
		eis_seat_add(self->seat);
		break;
	}

	case EIS_EVENT_CLIENT_DISCONNECT:
		drop_device(self);
		self->client = NULL;
		break;

	case EIS_EVENT_SEAT_BIND:
		/* The client bound a capability on our seat: (re)create the
		 * device so it can receive events.  Only one device needed. */
		if (self->device == NULL)
			create_device(self);
		break;

	case EIS_EVENT_DEVICE_CLOSED:
		drop_device(self);
		break;

	default:
		break;
	}

	eis_event_unref(event);
}

static gboolean
on_eis_fd(gint fd, GIOCondition cond, gpointer data)
{
	PortalEis *self = data;
	struct eis_event *event;

	(void)fd;
	(void)cond;

	eis_dispatch(self->ctx);
	while ((event = eis_get_event(self->ctx)) != NULL)
		handle_event(self, event);

	return G_SOURCE_CONTINUE;
}

PortalEis *
portal_eis_new(void)
{
	PortalEis *self;
	int fd;

	self = g_new0(PortalEis, 1);
	self->ctx = eis_new(self);
	if (self->ctx == NULL) {
		g_free(self);
		return NULL;
	}

	if (eis_setup_backend_fd(self->ctx) != 0) {
		eis_unref(self->ctx);
		g_free(self);
		return NULL;
	}

	fd = eis_get_fd(self->ctx);
	self->fd_source = g_unix_fd_add(fd, G_IO_IN, on_eis_fd, self);

	return self;
}

void
portal_eis_free(PortalEis *self)
{
	if (self == NULL)
		return;

	drop_device(self);
	if (self->fd_source != 0)
		g_source_remove(self->fd_source);
	if (self->ctx != NULL)
		eis_unref(self->ctx);
	g_free(self);
}

int
portal_eis_connect_fd(PortalEis *self)
{
	g_return_val_if_fail(self != NULL, -1);

	return eis_backend_fd_add_client(self->ctx);
}

void
portal_eis_start(PortalEis *self)
{
	if (self == NULL || self->device == NULL)
		return;

	self->sequence++;
	eis_device_start_emulating(self->device, self->sequence);
	self->device_ready = TRUE;
}

void
portal_eis_stop(PortalEis *self)
{
	if (self == NULL || self->device == NULL || !self->device_ready)
		return;

	eis_device_stop_emulating(self->device);
	self->device_ready = FALSE;
}

void
portal_eis_rel_motion(PortalEis *self, double dx, double dy)
{
	if (self == NULL || self->device == NULL || !self->device_ready)
		return;
	eis_device_pointer_motion(self->device, dx, dy);
}

void
portal_eis_button(PortalEis *self, uint32_t button, bool pressed)
{
	if (self == NULL || self->device == NULL || !self->device_ready)
		return;
	eis_device_button_button(self->device, button, pressed);
}

void
portal_eis_scroll(PortalEis *self, uint32_t axis, double value)
{
	if (self == NULL || self->device == NULL || !self->device_ready)
		return;
	/* axis 0 == vertical, 1 == horizontal. */
	if (axis == 1)
		eis_device_scroll_delta(self->device, value, 0.0);
	else
		eis_device_scroll_delta(self->device, 0.0, value);
}

void
portal_eis_key(PortalEis *self, uint32_t keycode, bool pressed)
{
	if (self == NULL || self->device == NULL || !self->device_ready)
		return;
	eis_device_keyboard_key(self->device, keycode, pressed);
}

void
portal_eis_modifiers(PortalEis *self, uint32_t depressed, uint32_t latched,
                     uint32_t locked, uint32_t group)
{
	if (self == NULL || self->device == NULL || !self->device_ready)
		return;
	eis_device_keyboard_send_xkb_modifiers(self->device, depressed,
		latched, locked, group);
}

void
portal_eis_frame(PortalEis *self)
{
	if (self == NULL || self->device == NULL || !self->device_ready)
		return;
	eis_device_frame(self->device, now_usec());
}
