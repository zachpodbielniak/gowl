/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * macro-test-client.h - A real Wayland client for the macro tests.
 *
 * Included (not linked) by test-macro-targeted-key.c and
 * test-macro-examples.c: a client thread that maps up to N_WINDOWS
 * toplevels with a wl_keyboard and records which surface every key
 * arrived on, and the headless rig around it.  Everything is static;
 * each test binary gets its own copy.  The includer needs
 * xdg-shell-client-protocol.h and wayland-client (see the Makefile).
 */

#ifndef MACRO_TEST_CLIENT_H
#define MACRO_TEST_CLIENT_H

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>

#include "xdg-shell-client-protocol.h"

#include "gowl.h"
#include "core/gowl-core-private.h"

#define SURFACE_W (160)
#define SURFACE_H (120)
#define N_WINDOWS (4)   /* the most a rig maps */
#define MAX_KEYS  (64)

/* ── The client ─────────────────────────────────────────────────── */

typedef struct {
	struct wl_surface   *surface;
	struct xdg_surface  *xdg_surface;
	struct xdg_toplevel *toplevel;
	const gchar         *title;
	const gchar         *app_id;
	gint                 configured;
} Window;

typedef struct {
	gint     window;   /* index into windows, -1 for none */
	guint32  key;
	guint32  state;
	guint32  mods;     /* depressed mask when it arrived */
} KeySeen;

typedef struct {
	const gchar *socket;

	struct wl_display    *display;
	struct wl_registry   *registry;
	struct wl_compositor *compositor;
	struct wl_shm        *shm;
	struct wl_seat       *seat;
	struct wl_keyboard   *keyboard;
	struct xdg_wm_base   *wm_base;
	Window                windows[N_WINDOWS];
	gint                  n_windows;

	/* Written by the client thread, read by the test under lock */
	GMutex   lock;
	gint     focused;            /* window with keyboard focus, -1 none */
	guint32  mods;
	KeySeen  keys[MAX_KEYS];
	gint     n_keys;
	gint     enters[N_WINDOWS];
	gint     stop;
	gint     ready;
} Client;

G_GNUC_UNUSED static struct wl_buffer *
make_buffer(
	Client *c,
	gint    w,
	gint    h
){
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	g_autofree gchar *path = NULL;
	gint fd;
	gsize size = (gsize)w * (gsize)h * 4;
	void *data;

	fd = g_file_open_tmp("gowl-macro-key-XXXXXX", &path, NULL);
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

G_GNUC_UNUSED static gint
window_of(
	Client            *c,
	struct wl_surface *surface
){
	gint i;

	for (i = 0; i < c->n_windows; i++)
		if (c->windows[i].surface == surface)
			return i;
	return -1;
}

G_GNUC_UNUSED static void
kb_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd,
          uint32_t size)
{
	(void)data; (void)k; (void)format; (void)size;
	close(fd);
}

G_GNUC_UNUSED static void
kb_enter(void *data, struct wl_keyboard *k, uint32_t serial,
         struct wl_surface *surface, struct wl_array *keys)
{
	Client *c = data;
	gint w;

	(void)k; (void)serial; (void)keys;
	g_mutex_lock(&c->lock);
	w = window_of(c, surface);
	c->focused = w;
	if (w >= 0)
		c->enters[w]++;
	g_mutex_unlock(&c->lock);
}

G_GNUC_UNUSED static void
kb_leave(void *data, struct wl_keyboard *k, uint32_t serial,
         struct wl_surface *surface)
{
	Client *c = data;

	(void)k; (void)serial; (void)surface;
	g_mutex_lock(&c->lock);
	c->focused = -1;
	g_mutex_unlock(&c->lock);
}

G_GNUC_UNUSED static void
kb_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time,
       uint32_t key, uint32_t state)
{
	Client *c = data;

	(void)k; (void)serial; (void)time;
	g_mutex_lock(&c->lock);
	if (c->n_keys < MAX_KEYS) {
		c->keys[c->n_keys].window = c->focused;
		c->keys[c->n_keys].key = key;
		c->keys[c->n_keys].state = state;
		c->keys[c->n_keys].mods = c->mods;
		c->n_keys++;
	}
	g_mutex_unlock(&c->lock);
}

G_GNUC_UNUSED static void
kb_modifiers(void *data, struct wl_keyboard *k, uint32_t serial,
             uint32_t depressed, uint32_t latched, uint32_t locked,
             uint32_t group)
{
	Client *c = data;

	(void)k; (void)serial; (void)latched; (void)locked; (void)group;
	g_mutex_lock(&c->lock);
	c->mods = depressed;
	g_mutex_unlock(&c->lock);
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_modifiers,
};

G_GNUC_UNUSED static void
xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
	Client *c = data;
	struct wl_buffer *buffer;
	gint i;

	xdg_surface_ack_configure(s, serial);
	for (i = 0; i < c->n_windows; i++) {
		if (c->windows[i].xdg_surface != s)
			continue;
		buffer = make_buffer(c, SURFACE_W, SURFACE_H);
		wl_surface_attach(c->windows[i].surface, buffer, 0, 0);
		wl_surface_damage_buffer(c->windows[i].surface, 0, 0,
		                         SURFACE_W, SURFACE_H);
		wl_surface_commit(c->windows[i].surface);
		g_atomic_int_set(&c->windows[i].configured, 1);
	}
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void xdg_top_configure(void *d, struct xdg_toplevel *t, int32_t w,
                              int32_t h, struct wl_array *st)
{ (void)d; (void)t; (void)w; (void)h; (void)st; }
static void xdg_top_close(void *d, struct xdg_toplevel *t)
{ (void)d; (void)t; }

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_top_configure, .close = xdg_top_close,
};

G_GNUC_UNUSED static void
wm_base_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
	(void)d;
	xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

G_GNUC_UNUSED static void
registry_global(void *data, struct wl_registry *reg, uint32_t name,
                const char *iface, uint32_t version)
{
	Client *c = data;

	(void)version;
	if (g_strcmp0(iface, wl_compositor_interface.name) == 0)
		c->compositor = wl_registry_bind(reg, name,
			&wl_compositor_interface, 4);
	else if (g_strcmp0(iface, wl_shm_interface.name) == 0)
		c->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (g_strcmp0(iface, wl_seat_interface.name) == 0)
		c->seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
	else if (g_strcmp0(iface, xdg_wm_base_interface.name) == 0)
		c->wm_base = wl_registry_bind(reg, name,
			&xdg_wm_base_interface, 1);
}

static void registry_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d; (void)r; (void)n; }

static const struct wl_registry_listener registry_listener = {
	.global = registry_global, .global_remove = registry_remove,
};

G_GNUC_UNUSED static gpointer
client_thread(gpointer data)
{
	Client *c = data;
	gint i;

	c->display = wl_display_connect(c->socket);
	if (c->display == NULL) {
		g_atomic_int_set(&c->stop, 1);
		return NULL;
	}
	c->registry = wl_display_get_registry(c->display);
	wl_registry_add_listener(c->registry, &registry_listener, c);
	wl_display_roundtrip(c->display);
	if (c->compositor == NULL || c->shm == NULL || c->seat == NULL
	    || c->wm_base == NULL) {
		g_atomic_int_set(&c->stop, 1);
		wl_display_disconnect(c->display);
		return NULL;
	}
	xdg_wm_base_add_listener(c->wm_base, &wm_base_listener, c);
	c->keyboard = wl_seat_get_keyboard(c->seat);
	wl_keyboard_add_listener(c->keyboard, &keyboard_listener, c);

	for (i = 0; i < c->n_windows; i++) {
		Window *w = &c->windows[i];

		w->surface = wl_compositor_create_surface(c->compositor);
		w->xdg_surface = xdg_wm_base_get_xdg_surface(c->wm_base, w->surface);
		xdg_surface_add_listener(w->xdg_surface, &xdg_surface_listener, c);
		w->toplevel = xdg_surface_get_toplevel(w->xdg_surface);
		xdg_toplevel_add_listener(w->toplevel, &xdg_toplevel_listener, c);
		xdg_toplevel_set_title(w->toplevel, w->title);
		xdg_toplevel_set_app_id(w->toplevel,
		                        w->app_id != NULL ? w->app_id : w->title);
		wl_surface_commit(w->surface);
	}
	wl_display_roundtrip(c->display);
	g_atomic_int_set(&c->ready, 1);

	while (!g_atomic_int_get(&c->stop)) {
		wl_display_dispatch_pending(c->display);
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

/* ── The rig ────────────────────────────────────────────────────── */

typedef struct {
	gchar             *runtime;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
	Client             client;
	GThread           *thread;
	GowlClient        *win[N_WINDOWS];   /* compositor-side, by index */
	gint               n;
} Rig;

G_GNUC_UNUSED static void
pump(
	Rig  *r,
	gint  ms
){
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < deadline) {
		wl_event_loop_dispatch(
			wl_display_get_event_loop(r->compositor->wl_display), 0);
		wl_display_flush_clients(r->compositor->wl_display);
		g_usleep(1000);
	}
}

G_GNUC_UNUSED static GowlClient *
client_titled(
	Rig         *r,
	const gchar *title
){
	GList *l;

	for (l = gowl_compositor_get_clients(r->compositor); l; l = l->next)
		if (g_strcmp0(gowl_client_get_title(l->data), title) == 0)
			return l->data;
	return NULL;
}

/*
 * A headless compositor, @modules (module .so paths, NULL-terminated,
 * or NULL) loaded and
 * started with it, and a client mapping one toplevel per @titles entry
 * (NULL-terminated, at most N_WINDOWS), with app-ids from @app_ids when
 * given.  FALSE (and the test skipped) when any of it cannot be had.
 */
G_GNUC_UNUSED static gboolean
rig_up(
	Rig                 *r,
	const gchar * const *titles,
	const gchar * const *app_ids,
	const gchar * const *modules
){
	const gchar *parent;
	GError *error = NULL;
	gint64 deadline;
	gint i;

	memset(r, 0, sizeof *r);
	parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-macro-key-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	r->config = gowl_config_new();
	gowl_config_set_lock_command(r->config, "");
	r->modules = gowl_module_manager_new();
	for (i = 0; modules != NULL && modules[i] != NULL; i++) {
		if (!gowl_module_manager_load_module(r->modules, modules[i],
		                                     &error)) {
			g_test_skip("a module did not load");
			g_clear_error(&error);
			return FALSE;
		}
	}
	gowl_module_manager_activate_all(r->modules);
	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_test_skip("no headless compositor here");
		g_clear_error(&error);
		return FALSE;
	}
	gowl_module_manager_dispatch_startup(r->modules, r->compositor);
	/* No input devices on a headless backend: announce a keyboard so
	   the client can bind one.  Keys come from the compositor's group. */
	wlr_seat_set_capabilities(r->compositor->wlr_seat,
	                          WL_SEAT_CAPABILITY_KEYBOARD);

	g_mutex_init(&r->client.lock);
	r->client.focused = -1;
	r->client.socket = gowl_compositor_get_socket_name(r->compositor);
	for (i = 0; titles[i] != NULL && i < N_WINDOWS; i++) {
		r->client.windows[i].title = titles[i];
		r->client.windows[i].app_id = app_ids != NULL ? app_ids[i] : NULL;
	}
	r->client.n_windows = r->n = i;
	r->thread = g_thread_new("wl-client", client_thread, &r->client);

	/* All mapped and known to the compositor */
	deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
	while (g_get_monotonic_time() < deadline) {
		gint mapped = 0;

		pump(r, 10);
		if (g_atomic_int_get(&r->client.stop))
			break;
		for (i = 0; i < r->n; i++) {
			r->win[i] = client_titled(r, r->client.windows[i].title);
			if (r->win[i] != NULL
			    && gowl_client_get_wlr_surface(r->win[i]) != NULL)
				mapped++;
		}
		if (mapped == r->n)
			break;
	}
	for (i = 0; i < r->n; i++) {
		if (r->win[i] == NULL) {
			g_test_skip("the test client's windows never mapped");
			return FALSE;
		}
	}
	return TRUE;
}

G_GNUC_UNUSED static void
rig_down(Rig *r)
{
	if (r->thread != NULL) {
		g_atomic_int_set(&r->client.stop, 1);
		g_thread_join(r->thread);
		r->thread = NULL;
	}
	if (r->compositor != NULL)
		pump(r, 20);
	if (r->compositor != NULL && r->modules != NULL)
		gowl_module_manager_dispatch_shutdown(r->modules, r->compositor);
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	if (r->runtime != NULL)
		g_rmdir(r->runtime);
	g_free(r->runtime);
	g_mutex_clear(&r->client.lock);
}

/* Focus @i the ordinary way and wait for the client to see it. */
G_GNUC_UNUSED static void
focus_window(
	Rig  *r,
	gint  i
){
	gint64 deadline;

	gowl_compositor_focus_client(r->compositor, r->win[i], FALSE);
	deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
	while (g_get_monotonic_time() < deadline) {
		gint f;

		pump(r, 5);
		g_mutex_lock(&r->client.lock);
		f = r->client.focused;
		g_mutex_unlock(&r->client.lock);
		if (f == i)
			return;
	}
	g_assert_not_reached();
}

G_GNUC_UNUSED static void
reset_keys(Rig *r)
{
	g_mutex_lock(&r->client.lock);
	r->client.n_keys = 0;
	g_mutex_unlock(&r->client.lock);
}

/* Waits for @n key events at the client. */
G_GNUC_UNUSED static gint
await_keys(
	Rig  *r,
	gint  n
){
	gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
	gint have = 0;

	while (g_get_monotonic_time() < deadline) {
		pump(r, 5);
		g_mutex_lock(&r->client.lock);
		have = r->client.n_keys;
		g_mutex_unlock(&r->client.lock);
		if (have >= n)
			break;
	}
	pump(r, 30);    /* and nothing more arrives */
	g_mutex_lock(&r->client.lock);
	have = r->client.n_keys;
	g_mutex_unlock(&r->client.lock);
	return have;
}

G_GNUC_UNUSED static gint
focused_now(Rig *r)
{
	gint f;

	pump(r, 20);
	g_mutex_lock(&r->client.lock);
	f = r->client.focused;
	g_mutex_unlock(&r->client.lock);
	return f;
}

#endif /* MACRO_TEST_CLIENT_H */
