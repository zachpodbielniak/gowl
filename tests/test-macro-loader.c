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
 * test-macro-loader.c - Finding, compiling and opening macro files.
 *
 * The loader is included directly, so these run without a compositor:
 * the search order, name resolution, a real crispy compile of a macro
 * written by the test, recompiling on edit, the #define-driven mode and
 * budget, and every way a macro file can be refused -- a compile error,
 * no run function, the wrong ABI, a constructor that crashes.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "gowl.h"
#include "macro/gowl-macro-private.h"
#include "../modules/macro/gowl-macro-loader.c"

typedef struct {
	gchar           *root;
	gchar           *dir_a;
	gchar           *dir_b;
	gchar           *cache;
	GowlMacroLoader *loader;
} Fixture;

/* Deliberate faults log warnings; lift the fatal mask around them. */
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
write_file(
	const gchar *dir,
	const gchar *name,
	const gchar *text
){
	g_autofree gchar *path = g_build_filename(dir, name, NULL);

	g_assert_true(g_file_set_contents(path, text, -1, NULL));
}

/* A macro that reports @word through its result. */
static gchar *
macro_saying(const gchar *word)
{
	return g_strdup_printf(
		"#include <gowl/gowl.h>\n"
		"G_MODULE_EXPORT const gchar *gowl_macro_info(void)\n"
		"{ return \"says %s\"; }\n"
		"G_MODULE_EXPORT gboolean gowl_macro_run(GowlMacroContext *ctx)\n"
		"{ gowl_macro_set_result(ctx, \"%s\"); return TRUE; }\n",
		word, word);
}

static void
fixture_setup(
	Fixture       *f,
	gconstpointer  data
){
	g_autofree gchar *src = NULL;

	(void)data;
	f->root = g_dir_make_tmp("gowl-macro-loader-XXXXXX", NULL);
	g_assert_nonnull(f->root);
	f->dir_a = g_build_filename(f->root, "a", NULL);
	f->dir_b = g_build_filename(f->root, "b", NULL);
	f->cache = g_build_filename(f->root, "cache", NULL);
	g_mkdir(f->dir_a, 0700);
	g_mkdir(f->dir_b, 0700);
	f->loader = gowl_macro_loader_new(f->cache);
	{
		const gchar *dirs[] = { f->dir_a, f->dir_b, NULL };

		gowl_macro_loader_set_dirs(f->loader, dirs);
	}
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
fixture_teardown(
	Fixture       *f,
	gconstpointer  data
){
	(void)data;
	gowl_macro_loader_free(f->loader);
	rm_rf(f->root);
	g_free(f->root);
	g_free(f->dir_a);
	g_free(f->dir_b);
	g_free(f->cache);
}

/* Runs a loaded script with a context that has no host. */
static gchar *
run_script(const GowlMacroScript *s)
{
	g_autoptr(GowlMacroContext) ctx = NULL;

	ctx = gowl_macro_context_new(NULL, s->name, s->path, NULL,
	                             GOWL_MACRO_TRIGGER_API, NULL, FALSE);
	g_assert_true(s->run(ctx));
	return g_strdup(gowl_macro_context_get_result(ctx));
}

/* --- the search path --- */

static void
test_default_dirs(void)
{
	g_auto(GStrv) dirs = NULL;
	g_autofree gchar *xdg = NULL;

	g_setenv("GOWL_MACRO_DIR", "/env/one:/env/two", TRUE);
	dirs = gowl_macro_loader_default_dirs("/cfg/a:~/cfg-b:/env/one");
	g_unsetenv("GOWL_MACRO_DIR");

	/* configured first, then the environment, then XDG, then system;
	   a repeat is dropped, ~/ expanded */
	g_assert_cmpstr(dirs[0], ==, "/cfg/a");
	g_assert_true(g_str_has_suffix(dirs[1], "/cfg-b"));
	g_assert_cmpstr(dirs[2], ==, "/env/one");
	g_assert_cmpstr(dirs[3], ==, "/env/two");
	xdg = g_build_filename(g_get_user_config_dir(), "gowl", "macros", NULL);
	g_assert_cmpstr(dirs[4], ==, xdg);
	g_assert_true(g_strv_contains((const gchar * const *)dirs,
	                              "/usr/share/gowl/macros"));
	g_assert_true(g_strv_contains((const gchar * const *)dirs,
	                              "/etc/gowl/macros"));
}

static void
test_normalize(void)
{
	g_autofree gchar *a = gowl_macro_loader_normalize_name("sort.c");
	g_autofree gchar *b = gowl_macro_loader_normalize_name("/x/y/sort.so");
	g_autofree gchar *c = gowl_macro_loader_normalize_name("sort");

	g_assert_cmpstr(a, ==, "sort");
	g_assert_cmpstr(b, ==, "sort");
	g_assert_cmpstr(c, ==, "sort");
}

/* First directory wins; name, name.c and name.so all resolve; a path is
   taken as-is and never looked up. */
static void
test_resolve(
	Fixture       *f,
	gconstpointer  data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *p1 = NULL;
	g_autofree gchar *p2 = NULL;
	g_autofree gchar *p3 = NULL;
	g_autofree gchar *expect_a = NULL;
	g_autofree gchar *explicit = NULL;
	g_autofree gchar *subdir = NULL;

	(void)data;
	write_file(f->dir_a, "both.c", "/* a */");
	write_file(f->dir_b, "both.c", "/* b */");
	write_file(f->dir_b, "only-b.so", "not really a .so");

	expect_a = g_build_filename(f->dir_a, "both.c", NULL);
	p1 = gowl_macro_loader_resolve(f->loader, "both", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(p1, ==, expect_a);
	p2 = gowl_macro_loader_resolve(f->loader, "both.c", &error);
	g_assert_cmpstr(p2, ==, expect_a);
	p3 = gowl_macro_loader_resolve(f->loader, "only-b", &error);
	g_assert_no_error(error);
	g_assert_true(g_str_has_suffix(p3, "/b/only-b.so"));

	/* a path: exactly that file */
	explicit = gowl_macro_loader_resolve(f->loader,
	                                     g_build_filename(f->dir_b, "both.c",
	                                                      NULL), &error);
	g_assert_true(g_str_has_suffix(explicit, "/b/both.c"));

	/* a directory named like a macro is not a macro */
	subdir = g_build_filename(f->dir_a, "dirmacro", NULL);
	g_mkdir(subdir, 0700);
	g_assert_null(gowl_macro_loader_resolve(f->loader, "dirmacro", &error));
	g_assert_error(error, GOWL_MACRO_LOADER_ERROR,
	               GOWL_MACRO_LOADER_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_rmdir(subdir);

	g_assert_null(gowl_macro_loader_resolve(f->loader, "nope", &error));
	g_assert_error(error, GOWL_MACRO_LOADER_ERROR,
	               GOWL_MACRO_LOADER_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_null(gowl_macro_loader_resolve(f->loader, "/no/such/file.c",
	                                        &error));
	g_assert_error(error, GOWL_MACRO_LOADER_ERROR,
	               GOWL_MACRO_LOADER_ERROR_NOT_FOUND);
}

static void
test_discover(
	Fixture       *f,
	gconstpointer  data
){
	g_autoptr(GPtrArray) found = NULL;

	(void)data;
	write_file(f->dir_a, "zeta.c", "");
	write_file(f->dir_a, "alpha.c", "");
	write_file(f->dir_b, "alpha.so", "");   /* shadowed by a/alpha.c */
	write_file(f->dir_b, "beta.so", "");
	write_file(f->dir_b, "notes.txt", "");

	found = gowl_macro_loader_discover(f->loader);
	g_assert_cmpuint(found->len, ==, 6);
	g_assert_cmpstr(g_ptr_array_index(found, 0), ==, "alpha");
	g_assert_true(g_str_has_suffix(g_ptr_array_index(found, 1), "/a/alpha.c"));
	g_assert_cmpstr(g_ptr_array_index(found, 2), ==, "beta");
	g_assert_cmpstr(g_ptr_array_index(found, 4), ==, "zeta");
}

/* --- compiling and opening --- */

static void
test_compile_and_run(
	Fixture       *f,
	gconstpointer  data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *src = macro_saying("first");
	g_autofree gchar *result = NULL;
	const GowlMacroScript *s;

	(void)data;
	write_file(f->dir_a, "say.c", src);
	s = gowl_macro_loader_load(f->loader, "say", &error);
	if (s == NULL && g_error_matches(error, GOWL_MACRO_LOADER_ERROR,
	                                 GOWL_MACRO_LOADER_ERROR_COMPILE)
	    && strstr(error->message, "no C compiler") != NULL) {
		g_test_skip("no gcc here");
		return;
	}
	g_assert_no_error(error);
	g_assert_nonnull(s);
	g_assert_cmpstr(s->name, ==, "say");
	g_assert_cmpstr(s->description, ==, "says first");
	g_assert_false(s->threaded);
	g_assert_cmpuint(s->timeout_ms, ==, 0);
	result = run_script(s);
	g_assert_cmpstr(result, ==, "first");

	/* unchanged source: the same script, no recompile */
	g_assert_true(gowl_macro_loader_load(f->loader, "say", &error) == s);
}

/* Edit the file, run it again: the new code runs. */
static void
test_recompile_on_edit(
	Fixture       *f,
	gconstpointer  data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *v1 = macro_saying("before");
	g_autofree gchar *v2 = macro_saying("after");
	g_autofree gchar *r1 = NULL;
	g_autofree gchar *r2 = NULL;
	const GowlMacroScript *s;

	(void)data;
	write_file(f->dir_a, "edit.c", v1);
	s = gowl_macro_loader_load(f->loader, "edit", &error);
	g_assert_no_error(error);
	r1 = run_script(s);
	g_assert_cmpstr(r1, ==, "before");

	write_file(f->dir_a, "edit.c", v2);
	s = gowl_macro_loader_load(f->loader, "edit", &error);
	g_assert_no_error(error);
	r2 = run_script(s);
	g_assert_cmpstr(r2, ==, "after");
}

static void
test_defines(
	Fixture       *f,
	gconstpointer  data
){
	g_autoptr(GError) error = NULL;
	const GowlMacroScript *s;

	(void)data;
	write_file(f->dir_a, "slow.c",
	           "#include <gowl/gowl.h>\n"
	           "#define GOWL_MACRO_THREADED 1\n"
	           "#define GOWL_MACRO_TIMEOUT_MS 12345\n"
	           "G_MODULE_EXPORT gboolean gowl_macro_run(GowlMacroContext *c)\n"
	           "{ (void)c; return TRUE; }\n");
	s = gowl_macro_loader_load(f->loader, "slow", &error);
	g_assert_no_error(error);
	g_assert_true(s->threaded);
	g_assert_true(s->has_timeout);
	g_assert_cmpuint(s->timeout_ms, ==, 12345);

	/* zero is a request (no watchdog), not "the default" */
	write_file(f->dir_a, "unbounded.c",
	           "#include <gowl/gowl.h>\n"
	           "#define GOWL_MACRO_TIMEOUT_MS 0\n"
	           "G_MODULE_EXPORT gboolean gowl_macro_run(GowlMacroContext *c)\n"
	           "{ (void)c; return TRUE; }\n");
	s = gowl_macro_loader_load(f->loader, "unbounded", &error);
	g_assert_no_error(error);
	g_assert_true(s->has_timeout);
	g_assert_cmpuint(s->timeout_ms, ==, 0);
	g_assert_true(gowl_macro_loader_compile_only(f->loader, "slow", &error));
}

/* Every way a file is refused, each with its own error. */
static void
test_refusals(
	Fixture       *f,
	gconstpointer  data
){
	g_autoptr(GError) error = NULL;

	(void)data;
	/* does not compile: one line of message, naming the file */
	write_file(f->dir_a, "broken.c",
	           "#include <gowl/gowl.h>\nthis is not C;\n");
	g_assert_null(gowl_macro_loader_load(f->loader, "broken", &error));
	g_assert_error(error, GOWL_MACRO_LOADER_ERROR,
	               GOWL_MACRO_LOADER_ERROR_COMPILE);
	g_assert_nonnull(strstr(error->message, "broken.c"));
	g_assert_null(strchr(error->message, '\n'));
	g_clear_error(&error);
	g_assert_false(gowl_macro_loader_compile_only(f->loader, "broken",
	                                              &error));
	g_clear_error(&error);

	/* compiles, but has no run function */
	write_file(f->dir_a, "norun.c",
	           "#include <gowl/gowl.h>\n"
	           "G_MODULE_EXPORT int something_else(void) { return 1; }\n");
	g_assert_null(gowl_macro_loader_load(f->loader, "norun", &error));
	g_assert_error(error, GOWL_MACRO_LOADER_ERROR,
	               GOWL_MACRO_LOADER_ERROR_LOAD);
	g_clear_error(&error);

	/* written for another ABI */
	write_file(f->dir_a, "future.c",
	           "#include <gowl/gowl.h>\n"
	           "G_MODULE_EXPORT const guint gowl_macro_abi = 99;\n"
	           "G_MODULE_EXPORT gboolean gowl_macro_run(GowlMacroContext *c)\n"
	           "{ (void)c; return TRUE; }\n");
	g_assert_null(gowl_macro_loader_load(f->loader, "future", &error));
	g_assert_error(error, GOWL_MACRO_LOADER_ERROR,
	               GOWL_MACRO_LOADER_ERROR_ABI);
	g_clear_error(&error);

	/* the right ABI is fine */
	write_file(f->dir_a, "present.c",
	           "#include <gowl/gowl.h>\n"
	           "G_MODULE_EXPORT const guint gowl_macro_abi = GOWL_MACRO_ABI;\n"
	           "G_MODULE_EXPORT gboolean gowl_macro_run(GowlMacroContext *c)\n"
	           "{ (void)c; return TRUE; }\n");
	g_assert_nonnull(gowl_macro_loader_load(f->loader, "present", &error));
	g_assert_no_error(error);
}

/* A constructor that crashes is contained while loading. */
static void
test_constructor_fault(
	Fixture       *f,
	gconstpointer  data
){
	g_autoptr(GError) error = NULL;

	(void)data;
	write_file(f->dir_a, "ctor.c",
	           "#include <gowl/gowl.h>\n"
	           "__attribute__((constructor)) static void boom(void)\n"
	           "{ volatile int *p = NULL; *p = 1; }\n"
	           "G_MODULE_EXPORT gboolean gowl_macro_run(GowlMacroContext *c)\n"
	           "{ (void)c; return TRUE; }\n");
	allow_warnings();
	g_assert_null(gowl_macro_loader_load(f->loader, "ctor", &error));
	restore_warnings();
	g_assert_error(error, GOWL_MACRO_LOADER_ERROR,
	               GOWL_MACRO_LOADER_ERROR_FAULT);
	g_clear_error(&error);

	/* ... and so is a description function that crashes */
	write_file(f->dir_a, "badinfo.c",
	           "#include <gowl/gowl.h>\n"
	           "G_MODULE_EXPORT const gchar *gowl_macro_info(void)\n"
	           "{ volatile const gchar *p = NULL; return (const gchar *)*p "
	           "== 'x' ? \"x\" : \"y\"; }\n"
	           "G_MODULE_EXPORT gboolean gowl_macro_run(GowlMacroContext *c)\n"
	           "{ (void)c; return TRUE; }\n");
	allow_warnings();
	g_assert_null(gowl_macro_loader_load(f->loader, "badinfo", &error));
	restore_warnings();
	g_assert_error(error, GOWL_MACRO_LOADER_ERROR,
	               GOWL_MACRO_LOADER_ERROR_FAULT);
	g_assert_nonnull(strstr(error->message, "gowl_macro_info"));
}

/* forget() makes the next load look again (a new directory order). */
static void
test_forget(
	Fixture       *f,
	gconstpointer  data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *a = macro_saying("from-a");
	g_autofree gchar *b = macro_saying("from-b");
	g_autofree gchar *r1 = NULL;
	g_autofree gchar *r2 = NULL;
	const gchar *swapped[3];

	(void)data;
	write_file(f->dir_a, "which.c", a);
	write_file(f->dir_b, "which.c", b);
	r1 = run_script(gowl_macro_loader_load(f->loader, "which", &error));
	g_assert_cmpstr(r1, ==, "from-a");

	swapped[0] = f->dir_b;
	swapped[1] = f->dir_a;
	swapped[2] = NULL;
	gowl_macro_loader_set_dirs(f->loader, swapped);
	gowl_macro_loader_forget(f->loader);
	r2 = run_script(gowl_macro_loader_load(f->loader, "which", &error));
	g_assert_cmpstr(r2, ==, "from-b");
}

/* The C-macro registry the module consults before any file. */
static gboolean
registered(
	GowlMacroContext *ctx,
	gpointer          data
){
	gowl_macro_set_result(ctx, data);
	return TRUE;
}

static void
test_registry(void)
{
	g_auto(GStrv) names = NULL;
	GowlMacroFunc func;
	gpointer data;

	gowl_macro_register_func("zz-test", registered, g_strdup("yes"), g_free);
	gowl_macro_register_func("aa-test", registered, NULL, NULL);
	g_assert_true(gowl_macro_registry_lookup("zz-test", &func, &data));
	g_assert_true(func == registered);
	g_assert_cmpstr(data, ==, "yes");
	names = gowl_macro_registry_names();
	g_assert_cmpstr(names[0], ==, "aa-test");
	g_assert_true(gowl_macro_unregister_func("zz-test"));
	g_assert_false(gowl_macro_unregister_func("zz-test"));
	g_assert_false(gowl_macro_registry_lookup("zz-test", &func, &data));
	gowl_macro_unregister_func("aa-test");
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/macro/loader/default-dirs", test_default_dirs);
	g_test_add_func("/macro/loader/normalize", test_normalize);
	g_test_add_func("/macro/loader/registry", test_registry);
#define ADD(path, fn) \
	g_test_add("/macro/loader/" path, Fixture, NULL, fixture_setup, fn, \
	           fixture_teardown)
	ADD("resolve", test_resolve);
	ADD("discover", test_discover);
	ADD("compile-and-run", test_compile_and_run);
	ADD("recompile-on-edit", test_recompile_on_edit);
	ADD("defines", test_defines);
	ADD("refusals", test_refusals);
	ADD("constructor-fault", test_constructor_fault);
	ADD("forget", test_forget);
#undef ADD

	return g_test_run();
}
