/* test-pointer-confine-live.c -- the compositor's pointer confinement,
 * against a real client
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * test-pointer-confine checks the rules on their own.  This puts them
 * in a headless gowl with real Wayland clients -- xdg toplevels that
 * bind relative-pointer and pointer-constraints, as a game does -- and
 * drives them through the compositor's own entry points: the action,
 * IPC, injected motion and keys, the lock, a tag switch, the
 * windowrules module.
 *
 * Every motion path is the compositor's: relative, absolute and the KVM
 * portal's layout warp all go through the same gate the physical
 * devices do, so a clamp seen here is a clamp a mouse gets.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>

#include "xdg-shell-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "core/gowl-monitor.h"
#include "config/gowl-keybind.h"

#define DEFAULT_W (200)
#define DEFAULT_H (150)

/* ── the client ─────────────────────────────────────────────────────
 *
 * One toplevel per client, each on its own connection and thread.  It
 * draws a buffer the size it is configured to, so the surface fills the
 * box the compositor holds the pointer in, and does what the test asks
 * through the cmd_* flags -- a lock, an unmap, a destroy -- on its own
 * thread, where its Wayland objects live.
 */
typedef struct {
	const gchar *socket;
	const gchar *app_id;

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

	gint want_w;        /* the size the last configure asked for */
	gint want_h;

	/* Read by the test thread. */
	gint entered;
	gint locked;
	gint rel_events;
	gint rel_dx;
	gint destroyed;

	/* Written by the test thread: 1 asks, the client sets 2 when done. */
	gint cmd_lock;
	gint cmd_unlock;
	gint cmd_unmap;
	gint cmd_destroy;
	gint stop;

	GThread *thread;
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

	fd = g_file_open_tmp("gowl-confine-XXXXXX", &path, NULL);
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

static void
pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
              struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
	g_atomic_int_set(&((Client *)data)->entered, 1);
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

/* Draw at the size asked for, so the surface fills its window. */
static void
xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
	Client *c = data;
	struct wl_buffer *buffer;
	gint w = c->want_w > 0 ? c->want_w : DEFAULT_W;
	gint h = c->want_h > 0 ? c->want_h : DEFAULT_H;

	xdg_surface_ack_configure(s, serial);
	if (g_atomic_int_get(&c->cmd_unmap) != 0)
		return;
	buffer = make_buffer(c, w, h);
	wl_surface_attach(c->surface, buffer, 0, 0);
	wl_surface_damage_buffer(c->surface, 0, 0, w, h);
	wl_surface_commit(c->surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void
xdg_top_configure(void *data, struct xdg_toplevel *t, int32_t w,
                  int32_t h, struct wl_array *st)
{
	Client *c = data;

	c->want_w = w;
	c->want_h = h;
}

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

/* Carry out whatever the test asked for, on this thread. */
static void
client_run_commands(Client *c)
{
	if (g_atomic_int_get(&c->cmd_lock) == 1 && c->lock == NULL) {
		g_atomic_int_set(&c->cmd_lock, 2);
		c->lock = zwp_pointer_constraints_v1_lock_pointer(
			c->constraints, c->surface, c->pointer, NULL,
			ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
		zwp_locked_pointer_v1_add_listener(c->lock, &lock_listener, c);
		wl_surface_commit(c->surface);
	}
	if (g_atomic_int_get(&c->cmd_unlock) == 1 && c->lock != NULL) {
		zwp_locked_pointer_v1_destroy(c->lock);
		c->lock = NULL;
		wl_surface_commit(c->surface);
		g_atomic_int_set(&c->cmd_unlock, 2);
	}
	/* Unmapping an xdg toplevel is committing no buffer. */
	if (g_atomic_int_get(&c->cmd_unmap) == 1) {
		wl_surface_attach(c->surface, NULL, 0, 0);
		wl_surface_commit(c->surface);
		g_atomic_int_set(&c->cmd_unmap, 2);
	}
	if (g_atomic_int_get(&c->cmd_destroy) == 1 && c->toplevel != NULL) {
		if (c->lock != NULL) {
			zwp_locked_pointer_v1_destroy(c->lock);
			c->lock = NULL;
		}
		xdg_toplevel_destroy(c->toplevel);
		xdg_surface_destroy(c->xdg_surface);
		wl_surface_destroy(c->surface);
		c->toplevel = NULL;
		c->xdg_surface = NULL;
		c->surface = NULL;
		g_atomic_int_set(&c->cmd_destroy, 2);
		g_atomic_int_set(&c->destroyed, 1);
	}
	wl_display_flush(c->display);
}

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
		wl_display_disconnect(c->display);
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
	xdg_toplevel_set_app_id(c->toplevel, c->app_id);
	xdg_toplevel_set_title(c->toplevel, c->app_id);
	wl_surface_commit(c->surface);
	wl_display_roundtrip(c->display);

	while (!g_atomic_int_get(&c->stop)) {
		client_run_commands(c);
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

/* ── the compositor ───────────────────────────────────────────────── */

typedef struct {
	gchar             *parent;
	gchar             *runtime;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;

	/* What pointer-confine-changed has said. */
	gint   ons;
	gint   offs;
	gchar *last_reason;
} Rig;

static void
on_confine_changed(GowlCompositor *comp, gboolean active,
                   GowlClient *client, const gchar *reason, gpointer data)
{
	Rig *r = data;

	if (active) {
		r->ons++;
		g_assert_nonnull(client);
	} else {
		r->offs++;
		g_assert_null(client);
	}
	g_free(r->last_reason);
	r->last_reason = g_strdup(reason);
}

/*
 * A headless gowl with one output.  @rule_app_id, when given, is a
 * `confine-pointer' window rule for that app-id, applied by the real
 * windowrules module -- which is then loaded, as cmacs and gowl load it.
 * @escape_bind, when TRUE, binds Super+Ctrl+Escape to
 * `toggle_pointer_confine on' as an ordinary config bind, so the test
 * can tell the escape apart from a bind on the same chord.
 */
static gboolean
rig_up(Rig *r, const gchar *rule_app_id, gboolean escape_bind)
{
	const gchar *parent;
	GError      *error = NULL;

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	r->parent = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	parent = r->parent;
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-confine-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	/* No animation module is loaded, so a move lands at once and the
	 * drawn frame and the geometry agree. */
	r->config = gowl_config_new();
	if (rule_app_id != NULL) {
		GowlRuleEntry rule;

		memset(&rule, 0, sizeof rule);
		rule.app_id = (gchar *)rule_app_id;
		rule.monitor = -1;
		rule.xwayland = -1;
		rule.match_floating = -1;
		rule.match_fullscreen = -1;
		rule.confine_pointer = TRUE;
		gowl_config_add_rule_entry(r->config, &rule);
	}
	if (escape_bind) {
		guint mods = 0, sym = 0;

		g_assert_true(gowl_keybind_parse("Super+Ctrl+Escape", &mods, &sym));
		gowl_config_add_keybind(r->config, mods, sym,
		                        GOWL_ACTION_TOGGLE_POINTER_CONFINE, "on");
	}

	r->modules = gowl_module_manager_new();
	if (rule_app_id != NULL) {
		g_autofree gchar *path = g_build_filename(GOWL_TEST_MODULE_DIR,
		                                          "windowrules.so", NULL);

		if (!gowl_module_manager_load_module(r->modules, path, &error))
			g_error("%s", error->message);
		gowl_module_manager_activate_all(r->modules);
	}

	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_test_skip("no headless compositor here");
		g_clear_error(&error);
		return FALSE;
	}
	gowl_module_manager_dispatch_startup(r->modules, r->compositor);

	/* A headless backend has no input devices, so the seat would tell
	 * the client there is no pointer.  Only the announcement is stood
	 * in for; the cursor, the keyboard group and every motion path are
	 * the compositor's own. */
	wlr_seat_set_capabilities(r->compositor->wlr_seat,
	                          WL_SEAT_CAPABILITY_POINTER
	                          | WL_SEAT_CAPABILITY_KEYBOARD);
	g_signal_connect(r->compositor, "pointer-confine-changed",
	                 G_CALLBACK(on_confine_changed), r);
	return TRUE;
}

static void
rig_down(Rig *r)
{
	if (r->modules != NULL && r->compositor != NULL)
		gowl_module_manager_dispatch_shutdown(r->modules, r->compositor);
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	if (r->runtime != NULL)
		g_rmdir(r->runtime);
	g_free(r->runtime);
	if (r->parent != NULL)
		g_setenv("XDG_RUNTIME_DIR", r->parent, TRUE);
	g_free(r->parent);
	g_free(r->last_reason);
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
await(Rig *r, gint *flag, gint value)
{
	gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline) {
		if (g_atomic_int_get(flag) == value)
			return TRUE;
		pump(r, 10);
	}
	return g_atomic_int_get(flag) == value;
}

/* The compositor's client for @app_id, once it is mapped. */
static GowlClient *
find_mapped(Rig *r, const gchar *app_id)
{
	GList *l;

	for (l = r->compositor->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if (g_strcmp0(c->app_id, app_id) == 0 && c->scene != NULL)
			return c;
	}
	return NULL;
}

static Client *
client_start(Rig *r, const gchar *app_id)
{
	Client *c = g_new0(Client, 1);

	c->socket = r->compositor->socket_name;
	c->app_id = app_id;
	c->thread = g_thread_new(app_id, client_thread, c);
	return c;
}

static GowlClient *
client_await_map(Rig *r, Client *c)
{
	gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline) {
		GowlClient *gc = find_mapped(r, c->app_id);

		if (gc != NULL)
			return gc;
		if (g_atomic_int_get(&c->stop))
			return NULL;
		pump(r, 10);
	}
	return NULL;
}

static void
client_stop(Rig *r, Client *c)
{
	if (c == NULL)
		return;
	g_atomic_int_set(&c->stop, 1);
	pump(r, 100);
	g_thread_join(c->thread);
	g_free(c);
}

/* Float @gc at @x,@y and @w x @h, and let the client draw at that size.
 * Through the compositor's own placement, the same as a layout. */
static void
place(Rig *r, GowlClient *gc, gint x, gint y, gint w, gint h)
{
	if (!gc->isfloating)
		gowl_client_set_floating(gc, TRUE);
	gowl_compositor_place_client(r->compositor, gc, x, y, w, h);
	pump(r, 150);
	gowl_compositor_arrange(r->compositor, gc->mon);
	pump(r, 50);
}

/* Bring up a rig with one mapped, placed, focused client, or skip. */
static gboolean
one_window(Rig *r, Client **out_c, GowlClient **out_gc,
           const gchar *app_id)
{
	*out_c = client_start(r, app_id);
	*out_gc = client_await_map(r, *out_c);
	if (*out_gc == NULL) {
		client_stop(r, *out_c);
		*out_c = NULL;
		g_test_skip("the client never mapped here");
		return FALSE;
	}
	place(r, *out_gc, 100, 100, 400, 300);
	gowl_compositor_focus_client(r->compositor, *out_gc, TRUE);
	pump(r, 50);
	return TRUE;
}

static gdouble
cursor_x(Rig *r)
{
	return r->compositor->wlr_cursor->x;
}

static gdouble
cursor_y(Rig *r)
{
	return r->compositor->wlr_cursor->y;
}

/* Run an action the way a key does. */
static void
action(Rig *r, GowlAction a, const gchar *arg)
{
	GowlKeybindEntry kb;
	g_autofree gchar *copy = g_strdup(arg);

	memset(&kb, 0, sizeof kb);
	kb.action = a;
	kb.arg = copy;
	gowl_compositor_run_keybind_entry(r->compositor, &kb);
	pump(r, 30);
}

/* Press and release a chord, through the compositor's key path. */
static void
press_chord(Rig *r, const guint32 *keys, gsize n)
{
	gsize i;

	for (i = 0; i < n; i++)
		gowl_compositor_inject_key(r->compositor, keys[i], TRUE);
	for (i = n; i > 0; i--)
		gowl_compositor_inject_key(r->compositor, keys[i - 1], FALSE);
	pump(r, 30);
}

static const guint32 escape_chord[] = { KEY_LEFTMETA, KEY_LEFTCTRL, KEY_ESC };

/* ── the tests ────────────────────────────────────────────────────── */

/*
 * The action confines to the focused window, and every motion path
 * respects it: a relative mouse, an absolute device, the KVM portal's
 * layout warp.  The relative stream still flows -- a client reading raw
 * deltas must not go deaf because the cursor is against the edge.
 */
static void
confine_clamps_every_motion_path(void)
{
	Rig r;
	Client *c = NULL;
	GowlClient *gc = NULL;
	gint bx, by, bw, bh;
	gint rel_before;
	g_autofree gchar *reply = NULL;
	g_autofree gchar *json = NULL;

	if (!rig_up(&r, NULL, FALSE))
		return;
	if (!one_window(&r, &c, &gc, "confine-a")) {
		rig_down(&r);
		return;
	}

	reply = gowl_compositor_ipc_command(r.compositor, "pointer-confine on");
	g_assert_cmpstr(reply, ==, "OK on");
	g_assert_true(gowl_compositor_get_pointer_confine_client(r.compositor)
	              == gc);
	g_assert_cmpint(r.ons, ==, 1);
	g_assert_cmpstr(r.last_reason, ==, "manual");
	g_assert_true(gowl_compositor_pointer_confine_pins_focus(r.compositor));

	/* The box is the window's surface: inside where it was placed. */
	g_assert_true(gowl_compositor_get_pointer_confine_box(r.compositor,
	                                                      &bx, &by, &bw, &bh));
	g_assert_cmpint(bx, >=, 100);
	g_assert_cmpint(by, >=, 100);
	g_assert_cmpint(bx + bw, <=, 500);
	g_assert_cmpint(by + bh, <=, 400);
	g_assert_cmpint(bw, >, 300);

	/* Confining pulled the cursor in. */
	g_assert_cmpfloat(cursor_x(&r), >=, bx);
	g_assert_cmpfloat(cursor_x(&r), <, bx + bw);

	/* Relative: a flick far to the right stops at the last column, and
	 * the client still hears about every pixel of it. */
	gowl_compositor_warp_cursor(r.compositor, bx + bw / 2.0, by + bh / 2.0);
	pump(&r, 50);
	g_assert_true(await(&r, &c->entered, 1));
	rel_before = g_atomic_int_get(&c->rel_events);
	gowl_compositor_inject_pointer_motion(r.compositor, 5000, 0);
	pump(&r, 100);
	g_assert_cmpfloat(cursor_x(&r), ==, bx + bw - 1);
	g_assert_cmpint(g_atomic_int_get(&c->rel_events), >, rel_before);
	g_assert_cmpint(g_atomic_int_get(&c->rel_dx), >=, 5000);

	/* And up-left, sliding along the edge rather than sticking. */
	gowl_compositor_inject_pointer_motion(r.compositor, -9000, -9000);
	pump(&r, 30);
	g_assert_cmpfloat(cursor_x(&r), ==, bx);
	g_assert_cmpfloat(cursor_y(&r), ==, by);

	/* Absolute: the far corner of the layout. */
	gowl_compositor_inject_pointer_motion_absolute(r.compositor, 1.0, 1.0);
	pump(&r, 30);
	g_assert_cmpfloat(cursor_x(&r), ==, bx + bw - 1);
	g_assert_cmpfloat(cursor_y(&r), ==, by + bh - 1);

	/* The portal's layout warp is motion too. */
	gowl_compositor_inject_pointer_warp(r.compositor, 0, 0);
	pump(&r, 30);
	g_assert_cmpfloat(cursor_x(&r), ==, bx);
	g_assert_cmpfloat(cursor_y(&r), ==, by);

	/* IPC tells the whole story. */
	json = gowl_compositor_ipc_command(r.compositor, "pointer-confine");
	g_assert_nonnull(strstr(json, "\"active\":true"));
	g_assert_nonnull(strstr(json, "\"app_id\":\"confine-a\""));
	g_assert_nonnull(strstr(json, "\"focus_pin\":true"));
	g_assert_nonnull(strstr(json, "\"client_constraint\":null"));
	g_free(reply);
	reply = gowl_compositor_ipc_command(r.compositor, "pointer-confine state");
	g_assert_cmpstr(reply, ==, "on");

	/* Off by IPC, and the cursor is free again. */
	g_free(reply);
	reply = gowl_compositor_ipc_command(r.compositor, "pointer-confine off");
	g_assert_cmpstr(reply, ==, "OK off");
	g_assert_cmpint(r.offs, ==, 1);
	gowl_compositor_inject_pointer_motion(r.compositor, 600, 0);
	pump(&r, 30);
	g_assert_cmpfloat(cursor_x(&r), >, bx + bw);

	client_stop(&r, c);
	rig_down(&r);
}

/* The box is the window wherever it is NOW: moved, the cursor comes
 * with it rather than being left outside a box that went elsewhere. */
static void
confine_follows_the_window(void)
{
	Rig r;
	Client *c = NULL;
	GowlClient *gc = NULL;
	gint bx, by, bw, bh;

	if (!rig_up(&r, NULL, FALSE))
		return;
	if (!one_window(&r, &c, &gc, "confine-move")) {
		rig_down(&r);
		return;
	}
	g_assert_true(gowl_compositor_confine_pointer(r.compositor, NULL));
	gowl_compositor_warp_cursor(r.compositor, 150, 150);
	pump(&r, 30);

	place(&r, gc, 600, 300, 300, 200);
	g_assert_true(gowl_compositor_get_pointer_confine_box(r.compositor,
	                                                      &bx, &by, &bw, &bh));
	g_assert_cmpint(bx, >=, 600);
	g_assert_cmpint(by, >=, 300);
	g_assert_cmpfloat(cursor_x(&r), >=, bx);
	g_assert_cmpfloat(cursor_x(&r), <, bx + bw);
	g_assert_cmpfloat(cursor_y(&r), >=, by);
	g_assert_cmpfloat(cursor_y(&r), <, by + bh);
	g_assert_true(gowl_compositor_pointer_confine_is_active(r.compositor));

	client_stop(&r, c);
	rig_down(&r);
}

/* Unmapped, the confinement goes first -- before anything of the
 * window is torn down -- and nothing reads the window afterwards. */
static void
release_on_unmap(void)
{
	Rig r;
	Client *c = NULL;
	GowlClient *gc = NULL;

	if (!rig_up(&r, NULL, FALSE))
		return;
	if (!one_window(&r, &c, &gc, "confine-unmap")) {
		rig_down(&r);
		return;
	}
	g_assert_true(gowl_compositor_confine_pointer(r.compositor, gc));

	g_atomic_int_set(&c->cmd_unmap, 1);
	g_assert_true(await(&r, &c->cmd_unmap, 2));
	pump(&r, 100);
	g_assert_false(gowl_compositor_pointer_confine_is_active(r.compositor));
	g_assert_cmpstr(r.last_reason, ==, "unmap");

	/* Motion afterwards reads no stale window. */
	gowl_compositor_inject_pointer_motion(r.compositor, 900, 500);
	pump(&r, 30);

	client_stop(&r, c);
	rig_down(&r);
}

static void
release_on_destroy(void)
{
	Rig r;
	Client *c = NULL;
	GowlClient *gc = NULL;

	if (!rig_up(&r, NULL, FALSE))
		return;
	if (!one_window(&r, &c, &gc, "confine-destroy")) {
		rig_down(&r);
		return;
	}
	g_assert_true(gowl_compositor_confine_pointer(r.compositor, gc));

	g_atomic_int_set(&c->cmd_destroy, 1);
	g_assert_true(await(&r, &c->destroyed, 1));
	pump(&r, 150);
	g_assert_false(gowl_compositor_pointer_confine_is_active(r.compositor));
	g_assert_null(gowl_compositor_get_pointer_confine_client(r.compositor));
	g_assert_cmpint(r.offs, ==, 1);

	gowl_compositor_inject_pointer_motion(r.compositor, -50, 70);
	pump(&r, 30);

	client_stop(&r, c);
	rig_down(&r);
}

/* Locking lets go, and nothing confines while locked. */
static void
release_on_lock(void)
{
	Rig r;
	Client *c = NULL;
	GowlClient *gc = NULL;

	if (!rig_up(&r, NULL, FALSE))
		return;
	if (!one_window(&r, &c, &gc, "confine-lock")) {
		rig_down(&r);
		return;
	}
	g_assert_true(gowl_compositor_confine_pointer(r.compositor, gc));

	gowl_compositor_set_locked(r.compositor, TRUE);
	pump(&r, 30);
	g_assert_false(gowl_compositor_pointer_confine_is_active(r.compositor));
	g_assert_cmpstr(r.last_reason, ==, "lock");
	g_assert_false(gowl_compositor_confine_pointer(r.compositor, gc));

	gowl_compositor_set_locked(r.compositor, FALSE);
	pump(&r, 30);
	/* A manual confinement is not restored by unlocking. */
	g_assert_false(gowl_compositor_pointer_confine_is_active(r.compositor));

	client_stop(&r, c);
	rig_down(&r);
}

/*
 * A workspace switch lets go: whether the window disappears with the
 * tag, or -- pinned to every tag -- stays on screen and keeps the
 * keyboard.  Switching workspace is the user doing something else.
 */
static void
release_on_workspace_switch(void)
{
	Rig r;
	Client *c = NULL;
	GowlClient *gc = NULL;

	if (!rig_up(&r, NULL, FALSE))
		return;
	if (!one_window(&r, &c, &gc, "confine-tags")) {
		rig_down(&r);
		return;
	}

	g_assert_true(gowl_compositor_confine_pointer(r.compositor, gc));
	action(&r, GOWL_ACTION_TAG_VIEW, "2");
	g_assert_false(gowl_compositor_pointer_confine_is_active(r.compositor));

	/* Back to it, pinned to every tag, confined again. */
	action(&r, GOWL_ACTION_TAG_VIEW, "1");
	gowl_client_set_sticky(gc, TRUE);
	gowl_compositor_focus_client(r.compositor, gc, TRUE);
	pump(&r, 30);
	g_assert_true(gowl_compositor_confine_pointer(r.compositor, gc));

	action(&r, GOWL_ACTION_TAG_VIEW, "4");
	g_assert_false(gowl_compositor_pointer_confine_is_active(r.compositor));
	g_assert_cmpstr(r.last_reason, ==, "workspace");

	client_stop(&r, c);
	rig_down(&r);
}

/* Focus moved by a key lets go -- a key is the user asking. */
static void
release_on_focus_change(void)
{
	Rig r;
	Client *a = NULL, *b;
	GowlClient *ga = NULL, *gb;

	if (!rig_up(&r, NULL, FALSE))
		return;
	if (!one_window(&r, &a, &ga, "confine-focus-a")) {
		rig_down(&r);
		return;
	}
	b = client_start(&r, "confine-focus-b");
	gb = client_await_map(&r, b);
	if (gb == NULL) {
		client_stop(&r, b);
		client_stop(&r, a);
		g_test_skip("the second client never mapped here");
		rig_down(&r);
		return;
	}
	gowl_compositor_focus_client(r.compositor, ga, TRUE);
	pump(&r, 30);
	g_assert_true(gowl_compositor_confine_pointer(r.compositor, NULL));
	g_assert_true(gowl_compositor_get_pointer_confine_client(r.compositor)
	              == ga);

	action(&r, GOWL_ACTION_FOCUS_STACK, "+1");
	g_assert_false(gowl_compositor_pointer_confine_is_active(r.compositor));
	g_assert_cmpstr(r.last_reason, ==, "focus");

	client_stop(&r, b);
	client_stop(&r, a);
	rig_down(&r);
}

/*
 * The client's own lock outranks the compositor: while it holds one,
 * the cursor stays where the LOCK says, not where the box would allow.
 * The confinement is not ended by it -- when the client lets go, the
 * box is back without anyone having to ask.
 */
static void
client_lock_takes_precedence(void)
{
	Rig r;
	Client *c = NULL;
	GowlClient *gc = NULL;
	gint bx, by, bw, bh;
	gdouble x, y;
	gint rel_before;

	if (!rig_up(&r, NULL, FALSE))
		return;
	if (!one_window(&r, &c, &gc, "confine-locker")) {
		rig_down(&r);
		return;
	}
	g_assert_true(gowl_compositor_confine_pointer(r.compositor, gc));
	g_assert_true(gowl_compositor_get_pointer_confine_box(r.compositor,
	                                                      &bx, &by, &bw, &bh));
	gowl_compositor_warp_cursor(r.compositor, bx + 20, by + 20);
	pump(&r, 50);
	if (!await(&r, &c->entered, 1)) {
		client_stop(&r, c);
		g_test_skip("the client never got pointer focus here");
		rig_down(&r);
		return;
	}

	g_atomic_int_set(&c->cmd_lock, 1);
	if (!await(&r, &c->locked, 1)) {
		client_stop(&r, c);
		g_test_skip("the client never took the lock here");
		rig_down(&r);
		return;
	}
	g_assert_true(gowl_compositor_pointer_is_locked(r.compositor));

	x = cursor_x(&r);
	y = cursor_y(&r);
	rel_before = g_atomic_int_get(&c->rel_events);
	gowl_compositor_inject_pointer_motion(r.compositor, 50, 30);
	pump(&r, 50);
	g_assert_cmpfloat(cursor_x(&r), ==, x);
	g_assert_cmpfloat(cursor_y(&r), ==, y);
	g_assert_cmpint(g_atomic_int_get(&c->rel_events), >, rel_before);
	g_assert_true(gowl_compositor_pointer_confine_is_active(r.compositor));
	{
		g_autofree gchar *json = gowl_compositor_ipc_command(
			r.compositor, "pointer-confine");

		g_assert_nonnull(strstr(json, "\"client_constraint\":\"locked\""));
	}

	/* The client lets go; the compositor's box is in charge again. */
	g_atomic_int_set(&c->cmd_unlock, 1);
	g_assert_true(await(&r, &c->cmd_unlock, 2));
	pump(&r, 100);
	g_assert_false(gowl_compositor_pointer_is_locked(r.compositor));
	gowl_compositor_inject_pointer_motion(r.compositor, 5000, 5000);
	pump(&r, 50);
	g_assert_cmpfloat(cursor_x(&r), ==, bx + bw - 1);
	g_assert_cmpfloat(cursor_y(&r), ==, by + bh - 1);

	client_stop(&r, c);
	rig_down(&r);
}

/*
 * The escape works whatever else holds the keyboard -- here a key mode
 * whose map has no such key -- and only while confined: the same chord,
 * bound in the config, runs its bind the rest of the time.  The bind
 * here confines, so pressing the chord twice confines and lets go.
 */
static void
escape_always_works(void)
{
	Rig r;
	Client *c = NULL;
	GowlClient *gc = NULL;

	if (!rig_up(&r, NULL, TRUE))
		return;
	if (!one_window(&r, &c, &gc, "confine-escape")) {
		rig_down(&r);
		return;
	}
	if (r.compositor->wlr_kb_group == NULL) {
		client_stop(&r, c);
		g_test_skip("no keyboard group here");
		rig_down(&r);
		return;
	}

	/* Not confined: the chord is an ordinary bind, and it confines. */
	press_chord(&r, escape_chord, G_N_ELEMENTS(escape_chord));
	g_assert_true(gowl_compositor_pointer_confine_is_active(r.compositor));

	/* Confined, in a key mode with nothing bound: the escape still
	 * lets go. */
	gowl_compositor_set_key_mode(r.compositor, "resize");
	press_chord(&r, escape_chord, G_N_ELEMENTS(escape_chord));
	g_assert_false(gowl_compositor_pointer_confine_is_active(r.compositor));
	g_assert_cmpstr(r.last_reason, ==, "escape");
	gowl_compositor_set_key_mode(r.compositor, NULL);

	/* Escape with a modifier too many is somebody else's chord. */
	g_assert_true(gowl_compositor_confine_pointer(r.compositor, gc));
	{
		static const guint32 shifted[] = {
			KEY_LEFTMETA, KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_ESC
		};

		press_chord(&r, shifted, G_N_ELEMENTS(shifted));
	}
	g_assert_true(gowl_compositor_pointer_confine_is_active(r.compositor));

	client_stop(&r, c);
	rig_down(&r);
}

/*
 * The window rule, through the real windowrules module: a matching
 * window holds the pointer as soon as it has the keyboard.  Let go of
 * by hand, it stays let go until focus has been somewhere else -- and
 * is taken again when focus comes back.
 */
static void
rule_confines_on_focus(void)
{
	Rig r;
	Client *game, *other;
	GowlClient *gg, *go;

	if (!rig_up(&r, "confine-rule-*", FALSE))
		return;

	game = client_start(&r, "confine-rule-game");
	gg = client_await_map(&r, game);
	if (gg == NULL) {
		client_stop(&r, game);
		g_test_skip("the client never mapped here");
		rig_down(&r);
		return;
	}
	pump(&r, 50);
	g_assert_true((gg->rule_flags & GOWL_CLIENT_RULE_CONFINE_POINTER) != 0);
	g_assert_true(gowl_compositor_get_pointer_confine_client(r.compositor)
	              == gg);
	g_assert_cmpstr(r.last_reason, ==, "rule");
	{
		g_autofree gchar *json = gowl_compositor_ipc_command(
			r.compositor, "pointer-confine");

		g_assert_nonnull(strstr(json, "\"by_rule\":true"));
	}

	/* Let go by hand: the rule does not take it straight back, even
	 * when the window is refocused. */
	action(&r, GOWL_ACTION_TOGGLE_POINTER_CONFINE, "off");
	g_assert_false(gowl_compositor_pointer_confine_is_active(r.compositor));
	gowl_compositor_focus_client(r.compositor, gg, TRUE);
	pump(&r, 30);
	g_assert_false(gowl_compositor_pointer_confine_is_active(r.compositor));

	/* A window that matches no rule maps and takes the keyboard (it is
	 * this test's own process, which focus pinning would let through
	 * anyway). */
	other = client_start(&r, "confine-plain");
	go = client_await_map(&r, other);
	g_assert_nonnull(go);
	gowl_compositor_focus_client(r.compositor, go, TRUE);
	pump(&r, 30);
	g_assert_false(gowl_compositor_pointer_confine_is_active(r.compositor));

	/* Focus back on the game, having been elsewhere: the rule again. */
	gowl_compositor_focus_client(r.compositor, gg, TRUE);
	pump(&r, 30);
	g_assert_true(gowl_compositor_get_pointer_confine_client(r.compositor)
	              == gg);

	client_stop(&r, other);
	client_stop(&r, game);
	rig_down(&r);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/pointer-confine-live/clamps-every-motion-path",
	                confine_clamps_every_motion_path);
	g_test_add_func("/pointer-confine-live/follows-the-window",
	                confine_follows_the_window);
	g_test_add_func("/pointer-confine-live/release/unmap", release_on_unmap);
	g_test_add_func("/pointer-confine-live/release/destroy",
	                release_on_destroy);
	g_test_add_func("/pointer-confine-live/release/lock", release_on_lock);
	g_test_add_func("/pointer-confine-live/release/workspace",
	                release_on_workspace_switch);
	g_test_add_func("/pointer-confine-live/release/focus",
	                release_on_focus_change);
	g_test_add_func("/pointer-confine-live/client-lock-first",
	                client_lock_takes_precedence);
	g_test_add_func("/pointer-confine-live/escape", escape_always_works);
	g_test_add_func("/pointer-confine-live/rule", rule_confines_on_focus);

	return g_test_run();
}
