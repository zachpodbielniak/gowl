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
 * test-broadcast-config.c - The `broadcast:' config section.
 *
 * YAML in (with a bad entry reported and skipped, the rest kept), YAML
 * out (gowl_config_generate_yaml() reads back to the same set), the C
 * API (replace by name), and both shipped examples: the YAML one loads
 * with no problems, and the C one -- compiled into this test -- builds
 * the same team.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "gowl.h"

/* The C example's extern, which a real C config gets from gowl */
GowlConfig *gowl_config = NULL;
#include "../data/example-broadcast.c"

/*
 * GTest makes every warning fatal, and a config with a bad entry warns
 * on purpose (that IS the report).  This writer swallows the warnings a
 * test names and counts them.
 */
static const gchar *expected_warning = NULL;
static guint        saw_expected_warning = 0;

static GLogWriterOutput
expect_warning_writer(
	GLogLevelFlags   level,
	const GLogField *fields,
	gsize            n_fields,
	gpointer         user_data
){
	gsize i;

	if (expected_warning != NULL && (level & G_LOG_LEVEL_WARNING)) {
		for (i = 0; i < n_fields; i++) {
			if (g_strcmp0(fields[i].key, "MESSAGE") == 0
			    && strstr((const gchar *)fields[i].value,
			              expected_warning) != NULL) {
				saw_expected_warning++;
				return G_LOG_WRITER_HANDLED;
			}
		}
	}
	return g_log_writer_default(level, fields, n_fields, user_data);
}

/* @yaml in a temporary file, loaded into a new config */
static GowlConfig *
load(const gchar *yaml)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	GowlConfig *config;
	gint fd;

	fd = g_file_open_tmp("gowl-broadcast-XXXXXX.yaml", &path, &error);
	g_assert_no_error(error);
	close(fd);
	g_assert_true(g_file_set_contents(path, yaml, -1, &error));
	config = gowl_config_new();
	g_assert_true(gowl_config_load_yaml(config, path, &error));
	g_assert_no_error(error);
	g_unlink(path);
	return config;
}

static void
test_section(void)
{
	g_autoptr(GowlConfig) config = NULL;
	GPtrArray *teams;
	GPtrArray *rules;

	expected_warning = "broadcast rule 2";
	saw_expected_warning = 0;
	config = load(
		"broadcast:\n"
		"  teams:\n"
		"    - name: eq\n"
		"      slots:\n"
		"        - {name: tank, match: 'app-id = eqgame.exe'}\n"
		"        - {name: cleric, match: 'app-id = eqgame.exe'}\n"
		"  rules:\n"
		"    - {name: follow, team: eq, keys: ['1-9']}\n"
		"    - {name: broken, team: eq, keys: [NoSuchKey]}\n"
		"    - {name: heal, team: eq, keys: [h], targets: slot:cleric,"
		" local: block}\n");
	expected_warning = NULL;
	g_assert_cmpuint(saw_expected_warning, ==, 1);
	g_assert_cmpuint(gowl_config_get_problem_count(config), >=, 1);

	teams = gowl_config_get_broadcast_teams(config);
	rules = gowl_config_get_broadcast_rules(config);
	g_assert_cmpuint(teams->len, ==, 1);
	g_assert_cmpuint(rules->len, ==, 2);
	g_assert_cmpstr(gowl_broadcast_rule_get_name(
		g_ptr_array_index(rules, 1)), ==, "heal");
}

static void
test_unknown_keys_warn(void)
{
	g_autoptr(GowlConfig) config = NULL;

	expected_warning = "broadcast: unknown key";
	saw_expected_warning = 0;
	config = load("broadcast:\n  teamz: []\n");
	expected_warning = NULL;
	g_assert_cmpuint(saw_expected_warning, ==, 1);
	g_assert_cmpuint(gowl_config_get_problem_count(config), >=, 1);
}

static void
test_roundtrip(void)
{
	g_autoptr(GowlConfig) config = NULL;
	g_autoptr(GowlConfig) again = NULL;
	g_autofree gchar *yaml = NULL;
	GPtrArray *a;
	GPtrArray *b;
	guint i;

	config = load(
		"broadcast:\n"
		"  teams:\n"
		"    - name: eq\n"
		"      slots:\n"
		"        - {name: tank, match: 'app-id = eqgame.exe',"
		" focus-key: Super+F1}\n"
		"        - {name: cleric, match: 'app-id = eqgame.exe',"
		" delivery: activate}\n"
		"      groups: {casters: [cleric]}\n"
		"  rules:\n"
		"    - {name: nuke, team: eq, keys: [n], targets: group:casters,"
		" translate: {'*': {n: '4'}}, mode: round-robin, reset-ms: 500,"
		" mouse: scaled, delay-ms: 10, local: block,"
		" when: 'focused-slot = tank'}\n");
	yaml = gowl_config_generate_yaml(config);
	g_assert_nonnull(strstr(yaml, "\nbroadcast:\n"));
	again = load(yaml);
	g_assert_cmpuint(gowl_config_get_problem_count(again), ==, 0);

	a = gowl_config_get_broadcast_teams(config);
	b = gowl_config_get_broadcast_teams(again);
	g_assert_cmpuint(a->len, ==, b->len);
	for (i = 0; i < a->len; i++) {
		g_autofree gchar *ya = gowl_broadcast_team_to_yaml(
			g_ptr_array_index(a, i));
		g_autofree gchar *yb = gowl_broadcast_team_to_yaml(
			g_ptr_array_index(b, i));

		g_assert_cmpstr(ya, ==, yb);
	}
	a = gowl_config_get_broadcast_rules(config);
	b = gowl_config_get_broadcast_rules(again);
	g_assert_cmpuint(a->len, ==, b->len);
	for (i = 0; i < a->len; i++) {
		g_autofree gchar *ya = gowl_broadcast_rule_to_yaml(
			g_ptr_array_index(a, i));
		g_autofree gchar *yb = gowl_broadcast_rule_to_yaml(
			g_ptr_array_index(b, i));

		g_assert_cmpstr(ya, ==, yb);
	}
}

static void
test_c_api(void)
{
	g_autoptr(GowlConfig) config = gowl_config_new();
	g_autoptr(GowlBroadcastRule) first = gowl_broadcast_rule_new("r");
	g_autoptr(GowlBroadcastRule) second = gowl_broadcast_rule_new("r");
	g_autoptr(GowlBroadcastTeam) team = gowl_broadcast_team_new("t");
	GPtrArray *rules;

	gowl_config_add_broadcast_rule(config, first);
	gowl_config_add_broadcast_rule(config, second);
	rules = gowl_config_get_broadcast_rules(config);
	/* replaced in place, not stacked */
	g_assert_cmpuint(rules->len, ==, 1);
	g_assert_true(g_ptr_array_index(rules, 0) == second);

	gowl_config_add_broadcast_team(config, team);
	g_assert_true(gowl_config_remove_broadcast_rule(config, "r"));
	g_assert_false(gowl_config_remove_broadcast_rule(config, "r"));
	g_assert_true(gowl_config_remove_broadcast_team(config, "t"));
	gowl_config_add_broadcast_team(config, team);
	gowl_config_clear_broadcast(config);
	g_assert_cmpuint(gowl_config_get_broadcast_teams(config)->len, ==, 0);
}

static void
test_shipped_yaml(void)
{
	g_autoptr(GowlConfig) config = gowl_config_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	GowlBroadcastTeam *team;

	path = g_build_filename(GOWL_DEV_DATADIR, "example-broadcast.yaml",
	                        NULL);
	g_assert_true(gowl_config_load_yaml(config, path, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(gowl_config_get_problem_count(config), ==, 0);
	g_assert_cmpuint(gowl_config_get_broadcast_teams(config)->len, ==, 1);
	g_assert_cmpuint(gowl_config_get_broadcast_rules(config)->len, ==, 5);
	team = g_ptr_array_index(gowl_config_get_broadcast_teams(config), 0);
	g_assert_cmpuint(gowl_broadcast_team_get_n_slots(team), ==, 3);
	g_assert_nonnull(gowl_broadcast_team_get_group(team, "casters"));
}

static void
test_shipped_c(void)
{
	GPtrArray *rules;

	gowl_config = gowl_config_new();
	g_assert_true(gowl_config_init());
	g_assert_cmpuint(gowl_config_get_broadcast_teams(gowl_config)->len,
	                 ==, 1);
	rules = gowl_config_get_broadcast_rules(gowl_config);
	g_assert_cmpuint(rules->len, ==, 4);
	g_assert_cmpstr(gowl_broadcast_rule_get_when(
		g_ptr_array_index(rules, 0)), ==, "focused-slot = tank");
	g_clear_object(&gowl_config);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);
	g_log_set_writer_func(expect_warning_writer, NULL, NULL);

	g_test_add_func("/broadcast/config/section", test_section);
	g_test_add_func("/broadcast/config/unknown-keys-warn",
	                test_unknown_keys_warn);
	g_test_add_func("/broadcast/config/roundtrip", test_roundtrip);
	g_test_add_func("/broadcast/config/c-api", test_c_api);
	g_test_add_func("/broadcast/config/shipped-yaml", test_shipped_yaml);
	g_test_add_func("/broadcast/config/shipped-c", test_shipped_c);

	return g_test_run();
}
