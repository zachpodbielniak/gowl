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
 * test-macro-examples.c - Every shipped example macro, compiled and run.
 *
 * data/macros/ is documentation people copy from, so a broken example
 * is a broken manual.  Every file is compiled strictly (gnu89, -Wall
 * -Wextra -Werror, as a user's own build would be), compiled and opened
 * by the real module, and RUN in a headless session with three real
 * windows: sort-windows must really reorder them, type-into must really
 * reach the named window, crash-demo must really be contained.  Nothing
 * here spawns a program or touches anything outside the session and the
 * test's temporary directory.
 *
 * A new example that this file does not run fails /macro/examples/run
 * -- add a line for it.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <linux/input-event-codes.h>
#include <string.h>

#include "macro-test-client.h"

/* Titles out of order on purpose; two share an app-id. */
static const gchar * const titles[] = { "charlie", "alpha", "bravo", NULL };
static const gchar * const app_ids[] = { "app.two", "app.one", "app.two",
                                         NULL };
#define W_CHARLIE (0)
#define W_ALPHA   (1)
#define W_BRAVO   (2)

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

static GPtrArray *
example_names(void)
{
	g_autoptr(GDir) dir = NULL;
	GPtrArray *names;
	const gchar *e;

	names = g_ptr_array_new_with_free_func(g_free);
	dir = g_dir_open(GOWL_TEST_MACRO_EXAMPLES, 0, NULL);
	g_assert_nonnull(dir);
	while ((e = g_dir_read_name(dir)) != NULL)
		if (g_str_has_suffix(e, ".c"))
			g_ptr_array_add(names, g_strndup(e, strlen(e) - 2));
	g_ptr_array_sort_values(names, (GCompareFunc)g_strcmp0);
	return names;
}

/* ── Strict compile ─────────────────────────────────────────────── */

static void
strict_compile(void)
{
	g_autoptr(GPtrArray) names = example_names();
	g_autofree gchar *deps = NULL;
	gint status;
	guint i;

	g_assert_cmpuint(names->len, >=, 20);
	if (!g_spawn_command_line_sync(
		    "pkg-config --cflags cairo pangocairo wayland-server "
		    "json-glib-1.0 glib-2.0 gobject-2.0 gmodule-2.0 gio-2.0",
		    &deps, NULL, &status, NULL)
	    || !g_spawn_check_wait_status(status, NULL)) {
		g_test_skip("pkg-config cannot describe the macro dependencies");
		return;
	}
	g_strstrip(deps);

	for (i = 0; i < names->len; i++) {
		g_autofree gchar *line = NULL;
		g_autofree gchar *err = NULL;

		line = g_strdup_printf(
			"gcc -std=gnu89 -fsyntax-only -Wall -Wextra -Werror "
			"-I%s -I%s/gowl -DWLR_USE_UNSTABLE %s %s/%s.c",
			GOWL_MACRO_DEV_INCLUDE, GOWL_MACRO_DEV_INCLUDE, deps,
			GOWL_TEST_MACRO_EXAMPLES,
			(const gchar *)g_ptr_array_index(names, i));
		g_assert_true(g_spawn_command_line_sync(line, NULL, &err, &status,
		                                        NULL));
		if (!g_spawn_check_wait_status(status, NULL))
			g_test_message("%s:\n%s",
			               (const gchar *)g_ptr_array_index(names, i), err);
		g_assert_true(g_spawn_check_wait_status(status, NULL));
	}
}

/* ── Running them ───────────────────────────────────────────────── */

typedef struct {
	Rig         rig;
	GowlModule *macro;
	gchar      *tmp;
	GHashTable *ran;       /* example names exercised */
} Session;

static gboolean
session_up(Session *s)
{
	/* the layouts the layout examples switch between */
	static const gchar * const modules[] = {
		GOWL_TEST_MACRO_MODULE,
		GOWL_TEST_MODULE_DIR "/tile.so",
		GOWL_TEST_MODULE_DIR "/monocle.so",
		NULL
	};
	g_autoptr(GHashTable) settings = NULL;
	g_autofree gchar *journal = NULL;

	memset(s, 0, sizeof *s);
	s->ran = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	if (!rig_up(&s->rig, titles, app_ids, modules))
		return FALSE;
	s->macro = gowl_module_manager_find_module(s->rig.modules, "macro");
	g_assert_nonnull(s->macro);
	s->tmp = g_dir_make_tmp("gowl-macro-examples-XXXXXX", NULL);
	journal = g_build_filename(s->tmp, "journal", NULL);
	settings = g_hash_table_new(g_str_hash, g_str_equal);
	g_hash_table_insert(settings, "macro-dir",
	                    (gpointer)GOWL_TEST_MACRO_EXAMPLES);
	g_hash_table_insert(settings, "journal", journal);
	g_hash_table_insert(settings, "timeout-ms", "2000");
	gowl_module_configure(s->macro, settings);
	return TRUE;
}

static void
rm_rf(const gchar *path)
{
	g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
	const gchar *e;

	if (dir != NULL) {
		while ((e = g_dir_read_name(dir)) != NULL) {
			g_autofree gchar *p = g_build_filename(path, e, NULL);

			if (g_file_test(p, G_FILE_TEST_IS_DIR))
				rm_rf(p);
			else
				g_unlink(p);
		}
	}
	g_rmdir(path);
}

static void
session_down(Session *s)
{
	rig_down(&s->rig);
	if (s->tmp != NULL)
		rm_rf(s->tmp);
	g_free(s->tmp);
	g_hash_table_unref(s->ran);
}

static void
set(
	Session     *s,
	const gchar *key,
	const gchar *value
){
	g_autoptr(GHashTable) settings = g_hash_table_new(g_str_hash,
	                                                  g_str_equal);

	g_hash_table_insert(settings, (gpointer)key, (gpointer)value);
	gowl_module_configure(s->macro, settings);
}

static gchar *
cmd(
	Session     *s,
	const gchar *line
){
	gchar *reply = gowl_compositor_run_command(s->rig.compositor, line);

	g_assert_nonnull(reply);
	g_strchomp(reply);
	return reply;
}

/*
 * Runs `macro-run LINE', records the example as exercised, lets its
 * timeline play, and checks the one thing every example owes: it was
 * not stopped by the guard.
 */
static gchar *
run(
	Session     *s,
	const gchar *line
){
	g_autofree gchar *full = NULL;
	g_auto(GStrv) words = NULL;
	gchar *reply;
	guint i;

	full = g_strdup_printf("macro-run %s", line);
	reply = cmd(s, full);
	g_assert_true(g_shell_parse_argv(line, NULL, &words, NULL));
	for (i = 0; words[i] != NULL && g_str_has_prefix(words[i], "--"); i++)
		;
	g_hash_table_add(s->ran, g_strdup(words[i]));
	if (strstr(reply, "was stopped") != NULL
	    || strstr(reply, "held back") != NULL)
		g_test_message("%s -> %s", line, reply);
	g_assert_null(strstr(reply, "was stopped"));
	g_assert_null(strstr(reply, "held back"));
	pump(&s->rig, 300);
	return reply;
}

static void
run_ok(
	Session     *s,
	const gchar *line
){
	g_autofree gchar *reply = run(s, line);

	if (!g_str_has_prefix(reply, "OK"))
		g_test_message("%s -> %s", line, reply);
	g_assert_true(g_str_has_prefix(reply, "OK"));
}

/* Our three windows' titles, in the compositor's (tiling) order. */
static gchar *
order_now(Session *s)
{
	GString *out = g_string_new(NULL);
	GList *l;

	for (l = gowl_compositor_get_clients(s->rig.compositor); l; l = l->next) {
		const gchar *t = gowl_client_get_title(l->data);

		if (g_strv_contains(titles, t)) {
			if (out->len > 0)
				g_string_append_c(out, ',');
			g_string_append(out, t);
		}
	}
	return g_string_free(out, FALSE);
}

/* Keys received on window @w since the last reset, presses only. */
static GString *
presses_on(
	Session *s,
	gint     w
){
	GString *out = g_string_new(NULL);
	gint i;

	g_mutex_lock(&s->rig.client.lock);
	for (i = 0; i < s->rig.client.n_keys; i++) {
		KeySeen *k = &s->rig.client.keys[i];

		if (k->window == w && k->state == WL_KEYBOARD_KEY_STATE_PRESSED)
			g_string_append_printf(out, "%s%u", out->len ? "," : "",
			                       k->key);
	}
	g_mutex_unlock(&s->rig.client.lock);
	return out;
}

static gboolean
nothing_running(Session *s)
{
	g_autofree gchar *status = cmd(s, "macro-status");

	return strstr(status, "\"running\":[]") != NULL;
}

static void
run_all(void)
{
	Session s;
	g_autoptr(GPtrArray) names = example_names();
	g_autofree gchar *reply = NULL;
	g_autofree gchar *order = NULL;
	g_autofree gchar *snap = NULL;
	g_autofree gchar *poll_file = NULL;
	g_autofree gchar *line = NULL;
	GString *keys;
	guint i;
	gint tries;

	if (!session_up(&s)) {
		session_down(&s);
		return;
	}

	/* Everything compiles and opens through the module, and says what
	   it is for */
	for (i = 0; i < names->len; i++) {
		g_autofree gchar *c = NULL;
		g_autofree gchar *info = NULL;

		line = g_strdup_printf("macro-compile %s",
		                       (const gchar *)g_ptr_array_index(names, i));
		c = cmd(&s, line);
		g_clear_pointer(&line, g_free);
		g_assert_true(g_str_has_prefix(c, "OK compiled"));
		line = g_strdup_printf("macro-info %s",
		                       (const gchar *)g_ptr_array_index(names, i));
		info = cmd(&s, line);
		g_clear_pointer(&line, g_free);
		g_assert_true(g_str_has_prefix(info, "OK {"));
		g_assert_null(strstr(info, "\"description\":null"));
		g_assert_null(strstr(info, "\"description\":\"\""));
	}

	gowl_compositor_focus_client(s.rig.compositor, s.rig.win[W_CHARLIE],
	                             FALSE);
	pump(&s.rig, 50);

	/* hello */
	reply = run(&s, "hello world");
	g_assert_cmpstr(reply, ==, "OK hello: hello");
	g_clear_pointer(&reply, g_free);

	/* sort-windows, both ways, and really */
	reply = run(&s, "sort-windows");
	g_assert_cmpstr(reply, ==, "OK sort-windows: sorted 3 windows");
	g_clear_pointer(&reply, g_free);
	order = order_now(&s);
	g_assert_cmpstr(order, ==, "alpha,bravo,charlie");
	g_clear_pointer(&order, g_free);
	run_ok(&s, "sort-windows reverse");
	order = order_now(&s);
	g_assert_cmpstr(order, ==, "charlie,bravo,alpha");
	g_clear_pointer(&order, g_free);

	/* sort-by-app: app.one first, then app.two by title */
	run_ok(&s, "sort-by-app");
	order = order_now(&s);
	g_assert_cmpstr(order, ==, "alpha,bravo,charlie");
	g_clear_pointer(&order, g_free);

	/* type-into: alpha gets "hi", charlie keeps focus */
	reset_keys(&s.rig);
	run_ok(&s, "type-into title:alpha hi");
	keys = presses_on(&s, W_ALPHA);
	g_assert_cmpstr(keys->str, ==, "35,23");          /* KEY_H, KEY_I */
	g_string_free(keys, TRUE);
	keys = presses_on(&s, W_CHARLIE);
	g_assert_cmpuint(keys->len, ==, 0);
	g_string_free(keys, TRUE);
	g_assert_cmpint(focused_now(&s.rig), ==, W_CHARLIE);

	/* keypress-sequence: ctrl+l, "ab", Return into bravo, over time */
	reset_keys(&s.rig);
	run_ok(&s, "keypress-sequence title:bravo ab");
	pump(&s.rig, 300);
	keys = presses_on(&s, W_BRAVO);
	g_assert_cmpstr(keys->str, ==, "38,30,48,28");   /* L A B ENTER */
	g_string_free(keys, TRUE);
	g_assert_cmpint(focused_now(&s.rig), ==, W_CHARLIE);

	/* paste-template: into the focused window */
	reset_keys(&s.rig);
	run_ok(&s, "paste-template lgtm");
	keys = presses_on(&s, W_CHARLIE);
	g_assert_cmpuint(keys->len, >, 0);
	g_string_free(keys, TRUE);
	reply = run(&s, "paste-template no-such");
	g_assert_true(g_str_has_prefix(reply, "ERROR"));
	g_clear_pointer(&reply, g_free);

	/* layouts */
	run_ok(&s, "layout-for-count");
	run_ok(&s, "cycle-layouts tile monocle");
	reply = run(&s, "cycle-layouts");
	g_assert_true(g_str_has_prefix(reply, "ERROR"));   /* usage */
	g_clear_pointer(&reply, g_free);

	/* windows */
	run_ok(&s, "float-center 50");
	run_ok(&s, "float-center");
	run_ok(&s, "focus-or-launch title:alpha false");
	run_ok(&s, "presentation on");
	run_ok(&s, "presentation off");
	g_free(run(&s, "swap-monitors"));          /* one output here */
	g_free(run(&s, "screenshot-annotated"));   /* no screenshot module */
	g_free(run(&s, "night"));                  /* depends on the clock */
	run_ok(&s, "--trigger=event '--detail=client-added app-id=app.one "
	       "title=alpha' tidy-on-map");
	reply = run(&s, "--trigger=remap '--detail=pedals KEY_A' pedal-demo x y");
	g_assert_true(g_str_has_prefix(reply, "OK pedal-demo: "));
	g_assert_nonnull(strstr(reply, "pedals KEY_A"));
	g_clear_pointer(&reply, g_free);

	/* placement: gather, snapshot, scatter, restore */
	reply = run(&s, "gather-app 'app.two'");
	g_assert_true(g_str_has_prefix(reply, "OK gather-app: gathered "));
	g_clear_pointer(&reply, g_free);
	snap = g_build_filename(s.tmp, "work.snapshot", NULL);
	line = g_strdup_printf("workspace-snapshot save %s", snap);
	reply = run(&s, line);
	g_clear_pointer(&line, g_free);
	g_assert_true(g_str_has_prefix(reply, "OK workspace-snapshot: saved "));
	g_assert_true(g_file_test(snap, G_FILE_TEST_EXISTS));
	g_clear_pointer(&reply, g_free);
	run_ok(&s, "scatter");
	line = g_strdup_printf("workspace-snapshot restore %s", snap);
	reply = run(&s, line);
	g_clear_pointer(&line, g_free);
	g_assert_true(g_str_has_prefix(reply,
	                               "OK workspace-snapshot: restored "));
	g_clear_pointer(&reply, g_free);

	/* the demos of the budget and the guard */
	run_ok(&s, "long-demo 50");
	poll_file = g_build_filename(s.tmp, "watched", NULL);
	g_assert_true(g_file_set_contents(poll_file, "a", -1, NULL));
	line = g_strdup_printf("threaded-poll %s", poll_file);
	reply = run(&s, line);
	g_clear_pointer(&line, g_free);
	g_assert_true(g_str_has_suffix(reply, " threaded"));
	g_clear_pointer(&reply, g_free);
	reply = cmd(&s, "macro-stop threaded-poll");
	g_assert_cmpstr(reply, ==, "OK stopped 1");
	g_clear_pointer(&reply, g_free);
	for (tries = 0; tries < 300 && !nothing_running(&s); tries++)
		pump(&s.rig, 10);
	g_assert_true(nothing_running(&s));

	g_hash_table_add(s.ran, g_strdup("crash-demo"));
	g_hash_table_add(s.ran, g_strdup("loop-demo"));
	allow_warnings();
	reply = cmd(&s, "macro-run crash-demo");
	g_assert_cmpstr(reply, ==, "ERROR crash-demo SIGSEGV and was stopped");
	g_clear_pointer(&reply, g_free);
	reply = cmd(&s, "macro-clear crash-demo");
	g_clear_pointer(&reply, g_free);
	reply = cmd(&s, "macro-run crash-demo divide");
	g_assert_cmpstr(reply, ==, "ERROR crash-demo SIGFPE and was stopped");
	g_clear_pointer(&reply, g_free);
	reply = cmd(&s, "macro-clear crash-demo");
	g_clear_pointer(&reply, g_free);
	reply = cmd(&s, "macro-run crash-demo abort");
	g_assert_cmpstr(reply, ==, "ERROR crash-demo SIGABRT and was stopped");
	g_clear_pointer(&reply, g_free);
	set(&s, "timeout-ms", "200");
	reply = cmd(&s, "macro-run loop-demo");
	g_assert_cmpstr(reply, ==, "ERROR loop-demo ran past its time budget "
	                "and was stopped");
	g_clear_pointer(&reply, g_free);
	restore_warnings();
	set(&s, "timeout-ms", "2000");

	/* last, since it asks a window to go (the test client ignores it) */
	reply = run(&s, "kill-by-title '*charl*'");
	g_assert_cmpstr(reply, ==, "OK kill-by-title: asked 1 window(s) to close");
	g_clear_pointer(&reply, g_free);

	/* ... and every example was exercised */
	for (i = 0; i < names->len; i++) {
		const gchar *n = g_ptr_array_index(names, i);

		if (!g_hash_table_contains(s.ran, n))
			g_test_message("example %s is not run by this test", n);
		g_assert_true(g_hash_table_contains(s.ran, n));
	}
	session_down(&s);
}

/* ── Filters against real windows ───────────────────────────────── */

static GPtrArray *recorded;   /* "tag:app-id:title" per run */

static gboolean
record_macro(
	GowlMacroContext *ctx,
	gpointer          data
){
	GowlClient *c;

	c = gowl_compositor_get_focused_client(gowl_macro_get_compositor(ctx));
	g_ptr_array_add(recorded,
	                g_strdup_printf("%s:%s", (const gchar *)data,
	                                c != NULL ? gowl_client_get_title(c) : ""));
	return TRUE;
}

static gboolean
recorded_has(const gchar *entry)
{
	guint i;

	for (i = 0; i < recorded->len; i++)
		if (g_strcmp0(g_ptr_array_index(recorded, i), entry) == 0)
			return TRUE;
	return FALSE;
}

/*
 * focus-changed carries the window: app-id and title come from it.
 * Three triggers on the one event, each keeping a different subset.
 */
static void
filters_on_real_windows(void)
{
	Session s;

	if (!session_up(&s)) {
		session_down(&s);
		return;
	}
	recorded = g_ptr_array_new_with_free_func(g_free);
	gowl_macro_register_func("rec-one", record_macro, "one", NULL);
	gowl_macro_register_func("rec-two", record_macro, "two", NULL);
	gowl_macro_register_func("rec-any", record_macro, "any", NULL);
	set(&s, "triggers",
	    "focus-changed [app-id=app.one and not title=bravo]: rec-one\n"
	    "focus-changed [app-id=app.two and (title=charlie or title=zulu)]:"
	    " rec-two\n"
	    "focus-changed [app-id=app.* and clients>=3 and floating=false]:"
	    " rec-any");

	gowl_compositor_focus_client(s.rig.compositor, s.rig.win[W_ALPHA], FALSE);
	pump(&s.rig, 100);
	gowl_compositor_focus_client(s.rig.compositor, s.rig.win[W_BRAVO], FALSE);
	pump(&s.rig, 100);
	gowl_compositor_focus_client(s.rig.compositor, s.rig.win[W_CHARLIE],
	                             FALSE);
	pump(&s.rig, 100);

	g_assert_true(recorded_has("one:alpha"));
	g_assert_false(recorded_has("one:bravo"));
	g_assert_false(recorded_has("one:charlie"));
	g_assert_true(recorded_has("two:charlie"));
	g_assert_false(recorded_has("two:bravo"));
	g_assert_false(recorded_has("two:alpha"));
	g_assert_true(recorded_has("any:alpha"));
	g_assert_true(recorded_has("any:bravo"));
	g_assert_true(recorded_has("any:charlie"));

	set(&s, "triggers", "");
	gowl_macro_unregister_func("rec-one");
	gowl_macro_unregister_func("rec-two");
	gowl_macro_unregister_func("rec-any");
	g_ptr_array_unref(recorded);
	session_down(&s);
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

	/* XDG directories somewhere disposable before GLib caches them */
	home = g_dir_make_tmp("gowl-macro-ex-home-XXXXXX", NULL);
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

	g_test_add_func("/macro/examples/strict-compile", strict_compile);
	g_test_add_func("/macro/examples/run", run_all);
	g_test_add_func("/macro/examples/filters-on-real-windows",
	                filters_on_real_windows);
	rc = g_test_run();
	rm_rf(home);
	return rc;
}
