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
 * The `devservices' widget: its model (compiled in directly) and the
 * widget itself (through bar.so and the stub host).
 *
 * Asserted, beyond the parsing:
 *
 *   STOP KILLS THE WHOLE GROUP AND NOTHING ELSE.  A service is started
 *   in its own session; stopping it reaches the grandchild a shell
 *   script backgrounded, and a pid whose start time no longer matches
 *   the record -- a pid reused by an unrelated process after a gowl
 *   restart -- is never signalled.
 *
 *   NOTHING WAITS ON THE COMPOSITOR THREAD.  Actions are a state
 *   machine the worker advances; the tests drive it poll by poll.
 *
 *   USER TEXT STAYS DATA.  URLs and paths reach a command line only
 *   shell-quoted; the scan never reads a .env.
 *
 * Real child processes (sleep) are used and always cleaned up; no
 * network, no real podman or curl -- stand-in scripts take their place.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef GOWL_TEST_BAR_MODULE
#define GOWL_TEST_BAR_MODULE "build/release/modules/bar.so"
#endif
#ifndef GOWL_TEST_FIXTURES
#define GOWL_TEST_FIXTURES "tests/fixtures/bar"
#endif

#define BAR_HARNESS_MODULE GOWL_TEST_BAR_MODULE
#include "bar-widget-harness.h"

/* The model has no bar.so dependencies: compile it in, with the
   bar-util helper it uses. */
#include "../modules/bar/bar-util.c"
#include "../modules/bar/bar-devservices-model.c"

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

static gchar *
fixture(const gchar *name)
{
	return g_build_filename(GOWL_TEST_FIXTURES, "devservices", name, NULL);
}

static gchar *
fixture_text(const gchar *name)
{
	g_autofree gchar *path = fixture(name);
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	return text;
}

static gchar *
scratch_dir(void)
{
	gchar *dir = g_build_filename(g_get_tmp_dir(), "gowl-ds-XXXXXX", NULL);

	g_assert_nonnull(g_mkdtemp(dir));
	return dir;
}

static void
rm_rf(const gchar *dir)
{
	g_autofree gchar *q = g_shell_quote(dir);
	g_autofree gchar *cmd = g_strdup_printf("rm -rf %s", q);

	if (system(cmd) != 0)
		g_test_message("could not remove %s", dir);
}

static void
write_file(const gchar *path, const gchar *text)
{
	g_autofree gchar *dir = g_path_get_dirname(path);

	g_mkdir_with_parents(dir, 0700);
	g_assert_true(g_file_set_contents(path, text, -1, NULL));
}

/* Alive and not a zombie. */
static gboolean
pid_alive(gint pid)
{
	return bar_devservices_proc_starttime("/proc", pid) != 0;
}

/* Wait (bounded) for a pid to die, reaping it if it is our child. */
static gboolean
wait_dead(gint pid)
{
	gint i;

	for (i = 0; i < 200; i++) {
		bar_devservices_reap(pid, NULL);
		if (!pid_alive(pid))
			return TRUE;
		g_usleep(10000);
	}
	return FALSE;
}

static gint
read_pid_file(const gchar *path)
{
	gint i;

	for (i = 0; i < 200; i++) {
		g_autofree gchar *text = NULL;

		if (g_file_get_contents(path, &text, NULL, NULL) &&
		    text[0] != '\0')
			return (gint)g_ascii_strtoll(text, NULL, 10);
		g_usleep(10000);
	}
	return 0;
}

static gint
proc_pgrp(gint pid)
{
	g_autofree gchar *path = g_strdup_printf("/proc/%d/stat", pid);
	g_autofree gchar *text = NULL;
	gint pgrp = 0;

	if (g_file_get_contents(path, &text, NULL, NULL))
		bar_devservices_parse_stat(text, NULL, &pgrp, NULL, NULL, NULL);
	return pgrp;
}

static BarDevservicesStack *
parse_ok(const gchar *yaml)
{
	g_autoptr(GError) error = NULL;
	BarDevservicesStack *stack;

	stack = bar_devservices_stack_parse(yaml, "t", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(stack);
	return stack;
}

/* ----------------------------------------------------------------
 * Parsing
 * ---------------------------------------------------------------- */

/* The documented example parses into exactly what it says: both cmd
   forms, every key, paths under the root.  A regression here is a
   stack that starts the wrong command in the wrong directory. */
static void
test_parse_example(void)
{
	g_autofree gchar *text = fixture_text("web-app.yaml");
	g_autoptr(GError) error = NULL;
	BarDevservicesStack *stack;
	BarDevservicesService *db, *api, *web;
	BarDevservicesTask *migrate;
	g_autofree gchar *root = g_build_filename(g_get_home_dir(), "source",
	                                          "projects", "shop", NULL);
	g_autofree gchar *compose = g_build_filename(root, "compose.yaml", NULL);
	g_autofree gchar *backend = g_build_filename(root, "backend", NULL);

	stack = bar_devservices_stack_parse(text, "shop", "/x/shop.yaml", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(stack->name, ==, "Shop");
	g_assert_cmpstr(stack->root, ==, root);
	g_assert_cmpuint(stack->services->len, ==, 3);
	g_assert_cmpuint(stack->tasks->len, ==, 2);

	db = g_ptr_array_index(stack->services, 0);
	g_assert_cmpint(db->kind, ==, BAR_DEVSERVICES_KIND_PODMAN);
	g_assert_cmpstr(db->compose, ==, compose);
	g_assert_cmpstr(db->compose_service, ==, "postgres");
	g_assert_cmpint(db->port, ==, 5432);
	g_assert_cmpstr(db->health, ==, "port");
	g_assert_cmpint(db->wait, ==, 90);
	g_assert_null(db->argv);

	api = g_ptr_array_index(stack->services, 1);
	g_assert_cmpint(api->kind, ==, BAR_DEVSERVICES_KIND_PROCESS);
	g_assert_false(api->shell);
	g_assert_cmpuint(g_strv_length(api->argv), ==, 6);
	g_assert_cmpstr(api->argv[0], ==, "uv");
	g_assert_cmpstr(api->argv[3], ==, "shop.app:app");
	g_assert_cmpstr(api->dir, ==, backend);
	g_assert_cmpint(api->wait, ==, 60);
	g_assert_false(api->open);
	g_assert_true(g_strv_contains((const gchar * const *)api->env,
		"DATABASE_URL=postgres://shop@127.0.0.1:5432/shop"));
	g_assert_cmpstr(api->needs[0], ==, "db");
	g_assert_null(api->needs[1]);

	/* The string form is handed to sh -c WHOLE: split on spaces, the
	   pipe would reach npm as an argument. */
	web = g_ptr_array_index(stack->services, 2);
	g_assert_true(web->shell);
	g_assert_cmpstr(web->argv[0], ==, "sh");
	g_assert_cmpstr(web->argv[1], ==, "-c");
	g_assert_cmpstr(web->argv[2], ==,
	                "npm run dev -- --port 5173 2>&1 | tee -a /dev/null");
	g_assert_null(web->argv[3]);
	g_assert_true(web->open);
	g_assert_cmpstr(web->url, ==, "http://127.0.0.1:5173/");
	g_assert_cmpstr(web->label, ==, "Frontend");

	migrate = bar_devservices_stack_find_task(stack, "migrate");
	g_assert_nonnull(migrate);
	g_assert_cmpstr(migrate->needs[0], ==, "db");
	g_assert_null(bar_devservices_stack_find_task(stack, "nope"));
	g_assert_cmpint(bar_devservices_stack_find_service(stack, "web"), ==, 2);
	bar_devservices_stack_free(stack);
}

/* Every way a stack file can be wrong is refused as a whole, with a
   reason naming the problem.  A stack that silently dropped a bad
   service would start without its database and call it success. */
static void
test_parse_bad(void)
{
	static const struct { const gchar *yaml; const gchar *why; } cases[] = {
		{ "", "mapping" },
		{ "- a\n- b\n", "mapping" },
		{ "name: x\n", "root" },
		{ "root: relative/dir\n", "absolute" },
		{ "root: /p\nservices: {a: 1}\n", "must be a list" },
		{ "root: /p\nservices: [3]\n", "not a mapping" },
		{ "root: /p\nservices:\n  - cmd: [a]\n", "needs an id" },
		{ "root: /p\nservices:\n  - id: ../x\n    cmd: [a]\n", "needs an id" },
		{ "root: /p\nservices:\n  - id: a\n", "no cmd" },
		{ "root: /p\nservices:\n  - id: a\n    cmd: []\n", "1 to 256" },
		{ "root: /p\nservices:\n  - id: a\n    cmd: \"\"\n", "cmd" },
		{ "root: /p\nservices:\n  - id: a\n    cmd: {x: y}\n", "list of words" },
		{ "root: /p\nservices:\n  - id: a\n    cmd: [[x]]\n", "not a string" },
		{ "root: /p\nservices:\n  - id: a\n    cmd: [a]\n    type: docker\n",
		  "process or podman" },
		{ "root: /p\nservices:\n  - id: a\n    type: podman\n", "compose" },
		{ "root: /p\nservices:\n  - id: a\n    cmd: [a]\n    port: 70000\n",
		  "1-65535" },
		{ "root: /p\nservices:\n  - id: a\n    cmd: [a]\n    wait: soon\n",
		  "wait" },
		{ "root: /p\nservices:\n  - id: a\n    cmd: [a]\n    health: port\n",
		  "no port" },
		{ "root: /p\nservices:\n  - id: a\n    cmd: [a]\n    health: ftp://x\n",
		  "health" },
		{ "root: /p\nservices:\n  - id: a\n    cmd: [a]\n"
		  "    url: file:///etc/passwd\n", "http" },
		{ "root: /p\nservices:\n  - id: a\n    cmd: [a]\n    open: maybe\n",
		  "open" },
		{ "root: /p\nservices:\n  - id: a\n    cmd: [a]\n    env: [x]\n",
		  "env" },
		{ "root: /p\nservices:\n  - id: a\n    cmd: [a]\n"
		  "    env: {\"A=B\": c}\n", "variable name" },
		{ "root: /p\nservices:\n  - id: a\n    cmd: [a]\n  - id: a\n"
		  "    cmd: [b]\n", "used twice" },
		{ "root: /p\nservices:\n  - id: a\n    cmd: [a]\n    needs: [zz]\n",
		  "not a service" },
		{ "root: /p\ntasks:\n  - id: t\n    cmd: [a]\n    needs: [zz]\n",
		  "not a service" },
		{ "root: /p\nservices: [\n", "" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autoptr(GError) error = NULL;
		BarDevservicesStack *stack;

		stack = bar_devservices_stack_parse(cases[i].yaml, "t", NULL,
		                                    &error);
		if (stack != NULL)
			g_error("case %u parsed: %s", i, cases[i].yaml);
		g_assert_nonnull(error);
		if (strstr(error->message, cases[i].why) == NULL)
			g_error("case %u: `%s' does not mention `%s'", i,
			        error->message, cases[i].why);
	}

	/* Text that is not UTF-8 never reaches pango as a label. */
	{
		g_autoptr(GError) error = NULL;

		g_assert_null(bar_devservices_stack_parse(
			"root: /p\nname: \xff\xfe\n", "t", NULL, &error));
		g_assert_error(error, BAR_DEVSERVICES_ERROR,
		               BAR_DEVSERVICES_ERROR_PARSE);
	}
	/* A stack key that would escape the state directory. */
	{
		g_autoptr(GError) error = NULL;

		g_assert_null(bar_devservices_stack_parse("root: /p\n", "../x",
		                                          NULL, &error));
		g_assert_nonnull(error);
	}
}

/* One bad file must not hide the good ones, and the error names the
   file.  The signature moves when a file changes, so the worker
   reloads; it does not move otherwise, so it does not reload every
   five seconds. */
static void
test_load_dir(void)
{
	g_autofree gchar *dir = scratch_dir();
	g_autofree gchar *good = g_build_filename(dir, "good.yaml", NULL);
	g_autofree gchar *bad = g_build_filename(dir, "bad.yml", NULL);
	g_autofree gchar *big = g_build_filename(dir, "big.yaml", NULL);
	g_autofree gchar *dup = g_build_filename(dir, "good.yml", NULL);
	g_autofree gchar *other = g_build_filename(dir, "notes.txt", NULL);
	g_autofree gchar *sig1 = NULL;
	g_autofree gchar *sig2 = NULL;
	g_autofree gchar *sig3 = NULL;
	g_autoptr(GPtrArray) errors = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) stacks = NULL;
	gchar *huge;

	write_file(good, "root: /p\nservices:\n  - id: a\n    cmd: [a]\n");
	write_file(bad, "root: /p\nservices: nope\n");
	write_file(dup, "root: /q\n");
	write_file(other, "not a stack");
	huge = g_strnfill(BAR_DEVSERVICES_MAX_FILE + 10, '#');
	write_file(big, huge);
	g_free(huge);

	stacks = bar_devservices_load_dir(dir, errors);
	g_assert_cmpuint(stacks->len, ==, 1);
	g_assert_cmpstr(((BarDevservicesStack *)g_ptr_array_index(stacks,
	                0))->key, ==, "good");
	g_assert_cmpuint(errors->len, ==, 3);

	sig1 = bar_devservices_dir_signature(dir);
	sig2 = bar_devservices_dir_signature(dir);
	g_assert_cmpstr(sig1, ==, sig2);
	write_file(good, "root: /p\nservices:\n  - id: a\n    cmd: [a, b]\n");
	sig3 = bar_devservices_dir_signature(dir);
	g_assert_cmpstr(sig1, !=, sig3);

	/* A missing directory is no stacks, not a crash. */
	{
		g_autoptr(GPtrArray) none = bar_devservices_load_dir(
			"/nonexistent/gowl-ds", NULL);

		g_assert_cmpuint(none->len, ==, 0);
	}
	rm_rf(dir);
}

/* ----------------------------------------------------------------
 * Ordering
 * ---------------------------------------------------------------- */

static gchar *
waves_text(const BarDevservicesStack *stack, GPtrArray *waves)
{
	GString *out = g_string_new(NULL);
	guint w, i;

	for (w = 0; w < waves->len; w++) {
		GArray *wave = g_ptr_array_index(waves, w);

		if (w > 0)
			g_string_append(out, " | ");
		for (i = 0; i < wave->len; i++) {
			BarDevservicesService *svc = g_ptr_array_index(
				stack->services, g_array_index(wave, guint, i));

			g_string_append_printf(out, "%s%s", i ? "," : "", svc->id);
		}
	}
	return g_string_free(out, FALSE);
}

/* Containers first, then processes; `needs' chains in order; a subset
   pulls in what it needs; a cycle is refused by name.  Getting this
   wrong starts the API before its database and calls it stuck. */
static void
test_plan(void)
{
	BarDevservicesStack *stack;
	g_autoptr(GPtrArray) waves = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;

	/* No needs: the spec'd "podman first, then processes". */
	stack = parse_ok("root: /p\nservices:\n"
		"  - {id: web, cmd: [w]}\n"
		"  - {id: db, type: podman, compose: c.yaml}\n"
		"  - {id: api, cmd: [a]}\n"
		"  - {id: cache, type: podman, compose: c.yaml}\n");
	waves = bar_devservices_plan(stack, NULL, &error);
	g_assert_no_error(error);
	text = waves_text(stack, waves);
	g_assert_cmpstr(text, ==, "db,cache | web,api");
	bar_devservices_stack_free(stack);
	g_clear_pointer(&waves, g_ptr_array_unref);
	g_clear_pointer(&text, g_free);

	/* A chain: a process needing a process comes after it. */
	stack = parse_ok("root: /p\nservices:\n"
		"  - {id: web, cmd: [w], needs: [api]}\n"
		"  - {id: api, cmd: [a], needs: [db]}\n"
		"  - {id: db, type: podman, compose: c.yaml}\n"
		"  - {id: docs, cmd: [d]}\n"
		"tasks:\n  - {id: mig, cmd: [m], needs: [api]}\n");
	waves = bar_devservices_plan(stack, NULL, &error);
	text = waves_text(stack, waves);
	g_assert_cmpstr(text, ==, "db | docs | api | web");
	g_clear_pointer(&waves, g_ptr_array_unref);
	g_clear_pointer(&text, g_free);

	/* A subset: what `mig' needs, and what that needs. */
	{
		const gchar *only[] = { "api", NULL };

		waves = bar_devservices_plan(stack, only, &error);
		text = waves_text(stack, waves);
		g_assert_cmpstr(text, ==, "db | api");
		g_clear_pointer(&waves, g_ptr_array_unref);
		g_clear_pointer(&text, g_free);
	}
	bar_devservices_stack_free(stack);

	/* A cycle names the services in it. */
	stack = parse_ok("root: /p\nservices:\n"
		"  - {id: a, cmd: [a], needs: [b]}\n"
		"  - {id: b, cmd: [b], needs: [c]}\n"
		"  - {id: c, cmd: [c], needs: [a]}\n"
		"  - {id: free, cmd: [f]}\n");
	waves = bar_devservices_plan(stack, NULL, &error);
	g_assert_null(waves);
	g_assert_error(error, BAR_DEVSERVICES_ERROR, BAR_DEVSERVICES_ERROR_CYCLE);
	g_assert_nonnull(strstr(error->message, "a, b, c"));
	g_assert_null(strstr(error->message, "free"));
	bar_devservices_stack_free(stack);
}

/* ----------------------------------------------------------------
 * Locating the project
 * ---------------------------------------------------------------- */

/* The longest root wins (a package inside a monorepo), the match is on
   a path component (`app' is not inside `application'), symlinks
   resolve, and `/' is never a project. */
static void
test_root_match(void)
{
	g_autofree gchar *base = scratch_dir();
	g_autofree gchar *mono = g_build_filename(base, "mono", NULL);
	g_autofree gchar *pkg = g_build_filename(base, "mono", "pkg", NULL);
	g_autofree gchar *deep = g_build_filename(pkg, "src", "x", NULL);
	g_autofree gchar *app = g_build_filename(base, "app", NULL);
	g_autofree gchar *appl = g_build_filename(base, "application", NULL);
	g_autofree gchar *link = g_build_filename(base, "link", NULL);
	g_autofree gchar *via = g_build_filename(link, "src", "x", NULL);
	GPtrArray *stacks;
	BarDevservicesStack *s;
	const gchar *roots[] = { NULL, NULL, NULL, "/" };
	guint i;

	g_mkdir_with_parents(deep, 0700);
	g_mkdir_with_parents(app, 0700);
	g_mkdir_with_parents(appl, 0700);
	g_assert_cmpint(symlink(pkg, link), ==, 0);

	roots[0] = mono;
	roots[1] = pkg;
	roots[2] = app;
	stacks = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_devservices_stack_free);
	for (i = 0; i < G_N_ELEMENTS(roots); i++) {
		g_autofree gchar *y = g_strdup_printf("root: %s\n", roots[i]);

		s = parse_ok(y);
		g_ptr_array_add(stacks, s);
	}

	g_assert_cmpint(bar_devservices_match_root(stacks, deep), ==, 1);
	g_assert_cmpint(bar_devservices_match_root(stacks, mono), ==, 0);
	g_assert_cmpint(bar_devservices_match_root(stacks, app), ==, 2);
	g_assert_cmpint(bar_devservices_match_root(stacks, appl), ==, -1);
	g_assert_cmpint(bar_devservices_match_root(stacks, via), ==, 1);
	g_assert_cmpint(bar_devservices_match_root(stacks, "/etc"), ==, -1);
	g_assert_cmpint(bar_devservices_match_root(stacks, NULL), ==, -1);
	g_ptr_array_unref(stacks);
	rm_rf(base);
}

/* A comm with spaces and parentheses in it must not shift the fields:
   reading the ppid from the wrong column walks the wrong tree. */
static void
test_parse_stat(void)
{
	const gchar *line = "4242 (my (odd) prog) S 4000 4242 4000 0 -1 "
		"4194560 100 0 0 0 1 2 0 0 20 0 1 0 987654 1000000 50 "
		"18446744073709551615\n";
	gint ppid = 0, pgrp = 0;
	guint64 start = 0;
	g_autofree gchar *comm = NULL;
	gchar state = 0;

	g_assert_true(bar_devservices_parse_stat(line, &ppid, &pgrp, &start,
	                                         &comm, &state));
	g_assert_cmpint(ppid, ==, 4000);
	g_assert_cmpint(pgrp, ==, 4242);
	g_assert_cmpuint(start, ==, 987654);
	g_assert_cmpstr(comm, ==, "my (odd) prog");
	g_assert_cmpint(state, ==, 'S');
	g_assert_false(bar_devservices_parse_stat("12 (x) S 1 2", NULL, NULL,
	                                          NULL, NULL, NULL));
	g_assert_false(bar_devservices_parse_stat("garbage", NULL, NULL, NULL,
	                                          NULL, NULL));
}

/* Build /proc/PID/{stat,cwd} under @root. */
static void
fake_proc(const gchar *root, gint pid, gint ppid, const gchar *comm,
          guint64 start, const gchar *cwd, gchar state)
{
	g_autofree gchar *pid_s = g_strdup_printf("%d", pid);
	g_autofree gchar *dir = g_build_filename(root, pid_s, NULL);
	g_autofree gchar *stat = g_build_filename(dir, "stat", NULL);
	g_autofree gchar *link = g_build_filename(dir, "cwd", NULL);
	g_autofree gchar *text = NULL;

	text = g_strdup_printf("%d (%s) %c %d %d %d 0 -1 0 0 0 0 0 0 0 0 0 20 0 "
	                       "1 0 %" G_GUINT64_FORMAT " 0 0\n", pid, comm,
	                       state, ppid, pid, ppid, start);
	write_file(stat, text);
	if (cwd != NULL)
		g_assert_cmpint(symlink(cwd, link), ==, 0);
}

/* The focused terminal's cwd is not the project: the shell it shows
   is.  Deepest shell wins (a nested shell); among panes at one depth
   the newest; a GUI app with no shell under it is itself. */
static void
test_focus_cwd(void)
{
	g_autofree gchar *root = scratch_dir();
	g_autofree gchar *cwd = NULL;

	/* A terminal (100) with bash (101) running a nested zsh (102)
	   running vim (103), and an unrelated bash (200). */
	fake_proc(root, 100, 1, "gst", 10, "/home/u", 'S');
	fake_proc(root, 101, 100, "bash", 11, "/src/outer", 'S');
	fake_proc(root, 102, 101, "zsh", 12, "/src/inner", 'S');
	fake_proc(root, 103, 102, "vim", 13, "/src/inner/sub", 'S');
	fake_proc(root, 200, 1, "bash", 99, "/elsewhere", 'S');
	cwd = bar_devservices_focus_cwd(root, 100);
	g_assert_cmpstr(cwd, ==, "/src/inner");
	g_clear_pointer(&cwd, g_free);

	/* tmux-like: two login shells at the same depth, newest wins. */
	fake_proc(root, 300, 1, "tmux: server", 20, "/", 'S');
	fake_proc(root, 301, 300, "-bash", 21, "/src/old-pane", 'S');
	fake_proc(root, 302, 300, "-bash", 25, "/src/new-pane", 'S');
	fake_proc(root, 303, 300, "fish", 30, "/src/dead-pane", 'Z');
	cwd = bar_devservices_focus_cwd(root, 300);
	g_assert_cmpstr(cwd, ==, "/src/new-pane");
	g_clear_pointer(&cwd, g_free);

	/* No shell below: the process's own directory. */
	fake_proc(root, 400, 1, "firefox", 40, "/home/u/Downloads", 'S');
	fake_proc(root, 401, 400, "Web Content", 41, "/", 'S');
	cwd = bar_devservices_focus_cwd(root, 400);
	g_assert_cmpstr(cwd, ==, "/home/u/Downloads");
	g_clear_pointer(&cwd, g_free);

	/* A ppid loop in a racing /proc cannot spin forever. */
	fake_proc(root, 500, 501, "bash", 50, "/loop/a", 'S');
	fake_proc(root, 501, 500, "bash", 51, "/loop/b", 'S');
	cwd = bar_devservices_focus_cwd(root, 500);
	g_assert_nonnull(cwd);
	g_clear_pointer(&cwd, g_free);

	g_assert_null(bar_devservices_focus_cwd(root, 999));
	g_assert_null(bar_devservices_focus_cwd(root, 0));
	rm_rf(root);
}

/* ----------------------------------------------------------------
 * Ports
 * ---------------------------------------------------------------- */

static guint64
inode_of(GHashTable *ports, guint port)
{
	guint64 *v = g_hash_table_lookup(ports, GUINT_TO_POINTER(port));

	return (v != NULL) ? *v : 0;
}

/* Only LISTEN sockets, hex ports from either family, garbage skipped.
   Counting an ESTABLISHED connection to :8000 as "listening" would
   call a dead server healthy. */
static void
test_net_tcp(void)
{
	g_autofree gchar *tcp = fixture_text("net-tcp");
	g_autofree gchar *tcp6 = fixture_text("net-tcp6");
	g_autoptr(GHashTable) ports = NULL;
	g_autofree gchar *root = scratch_dir();
	g_autofree gchar *net = g_build_filename(root, "net", NULL);
	g_autofree gchar *p4 = g_build_filename(net, "tcp", NULL);
	g_autofree gchar *p6 = g_build_filename(net, "tcp6", NULL);

	ports = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                              g_free);
	g_assert_cmpuint(bar_devservices_parse_net_tcp(tcp, ports), ==, 2);
	g_assert_cmpuint(inode_of(ports, 5432), ==, 41001);
	g_assert_cmpuint(inode_of(ports, 8000), ==, 41002);
	g_assert_false(g_hash_table_contains(ports, GUINT_TO_POINTER(0)));
	g_assert_cmpuint(bar_devservices_parse_net_tcp(tcp6, ports), ==, 2);
	g_assert_cmpuint(inode_of(ports, 5173), ==, 52001);
	g_assert_cmpuint(inode_of(ports, 22), ==, 52002);
	g_assert_cmpuint(bar_devservices_parse_net_tcp("", ports), ==, 0);
	g_assert_cmpuint(bar_devservices_parse_net_tcp(NULL, ports), ==, 0);

	/* The same through a fake proc root. */
	write_file(p4, tcp);
	write_file(p6, tcp6);
	{
		g_autoptr(GHashTable) all = bar_devservices_listening(root);

		g_assert_cmpuint(g_hash_table_size(all), ==, 4);
	}
	rm_rf(root);
}

/* The process holding a socket inode is found through its fds, and
   only among our own processes. */
static void
test_socket_owner(void)
{
	g_autofree gchar *root = scratch_dir();
	g_autofree gchar *fd = g_build_filename(root, "77", "fd", "4", NULL);
	g_autofree gchar *fd_other = g_build_filename(root, "78", "fd", "3",
	                                              NULL);
	g_autofree gchar *dir = g_path_get_dirname(fd);
	g_autofree gchar *dir2 = g_path_get_dirname(fd_other);

	g_mkdir_with_parents(dir, 0700);
	g_mkdir_with_parents(dir2, 0700);
	g_assert_cmpint(symlink("socket:[41001]", fd), ==, 0);
	g_assert_cmpint(symlink("pipe:[41001]", fd_other), ==, 0);
	g_assert_cmpint(bar_devservices_socket_owner(root, 41001, getuid()),
	                ==, 77);
	g_assert_cmpint(bar_devservices_socket_owner(root, 9, getuid()), ==, 0);
	/* Someone else's uid: never ours to stop. */
	g_assert_cmpint(bar_devservices_socket_owner(root, 41001, getuid() + 1),
	                ==, 0);
	rm_rf(root);
}

/* A real listener answers; a closed port does not, and quickly. */
static void
test_port_connects(void)
{
	gint fd;
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	gint port;
	gint64 t0;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	g_assert_cmpint(bind(fd, (struct sockaddr *)&addr, sizeof(addr)), ==, 0);
	g_assert_cmpint(listen(fd, 1), ==, 0);
	g_assert_cmpint(getsockname(fd, (struct sockaddr *)&addr, &len), ==, 0);
	port = ntohs(addr.sin_port);
	g_assert_true(bar_devservices_port_connects(port, 500));
	close(fd);
	t0 = g_get_monotonic_time();
	g_assert_false(bar_devservices_port_connects(port, 500));
	g_assert_cmpint(g_get_monotonic_time() - t0, <, 1100 * 1000);
	g_assert_false(bar_devservices_port_connects(0, 10));
}

/* ----------------------------------------------------------------
 * State file
 * ---------------------------------------------------------------- */

/* The record survives a round trip, dotted ids included; anything
   half-written or hostile is dropped rather than becoming a pid the
   widget might signal. */
static void
test_state_round_trip(void)
{
	g_autoptr(GPtrArray) runs = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_devservices_run_free);
	g_autoptr(GPtrArray) back = NULL;
	g_autoptr(GPtrArray) hostile = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *dir = scratch_dir();
	g_autofree gchar *path = g_build_filename(dir, "sub", "p.state", NULL);
	BarDevservicesRun *r;
	GStatBuf st;

	r = bar_devservices_run_new("web.v2");
	r->pid = 1234; r->pgid = 1234; r->starttime = 99887766; r->started = 17;
	g_ptr_array_add(runs, r);
	r = bar_devservices_run_new("task:migrate");
	r->pid = 555; r->pgid = 555; r->starttime = 42; r->started = 18;
	g_ptr_array_add(runs, r);

	text = bar_devservices_state_to_text(runs);
	back = bar_devservices_state_from_text(text);
	g_assert_cmpuint(back->len, ==, 2);
	r = g_ptr_array_index(back, 0);
	g_assert_cmpstr(r->id, ==, "web.v2");
	g_assert_cmpint(r->pid, ==, 1234);
	g_assert_cmpuint(r->starttime, ==, 99887766);
	g_assert_cmpint(r->started, ==, 17);
	g_assert_cmpstr(((BarDevservicesRun *)g_ptr_array_index(back, 1))->id,
	                ==, "task:migrate");

	hostile = bar_devservices_state_from_text(
		"# comment\n"
		"nostart.pid=10\nnostart.pgid=10\n"          /* no start time */
		"init.pid=1\ninit.pgid=1\ninit.starttime=5\n"  /* pid 1 */
		"../x.pid=10\n../x.pgid=10\n../x.starttime=5\n"
		"neg.pid=-4\nneg.pgid=-4\nneg.starttime=5\n"
		"num.pid=12abc\nnum.pgid=12\nnum.starttime=5\n"
		"good.pid=321\ngood.pgid=321\ngood.starttime=7\n"
		"garbage without equals\n=\n.=\n");
	g_assert_cmpuint(hostile->len, ==, 1);
	g_assert_cmpstr(((BarDevservicesRun *)g_ptr_array_index(hostile,
	                0))->id, ==, "good");

	g_assert_true(bar_devservices_state_save(path, runs, NULL));
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	{
		g_autoptr(GPtrArray) loaded = bar_devservices_state_load(path);

		g_assert_cmpuint(loaded->len, ==, 2);
	}
	g_ptr_array_set_size(runs, 0);
	g_assert_true(bar_devservices_state_save(path, runs, NULL));
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	{
		g_autoptr(GPtrArray) none = bar_devservices_state_load(path);

		g_assert_cmpuint(none->len, ==, 0);
	}
	rm_rf(dir);
}

/* ----------------------------------------------------------------
 * Process groups (real children)
 * ---------------------------------------------------------------- */

/* A service is its own process group, and stopping it reaches the
   grandchild a script backgrounded -- the server a `npm run dev'
   wrapper actually starts.  Its output lands in the log. */
static void
test_spawn_group_stop(void)
{
	g_autofree gchar *dir = scratch_dir();
	g_autofree gchar *log = g_build_filename(dir, "logs", "p-svc.log", NULL);
	g_autofree gchar *gfile = g_build_filename(dir, "grand", NULL);
	g_autofree gchar *script = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *line = NULL;
	BarDevservicesRun *run;
	const gchar *argv[] = { "sh", "-c", NULL, NULL };
	const gchar *env[] = { "DS_GREETING=hello-from-env", NULL };
	gint grand, i;
	GStatBuf st;

	script = g_strdup_printf("echo \"$DS_GREETING\"; sleep 30 & "
	                         "echo $! > '%s'; wait", gfile);
	argv[2] = script;
	run = bar_devservices_spawn(argv, dir, env, log, "svc", "/proc", &error);
	g_assert_no_error(error);
	g_assert_nonnull(run);
	g_assert_cmpint(run->pgid, ==, run->pid);
	g_assert_cmpuint(run->starttime, >, 0);

	grand = read_pid_file(gfile);
	g_assert_cmpint(grand, >, 0);
	/* Own group, and not ours. */
	g_assert_cmpint(proc_pgrp(run->pid), ==, run->pid);
	g_assert_cmpint(proc_pgrp(grand), ==, run->pid);
	g_assert_cmpint(proc_pgrp(run->pid), !=, (gint)getpgrp());
	g_assert_cmpint(bar_devservices_run_liveness("/proc", run), ==,
	                BAR_DEVSERVICES_ALIVE);

	for (i = 0; i < 100 && line == NULL; i++) {
		line = bar_devservices_log_tail(log);
		if (line == NULL)
			g_usleep(10000);
	}
	g_assert_cmpstr(line, ==, "hello-from-env");
	g_assert_cmpint(g_stat(log, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);

	g_assert_cmpint(bar_devservices_run_signal("/proc", run, SIGTERM), ==,
	                BAR_DEVSERVICES_ALIVE);
	g_assert_true(wait_dead(run->pid));
	g_assert_true(wait_dead(grand));
	g_assert_cmpint(bar_devservices_run_liveness("/proc", run), ==,
	                BAR_DEVSERVICES_GONE);
	bar_devservices_run_free(run);
	rm_rf(dir);
}

/* A pid whose start time does not match the record is somebody else
   now.  It is reported as such and NOT signalled.  This is the check
   that makes adopting a state file after a restart safe. */
static void
test_spawn_mismatch_refused(void)
{
	g_autofree gchar *dir = scratch_dir();
	g_autofree gchar *log = g_build_filename(dir, "x.log", NULL);
	const gchar *argv[] = { "sleep", "30", NULL };
	BarDevservicesRun *run;
	BarDevservicesRun fake;

	run = bar_devservices_spawn(argv, NULL, NULL, log, "s", "/proc", NULL);
	g_assert_nonnull(run);
	fake = *run;
	fake.starttime = run->starttime + 1;
	g_assert_cmpint(bar_devservices_run_liveness("/proc", &fake), ==,
	                BAR_DEVSERVICES_MISMATCH);
	g_assert_cmpint(bar_devservices_run_signal("/proc", &fake, SIGKILL), ==,
	                BAR_DEVSERVICES_MISMATCH);
	g_usleep(50000);
	g_assert_true(pid_alive(run->pid));

	/* A record pointing at our own group is refused too. */
	fake.pid = (gint)getpid();
	fake.pgid = (gint)getpgrp();
	fake.starttime = bar_devservices_proc_starttime("/proc", (gint)getpid());
	g_assert_cmpint(bar_devservices_run_signal("/proc", &fake, 0), ==,
	                BAR_DEVSERVICES_MISMATCH);

	bar_devservices_run_signal("/proc", run, SIGKILL);
	g_assert_true(wait_dead(run->pid));
	bar_devservices_run_free(run);
	rm_rf(dir);
}

/* A leader that exits leaving its group running (a script that
   backgrounds the server and returns) is still stoppable. */
static void
test_spawn_orphans(void)
{
	g_autofree gchar *dir = scratch_dir();
	g_autofree gchar *log = g_build_filename(dir, "x.log", NULL);
	g_autofree gchar *gfile = g_build_filename(dir, "grand", NULL);
	g_autofree gchar *script = NULL;
	const gchar *argv[] = { "sh", "-c", NULL, NULL };
	BarDevservicesRun *run;
	gint grand;

	script = g_strdup_printf("sleep 30 & echo $! > '%s'; exit 0", gfile);
	argv[2] = script;
	run = bar_devservices_spawn(argv, NULL, NULL, log, "o", "/proc", NULL);
	g_assert_nonnull(run);
	grand = read_pid_file(gfile);
	g_assert_cmpint(grand, >, 0);
	g_assert_true(wait_dead(run->pid));
	g_assert_cmpint(bar_devservices_run_liveness("/proc", run), ==,
	                BAR_DEVSERVICES_ORPHANS);
	g_assert_cmpint(bar_devservices_run_signal("/proc", run, SIGTERM), ==,
	                BAR_DEVSERVICES_ORPHANS);
	g_assert_true(wait_dead(grand));
	g_assert_cmpint(bar_devservices_run_liveness("/proc", run), ==,
	                BAR_DEVSERVICES_GONE);
	bar_devservices_run_free(run);
	rm_rf(dir);
}

/* A command that does not exist fails at spawn with a reason, and a
   working directory that does not exist is caught before forking. */
static void
test_spawn_failures(void)
{
	g_autofree gchar *dir = scratch_dir();
	g_autofree gchar *log = g_build_filename(dir, "x.log", NULL);
	const gchar *argv[] = { "/nonexistent/gowl-ds-cmd", NULL };
	const gchar *ok[] = { "true", NULL };
	g_autoptr(GError) e1 = NULL;
	g_autoptr(GError) e2 = NULL;

	g_assert_null(bar_devservices_spawn(argv, NULL, NULL, log, "x", "/proc",
	                                    &e1));
	g_assert_nonnull(e1);
	g_assert_null(bar_devservices_spawn(ok, "/nonexistent/dir", NULL, log,
	                                    "x", "/proc", &e2));
	g_assert_error(e2, BAR_DEVSERVICES_ERROR, BAR_DEVSERVICES_ERROR_SPAWN);
	rm_rf(dir);
}

/* ----------------------------------------------------------------
 * Logs
 * ---------------------------------------------------------------- */

/* The panel's subtitle: the last real line, colour codes stripped,
   valid UTF-8, bounded.  Rotation keeps the writer's fd valid. */
static void
test_logs(void)
{
	g_autofree gchar *dir = scratch_dir();
	g_autofree gchar *log = g_build_filename(dir, "a.log", NULL);
	g_autofree gchar *old = g_build_filename(dir, "a.log.1", NULL);
	g_autofree gchar *line = NULL;
	g_autofree gchar *long_line = NULL;
	g_autofree gchar *text = NULL;
	GString *big;
	GStatBuf st;

	write_file(log, "first\n\x1b[32mready\x1b[0m on :8000 \xff\n\n  \n");
	line = bar_devservices_log_tail(log);
	g_assert_nonnull(line);
	g_assert_true(g_utf8_validate(line, -1, NULL));
	g_assert_true(g_str_has_prefix(line, "ready on :8000"));
	g_clear_pointer(&line, g_free);

	long_line = g_strnfill(500, 'x');
	text = g_strdup_printf("%s\n", long_line);
	write_file(log, text);
	line = bar_devservices_log_tail(log);
	g_assert_cmpint(g_utf8_strlen(line, -1), ==, 120);
	g_clear_pointer(&line, g_free);

	g_assert_null(bar_devservices_log_tail("/nonexistent/x.log"));

	big = g_string_new(NULL);
	while (big->len < 3000)
		g_string_append(big, "0123456789 log line\n");
	write_file(log, big->str);
	g_assert_false(bar_devservices_log_rotate(log, 1 << 20));
	g_assert_true(bar_devservices_log_rotate(log, 1000));
	g_assert_cmpint(g_stat(log, &st), ==, 0);
	g_assert_cmpint(st.st_size, ==, 0);
	g_assert_cmpint(g_stat(old, &st), ==, 0);
	g_assert_cmpint(st.st_size, ==, (goffset)big->len);
	g_string_free(big, TRUE);
	rm_rf(dir);
}

/* ----------------------------------------------------------------
 * Status
 * ---------------------------------------------------------------- */

/* Starting until healthy, stuck after N bad polls (including a server
   that WAS up and stopped answering), failed sticks until restarted. */
static void
test_status_machine(void)
{
	gint bad = 0;
	BarDevservicesState s = BAR_DEVSERVICES_OFF;

	s = bar_devservices_status_step(&bad, s, TRUE, FALSE, 3);
	g_assert_cmpint(s, ==, BAR_DEVSERVICES_STARTING);
	s = bar_devservices_status_step(&bad, s, TRUE, FALSE, 3);
	g_assert_cmpint(s, ==, BAR_DEVSERVICES_STARTING);
	s = bar_devservices_status_step(&bad, s, TRUE, TRUE, 3);
	g_assert_cmpint(s, ==, BAR_DEVSERVICES_UP);
	g_assert_cmpint(bad, ==, 0);
	s = bar_devservices_status_step(&bad, s, TRUE, FALSE, 3);
	s = bar_devservices_status_step(&bad, s, TRUE, FALSE, 3);
	g_assert_cmpint(s, ==, BAR_DEVSERVICES_STARTING);
	s = bar_devservices_status_step(&bad, s, TRUE, FALSE, 3);
	g_assert_cmpint(s, ==, BAR_DEVSERVICES_STUCK);
	s = bar_devservices_status_step(&bad, s, TRUE, TRUE, 3);
	g_assert_cmpint(s, ==, BAR_DEVSERVICES_UP);
	s = bar_devservices_status_step(&bad, s, FALSE, FALSE, 3);
	g_assert_cmpint(s, ==, BAR_DEVSERVICES_OFF);
	s = bar_devservices_status_step(&bad, BAR_DEVSERVICES_FAILED, FALSE,
	                                FALSE, 3);
	g_assert_cmpint(s, ==, BAR_DEVSERVICES_FAILED);
	s = bar_devservices_status_step(&bad, BAR_DEVSERVICES_STOPPING, TRUE,
	                                TRUE, 3);
	g_assert_cmpint(s, ==, BAR_DEVSERVICES_STOPPING);
	g_assert_cmpstr(bar_devservices_state_name(BAR_DEVSERVICES_STUCK), ==,
	                "stuck");
}

/* Error beats working beats ok beats off. */
static void
test_aggregate(void)
{
	BarDevservicesState a[] = { BAR_DEVSERVICES_UP, BAR_DEVSERVICES_UP,
	                            BAR_DEVSERVICES_OFF };
	BarDevservicesState b[] = { BAR_DEVSERVICES_UP, BAR_DEVSERVICES_STARTING,
	                            BAR_DEVSERVICES_STUCK };
	BarDevservicesState c[] = { BAR_DEVSERVICES_OFF, BAR_DEVSERVICES_OFF };
	guint up = 0;

	g_assert_cmpint(bar_devservices_aggregate(a, 3, FALSE, &up), ==,
	                BAR_DEVSERVICES_AGG_OK);
	g_assert_cmpuint(up, ==, 2);
	g_assert_cmpint(bar_devservices_aggregate(a, 3, TRUE, NULL), ==,
	                BAR_DEVSERVICES_AGG_WORKING);
	g_assert_cmpint(bar_devservices_aggregate(b, 3, TRUE, &up), ==,
	                BAR_DEVSERVICES_AGG_ERROR);
	g_assert_cmpint(bar_devservices_aggregate(c, 2, FALSE, &up), ==,
	                BAR_DEVSERVICES_AGG_OFF);
	g_assert_cmpint(bar_devservices_aggregate(NULL, 0, FALSE, &up), ==,
	                BAR_DEVSERVICES_AGG_OFF);
}

/* ----------------------------------------------------------------
 * Podman
 * ---------------------------------------------------------------- */

/* `podman compose', never docker; the compose file and the service are
   single argv words whatever they contain; down is down. */
static void
test_compose_argv(void)
{
	BarDevservicesStack *stack;
	BarDevservicesService *svc;
	g_auto(GStrv) up = NULL;
	g_auto(GStrv) down = NULL;
	g_auto(GStrv) custom = NULL;

	stack = parse_ok("root: \"/p/my proj; rm -rf ~\"\nservices:\n"
		"  - {id: db, type: podman, compose: \"dev $(id).yaml\","
		" service: postgres, project: shop}\n");
	svc = g_ptr_array_index(stack->services, 0);
	up = bar_devservices_compose_argv(NULL, svc, TRUE);
	g_assert_cmpstr(up[0], ==, "podman");
	g_assert_cmpstr(up[1], ==, "compose");
	g_assert_cmpstr(up[2], ==, "-f");
	g_assert_cmpstr(up[3], ==, "/p/my proj; rm -rf ~/dev $(id).yaml");
	g_assert_cmpstr(up[4], ==, "-p");
	g_assert_cmpstr(up[5], ==, "shop");
	g_assert_cmpstr(up[6], ==, "up");
	g_assert_cmpstr(up[7], ==, "-d");
	g_assert_cmpstr(up[8], ==, "postgres");
	g_assert_null(up[9]);
	down = bar_devservices_compose_argv("podman compose", svc, FALSE);
	g_assert_cmpstr(down[6], ==, "down");
	g_assert_cmpstr(down[7], ==, "postgres");
	custom = bar_devservices_compose_argv("podman-compose --podman-path "
	                                      "'/opt/my podman'", svc, TRUE);
	g_assert_cmpstr(custom[0], ==, "podman-compose");
	g_assert_cmpstr(custom[2], ==, "/opt/my podman");
	g_assert_null(bar_devservices_compose_argv("'unterminated", svc, TRUE));
	bar_devservices_stack_free(stack);
}

/* Running means: our compose service label, OUR project (another
   project's `postgres' must not make ours look up), state running. */
static void
test_podman_running(void)
{
	g_autofree gchar *json = fixture_text("podman-ps.json");
	BarDevservicesStack *stack;

	stack = parse_ok("root: /home/dev/source/projects/shop\nservices:\n"
		"  - {id: db, type: podman, compose: compose.yaml, service: postgres}\n"
		"  - {id: cache, type: podman, compose: compose.yaml, service: redis}\n"
		"  - {id: mq, type: podman, compose: compose.yaml, service: rabbit}\n"
		"  - {id: x, type: podman, compose: compose.yaml, service: postgres,"
		" project: other}\n");
	g_assert_true(bar_devservices_podman_running(json,
		g_ptr_array_index(stack->services, 0)));
	g_assert_false(bar_devservices_podman_running(json,
		g_ptr_array_index(stack->services, 1)));
	g_assert_false(bar_devservices_podman_running(json,
		g_ptr_array_index(stack->services, 2)));
	g_assert_true(bar_devservices_podman_running(json,
		g_ptr_array_index(stack->services, 3)));
	g_assert_false(bar_devservices_podman_running("[]",
		g_ptr_array_index(stack->services, 0)));
	g_assert_false(bar_devservices_podman_running("not json {",
		g_ptr_array_index(stack->services, 0)));
	g_assert_false(bar_devservices_podman_running(NULL,
		g_ptr_array_index(stack->services, 0)));
	bar_devservices_stack_free(stack);

	/* Elsewhere on disk, a project of the same directory NAME is
	   still recognised by the default project name. */
	stack = parse_ok("root: /srv/checkouts/shop\nservices:\n"
		"  - {id: db, type: podman, compose: compose.yaml, service: postgres}\n");
	g_assert_true(bar_devservices_podman_running(json,
		g_ptr_array_index(stack->services, 0)));
	bar_devservices_stack_free(stack);
}

/* Two projects claiming one port: the panel's "Move port" hint. */
static void
test_port_clashes(void)
{
	g_autoptr(GPtrArray) stacks = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_devservices_stack_free);
	g_autoptr(GPtrArray) clashes = NULL;
	BarDevservicesStack *a, *b;

	a = parse_ok("root: /a\nname: A\nservices:\n"
	             "  - {id: web, cmd: [w], port: 3000}\n"
	             "  - {id: db, cmd: [d], port: 5432}\n");
	b = parse_ok("root: /b\nname: B\nservices:\n"
	             "  - {id: web, cmd: [w], port: 3000}\n"
	             "  - {id: api, cmd: [a], port: 8080}\n");
	g_ptr_array_add(stacks, a);
	g_ptr_array_add(stacks, b);
	clashes = bar_devservices_port_clashes(stacks);
	g_assert_cmpuint(clashes->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(clashes, 0), ==,
	                "Port 3000: A/web and B/web");
}

/* ----------------------------------------------------------------
 * Scanning
 * ---------------------------------------------------------------- */

/* Recognised stacks become drafts that PARSE (a draft that does not
   load is worse than none), the compose ports come through, one
   process service only, and the .env next to them is never read. */
static void
test_scan(void)
{
	g_autofree gchar *roots = fixture("scan");
	g_autofree gchar *webapp = g_build_filename(roots, "webapp", NULL);
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GPtrArray) skip = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_devservices_stack_free);
	BarDevservicesCandidate *cand;
	BarDevservicesStack *draft;
	guint i;

	found = bar_devservices_scan(roots, NULL, 10);
	/* webapp, rusty, pyonly; never `plain'. */
	g_assert_cmpuint(found->len, ==, 3);
	for (i = 0; i < found->len; i++) {
		g_autoptr(GError) error = NULL;

		cand = g_ptr_array_index(found, i);
		g_assert_null(strstr(cand->draft, "hunter2"));
		draft = bar_devservices_stack_parse(cand->draft, cand->key, NULL,
		                                    &error);
		g_assert_no_error(error);
		if (strcmp(cand->key, "webapp") == 0) {
			BarDevservicesService *db, *web;

			g_assert_cmpuint(draft->services->len, ==, 4);
			db = g_ptr_array_index(draft->services, 0);
			g_assert_cmpint(db->kind, ==, BAR_DEVSERVICES_KIND_PODMAN);
			g_assert_cmpint(db->port, ==, 5432);
			g_assert_cmpint(((BarDevservicesService *)
				g_ptr_array_index(draft->services, 1))->port, ==, 6379);
			g_assert_cmpint(((BarDevservicesService *)
				g_ptr_array_index(draft->services, 2))->port, ==, 0);
			web = g_ptr_array_index(draft->services, 3);
			g_assert_cmpstr(web->argv[0], ==, "pnpm");
			g_assert_cmpstr(draft->root, ==, webapp);
		} else if (strcmp(cand->key, "rusty") == 0) {
			BarDevservicesService *app;

			/* The Makefile's run target wraps cargo: ONE
			   service, the Makefile's, and cargo as a note. */
			g_assert_cmpuint(draft->services->len, ==, 1);
			app = g_ptr_array_index(draft->services, 0);
			g_assert_cmpstr(app->argv[0], ==, "make");
			g_assert_cmpstr(app->argv[1], ==, "run");
			g_assert_nonnull(strstr(cand->draft, "# Cargo.toml"));
		} else {
			g_assert_cmpstr(cand->key, ==, "pyonly");
			g_assert_cmpuint(draft->services->len, ==, 0);
			g_assert_nonnull(strstr(cand->draft, "pyproject"));
		}
		bar_devservices_stack_free(draft);
	}

	/* An already-declared root is not offered again. */
	{
		g_autofree gchar *y = g_strdup_printf("root: %s\n", webapp);
		g_autoptr(GPtrArray) again = NULL;

		g_ptr_array_add(skip, parse_ok(y));
		again = bar_devservices_scan(roots, skip, 10);
		g_assert_cmpuint(again->len, ==, 2);
		g_clear_pointer(&again, g_ptr_array_unref);
		again = bar_devservices_scan(roots, NULL, 1);
		g_assert_cmpuint(again->len, ==, 1);
	}

	g_assert_null(bar_devservices_scan_dir("/nonexistent/gowl"));
}

/* Declare writes KEY.yaml at 0644 and refuses to overwrite an edit. */
static void
test_write_draft(void)
{
	g_autofree gchar *src = fixture("scan/rusty");
	g_autofree gchar *dir = scratch_dir();
	g_autofree gchar *conf = g_build_filename(dir, "conf", NULL);
	g_autofree gchar *path = NULL;
	g_autoptr(GError) error = NULL;
	BarDevservicesCandidate *cand;
	GStatBuf st;
	mode_t old_mask;

	cand = bar_devservices_scan_dir(src);
	g_assert_nonnull(cand);
	old_mask = umask(077);
	g_assert_true(bar_devservices_write_draft(conf, cand, &path, NULL));
	umask(old_mask);
	g_assert_true(g_str_has_suffix(path, "/rusty.yaml"));
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0644);
	g_assert_false(bar_devservices_write_draft(conf, cand, NULL, &error));
	g_assert_nonnull(strstr(error->message, "already exists"));
	g_free(cand->key);
	cand->key = g_strdup("../escape");
	g_assert_false(bar_devservices_write_draft(conf, cand, NULL, NULL));
	bar_devservices_candidate_free(cand);
	rm_rf(dir);
}

static void
test_safe_key(void)
{
	g_autofree gchar *a = bar_devservices_safe_key("My Project (v2)");
	g_autofree gchar *b = bar_devservices_safe_key("..");
	g_autofree gchar *c = bar_devservices_safe_key("");
	g_autofree gchar *d = bar_devservices_safe_key("a/../../b");

	g_assert_cmpstr(a, ==, "my-project-v2");
	g_assert_cmpstr(b, ==, "project");
	g_assert_cmpstr(c, ==, "project");
	g_assert_true(bar_devservices_valid_id(d));
	g_assert_null(strchr(d, '/'));
}

/* ----------------------------------------------------------------
 * The widget, through bar.so
 * ---------------------------------------------------------------- */

typedef struct {
	BarHarness h;
	gchar     *conf;
	gchar     *state;
	gchar     *root;
} Ds;

/*
 * Bring the widget up in `fixed' mode on one stack, `p', whose YAML is
 * @services (and @extra) with the root filled in.  The state directory
 * is OUTSIDE the harness's scratch dir so a test can restart the
 * widget against it.
 */
static gboolean
ds_up(Ds *d, const gchar *state, const gchar *body)
{
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *file = NULL;

	memset(d, 0, sizeof(*d));
	if (!bar_harness_up(&d->h, "bar_register_devservices_plugins",
	                    "devservices"))
		return FALSE;
	d->conf = bar_harness_path(&d->h, "conf");
	d->root = bar_harness_path(&d->h, "proj");
	d->state = (state != NULL) ? g_strdup(state)
	                           : bar_harness_path(&d->h, "state2");
	g_mkdir_with_parents(d->root, 0700);
	if (body != NULL) {
		yaml = g_strdup_printf("name: P\nroot: %s\n%s", d->root, body);
		file = g_build_filename(d->conf, "p.yaml", NULL);
		write_file(file, yaml);
	} else {
		g_mkdir_with_parents(d->conf, 0700);
	}
	bar_harness_set(&d->h, "config-dir", d->conf);
	bar_harness_set(&d->h, "state-dir", d->state);
	bar_harness_set(&d->h, "mode", "fixed");
	bar_harness_set(&d->h, "project", "p");
	bar_harness_set(&d->h, "stop-grace", "2");
	/* The first step loads the stacks, as the bar's first poll does
	   long before anyone can open the panel and click. */
	bar_harness_poll(&d->h);
	return TRUE;
}

static void
ds_down(Ds *d)
{
	bar_harness_down(&d->h);
	g_free(d->conf);
	g_free(d->state);
	g_free(d->root);
}

/* Poll until a toast mentions @needle (bounded at ~6 s). */
static gboolean
ds_pump_toast(Ds *d, const gchar *needle)
{
	gint i;

	for (i = 0; i < 300; i++) {
		bar_harness_poll(&d->h);
		if (bar_harness_toasted(&d->h, needle))
			return TRUE;
		g_usleep(20000);
	}
	/* Say what DID arrive: a wrong toast is the usual reason. */
	for (i = 0; i < (gint)d->h.host->toasts->len; i++)
		g_printerr("# toast: %s\n",
		           (const gchar *)g_ptr_array_index(d->h.host->toasts, i));
	return FALSE;
}

static gboolean
ds_pump_label(Ds *d, const gchar *want)
{
	gint i;

	for (i = 0; i < 300; i++) {
		g_autofree gchar *label = NULL;

		bar_harness_poll(&d->h);
		label = gowl_bar_plugin_dup_label(d->h.plugin);
		if (g_strcmp0(label, want) == 0)
			return TRUE;
		g_usleep(20000);
	}
	return FALSE;
}

static void
ds_click(Ds *d, const gchar *id, gint index)
{
	gowl_bar_plugin_panel_action(d->h.plugin, id, index, 0.0, 1);
}

static gboolean
ds_panel_says(Ds *d, const gchar *needle)
{
	g_autoptr(GowlBarPanel) panel = bar_harness_panel(&d->h);

	return bar_harness_panel_mentions(panel, needle);
}

/* With nothing declared the widget says where stacks go, and the
   bar shows no count.  A fresh install must not show "0/0" in red. */
static void
test_widget_no_stacks(void)
{
	Ds d;
	g_autofree gchar *tip = NULL;
	g_autofree gchar *label = NULL;

	if (!ds_up(&d, NULL, NULL))
		return;
	bar_harness_poll(&d.h);
	label = gowl_bar_plugin_dup_label(d.h.plugin);
	g_assert_null(label);
	tip = gowl_bar_plugin_dup_tooltip(d.h.plugin);
	g_assert_nonnull(strstr(tip, "no stacks"));
	g_assert_true(ds_panel_says(&d, "No stacks declared"));
	g_assert_cmpint(gowl_bar_plugin_get_color(d.h.plugin), ==,
	                GOWL_BAR_COLOR_SUBTEXT);
	ds_down(&d);
}

/* Up starts every service, the label counts them and turns green, the
   state file records them; Down kills every process of every group --
   the backgrounded grandchild included -- and the label goes back. */
static void
test_widget_up_down(void)
{
	Ds d;
	g_autofree gchar *gfile = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *statefile = NULL;
	g_autofree gchar *state_text = NULL;
	gint grand;

	gfile = g_build_filename(g_get_tmp_dir(), "gowl-ds-grand-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp(gfile));
	{
		gchar *f = g_build_filename(gfile, "pid", NULL);

		g_free(gfile);
		gfile = f;
	}
	body = g_strdup_printf("services:\n"
		"  - {id: one, cmd: [\"sleep\", \"30\"]}\n"
		"  - {id: two, label: Two, cmd: \"sleep 30 & echo $! > '%s'; wait\","
		" needs: [one]}\n", gfile);
	if (!ds_up(&d, NULL, body))
		return;
	bar_harness_poll(&d.h);
	g_assert_true(ds_pump_label(&d, "0/2"));

	bar_harness_poll(&d.h);
	ds_click(&d, "tool", 0);
	g_assert_true(ds_pump_toast(&d, "P is up"));
	g_assert_true(ds_pump_label(&d, "2/2"));
	g_assert_cmpint(gowl_bar_plugin_get_color(d.h.plugin), ==,
	                GOWL_BAR_COLOR_GREEN);
	grand = read_pid_file(gfile);
	g_assert_cmpint(grand, >, 0);
	g_assert_true(pid_alive(grand));

	statefile = g_build_filename(d.state, "p.state", NULL);
	g_assert_true(g_file_get_contents(statefile, &state_text, NULL, NULL));
	g_assert_nonnull(strstr(state_text, "one.pid="));
	g_assert_nonnull(strstr(state_text, "two.starttime="));
	g_assert_true(ds_panel_says(&d, "2 of 2 up"));

	ds_click(&d, "tool", 1);
	g_assert_true(ds_pump_toast(&d, "P is down"));
	g_assert_true(ds_pump_label(&d, "0/2"));
	g_assert_true(wait_dead(grand));
	g_assert_false(g_file_test(statefile, G_FILE_TEST_EXISTS));
	ds_down(&d);
	{
		g_autofree gchar *pdir = g_path_get_dirname(gfile);

		rm_rf(pdir);
	}
}

/* A service that exits at once fails the Up with its exit status, in
   red, and says so in a toast and in the panel. */
static void
test_widget_failure(void)
{
	Ds d;

	if (!ds_up(&d, NULL, "services:\n"
	           "  - {id: bad, label: Broken, cmd: [\"sh\", \"-c\", "
	           "\"echo boom; exit 3\"]}\n"))
		return;
	bar_harness_poll(&d.h);
	ds_click(&d, "tool", 0);
	g_assert_true(ds_pump_toast(&d, "P: up failed"));
	g_assert_true(bar_harness_toasted(&d.h, "exited with status 3"));
	bar_harness_poll(&d.h);
	g_assert_cmpint(gowl_bar_plugin_get_color(d.h.plugin), ==,
	                GOWL_BAR_COLOR_RED);
	g_assert_true(ds_panel_says(&d, "exited with status 3"));
	g_assert_true(ds_panel_says(&d, "failed"));
	ds_down(&d);
}

/* Without podman the widget says which packages to install -- in the
   toast, the panel and the tooltip -- and starts nothing. */
static void
test_widget_missing_podman(void)
{
	Ds d;
	g_autofree gchar *tip = NULL;

	if (!ds_up(&d, NULL, "services:\n"
	           "  - {id: db, type: podman, compose: compose.yaml}\n"))
		return;
	bar_harness_set(&d.h, "podman", "/nonexistent/podman");
	bar_harness_poll(&d.h);
	g_assert_true(ds_panel_says(&d, "podman-compose"));
	tip = gowl_bar_plugin_dup_tooltip(d.h.plugin);
	g_assert_nonnull(strstr(tip, "podman is not installed"));
	bar_harness_poll(&d.h);
	ds_click(&d, "tool", 0);
	g_assert_true(ds_pump_toast(&d, "Fedora: podman, podman-compose"));
	ds_down(&d);
}

/* Podman services go through `podman compose -f FILE up -d SERVICE'
   and `down SERVICE' (a stand-in records the argv), and are running
   when `podman ps' says so for our project. */
static void
test_widget_podman(void)
{
	Ds d;
	g_autofree gchar *rec = NULL;
	g_autofree gchar *fake = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *script = NULL;
	g_autofree gchar *calls = NULL;
	g_autofree gchar *compose_cmd = NULL;
	g_autofree gchar *flag = NULL;

	if (!ds_up(&d, NULL, NULL))
		return;
	rec = bar_harness_path(&d.h, "podman-calls");
	flag = bar_harness_path(&d.h, "db-up");
	/* `ps' answers from the up/down the stand-in was told to do, so
	   the widget has to wait for the compose child to finish. */
	body = g_strdup_printf(
		"echo \"$*\" >> '%s'\n"
		"case \"$*\" in\n"
		"  *' up -d '*) touch '%s' ;;\n"
		"  *' down '*) rm -f '%s' ;;\n"
		"  ps*) if [ -e '%s' ]; then\n"
		"         printf '[{\"State\":\"running\",\"Labels\":"
		"{\"com.docker.compose.service\":\"postgres\","
		"\"com.docker.compose.project.working_dir\":\"%s\"}}]'\n"
		"       else printf '[]'; fi ;;\n"
		"esac\nexit 0\n", rec, flag, flag, flag, d.root);
	fake = bar_harness_script(&d.h, "podman", body);
	compose_cmd = g_strdup_printf("%s compose", fake);
	script = g_strdup_printf("name: P\nroot: %s\nservices:\n"
		"  - {id: db, type: podman, compose: compose.yaml, service: postgres}\n",
		d.root);
	{
		g_autofree gchar *file = g_build_filename(d.conf, "p.yaml", NULL);

		write_file(file, script);
	}
	bar_harness_set(&d.h, "podman", fake);
	bar_harness_set(&d.h, "compose-command", compose_cmd);

	bar_harness_poll(&d.h);
	ds_click(&d, "tool", 0);
	g_assert_true(ds_pump_toast(&d, "P is up"));
	g_assert_true(ds_pump_label(&d, "1/1"));
	g_assert_true(g_file_get_contents(rec, &calls, NULL, NULL));
	g_assert_nonnull(strstr(calls, "compose -f "));
	g_assert_nonnull(strstr(calls, "/compose.yaml up -d postgres"));
	g_assert_nonnull(strstr(calls, "ps --format json"));
	g_assert_null(strstr(calls, "docker"));

	ds_click(&d, "tool", 1);
	g_assert_true(ds_pump_toast(&d, "P is down"));
	g_assert_true(ds_pump_label(&d, "0/1"));
	g_clear_pointer(&calls, g_free);
	g_assert_true(g_file_get_contents(rec, &calls, NULL, NULL));
	g_assert_nonnull(strstr(calls, "/compose.yaml down postgres"));
	ds_down(&d);
}

/* URL health goes through curl (a stand-in): healthy when it succeeds;
   a service that never gets healthy fails the Up after its `wait'. */
static void
test_widget_curl_health(void)
{
	Ds d;
	g_autofree gchar *curl = NULL;
	g_autofree gchar *body = NULL;

	if (!ds_up(&d, NULL, NULL))
		return;
	curl = bar_harness_script(&d.h, "curl",
		"for a in \"$@\"; do url=\"$a\"; done\n"
		"case \"$url\" in *ok*) printf 200; exit 0 ;; esac\nexit 22\n");
	body = g_strdup_printf("name: P\nroot: %s\nservices:\n"
		"  - {id: good, cmd: [sleep, \"30\"], health: \"http://127.0.0.1:9/ok\","
		" wait: 1}\n"
		"  - {id: slow, cmd: [sleep, \"30\"], health: \"http://127.0.0.1:9/no\","
		" wait: 1}\n", d.root);
	{
		g_autofree gchar *file = g_build_filename(d.conf, "p.yaml", NULL);

		write_file(file, body);
	}
	bar_harness_set(&d.h, "curl", curl);
	bar_harness_poll(&d.h);
	ds_click(&d, "tool", 0);
	g_assert_true(ds_pump_toast(&d, "up failed"));
	g_assert_true(bar_harness_toasted(&d.h, "not healthy in time: slow"));
	g_assert_true(ds_panel_says(&d, "not healthy after 1 s"));
	g_assert_true(ds_pump_label(&d, "1/2"));
	/* Clean up the two sleeps. */
	ds_click(&d, "tool", 1);
	g_assert_true(ds_pump_toast(&d, "P is down"));
	ds_down(&d);
}

/* A server that was up and stops answering turns stuck (red) after
   `stuck-after' polls, rather than staying green forever. */
static void
test_widget_stuck(void)
{
	Ds d;
	g_autofree gchar *curl = NULL;
	g_autofree gchar *flag = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *script = NULL;
	gint i;

	if (!ds_up(&d, NULL, NULL))
		return;
	flag = bar_harness_path(&d.h, "healthy");
	write_file(flag, "1");
	script = g_strdup_printf("[ -e '%s' ] && { printf 200; exit 0; }\n"
	                         "exit 22\n", flag);
	curl = bar_harness_script(&d.h, "curl", script);
	body = g_strdup_printf("name: P\nroot: %s\nservices:\n"
		"  - {id: api, cmd: [sleep, \"30\"], health: \"http://127.0.0.1:9/\"}\n",
		d.root);
	{
		g_autofree gchar *file = g_build_filename(d.conf, "p.yaml", NULL);

		write_file(file, body);
	}
	bar_harness_set(&d.h, "curl", curl);
	bar_harness_set(&d.h, "stuck-after", "2");
	bar_harness_set(&d.h, "notify", "all");
	bar_harness_poll(&d.h);
	ds_click(&d, "tool", 0);
	g_assert_true(ds_pump_toast(&d, "P is up"));
	g_assert_true(bar_harness_toasted(&d.h, "P: api is up"));
	g_unlink(flag);
	for (i = 0; i < 4; i++)
		bar_harness_poll(&d.h);
	g_assert_cmpint(gowl_bar_plugin_get_color(d.h.plugin), ==,
	                GOWL_BAR_COLOR_RED);
	g_assert_true(ds_panel_says(&d, "stuck"));
	g_assert_true(bar_harness_toasted(&d.h, "api is stuck"));
	ds_click(&d, "tool", 1);
	g_assert_true(ds_pump_toast(&d, "P is down"));
	ds_down(&d);
}

/* Each colour setting overrides its state's role, as a role name or a
   literal. */
static void
test_widget_colours(void)
{
	Ds d;
	gdouble rgba[4];

	if (!ds_up(&d, NULL, "services:\n  - {id: a, cmd: [sleep, \"30\"]}\n"))
		return;
	bar_harness_set(&d.h, "color-off", "mauve");
	bar_harness_poll(&d.h);
	g_assert_cmpint(gowl_bar_plugin_get_color(d.h.plugin), ==,
	                GOWL_BAR_COLOR_MAUVE);
	bar_harness_set(&d.h, "color-off", "#112233");
	bar_harness_poll(&d.h);
	g_assert_true(gowl_bar_plugin_get_color_rgba(d.h.plugin, rgba));
	g_assert_cmpfloat_with_epsilon(rgba[0], 0x11 / 255.0, 0.01);
	g_assert_cmpfloat_with_epsilon(rgba[2], 0x33 / 255.0, 0.01);
	bar_harness_set(&d.h, "color-ok", "teal");
	bar_harness_poll(&d.h);
	ds_click(&d, "tool", 0);
	g_assert_true(ds_pump_label(&d, "1/1"));
	g_assert_true(ds_pump_toast(&d, "P is up"));
	g_assert_cmpint(gowl_bar_plugin_get_color(d.h.plugin), ==,
	                GOWL_BAR_COLOR_TEAL);
	ds_click(&d, "tool", 1);
	g_assert_true(ds_pump_toast(&d, "P is down"));
	ds_down(&d);
}

/* notify=off raises nothing, even for a failure. */
static void
test_widget_notify_off(void)
{
	Ds d;
	gint i;

	if (!ds_up(&d, NULL, "services:\n  - {id: bad, cmd: [\"false\"]}\n"))
		return;
	bar_harness_set(&d.h, "notify", "off");
	bar_harness_poll(&d.h);
	ds_click(&d, "tool", 0);
	for (i = 0; i < 30; i++) {
		bar_harness_poll(&d.h);
		g_usleep(10000);
	}
	g_assert_cmpuint(d.h.host->toasts->len, ==, 0);
	g_assert_true(ds_panel_says(&d, "failed"));
	ds_down(&d);
}

/* Logs and Open put user-derived text on a command line, so every
   piece of it is shell-quoted: a state dir with a quote in it and a
   URL with `;' in it stay single words. */
static void
test_widget_logs_open_quoting(void)
{
	Ds d;
	g_autofree gchar *state = NULL;
	g_autofree gchar *base = NULL;

	base = g_build_filename(g_get_tmp_dir(), "gowl-ds-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp(base));
	state = g_build_filename(base, "it's state", NULL);
	if (!ds_up(&d, state, "services:\n"
	           "  - {id: web, cmd: [sleep, \"30\"],"
	           " url: \"http://127.0.0.1:1/a;touch /tmp/pwned\"}\n"))
		return;
	bar_harness_set(&d.h, "terminal", "myterm");
	bar_harness_poll(&d.h);

	ds_click(&d, "tool", 3);
	g_assert_true(bar_harness_spawned(&d.h, "myterm -e tail -F "));
	g_assert_true(bar_harness_spawned(&d.h, "'\\''s state/p-web.log'"));

	bar_harness_set(&d.h, "open-command", "my-open");
	ds_click(&d, "tool", 4);
	g_assert_true(bar_harness_spawned(&d.h,
		"my-open 'http://127.0.0.1:1/a;touch /tmp/pwned'"));

	bar_harness_set(&d.h, "log-command", "kitty --hold tail -n 50");
	ds_click(&d, "svc:web", 0);
	g_assert_true(bar_harness_spawned(&d.h, "kitty --hold tail -n 50 '"));

	bar_harness_set(&d.h, "edit-command", "myedit");
	ds_click(&d, "tool2", 1);
	g_assert_true(bar_harness_spawned(&d.h, "myedit '"));
	g_assert_true(bar_harness_spawned(&d.h, "/conf/p.yaml'"));
	ds_down(&d);
	rm_rf(base);
}

/* A task runs after the services it needs, in its own group, and its
   result is reported; a `needs' cycle refuses to start with a reason. */
static void
test_widget_tasks_and_cycle(void)
{
	Ds d;

	if (!ds_up(&d, NULL, "services:\n"
	           "  - {id: db, cmd: [sleep, \"30\"]}\n"
	           "tasks:\n"
	           "  - {id: mig, label: Migrate, cmd: [sh, -c, \"exit 4\"],"
	           " needs: [db]}\n"
	           "  - {id: ok, label: Lint, cmd: [\"true\"]}\n"))
		return;
	bar_harness_poll(&d.h);
	g_assert_true(ds_panel_says(&d, "needs db"));
	ds_click(&d, "task:mig", 0);
	g_assert_true(ds_pump_toast(&d, "P: Migrate failed (status 4)"));
	/* The service it needed was started first. */
	g_assert_true(ds_pump_label(&d, "1/1"));
	ds_click(&d, "task:ok", 0);
	g_assert_true(ds_pump_toast(&d, "P: Lint finished"));
	ds_click(&d, "tool", 1);
	g_assert_true(ds_pump_toast(&d, "P is down"));
	ds_down(&d);

	if (!ds_up(&d, NULL, "services:\n"
	           "  - {id: a, cmd: [a], needs: [b]}\n"
	           "  - {id: b, cmd: [b], needs: [a]}\n"))
		return;
	bar_harness_poll(&d.h);
	bar_harness_poll(&d.h);
	ds_click(&d, "tool", 0);
	g_assert_true(ds_pump_toast(&d, "P: cannot start"));
	g_assert_true(bar_harness_toasted(&d.h, "circle"));
	ds_down(&d);
}

/* A stranger on our port is shown, and Take over is two clicks: the
   first only arms (nothing dies), the second stops exactly that pid. */
static void
test_widget_takeover(void)
{
	Ds d;
	g_autofree gchar *body = NULL;
	gint sv[2];
	pid_t child;
	gint port = 0;
	gint i, status;
	gboolean died = FALSE;

	/* A child that listens on an ephemeral port and tells us which. */
	g_assert_cmpint(pipe(sv), ==, 0);
	child = fork();
	g_assert_cmpint(child, >=, 0);
	if (child == 0) {
		gint fd = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a;
		socklen_t len = sizeof(a);

		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
		    listen(fd, 1) != 0 ||
		    getsockname(fd, (struct sockaddr *)&a, &len) != 0)
			_exit(1);
		port = ntohs(a.sin_port);
		if (write(sv[1], &port, sizeof(port)) != sizeof(port))
			_exit(1);
		sleep(30);
		_exit(0);
	}
	close(sv[1]);
	g_assert_cmpint(read(sv[0], &port, sizeof(port)), ==, sizeof(port));
	close(sv[0]);

	body = g_strdup_printf("services:\n  - {id: web, cmd: [sleep, \"30\"],"
	                       " port: %d}\n", port);
	if (!ds_up(&d, NULL, body)) {
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return;
	}
	bar_harness_poll(&d.h);
	{
		g_autofree gchar *want = g_strdup_printf("Take over port %d", port);
		g_autofree gchar *who = g_strdup_printf("pid %d", (gint)child);

		g_assert_true(ds_panel_says(&d, want));
		g_assert_true(ds_panel_says(&d, who));
	}
	ds_click(&d, "takeover:web", 0);
	g_assert_true(ds_panel_says(&d, "Click again to stop pid"));
	bar_harness_poll(&d.h);
	g_assert_cmpint(waitpid(child, &status, WNOHANG), ==, 0);

	ds_click(&d, "takeover:web", 0);
	for (i = 0; i < 200 && !died; i++) {
		died = (waitpid(child, &status, WNOHANG) == child);
		if (!died)
			g_usleep(10000);
	}
	g_assert_true(died);
	g_assert_true(WIFSIGNALED(status));
	g_assert_cmpint(WTERMSIG(status), ==, SIGTERM);
	g_assert_true(ds_pump_toast(&d, "Stopped"));
	ds_down(&d);
}

/* Our own listener is not "someone else's" to take over: a port held
   by gowl itself (here, the test process) is refused. */
static void
test_widget_takeover_refuses_self(void)
{
	Ds d;
	g_autofree gchar *body = NULL;
	gint fd;
	struct sockaddr_in a;
	socklen_t len = sizeof(a);

	fd = socket(AF_INET, SOCK_STREAM, 0);
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	g_assert_cmpint(bind(fd, (struct sockaddr *)&a, sizeof(a)), ==, 0);
	g_assert_cmpint(listen(fd, 1), ==, 0);
	g_assert_cmpint(getsockname(fd, (struct sockaddr *)&a, &len), ==, 0);
	body = g_strdup_printf("services:\n  - {id: web, cmd: [sleep, \"30\"],"
	                       " port: %d}\n", ntohs(a.sin_port));
	if (!ds_up(&d, NULL, body)) {
		close(fd);
		return;
	}
	bar_harness_poll(&d.h);
	ds_click(&d, "takeover:web", 0);
	ds_click(&d, "takeover:web", 0);
	g_assert_true(ds_pump_toast(&d, "not held by one of your processes"));
	close(fd);
	ds_down(&d);
}

/* Scan finds undeclared projects under the search roots, Declare
   writes the draft into the config dir, and the next step loads it. */
static void
test_widget_scan_declare(void)
{
	Ds d;
	g_autofree gchar *roots = fixture("scan");
	g_autofree gchar *written = NULL;

	if (!ds_up(&d, NULL, NULL))
		return;
	bar_harness_set(&d.h, "search-roots", roots);
	bar_harness_set(&d.h, "mode", "all");
	bar_harness_poll(&d.h);
	ds_click(&d, "tool2", 0);
	bar_harness_poll(&d.h);
	g_assert_true(ds_panel_says(&d, "declare:webapp"));
	g_assert_true(ds_panel_says(&d, "package.json (dev script)"));
	ds_click(&d, "declare:rusty", 0);
	g_assert_true(ds_pump_toast(&d, "Declared rusty"));
	written = g_build_filename(d.conf, "rusty.yaml", NULL);
	g_assert_true(g_file_test(written, G_FILE_TEST_EXISTS));
	bar_harness_poll(&d.h);
	g_assert_true(ds_panel_says(&d, "project:rusty"));
	g_assert_true(ds_panel_says(&d, "edit:rusty"));
	ds_down(&d);
}

/* auto mode follows the directory it is told the focus is in (cmacs
   pushes focus-cwd; standalone gowl reads the focused window's shell);
   a click pins a project, Unpin follows the focus again. */
static void
test_widget_auto_and_pin(void)
{
	Ds d;
	g_autofree gchar *sub = NULL;
	g_autofree gchar *tip = NULL;
	g_autofree gchar *label = NULL;

	if (!ds_up(&d, NULL, "services:\n  - {id: a, cmd: [sleep, \"30\"]}\n"))
		return;
	sub = g_build_filename(d.root, "src", "deep", NULL);
	g_mkdir_with_parents(sub, 0700);
	bar_harness_set(&d.h, "mode", "auto");
	bar_harness_set(&d.h, "focus-cwd", "/");
	bar_harness_poll(&d.h);
	label = gowl_bar_plugin_dup_label(d.h.plugin);
	g_assert_null(label);
	tip = gowl_bar_plugin_dup_tooltip(d.h.plugin);
	g_assert_nonnull(strstr(tip, "not in a declared project"));
	g_assert_true(ds_panel_says(&d, "project:p"));

	bar_harness_set(&d.h, "focus-cwd", sub);
	g_assert_true(ds_pump_label(&d, "0/1"));

	bar_harness_set(&d.h, "focus-cwd", "/");
	bar_harness_poll(&d.h);
	ds_click(&d, "project:p", 0);
	bar_harness_poll(&d.h);
	g_clear_pointer(&label, g_free);
	label = gowl_bar_plugin_dup_label(d.h.plugin);
	g_assert_cmpstr(label, ==, "0/1");
	g_assert_true(ds_panel_says(&d, "unpin"));
	ds_click(&d, "unpin", 0);
	bar_harness_poll(&d.h);
	g_clear_pointer(&label, g_free);
	label = gowl_bar_plugin_dup_label(d.h.plugin);
	g_assert_null(label);
	ds_down(&d);
}

/* A restarted widget (a gowl restart) adopts the processes the last
   one started, through the state file, and can stop them. */
static void
test_widget_adopt_after_restart(void)
{
	Ds d;
	g_autofree gchar *state = scratch_dir();
	g_autofree gchar *statefile = g_build_filename(state, "p.state", NULL);
	g_autoptr(GPtrArray) runs = NULL;
	gint pid;
	const gchar *body = "services:\n  - {id: a, cmd: [sleep, \"30\"]}\n";

	if (!ds_up(&d, state, body))
		return;
	bar_harness_poll(&d.h);
	ds_click(&d, "tool", 0);
	g_assert_true(ds_pump_toast(&d, "P is up"));
	runs = bar_devservices_state_load(statefile);
	g_assert_cmpuint(runs->len, ==, 1);
	pid = ((BarDevservicesRun *)g_ptr_array_index(runs, 0))->pid;
	ds_down(&d);
	/* The widget going away does not stop the user's servers. */
	g_assert_true(pid_alive(pid));

	if (!ds_up(&d, state, body))
		return;
	g_assert_true(ds_pump_label(&d, "1/1"));
	ds_click(&d, "tool", 1);
	g_assert_true(ds_pump_toast(&d, "P is down"));
	g_assert_true(wait_dead(pid));
	ds_down(&d);
	rm_rf(state);
}

/* A state file naming a live process with the WRONG start time -- a
   reused pid -- is dropped, and Down does not touch that process. */
static void
test_widget_stale_state_is_not_signalled(void)
{
	Ds d;
	g_autofree gchar *state = scratch_dir();
	g_autofree gchar *statefile = g_build_filename(state, "p.state", NULL);
	g_autofree gchar *log = g_build_filename(state, "victim.log", NULL);
	g_autofree gchar *text = NULL;
	const gchar *argv[] = { "sleep", "30", NULL };
	BarDevservicesRun *victim;

	victim = bar_devservices_spawn(argv, NULL, NULL, log, "v", "/proc", NULL);
	g_assert_nonnull(victim);
	text = g_strdup_printf("a.pid=%d\na.pgid=%d\na.starttime=%"
	                       G_GUINT64_FORMAT "\n", victim->pid, victim->pgid,
	                       victim->starttime + 7);
	write_file(statefile, text);

	if (!ds_up(&d, state, "services:\n  - {id: a, cmd: [sleep, \"30\"]}\n"))
		return;
	g_assert_true(ds_pump_label(&d, "0/1"));
	ds_click(&d, "tool", 1);
	g_assert_true(ds_pump_toast(&d, "P is down"));
	g_usleep(50000);
	g_assert_true(pid_alive(victim->pid));
	ds_down(&d);

	bar_devservices_run_signal("/proc", victim, SIGKILL);
	g_assert_true(wait_dead(victim->pid));
	bar_devservices_run_free(victim);
	rm_rf(state);
}

/* A bad stack file is shown as a problem in the panel and the good
   ones still work; the alias resolves to the same widget. */
static void
test_widget_problems_and_alias(void)
{
	Ds d;
	g_autofree gchar *bad = NULL;
	GowlBarPlugin *alias;
	g_autoptr(GError) error = NULL;

	if (!ds_up(&d, NULL, "services:\n  - {id: a, cmd: [sleep, \"30\"]}\n"))
		return;
	bad = g_build_filename(d.conf, "broken.yaml", NULL);
	write_file(bad, "root: /x\nservices:\n  - {id: a}\n");
	bar_harness_poll(&d.h);
	g_assert_true(ds_panel_says(&d, "0 of 1 up"));
	{
		g_autoptr(GowlBarPanel) panel = bar_harness_panel(&d.h);
		guint i;
		gboolean found = FALSE;

		for (i = 0; i < gowl_bar_panel_n_items(panel); i++) {
			const gchar *t = gowl_bar_panel_item_get_title(
				gowl_bar_panel_get_item(panel, i));

			if (t != NULL && strstr(t, "broken.yaml: service `a' has "
			                        "no cmd") != NULL)
				found = TRUE;
		}
		g_assert_true(found);
	}
	alias = gowl_bar_registry_instantiate(d.h.registry, "devstack", &error);
	g_assert_no_error(error);
	g_assert_nonnull(alias);
	g_object_unref(alias);
	ds_down(&d);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/devservices/parse/example", test_parse_example);
	g_test_add_func("/devservices/parse/bad", test_parse_bad);
	g_test_add_func("/devservices/parse/load-dir", test_load_dir);
	g_test_add_func("/devservices/plan", test_plan);
	g_test_add_func("/devservices/locate/root-match", test_root_match);
	g_test_add_func("/devservices/locate/stat", test_parse_stat);
	g_test_add_func("/devservices/locate/focus-cwd", test_focus_cwd);
	g_test_add_func("/devservices/ports/net-tcp", test_net_tcp);
	g_test_add_func("/devservices/ports/socket-owner", test_socket_owner);
	g_test_add_func("/devservices/ports/connect", test_port_connects);
	g_test_add_func("/devservices/state/round-trip", test_state_round_trip);
	g_test_add_func("/devservices/group/stop-kills-group",
	                test_spawn_group_stop);
	g_test_add_func("/devservices/group/mismatch-refused",
	                test_spawn_mismatch_refused);
	g_test_add_func("/devservices/group/orphans", test_spawn_orphans);
	g_test_add_func("/devservices/group/spawn-failures",
	                test_spawn_failures);
	g_test_add_func("/devservices/logs", test_logs);
	g_test_add_func("/devservices/status/machine", test_status_machine);
	g_test_add_func("/devservices/status/aggregate", test_aggregate);
	g_test_add_func("/devservices/podman/argv", test_compose_argv);
	g_test_add_func("/devservices/podman/running", test_podman_running);
	g_test_add_func("/devservices/port-clashes", test_port_clashes);
	g_test_add_func("/devservices/scan/detect", test_scan);
	g_test_add_func("/devservices/scan/write-draft", test_write_draft);
	g_test_add_func("/devservices/scan/safe-key", test_safe_key);
	g_test_add_func("/devservices/widget/no-stacks", test_widget_no_stacks);
	g_test_add_func("/devservices/widget/up-down", test_widget_up_down);
	g_test_add_func("/devservices/widget/failure", test_widget_failure);
	g_test_add_func("/devservices/widget/missing-podman",
	                test_widget_missing_podman);
	g_test_add_func("/devservices/widget/podman", test_widget_podman);
	g_test_add_func("/devservices/widget/curl-health",
	                test_widget_curl_health);
	g_test_add_func("/devservices/widget/stuck", test_widget_stuck);
	g_test_add_func("/devservices/widget/colours", test_widget_colours);
	g_test_add_func("/devservices/widget/notify-off", test_widget_notify_off);
	g_test_add_func("/devservices/widget/quoting",
	                test_widget_logs_open_quoting);
	g_test_add_func("/devservices/widget/tasks-and-cycle",
	                test_widget_tasks_and_cycle);
	g_test_add_func("/devservices/widget/takeover", test_widget_takeover);
	g_test_add_func("/devservices/widget/takeover-refuses-self",
	                test_widget_takeover_refuses_self);
	g_test_add_func("/devservices/widget/scan-declare",
	                test_widget_scan_declare);
	g_test_add_func("/devservices/widget/auto-and-pin",
	                test_widget_auto_and_pin);
	g_test_add_func("/devservices/widget/adopt-after-restart",
	                test_widget_adopt_after_restart);
	g_test_add_func("/devservices/widget/stale-state",
	                test_widget_stale_state_is_not_signalled);
	g_test_add_func("/devservices/widget/problems-and-alias",
	                test_widget_problems_and_alias);
	return g_test_run();
}
