/* test-interactive-resize.c -- a pointer resize is paced to the client
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Dragging a window's edge used to send the client a configure on every
 * pointer motion event -- up to a thousand a second -- and a second one
 * on every commit the client made, because mid-drag its committed size
 * always trails the newest one.  No client lays itself out a thousand
 * times a second, so it fell further behind with every event and the
 * resize lagged however fast the compositor was.
 *
 * Now the newest size waits until the client has committed the last one
 * and goes out at most once per output frame (resize_flush()).  This
 * proves it with a real Wayland client that can HOLD its acks: while it
 * holds, fifty motion events produce one configure, not fifty; when it
 * answers, the next (newest) size follows; and letting go sends the
 * final size at once, whatever is still held back.  A pointer MOVE
 * sends no configure at all.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/edges.h>

#include "xdg-shell-client-protocol.h"

#include "gowl.h"
#include "core/gowl-core-private.h"

/* ── A client that can hold its acks ───────────────────────────── */

typedef struct {
	const gchar *socket;

	struct wl_display    *display;
	struct wl_registry   *registry;
	struct wl_compositor *compositor;
	struct wl_shm        *shm;
	struct xdg_wm_base   *wm_base;
	struct wl_surface    *surface;
	struct xdg_surface   *xdg_surface;
	struct xdg_toplevel  *toplevel;

	/* Shared with the test, under @lock. */
	GMutex   lock;
	gboolean hold;          /* do not ack configures, just count them */
	gint     configures;    /* xdg_surface.configure events seen */
	guint32  held_serial;   /* the newest unacked one */
	gint     held_w;        /* the size it carried */
	gint     held_h;
	gint     want_w;        /* toplevel.configure's size, pending */
	gint     want_h;
	gboolean answer;        /* ack + commit the held one, then clear */
	gint     mapped;
	gint     stop;
} Client;

static struct wl_buffer *
make_buffer(Client *c, gint w, gint h)
{
	struct wl_shm_pool *pool;
	struct wl_buffer   *buffer;
	g_autofree gchar   *path = NULL;
	gsize               size;
	gint                fd;
	void               *data;

	w = MAX(1, w);
	h = MAX(1, h);
	size = (gsize)w * (gsize)h * 4;
	fd = g_file_open_tmp("gowl-resize-XXXXXX", &path, NULL);
	g_assert_cmpint(fd, >=, 0);
	g_unlink(path);
	g_assert_cmpint(ftruncate(fd, (off_t)size), ==, 0);
	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	g_assert_true(data != MAP_FAILED);
	memset(data, 0xFF, size);
	pool = wl_shm_create_pool(c->shm, fd, (int32_t)size);
	buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4,
	                                   WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	munmap(data, size);
	return buffer;
}

/* Ack @serial and commit a buffer of @w x @h: what a real client does
 * once it has laid itself out at the new size. */
static void
ack_and_draw(Client *c, guint32 serial, gint w, gint h)
{
	struct wl_buffer *buffer;

	xdg_surface_ack_configure(c->xdg_surface, serial);
	buffer = make_buffer(c, w > 0 ? w : 200, h > 0 ? h : 150);
	wl_surface_attach(c->surface, buffer, 0, 0);
	wl_surface_damage_buffer(c->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(c->surface);
}

static void
on_toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w,
                      int32_t h, struct wl_array *states)
{
	Client *c = data;

	(void)t;
	(void)states;
	g_mutex_lock(&c->lock);
	c->want_w = w;
	c->want_h = h;
	g_mutex_unlock(&c->lock);
}

static void
on_toplevel_close(void *data, struct xdg_toplevel *t)
{
	(void)data;
	(void)t;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = on_toplevel_configure, .close = on_toplevel_close,
};

static void
on_surface_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
	Client  *c = data;
	gboolean hold;
	gint     w, h;

	(void)s;
	g_mutex_lock(&c->lock);
	c->configures++;
	hold = c->hold;
	w = c->want_w;
	h = c->want_h;
	if (hold) {
		c->held_serial = serial;
		c->held_w = w;
		c->held_h = h;
	}
	g_mutex_unlock(&c->lock);
	if (!hold) {
		ack_and_draw(c, serial, w, h);
		g_atomic_int_set(&c->mapped, 1);
	}
}

static const struct xdg_surface_listener surface_listener = {
	.configure = on_surface_configure,
};

static void
on_ping(void *data, struct xdg_wm_base *b, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = on_ping,
};

static void
on_global(void *data, struct wl_registry *reg, uint32_t name,
          const char *iface, uint32_t version)
{
	Client *c = data;

	(void)version;
	if (g_strcmp0(iface, wl_compositor_interface.name) == 0)
		c->compositor = wl_registry_bind(reg, name,
		                                 &wl_compositor_interface, 4);
	else if (g_strcmp0(iface, wl_shm_interface.name) == 0)
		c->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (g_strcmp0(iface, xdg_wm_base_interface.name) == 0)
		c->wm_base = wl_registry_bind(reg, name,
		                              &xdg_wm_base_interface, 1);
}

static void
on_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data;
	(void)reg;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = on_global, .global_remove = on_global_remove,
};

static gpointer
client_thread(gpointer data)
{
	Client *c = data;

	c->display = wl_display_connect(c->socket);
	if (c->display == NULL) {
		g_atomic_int_set(&c->stop, 1);
		return NULL;
	}
	c->registry = wl_display_get_registry(c->display);
	wl_registry_add_listener(c->registry, &registry_listener, c);
	wl_display_roundtrip(c->display);
	if (c->compositor == NULL || c->shm == NULL || c->wm_base == NULL) {
		g_atomic_int_set(&c->stop, 1);
		wl_display_disconnect(c->display);
		return NULL;
	}
	xdg_wm_base_add_listener(c->wm_base, &wm_base_listener, c);
	c->surface = wl_compositor_create_surface(c->compositor);
	c->xdg_surface = xdg_wm_base_get_xdg_surface(c->wm_base, c->surface);
	xdg_surface_add_listener(c->xdg_surface, &surface_listener, c);
	c->toplevel = xdg_surface_get_toplevel(c->xdg_surface);
	xdg_toplevel_add_listener(c->toplevel, &toplevel_listener, c);
	xdg_toplevel_set_title(c->toplevel, "resize-me");
	wl_surface_commit(c->surface);

	while (!g_atomic_int_get(&c->stop)) {
		gboolean answer;
		guint32  serial;
		gint     w, h;

		g_mutex_lock(&c->lock);
		answer = c->answer;
		serial = c->held_serial;
		w = c->held_w;
		h = c->held_h;
		if (answer) {
			c->answer = FALSE;
			c->held_serial = 0;
		}
		g_mutex_unlock(&c->lock);
		if (answer && serial != 0)
			ack_and_draw(c, serial, w, h);

		wl_display_flush(c->display);
		g_usleep(1000);
		if (wl_display_prepare_read(c->display) == 0) {
			wl_display_read_events(c->display);
			wl_display_dispatch_pending(c->display);
		} else {
			wl_display_dispatch_pending(c->display);
		}
	}
	wl_display_disconnect(c->display);
	return NULL;
}

/* ── The rig ──────────────────────────────────────────────────── */

typedef struct {
	gchar          *runtime;
	GowlConfig     *config;
	GowlModuleManager *modules;
	GowlCompositor *compositor;
	Client          client;
	GThread        *thread;
	GowlClient     *win;
} Rig;

static void
pump(Rig *r, gint ms)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;

	do {
		wl_event_loop_dispatch(
			wl_display_get_event_loop(r->compositor->wl_display), 0);
		wl_display_flush_clients(r->compositor->wl_display);
		g_usleep(500);
	} while (g_get_monotonic_time() < deadline);
}

static gint
configures(Rig *r)
{
	gint n;

	g_mutex_lock(&r->client.lock);
	n = r->client.configures;
	g_mutex_unlock(&r->client.lock);
	return n;
}

static gboolean
rig_up(Rig *r)
{
	GError *error = NULL;
	gint64  deadline;
	GList  *l;

	memset(r, 0, sizeof(*r));
	r->runtime = g_build_filename(g_get_tmp_dir(), "gowl-resize-XXXXXX",
	                              NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	r->config = gowl_config_new();
	gowl_config_set_lock_command(r->config, "");
	r->modules = gowl_module_manager_new();
	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_clear_error(&error);
		return FALSE;
	}
	wlr_seat_set_capabilities(r->compositor->wlr_seat,
	                          WL_SEAT_CAPABILITY_POINTER);

	g_mutex_init(&r->client.lock);
	r->client.socket = gowl_compositor_get_socket_name(r->compositor);
	r->thread = g_thread_new("wl-client", client_thread, &r->client);

	deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
	while (g_get_monotonic_time() < deadline && r->win == NULL) {
		pump(r, 10);
		if (g_atomic_int_get(&r->client.stop))
			return FALSE;
		for (l = gowl_compositor_get_clients(r->compositor); l; l = l->next)
			if (g_strcmp0(gowl_client_get_title(l->data),
			              "resize-me") == 0
			    && gowl_client_get_wlr_surface(l->data) != NULL)
				r->win = l->data;
	}
	if (r->win == NULL)
		return FALSE;
	/* Floating, at a known box, and every configure of the map
	 * answered before the drag starts. */
	gowl_compositor_set_client_floating(r->compositor, r->win, TRUE);
	gowl_compositor_resize_client(r->compositor, r->win,
	                              100, 100, 400, 300);
	pump(r, 100);
	return TRUE;
}

static void
rig_down(Rig *r)
{
	if (r->thread != NULL) {
		g_atomic_int_set(&r->client.stop, 1);
		g_thread_join(r->thread);
	}
	if (r->compositor != NULL)
		pump(r, 20);
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	g_rmdir(r->runtime);
	g_free(r->runtime);
	g_mutex_clear(&r->client.lock);
}

/* ── The cases ────────────────────────────────────────────────── */

static void
test_resize_waits_for_the_client(void)
{
	Rig  r;
	gint before, i, w;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor or client");
		rig_down(&r);
		return;
	}

	/* Grab the bottom-right corner and hold every ack from here on. */
	gowl_compositor_inject_pointer_warp(r.compositor, 499, 399);
	pump(&r, 20);
	g_assert_true(gowl_compositor_begin_resize(r.compositor, r.win,
	                                           WLR_EDGE_RIGHT
	                                           | WLR_EDGE_BOTTOM));
	g_mutex_lock(&r.client.lock);
	r.client.hold = TRUE;
	g_mutex_unlock(&r.client.lock);
	before = configures(&r);

	/* Fifty motion events, as fast as a gaming mouse sends them. */
	for (i = 1; i <= 50; i++) {
		gowl_compositor_inject_pointer_warp(r.compositor, 499 + i * 4,
		                                    399 + i * 2);
		pump(&r, 1);
	}
	pump(&r, 100);
	/* One went out; the rest waited for the client.  (Two would still
	 * be pacing; fifty is the flood this replaced.) */
	g_assert_cmpint(configures(&r) - before, >=, 1);
	g_assert_cmpint(configures(&r) - before, <=, 2);
	/* The frame moved with what was sent, not ahead of it. */
	w = r.win->geom.width;
	g_assert_cmpint(w, <, 400 + 50 * 4);

	/* The client answers: the NEWEST size follows. */
	g_mutex_lock(&r.client.lock);
	r.client.answer = TRUE;
	g_mutex_unlock(&r.client.lock);
	before = configures(&r);
	pump(&r, 100);
	g_assert_cmpint(configures(&r) - before, ==, 1);
	g_assert_cmpint(r.win->geom.width, ==, 400 + 50 * 4);
	g_assert_cmpint(r.win->geom.height, ==, 300 + 50 * 2);

	/* Moved again and let go before the client answers: the release
	 * sends the final size at once. */
	gowl_compositor_inject_pointer_warp(r.compositor, 499 + 300, 399 + 150);
	pump(&r, 5);
	gowl_compositor_inject_button(r.compositor, BTN_LEFT, FALSE);
	pump(&r, 20);
	g_assert_cmpint(r.compositor->cursor_mode, ==, GOWL_CURSOR_MODE_NORMAL);
	g_assert_cmpint(r.win->geom.width, ==, 400 + 300);
	g_assert_cmpint(r.win->geom.height, ==, 300 + 150);

	rig_down(&r);
}

/* A client that never answers cannot freeze the frame: past the ack
 * timeout the next size goes anyway. */
static void
test_a_silent_client_does_not_freeze_the_resize(void)
{
	Rig  r;
	gint before;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor or client");
		rig_down(&r);
		return;
	}
	gowl_compositor_inject_pointer_warp(r.compositor, 499, 399);
	pump(&r, 20);
	g_assert_true(gowl_compositor_begin_resize(r.compositor, r.win,
	                                           WLR_EDGE_RIGHT
	                                           | WLR_EDGE_BOTTOM));
	g_mutex_lock(&r.client.lock);
	r.client.hold = TRUE;
	g_mutex_unlock(&r.client.lock);
	before = configures(&r);

	gowl_compositor_inject_pointer_warp(r.compositor, 520, 410);
	pump(&r, 20);
	gowl_compositor_inject_pointer_warp(r.compositor, 560, 430);
	/* Longer than the ack timeout, with output frames arriving. */
	pump(&r, 400);
	g_assert_cmpint(configures(&r) - before, ==, 2);
	g_assert_cmpint(r.win->geom.width, ==, 400 + 61);
	gowl_compositor_inject_button(r.compositor, BTN_LEFT, FALSE);
	pump(&r, 20);
	rig_down(&r);
}

/* Moving a window tells the client nothing. */
static void
test_a_move_sends_no_configure(void)
{
	Rig  r;
	gint before, i;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor or client");
		rig_down(&r);
		return;
	}
	gowl_compositor_inject_pointer_warp(r.compositor, 200, 150);
	pump(&r, 20);
	g_assert_true(gowl_compositor_begin_move(r.compositor, r.win));
	before = configures(&r);
	for (i = 1; i <= 30; i++) {
		gowl_compositor_inject_pointer_warp(r.compositor, 200 + i * 5,
		                                    150 + i * 3);
		pump(&r, 1);
	}
	pump(&r, 50);
	g_assert_cmpint(configures(&r) - before, ==, 0);
	g_assert_cmpint(r.win->geom.x, ==, 100 + 150);
	g_assert_cmpint(r.win->geom.width, ==, 400);
	gowl_compositor_inject_button(r.compositor, BTN_LEFT, FALSE);
	pump(&r, 20);
	rig_down(&r);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/interactive-resize/waits-for-the-client",
	                test_resize_waits_for_the_client);
	g_test_add_func("/interactive-resize/silent-client-does-not-freeze",
	                test_a_silent_client_does_not_freeze_the_resize);
	g_test_add_func("/interactive-resize/move-sends-no-configure",
	                test_a_move_sends_no_configure);
	return g_test_run();
}
