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
 * The protocol globals an application expects, and what the output
 * power path does behind them.
 *
 * A real headless compositor is started and a real Wayland client
 * connects to it from a second thread and reads the registry: the
 * globals a video player (idle-inhibit), a VM viewer
 * (keyboard-shortcuts-inhibit), a taskbar (foreign-toplevel), wlopm
 * (output-power), a game (tearing-control, content-type) and a portal
 * dialog (xdg-foreign) look for on connect.  Each was missing once,
 * and each absence was silent: the program simply did without.
 *
 * Then the dpms path, on the headless output: the timer powers it
 * off, input powers it on, an explicit `output-power off' survives
 * the input that follows it, and the `output-power' action toggles.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "gowl.h"
#include "core/gowl-core-private.h"
#include "config/gowl-config.h"

typedef struct {
	gchar          *runtime;
	GowlConfig     *config;
	GowlCompositor *compositor;
	GowlMonitor    *monitor;
	gint            power_events;
	gboolean        last_power_on;
} Fixture;

static void
isolate(Fixture *f)
{
	const gchar *parent;
	gchar *state;

	parent = g_getenv("XDG_RUNTIME_DIR");
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	f->runtime = g_build_filename(parent, "gowl-proto-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(f->runtime, 0700));
	state = g_build_filename(f->runtime, "state", NULL);
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", f->runtime, TRUE);
	g_setenv("XDG_STATE_HOME", state, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);
	g_free(state);
}

static void
on_power_changed(
	GowlCompositor *comp,
	GowlMonitor    *m,
	gboolean        on,
	gpointer        data
){
	Fixture *f = (Fixture *)data;
	(void)comp;
	(void)m;

	f->power_events++;
	f->last_power_on = on;
}

static void
fixture_setup(Fixture *f, gconstpointer data)
{
	GError *error = NULL;
	(void)data;

	isolate(f);
	f->config = gowl_config_new();
	/* A one-second dpms timeout is what the timer test waits on. */
	g_object_set(f->config, "dpms-timeout", 1, "idle-timeout", 1,
	             "xkb-layout", "us,de", NULL);
	f->compositor = gowl_compositor_new();
	gowl_compositor_set_config(f->compositor, f->config);
	if (!gowl_compositor_start(f->compositor, &error))
		g_error("could not start a headless compositor: %s",
		        error->message);
	g_assert_nonnull(f->compositor->monitors);
	f->monitor = (GowlMonitor *)f->compositor->monitors->data;
	g_signal_connect(f->compositor, "output-power-changed",
	                 G_CALLBACK(on_power_changed), f);
}

static void
fixture_teardown(Fixture *f, gconstpointer data)
{
	static const gchar *const store[] = { "state/gowl", "state" };
	gsize i;
	(void)data;

	g_object_unref(f->compositor);
	g_object_unref(f->config);
	for (i = 0; i < G_N_ELEMENTS(store); i++) {
		gchar *path = g_build_filename(f->runtime, store[i], NULL);

		if (g_file_test(path, G_FILE_TEST_IS_DIR))
			g_rmdir(path);
		g_free(path);
	}
	g_rmdir(f->runtime);
	g_free(f->runtime);
}

/* Run the compositor's loop for @msec. */
static void
pump(Fixture *f, gint msec)
{
	gint64 until = g_get_monotonic_time() + (gint64)msec * 1000;

	while (g_get_monotonic_time() < until) {
		wl_display_flush_clients(f->compositor->wl_display);
		wl_event_loop_dispatch(f->compositor->event_loop, 5);
	}
}

/* --- the registry, read by a client on another thread --- */

typedef struct {
	const gchar *socket;
	GPtrArray   *globals;   /* interface names */
	gboolean     ok;
} Registry;

static void
registry_global(
	void               *data,
	struct wl_registry *registry,
	uint32_t            name,
	const char         *interface,
	uint32_t            version
){
	Registry *r = (Registry *)data;
	(void)registry;
	(void)name;
	(void)version;

	g_ptr_array_add(r->globals, g_strdup(interface));
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_global_remove,
};

static gpointer
read_registry(gpointer data)
{
	Registry *r = (Registry *)data;
	struct wl_display *display;
	struct wl_registry *registry;

	display = wl_display_connect(r->socket);
	if (display == NULL)
		return NULL;
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, r);
	r->ok = wl_display_roundtrip(display) >= 0;
	wl_registry_destroy(registry);
	wl_display_disconnect(display);
	return NULL;
}

static gboolean
has_global(Registry *r, const gchar *iface)
{
	guint i;

	for (i = 0; i < r->globals->len; i++)
		if (g_str_equal(g_ptr_array_index(r->globals, i), iface))
			return TRUE;
	return FALSE;
}

static void
test_globals_advertised(Fixture *f, gconstpointer data)
{
	static const gchar *const expected[] = {
		"zwp_idle_inhibit_manager_v1",
		"ext_idle_notifier_v1",
		"zwlr_output_power_manager_v1",
		"zwp_keyboard_shortcuts_inhibit_manager_v1",
		"zwlr_foreign_toplevel_manager_v1",
		"ext_foreign_toplevel_list_v1",
		"wp_tearing_control_manager_v1",
		"wp_content_type_manager_v1",
		"zxdg_exporter_v2",
		"zxdg_importer_v2",
		"zxdg_exporter_v1",
		"zwp_text_input_manager_v3",
		"zwp_input_method_manager_v2",
	};
	Registry r;
	GThread *thread;
	gsize i;
	(void)data;

	r.socket = gowl_compositor_get_socket_name(f->compositor);
	r.globals = g_ptr_array_new_with_free_func(g_free);
	r.ok = FALSE;
	g_assert_nonnull(r.socket);

	thread = g_thread_new("registry", read_registry, &r);
	pump(f, 500);
	g_thread_join(thread);

	g_assert_true(r.ok);
	for (i = 0; i < G_N_ELEMENTS(expected); i++) {
		if (!has_global(&r, expected[i]))
			g_error("global %s is not advertised", expected[i]);
	}
	g_ptr_array_unref(r.globals);
}

/* --- output power --- */

static void
test_dpms_timer_and_wake(Fixture *f, gconstpointer data)
{
	GowlIdleManager *idle;
	(void)data;

	idle = gowl_compositor_get_idle_manager(f->compositor);
	g_assert_nonnull(idle);
	g_assert_cmpint(gowl_idle_manager_get_dpms_timeout(idle), ==, 1);
	g_assert_false(f->monitor->powered_off);
	g_assert_true(f->monitor->wlr_output->enabled);

	/* Nobody touches anything for a second: off, by the timer, and
	 * the session is idle. */
	pump(f, 1400);
	g_assert_true(f->monitor->powered_off);
	g_assert_false(f->monitor->wlr_output->enabled);
	g_assert_cmpint(f->power_events, ==, 1);
	g_assert_false(f->last_power_on);
	g_assert_cmpint(gowl_idle_manager_get_state(idle), ==, 1);

	/* Input: straight back on, no grace, because the timer did it. */
	gowl_idle_manager_note_activity(idle);
	g_assert_false(f->monitor->powered_off);
	g_assert_true(f->monitor->wlr_output->enabled);
	g_assert_cmpint(f->power_events, ==, 2);
	g_assert_true(f->last_power_on);
	g_assert_cmpint(gowl_idle_manager_get_state(idle), ==, 0);
}

static void
test_explicit_off_survives_the_key_release(Fixture *f, gconstpointer data)
{
	GowlIdleManager *idle;
	(void)data;

	idle = gowl_compositor_get_idle_manager(f->compositor);
	gowl_idle_manager_set_dpms_timeout(idle, 0);

	gowl_compositor_set_outputs_powered(f->compositor, FALSE);
	g_assert_true(f->monitor->powered_off);
	g_assert_true(gowl_compositor_any_output_powered_off(f->compositor));

	/* The release of the key that ran it. */
	gowl_idle_manager_note_activity(idle);
	g_assert_true(f->monitor->powered_off);

	/* Well after the grace period, input wakes it. */
	pump(f, 700);
	gowl_idle_manager_note_activity(idle);
	g_assert_false(f->monitor->powered_off);
	g_assert_false(gowl_compositor_any_output_powered_off(f->compositor));
}

static void
test_output_power_action(Fixture *f, gconstpointer data)
{
	GowlIdleManager *idle;
	(void)data;

	idle = gowl_compositor_get_idle_manager(f->compositor);
	gowl_idle_manager_set_dpms_timeout(idle, 0);

	gowl_config_add_keybind_full(f->config, GOWL_KEY_MOD_LOGO, XKB_KEY_o,
	                             GOWL_ACTION_OUTPUT_POWER, NULL, NULL);
	gowl_config_add_keybind_full(f->config, GOWL_KEY_MOD_LOGO, XKB_KEY_p,
	                             GOWL_ACTION_OUTPUT_POWER, "on", NULL);

	g_assert_true(gowl_compositor_dispatch_keybind(f->compositor,
		GOWL_KEY_MOD_LOGO, XKB_KEY_o));
	g_assert_true(f->monitor->powered_off);
	g_assert_true(gowl_compositor_dispatch_keybind(f->compositor,
		GOWL_KEY_MOD_LOGO, XKB_KEY_o));
	g_assert_false(f->monitor->powered_off);
	g_assert_true(gowl_compositor_dispatch_keybind(f->compositor,
		GOWL_KEY_MOD_LOGO, XKB_KEY_p));
	g_assert_false(f->monitor->powered_off);
	g_assert_cmpint(f->power_events, ==, 2);
}

static void
test_reload_reapplies_timeouts(Fixture *f, gconstpointer data)
{
	GowlIdleManager *idle;
	(void)data;

	idle = gowl_compositor_get_idle_manager(f->compositor);
	g_assert_cmpint(gowl_idle_manager_get_timeout(idle), ==, 1);
	gowl_idle_manager_set_timeout(idle, 0);
	gowl_idle_manager_set_dpms_timeout(idle, 0);
	/* Parked: a second passes and nothing happens. */
	pump(f, 1200);
	g_assert_false(f->monitor->powered_off);
	g_assert_cmpint(gowl_idle_manager_get_state(idle), ==, 0);
}

/* --- output profiles --- */

static void
on_profile_changed(GowlCompositor *c, const gchar *name, gpointer data)
{
	gchar **seen = (gchar **)data;
	(void)c;

	g_free(*seen);
	*seen = g_strdup(name);
}

/*
 * The first profile whose outputs are all connected is the one in
 * force: "desk" names an output that is not there and is skipped for
 * "alone", which names the headless one and scales it.  A reload that
 * drops the profiles ends it, announced the same way.
 */
static void
test_output_profiles(Fixture *f, gconstpointer data)
{
	g_autofree gchar *path = g_build_filename(f->runtime, "profiles.yaml",
	                                          NULL);
	gchar *seen = NULL;
	const gchar *name = f->monitor->wlr_output->name;
	g_autofree gchar *yaml = g_strdup_printf(
		"profiles:\n"
		"  desk:\n"
		"    %s:\n"
		"    DP-9:\n"
		"  alone:\n"
		"    %s:\n"
		"      scale: 2.0\n", name, name);
	(void)data;

	g_signal_connect(f->compositor, "output-profile-changed",
	                 G_CALLBACK(on_profile_changed), &seen);
	g_assert_null(gowl_compositor_get_output_profile(f->compositor));

	g_assert_true(g_file_set_contents(path, yaml, -1, NULL));
	g_assert_true(gowl_config_load_yaml(f->config, path, NULL));
	gowl_compositor_apply_monitor_configs(f->compositor);
	pump(f, 50);
	g_assert_cmpstr(gowl_compositor_get_output_profile(f->compositor),
	                ==, "alone");
	g_assert_cmpstr(seen, ==, "alone");
	g_assert_cmpfloat(f->monitor->wlr_output->scale, ==, 2.0);

	/* Nothing changed: no second emission. */
	g_free(seen);
	seen = NULL;
	g_assert_false(gowl_compositor_select_output_profile(f->compositor));
	g_assert_null(seen);

	/* The profiles go away on a reload. */
	g_assert_true(g_file_set_contents(path, "border-width: 2\n", -1, NULL));
	g_assert_true(gowl_config_load_yaml(f->config, path, NULL));
	gowl_compositor_apply_monitor_configs(f->compositor);
	g_assert_null(gowl_compositor_get_output_profile(f->compositor));
	g_assert_cmpstr(seen, ==, "");

	g_signal_handlers_disconnect_by_func(f->compositor,
	                                     G_CALLBACK(on_profile_changed),
	                                     &seen);
	g_free(seen);
	g_unlink(path);
}

/* --- keyboard layouts --- */

typedef struct {
	guint calls;
	gchar *name;
	guint index;
} LayoutProbe;

static void
on_layout_changed(GowlCompositor *c, const gchar *name, guint index,
                  gpointer data)
{
	LayoutProbe *p = (LayoutProbe *)data;
	(void)c;

	p->calls++;
	g_free(p->name);
	p->name = g_strdup(name);
	p->index = index;
}

static void
test_keyboard_layouts(Fixture *f, gconstpointer data)
{
	LayoutProbe lp = { 0, NULL, 0 };
	const gchar *name;
	guint index = 99;
	(void)data;

	g_signal_connect(f->compositor, "keyboard-layout-changed",
	                 G_CALLBACK(on_layout_changed), &lp);

	/* xkb-layout "us,de" compiled into the group keymap. */
	name = gowl_compositor_get_keyboard_layout(f->compositor, &index);
	g_assert_nonnull(name);
	g_assert_cmpuint(index, ==, 0);
	g_assert_nonnull(strstr(name, "English"));

	g_assert_true(gowl_compositor_switch_keyboard_layout(f->compositor, "next"));
	name = gowl_compositor_get_keyboard_layout(f->compositor, &index);
	g_assert_cmpuint(index, ==, 1);
	g_assert_nonnull(strstr(name, "German"));
	g_assert_cmpuint(lp.calls, ==, 1);
	g_assert_cmpuint(lp.index, ==, 1);
	g_assert_nonnull(strstr(lp.name, "German"));

	/* Wraps, by index, and a no-op switch says so. */
	g_assert_true(gowl_compositor_switch_keyboard_layout(f->compositor, "next"));
	gowl_compositor_get_keyboard_layout(f->compositor, &index);
	g_assert_cmpuint(index, ==, 0);
	g_assert_true(gowl_compositor_switch_keyboard_layout(f->compositor, "1"));
	g_assert_false(gowl_compositor_switch_keyboard_layout(f->compositor, "1"));
	g_assert_cmpuint(lp.calls, ==, 3);

	/* The action does the same through a bind. */
	gowl_config_add_keybind_full(f->config, GOWL_KEY_MOD_LOGO, XKB_KEY_space,
	                             GOWL_ACTION_SWITCH_LAYOUT, "prev", NULL);
	g_assert_true(gowl_compositor_dispatch_keybind(f->compositor,
		GOWL_KEY_MOD_LOGO, XKB_KEY_space));
	gowl_compositor_get_keyboard_layout(f->compositor, &index);
	g_assert_cmpuint(index, ==, 0);

	/* A reload with one layout leaves nothing to switch to. */
	g_object_set(f->config, "xkb-layout", "us", NULL);
	gowl_compositor_apply_keymap(f->compositor);
	g_assert_false(gowl_compositor_switch_keyboard_layout(f->compositor, "next"));
	g_free(lp.name);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add("/protocols/globals-advertised", Fixture, NULL,
	           fixture_setup, test_globals_advertised, fixture_teardown);
	g_test_add("/protocols/output-power/dpms-timer-and-wake", Fixture, NULL,
	           fixture_setup, test_dpms_timer_and_wake, fixture_teardown);
	g_test_add("/protocols/output-power/explicit-off-survives-release",
	           Fixture, NULL, fixture_setup,
	           test_explicit_off_survives_the_key_release, fixture_teardown);
	g_test_add("/protocols/output-power/action", Fixture, NULL,
	           fixture_setup, test_output_power_action, fixture_teardown);
	g_test_add("/protocols/keyboard/layouts", Fixture, NULL,
	           fixture_setup, test_keyboard_layouts, fixture_teardown);
	g_test_add("/protocols/output-profiles", Fixture, NULL,
	           fixture_setup, test_output_profiles, fixture_teardown);
	g_test_add("/protocols/idle/timeouts-parked", Fixture, NULL,
	           fixture_setup, test_reload_reapplies_timeouts, fixture_teardown);

	return g_test_run();
}
