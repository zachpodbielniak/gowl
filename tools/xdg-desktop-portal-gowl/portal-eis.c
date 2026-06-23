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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* memfd_create */
#endif

#include "portal-eis.h"

#include <glib-unix.h>
#include <libeis.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <xkbcommon/xkbcommon.h>

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

/* One capture zone (monitor) in layout coordinates; becomes an ei_region
 * on the device so the receiver client can derive its screen shape. */
struct portal_eis_zone {
	int32_t  x, y;
	uint32_t w, h;
};

struct _PortalEis {
	struct eis        *ctx;
	guint              fd_source;     /* g_unix_fd_add id */

	GArray            *zones;         /* struct portal_eis_zone */

	struct eis_client *client;        /* the connected receiver client */
	struct eis_seat   *seat;
	struct eis_device *device;        /* pointer+keyboard virtual device */

	/* deskflow connects as a libei RECEIVER context, so OUR EIS context is
	 * the SENDER: the server creates the device and SENDS events to the
	 * client (it is the server that calls eis_device_start_emulating(),
	 * choosing its own monotonic sequence -- see the libeis-receiver /
	 * libei-sender group docs).  A receiver client never emits
	 * EIS_EVENT_DEVICE_START_EMULATING, so the server must NOT wait for it.
	 *
	 * State:
	 *   - resumed: the device has been added + resumed (ready to emulate).
	 *   - emulating: we have an active start_emulating sequence in flight.
	 *   - capture_active: the portal Activated (a barrier was crossed).
	 * Events stream only when capture_active && emulating; emulation is
	 * (re)started when the portal activates and the device is resumed. */
	gboolean           resumed;
	gboolean           emulating;
	gboolean           capture_active;
	uint32_t           sequence;      /* server-chosen, +1 per start */
};

static uint64_t
now_usec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

/* Route libeis log messages to g_log so they land in the session log.
 * libeis reports protocol violations / disconnect reasons here, which is
 * the only way to see WHY a client (deskflow) is dropped. */
static void
eis_log(struct eis *eis, enum eis_log_priority priority,
        const char *message, struct eis_log_context *ctx)
{
	(void)eis;
	(void)ctx;
	if (priority >= EIS_LOG_PRIORITY_ERROR)
		g_warning("EIS: %s", message);
	else if (priority >= EIS_LOG_PRIORITY_WARNING)
		g_message("EIS: %s", message);
	else
		g_debug("EIS: %s", message);
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
	self->resumed = FALSE;
	self->emulating = FALSE;
}

/* Build a default XKB keymap and write it to a sealed memfd.  Returns the
 * fd (caller owns it) and stores the byte length in *size_out, or -1 on
 * failure.  A keyboard EIS device with no keymap makes the libei client
 * (deskflow) warn "does not have a keymap, we are guessing" and mis-map
 * keycodes; giving it a real keymap fixes keyboard forwarding. */
static int
make_keymap_fd(size_t *size_out)
{
	struct xkb_context *ctx;
	struct xkb_keymap  *keymap;
	char               *str;
	size_t              len;
	int                 fd;
	ssize_t             written;

	ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (ctx == NULL)
		return -1;

	/* NULL rule-names -> the system default layout (RMLVO from the
	 * environment or compile-time defaults). */
	keymap = xkb_keymap_new_from_names(ctx, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (keymap == NULL) {
		xkb_context_unref(ctx);
		return -1;
	}

	str = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	xkb_keymap_unref(keymap);
	xkb_context_unref(ctx);
	if (str == NULL)
		return -1;

	len = strlen(str) + 1;   /* include the NUL; eis maps the fd */
	fd = memfd_create("gowl-eis-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0) {
		free(str);
		return -1;
	}
	written = write(fd, str, len);
	free(str);
	if (written < 0 || (size_t)written != len) {
		close(fd);
		return -1;
	}

	*size_out = len;
	return fd;
}

/* Create the pointer+keyboard device on a freshly-bound seat. */
static void
create_device(PortalEis *self)
{
	struct eis_device *dev;
	int                keymap_fd;
	size_t             keymap_size = 0;

	if (self->seat == NULL)
		return;

	dev = eis_seat_new_device(self->seat);
	eis_device_configure_name(dev, "gowl captured input");
	eis_device_configure_type(dev, EIS_DEVICE_TYPE_VIRTUAL);
	eis_device_configure_capability(dev, EIS_DEVICE_CAP_POINTER);
	eis_device_configure_capability(dev, EIS_DEVICE_CAP_BUTTON);
	eis_device_configure_capability(dev, EIS_DEVICE_CAP_SCROLL);
	eis_device_configure_capability(dev, EIS_DEVICE_CAP_KEYBOARD);

	/* Attach a region per capture zone (must be done before
	 * eis_device_add).  The receiver client (deskflow) sums the device's
	 * ei_regions to derive its screen shape; with no region it sees a 1x1
	 * screen, the pointer-barrier crossing position normalises out of
	 * [0,1], getNeighbor() finds nothing in any direction and the cursor
	 * never crosses to the remote.  A device with the POINTER (relative)
	 * capability does not need a region for the wire protocol, but the
	 * client needs it for screen geometry -- so we always add one. */
	if (self->zones != NULL && self->zones->len > 0) {
		guint i;

		for (i = 0; i < self->zones->len; i++) {
			struct portal_eis_zone *z =
				&g_array_index(self->zones,
				               struct portal_eis_zone, i);
			struct eis_region *region;

			region = eis_device_new_region(dev);
			if (region == NULL)
				continue;
			/* eis offsets are unsigned; negative-origin monitors
			 * are clamped to 0 (the common single-monitor case is
			 * at the origin anyway). */
			eis_region_set_offset(region,
				z->x < 0 ? 0u : (uint32_t)z->x,
				z->y < 0 ? 0u : (uint32_t)z->y);
			eis_region_set_size(region, z->w, z->h);
			eis_region_add(region);
		}
	} else {
		g_warning("portal: no capture zones set; the EIS device has no "
			"region and the client will see a 1x1 screen (the "
			"pointer will not cross).  GetZones must run first.");
	}

	/* Attach an XKB keymap so the client maps keycodes correctly (must be
	 * done before eis_device_add).  deskflow warns "does not have a
	 * keymap, we are guessing" without it. */
	keymap_fd = make_keymap_fd(&keymap_size);
	if (keymap_fd >= 0) {
		struct eis_keymap *km;

		km = eis_device_new_keymap(dev, EIS_KEYMAP_TYPE_XKB,
			keymap_fd, keymap_size);
		if (km != NULL)
			eis_keymap_add(km);
		close(keymap_fd);   /* eis dup'd / mapped it */
	} else {
		g_warning("portal: failed to build EIS keymap; the client "
			"will guess keycodes");
	}

	eis_device_add(dev);
	eis_device_resume(dev);

	self->device = dev;
	self->resumed = TRUE;

	/* If the portal already activated capture (the barrier was crossed
	 * before the client finished binding), begin emulating right away so
	 * no events are lost. */
	if (self->capture_active && !self->emulating) {
		self->sequence++;
		eis_device_start_emulating(self->device, self->sequence);
		self->emulating = TRUE;
	}
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

	/* EIS_EVENT_DEVICE_START_EMULATING / _STOP_EMULATING are only
	 * generated on a *receiving* EIS context (i.e. when WE handle a
	 * sender client).  deskflow is a receiver client, so our context is
	 * the sender and these never fire -- the server itself drives
	 * eis_device_start_emulating() on portal activation (see
	 * portal_eis_start).  Nothing to do here. */

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
	self->zones = g_array_new(FALSE, FALSE, sizeof(struct portal_eis_zone));
	self->ctx = eis_new(self);
	if (self->ctx == NULL) {
		g_array_unref(self->zones);
		g_free(self);
		return NULL;
	}

	/* Surface libeis diagnostics (incl. disconnect reasons).  DEBUG-level
	 * when GOWL_PORTAL_EIS_DEBUG is set, else WARNING and above. */
	eis_log_set_handler(self->ctx, eis_log);
	eis_log_set_priority(self->ctx,
		g_getenv("GOWL_PORTAL_EIS_DEBUG") != NULL
		? EIS_LOG_PRIORITY_DEBUG : EIS_LOG_PRIORITY_WARNING);

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
	if (self->zones != NULL)
		g_array_unref(self->zones);
	g_free(self);
}

int
portal_eis_connect_fd(PortalEis *self)
{
	g_return_val_if_fail(self != NULL, -1);

	return eis_backend_fd_add_client(self->ctx);
}

void
portal_eis_clear_zones(PortalEis *self)
{
	if (self == NULL || self->zones == NULL)
		return;
	g_array_set_size(self->zones, 0);
}

void
portal_eis_add_zone(PortalEis *self, int32_t x, int32_t y,
                    uint32_t width, uint32_t height)
{
	struct portal_eis_zone z;

	if (self == NULL || self->zones == NULL)
		return;

	z.x = x;
	z.y = y;
	z.w = width;
	z.h = height;
	g_array_append_val(self->zones, z);
}

/* Events may flow only when the device is resumed and we have an active
 * emulation sequence (which the server starts on portal activation). */
static gboolean
streaming_ok(PortalEis *self)
{
	return self != NULL && self->device != NULL
	       && self->resumed && self->emulating;
}

void
portal_eis_start(PortalEis *self)
{
	if (self == NULL)
		return;

	/* Portal Activated (barrier crossed): WE are the sender, so the
	 * server starts emulation itself with a monotonic sequence and then
	 * streams events.  (A receiver client never asks us to start.)  If the
	 * device is not resumed yet, create_device() will start emulating once
	 * it is. */
	self->capture_active = TRUE;
	if (self->resumed && self->device != NULL && !self->emulating) {
		self->sequence++;
		eis_device_start_emulating(self->device, self->sequence);
		self->emulating = TRUE;
	}
}

void
portal_eis_stop(PortalEis *self)
{
	if (self == NULL)
		return;

	/* Portal Deactivated/Released: stop the emulation sequence. */
	if (self->emulating && self->device != NULL)
		eis_device_stop_emulating(self->device);
	self->emulating = FALSE;
	self->capture_active = FALSE;
}

void
portal_eis_rel_motion(PortalEis *self, double dx, double dy)
{
	if (!streaming_ok(self))
		return;
	eis_device_pointer_motion(self->device, dx, dy);
}

void
portal_eis_button(PortalEis *self, uint32_t button, bool pressed)
{
	if (!streaming_ok(self))
		return;
	eis_device_button_button(self->device, button, pressed);
}

void
portal_eis_scroll(PortalEis *self, uint32_t axis, double value)
{
	if (!streaming_ok(self))
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
	if (!streaming_ok(self))
		return;
	eis_device_keyboard_key(self->device, keycode, pressed);
}

void
portal_eis_modifiers(PortalEis *self, uint32_t depressed, uint32_t latched,
                     uint32_t locked, uint32_t group)
{
	if (!streaming_ok(self))
		return;
	eis_device_keyboard_send_xkb_modifiers(self->device, depressed,
		latched, locked, group);
}

void
portal_eis_frame(PortalEis *self)
{
	if (!streaming_ok(self))
		return;
	eis_device_frame(self->device, now_usec());
}
