/* test-input-capture-zones.c -- a KVM hears when the screens change
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * deskflow (and anything else on the InputCapture / RemoteDesktop
 * portals) learns the screen's shape from libei regions, and
 * xdg-desktop-portal-gowl builds those regions from the zones this
 * protocol hands it.  The protocol has a `zones_changed' event for when
 * they stop being true, and the portal answers it -- but the compositor
 * never sent it.  So after rotating an output the remote pointer still
 * moved over the old, unrotated rectangle: allowed past the new bottom
 * edge, stopped short of the new right one.  The same after plugging a
 * monitor in or pulling one out.
 *
 * This is the portal's side of the wire, driven for real: a Wayland
 * client in the test binds the manager, opens a capture session, reads
 * the zones, and then watches what arrives while the outputs change
 * under it -- a rotation, a rotation back, and a hotplug.
 *
 * Client and compositor run on one thread, taking turns; nothing here
 * may block on the other side.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <poll.h>
#include <string.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/types/wlr_output.h>

#include "gowl-input-capture-v1-client-protocol.h"

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "core/gowl-monitor.h"

typedef struct {
	gint x, y;
	guint w, h;
} Zone;

typedef struct {
	struct wl_display                       *display;
	struct wl_registry                      *registry;
	struct zgowl_input_capture_manager_v1   *manager;
	struct zgowl_input_capture_v1           *capture;
	GArray                                  *pending;  /* Zone */
	GArray                                  *zones;    /* Zone, last full set */
	guint                                    zones_done;
	guint                                    changed;
	guint32                                  last_set;
} Client;

typedef struct {
	gchar          *parent;
	gchar          *runtime;
	GowlConfig     *config;
	GowlCompositor *compositor;
	Client          c;
} Rig;

/* ── The client ──────────────────────────────────────────────────── */

static void
cap_capabilities(void *data, struct zgowl_input_capture_v1 *cap,
                 uint32_t caps)
{
}

static void
cap_zone(void *data, struct zgowl_input_capture_v1 *cap,
         uint32_t w, uint32_t h, int32_t x, int32_t y)
{
	Client *c = data;
	Zone z = { x, y, w, h };

	g_array_append_val(c->pending, z);
}

static void
cap_zones_done(void *data, struct zgowl_input_capture_v1 *cap,
               uint32_t zone_set)
{
	Client *c = data;

	g_array_set_size(c->zones, 0);
	g_array_append_vals(c->zones, c->pending->data, c->pending->len);
	g_array_set_size(c->pending, 0);
	c->last_set = zone_set;
	c->zones_done++;
}

/* What the portal does: hear it, and ask again. */
static void
cap_zones_changed(void *data, struct zgowl_input_capture_v1 *cap,
                  uint32_t zone_set)
{
	Client *c = data;

	c->changed++;
	zgowl_input_capture_v1_get_zones(cap);
}

static void cap_barrier_status(void *d, struct zgowl_input_capture_v1 *c,
                               uint32_t id, uint32_t accepted) { }
static void cap_activated(void *d, struct zgowl_input_capture_v1 *c,
                          uint32_t id, wl_fixed_t x, wl_fixed_t y,
                          uint32_t barrier) { }
static void cap_deactivated(void *d, struct zgowl_input_capture_v1 *c,
                            uint32_t id) { }
static void cap_rel_motion(void *d, struct zgowl_input_capture_v1 *c,
                           uint32_t t, wl_fixed_t dx, wl_fixed_t dy) { }
static void cap_button(void *d, struct zgowl_input_capture_v1 *c,
                       uint32_t t, uint32_t b, uint32_t s) { }
static void cap_axis(void *d, struct zgowl_input_capture_v1 *c,
                     uint32_t t, uint32_t a, wl_fixed_t v, int32_t disc) { }
static void cap_key(void *d, struct zgowl_input_capture_v1 *c,
                    uint32_t t, uint32_t k, uint32_t s) { }
static void cap_modifiers(void *d, struct zgowl_input_capture_v1 *c,
                          uint32_t a, uint32_t b, uint32_t e, uint32_t g) { }

static const struct zgowl_input_capture_v1_listener cap_listener = {
	.capabilities   = cap_capabilities,
	.zone           = cap_zone,
	.zones_done     = cap_zones_done,
	.zones_changed  = cap_zones_changed,
	.barrier_status = cap_barrier_status,
	.activated      = cap_activated,
	.deactivated    = cap_deactivated,
	.rel_motion     = cap_rel_motion,
	.button         = cap_button,
	.axis           = cap_axis,
	.key            = cap_key,
	.modifiers      = cap_modifiers,
};

static void
registry_global(void *data, struct wl_registry *reg, uint32_t name,
                const char *iface, uint32_t version)
{
	Client *c = data;

	if (g_strcmp0(iface, zgowl_input_capture_manager_v1_interface.name) == 0)
		c->manager = wl_registry_bind(reg, name,
			&zgowl_input_capture_manager_v1_interface, 1);
}

static void registry_remove(void *d, struct wl_registry *r, uint32_t n) { }

static const struct wl_registry_listener registry_listener = {
	.global = registry_global, .global_remove = registry_remove,
};

/* ── Taking turns ────────────────────────────────────────────────── */

/* One turn each: the compositor dispatches and flushes, then the client
 * reads whatever is waiting without blocking and handles it. */
static void
turn(Rig *r)
{
	struct pollfd pfd;

	wl_event_loop_dispatch(
		wl_display_get_event_loop(r->compositor->wl_display), 0);
	wl_display_flush_clients(r->compositor->wl_display);

	wl_display_flush(r->c.display);
	while (wl_display_prepare_read(r->c.display) != 0)
		wl_display_dispatch_pending(r->c.display);
	pfd.fd = wl_display_get_fd(r->c.display);
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 1) > 0)
		wl_display_read_events(r->c.display);
	else
		wl_display_cancel_read(r->c.display);
	wl_display_dispatch_pending(r->c.display);
}

static void
pump(Rig *r, gint ms)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < deadline)
		turn(r);
}

/* Turn until @count reaches @want, for at most two seconds. */
static gboolean
await_count(Rig *r, guint *count, guint want)
{
	gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;

	while (*count < want && g_get_monotonic_time() < deadline)
		turn(r);
	return *count >= want;
}

/* ── The rig ─────────────────────────────────────────────────────── */

static gboolean
rig_up(Rig *r)
{
	const gchar *parent;
	GError *error = NULL;
	const gchar *socket;
	gchar *path;

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	r->parent = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	parent = r->parent;
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-ic-zones-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);
	g_setenv("GOWL_OUTPUT_SIZE", "1920x1200", TRUE);

	r->config = gowl_config_new();
	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_clear_error(&error);
		return FALSE;
	}
	/* Every output from here on brings its own size. */
	g_unsetenv("GOWL_OUTPUT_SIZE");

	socket = gowl_compositor_get_socket_name(r->compositor);
	g_assert_nonnull(socket);
	path = g_build_filename(r->runtime, socket, NULL);
	r->c.display = wl_display_connect(path);
	g_free(path);
	g_assert_nonnull(r->c.display);
	r->c.pending = g_array_new(FALSE, FALSE, sizeof(Zone));
	r->c.zones = g_array_new(FALSE, FALSE, sizeof(Zone));
	r->c.registry = wl_display_get_registry(r->c.display);
	wl_registry_add_listener(r->c.registry, &registry_listener, &r->c);
	pump(r, 100);
	g_assert_nonnull(r->c.manager);

	r->c.capture = zgowl_input_capture_manager_v1_create_capture(
		r->c.manager);
	zgowl_input_capture_v1_add_listener(r->c.capture, &cap_listener, &r->c);
	zgowl_input_capture_v1_get_zones(r->c.capture);
	g_assert_true(await_count(r, &r->c.zones_done, 1));
	return TRUE;
}

static void
rig_down(Rig *r)
{
	if (r->c.capture != NULL)
		zgowl_input_capture_v1_destroy(r->c.capture);
	if (r->c.manager != NULL)
		zgowl_input_capture_manager_v1_destroy(r->c.manager);
	if (r->c.registry != NULL)
		wl_registry_destroy(r->c.registry);
	if (r->c.display != NULL) {
		wl_display_flush(r->c.display);
		pump(r, 20);
		wl_display_disconnect(r->c.display);
	}
	if (r->c.pending != NULL)
		g_array_unref(r->c.pending);
	if (r->c.zones != NULL)
		g_array_unref(r->c.zones);
	g_clear_object(&r->compositor);
	g_clear_object(&r->config);
	if (r->parent != NULL)
		g_setenv("XDG_RUNTIME_DIR", r->parent, TRUE);
	else
		g_unsetenv("XDG_RUNTIME_DIR");
	g_rmdir(r->runtime);
	g_free(r->runtime);
	g_free(r->parent);
	memset(r, 0, sizeof(*r));
}

static Zone
zone_at(Rig *r, guint i)
{
	g_assert_cmpuint(r->c.zones->len, >, i);
	return g_array_index(r->c.zones, Zone, i);
}

static void
find_headless(struct wlr_backend *backend, void *data)
{
	struct wlr_backend **out = (struct wlr_backend **)data;

	if (*out == NULL && wlr_backend_is_headless(backend))
		*out = backend;
}

/* ── The cases ───────────────────────────────────────────────────── */

/*
 * Rotating the output reshapes the zone the session sees, and rotating
 * it back reshapes it again -- each time announced, never left for the
 * client to notice.
 */
static void
test_rotation_reaches_the_session(void)
{
	GowlMonitor *m;
	Zone z;
	guint changed;
	Rig r;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor here");
		return;
	}
	z = zone_at(&r, 0);
	g_assert_cmpuint(z.w, ==, 1920);
	g_assert_cmpuint(z.h, ==, 1200);
	g_assert_cmpuint(r.c.changed, ==, 0);

	m = r.compositor->selmon;
	g_assert_true(gowl_monitor_set_transform(m, WL_OUTPUT_TRANSFORM_90));
	g_assert_true(await_count(&r, &r.c.changed, 1));
	/* ...and the zones the client then asks for are the rotated ones. */
	g_assert_true(await_count(&r, &r.c.zones_done, 2));
	z = zone_at(&r, 0);
	g_assert_cmpuint(z.w, ==, 1200);
	g_assert_cmpuint(z.h, ==, 1920);

	changed = r.c.changed;
	g_assert_true(gowl_monitor_set_transform(m, WL_OUTPUT_TRANSFORM_NORMAL));
	g_assert_true(await_count(&r, &r.c.changed, changed + 1));
	g_assert_true(await_count(&r, &r.c.zones_done, 3));
	z = zone_at(&r, 0);
	g_assert_cmpuint(z.w, ==, 1920);
	g_assert_cmpuint(z.h, ==, 1200);
	rig_down(&r);
}

/* A monitor plugged in is a second zone, and the session is told. */
static void
test_hotplug_reaches_the_session(void)
{
	struct wlr_backend *headless = NULL;
	struct wlr_output *added;
	Rig r;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor here");
		return;
	}
	wlr_multi_for_each_backend(r.compositor->backend, find_headless,
	                           &headless);
	if (headless == NULL) {
		rig_down(&r);
		g_test_skip("no headless backend to add an output to");
		return;
	}
	added = wlr_headless_add_output(headless, 1080, 1920);
	g_assert_nonnull(added);
	g_assert_true(await_count(&r, &r.c.changed, 1));
	g_assert_true(await_count(&r, &r.c.zones_done, 2));
	g_assert_cmpuint(r.c.zones->len, ==, 2);
	rig_down(&r);
}

/*
 * Nothing changed, nothing said: a layout change that leaves every zone
 * as it was must not drop the session's barriers by announcing one.
 */
static void
test_unchanged_layout_is_quiet(void)
{
	GowlMonitor *m;
	Rig r;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor here");
		return;
	}
	m = r.compositor->selmon;
	/* Moving the only output to where it already is. */
	g_assert_true(gowl_monitor_set_position(m, m->m.x, m->m.y));
	pump(&r, 100);
	g_assert_cmpuint(r.c.changed, ==, 0);
	rig_down(&r);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/input-capture/zones/rotation",
	                test_rotation_reaches_the_session);
	g_test_add_func("/input-capture/zones/hotplug",
	                test_hotplug_reaches_the_session);
	g_test_add_func("/input-capture/zones/unchanged-is-quiet",
	                test_unchanged_layout_is_quiet);
	return g_test_run();
}
