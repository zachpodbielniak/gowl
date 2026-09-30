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
 * test-macro-runner.c - The macro module in a real compositor.
 *
 * A headless compositor with the real macro.so loaded, and macro files
 * the test writes into a directory of its own, compiled by crispy like
 * any user's.  Steps are observed where they end: a `custom' action
 * lands in the compositor's custom action handler, which records it with
 * a timestamp.  The wl event loop is pumped by hand, so the timeline
 * plays exactly as it does in a session.
 *
 * Covers: a result and a failure; timeline order and timing; stop by
 * name and Super+Escape; crash, abort and loop contained and held back,
 * refused until cleared; a macro granting itself more time; threaded
 * runs -- steps hopping to the compositor, a crash, a cancel, and a
 * watchdog firing mid-sleep without wedging a lock; max-running,
 * reentrant and max-steps; journal recovery; fault notifications
 * (toast-requested, on-fault, on-fault-custom); timer and event
 * triggers; a C-registered macro; a defined alias; the input
 * remapper's {macro:} target; the D-Bus service; the log file.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>

#include "gowl.h"
#include "core/gowl-core-private.h"

/* ── What came out ──────────────────────────────────────────────── */

typedef struct {
	gchar  *arg;
	gint64  when;   /* monotonic, microseconds */
} Action;

static GPtrArray *actions;   /* Action*, in order */
static GPtrArray *toasts;    /* gchar* */
static GMutex     actions_lock;

static void
action_free(gpointer data)
{
	Action *a = data;

	g_free(a->arg);
	g_free(a);
}

static gboolean
custom_action(
	GowlCompositor *compositor,
	const gchar    *arg,
	gpointer        data
){
	Action *a;

	(void)compositor;
	(void)data;
	a = g_new0(Action, 1);
	a->arg = g_strdup(arg);
	a->when = g_get_monotonic_time();
	g_mutex_lock(&actions_lock);
	g_ptr_array_add(actions, a);
	g_mutex_unlock(&actions_lock);
	return TRUE;
}

static void
on_toast(
	GowlCompositor *compositor,
	GowlMonitor    *monitor,
	const gchar    *text,
	gpointer        data
){
	(void)compositor;
	(void)monitor;
	(void)data;
	g_ptr_array_add(toasts, g_strdup(text));
}

/* ── The rig ────────────────────────────────────────────────────── */

typedef struct {
	gchar             *runtime;
	gchar             *macros;     /* the macro directory */
	gchar             *journal;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
	GowlModule        *macro;
	GTestDBus         *bus;
	GPtrArray         *keyboards;
	gboolean           up;
} Rig;

enum {
	RIG_PLAIN   = 0,
	RIG_REMAP   = 1 << 0,
	RIG_DBUS    = 1 << 1
};

static GLogLevelFlags saved_mask;

static void
allow_warnings(void)
{
	saved_mask = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);
}

static void
restore_warnings(void)
{
	g_log_set_always_fatal(saved_mask);
}

static void
rm_rf(const gchar *path)
{
	g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
	const gchar *e;

	if (dir != NULL) {
		while ((e = g_dir_read_name(dir)) != NULL) {
			g_autofree gchar *p = g_build_filename(path, e, NULL);

			if (g_file_test(p, G_FILE_TEST_IS_DIR)
			    && !g_file_test(p, G_FILE_TEST_IS_SYMLINK))
				rm_rf(p);
			else
				g_unlink(p);
		}
	}
	g_rmdir(path);
}

/* Settings for the module, as `modules: macro:' would give them. */
static void
configure(
	Rig         *r,
	const gchar *first_key,
	...
){
	g_autoptr(GHashTable) s = NULL;
	const gchar *k;
	va_list ap;

	s = g_hash_table_new(g_str_hash, g_str_equal);
	va_start(ap, first_key);
	for (k = first_key; k != NULL; k = va_arg(ap, const gchar *))
		g_hash_table_insert(s, (gpointer)k, va_arg(ap, gchar *));
	va_end(ap);
	gowl_module_configure(r->macro, s);
}

static void
rig_setup(
	Rig           *r,
	gconstpointer  data
){
	gint flags;
	const gchar *parent;
	g_autofree gchar *so = NULL;
	GError *error = NULL;

	flags = GPOINTER_TO_INT(data);
	memset(r, 0, sizeof *r);
	actions = g_ptr_array_new_with_free_func(action_free);
	toasts = g_ptr_array_new_with_free_func(g_free);
	r->keyboards = g_ptr_array_new();

	/* A bus of our own first: g_test_dbus_up unsets XDG_RUNTIME_DIR */
	if (flags & RIG_DBUS) {
		r->bus = g_test_dbus_new(G_TEST_DBUS_NONE);
		g_test_dbus_up(r->bus);
	}

	parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-macro-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	r->macros = g_build_filename(r->runtime, "macros", NULL);
	g_mkdir(r->macros, 0700);
	r->journal = g_build_filename(r->runtime, "macros.journal", NULL);
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	r->config = gowl_config_new();
	gowl_config_set_lock_command(r->config, "");
	r->modules = gowl_module_manager_new();
	if (!gowl_module_manager_load_module(r->modules, GOWL_TEST_MACRO_MODULE,
	                                     &error)) {
		g_test_skip("macro.so did not load");
		g_clear_error(&error);
		return;
	}
	if (flags & RIG_REMAP) {
		so = g_build_filename(GOWL_TEST_MODULE_DIR, "inputremap.so", NULL);
		if (!gowl_module_manager_load_module(r->modules, so, &error)) {
			g_test_skip("inputremap.so did not load");
			g_clear_error(&error);
			return;
		}
	}
	gowl_module_manager_activate_all(r->modules);
	r->macro = gowl_module_manager_find_module(r->modules, "macro");
	g_assert_nonnull(r->macro);

	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_test_skip("no headless compositor here");
		g_clear_error(&error);
		return;
	}
	gowl_module_manager_dispatch_startup(r->modules, r->compositor);
	gowl_compositor_set_custom_action_handler(r->compositor, custom_action,
	                                          NULL);
	g_signal_connect(r->compositor, "toast-requested", G_CALLBACK(on_toast),
	                 NULL);
	/* This test's macros, this test's journal, quiet unless a fault */
	configure(r, "macro-dir", r->macros, "journal", r->journal,
	          "timeout-ms", "2000", NULL);
	r->up = TRUE;
}

static void
rig_teardown(
	Rig           *r,
	gconstpointer  data
){
	guint i;

	(void)data;
	for (i = 0; i < r->keyboards->len; i++) {
		struct wlr_keyboard *kb = g_ptr_array_index(r->keyboards, i);

		wlr_keyboard_finish(kb);
		g_free(kb);
	}
	g_ptr_array_unref(r->keyboards);
	if (r->compositor != NULL && r->modules != NULL)
		gowl_module_manager_dispatch_shutdown(r->modules, r->compositor);
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	if (r->bus != NULL) {
		g_test_dbus_down(r->bus);
		g_object_unref(r->bus);
	}
	rm_rf(r->runtime);
	g_free(r->runtime);
	g_free(r->macros);
	g_free(r->journal);
	g_ptr_array_unref(actions);
	g_ptr_array_unref(toasts);
}

#define RIG_UP(r) do { if (!(r)->up) return; } while (0)

/* A macro file: @body is the inside of gowl_macro_run(). */
static void
write_macro(
	Rig         *r,
	const gchar *name,
	const gchar *defines,
	const gchar *body
){
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;

	path = g_strdup_printf("%s/%s.c", r->macros, name);
	text = g_strdup_printf(
		"#include <gowl/gowl.h>\n"
		"#include <signal.h>\n"
		"#include <stdlib.h>\n"
		"%s\n"
		"static volatile guint64 sink;\n"
		"G_MODULE_EXPORT gboolean\n"
		"gowl_macro_run(GowlMacroContext *ctx)\n"
		"{\n"
		"\t(void)sink;\n"
		"%s\n"
		"}\n", defines != NULL ? defines : "", body);
	g_assert_true(g_file_set_contents(path, text, -1, NULL));
}

static gchar *
cmd(
	Rig         *r,
	const gchar *line
){
	gchar *reply = gowl_compositor_run_command(r->compositor, line);

	g_assert_nonnull(reply);
	g_strchomp(reply);
	return reply;
}

static void
assert_ok(
	Rig         *r,
	const gchar *line
){
	g_autofree gchar *reply = cmd(r, line);

	if (!g_str_has_prefix(reply, "OK"))
		g_test_message("%s -> %s", line, reply);
	g_assert_true(g_str_has_prefix(reply, "OK"));
}

/* Runs the event loop for @ms, or until @done says so. */
static void
pump_until(
	Rig      *r,
	guint     ms,
	gboolean (*done)(Rig *r, gpointer data),
	gpointer  data
){
	struct wl_event_loop *loop;
	gint64 end;

	loop = gowl_compositor_get_event_loop(r->compositor);
	end = g_get_monotonic_time() + (gint64)ms * 1000;
	while (g_get_monotonic_time() < end) {
		if (done != NULL && done(r, data))
			return;
		wl_event_loop_dispatch(loop, 5);
		while (g_main_context_iteration(NULL, FALSE))
			;
	}
}

static void
pump(
	Rig   *r,
	guint  ms
){
	pump_until(r, ms, NULL, NULL);
}

static guint
n_actions_named(const gchar *arg)
{
	guint i;
	guint n = 0;

	g_mutex_lock(&actions_lock);
	for (i = 0; i < actions->len; i++) {
		Action *a = g_ptr_array_index(actions, i);

		if (g_strcmp0(a->arg, arg) == 0)
			n++;
	}
	g_mutex_unlock(&actions_lock);
	return n;
}

static gboolean
action_seen(
	Rig      *r,
	gpointer  data
){
	(void)r;
	return n_actions_named(data) > 0;
}

static gboolean
nothing_running(
	Rig      *r,
	gpointer  data
){
	g_autofree gchar *status = cmd(r, "macro-status");

	(void)data;
	return strstr(status, "\"running\":[]") != NULL;
}

static gboolean
held_back(
	Rig      *r,
	gpointer  name
){
	g_autofree gchar *line = g_strdup_printf("macro-info %s", (gchar *)name);
	g_autofree gchar *info = cmd(r, line);

	return strstr(info, "\"held-back\":true") != NULL;
}

/* ── Results, failures, the reply line ──────────────────────────── */

static void
test_result_and_failure(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	g_autofree gchar *d = NULL;
	g_autofree gchar *e = NULL;

	(void)data;
	RIG_UP(r);
	write_macro(r, "quick", NULL,
	            "\tgowl_macro_set_result(ctx, \"quick-result\");\n"
	            "\treturn TRUE;");
	write_macro(r, "silent", NULL, "\t(void)ctx;\n\treturn TRUE;");
	write_macro(r, "fails", NULL,
	            "\tgowl_macro_set_result(ctx, \"nope\");\n\treturn FALSE;");
	write_macro(r, "argv", NULL,
	            "\tg_autofree gchar *s = g_strdup_printf(\"%u %s %s %d\",\n"
	            "\t\tgowl_macro_get_argc(ctx), gowl_macro_get_arg(ctx, 1),\n"
	            "\t\tgowl_macro_get_trigger_detail(ctx),\n"
	            "\t\t(gint)gowl_macro_get_trigger(ctx));\n"
	            "\tgowl_macro_set_result(ctx, s);\n\treturn TRUE;");

	a = cmd(r, "macro-run quick");
	g_assert_cmpstr(a, ==, "OK quick: quick-result");
	b = cmd(r, "macro-run silent.c");
	g_assert_cmpstr(b, ==, "OK ran silent");
	c = cmd(r, "macro-run fails");
	g_assert_cmpstr(c, ==, "ERROR fails failed: nope");
	/* shell-parsed args; the trigger is recorded */
	d = cmd(r, "macro-run --trigger=event --detail=why argv one 'two words'");
	g_assert_cmpstr(d, ==, "OK argv: 2 two words why 3");
	e = cmd(r, "macro-run no-such-macro");
	g_assert_true(g_str_has_prefix(e, "ERROR"));
	g_assert_nonnull(strstr(e, "no-such-macro"));
	/* a failure is not a fault: nothing is held back */
	g_assert_false(held_back(r, "fails"));
}

/* ── The timeline ───────────────────────────────────────────────── */

static void
test_timeline_order_and_timing(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *reply = NULL;
	Action *one;
	Action *two;
	gint64 start;

	(void)data;
	RIG_UP(r);
	write_macro(r, "order", NULL,
	            "\tgowl_macro_action(ctx, GOWL_ACTION_CUSTOM, \"one\");\n"
	            "\tgowl_macro_wait(ctx, 150);\n"
	            "\tgowl_macro_action(ctx, GOWL_ACTION_CUSTOM, \"two\");\n"
	            "\treturn TRUE;");
	start = g_get_monotonic_time();
	reply = cmd(r, "macro-run order");
	g_assert_true(g_str_has_prefix(reply, "OK started order id="));
	g_assert_nonnull(strstr(reply, "steps=3"));
	/* nothing has played yet: the body only queued */
	g_assert_cmpuint(actions->len, ==, 0);

	pump_until(r, 2000, action_seen, "two");
	g_assert_cmpuint(actions->len, ==, 2);
	one = g_ptr_array_index(actions, 0);
	two = g_ptr_array_index(actions, 1);
	g_assert_cmpstr(one->arg, ==, "one");
	g_assert_cmpstr(two->arg, ==, "two");
	g_assert_cmpint(two->when - one->when, >=, 140 * 1000);
	g_assert_cmpint(two->when - start, <, 1500 * 1000);
	pump_until(r, 500, nothing_running, NULL);
	g_assert_true(nothing_running(r, NULL));
}

static void
test_stop_and_super_escape(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	GowlKeybindHandler *kb;

	(void)data;
	RIG_UP(r);
	write_macro(r, "slow", NULL,
	            "\tgowl_macro_wait(ctx, 5000);\n"
	            "\tgowl_macro_action(ctx, GOWL_ACTION_CUSTOM, \"late\");\n"
	            "\treturn TRUE;");

	assert_ok(r, "macro-run slow");
	pump(r, 50);
	g_assert_false(nothing_running(r, NULL));
	a = cmd(r, "macro-stop slow");
	g_assert_cmpstr(a, ==, "OK stopped 1");
	g_assert_true(nothing_running(r, NULL));
	pump(r, 100);
	g_assert_cmpuint(n_actions_named("late"), ==, 0);

	/* Super+Escape: consumed only while something runs */
	kb = GOWL_KEYBIND_HANDLER(r->macro);
	g_assert_false(gowl_keybind_handler_handle_key(kb, GOWL_KEY_MOD_LOGO,
	                                               XKB_KEY_Escape, TRUE));
	assert_ok(r, "macro-run slow");
	g_assert_false(gowl_keybind_handler_handle_key(
		kb, GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_SHIFT, XKB_KEY_Escape, TRUE));
	g_assert_false(gowl_keybind_handler_handle_key(kb, GOWL_KEY_MOD_LOGO,
	                                               XKB_KEY_Escape, FALSE));
	g_assert_true(gowl_keybind_handler_handle_key(kb, GOWL_KEY_MOD_LOGO,
	                                              XKB_KEY_Escape, TRUE));
	g_assert_true(nothing_running(r, NULL));
	g_assert_cmpuint(toasts->len, >=, 1);

	/* the stop key moves, and `none' switches it off */
	configure(r, "stop-key", "Super+Alt+Escape", NULL);
	assert_ok(r, "macro-run slow");
	g_assert_false(gowl_keybind_handler_handle_key(kb, GOWL_KEY_MOD_LOGO,
	                                               XKB_KEY_Escape, TRUE));
	g_assert_true(gowl_keybind_handler_handle_key(
		kb, GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_ALT, XKB_KEY_Escape, TRUE));
	g_assert_true(nothing_running(r, NULL));
	configure(r, "stop-key", "none", NULL);
	assert_ok(r, "macro-run slow");
	g_assert_false(gowl_keybind_handler_handle_key(
		kb, GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_ALT, XKB_KEY_Escape, TRUE));
	assert_ok(r, "macro-stop all");
	allow_warnings();
	configure(r, "stop-key", "Super+NotAKey", NULL);   /* kept: none */
	restore_warnings();

	/* stopping nothing is not an error */
	b = cmd(r, "macro-stop all");
	g_assert_cmpstr(b, ==, "OK stopped 0");
}

/* ── Faults ─────────────────────────────────────────────────────── */

static void
test_crash_held_back_until_cleared(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	g_autofree gchar *d = NULL;
	g_autofree gchar *journal = NULL;
	g_autofree gchar *status = NULL;
	guint i;
	gboolean custom_seen;

	(void)data;
	RIG_UP(r);
	write_macro(r, "crash", NULL,
	            "\t(void)ctx;\n"
	            "\t{ volatile gint *p = NULL; *p = 1; }\n\treturn TRUE;");
	write_macro(r, "aborts", NULL, "\t(void)ctx;\n\tabort();\n\treturn TRUE;");
	configure(r, "on-fault-custom", "(fault %n %s %t)", NULL);

	allow_warnings();
	a = cmd(r, "macro-run crash");
	restore_warnings();
	g_assert_cmpstr(a, ==, "ERROR crash SIGSEGV and was stopped");
	g_assert_true(held_back(r, "crash"));

	/* told: a toast, and the custom hook with Lisp-quoted words */
	g_assert_cmpuint(toasts->len, >=, 1);
	custom_seen = FALSE;
	for (i = 0; i < actions->len; i++) {
		Action *x = g_ptr_array_index(actions, i);

		if (g_strcmp0(x->arg, "(fault \"crash\" \"SIGSEGV\" \"fault\")") == 0)
			custom_seen = TRUE;
	}
	g_assert_true(custom_seen);

	/* refused while held back, and the journal remembers why */
	b = cmd(r, "macro-run crash");
	g_assert_true(g_str_has_prefix(b, "ERROR crash is held back"));
	g_assert_true(g_file_get_contents(r->journal, &journal, NULL, NULL));
	g_assert_nonnull(strstr(journal, "[quarantine]"));
	g_assert_nonnull(strstr(journal, "crash=crashed (SIGSEGV)"));
	/* not left marked running */
	g_assert_null(strstr(journal, "[running]\ncrash"));

	allow_warnings();
	c = cmd(r, "macro-run aborts");
	restore_warnings();
	g_assert_cmpstr(c, ==, "ERROR aborts SIGABRT and was stopped");

	status = cmd(r, "macro-status");
	g_assert_nonnull(strstr(status, "\"faults\":2"));

	d = cmd(r, "macro-clear crash");
	g_assert_cmpstr(d, ==, "OK cleared crash");
	g_assert_false(held_back(r, "crash"));
	g_assert_true(held_back(r, "aborts"));
	assert_ok(r, "macro-clear all");
	g_assert_false(held_back(r, "aborts"));
	/* the session carries on */
	write_macro(r, "after", NULL,
	            "\tgowl_macro_set_result(ctx, \"alive\");\n\treturn TRUE;");
	g_free(d);
	d = cmd(r, "macro-run after");
	g_assert_cmpstr(d, ==, "OK after: alive");
}

static void
test_loop_and_self_timeout(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	gint64 start;

	(void)data;
	RIG_UP(r);
	write_macro(r, "spin", NULL, "\t(void)ctx;\n\tfor (;;) sink++;\n"
	            "\treturn TRUE;");
	write_macro(r, "long", NULL,
	            "\tgint64 end;\n"
	            "\tgowl_macro_set_timeout(ctx, 3000);\n"
	            "\tend = g_get_monotonic_time() + 300 * 1000;\n"
	            "\twhile (g_get_monotonic_time() < end) sink++;\n"
	            "\tgowl_macro_set_result(ctx, \"made it\");\n\treturn TRUE;");
	write_macro(r, "declared", "#define GOWL_MACRO_TIMEOUT_MS 3000",
	            "\tgint64 end = g_get_monotonic_time() + 300 * 1000;\n"
	            "\twhile (g_get_monotonic_time() < end) sink++;\n"
	            "\t(void)ctx;\n\treturn TRUE;");
	write_macro(r, "unbounded", "#define GOWL_MACRO_TIMEOUT_MS 0",
	            "\tgint64 end = g_get_monotonic_time() + 300 * 1000;\n"
	            "\twhile (g_get_monotonic_time() < end) sink++;\n"
	            "\t(void)ctx;\n\treturn TRUE;");
	configure(r, "timeout-ms", "100", NULL);

	start = g_get_monotonic_time();
	allow_warnings();
	a = cmd(r, "macro-run spin");
	restore_warnings();
	g_assert_cmpstr(a, ==, "ERROR spin ran past its time budget and was "
	                "stopped");
	g_assert_cmpint(g_get_monotonic_time() - start, <, 1000 * 1000);
	g_assert_true(held_back(r, "spin"));

	/* the same budget, but the macro asks for more -- in code ... */
	b = cmd(r, "macro-run long");
	g_assert_cmpstr(b, ==, "OK long: made it");
	/* ... or in its source */
	c = cmd(r, "macro-run declared");
	g_assert_cmpstr(c, ==, "OK ran declared");
	/* zero in the source means no watchdog, not the default */
	g_free(c);
	c = cmd(r, "macro-run unbounded");
	g_assert_cmpstr(c, ==, "OK ran unbounded");
}

/* ── Threaded ───────────────────────────────────────────────────── */

static void
test_threaded(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	g_autofree gchar *d = NULL;

	(void)data;
	RIG_UP(r);
	write_macro(r, "th-ok", "#define GOWL_MACRO_THREADED 1",
	            "\tif (!gowl_macro_sleep(ctx, 30)) return FALSE;\n"
	            "\tgowl_macro_action(ctx, GOWL_ACTION_CUSTOM, \"from-worker\");\n"
	            "\tgowl_macro_set_result(ctx, \"th done\");\n\treturn TRUE;");
	write_macro(r, "th-crash", "#define GOWL_MACRO_THREADED 1",
	            "\t(void)ctx;\n"
	            "\t{ volatile gint *p = NULL; *p = 1; }\n\treturn TRUE;");
	write_macro(r, "th-cancel", "#define GOWL_MACRO_THREADED 1",
	            "\tgowl_macro_set_timeout(ctx, 0);\n"
	            "\twhile (gowl_macro_sleep(ctx, 20)) ;\n"
	            "\tgowl_macro_set_result(ctx, \"saw cancel\");\n\treturn TRUE;");
	/* no raised budget: the watchdog fires while it sleeps */
	write_macro(r, "th-budget", "#define GOWL_MACRO_THREADED 1",
	            "\tgowl_macro_sleep(ctx, 5000);\n\treturn TRUE;");
	write_macro(r, "tl-sleep", NULL,
	            "\treturn gowl_macro_sleep(ctx, 10);");

	a = cmd(r, "macro-run th-ok");
	g_assert_true(g_str_has_prefix(a, "OK started th-ok id="));
	g_assert_true(g_str_has_suffix(a, " threaded"));
	pump_until(r, 3000, action_seen, "from-worker");
	g_assert_cmpuint(n_actions_named("from-worker"), ==, 1);
	pump_until(r, 2000, nothing_running, NULL);
	g_assert_true(nothing_running(r, NULL));

	allow_warnings();
	assert_ok(r, "macro-run th-crash");
	pump_until(r, 3000, held_back, "th-crash");
	restore_warnings();
	g_assert_true(held_back(r, "th-crash"));
	pump_until(r, 1000, nothing_running, NULL);

	assert_ok(r, "macro-run th-cancel");
	pump(r, 100);
	g_assert_false(nothing_running(r, NULL));
	b = cmd(r, "macro-stop th-cancel");
	g_assert_cmpstr(b, ==, "OK stopped 1");
	pump_until(r, 2000, nothing_running, NULL);
	g_assert_true(nothing_running(r, NULL));
	g_assert_false(held_back(r, "th-cancel"));

	/* A watchdog mid-sleep unwinds between slices, not with the
	   context's lock held: stop and status on this thread still work. */
	configure(r, "timeout-ms", "150", NULL);
	allow_warnings();
	assert_ok(r, "macro-run th-budget");
	pump_until(r, 3000, held_back, "th-budget");
	restore_warnings();
	g_assert_true(held_back(r, "th-budget"));
	c = cmd(r, "macro-stop all");
	g_assert_true(g_str_has_prefix(c, "OK stopped"));
	pump_until(r, 1000, nothing_running, NULL);
	g_assert_true(nothing_running(r, NULL));

	/* sleeping is refused outside a worker, not obeyed */
	d = cmd(r, "macro-run tl-sleep");
	g_assert_true(g_str_has_prefix(d, "ERROR tl-sleep failed"));
}

/* ── Limits ─────────────────────────────────────────────────────── */

static void
test_limits(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	g_autofree gchar *d = NULL;

	(void)data;
	RIG_UP(r);
	write_macro(r, "hold", NULL,
	            "\tgowl_macro_wait(ctx, 3000);\n\treturn TRUE;");
	write_macro(r, "other", NULL, "\t(void)ctx;\n\treturn TRUE;");
	write_macro(r, "many", NULL,
	            "\tguint i;\n"
	            "\tfor (i = 0; i < 10; i++)\n"
	            "\t\tgowl_macro_action(ctx, GOWL_ACTION_CUSTOM, \"n\");\n"
	            "\treturn TRUE;");

	/* not reentrant by default */
	assert_ok(r, "macro-run hold");
	a = cmd(r, "macro-run hold");
	g_assert_cmpstr(a, ==, "ERROR hold is already running");
	configure(r, "reentrant", "true", NULL);
	assert_ok(r, "macro-run hold");
	assert_ok(r, "macro-stop all");

	/* max-running */
	configure(r, "reentrant", "false", "max-running", "1", NULL);
	assert_ok(r, "macro-run hold");
	b = cmd(r, "macro-run other");
	g_assert_cmpstr(b, ==, "ERROR 1 macros are already running "
	                "(max-running)");
	assert_ok(r, "macro-stop hold");
	c = cmd(r, "macro-run other");
	g_assert_cmpstr(c, ==, "OK ran other");

	/* max-steps: the rest are dropped, the run is not a fault */
	configure(r, "max-steps", "4", NULL);
	d = cmd(r, "macro-run many");
	g_assert_nonnull(strstr(d, "steps=4"));
	pump_until(r, 1000, nothing_running, NULL);
	g_assert_cmpuint(n_actions_named("n"), ==, 4);
	g_assert_false(held_back(r, "many"));
}

/* ── The journal ────────────────────────────────────────────────── */

static void
test_journal_recovery(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *other = NULL;
	g_autofree gchar *reply = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *after = NULL;

	(void)data;
	RIG_UP(r);
	/* A session that ended while `killer' ran, one that had `old' held */
	other = g_build_filename(r->runtime, "old.journal", NULL);
	g_assert_true(g_file_set_contents(other,
		"[running]\nkiller=1700000000\n\n"
		"[quarantine]\nold=crashed (SIGSEGV)\n", -1, NULL));
	allow_warnings();
	configure(r, "journal", other, NULL);
	restore_warnings();

	write_macro(r, "killer", NULL, "\t(void)ctx;\n\treturn TRUE;");
	reply = cmd(r, "macro-run killer");
	g_assert_true(g_str_has_prefix(reply, "ERROR killer is held back"));
	g_assert_nonnull(strstr(reply, "last session ended"));
	status = cmd(r, "macro-status");
	g_assert_nonnull(strstr(status, "\"old\""));

	/* the running mark is consumed, not re-applied every start */
	g_assert_true(g_file_get_contents(other, &after, NULL, NULL));
	g_assert_null(strstr(after, "[running]"));
}

/* ── Notifications from a macro ─────────────────────────────────── */

static void
test_notify_and_log_file(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *log = NULL;
	g_autofree gchar *text = NULL;

	(void)data;
	RIG_UP(r);
	log = g_build_filename(r->runtime, "macro.log", NULL);
	configure(r, "log", "all", "log-file", log, NULL);
	write_macro(r, "hello", NULL,
	            "\tgowl_macro_log(ctx, \"said %d\", 42);\n"
	            "\tgowl_macro_notify(ctx, \"Hello\", \"from a macro\");\n"
	            "\treturn TRUE;");
	assert_ok(r, "macro-run hello");
	g_assert_cmpuint(toasts->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(toasts, 0), ==, "Hello");

	g_assert_true(g_file_get_contents(log, &text, NULL, NULL));
	g_assert_nonnull(strstr(text, "started hello"));
	g_assert_nonnull(strstr(text, "hello: said 42"));
	g_assert_nonnull(strstr(text, "finished hello"));
}

/* ── Triggers ───────────────────────────────────────────────────── */

static void
test_triggers(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *reply = NULL;
	guint i;
	gboolean found;

	(void)data;
	RIG_UP(r);
	write_macro(r, "tick", NULL,
	            "\tgowl_macro_action(ctx, GOWL_ACTION_CUSTOM, \"tick\");\n"
	            "\treturn gowl_macro_get_trigger(ctx) == GOWL_MACRO_TRIGGER_TIMER;");
	write_macro(r, "evt", NULL,
	            "\tg_autofree gchar *s = g_strdup_printf(\"evt %s %s\",\n"
	            "\t\tgowl_macro_get_trigger_detail(ctx),\n"
	            "\t\tgowl_macro_get_arg(ctx, 0));\n"
	            "\tgowl_macro_action(ctx, GOWL_ACTION_CUSTOM, s);\n"
	            "\treturn TRUE;");
	allow_warnings();
	configure(r, "triggers",
	          "every 100: tick\n"
	          "layout-changed: evt 'an arg'\n"
	          "# a comment\n"
	          "no-such-signal: evt\n"
	          "every 5: tick\n"
	          "garbage", NULL);
	restore_warnings();

	pump(r, 380);
	g_assert_cmpuint(n_actions_named("tick"), >=, 2);
	g_assert_cmpuint(n_actions_named("tick"), <=, 5);

	/* an event runs from an idle, never inside the emission */
	g_signal_emit_by_name(r->compositor, "layout-changed",
	                      gowl_compositor_get_selected_monitor(r->compositor),
	                      "[]=");
	g_assert_false(action_seen(r, "evt layout-changed an arg"));
	pump_until(r, 1000, action_seen, "evt layout-changed an arg");
	found = FALSE;
	for (i = 0; i < actions->len; i++) {
		Action *a = g_ptr_array_index(actions, i);

		if (g_strcmp0(a->arg, "evt layout-changed an arg") == 0)
			found = TRUE;
	}
	g_assert_true(found);

	/* reload keeps the valid ones: two */
	reply = cmd(r, "macro-reload");
	g_assert_cmpstr(reply, ==, "OK reloaded, 2 trigger(s)");
	/* and an empty list removes them */
	configure(r, "triggers", "", NULL);
	pump_until(r, 1000, nothing_running, NULL);   /* a tick in flight */
	g_ptr_array_set_size(actions, 0);
	pump(r, 250);
	g_assert_cmpuint(n_actions_named("tick"), ==, 0);
}

/* ── Filtered triggers ──────────────────────────────────────────── */

static void
emit_layout(
	Rig         *r,
	const gchar *symbol
){
	g_signal_emit_by_name(r->compositor, "layout-changed",
	                      gowl_compositor_get_selected_monitor(r->compositor),
	                      symbol);
}

/* Several triggers on one event, each with its own filter: only the
   ones whose filter passes run.  AND, OR, NOT, parentheses. */
static void
test_filtered_triggers(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *list = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *probe = NULL;
	g_autofree gchar *bad = NULL;

	(void)data;
	RIG_UP(r);
	write_macro(r, "mark", NULL,
	            "\tgowl_macro_action(ctx, GOWL_ACTION_CUSTOM,\n"
	            "\t\tgowl_macro_get_arg(ctx, 0));\n\treturn TRUE;");
	/* Every trigger here runs the SAME macro; its queued step keeps a
	   run alive until it plays, so without this the second trigger on
	   one event finds `mark' already running and is refused. */
	configure(r, "reentrant", "true", "max-running", "16", NULL);
	allow_warnings();
	configure(r, "triggers",
	          /* no filter: every layout change */
	          "layout-changed: mark all\n"
	          /* one condition */
	          "layout-changed [arg=\"[M]\"]: mark monocle\n"
	          /* OR */
	          "layout-changed [arg=\"[M]\" or arg=\"[]=\"]: mark m-or-t\n"
	          /* AND with a field the monitor supplies */
	          "layout-changed [arg=\"[]=\" and monitor=*? and tag=1]:"
	          " mark tile-on-1\n"
	          /* NOT, parentheses, regex */
	          "layout-changed [not (arg~'^\\[M' or arg=\"[]=\")]: mark other\n"
	          /* never: a monitor that is not there */
	          "layout-changed [arg=* and monitor=NO-SUCH-OUTPUT]: mark never\n"
	          /* refused: unknown field, and a broken expression */
	          "layout-changed [colour=red]: mark bad\n"
	          "layout-changed [arg=x and]: mark bad\n"
	          /* a timer that is filtered away, and one that is not */
	          "every 100 [hour>=0 and hour<=23]: mark tick\n"
	          "every 100 [weekday=never]: mark no-tick", NULL);
	restore_warnings();

	/* compiled once up front, so the first event's runs are not
	   waiting behind the compiler */
	g_free(cmd(r, "macro-run mark warm-up"));
	pump(r, 100);
	emit_layout(r, "[M]");
	pump(r, 400);
	g_assert_cmpuint(n_actions_named("all"), ==, 1);
	g_assert_cmpuint(n_actions_named("monocle"), ==, 1);
	g_assert_cmpuint(n_actions_named("m-or-t"), ==, 1);
	g_assert_cmpuint(n_actions_named("tile-on-1"), ==, 0);
	g_assert_cmpuint(n_actions_named("other"), ==, 0);

	emit_layout(r, "[]=");
	pump(r, 400);
	g_assert_cmpuint(n_actions_named("all"), ==, 2);
	g_assert_cmpuint(n_actions_named("monocle"), ==, 1);
	g_assert_cmpuint(n_actions_named("m-or-t"), ==, 2);
	g_assert_cmpuint(n_actions_named("tile-on-1"), ==, 1);
	g_assert_cmpuint(n_actions_named("other"), ==, 0);

	emit_layout(r, "[G]");
	pump(r, 400);
	g_assert_cmpuint(n_actions_named("all"), ==, 3);
	g_assert_cmpuint(n_actions_named("m-or-t"), ==, 2);
	g_assert_cmpuint(n_actions_named("other"), ==, 1);
	g_assert_cmpuint(n_actions_named("never"), ==, 0);
	g_assert_cmpuint(n_actions_named("bad"), ==, 0);

	pump(r, 300);
	g_assert_cmpuint(n_actions_named("tick"), >=, 1);
	g_assert_cmpuint(n_actions_named("no-tick"), ==, 0);

	/* listed with the filter as understood, and counted */
	list = cmd(r, "macro-triggers");
	g_assert_nonnull(strstr(list, "\"errors\":2"));
	g_assert_nonnull(strstr(list, "(arg=\\\"[M]\\\" or arg=\\\"[]=\\\")"));
	g_assert_nonnull(strstr(list, "\"skipped\":"));
	status = cmd(r, "macro-status");
	g_assert_nonnull(strstr(status, "\"trigger-errors\":2"));

	/* try a filter against the live state */
	probe = cmd(r, "macro-filter-test --event=layout-changed "
	            "monitor=* and clients<1");
	g_assert_true(g_str_has_prefix(probe, "OK {\"match\":true"));
	g_assert_nonnull(strstr(probe, "\"event\":\"layout-changed\""));
	g_assert_nonnull(strstr(probe, "\"weekday\":"));
	bad = cmd(r, "macro-filter-test title=x or");
	g_assert_true(g_str_has_prefix(bad, "ERROR filter, at column"));
}

/* ── The Super+space menu's Macros submenu ──────────────────────── */

static const GowlMenuRow *
menu_row(
	GPtrArray   *rows,
	const gchar *label
){
	guint i;

	for (i = 0; i < rows->len; i++) {
		const GowlMenuRow *r = g_ptr_array_index(rows, i);

		if (g_strcmp0(r->label, label) == 0)
			return r;
	}
	return NULL;
}

/*
 * With the module loaded, the shipped Macros submenu lists every macro
 * by name, after its tools and without `Load macros'; a held-back one
 * says so; search at the top finds one; choosing a row runs the macro
 * with the detail `menu'.
 */
static void
test_menu_lists_and_runs_macros(
	Rig           *r,
	gconstpointer  data
){
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) again = NULL;
	g_autoptr(GPtrArray) found = NULL;
	g_autofree gchar *next = NULL;
	g_autofree gchar *held = NULL;
	const GowlMenuRow *row;
	const GowlMenuRow *tools;
	GowlMenuResult result;

	(void)data;
	RIG_UP(r);
	write_macro(r, "from-menu", NULL,
	            "\tgowl_macro_action(ctx, GOWL_ACTION_CUSTOM,\n"
	            "\t\tgowl_macro_get_trigger_detail(ctx));\n"
	            "\treturn TRUE;");
	write_macro(r, "broken-one", NULL,
	            "\t(void)ctx;\n\t{ volatile gint *p = NULL; *p = 1; }\n"
	            "\treturn TRUE;");
	allow_warnings();
	held = cmd(r, "macro-run broken-one");
	restore_warnings();
	g_assert_true(g_str_has_prefix(held, "ERROR"));
	assert_ok(r, "macro-define elisp-thing custom (ignore)");

	g_assert_true(gowl_menu_load_file(menu, GOWL_TEST_MENU_FILE, FALSE,
	                                  NULL));
	rows = gowl_menu_list(menu, r->compositor, "macros");
	g_assert_null(menu_row(rows, "Load macros"));
	tools = menu_row(rows, "Macro tools");
	g_assert_nonnull(tools);
	g_assert_true(tools->submenu);

	row = menu_row(rows, "from-menu");
	g_assert_nonnull(row);
	g_assert_true(row->runnable);
	g_assert_nonnull(row->detail);
	g_assert_true(g_str_has_suffix(row->detail, "/from-menu.c"));
	g_assert_cmpstr(menu_row(rows, "broken-one")->value, ==, "held back");
	g_assert_cmpstr(menu_row(rows, "elisp-thing")->detail, ==, "Elisp");
	/* the shipped examples are on the search path too */
	g_assert_nonnull(menu_row(rows, "sort-windows"));

	/* choosing it runs it, marked as picked from the menu */
	result = gowl_menu_activate(menu, r->compositor, row->route, &next);
	g_assert_cmpint(result, !=, GOWL_MENU_RESULT_OPEN);
	pump_until(r, 1000, action_seen, "menu");
	g_assert_cmpuint(n_actions_named("menu"), ==, 1);

	/* found from the top, by name.  On a menu of just this provider:
	   a search of the whole shipped tree runs every provider, and the
	   tray one opens a session-bus connection that lives as long as
	   the process -- which the D-Bus test after this one would trip
	   over when it takes its private bus down. */
	{
		g_autoptr(GowlMenu) small = gowl_menu_new();

		g_assert_true(gowl_menu_load_data(small,
			"menu:\n"
			"  - {id: macros, label: Macros, provider: macros}\n",
			FALSE, NULL));
		found = gowl_menu_search(small, r->compositor, "from-menu");
		g_assert_nonnull(menu_row(found, "from-menu"));
	}

	/* the tools act on the module */
	again = gowl_menu_list(menu, r->compositor, "macros.tools");
	g_assert_nonnull(menu_row(again, "Stop running macros"));
	g_assert_nonnull(menu_row(again, "Let held-back macros run again"));
	g_clear_pointer(&next, g_free);
	gowl_menu_activate(menu, r->compositor,
	                   menu_row(again, "Let held-back macros run again")->route,
	                   &next);
	g_assert_false(held_back(r, "broken-one"));
}

/* ── Registered from C, defined over IPC ────────────────────────── */

static gboolean
c_macro(
	GowlMacroContext *ctx,
	gpointer          data
){
	g_autofree gchar *s = NULL;

	s = g_strdup_printf("%s:%s:%u", (const gchar *)data,
	                    gowl_macro_get_name(ctx), gowl_macro_get_argc(ctx));
	gowl_macro_set_result(ctx, s);
	gowl_macro_action(ctx, GOWL_ACTION_CUSTOM, "c-step");
	return TRUE;
}

static gboolean
c_crash(
	GowlMacroContext *ctx,
	gpointer          data
){
	volatile gint *p = NULL;

	(void)ctx;
	(void)data;
	*p = 1;
	return TRUE;
}

static void
test_registered_and_defined(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	g_autofree gchar *d = NULL;
	g_autofree gchar *e = NULL;
	g_autofree gchar *list = NULL;
	g_autofree gchar *f = NULL;
	const gchar *argv[] = { "x", NULL };

	(void)data;
	RIG_UP(r);
	gowl_macro_register_func("c-macro", c_macro, g_strdup("tag"), g_free);
	gowl_macro_register_func("c-crash", c_crash, NULL, NULL);

	a = cmd(r, "macro-run c-macro one two");
	g_assert_true(g_str_has_prefix(a, "OK started c-macro"));
	pump_until(r, 1000, action_seen, "c-step");
	g_assert_cmpuint(n_actions_named("c-step"), ==, 1);

	/* the public entry point any C callback can use */
	b = gowl_macro_run_by_name(r->compositor, "c-macro", argv);
	g_assert_nonnull(b);
	g_assert_true(g_str_has_prefix(b, "OK"));

	/* a registered macro runs under the same guard */
	allow_warnings();
	c = cmd(r, "macro-run c-crash");
	restore_warnings();
	g_assert_cmpstr(c, ==, "ERROR c-crash SIGSEGV and was stopped");
	g_assert_true(held_back(r, "c-crash"));

	/* defined: command, custom (Lisp-quoted), action */
	assert_ok(r, "macro-define greet custom (greet %n %a %t)");
	d = cmd(r, "macro-run greet a \"b c\"");
	g_assert_cmpstr(d, ==, "OK ran greet");
	g_assert_cmpuint(n_actions_named("(greet \"greet\" (\"a\" \"b c\") "
	                                 "\"ipc\")"), ==, 1);
	assert_ok(r, "macro-define viacmd command macro-run c-macro %a");
	assert_ok(r, "macro-run viacmd q");
	assert_ok(r, "macro-define act action custom plain-arg");
	assert_ok(r, "macro-run act");
	g_assert_cmpuint(n_actions_named("plain-arg"), ==, 1);
	e = cmd(r, "macro-define bad action no-such-action");
	g_assert_true(g_str_has_prefix(e, "ERROR unknown action"));

	list = cmd(r, "macro-list");
	g_assert_nonnull(strstr(list, "\"name\":\"c-macro\""));
	g_assert_nonnull(strstr(list, "\"kind\":\"registered\""));
	g_assert_nonnull(strstr(list, "\"name\":\"greet\""));
	g_assert_nonnull(strstr(list, "\"kind\":\"custom\""));

	assert_ok(r, "macro-undefine greet");
	f = cmd(r, "macro-run greet");
	g_assert_true(g_str_has_prefix(f, "ERROR"));

	gowl_macro_unregister_func("c-macro");
	gowl_macro_unregister_func("c-crash");
}

/* ── IPC housekeeping ───────────────────────────────────────────── */

static void
test_info_dirs_compile(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *info = NULL;
	g_autofree gchar *dirs = NULL;
	g_autofree gchar *ok = NULL;
	g_autofree gchar *bad = NULL;
	g_autofree gchar *list = NULL;
	g_autofree gchar *unknown = NULL;
	g_autofree gchar *path = NULL;

	(void)data;
	RIG_UP(r);
	write_macro(r, "described", "#define GOWL_MACRO_THREADED 1\n"
	            "G_MODULE_EXPORT const gchar *gowl_macro_info(void)"
	            " { return \"does a thing\"; }",
	            "\t(void)ctx;\n\treturn TRUE;");
	path = g_build_filename(r->macros, "broken.c", NULL);
	g_assert_true(g_file_set_contents(path, "not C at all", -1, NULL));

	info = cmd(r, "macro-info described");
	g_assert_nonnull(strstr(info, "\"kind\":\"file\""));
	g_assert_nonnull(strstr(info, "\"description\":\"does a thing\""));
	g_assert_nonnull(strstr(info, "\"threaded\":true"));

	dirs = cmd(r, "macro-dirs");
	g_assert_true(g_str_has_prefix(dirs, "OK ["));
	g_assert_nonnull(strstr(dirs, r->macros));

	ok = cmd(r, "macro-compile described");
	g_assert_cmpstr(ok, ==, "OK compiled described");
	bad = cmd(r, "macro-compile broken");
	g_assert_true(g_str_has_prefix(bad, "ERROR"));
	g_assert_null(strchr(bad, '\n'));

	list = cmd(r, "macro-list");
	g_assert_nonnull(strstr(list, "\"name\":\"described\""));

	unknown = cmd(r, "macro-frobnicate");
	g_assert_true(g_str_has_prefix(unknown, "ERROR unknown command"));
	assert_ok(r, "macro-log run");
}

/* ── The input remapper's {macro:} target ───────────────────────── */

static struct wlr_keyboard_impl fake_keyboard_impl;

static void
test_remap_target(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_keyboard *kb;
	struct wlr_keyboard_key_event ev;

	(void)data;
	RIG_UP(r);
	write_macro(r, "pedal", NULL,
	            "\tg_autofree gchar *s = g_strdup_printf(\"pedal %s %s %s\",\n"
	            "\t\tgowl_macro_get_trigger(ctx) == GOWL_MACRO_TRIGGER_REMAP\n"
	            "\t\t? \"remap\" : \"other\",\n"
	            "\t\tgowl_macro_get_trigger_detail(ctx),\n"
	            "\t\tgowl_macro_get_arg(ctx, 0));\n"
	            "\tgowl_macro_action(ctx, GOWL_ACTION_CUSTOM, s);\n"
	            "\treturn TRUE;");
	assert_ok(r, "inputremap-add {name: pedals, match: {name: \"Test Pedal\"},"
	          " map: {KEY_A: {macro: pedal, args: \"'left foot' second\"}}}");

	fake_keyboard_impl.name = "gowl-test-keyboard";
	kb = g_new0(struct wlr_keyboard, 1);
	wlr_keyboard_init(kb, &fake_keyboard_impl, "Test Pedal");
	g_ptr_array_add(r->keyboards, kb);
	wl_signal_emit_mutable(&r->compositor->backend->events.new_input,
	                       &kb->base);

	memset(&ev, 0, sizeof ev);
	ev.time_msec = 1;
	ev.keycode = KEY_A;
	ev.update_state = TRUE;
	ev.state = WL_KEYBOARD_KEY_STATE_PRESSED;
	wlr_keyboard_notify_key(kb, &ev);
	ev.state = WL_KEYBOARD_KEY_STATE_RELEASED;
	wlr_keyboard_notify_key(kb, &ev);

	pump_until(r, 2000, action_seen, "pedal remap pedals KEY_A left foot");
	g_assert_cmpuint(n_actions_named("pedal remap pedals KEY_A left foot"),
	                 ==, 1);   /* the press only */
}

/* ── D-Bus ──────────────────────────────────────────────────────── */

static void
on_call_done(
	GObject      *source,
	GAsyncResult *res,
	gpointer      data
){
	GVariant **out = data;
	GError *error = NULL;

	*out = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res,
	                                     &error);
	if (*out == NULL) {
		g_test_message("D-Bus call failed: %s", error->message);
		g_error_free(error);
		*out = g_variant_new("(s)", "D-Bus call failed");
		g_variant_ref_sink(*out);
	}
}

static gboolean
have_reply(
	Rig      *r,
	gpointer  data
){
	(void)r;
	return *(GVariant **)data != NULL;
}

static gboolean
name_owned(GDBusConnection *conn)
{
	GVariant *v;
	gboolean has = FALSE;

	v = g_dbus_connection_call_sync(conn, "org.freedesktop.DBus",
		"/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
		g_variant_new("(s)", "org.gowl.Macro1"), G_VARIANT_TYPE("(b)"),
		G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
	if (v != NULL) {
		g_variant_get(v, "(b)", &has);
		g_variant_unref(v);
	}
	return has;
}

static gchar *
dbus_call(
	Rig             *r,
	GDBusConnection *conn,
	const gchar     *method,
	GVariant        *params
){
	GVariant *reply = NULL;
	gchar *text;

	g_dbus_connection_call(conn, "org.gowl.Macro1", "/org/gowl/Macro1",
	                       "org.gowl.Macro1", method, params,
	                       G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE,
	                       5000, NULL, on_call_done, &reply);
	pump_until(r, 6000, have_reply, &reply);
	g_assert_nonnull(reply);
	g_variant_get(reply, "(s)", &text);
	g_variant_unref(reply);
	return text;
}

static void
test_dbus(
	Rig           *r,
	gconstpointer  data
){
	g_autoptr(GDBusConnection) conn = NULL;
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	g_autofree gchar *status = NULL;
	const gchar *args[] = { "via", "bus", NULL };
	GError *error = NULL;
	gint tries;

	(void)data;
	RIG_UP(r);
	write_macro(r, "bus", NULL,
	            "\tg_autofree gchar *s = g_strjoinv(\" \",\n"
	            "\t\t(gchar **)gowl_macro_get_argv(ctx));\n"
	            "\tgowl_macro_set_result(ctx, s);\n"
	            "\treturn gowl_macro_get_trigger(ctx) == GOWL_MACRO_TRIGGER_DBUS;");
	configure(r, "dbus", "true", NULL);

	conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	g_assert_no_error(error);
	for (tries = 0; tries < 200 && !name_owned(conn); tries++)
		pump(r, 10);
	g_assert_true(name_owned(conn));
	status = cmd(r, "macro-status");
	g_assert_nonnull(strstr(status, "\"dbus\":true"));

	a = dbus_call(r, conn, "Run", g_variant_new("(s^as)", "bus", args));
	g_assert_cmpstr(a, ==, "OK bus: via bus");
	b = dbus_call(r, conn, "List", NULL);
	g_assert_nonnull(strstr(b, "\"name\":\"bus\""));
	c = dbus_call(r, conn, "Stop", g_variant_new("(s)", "all"));
	g_assert_cmpstr(c, ==, "OK stopped 0");

	/* switched off: the name goes */
	configure(r, "dbus", "false", NULL);
	for (tries = 0; tries < 200 && name_owned(conn); tries++)
		pump(r, 10);
	g_assert_false(name_owned(conn));
}

int
main(
	int    argc,
	char **argv
){
	g_autofree gchar *home = NULL;
	g_autofree gchar *sub = NULL;
	gint rc;

	g_test_init(&argc, &argv, NULL);

	/* GLib caches the XDG directories on first use, so they are pointed
	   somewhere disposable before anything asks: the loader's cache and
	   the module's default journal must never touch the real ones. */
	home = g_dir_make_tmp("gowl-macro-home-XXXXXX", NULL);
	g_assert_nonnull(home);
	sub = g_build_filename(home, "cache", NULL);
	g_setenv("XDG_CACHE_HOME", sub, TRUE);
	g_free(sub);
	sub = g_build_filename(home, "state", NULL);
	g_setenv("XDG_STATE_HOME", sub, TRUE);
	g_free(sub);
	sub = g_build_filename(home, "config", NULL);
	g_setenv("XDG_CONFIG_HOME", sub, TRUE);
	g_free(sub);
	sub = g_build_filename(home, "data", NULL);
	g_setenv("XDG_DATA_HOME", sub, TRUE);
	g_unsetenv("GOWL_MACRO_DIR");

#define ADD(path, flags, fn) \
	g_test_add("/macro/runner/" path, Rig, GINT_TO_POINTER(flags), \
	           rig_setup, fn, rig_teardown)
	ADD("result-and-failure", RIG_PLAIN, test_result_and_failure);
	ADD("timeline-order-and-timing", RIG_PLAIN, test_timeline_order_and_timing);
	ADD("stop-and-super-escape", RIG_PLAIN, test_stop_and_super_escape);
	ADD("crash-held-back-until-cleared", RIG_PLAIN,
	    test_crash_held_back_until_cleared);
	ADD("loop-and-self-timeout", RIG_PLAIN, test_loop_and_self_timeout);
	ADD("threaded", RIG_PLAIN, test_threaded);
	ADD("limits", RIG_PLAIN, test_limits);
	ADD("journal-recovery", RIG_PLAIN, test_journal_recovery);
	ADD("notify-and-log-file", RIG_PLAIN, test_notify_and_log_file);
	ADD("triggers", RIG_PLAIN, test_triggers);
	ADD("filtered-triggers", RIG_PLAIN, test_filtered_triggers);
	ADD("menu-lists-and-runs-macros", RIG_PLAIN,
	    test_menu_lists_and_runs_macros);
	ADD("registered-and-defined", RIG_PLAIN, test_registered_and_defined);
	ADD("info-dirs-compile", RIG_PLAIN, test_info_dirs_compile);
	ADD("remap-target", RIG_REMAP, test_remap_target);
	ADD("dbus", RIG_DBUS, test_dbus);
#undef ADD

	rc = g_test_run();
	rm_rf(home);
	return rc;
}
