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
 * test-hotkey-registry.c - libgowl's hotkey registries.
 *
 * What a C config, a crispy hotkey script or cmacs uses before (and
 * whether or not) the module is loaded: functions -- a call's facts and
 * result, a replaced function's destroy notify, a faulting function
 * contained by the guard -- contexts as predicates and as booleans,
 * sources added, replaced, removed and refused, gowl_hotkey_define()
 * checking its key, and the counter that tells the module to reload.
 */

#include <glib.h>
#include <signal.h>
#include <string.h>

#include "gowl.h"
#include "hotkey/gowl-hotkey-private.h"
#include "util/gowl-fault-guard.h"

static gint destroyed;

static void
count_destroy(gpointer data)
{
	(void)data;
	destroyed++;
}

static void
func_echo(
	GowlHotkeyCall *call,
	gpointer        data
){
	g_autofree gchar *joined = g_strjoinv("+",
		(gchar **)gowl_hotkey_call_get_args(call));
	g_autofree gchar *r = g_strdup_printf("%s:%s:%s:%s",
		(const gchar *)data, gowl_hotkey_call_get_id(call),
		gowl_hotkey_call_get_trigger(call), joined);

	gowl_hotkey_call_set_result(call, r);
}

static void
func_crash(
	GowlHotkeyCall *call,
	gpointer        data
){
	volatile gint *bad = NULL;

	(void)call;
	(void)data;
	*bad = 1;
}

static gboolean
pred_focused(
	GowlHotkeyCall *call,
	gpointer        data
){
	(void)data;
	return gowl_hotkey_call_get_focused(call) != NULL;
}

static void
test_functions(void)
{
	const gchar *args[] = { "a", "b", NULL };
	GowlHotkeyCall *call;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) names = NULL;
	g_autofree gchar *result = NULL;
	guint gen;

	destroyed = 0;
	gen = gowl_hotkey_get_generation();
	gowl_hotkey_register_func("echo", func_echo, (gpointer)"one",
	                          count_destroy);
	g_assert_cmpuint(gowl_hotkey_get_generation(), !=, gen);
	g_assert_true(gowl_hotkey_has_func("echo"));
	names = gowl_hotkey_list_funcs();
	g_assert_true(g_strv_contains((const gchar * const *)names, "echo"));

	call = gowl_hotkey_call_new(NULL, NULL, "hk-1", "hotkey", "#!t", args);
	g_assert_cmpstr(gowl_hotkey_call_get_arg(call, 1), ==, "b");
	g_assert_null(gowl_hotkey_call_get_arg(call, 2));
	g_assert_cmpstr(gowl_hotkey_call_get_detail(call), ==, "#!t");
	g_assert_true(gowl_hotkey_invoke_func("echo", call, 1000, &error));
	g_assert_no_error(error);
	result = gowl_hotkey_call_steal_result(call);
	g_assert_cmpstr(result, ==, "one:hk-1:hotkey:a+b");
	gowl_hotkey_call_free(call);

	/* replaced: the old data's destroy notify runs */
	gowl_hotkey_register_func("echo", func_echo, (gpointer)"two",
	                          count_destroy);
	g_assert_cmpint(destroyed, ==, 1);
	g_assert_true(gowl_hotkey_unregister_func("echo"));
	g_assert_cmpint(destroyed, ==, 2);
	g_assert_false(gowl_hotkey_unregister_func("echo"));
	g_assert_false(gowl_hotkey_has_func("echo"));

	/* unknown */
	call = gowl_hotkey_call_new(NULL, NULL, NULL, NULL, NULL, NULL);
	g_assert_cmpstr(gowl_hotkey_call_get_trigger(call), ==, "api");
	g_assert_nonnull(gowl_hotkey_call_get_args(call));
	g_assert_false(gowl_hotkey_invoke_func("nope", call, 1000, &error));
	g_assert_error(error, GOWL_HOTKEY_ERROR, GOWL_HOTKEY_ERROR_NOT_FOUND);
	gowl_hotkey_call_free(call);
}

static void
test_fault_contained(void)
{
	GowlHotkeyCall *call;
	g_autoptr(GError) error = NULL;
	GLogLevelFlags saved;

	gowl_fault_guard_init();
	if (!gowl_fault_guard_is_available()) {
		g_test_skip("the fault guard could not install");
		return;
	}
	gowl_hotkey_register_func("crash", func_crash, NULL, NULL);
	call = gowl_hotkey_call_new(NULL, NULL, "x", NULL, NULL, NULL);
	saved = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);
	g_assert_false(gowl_hotkey_invoke_func("crash", call, 1000, &error));
	g_log_set_always_fatal(saved);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "crash"));
	g_assert_nonnull(strstr(error->message, "stopped"));
	gowl_hotkey_call_free(call);
	gowl_hotkey_unregister_func("crash");
}

static void
test_contexts(void)
{
	GowlHotkeyCall *with;
	GowlHotkeyCall *without;
	gboolean v = FALSE;
	g_auto(GStrv) names = NULL;
	gint dummy;

	with = gowl_hotkey_call_new(NULL, &dummy, "x", "context", NULL, NULL);
	without = gowl_hotkey_call_new(NULL, NULL, "x", "context", NULL, NULL);
	g_assert_cmpint(gowl_hotkey_eval_context("nothing", with, 100, NULL), ==,
	                -1);

	/* a boolean someone keeps up to date (cmacs's Elisp contexts) */
	gowl_hotkey_set_context("gaming", TRUE);
	g_assert_true(gowl_hotkey_get_context("gaming", &v));
	g_assert_true(v);
	g_assert_cmpint(gowl_hotkey_eval_context("gaming", with, 100, NULL), ==, 1);
	gowl_hotkey_set_context("gaming", FALSE);
	g_assert_cmpint(gowl_hotkey_eval_context("gaming", with, 100, NULL), ==, 0);

	/* a predicate */
	gowl_hotkey_register_context("has-focus", pred_focused, NULL, NULL);
	g_assert_cmpint(gowl_hotkey_eval_context("has-focus", with, 100, NULL),
	                ==, 1);
	g_assert_cmpint(gowl_hotkey_eval_context("has-focus", without, 100, NULL),
	                ==, 0);
	/* a predicate wins over a boolean of the same name */
	gowl_hotkey_set_context("has-focus", FALSE);
	g_assert_cmpint(gowl_hotkey_eval_context("has-focus", with, 100, NULL),
	                ==, 1);
	names = gowl_hotkey_list_contexts();
	g_assert_true(g_strv_contains((const gchar * const *)names, "gaming"));
	g_assert_true(g_strv_contains((const gchar * const *)names, "has-focus"));
	/* listed once */
	{
		guint i;
		guint n = 0;

		for (i = 0; names[i] != NULL; i++)
			if (g_strcmp0(names[i], "has-focus") == 0)
				n++;
		g_assert_cmpuint(n, ==, 1);
	}
	g_assert_true(gowl_hotkey_unregister_context("has-focus"));
	g_assert_true(gowl_hotkey_clear_context("has-focus"));
	g_assert_true(gowl_hotkey_clear_context("gaming"));
	g_assert_false(gowl_hotkey_get_context("gaming", NULL));
	g_assert_cmpint(gowl_hotkey_eval_context("gaming", with, 100, NULL), ==,
	                -1);
	gowl_hotkey_call_free(with);
	gowl_hotkey_call_free(without);
}

static void
test_sources(void)
{
	g_autoptr(GPtrArray) s = NULL;
	g_autoptr(GError) error = NULL;
	guint gen;

	gowl_hotkey_clear_sources();
	gen = gowl_hotkey_get_generation();
	g_assert_true(gowl_hotkey_add_yaml("layer",
		"hotkeys: [{key: \"CapsLock & h\", do: {send: \"{Left}\"}}]\n",
		&error));
	g_assert_no_error(error);
	g_assert_cmpuint(gowl_hotkey_get_generation(), !=, gen);
	g_assert_true(gowl_hotkey_add_yaml("other", "remaps: {a: b}\n", &error));
	/* the same id again: replaced in place */
	g_assert_true(gowl_hotkey_add_yaml("layer", "hotkeys: []\n", &error));
	s = gowl_hotkey_dup_sources();
	g_assert_cmpuint(s->len, ==, 2);
	g_assert_cmpstr(((GowlHotkeySource *)g_ptr_array_index(s, 0))->id, ==,
	                "layer");
	g_assert_cmpstr(((GowlHotkeySource *)g_ptr_array_index(s, 0))->yaml, ==,
	                "hotkeys: []\n");
	g_clear_pointer(&s, g_ptr_array_unref);
	/* refused: bad YAML, or not a section */
	g_assert_false(gowl_hotkey_add_yaml("bad", "hotkeys: [", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_false(gowl_hotkey_add_yaml("bad", "- a list\n", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_true(gowl_hotkey_remove_yaml("layer"));
	g_assert_false(gowl_hotkey_remove_yaml("layer"));
	s = gowl_hotkey_dup_sources();
	g_assert_cmpuint(s->len, ==, 1);
	gowl_hotkey_clear_sources();
	g_clear_pointer(&s, g_ptr_array_unref);
	s = gowl_hotkey_dup_sources();
	g_assert_cmpuint(s->len, ==, 0);
}

static void
test_define(void)
{
	g_autoptr(GPtrArray) s = NULL;
	g_autoptr(GError) error = NULL;
	GowlHotkeySource *src;

	gowl_hotkey_clear_sources();
	destroyed = 0;
	/* the key is checked here, and the data freed on a refusal */
	g_assert_false(gowl_hotkey_define("bad", "Frobnicate", func_echo, NULL,
	                                  count_destroy, &error));
	g_assert_error(error, GOWL_HOTKEY_ERROR, GOWL_HOTKEY_ERROR_UNKNOWN_KEY);
	g_assert_cmpint(destroyed, ==, 1);
	g_assert_false(gowl_hotkey_has_func("bad"));
	g_clear_error(&error);

	g_assert_true(gowl_hotkey_define("say-hi", "#!h", func_echo,
	                                 (gpointer)"hi", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(gowl_hotkey_has_func("say-hi"));
	s = gowl_hotkey_dup_sources();
	g_assert_cmpuint(s->len, ==, 1);
	src = g_ptr_array_index(s, 0);
	g_assert_cmpstr(src->id, ==, "c:say-hi");
	/* `#' quoted, so it is not a YAML comment */
	g_assert_nonnull(strstr(src->yaml, "key: \"#!h\""));
	g_assert_nonnull(strstr(src->yaml, "do: {call: \"say-hi\"}"));
	g_assert_true(gowl_hotkey_undefine("say-hi"));
	g_assert_false(gowl_hotkey_has_func("say-hi"));
	g_assert_false(gowl_hotkey_undefine("say-hi"));
	g_clear_pointer(&s, g_ptr_array_unref);
	s = gowl_hotkey_dup_sources();
	g_assert_cmpuint(s->len, ==, 0);
}

/* Registration from many threads at once: nothing lost, nothing torn. */
static gpointer
register_many(gpointer data)
{
	guint base = GPOINTER_TO_UINT(data);
	guint i;

	for (i = 0; i < 200; i++) {
		g_autofree gchar *name = g_strdup_printf("t%u-%u", base, i);

		gowl_hotkey_register_func(name, func_echo, NULL, NULL);
		gowl_hotkey_set_context(name, i % 2 == 0);
	}
	return NULL;
}

static void
test_threads(void)
{
	GThread *t[4];
	g_auto(GStrv) names = NULL;
	guint i;
	guint n = 0;

	for (i = 0; i < 4; i++)
		t[i] = g_thread_new("reg", register_many, GUINT_TO_POINTER(i));
	for (i = 0; i < 4; i++)
		g_thread_join(t[i]);
	names = gowl_hotkey_list_funcs();
	for (i = 0; names[i] != NULL; i++)
		if (names[i][0] == 't' && strchr(names[i], '-') != NULL) {
			n++;
			gowl_hotkey_unregister_func(names[i]);
			gowl_hotkey_clear_context(names[i]);
		}
	g_assert_cmpuint(n, ==, 800);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/hotkey/registry/functions", test_functions);
	g_test_add_func("/hotkey/registry/fault-contained", test_fault_contained);
	g_test_add_func("/hotkey/registry/contexts", test_contexts);
	g_test_add_func("/hotkey/registry/sources", test_sources);
	g_test_add_func("/hotkey/registry/define", test_define);
	g_test_add_func("/hotkey/registry/threads", test_threads);

	return g_test_run();
}
