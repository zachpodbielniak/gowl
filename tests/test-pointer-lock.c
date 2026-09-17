/* test-pointer-lock.c -- a locked pointer, from the client's side
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * test-pointer-constraints checks the managers exist and that the
 * helpers behave with nothing constrained.  It says so itself:
 * "exercising an actual lock needs a client that binds the protocol and
 * creates a constraint on a surface it owns, which is a Wayland client
 * rather than a unit test."  So the lock had never been exercised, and
 * the injection path did not honour it at all.
 *
 * A locked pointer is the whole of a game's mouselook: the cursor must
 * not move, and the relative stream is the ONLY input the client gets.
 * Get either half wrong and the symptom is the same pair -- a cursor
 * that wanders off the window, and a camera that spins, because the
 * client is left deriving its own deltas from a pointer nobody is
 * holding still.
 *
 * This is a real client: it binds the protocols, maps an xdg toplevel,
 * takes the lock on pointer enter, and counts what arrives.
 */

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
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"

#include "gowl.h"
#include "core/gowl-core-private.h"

#define SURFACE_W (200)
#define SURFACE_H (150)

/* Where the client says its own cursor is, surface-local. */
#define HINT_X (120)
#define HINT_Y (90)

typedef struct {
	const gchar *socket;

	struct wl_display    *display;
	struct wl_registry   *registry;
	struct wl_compositor *compositor;
	struct wl_shm        *shm;
	struct wl_seat       *seat;
	struct wl_pointer    *pointer;
	struct xdg_wm_base   *wm_base;
	struct wl_surface    *surface;
	struct xdg_surface   *xdg_surface;
	struct xdg_toplevel  *toplevel;

	struct zwp_pointer_constraints_v1      *constraints;
	struct zwp_locked_pointer_v1           *lock;
	struct zwp_relative_pointer_manager_v1 *relative_mgr;
	struct zwp_relative_pointer_v1         *relative;

	/* Read by the test thread. */
	gint entered;
	gint locked;
	gint rel_events;
	gint rel_dx;
	gint unlock_now;   /* the test asks the client to drop the lock */
	gint unlocked;     /* and it has */
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

	fd = g_file_open_tmp("gowl-pointer-lock-XXXXXX", &path, NULL);
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
rel_motion(void *data, struct zwp_relative_pointer_v1 *rel,
           uint32_t hi, uint32_t lo, wl_fixed_t dx, wl_fixed_t dy,
           wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel)
{
	Client *c = data;

	g_atomic_int_inc(&c->rel_events);
	g_atomic_int_add(&c->rel_dx, (gint)wl_fixed_to_double(dx));
}

static const struct zwp_relative_pointer_v1_listener rel_listener = {
	.relative_motion = rel_motion,
};

static void
lock_locked(void *data, struct zwp_locked_pointer_v1 *lock)
{
	g_atomic_int_set(&((Client *)data)->locked, 1);
}

static void
lock_unlocked(void *data, struct zwp_locked_pointer_v1 *lock)
{
	g_atomic_int_set(&((Client *)data)->locked, 0);
}

static const struct zwp_locked_pointer_v1_listener lock_listener = {
	.locked   = lock_locked,
	.unlocked = lock_unlocked,
};

/* Take the lock the moment the pointer is over us, as a game does on
 * the click that starts a mouselook. */
static void
pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
              struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
	Client *c = data;

	if (c->lock != NULL || c->constraints == NULL)
		return;
	c->lock = zwp_pointer_constraints_v1_lock_pointer(
		c->constraints, c->surface, c->pointer, NULL,
		ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
	zwp_locked_pointer_v1_add_listener(c->lock, &lock_listener, c);
	/* Where this client draws its own cursor, which is the whole
	 * point of the hint: it is not where the pointer froze. */
	zwp_locked_pointer_v1_set_cursor_position_hint(
		c->lock, wl_fixed_from_int(HINT_X),
		wl_fixed_from_int(HINT_Y));
	wl_surface_commit(c->surface);
	wl_display_flush(c->display);
	g_atomic_int_set(&c->entered, 1);
}

static void pointer_leave(void *d, struct wl_pointer *p, uint32_t s,
                          struct wl_surface *sf) { }
static void pointer_motion(void *d, struct wl_pointer *p, uint32_t t,
                           wl_fixed_t x, wl_fixed_t y) { }
static void pointer_button(void *d, struct wl_pointer *p, uint32_t s,
                           uint32_t t, uint32_t b, uint32_t st) { }
static void pointer_axis(void *d, struct wl_pointer *p, uint32_t t,
                         uint32_t a, wl_fixed_t v) { }

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter, .leave = pointer_leave,
	.motion = pointer_motion, .button = pointer_button,
	.axis = pointer_axis,
};

static void
xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
	Client *c = data;
	struct wl_buffer *buffer;

	xdg_surface_ack_configure(s, serial);
	buffer = make_buffer(c, SURFACE_W, SURFACE_H);
	wl_surface_attach(c->surface, buffer, 0, 0);
	wl_surface_damage_buffer(c->surface, 0, 0, SURFACE_W, SURFACE_H);
	wl_surface_commit(c->surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void xdg_top_configure(void *d, struct xdg_toplevel *t, int32_t w,
                              int32_t h, struct wl_array *st) { }
static void xdg_top_close(void *d, struct xdg_toplevel *t) { }

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_top_configure, .close = xdg_top_close,
};

static void
wm_base_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
	xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void
registry_global(void *data, struct wl_registry *reg, uint32_t name,
                const char *iface, uint32_t version)
{
	Client *c = data;

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
	else if (g_strcmp0(iface,
	                   zwp_pointer_constraints_v1_interface.name) == 0)
		c->constraints = wl_registry_bind(reg, name,
			&zwp_pointer_constraints_v1_interface, 1);
	else if (g_strcmp0(iface,
	                   zwp_relative_pointer_manager_v1_interface.name) == 0)
		c->relative_mgr = wl_registry_bind(reg, name,
			&zwp_relative_pointer_manager_v1_interface, 1);
}

static void registry_remove(void *d, struct wl_registry *r, uint32_t n) { }

static const struct wl_registry_listener registry_listener = {
	.global = registry_global, .global_remove = registry_remove,
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

	if (c->compositor == NULL || c->shm == NULL || c->seat == NULL
	    || c->wm_base == NULL || c->constraints == NULL
	    || c->relative_mgr == NULL) {
		g_atomic_int_set(&c->stop, 1);
		return NULL;
	}
	xdg_wm_base_add_listener(c->wm_base, &wm_base_listener, c);

	c->pointer = wl_seat_get_pointer(c->seat);
	wl_pointer_add_listener(c->pointer, &pointer_listener, c);
	c->relative = zwp_relative_pointer_manager_v1_get_relative_pointer(
		c->relative_mgr, c->pointer);
	zwp_relative_pointer_v1_add_listener(c->relative, &rel_listener, c);

	c->surface = wl_compositor_create_surface(c->compositor);
	c->xdg_surface = xdg_wm_base_get_xdg_surface(c->wm_base, c->surface);
	xdg_surface_add_listener(c->xdg_surface, &xdg_surface_listener, c);
	c->toplevel = xdg_surface_get_toplevel(c->xdg_surface);
	xdg_toplevel_add_listener(c->toplevel, &xdg_toplevel_listener, c);
	xdg_toplevel_set_title(c->toplevel, "pointer-lock");
	wl_surface_commit(c->surface);
	wl_display_roundtrip(c->display);

	while (!g_atomic_int_get(&c->stop)) {
		if (g_atomic_int_get(&c->unlock_now) == 1 && c->lock != NULL) {
			g_atomic_int_set(&c->unlock_now, 2);
			zwp_locked_pointer_v1_destroy(c->lock);
			c->lock = NULL;
			wl_surface_commit(c->surface);
			wl_display_flush(c->display);
			g_atomic_int_set(&c->unlocked, 1);
		}
		wl_display_dispatch_pending(c->display);
		wl_display_flush(c->display);
		g_usleep(2000);
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

typedef struct {
	gchar             *parent;
	gchar             *runtime;
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
	r->runtime = g_build_filename(parent, "gowl-pointer-lock-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	r->config = gowl_config_new();
	r->modules = gowl_module_manager_new();
	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_test_skip("no headless compositor here");
		g_clear_error(&error);
		return FALSE;
	}

	/* A headless backend has no input devices, so the seat would tell
	 * the client there is no pointer and it could never bind one.  Only
	 * the announcement is stood in for; the cursor and every motion
	 * path below are the compositor's own. */
	wlr_seat_set_capabilities(r->compositor->wlr_seat,
	                          WL_SEAT_CAPABILITY_POINTER);
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

static void
pump(Rig *r, gint ms)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < deadline) {
		wl_event_loop_dispatch(
			wl_display_get_event_loop(r->compositor->wl_display), 0);
		wl_display_flush_clients(r->compositor->wl_display);
		g_usleep(1000);
	}
}

static gboolean
await(Rig *r, gint *flag)
{
	gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline) {
		if (g_atomic_int_get(flag))
			return TRUE;
		pump(r, 10);
	}
	return g_atomic_int_get(flag) != 0;
}

static void
locked_pointer_holds_still_and_still_reports(void)
{
	Rig      r;
	Client   c;
	GThread *thread;
	gdouble  x, y;
	gint     rel_before;

	if (!rig_up(&r))
		return;

	memset(&c, 0, sizeof c);
	c.socket = r.compositor->socket_name;
	thread = g_thread_new("pointer-lock-client", client_thread, &c);
	pump(&r, 400);

	/* Put the cursor on the client's window.  Injected, because it is a
	 * public entry point and the one that runs motionnotify. */
	gowl_compositor_inject_pointer_motion(r.compositor, 5, 5);
	pump(&r, 200);

	if (!await(&r, &c.entered) || !await(&r, &c.locked)) {
		g_atomic_int_set(&c.stop, 1);
		pump(&r, 200);
		g_thread_join(thread);
		g_test_skip("the client never took the lock here");
		rig_down(&r);
		return;
	}
	g_assert_true(gowl_compositor_pointer_is_locked(r.compositor));

	x = r.compositor->wlr_cursor->x;
	y = r.compositor->wlr_cursor->y;
	rel_before = g_atomic_int_get(&c.rel_events);

	/*
	 * Motion, while locked.  Both halves are asserted because both were
	 * wrong here and each alone reads as a different bug: a cursor that
	 * keeps moving wanders out of the window, and a relative stream
	 * that never arrives leaves the client deriving its own deltas from
	 * a pointer nobody is holding still.
	 */
	gowl_compositor_inject_pointer_motion(r.compositor, 40, 25);
	pump(&r, 300);

	g_assert_cmpfloat(r.compositor->wlr_cursor->x, ==, x);
	g_assert_cmpfloat(r.compositor->wlr_cursor->y, ==, y);
	g_assert_cmpint(g_atomic_int_get(&c.rel_events), >, rel_before);
	g_assert_cmpint(g_atomic_int_get(&c.rel_dx), >, 0);

	g_atomic_int_set(&c.stop, 1);
	pump(&r, 200);
	g_thread_join(thread);
	rig_down(&r);
}

/*
 * When the lock ends, the cursor goes where the client said it was.
 *
 * A locked pointer does not move, so at unlock the cursor sits wherever
 * it froze -- while the client has spent the lock drawing its own
 * cursor somewhere else and saying where with
 * set_cursor_position_hint.  gowl never read the hint, so every
 * mouselook ended with the pointer back at the point the look began.
 * A game does that several times a second.
 */
static void
unlocking_puts_the_cursor_where_the_client_says(void)
{
	Rig      r;
	Client   c;
	GThread *thread;
	gint     sx, sy;
	GowlClient *client;

	if (!rig_up(&r))
		return;

	memset(&c, 0, sizeof c);
	c.socket = r.compositor->socket_name;
	thread = g_thread_new("pointer-hint-client", client_thread, &c);
	pump(&r, 400);

	gowl_compositor_inject_pointer_motion(r.compositor, 5, 5);
	pump(&r, 200);

	if (!await(&r, &c.entered) || !await(&r, &c.locked)) {
		g_atomic_int_set(&c.stop, 1);
		pump(&r, 200);
		g_thread_join(thread);
		g_test_skip("the client never took the lock here");
		rig_down(&r);
		return;
	}

	/* Where the surface actually is, so the hint can be checked in
	 * layout coordinates rather than assumed. */
	client = r.compositor->clients != NULL
		? (GowlClient *)r.compositor->clients->data : NULL;
	if (client == NULL || client->scene_surface == NULL
	    || !wlr_scene_node_coords(&client->scene_surface->node, &sx, &sy)) {
		g_atomic_int_set(&c.stop, 1);
		pump(&r, 200);
		g_thread_join(thread);
		g_test_skip("no placed surface to measure the hint against");
		rig_down(&r);
		return;
	}

	/* Move while locked, so the cursor and the client's idea of it
	 * have genuinely diverged before the lock ends. */
	gowl_compositor_inject_pointer_motion(r.compositor, 40, 25);
	pump(&r, 200);

	g_atomic_int_set(&c.unlock_now, 1);
	g_assert_true(await(&r, &c.unlocked));
	pump(&r, 300);
	/* A motion to make the compositor notice the constraint is gone. */
	gowl_compositor_inject_pointer_motion(r.compositor, 0, 0);
	pump(&r, 200);

	g_assert_false(gowl_compositor_pointer_is_locked(r.compositor));
	g_assert_cmpfloat(r.compositor->wlr_cursor->x, ==, (gdouble)(sx + HINT_X));
	g_assert_cmpfloat(r.compositor->wlr_cursor->y, ==, (gdouble)(sy + HINT_Y));

	g_atomic_int_set(&c.stop, 1);
	pump(&r, 200);
	g_thread_join(thread);
	rig_down(&r);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/pointer-lock/holds-still-and-still-reports",
	                locked_pointer_holds_still_and_still_reports);
	g_test_add_func("/pointer-lock/unlocking-honours-the-cursor-hint",
	                unlocking_puts_the_cursor_where_the_client_says);
	return g_test_run();
}
