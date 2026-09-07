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
 * The plugin contract, the theme, the registry and the fault guard.
 *
 * The guard tests are the ones worth reading twice: they deliberately
 * raise the faults a bad plugin raises, and assert that the process
 * survives.  If they ever start crashing the test runner rather than
 * failing, the containment the whole plugin system rests on is gone.
 */

#include <signal.h>
#include <string.h>

#include "barkit/gowl-bar-guard.h"
#include "barkit/gowl-bar-plugin.h"
#include "barkit/gowl-bar-plugin-proxy.h"
#include "barkit/gowl-bar-registry.h"
#include "barkit/gowl-bar-theme.h"
#include "barkit/gowl-bar-toast.h"

/*
 * gowl builds with G_LOG_USE_STRUCTURED, and g_test_expect_message does
 * not intercept structured logs -- so a test that deliberately provokes
 * a warning has to lift GTest's fatal-warning mask instead of declaring
 * the message.  Lifted narrowly, around the one call, so an unexpected
 * warning anywhere else still fails the suite.
 */
static GLogLevelFlags saved_fatal_mask;

static void
allow_warnings(void)
{
	saved_fatal_mask = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);
}

static void
restore_warnings(void)
{
	g_log_set_always_fatal(saved_fatal_mask);
}

/* ---- Theme ---- */

static void
test_theme_defaults_to_mocha(void)
{
	g_autoptr(GowlBarTheme) theme = NULL;
	const gdouble *base;

	theme = gowl_bar_theme_new();
	base = gowl_bar_theme_color(theme, GOWL_BAR_COLOR_BASE);

	/* #1e1e2e */
	g_assert_cmpfloat(ABS(base[0] - 0.118), <, 0.01);
	g_assert_cmpfloat(ABS(base[2] - 0.180), <, 0.01);
}

static void
test_theme_follows_the_palette(void)
{
	g_autoptr(GowlBarTheme) theme = NULL;
	g_autoptr(GowlPalette) latte = NULL;
	const gdouble *base;

	latte = gowl_palette_new_builtin("latte");
	theme = gowl_bar_theme_new_for_palette(latte);
	base = gowl_bar_theme_color(theme, GOWL_BAR_COLOR_BASE);

	/* Latte's base is light; the point of the palette wiring is that
	   no plugin had to know that. */
	g_assert_cmpfloat(base[0], >, 0.8);
}

static void
test_pinned_colours_survive_a_palette_change(void)
{
	g_autoptr(GowlBarTheme) theme = NULL;
	g_autoptr(GowlPalette) latte = NULL;
	const gdouble *accent;

	theme = gowl_bar_theme_new();
	gowl_bar_theme_set_color(theme, GOWL_BAR_COLOR_ACCENT, "#ff0000");
	gowl_bar_theme_apply_palette(theme, latte =
		gowl_palette_new_builtin("latte"));

	/* An explicit choice in the config is not something a theme
	   switch may quietly undo. */
	accent = gowl_bar_theme_color(theme, GOWL_BAR_COLOR_ACCENT);
	g_assert_cmpfloat(accent[0], >, 0.99);
	g_assert_cmpfloat(accent[1], <, 0.01);
}

static void
test_theme_settings(void)
{
	g_autoptr(GowlBarTheme) theme = NULL;
	const gdouble *accent;

	theme = gowl_bar_theme_new();

	g_assert_true(gowl_bar_theme_apply_setting(theme, "theme-accent",
	                                           "#00ff00"));
	accent = gowl_bar_theme_color(theme, GOWL_BAR_COLOR_ACCENT);
	g_assert_cmpfloat(accent[1], >, 0.99);

	/* A role may be named rather than spelled out in hex. */
	g_assert_true(gowl_bar_theme_apply_setting(theme, "theme-accent",
	                                           "red"));

	g_assert_true(gowl_bar_theme_apply_setting(theme,
		"theme-metric-radius", "12"));
	g_assert_cmpint(gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_RADIUS),
	                ==, 12);

	g_assert_true(gowl_bar_theme_apply_setting(theme, "theme-font",
	                                           "Iosevka 12"));
	g_assert_cmpstr(gowl_bar_theme_get_font(theme), ==, "Iosevka 12");

	g_assert_false(gowl_bar_theme_apply_setting(theme, "widgets", "cpu"));
	g_assert_false(gowl_bar_theme_apply_setting(theme, "theme-nonsense",
	                                            "1"));
}

static void
test_scale_moves_every_metric(void)
{
	g_autoptr(GowlBarTheme) theme = NULL;
	gint before, after;

	theme = gowl_bar_theme_new();
	before = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_ROW_HEIGHT);
	gowl_bar_theme_set_scale(theme, 2.0);
	after = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_ROW_HEIGHT);

	g_assert_cmpint(after, ==, before * 2);

	/* A hairline must not round away to nothing at a small scale. */
	gowl_bar_theme_set_scale(theme, 0.5);
	g_assert_cmpint(gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_BORDER),
	                >=, 1);
}

/* ---- The vtable plugin ---- */

typedef struct {
	gint polls;
	gint clicks;
	gint panels;
	gint actions;
	gboolean destroyed;
} ProbeData;

static ProbeData probe;

static gpointer
probe_create(GowlBarPlugin *plugin)
{
	(void)plugin;
	memset(&probe, 0, sizeof(probe));
	return &probe;
}

static void
probe_destroy(GowlBarPlugin *plugin, gpointer data)
{
	ProbeData *p = data;

	(void)plugin;
	p->destroyed = TRUE;
}

static void
probe_poll(GowlBarPlugin *plugin, gpointer data)
{
	ProbeData *p = data;

	p->polls++;
	gowl_bar_plugin_set_label(plugin, "probed");
}

static gboolean
probe_click(GowlBarPlugin *plugin, gpointer data, guint button, gint x,
            gint y, guint modifiers)
{
	ProbeData *p = data;

	(void)plugin;
	(void)button;
	(void)x;
	(void)y;
	(void)modifiers;
	p->clicks++;
	return TRUE;
}

static GowlBarPanel *
probe_panel(GowlBarPlugin *plugin, gpointer data)
{
	ProbeData *p = data;
	GowlBarPanel *panel;

	(void)plugin;
	p->panels++;
	panel = gowl_bar_panel_new();
	gowl_bar_panel_add_label(panel, "hello");
	return panel;
}

static void
probe_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
             gint index, gdouble value, guint button)
{
	ProbeData *p = data;

	(void)plugin;
	(void)item_id;
	(void)index;
	(void)value;
	(void)button;
	p->actions++;
}

static const GowlBarPluginVTable probe_vtable = {
	sizeof(GowlBarPluginVTable),
	probe_create, probe_destroy,
	NULL, NULL, NULL,
	NULL, probe_poll, NULL,
	NULL, NULL,
	probe_click, NULL,
	probe_panel, probe_action,
	NULL, NULL
};

/* A vtable that claims to be shorter than it is, standing in for a
   plugin built against an older barkit. */
static const GowlBarPluginVTable short_vtable = {
	G_STRUCT_OFFSET(GowlBarPluginVTable, poll) + sizeof(gpointer),
	probe_create, probe_destroy,
	NULL, NULL, NULL,
	NULL, probe_poll, NULL,
	NULL, NULL,
	probe_click, NULL,
	probe_panel, probe_action,
	NULL, NULL
};

static void
test_proxy_forwards_the_vtable(void)
{
	GowlBarPlugin *plugin;
	GowlBarPanel *panel;

	plugin = gowl_bar_plugin_proxy_new("probe", "Probe", "A test plugin",
	                                   &probe_vtable);
	g_assert_true(gowl_bar_plugin_activate(plugin, NULL));
	g_assert_cmpstr(gowl_bar_plugin_get_title(plugin), ==, "Probe");

	gowl_bar_plugin_poll(plugin);
	g_assert_cmpint(probe.polls, ==, 1);
	{
		g_autofree gchar *label = NULL;

		label = gowl_bar_plugin_dup_label(plugin);
		g_assert_cmpstr(label, ==, "probed");
	}

	g_assert_true(gowl_bar_plugin_on_click(plugin, 1, 0, 0, 0));
	g_assert_cmpint(probe.clicks, ==, 1);

	g_assert_true(gowl_bar_plugin_has_panel(plugin));
	panel = gowl_bar_plugin_build_panel(plugin);
	g_assert_nonnull(panel);
	g_object_unref(panel);

	gowl_bar_plugin_panel_action(plugin, "x", -1, 0.0, 1);
	g_assert_cmpint(probe.actions, ==, 1);

	gowl_bar_plugin_deactivate(plugin);
	g_object_unref(plugin);
	g_assert_true(probe.destroyed);
}

static void
test_a_short_vtable_still_loads(void)
{
	GowlBarPlugin *plugin;

	/* This is the whole reason the vtable carries its own size: a
	   plugin compiled before the struct grew must keep working, and
	   the fields past its size must never be read. */
	plugin = gowl_bar_plugin_proxy_new("old", NULL, NULL, &short_vtable);
	g_assert_true(gowl_bar_plugin_activate(plugin, NULL));

	gowl_bar_plugin_poll(plugin);
	g_assert_cmpint(probe.polls, ==, 1);

	/* click and panel lie beyond the declared size, so the proxy must
	   not reach them even though the memory happens to be valid. */
	g_assert_false(gowl_bar_plugin_on_click(plugin, 1, 0, 0, 0));
	g_assert_false(gowl_bar_plugin_has_panel(plugin));

	g_object_unref(plugin);
}

static void
test_settings_accessors(void)
{
	GowlBarPlugin *plugin;

	plugin = gowl_bar_plugin_proxy_new("s", NULL, NULL, &probe_vtable);

	gowl_bar_plugin_set_setting(plugin, "count", "42");
	gowl_bar_plugin_set_setting(plugin, "ratio", "0.5");
	gowl_bar_plugin_set_setting(plugin, "on", "yes");
	gowl_bar_plugin_set_setting(plugin, "off", "nil");

	g_assert_cmpint(gowl_bar_plugin_get_setting_int(plugin, "count", 0),
	                ==, 42);
	g_assert_cmpfloat(
		gowl_bar_plugin_get_setting_double(plugin, "ratio", 0.0),
		==, 0.5);
	/* Values reach a plugin from YAML, from Elisp and from IPC, and
	   each spells booleans its own way. */
	g_assert_true(gowl_bar_plugin_get_setting_bool(plugin, "on", FALSE));
	g_assert_false(gowl_bar_plugin_get_setting_bool(plugin, "off", TRUE));
	g_assert_true(gowl_bar_plugin_get_setting_bool(plugin, "absent",
	                                               TRUE));

	g_object_unref(plugin);
}

/* ---- Registry ---- */

static void
test_registry_instantiates_specs(void)
{
	g_autoptr(GowlBarRegistry) registry = NULL;
	g_autofree gchar *dir = NULL;
	GowlBarPlugin *plugin;

	dir = g_dir_make_tmp("gowl-bar-test-XXXXXX", NULL);
	registry = gowl_bar_registry_new(dir);

	gowl_bar_registry_register_vtable(registry, "probe", "Probe", NULL,
	                                  &probe_vtable);
	gowl_bar_registry_register_alias(registry, "p", "probe");

	g_assert_true(gowl_bar_registry_has(registry, "probe"));
	g_assert_true(gowl_bar_registry_has(registry, "p"));
	g_assert_false(gowl_bar_registry_has(registry, "nope"));

	/* `name:param@interval' is the whole configuration grammar for a
	   widget, and every piece of it lands as a setting. */
	plugin = gowl_bar_registry_instantiate(registry, "probe:/var@15",
	                                       NULL);
	g_assert_nonnull(plugin);
	g_assert_cmpstr(gowl_bar_plugin_get_id(plugin), ==, "probe:/var@15");
	g_assert_cmpstr(gowl_bar_plugin_get_setting(plugin, "name"), ==,
	                "probe");
	g_assert_cmpstr(gowl_bar_plugin_get_setting(plugin, "param"), ==,
	                "/var");
	g_assert_cmpint(gowl_bar_plugin_get_interval(plugin), ==, 15);
	g_object_unref(plugin);

	/* An alias produces the same plugin under the name that was
	   asked for, so two instances stay separately addressable. */
	plugin = gowl_bar_registry_instantiate(registry, "p", NULL);
	g_assert_nonnull(plugin);
	g_assert_cmpstr(gowl_bar_plugin_get_setting(plugin, "name"), ==,
	                "probe");
	g_object_unref(plugin);

	g_assert_null(gowl_bar_registry_instantiate(registry, "missing",
	                                            NULL));
}

static void
test_a_command_with_an_at_sign_still_parses(void)
{
	g_autoptr(GowlBarRegistry) registry = NULL;
	GowlBarPlugin *plugin;

	registry = gowl_bar_registry_new(NULL);
	gowl_bar_registry_register_vtable(registry, "cmd", NULL, NULL,
	                                  &probe_vtable);

	/* The last `@' wins, so `ssh me@host' keeps its own. */
	plugin = gowl_bar_registry_instantiate(registry,
		"cmd:ssh me@host@30", NULL);
	g_assert_nonnull(plugin);
	g_assert_cmpstr(gowl_bar_plugin_get_setting(plugin, "param"), ==,
	                "ssh me@host");
	g_assert_cmpint(gowl_bar_plugin_get_interval(plugin), ==, 30);
	g_object_unref(plugin);

	/* And a bare `@' that is not a number is left alone entirely. */
	plugin = gowl_bar_registry_instantiate(registry, "cmd:ssh me@host",
	                                       NULL);
	g_assert_nonnull(plugin);
	g_assert_cmpstr(gowl_bar_plugin_get_setting(plugin, "param"), ==,
	                "ssh me@host");
	g_object_unref(plugin);
}

static void
test_quarantine_survives_a_restart(void)
{
	g_autofree gchar *dir = NULL;
	g_autoptr(GError) error = NULL;

	dir = g_dir_make_tmp("gowl-bar-quarantine-XXXXXX", NULL);

	{
		g_autoptr(GowlBarRegistry) first = NULL;

		first = gowl_bar_registry_new(dir);
		gowl_bar_registry_register_vtable(first, "bad", NULL, NULL,
		                                  &probe_vtable);
		gowl_bar_registry_quarantine(first, "bad", "it exploded");
		g_assert_true(gowl_bar_registry_is_quarantined(first, "bad"));
		g_assert_null(gowl_bar_registry_instantiate(first, "bad",
		                                            &error));
		g_assert_nonnull(error);
	}

	{
		g_autoptr(GowlBarRegistry) second = NULL;
		g_autofree gchar *culprit = NULL;

		/* The whole point: a new session must remember. */
		second = gowl_bar_registry_new(dir);
		culprit = gowl_bar_registry_recover_journal(second);
		g_assert_null(culprit);   /* nothing was mid-load */
		g_assert_true(gowl_bar_registry_is_quarantined(second, "bad"));
		g_assert_cmpstr(
			gowl_bar_registry_quarantine_reason(second, "bad"),
			==, "it exploded");

		g_assert_true(gowl_bar_registry_clear_quarantine(second,
		                                                 "bad"));
		g_assert_false(gowl_bar_registry_is_quarantined(second,
		                                                "bad"));
	}
}

static void
test_an_unfinished_load_is_recovered(void)
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *journal = NULL;
	g_autoptr(GKeyFile) kf = NULL;
	g_autoptr(GowlBarRegistry) registry = NULL;
	g_autofree gchar *culprit = NULL;

	dir = g_dir_make_tmp("gowl-bar-journal-XXXXXX", NULL);
	journal = g_build_filename(dir, "bar-plugins.journal", NULL);

	/* Exactly what the journal looks like after a plugin took the
	   session down while loading. */
	kf = g_key_file_new();
	g_key_file_set_string(kf, "loading", "explodey",
	                      "/tmp/explodey.so");
	g_assert_true(g_key_file_save_to_file(kf, journal, NULL));

	registry = gowl_bar_registry_new(dir);
	allow_warnings();
	culprit = gowl_bar_registry_recover_journal(registry);
	restore_warnings();

	g_assert_cmpstr(culprit, ==, "explodey");
	g_assert_true(gowl_bar_registry_is_quarantined(registry, "explodey"));
}

/* ---- The fault guard ---- */

static void
guard_ok_body(gpointer data)
{
	gint *counter = data;

	(*counter)++;
}

static void
guard_segv_body(gpointer data)
{
	volatile gint *bad = NULL;

	(void)data;
	/* Deliberate: the guard exists for exactly this. */
	*bad = 1;
}

static void
guard_abort_body(gpointer data)
{
	(void)data;
	/* g_error and a failed g_assert both land here, and a plugin's
	   failed assertion must not be a logout. */
	abort();
}

static void
test_guard_passes_a_clean_call_through(void)
{
	gint counter = 0;
	gint signo = -1;

	gowl_bar_guard_init();
	g_assert_true(gowl_bar_guard_call(guard_ok_body, &counter, &signo,
	                                  "test"));
	g_assert_cmpint(counter, ==, 1);
	g_assert_cmpint(signo, ==, 0);
}

static void
test_guard_catches_a_segfault(void)
{
	gint signo = 0;
	gint counter = 0;

	gowl_bar_guard_init();
	if (!gowl_bar_guard_is_available()) {
		g_test_skip("the fault guard could not install its handlers");
		return;
	}

	allow_warnings();
	g_assert_false(gowl_bar_guard_call(guard_segv_body, NULL, &signo,
	                                   "a deliberate fault"));
	restore_warnings();
	g_assert_cmpint(signo, ==, SIGSEGV);

	/* And the process is still usable afterwards, which is the only
	   thing that makes catching it worth anything. */
	g_assert_true(gowl_bar_guard_call(guard_ok_body, &counter, NULL,
	                                  "after"));
	g_assert_cmpint(counter, ==, 1);
}

static void
test_guard_catches_an_abort(void)
{
	gint signo = 0;

	gowl_bar_guard_init();
	if (!gowl_bar_guard_is_available()) {
		g_test_skip("the fault guard could not install its handlers");
		return;
	}

	allow_warnings();
	g_assert_false(gowl_bar_guard_call(guard_abort_body, NULL, &signo,
	                                   "a deliberate abort"));
	restore_warnings();
	g_assert_cmpint(signo, ==, SIGABRT);
}

/* ---- Toasts ---- */

static void
test_toast_expiry(void)
{
	g_autoptr(GowlBarToastStack) stack = NULL;
	GowlBarToast *toast;
	gint64 now;

	stack = gowl_bar_toast_stack_new();
	now = g_get_monotonic_time();

	toast = gowl_bar_toast_new("Hello", "Body");
	gowl_bar_toast_set_timeout(toast, 1000);
	gowl_bar_toast_set_created(toast, now);
	gowl_bar_toast_stack_push(stack, toast);

	g_assert_cmpuint(gowl_bar_toast_stack_size(stack), ==, 1);
	g_assert_false(gowl_bar_toast_stack_expire(stack, now + 500 * 1000));
	g_assert_true(gowl_bar_toast_stack_expire(stack, now + 2000 * 1000));
	g_assert_cmpuint(gowl_bar_toast_stack_size(stack), ==, 0);
}

static void
test_a_critical_toast_does_not_expire(void)
{
	g_autoptr(GowlBarToastStack) stack = NULL;
	GowlBarToast *toast;
	gint64 now;
	gint i;

	stack = gowl_bar_toast_stack_new();
	now = g_get_monotonic_time();

	toast = gowl_bar_toast_new("Plugin held back", "It faulted");
	gowl_bar_toast_set_urgency(toast, GOWL_BAR_TOAST_CRITICAL);
	gowl_bar_toast_set_created(toast, now);
	gowl_bar_toast_stack_push(stack, toast);

	/* Setting critical also clears the timeout: a crash report that
	   scrolls away before you look at the screen is no report. */
	g_assert_cmpint(gowl_bar_toast_get_timeout(toast), ==, 0);
	g_assert_false(gowl_bar_toast_stack_expire(stack,
		now + 3600LL * 1000 * 1000));

	/* Nor may a burst of chatter push it off the stack. */
	for (i = 0; i < 20; i++) {
		gowl_bar_toast_stack_push(stack,
			gowl_bar_toast_new("chatter", NULL));
	}
	g_assert_nonnull(gowl_bar_toast_stack_get(stack, 0));
	g_assert_cmpint(gowl_bar_toast_get_urgency(
		gowl_bar_toast_stack_get(stack, 0)), ==,
		GOWL_BAR_TOAST_CRITICAL);
}

static void
test_toast_carries_its_panel_target(void)
{
	GowlBarToast *toast;

	/* The feature the whole toast layer exists for: a notification
	   that hands you the dropdown that fixes it. */
	toast = gowl_bar_toast_new("Setup Wi-Fi",
	                           "Click to configure the network");
	gowl_bar_toast_set_panel(toast, "network");
	g_assert_cmpstr(gowl_bar_toast_get_panel(toast), ==, "network");
	gowl_bar_toast_free(toast);
}

/* ── A stub host ─────────────────────────────────────────────────── */

/*
 * The smallest thing that satisfies GowlBarHost, so the plugin->host
 * calls can be tested without a compositor.
 *
 * It exists for gowl_bar_plugin_set_bar_setting(), which is the call
 * that lets a panel control change the BAR rather than merely record a
 * wish about it -- the display plugin's text-size buttons used to write
 * a setting nobody read, so they moved and nothing happened.  A stub
 * host is the only way to assert that the call actually arrives.
 */
typedef struct {
	GObject parent;
	gchar  *last_key;
	gchar  *last_value;
	gboolean accept;
} StubHost;

typedef struct { GObjectClass parent; } StubHostClass;

static void stub_host_iface_init(GowlBarHostInterface *iface);

G_DEFINE_TYPE_WITH_CODE(StubHost, stub_host, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_BAR_HOST, stub_host_iface_init))

static gboolean
stub_host_set_bar_setting(GowlBarHost *host, const gchar *key,
                          const gchar *value)
{
	StubHost *self = (StubHost *)host;

	g_free(self->last_key);
	g_free(self->last_value);
	self->last_key = g_strdup(key);
	self->last_value = g_strdup(value);
	return self->accept;
}

static void
stub_host_iface_init(GowlBarHostInterface *iface)
{
	iface->set_bar_setting = stub_host_set_bar_setting;
}

static void
stub_host_finalize(GObject *o)
{
	StubHost *self = (StubHost *)o;

	g_free(self->last_key);
	g_free(self->last_value);
	G_OBJECT_CLASS(stub_host_parent_class)->finalize(o);
}

static void
stub_host_class_init(StubHostClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = stub_host_finalize;
}

static void stub_host_init(StubHost *self) { self->accept = TRUE; }

/*
 * A plugin's bar-setting change reaches the host, carries the key and
 * value unchanged, and reports the host's verdict rather than always
 * claiming success.
 */
static void
test_plugin_reaches_the_host_for_bar_settings(void)
{
	g_autoptr(GowlBarRegistry) registry = NULL;
	GowlBarPlugin *plugin;
	StubHost      *host;

	registry = gowl_bar_registry_new(NULL);
	gowl_bar_registry_register_vtable(registry, "probe", NULL, NULL,
	                                  &probe_vtable);
	plugin = gowl_bar_registry_instantiate(registry, "probe", NULL);
	g_assert_nonnull(plugin);

	host = g_object_new(stub_host_get_type(), NULL);
	gowl_bar_plugin_set_host(plugin, GOWL_BAR_HOST(host));

	g_assert_true(gowl_bar_plugin_set_bar_setting(plugin,
	                                              "theme-scale", "1.20"));
	g_assert_cmpstr(host->last_key, ==, "theme-scale");
	g_assert_cmpstr(host->last_value, ==, "1.20");

	/* A host that refuses must be reported as refusing: the caller
	   decides whether to tell the user it did nothing. */
	host->accept = FALSE;
	g_assert_false(gowl_bar_plugin_set_bar_setting(plugin,
	                                               "theme-scale", "2.0"));

	g_object_unref(plugin);
	g_object_unref(host);
}

/* With no host at all the call must fail rather than crash: a plugin
   can be instantiated and queried before it is ever adopted. */
static void
test_bar_setting_without_a_host(void)
{
	g_autoptr(GowlBarRegistry) registry = NULL;
	GowlBarPlugin *plugin;

	registry = gowl_bar_registry_new(NULL);
	gowl_bar_registry_register_vtable(registry, "probe", NULL, NULL,
	                                  &probe_vtable);
	plugin = gowl_bar_registry_instantiate(registry, "probe", NULL);
	g_assert_nonnull(plugin);

	g_assert_false(gowl_bar_plugin_set_bar_setting(plugin,
	                                               "theme-scale", "1.0"));
	g_object_unref(plugin);
}


int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-theme/defaults", test_theme_defaults_to_mocha);
	g_test_add_func("/bar-theme/follows-palette",
	                test_theme_follows_the_palette);
	g_test_add_func("/bar-theme/pinned-survive",
	                test_pinned_colours_survive_a_palette_change);
	g_test_add_func("/bar-theme/settings", test_theme_settings);
	g_test_add_func("/bar-theme/scale", test_scale_moves_every_metric);

	g_test_add_func("/bar-plugin/proxy-forwards",
	                test_proxy_forwards_the_vtable);
	g_test_add_func("/bar-plugin/short-vtable",
	                test_a_short_vtable_still_loads);
	g_test_add_func("/bar-plugin/settings", test_settings_accessors);

	g_test_add_func("/bar-registry/instantiate",
	                test_registry_instantiates_specs);
	g_test_add_func("/bar-registry/at-sign-in-command",
	                test_a_command_with_an_at_sign_still_parses);
	g_test_add_func("/bar-registry/quarantine-persists",
	                test_quarantine_survives_a_restart);
	g_test_add_func("/bar-registry/journal-recovery",
	                test_an_unfinished_load_is_recovered);

	g_test_add_func("/bar-guard/clean-call",
	                test_guard_passes_a_clean_call_through);
	g_test_add_func("/bar-guard/catches-segv",
	                test_guard_catches_a_segfault);
	g_test_add_func("/bar-guard/catches-abort",
	                test_guard_catches_an_abort);

	g_test_add_func("/bar-toast/expiry", test_toast_expiry);
	g_test_add_func("/bar-toast/critical-persists",
	                test_a_critical_toast_does_not_expire);
	g_test_add_func("/bar-toast/panel-target",
	                test_toast_carries_its_panel_target);

	g_test_add_func("/bar-plugin/host-bar-setting",
	                test_plugin_reaches_the_host_for_bar_settings);
	g_test_add_func("/bar-plugin/bar-setting-without-host",
	                test_bar_setting_without_a_host);
	return g_test_run();
}
