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

#include "config/gowl-config.h"
#include "gowl-enums.h"
#include <glib/gstdio.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static void
test_config_new(void)
{
	GowlConfig *config;

	config = gowl_config_new();
	g_assert_nonnull(config);
	g_assert_true(GOWL_IS_CONFIG(config));
	g_object_unref(config);
}

static void
test_config_defaults(void)
{
	GowlConfig *config;

	config = gowl_config_new();

	g_assert_cmpint(gowl_config_get_border_width(config), ==, 2);
	g_assert_cmpfloat_with_epsilon(gowl_config_get_mfact(config), 0.55, 0.001);
	g_assert_cmpint(gowl_config_get_nmaster(config), ==, 1);
	g_assert_cmpint(gowl_config_get_tag_count(config), ==, 9);
	g_assert_cmpint(gowl_config_get_repeat_rate(config), ==, 25);
	g_assert_cmpint(gowl_config_get_repeat_delay(config), ==, 600);
	g_assert_true(gowl_config_get_sloppyfocus(config));
	g_assert_true(gowl_config_get_manage_lid(config));
	g_assert_cmpstr(gowl_config_get_terminal(config), ==, "gst");
	g_assert_cmpstr(gowl_config_get_menu(config), ==, "bemenu-run");

	g_object_unref(config);
}

static void
test_config_manage_lid(void)
{
	GowlConfig *config;
	gboolean    val;
	gchar      *yaml;

	config = gowl_config_new();

	/* Default on, readable via the getter and the property. */
	g_assert_true(gowl_config_get_manage_lid(config));
	g_object_get(G_OBJECT(config), "manage-lid", &val, NULL);
	g_assert_true(val);

	/* Round-trips through the property. */
	g_object_set(G_OBJECT(config), "manage-lid", FALSE, NULL);
	g_assert_false(gowl_config_get_manage_lid(config));
	g_object_set(G_OBJECT(config), "manage-lid", TRUE, NULL);
	g_assert_true(gowl_config_get_manage_lid(config));

	/* Serialized into the generated YAML. */
	yaml = gowl_config_generate_yaml(config);
	g_assert_nonnull(yaml);
	g_assert_true(strstr(yaml, "manage_lid") != NULL);
	g_free(yaml);

	g_object_unref(config);
}

static void
test_config_set_properties(void)
{
	GowlConfig *config;

	config = gowl_config_new();

	g_object_set(G_OBJECT(config),
		"border-width", 5,
		"mfact", 0.65,
		"nmaster", 2,
		"tag-count", 6,
		"terminal", "kitty",
		NULL);

	g_assert_cmpint(gowl_config_get_border_width(config), ==, 5);
	g_assert_cmpfloat_with_epsilon(gowl_config_get_mfact(config), 0.65, 0.001);
	g_assert_cmpint(gowl_config_get_nmaster(config), ==, 2);
	g_assert_cmpint(gowl_config_get_tag_count(config), ==, 6);
	g_assert_cmpstr(gowl_config_get_terminal(config), ==, "kitty");

	g_object_unref(config);
}

static void
test_config_generate_yaml(void)
{
	GowlConfig *config;
	gchar *yaml;

	config = gowl_config_new();
	yaml = gowl_config_generate_yaml(config);

	g_assert_nonnull(yaml);
	/* Check that some expected keys are present */
	g_assert_true(strstr(yaml, "border-width") != NULL);
	g_assert_true(strstr(yaml, "mfact") != NULL);
	g_assert_true(strstr(yaml, "terminal") != NULL);

	g_free(yaml);
	g_object_unref(config);
}

static void
test_config_add_rule(void)
{
	GowlConfig *config;

	config = gowl_config_new();

	gowl_config_add_rule(config, "firefox", NULL, 1 << 1, FALSE, -1);
	gowl_config_add_rule(config, NULL, "*popup*", 0, TRUE, -1);

	/* Rules are stored internally - we just verify no crash */
	g_object_unref(config);
}

static void
test_config_type(void)
{
	GType type;

	type = GOWL_TYPE_CONFIG;
	g_assert_true(type != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_OBJECT(type));
}

/* --- monitors: YAML block --- */

/* Load YAML from an inline string by writing it to a temp file
 * and calling gowl_config_load_yaml.  Returns TRUE on success. */
static gboolean
load_yaml_from_string(GowlConfig  *config,
                       const gchar *yaml,
                       GError     **error)
{
	g_autofree gchar *tmpdir = NULL;
	g_autofree gchar *path   = NULL;

	tmpdir = g_dir_make_tmp("gowl-test-XXXXXX", error);
	if (tmpdir == NULL)
		return FALSE;

	path = g_build_filename(tmpdir, "config.yaml", NULL);
	if (!g_file_set_contents(path, yaml, -1, error)) {
		g_unlink(path);
		g_rmdir(tmpdir);
		return FALSE;
	}

	if (!gowl_config_load_yaml(config, path, error)) {
		g_unlink(path);
		g_rmdir(tmpdir);
		return FALSE;
	}

	g_unlink(path);
	g_rmdir(tmpdir);
	return TRUE;
}

static void
test_config_monitors_full(void)
{
	GowlConfig *config;
	const GowlMonitorConfig *mc;
	GError *err = NULL;
	gboolean ok;
	const gchar *yaml =
		"monitors:\n"
		"  eDP-1:\n"
		"    width: 1920\n"
		"    height: 1080\n"
		"    refresh: 60.0\n"
		"    x: 0\n"
		"    y: 0\n"
		"    scale: 1.5\n"
		"    enabled: true\n"
		"    transform: 90\n";

	config = gowl_config_new();
	ok = load_yaml_from_string(config, yaml, &err);
	g_assert_no_error(err);
	g_assert_true(ok);

	mc = gowl_config_get_monitor_config(config, "eDP-1");
	g_assert_nonnull(mc);
	g_assert_cmpint(mc->width, ==, 1920);
	g_assert_cmpint(mc->height, ==, 1080);
	g_assert_cmpfloat_with_epsilon(mc->refresh, 60.0, 0.001);
	g_assert_cmpint(mc->x, ==, 0);
	g_assert_cmpint(mc->y, ==, 0);
	g_assert_cmpfloat_with_epsilon(mc->scale, 1.5, 0.001);
	g_assert_cmpint(mc->enabled, ==, 1);
	g_assert_cmpint(mc->transform, ==, 1);

	g_assert_null(gowl_config_get_monitor_config(config, "HDMI-A-1"));

	g_object_unref(config);
}

static void
test_config_monitors_partial(void)
{
	GowlConfig *config;
	const GowlMonitorConfig *mc;
	GError *err = NULL;
	gboolean ok;
	const gchar *yaml =
		"monitors:\n"
		"  eDP-1:\n"
		"    transform: 90\n";

	config = gowl_config_new();
	ok = load_yaml_from_string(config, yaml, &err);
	g_assert_no_error(err);
	g_assert_true(ok);

	mc = gowl_config_get_monitor_config(config, "eDP-1");
	g_assert_nonnull(mc);
	g_assert_cmpint(mc->transform, ==, 1);

	/* All other fields stay at the "unset" sentinel */
	g_assert_cmpint(mc->width, ==, 0);
	g_assert_cmpint(mc->height, ==, 0);
	g_assert_cmpfloat(mc->refresh, ==, 0.0);
	g_assert_cmpint(mc->x, ==, G_MININT);
	g_assert_cmpint(mc->y, ==, G_MININT);
	g_assert_cmpfloat(mc->scale, ==, 0.0);
	g_assert_cmpint(mc->enabled, ==, -1);

	g_object_unref(config);
}

static void
test_config_monitors_transform_string(void)
{
	GowlConfig *config;
	const GowlMonitorConfig *mc;
	GError *err = NULL;
	gboolean ok;
	const gchar *yaml =
		"monitors:\n"
		"  eDP-1:\n"
		"    transform: flipped-180\n"
		"  HDMI-A-1:\n"
		"    transform: normal\n"
		"  DP-2:\n"
		"    transform: 270\n";

	config = gowl_config_new();
	ok = load_yaml_from_string(config, yaml, &err);
	g_assert_no_error(err);
	g_assert_true(ok);

	mc = gowl_config_get_monitor_config(config, "eDP-1");
	g_assert_nonnull(mc);
	g_assert_cmpint(mc->transform, ==, 6);

	mc = gowl_config_get_monitor_config(config, "HDMI-A-1");
	g_assert_nonnull(mc);
	g_assert_cmpint(mc->transform, ==, 0);

	/* "270" is the canonical *name* for transform 3, not the
	 * angle.  Our parser tries int first, so this string-parses
	 * as 270 (out of range) and falls back to the name table. */
	mc = gowl_config_get_monitor_config(config, "DP-2");
	g_assert_nonnull(mc);
	g_assert_cmpint(mc->transform, ==, 3);

	g_object_unref(config);
}

/* Custom writer that drops the parser's expected warning to the
 * floor.  With G_LOG_USE_STRUCTURED set (gowl's default), the test
 * framework's expect_message/fatal-handler hooks are bypassed --
 * structured logs route through the writer instead.  Returning
 * G_LOG_WRITER_HANDLED prevents both the default print and the
 * implicit abort that GTest installs on G_LOG_LEVEL_WARNING. */
/*
 * The build defines G_LOG_USE_STRUCTURED, so warnings go through the
 * writer rather than the legacy handler -- which means
 * g_test_expect_message() never sees them, and GTest's default abort on
 * G_LOG_LEVEL_WARNING fires instead.  Swallowing the one warning a test
 * expects is the only interception point.
 */
static const char *expected_warning = NULL;
static gboolean saw_expected_warning = FALSE;

static GLogWriterOutput expect_warning_writer(GLogLevelFlags, const GLogField *,
                                               gsize, gpointer);

/*
 * g_log_set_writer_func() may be called only ONCE per process, so the
 * writer is installed on first use and left in place.  It is
 * transparent while expected_warning is NULL, falling through to the
 * default, so a test that wants no interception simply clears it.
 */
static void
expect_warning(const char *substring)
{
	static gboolean installed = FALSE;

	if (!installed) {
		g_log_set_writer_func(expect_warning_writer, NULL, NULL);
		installed = TRUE;
	}
	expected_warning = substring;
	saw_expected_warning = FALSE;
}

static GLogWriterOutput
expect_warning_writer(GLogLevelFlags    log_level,
                       const GLogField  *fields,
                       gsize             n_fields,
                       gpointer          user_data)
{
	gsize i;
	(void)user_data;

	if (log_level & G_LOG_LEVEL_WARNING) {
		for (i = 0; i < n_fields; i++) {
			if (g_strcmp0(fields[i].key, "MESSAGE") == 0
			    && fields[i].value != NULL
			    && expected_warning != NULL
			    && strstr((const char *)fields[i].value,
			              expected_warning) != NULL) {
				saw_expected_warning = TRUE;
				return G_LOG_WRITER_HANDLED;
			}
		}
	}
	return g_log_writer_default(log_level, fields, n_fields, NULL);
}

static void
test_config_monitors_transform_invalid(void)
{
	GowlConfig *config;
	const GowlMonitorConfig *mc;
	GError *err = NULL;
	gboolean ok;
	const gchar *yaml =
		"monitors:\n"
		"  eDP-1:\n"
		"    transform: ninety\n";

	config = gowl_config_new();

	/* Install a structured-log writer that swallows the expected
	 * "invalid transform" warning, then restore the default after.
	 * g_log_set_writer_func can be called multiple times in tests
	 * by passing NULL to revert. */
	expect_warning("invalid transform");

	ok = load_yaml_from_string(config, yaml, &err);
	g_assert_no_error(err);
	g_assert_true(ok);

	mc = gowl_config_get_monitor_config(config, "eDP-1");
	g_assert_nonnull(mc);
	g_assert_cmpint(mc->transform, ==, -1);

	g_object_unref(config);
}

/* --- Keybind descriptions and YAML round-trip --- */

/* Finds a keybind by its 1-based position, or NULL. */
static GowlKeybindEntry *
nth_keybind(GowlConfig *config, guint n)
{
	GArray *binds = gowl_config_get_keybinds(config);

	if (binds == NULL || binds->len <= n)
		return NULL;
	return &g_array_index(binds, GowlKeybindEntry, n);
}

/* A "desc" on a keybind is parsed and stored; its absence is NULL
 * rather than an empty string, so a caller can tell "no description"
 * from "described as nothing". */
static void
test_config_keybind_desc_from_yaml(void)
{
	GowlConfig *config;
	GowlKeybindEntry *kb;
	GError *err = NULL;
	const gchar *yaml =
		"keybinds:\n"
		"  \"Super+Return\":"
		" { action: spawn, arg: \"gst\", desc: \"Terminal\" }\n"
		"  \"Super+Shift+q\": { action: quit }\n";

	config = gowl_config_new();
	g_assert_true(load_yaml_from_string(config, yaml, &err));
	g_assert_no_error(err);

	g_assert_cmpuint(gowl_config_get_keybinds(config)->len, ==, 2);

	kb = nth_keybind(config, 0);
	g_assert_nonnull(kb);
	g_assert_cmpstr(kb->arg, ==, "gst");
	g_assert_cmpstr(kb->desc, ==, "Terminal");

	kb = nth_keybind(config, 1);
	g_assert_nonnull(kb);
	g_assert_null(kb->desc);

	g_object_unref(config);
}

/* The XF86 media keysyms resolve and bind with no modifier.  This is
 * what makes the shipped media-key binds work: dispatch compares a
 * cleaned modifier mask, and 0 == 0 matches like any other combo. */
static void
test_config_keybind_media_keys(void)
{
	GowlConfig *config;
	GowlKeybindEntry *kb;
	GError *err = NULL;
	const gchar *yaml =
		"keybinds:\n"
		"  \"XF86AudioRaiseVolume\":"
		" { action: spawn, arg: \"vol up\", desc: \"Volume up\" }\n";

	config = gowl_config_new();
	g_assert_true(load_yaml_from_string(config, yaml, &err));
	g_assert_no_error(err);

	kb = nth_keybind(config, 0);
	g_assert_nonnull(kb);
	g_assert_cmpuint(kb->modifiers, ==, 0);
	g_assert_cmpuint(kb->keysym, !=, 0);
	g_assert_cmpstr(kb->desc, ==, "Volume up");

	g_object_unref(config);
}

/* Generated YAML must be loadable again.  The generator used to emit a
 * sequence of "- bind:" items while the parser asked for a mapping, so
 * every keybind vanished on reload -- silently, because a missing
 * mapping member is not an error. */
static void
test_config_keybind_yaml_round_trip(void)
{
	GowlConfig *config;
	GowlConfig *reloaded;
	GowlKeybindEntry *kb;
	GError *err = NULL;
	g_autofree gchar *yaml = NULL;

	config = gowl_config_new();
	gowl_config_add_keybind_full(config, GOWL_KEY_MOD_LOGO,
	                              XKB_KEY_Return, GOWL_ACTION_SPAWN,
	                              "gst", "Terminal");
	gowl_config_add_keybind(config, GOWL_KEY_MOD_LOGO,
	                         XKB_KEY_q, GOWL_ACTION_QUIT, NULL);

	yaml = gowl_config_generate_yaml(config);
	g_assert_nonnull(yaml);

	reloaded = gowl_config_new();
	g_assert_true(load_yaml_from_string(reloaded, yaml, &err));
	g_assert_no_error(err);

	g_assert_cmpuint(gowl_config_get_keybinds(reloaded)->len, ==, 2);

	kb = nth_keybind(reloaded, 0);
	g_assert_nonnull(kb);
	g_assert_cmpint(kb->action, ==, GOWL_ACTION_SPAWN);
	g_assert_cmpstr(kb->arg, ==, "gst");
	g_assert_cmpstr(kb->desc, ==, "Terminal");

	kb = nth_keybind(reloaded, 1);
	g_assert_nonnull(kb);
	g_assert_cmpint(kb->action, ==, GOWL_ACTION_QUIT);
	g_assert_null(kb->desc);

	g_object_unref(reloaded);
	g_object_unref(config);
}

/* A quote or backslash in an arg has to survive the emitted
 * double-quoted scalar.  Unescaped, the closing quote lands early and
 * the document either fails to parse or parses into the wrong values. */
static void
test_config_keybind_yaml_escapes(void)
{
	GowlConfig *config;
	GowlConfig *reloaded;
	GowlKeybindEntry *kb;
	GError *err = NULL;
	g_autofree gchar *yaml = NULL;
	const gchar *tricky = "sh -c \"echo a\\b\"";

	config = gowl_config_new();
	gowl_config_add_keybind_full(config, GOWL_KEY_MOD_LOGO,
	                              XKB_KEY_x, GOWL_ACTION_SPAWN,
	                              tricky, "Quote \"test\"");

	yaml = gowl_config_generate_yaml(config);
	reloaded = gowl_config_new();
	g_assert_true(load_yaml_from_string(reloaded, yaml, &err));
	g_assert_no_error(err);

	kb = nth_keybind(reloaded, 0);
	g_assert_nonnull(kb);
	g_assert_cmpstr(kb->arg, ==, tricky);
	g_assert_cmpstr(kb->desc, ==, "Quote \"test\"");

	g_object_unref(reloaded);
	g_object_unref(config);
}

/* The "custom" action parses from YAML like any other nick.  Standalone
 * gowl has no handler for it, but the config must still carry it so an
 * embedder can act on the bind. */
static void
test_config_keybind_custom_action(void)
{
	GowlConfig *config;
	GowlKeybindEntry *kb;
	GError *err = NULL;
	const gchar *yaml =
		"keybinds:\n"
		"  \"XF86AudioMute\":"
		" { action: custom, arg: \"(cmacs-volume-mute)\" }\n";

	config = gowl_config_new();
	g_assert_true(load_yaml_from_string(config, yaml, &err));
	g_assert_no_error(err);

	kb = nth_keybind(config, 0);
	g_assert_nonnull(kb);
	g_assert_cmpint(kb->action, ==, GOWL_ACTION_CUSTOM);
	g_assert_cmpstr(kb->arg, ==, "(cmacs-volume-mute)");

	g_object_unref(config);
}

/* --- rules.d --- */

/* Write FILES (name, content pairs) into a fresh temp dir and return it. */
static gchar *
make_rules_tree(const gchar *main_yaml, const gchar **files)
{
	gchar *dir = g_dir_make_tmp("gowl-rulesd-XXXXXX", NULL);
	g_autofree gchar *rd = g_build_filename(dir, "rules.d", NULL);
	g_autofree gchar *main_path = g_build_filename(dir, "config.yaml", NULL);
	gint i;

	g_mkdir_with_parents(rd, 0700);
	g_file_set_contents(main_path, main_yaml, -1, NULL);

	for (i = 0; files != NULL && files[i] != NULL; i += 2) {
		g_autofree gchar *p = g_build_filename(rd, files[i], NULL);

		g_file_set_contents(p, files[i + 1], -1, NULL);
	}
	return dir;
}

static void
remove_rules_tree(gchar *dir)
{
	g_autofree gchar *rd = g_build_filename(dir, "rules.d", NULL);
	g_autoptr(GDir) d = g_dir_open(rd, 0, NULL);
	const gchar *name;

	if (d != NULL) {
		while ((name = g_dir_read_name(d)) != NULL) {
			g_autofree gchar *p = g_build_filename(rd, name, NULL);
			g_unlink(p);
		}
	}
	{
		g_autofree gchar *m = g_build_filename(dir, "config.yaml", NULL);
		g_unlink(m);
	}
	g_rmdir(rd);
	g_rmdir(dir);
	g_free(dir);
}

/* Fragments add to the main config's rules rather than replacing them. */
static void
test_config_rules_d_merges(void)
{
	const gchar *files[] = {
		"10-steam.yaml", "rules:\n  - app-id: \"steam\"\n    floating: true\n",
		"20-pip.yaml",   "rules:\n  - title: \"Picture-in-Picture\"\n    floating: true\n",
		NULL
	};
	gchar *dir = make_rules_tree(
		"border-width: 3\nrules:\n  - app-id: \"firefox\"\n    tags: 2\n",
		files);
	g_autofree gchar *main_path = g_build_filename(dir, "config.yaml", NULL);
	GowlConfig *config = gowl_config_new();
	GError *err = NULL;

	g_assert_true(gowl_config_load_yaml(config, main_path, &err));
	g_assert_no_error(err);
	g_assert_cmpuint(gowl_config_get_rules(config)->len, ==, 1);

	g_assert_cmpuint(gowl_config_load_rules_d(config, main_path), ==, 2);
	g_assert_cmpuint(gowl_config_get_rules(config)->len, ==, 3);

	/* A fragment carries only rules; it must not reset anything else
	 * back to a default just by being a valid config file. */
	g_assert_cmpint(gowl_config_get_border_width(config), ==, 3);

	g_object_unref(config);
	remove_rules_tree(dir);
}

/*
 * A fragment that does not parse costs itself and nothing else.  These
 * are per-application files a user edits by hand; one typo must not
 * take out the rules that were fine, nor stop the compositor starting.
 */
static void
test_config_rules_d_survives_a_bad_file(void)
{
	const gchar *files[] = {
		"10-good.yaml",   "rules:\n  - app-id: \"steam\"\n    floating: true\n",
		"99-broken.yaml", "rules: [[[ not valid\n",
		NULL
	};
	gchar *dir = make_rules_tree("rules: []\n", files);
	g_autofree gchar *main_path = g_build_filename(dir, "config.yaml", NULL);
	GowlConfig *config = gowl_config_new();

	g_assert_true(gowl_config_load_yaml(config, main_path, NULL));

	/* Only the good one counts, and it did load.  The warning is
	 * swallowed by a writer rather than g_test_expect_message: this
	 * build logs structurally, which that never sees. */
	expect_warning("99-broken");

	g_assert_cmpuint(gowl_config_load_rules_d(config, main_path), ==, 1);

	g_assert_true(saw_expected_warning);
	expected_warning = NULL;
	g_assert_cmpuint(gowl_config_get_rules(config)->len, ==, 1);

	g_object_unref(config);
	remove_rules_tree(dir);
}

/* No rules.d is the normal case and is not an error. */
static void
test_config_rules_d_absent(void)
{
	gchar *dir = make_rules_tree("rules: []\n", NULL);
	g_autofree gchar *main_path = g_build_filename(dir, "config.yaml", NULL);
	GowlConfig *config = gowl_config_new();

	g_assert_true(gowl_config_load_yaml(config, main_path, NULL));
	g_assert_cmpuint(gowl_config_load_rules_d(config, main_path), ==, 0);

	g_object_unref(config);
	remove_rules_tree(dir);
}

static void
test_config_monitors_names_iter(void)
{
	GowlConfig *config;
	GList *names;
	GError *err = NULL;
	const gchar *yaml =
		"monitors:\n"
		"  eDP-1:\n"
		"    transform: 1\n"
		"  HDMI-A-1:\n"
		"    scale: 1.5\n";

	config = gowl_config_new();
	g_assert_true(load_yaml_from_string(config, yaml, &err));
	g_assert_no_error(err);

	names = gowl_config_get_monitor_names(config);
	g_assert_cmpuint(g_list_length(names), ==, 2);
	g_list_free(names);

	g_object_unref(config);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/config/new", test_config_new);
	g_test_add_func("/config/defaults", test_config_defaults);
	g_test_add_func("/config/manage-lid", test_config_manage_lid);
	g_test_add_func("/config/set-properties", test_config_set_properties);
	g_test_add_func("/config/generate-yaml", test_config_generate_yaml);
	g_test_add_func("/config/add-rule", test_config_add_rule);
	g_test_add_func("/config/type", test_config_type);
	g_test_add_func("/config/monitors-full",
	                test_config_monitors_full);
	g_test_add_func("/config/monitors-partial",
	                test_config_monitors_partial);
	g_test_add_func("/config/monitors-transform-string",
	                test_config_monitors_transform_string);
	g_test_add_func("/config/monitors-transform-invalid",
	                test_config_monitors_transform_invalid);
	g_test_add_func("/config/keybind-desc-from-yaml",
	                test_config_keybind_desc_from_yaml);
	g_test_add_func("/config/keybind-media-keys",
	                test_config_keybind_media_keys);
	g_test_add_func("/config/keybind-yaml-round-trip",
	                test_config_keybind_yaml_round_trip);
	g_test_add_func("/config/keybind-yaml-escapes",
	                test_config_keybind_yaml_escapes);
	g_test_add_func("/config/keybind-custom-action",
	                test_config_keybind_custom_action);
	g_test_add_func("/config/rules-d-merges", test_config_rules_d_merges);
	g_test_add_func("/config/rules-d-bad-file",
	                test_config_rules_d_survives_a_bad_file);
	g_test_add_func("/config/rules-d-absent", test_config_rules_d_absent);
	g_test_add_func("/config/monitors-names-iter",
	                test_config_monitors_names_iter);

	return g_test_run();
}
