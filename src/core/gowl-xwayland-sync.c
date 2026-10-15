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
 * gowl-xwayland-sync.c - Knowing when Xwayland has caught up.
 *
 * Putting a key into an X11 window that does not have focus cannot be
 * done the Wayland way.  Xwayland hands every key it receives to the
 * window holding X INPUT focus, whatever wl_surface the seat entered, so
 * the X focus has to move to the target first (the xwm's activate) and
 * back afterwards.  Measured against a real Xwayland, both halves race:
 *
 *   - the xwm's SetInputFocus travels on the xwm's X connection, the key
 *     on Xwayland's Wayland socket, and Xwayland services the two in no
 *     particular order.  Activate-then-send lands the key on the OLD
 *     window about as often as not;
 *   - the same holds for moving focus back: restore too early and the
 *     key that was still queued goes to the window being returned to.
 *
 * So each half is confirmed rather than assumed, over an X connection
 * of our own:
 *
 *   - wait_focus: GetInputFocus until the server reports the target (or
 *     a window inside it -- a client using WM_TAKE_FOCUS, wine for one,
 *     may focus a child of its toplevel);
 *   - wait_key: an XI2 raw key event on the root window.  The server
 *     emits it when it PROCESSES the key, whichever window then gets
 *     the core event, so seeing it means the key has landed and focus
 *     may move.
 *
 * Without xcb-xinput (GOWL_HAVE_XWAYLAND_SYNC unset) wait_key degrades
 * to a fixed settle delay: correct on an idle machine, a race on a busy
 * one.  wait_focus needs only core xcb and always works.
 *
 * Everything here blocks the calling thread for at most the timeout it
 * is given, and only ever waits on the X server: answering GetInputFocus
 * or emitting a raw event needs nothing from the compositor, so calling
 * this from the compositor thread cannot deadlock -- at worst a wedged
 * Xwayland costs each call its timeout, which the caller bounds.
 */

#include "gowl-xwayland-sync.h"

#include <gio/gio.h>

#ifdef GOWL_HAVE_XWAYLAND

#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#ifdef GOWL_HAVE_XWAYLAND_SYNC
#include <xcb/xinput.h>
#endif

/* How deep a focus window may sit below the toplevel it belongs to.  A
 * wine focus proxy is one level down; anything past this is not ours. */
#define GOWL_XSYNC_MAX_DEPTH (16)

/* The fallback when the server cannot tell us a key was processed. */
#define GOWL_XSYNC_SETTLE_US (8000)

struct _GowlXwaylandSync {
	xcb_connection_t *conn;
	xcb_window_t      root;
	gboolean          raw;      /* XI2 raw key events selected */
};

static gint64
deadline_after(guint timeout_ms)
{
	return g_get_monotonic_time() + (gint64)timeout_ms * 1000;
}

/* Waits for the connection to become readable, at most until @deadline.
 * Returns FALSE on timeout or a dead connection. */
static gboolean
wait_readable(
	GowlXwaylandSync *self,
	gint64            deadline
){
	struct pollfd pfd;
	gint64 left;
	gint rc;

	left = deadline - g_get_monotonic_time();
	if (left <= 0)
		return FALSE;
	pfd.fd = xcb_get_file_descriptor(self->conn);
	pfd.events = POLLIN;
	pfd.revents = 0;
	rc = poll(&pfd, 1, (gint)((left + 999) / 1000));
	return rc > 0 && (pfd.revents & POLLIN) != 0;
}

/* The reply for @sequence, or NULL on timeout or error.  Events that
 * arrive meanwhile stay queued for xcb_poll_for_event(). */
static void *
reply_before(
	GowlXwaylandSync *self,
	guint             sequence,
	gint64            deadline
){
	xcb_flush(self->conn);
	for (;;) {
		void *reply = NULL;
		xcb_generic_error_t *err = NULL;

		if (xcb_poll_for_reply(self->conn, sequence, &reply, &err)) {
			free(err);
			return reply;
		}
		if (xcb_connection_has_error(self->conn))
			return NULL;
		if (!wait_readable(self, deadline))
			return NULL;
	}
}

/**
 * gowl_xwayland_sync_new: (skip)
 * @display_name: Xwayland's display, e.g. ":1"
 * @error: return location for a #GError
 *
 * Connects to Xwayland and, where the server and the build allow,
 * selects XI2 raw key events on the root window.  The connection is
 * made once, on first use; Xwayland answers it without the compositor's
 * help, so this is safe on the compositor thread.
 *
 * Returns: (transfer full) (nullable): the sync, %NULL on error
 */
GowlXwaylandSync *
gowl_xwayland_sync_new(
	const gchar  *display_name,
	GError      **error
){
	GowlXwaylandSync *self;
	const xcb_setup_t *setup;
	gint screen_nr;

	g_return_val_if_fail(display_name != NULL, NULL);

	self = g_new0(GowlXwaylandSync, 1);
	screen_nr = 0;
	self->conn = xcb_connect(display_name, &screen_nr);
	if (self->conn == NULL || xcb_connection_has_error(self->conn)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "cannot connect to Xwayland on %s", display_name);
		gowl_xwayland_sync_free(self);
		return NULL;
	}
	setup = xcb_get_setup(self->conn);
	self->root = xcb_setup_roots_iterator(setup).data->root;

#ifdef GOWL_HAVE_XWAYLAND_SYNC
	{
		xcb_input_xi_query_version_reply_t *ver;
		gint64 deadline = deadline_after(500);

		ver = reply_before(self,
			xcb_input_xi_query_version(self->conn, 2, 2).sequence,
			deadline);
		if (ver != NULL && ver->major_version >= 2) {
			struct {
				xcb_input_event_mask_t head;
				uint32_t               mask;
			} em;

			em.head.deviceid = XCB_INPUT_DEVICE_ALL_MASTER;
			em.head.mask_len = 1;
			em.mask = XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS
			        | XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE;
			xcb_input_xi_select_events(self->conn, self->root, 1,
			                           &em.head);
			xcb_flush(self->conn);
			self->raw = TRUE;
		}
		free(ver);
	}
#endif
	if (!self->raw)
		g_info("xwayland-sync: no XI2 raw key events; X11 delivery "
		       "falls back to a %d ms settle delay",
		       GOWL_XSYNC_SETTLE_US / 1000);
	return self;
}

/**
 * gowl_xwayland_sync_free: (skip)
 * @self: (nullable): a #GowlXwaylandSync
 */
void
gowl_xwayland_sync_free(GowlXwaylandSync *self)
{
	if (self == NULL)
		return;
	if (self->conn != NULL)
		xcb_disconnect(self->conn);
	g_free(self);
}

/**
 * gowl_xwayland_sync_has_raw: (skip)
 * @self: a #GowlXwaylandSync
 *
 * Returns: %TRUE when key processing is confirmed by the server rather
 *   than assumed after a delay
 */
gboolean
gowl_xwayland_sync_has_raw(GowlXwaylandSync *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	return self->raw;
}

/**
 * gowl_xwayland_sync_get_fd: (skip)
 * @self: a #GowlXwaylandSync
 *
 * The connection's descriptor, for an event-loop source that drains it:
 * raw key events arrive for every key typed into any X11 window, and a
 * connection nobody reads makes the server buffer them without bound.
 *
 * Returns: the file descriptor
 */
gint
gowl_xwayland_sync_get_fd(GowlXwaylandSync *self)
{
	g_return_val_if_fail(self != NULL, -1);
	return xcb_get_file_descriptor(self->conn);
}

/**
 * gowl_xwayland_sync_is_alive: (skip)
 * @self: a #GowlXwaylandSync
 *
 * Returns: %FALSE once the server has gone (Xwayland exited or was
 *   restarted); the caller drops this one and connects afresh
 */
gboolean
gowl_xwayland_sync_is_alive(GowlXwaylandSync *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	return xcb_connection_has_error(self->conn) == 0;
}

/* Is @win @want, or below it?  Walks up with QueryTree. */
static gboolean
window_within(
	GowlXwaylandSync *self,
	xcb_window_t      win,
	xcb_window_t      want,
	gint64            deadline
){
	gint depth;

	for (depth = 0; depth < GOWL_XSYNC_MAX_DEPTH; depth++) {
		xcb_query_tree_reply_t *tree;
		xcb_window_t parent;

		if (win == want)
			return TRUE;
		if (win == XCB_WINDOW_NONE || win == self->root
		    || win == 1 /* PointerRoot */)
			return FALSE;
		tree = reply_before(self,
			xcb_query_tree(self->conn, win).sequence, deadline);
		if (tree == NULL)
			return FALSE;
		parent = tree->parent;
		free(tree);
		win = parent;
	}
	return FALSE;
}

/**
 * gowl_xwayland_sync_wait_focus: (skip)
 * @self: a #GowlXwaylandSync
 * @window: the X window that should have input focus
 * @timeout_ms: how long to wait at most
 *
 * Polls the server's input focus until it is @window or a window inside
 * it.  Each poll is one round trip; a client that takes focus itself
 * (WM_TAKE_FOCUS) is given the whole timeout to do it.
 *
 * Returns: %TRUE once the server agrees, %FALSE on timeout
 */
gboolean
gowl_xwayland_sync_wait_focus(
	GowlXwaylandSync *self,
	guint32           window,
	guint             timeout_ms
){
	gint64 deadline;

	g_return_val_if_fail(self != NULL, FALSE);

	deadline = deadline_after(timeout_ms);
	while (g_get_monotonic_time() < deadline) {
		xcb_get_input_focus_reply_t *focus;
		xcb_window_t win;

		focus = reply_before(self,
			xcb_get_input_focus(self->conn).sequence, deadline);
		if (focus == NULL)
			return FALSE;
		win = focus->focus;
		free(focus);
		if (window_within(self, win, (xcb_window_t)window, deadline))
			return TRUE;
		g_usleep(200);
	}
	return FALSE;
}

/**
 * gowl_xwayland_sync_drain: (skip)
 * @self: a #GowlXwaylandSync
 *
 * Throws away every event already received, so a later wait_key can
 * only be satisfied by a key sent after this call.
 */
void
gowl_xwayland_sync_drain(GowlXwaylandSync *self)
{
	xcb_generic_event_t *ev;

	g_return_if_fail(self != NULL);
	while ((ev = xcb_poll_for_event(self->conn)) != NULL)
		free(ev);
}

/**
 * gowl_xwayland_sync_key_is_down: (skip)
 * @self: a #GowlXwaylandSync
 * @evdev_keycode: a key, as the seat sends it (no +8)
 * @timeout_ms: how long to wait for the answer
 *
 * The X server's own idea of whether the key is down (QueryKeymap).
 * There is ONE key state per X server, shared by every X11 window: a
 * press of a key that is already down is dropped (the server takes it
 * for autorepeat), and a release of a key that is up goes nowhere.  So
 * putting a key into several X11 windows in turn has to know, before
 * each event, which of the two it is sending into.
 *
 * Returns: 1 when down, 0 when up, -1 when the server did not answer
 */
gint
gowl_xwayland_sync_key_is_down(
	GowlXwaylandSync *self,
	guint32           evdev_keycode,
	guint             timeout_ms
){
	xcb_query_keymap_reply_t *map;
	guint32 kc;
	gint down;

	g_return_val_if_fail(self != NULL, -1);

	map = reply_before(self, xcb_query_keymap(self->conn).sequence,
	                   deadline_after(timeout_ms));
	if (map == NULL)
		return -1;
	kc = evdev_keycode + 8;
	down = kc < 256 && (map->keys[kc / 8] & (1u << (kc % 8))) != 0;
	free(map);
	return down ? 1 : 0;
}

/**
 * gowl_xwayland_sync_wait_key: (skip)
 * @self: a #GowlXwaylandSync
 * @evdev_keycode: the key, as the seat sent it (no +8)
 * @pressed: %TRUE to wait for its press, %FALSE for its release
 * @timeout_ms: how long to wait at most
 *
 * Waits until the X server has processed the key.  The caller has
 * already flushed it to Xwayland (wl_display_flush_clients()).  An
 * autorepeat of the same key is not an answer: it can be generated while
 * the key that was just sent is still queued.
 *
 * Returns: %TRUE once processed; %FALSE on timeout.  Without XI2 this
 *   sleeps a fixed settle delay and returns %TRUE.
 */
gboolean
gowl_xwayland_sync_wait_key(
	GowlXwaylandSync *self,
	guint32           evdev_keycode,
	gboolean          pressed,
	guint             timeout_ms
){
#ifdef GOWL_HAVE_XWAYLAND_SYNC
	gint64 deadline;
	guint8 want_type;
#endif

	g_return_val_if_fail(self != NULL, FALSE);

	if (!self->raw) {
		g_usleep(GOWL_XSYNC_SETTLE_US);
		return TRUE;
	}
#ifdef GOWL_HAVE_XWAYLAND_SYNC
	want_type = pressed ? XCB_INPUT_RAW_KEY_PRESS : XCB_INPUT_RAW_KEY_RELEASE;
	deadline = deadline_after(timeout_ms);
	for (;;) {
		xcb_generic_event_t *ev;

		while ((ev = xcb_poll_for_event(self->conn)) != NULL) {
			gboolean hit = FALSE;

			if ((ev->response_type & 0x7f) == XCB_GE_GENERIC) {
				xcb_ge_generic_event_t *ge;

				ge = (xcb_ge_generic_event_t *)ev;
				if (ge->event_type == want_type) {
					xcb_input_raw_key_press_event_t *rk;

					rk = (xcb_input_raw_key_press_event_t *)ev;
					hit = rk->detail == evdev_keycode + 8
					      && (rk->flags
					          & XCB_INPUT_KEY_EVENT_FLAGS_KEY_REPEAT)
					         == 0;
				}
			}
			free(ev);
			if (hit)
				return TRUE;
		}
		if (xcb_connection_has_error(self->conn))
			return FALSE;
		if (!wait_readable(self, deadline))
			return FALSE;
	}
#else
	(void)evdev_keycode;
	(void)pressed;
	(void)timeout_ms;
	return TRUE;
#endif
}

#else /* !GOWL_HAVE_XWAYLAND */

/* No X11 here: nothing to synchronise with. */

GowlXwaylandSync *
gowl_xwayland_sync_new(
	const gchar  *display_name,
	GError      **error
){
	(void)display_name;
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
	            "built without XWayland");
	return NULL;
}

void
gowl_xwayland_sync_free(GowlXwaylandSync *self)
{
	(void)self;
}

gboolean
gowl_xwayland_sync_has_raw(GowlXwaylandSync *self)
{
	(void)self;
	return FALSE;
}

gint
gowl_xwayland_sync_get_fd(GowlXwaylandSync *self)
{
	(void)self;
	return -1;
}

gboolean
gowl_xwayland_sync_is_alive(GowlXwaylandSync *self)
{
	(void)self;
	return FALSE;
}

gboolean
gowl_xwayland_sync_wait_focus(
	GowlXwaylandSync *self,
	guint32           window,
	guint             timeout_ms
){
	(void)self;
	(void)window;
	(void)timeout_ms;
	return FALSE;
}

void
gowl_xwayland_sync_drain(GowlXwaylandSync *self)
{
	(void)self;
}

gint
gowl_xwayland_sync_key_is_down(
	GowlXwaylandSync *self,
	guint32           evdev_keycode,
	guint             timeout_ms
){
	(void)self;
	(void)evdev_keycode;
	(void)timeout_ms;
	return -1;
}

gboolean
gowl_xwayland_sync_wait_key(
	GowlXwaylandSync *self,
	guint32           evdev_keycode,
	gboolean          pressed,
	guint             timeout_ms
){
	(void)self;
	(void)evdev_keycode;
	(void)pressed;
	(void)timeout_ms;
	return FALSE;
}

#endif /* GOWL_HAVE_XWAYLAND */
