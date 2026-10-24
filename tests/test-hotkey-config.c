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
 * test-hotkey-config.c - Every shipped hotkey example, loaded.
 *
 * The documentation shows these files, so they are held to: the full
 * YAML example through GowlConfig (the `hotkey:' key is known, nothing
 * else in it is a problem) and then the module's own model (no problem
 * at all, and the hotkeys it promises are there); every cookbook case
 * under data/example-hotkey-cases/ the same way; the C config example
 * compiled in and run, with the sources and functions it registers
 * loading clean; the example crispy script compiled in too, its init
 * run and its registrations checked; and the `hotkey:' section kept
 * whole on a GowlConfig, a non-mapping one reported.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include <yaml-glib.h>

#include "gowl.h"
#include "hotkey/gowl-hotkey-private.h"
#include "../modules/hotkey/gowl-hotkey-model.c"
#include "../data/example-hotkey.c"
#include "../data/example-hotkey-script.c"

/* Swallows the one warning a test names (GTest makes warnings fatal and
 * a bad section warns on purpose); g_test_expect_message() does not see
 * structured logging. */
static const gchar *expected_warning;
static guint        saw_expected_warning;

static GLogWriterOutput
expect_warning_writer(
	GLogLevelFlags   level,
	const GLogField *fields,
	gsize            n_fields,
	gpointer         user_data
){
	gsize i;

	if (expected_warning != NULL && (level & G_LOG_LEVEL_WARNING)) {
		for (i = 0; i < n_fields; i++)
			if (g_strcmp0(fields[i].key, "MESSAGE") == 0
			    && strstr((const gchar *)fields[i].value,
			              expected_warning) != NULL) {
				saw_expected_warning++;
				return G_LOG_WRITER_HANDLED;
			}
	}
	return g_log_writer_default(level, fields, n_fields, user_data);
}

static void
assert_no_problems(HkSet *set)
{
	guint i;

	for (i = 0; i < set->problems->len; i++)
		g_test_message("problem: %s",
		               (const gchar *)g_ptr_array_index(set->problems, i));
	g_assert_cmpuint(set->problems->len, ==, 0);
}

/* A whole config file: GowlConfig takes it without a problem, and the
 * module's model takes its `hotkey:' sections without one either. */
static HkSet *
load_config_file(const gchar *path)
{
	g_autoptr(GowlConfig) config = gowl_config_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(HkLoader) loader = hk_loader_new();
	GPtrArray *nodes;
	HkSet *set;
	guint i;

	g_assert_true(gowl_config_load_yaml(config, path, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(gowl_config_get_problem_count(config), ==, 0);
	nodes = gowl_config_get_hotkey_nodes(config);
	g_assert_cmpuint(nodes->len, ==, 1);
	for (i = 0; i < nodes->len; i++)
		hk_loader_add_node(loader, g_ptr_array_index(nodes, i), path,
		                   gowl_config_get_hotkey_base_dir(config));
	set = hk_loader_finish(loader);
	assert_no_problems(set);
	return set;
}

static void
test_full_example(void)
{
	g_autofree gchar *path = g_build_filename(GOWL_TEST_DATA,
	                                          "example-hotkey.yaml", NULL);
	g_autoptr(HkSet) set = load_config_file(path);
	HkHotkey *hk;

	g_assert_cmpuint(set->hotkeys->len, >=, 20);
	g_assert_nonnull(hk_set_find_hotkey(set, "#!t"));
	hk = hk_set_find_hotkey(set, "browser");
	g_assert_nonnull(hk);
	g_assert_cmpstr(hk->description, ==, "a browser");
	g_assert_nonnull(hk_set_find_hotkey(set, "CapsLock & j"));
	g_assert_nonnull(hk_set_find_hotkey(set, "^+d@browser"));
	g_assert_cmpuint(hk_set_find_hotkey(set, "Esc")->hold_ms, ==, 500);
	g_assert_cmpuint(hk_set_find_hotkey(set, "LShift")->double_ms, ==, 300);
	g_assert_true(hk_set_find_hotkey(set, "remap:AppsKey")->is_remap);
	g_assert_nonnull(hk_set_find_hotkey(set, "h#resize"));
	g_assert_nonnull(g_hash_table_lookup(set->modes, "resize"));
	g_assert_nonnull(g_hash_table_lookup(set->contexts, "youtube"));
	g_assert_nonnull(g_hash_table_lookup(set->groups, "terminals"));
	g_assert_nonnull(g_hash_table_lookup(set->functions, "tidy"));
	g_assert_cmpuint(set->hotstrings->len, ==, 3);
	g_assert_cmpuint(set->timers->len, ==, 1);
	g_assert_cmpuint(set->hooks->len, ==, 2);
}

/* Every cookbook case: a config snippet with a `hotkey:' section */
static void
test_cookbook_cases(void)
{
	g_autofree gchar *dir = g_build_filename(GOWL_TEST_DATA,
	                                         "example-hotkey-cases", NULL);
	g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
	const gchar *name;
	guint n = 0;

	g_assert_nonnull(d);
	while ((name = g_dir_read_name(d)) != NULL) {
		g_autofree gchar *path = NULL;
		g_autoptr(HkSet) set = NULL;

		if (!g_str_has_suffix(name, ".yaml"))
			continue;
		path = g_build_filename(dir, name, NULL);
		g_test_message("case %s", name);
		set = load_config_file(path);
		/* each case is about something */
		g_assert_cmpuint(set->hotkeys->len + set->hotstrings->len
		                 + set->timers->len + set->hooks->len, >, 0);
		n++;
	}
	/* the documented cookbook */
	g_assert_cmpuint(n, ==, 15);
}

/* The C config example, compiled in and run */
static void
test_c_config_example(void)
{
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(HkLoader) loader = hk_loader_new();
	g_autoptr(HkSet) set = NULL;
	GowlHotkeyCall *call;
	g_autoptr(GError) error = NULL;
	guint i;

	gowl_hotkey_clear_sources();
	g_assert_true(add_hotkeys());
	g_assert_true(gowl_hotkey_has_func("which-window"));
	g_assert_true(gowl_hotkey_has_func("say"));
	{
		g_auto(GStrv) ctx = gowl_hotkey_list_contexts();

		g_assert_true(g_strv_contains((const gchar * const *)ctx,
		                              "floating"));
	}
	/* what it adds loads clean */
	sources = gowl_hotkey_dup_sources();
	g_assert_cmpuint(sources->len, ==, 2);
	for (i = 0; i < sources->len; i++) {
		GowlHotkeySource *s = g_ptr_array_index(sources, i);

		g_assert_true(hk_loader_add_text(loader, s->yaml, s->id, &error));
		g_assert_no_error(error);
	}
	set = hk_loader_finish(loader);
	assert_no_problems(set);
	g_assert_nonnull(hk_set_find_hotkey(set, "which-window"));
	g_assert_nonnull(hk_set_find_hotkey(set, "#Left@floating"));
	/* its context with no window: false */
	call = gowl_hotkey_call_new(NULL, NULL, "x", "context", NULL, NULL);
	g_assert_cmpint(gowl_hotkey_eval_context("floating", call, 1000, NULL),
	                ==, 0);
	gowl_hotkey_call_free(call);
	gowl_hotkey_undefine("which-window");
	gowl_hotkey_unregister_func("say");
	gowl_hotkey_unregister_context("floating");
	gowl_hotkey_clear_sources();
}

/* The example script, compiled in: its init registers what it says */
static void
test_script_example(void)
{
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(HkLoader) loader = hk_loader_new();
	g_autoptr(HkSet) set = NULL;
	guint i;

	gowl_hotkey_clear_sources();
	g_assert_true(gowl_hotkey_script_init());
	g_assert_true(gowl_hotkey_has_func("script-count"));
	g_assert_true(gowl_hotkey_has_func("stash"));
	sources = gowl_hotkey_dup_sources();
	g_assert_cmpuint(sources->len, ==, 2);
	for (i = 0; i < sources->len; i++) {
		GowlHotkeySource *s = g_ptr_array_index(sources, i);

		g_assert_true(hk_loader_add_text(loader, s->yaml, s->id, NULL));
	}
	set = hk_loader_finish(loader);
	assert_no_problems(set);
	g_assert_nonnull(hk_set_find_hotkey(set, "script-count"));
	g_assert_nonnull(hk_set_find_hotkey(set, "F12@working-hours"));
	gowl_hotkey_script_fini();
	gowl_hotkey_undefine("script-count");
	gowl_hotkey_unregister_func("stash");
	gowl_hotkey_unregister_context("working-hours");
	gowl_hotkey_clear_sources();
}

/* The section is kept whole on the config; a non-mapping is a problem */
static void
test_config_section(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("hotkey-config-XXXXXX", NULL);
	g_autofree gchar *good = g_build_filename(dir, "good.yaml", NULL);
	g_autofree gchar *bad = g_build_filename(dir, "bad.yaml", NULL);
	g_autoptr(GowlConfig) config = gowl_config_new();
	g_autoptr(GowlConfig) other = gowl_config_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(g_file_set_contents(good,
		"hotkey:\n  hotkeys: [{key: a, do: {notify: x}}]\n", -1, NULL));
	g_assert_true(g_file_set_contents(bad, "hotkey: [1, 2]\n", -1, NULL));
	g_assert_true(gowl_config_load_yaml(config, good, NULL));
	g_assert_cmpuint(gowl_config_get_hotkey_nodes(config)->len, ==, 1);
	g_assert_cmpstr(gowl_config_get_hotkey_base_dir(config), ==, dir);

	expected_warning = "`hotkey:' is a mapping";
	saw_expected_warning = 0;
	g_assert_true(gowl_config_load_yaml(other, bad, NULL));
	expected_warning = NULL;
	g_assert_cmpuint(saw_expected_warning, ==, 1);
	g_assert_cmpuint(gowl_config_get_problem_count(other), >=, 1);
	g_assert_cmpuint(gowl_config_get_hotkey_nodes(other)->len, ==, 0);

	/* the C config's form */
	g_assert_true(gowl_config_add_hotkey_yaml(other,
		"remaps: {CapsLock: Control_L}\n", &error));
	g_assert_no_error(error);
	g_assert_cmpuint(gowl_config_get_hotkey_nodes(other)->len, ==, 1);
	g_assert_false(gowl_config_add_hotkey_yaml(other, "- x\n", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_false(gowl_config_add_hotkey_yaml(other, "a: [", &error));
	g_assert_nonnull(error);
	gowl_config_clear_hotkey(other);
	g_assert_cmpuint(gowl_config_get_hotkey_nodes(other)->len, ==, 0);
	g_unlink(good);
	g_unlink(bad);
	g_rmdir(dir);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);
	g_log_set_writer_func(expect_warning_writer, NULL, NULL);

	g_test_add_func("/hotkey/config/full-example", test_full_example);
	g_test_add_func("/hotkey/config/cookbook-cases", test_cookbook_cases);
	g_test_add_func("/hotkey/config/c-config-example", test_c_config_example);
	g_test_add_func("/hotkey/config/script-example", test_script_example);
	g_test_add_func("/hotkey/config/section", test_config_section);

	return g_test_run();
}
