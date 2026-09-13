/* test-lock-input.c -- a lock screen you can type into
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * gowl implemented ext-session-lock-v1 on the server side, but its
 * keyboard path treated "the session is locked" as "the built-in
 * screenlock module wants this key" and returned without forwarding
 * anything to the seat.  A lock CLIENT -- gowl-lock, swaylock, anything
 * -- therefore held the session, drew its password prompt, and never
 * received a single character.  There was no way out but a VT switch,
 * and nothing in the tree noticed, because the only lock that had ever
 * been used was the module that did not need the seat.
 *
 * So: a real headless compositor, and a real Wayland client on another
 * thread that takes the lock the way a lock program does -- binds the
 * manager, locks, puts a surface on the output, acks the configure and
 * commits a buffer of exactly the size it was given.  Then a key is
 * injected, and the client has to see it.
 *
 * A buffer really is required: wlroots refuses a lock surface committed
 * at the wrong size, which is the protocol making sure a lock screen
 * cannot be a hole.  Committing one here is also what makes this a test
 * of the whole path rather than of a listener.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>

#include "ext-session-lock-v1-client-protocol.h"

#include "gowl.h"
#include "core/gowl-core-private.h"

/* ── The client ──────────────────────────────────────────────────── */

typedef struct {
	const gchar *socket;

	struct wl_display    *display;
	struct wl_registry   *registry;
	struct wl_compositor *compositor;
	struct wl_shm        *shm;
	struct wl_seat       *seat;
	struct wl_keyboard   *keyboard;
	struct wl_output     *output;
	struct ext_session_lock_manager_v1 *manager;
	struct ext_session_lock_v1         *lock;
	struct wl_surface                  *surface;
	struct ext_session_lock_surface_v1 *lock_surface;

	/* Read by the test thread. */
	gint locked;       /* the compositor confirmed the lock */
	gint keys;         /* key events received */
	gint entered;      /* keyboard focus arrived */
	gint stop;
} Client;

static struct wl_buffer *
make_buffer(Client *c, gint w, gint h)
{
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	g_autofree gchar *path = NULL;
	gint fd;
	gsize size = (gsize)w * h * 4;
	void *data;

	fd = g_file_open_tmp("gowl-lock-input-XXXXXX", &path, NULL);
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

static void
ls_configure(void *data, struct ext_session_lock_surface_v1 *s,
             uint32_t serial, uint32_t width, uint32_t height)
{
	Client *c = (Client *)data;
	struct wl_buffer *buffer;

	ext_session_lock_surface_v1_ack_configure(s, serial);
	buffer = make_buffer(c, (gint)width, (gint)height);
	wl_surface_attach(c->surface, buffer, 0, 0);
	wl_surface_damage_buffer(c->surface, 0, 0, (gint)width, (gint)height);
	wl_surface_commit(c->surface);
}

static const struct ext_session_lock_surface_v1_listener ls_listener = {
	.configure = ls_configure,
};

static void
lock_locked(void *data, struct ext_session_lock_v1 *lock)
{
	g_atomic_int_set(&((Client *)data)->locked, 1);
}

static void
lock_finished(void *data, struct ext_session_lock_v1 *lock)
{
	g_atomic_int_set(&((Client *)data)->stop, 1);
}

static const struct ext_session_lock_v1_listener lock_listener = {
	.locked   = lock_locked,
	.finished = lock_finished,
};

static void
kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd,
          uint32_t size)
{
	close(fd);
}

static void
kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
         struct wl_surface *surface, struct wl_array *keys)
{
	g_atomic_int_set(&((Client *)data)->entered, 1);
}

static void
kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
         struct wl_surface *surface)
{
}

static void
kb_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time,
       uint32_t key, uint32_t state)
{
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
		g_atomic_int_inc(&((Client *)data)->keys);
}

static void
kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
             uint32_t dep, uint32_t lat, uint32_t lck, uint32_t grp)
{
}

static void
kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
               int32_t delay)
{
}

static const struct wl_keyboard_listener kb_listener = {
	.keymap      = kb_keymap,
	.enter       = kb_enter,
	.leave       = kb_leave,
	.key         = kb_key,
	.modifiers   = kb_modifiers,
	.repeat_info = kb_repeat_info,
};

static void
seat_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
	Client *c = (Client *)data;

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && c->keyboard == NULL) {
		c->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(c->keyboard, &kb_listener, c);
	}
}

static void
seat_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps,
	.name         = seat_name,
};

static void
registry_global(void *data, struct wl_registry *r, uint32_t name,
                const char *iface, uint32_t version)
{
	Client *c = (Client *)data;

	if (g_strcmp0(iface, wl_compositor_interface.name) == 0)
		c->compositor = wl_registry_bind(r, name, &wl_compositor_interface,
		                                 MIN(version, 4u));
	else if (g_strcmp0(iface, wl_shm_interface.name) == 0)
		c->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (g_strcmp0(iface, wl_output_interface.name) == 0
	         && c->output == NULL)
		c->output = wl_registry_bind(r, name, &wl_output_interface,
		                             MIN(version, 3u));
	else if (g_strcmp0(iface, wl_seat_interface.name) == 0
	         && c->seat == NULL) {
		c->seat = wl_registry_bind(r, name, &wl_seat_interface,
		                           MIN(version, 7u));
		wl_seat_add_listener(c->seat, &seat_listener, c);
	} else if (g_strcmp0(iface,
	                     ext_session_lock_manager_v1_interface.name) == 0)
		c->manager = wl_registry_bind(r, name,
			&ext_session_lock_manager_v1_interface, 1);
}

static void
registry_global_remove(void *data, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global        = registry_global,
	.global_remove = registry_global_remove,
};

/* The client's whole life, on its own thread: connect, lock, and then
 * turn the connection until the test says stop. */
static gpointer
client_thread(gpointer data)
{
	Client *c = (Client *)data;

	c->display = wl_display_connect(c->socket);
	if (c->display == NULL)
		return NULL;
	c->registry = wl_display_get_registry(c->display);
	wl_registry_add_listener(c->registry, &registry_listener, c);
	wl_display_roundtrip(c->display);
	wl_display_roundtrip(c->display);

	if (c->manager == NULL || c->compositor == NULL || c->shm == NULL
	    || c->output == NULL) {
		g_atomic_int_set(&c->stop, 1);
		return NULL;
	}

	c->lock = ext_session_lock_manager_v1_lock(c->manager);
	ext_session_lock_v1_add_listener(c->lock, &lock_listener, c);

	c->surface = wl_compositor_create_surface(c->compositor);
	c->lock_surface = ext_session_lock_v1_get_lock_surface(
		c->lock, c->surface, c->output);
	ext_session_lock_surface_v1_add_listener(c->lock_surface, &ls_listener,
	                                          c);
	wl_display_flush(c->display);

	/* Polled with a timeout rather than a blocking dispatch: the test
	 * ends by setting `stop', and a blocking dispatch would sit in
	 * read() waiting for an event that is never coming. */
	while (!g_atomic_int_get(&c->stop)) {
		struct pollfd pfd;

		while (wl_display_prepare_read(c->display) != 0) {
			if (wl_display_dispatch_pending(c->display) < 0)
				goto out;
		}
		if (wl_display_flush(c->display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(c->display);
			goto out;
		}
		pfd.fd = wl_display_get_fd(c->display);
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN) != 0)
			wl_display_read_events(c->display);
		else
			wl_display_cancel_read(c->display);
		if (wl_display_dispatch_pending(c->display) < 0)
			goto out;
	}
out:

	if (c->lock != NULL)
		ext_session_lock_v1_destroy(c->lock);
	wl_display_disconnect(c->display);
	c->display = NULL;
	return NULL;
}

/* ── The compositor ──────────────────────────────────────────────── */

typedef struct {
	gchar             *runtime;
	gchar             *parent;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
} Rig;

static gboolean
rig_up(Rig *r)
{
	const gchar *parent;
	GError      *error = NULL;

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	r->parent = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	parent = r->parent;
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-lock-input-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	r->config = gowl_config_new();
	/* No lock program: this test IS the lock client, and a second one
	 * starting would be refused and muddy the result. */
	gowl_config_set_lock_command(r->config, "");
	r->modules = gowl_module_manager_new();
	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_test_skip("no headless compositor here");
		g_clear_error(&error);
		return FALSE;
	}

	/*
	 * Say the seat has a keyboard.
	 *
	 * gowl advertises the capability only once a real input device has
	 * been added to its keyboard group, and a headless backend has
	 * none -- so without this the client is told there is no keyboard,
	 * never binds one, and could not receive a key however correct the
	 * compositor is.  The seat's keyboard itself (the group's) was set
	 * at startup and is the one the injection path uses; this is only
	 * the announcement, standing in for the keyboard every real session
	 * has.
	 */
	wlr_seat_set_capabilities(r->compositor->wlr_seat,
	                          WL_SEAT_CAPABILITY_KEYBOARD);
	return TRUE;
}

static void
rig_down(Rig *r)
{
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	if (r->runtime != NULL)
		g_rmdir(r->runtime);
	g_free(r->runtime);
	if (r->parent != NULL)
		g_setenv("XDG_RUNTIME_DIR", r->parent, TRUE);
	g_free(r->parent);
	memset(r, 0, sizeof(*r));
}

/* Turn the compositor's loop until @flag is set, for at most a second. */
static gboolean
await(Rig *r, gint *flag)
{
	gint64 deadline = g_get_monotonic_time() + G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline) {
		wl_event_loop_dispatch(r->compositor->event_loop, 5);
		wl_display_flush_clients(r->compositor->wl_display);
		if (g_atomic_int_get(flag) != 0)
			return TRUE;
	}
	return FALSE;
}

static void
pump(Rig *r, gint ms)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < deadline) {
		wl_event_loop_dispatch(r->compositor->event_loop, 5);
		wl_display_flush_clients(r->compositor->wl_display);
	}
}

/* ── The case ────────────────────────────────────────────────────── */

static void
lock_client_receives_keys(void)
{
	Rig      r;
	Client   c;
	GThread *thread;
	gint     before;

	if (!rig_up(&r))
		return;

	memset(&c, 0, sizeof c);
	c.socket = r.compositor->socket_name;
	thread = g_thread_new("lock-client", client_thread, &c);

	if (!await(&r, &c.locked)) {
		g_atomic_int_set(&c.stop, 1);
		g_thread_join(thread);
		g_test_skip("the lock client never got the session");
		rig_down(&r);
		return;
	}
	g_assert_true(gowl_compositor_is_locked(r.compositor));

	/* The lock surface is given keyboard focus when it is created. */
	g_assert_true(await(&r, &c.entered));

	/* A key, while locked.  This is the line the fix is about: the
	 * locked branch used to hand every key to the built-in module and
	 * return, so a lock CLIENT saw nothing at all. */
	before = g_atomic_int_get(&c.keys);
	gowl_compositor_inject_key(r.compositor, KEY_A, TRUE);
	gowl_compositor_inject_key(r.compositor, KEY_A, FALSE);
	pump(&r, 200);

	g_atomic_int_set(&c.stop, 1);
	/* One more turn so the client's dispatch wakes and sees the flag. */
	pump(&r, 100);
	g_thread_join(thread);

	g_assert_cmpint(g_atomic_int_get(&c.keys), >, before);
	rig_down(&r);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/lock/client-receives-keys", lock_client_receives_keys);
	return g_test_run();
}
