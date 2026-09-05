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
 * test-palette.c -- the shared colour source
 *
 * Every assertion here is about resolution rather than about which hex
 * value a flavour holds, with one exception: the flavours are checked
 * to define the same key set, because a config written against one and
 * rendered under another is the whole point and a missing key fails
 * silently --- the name resolves to itself and the consumer paints an
 * unparseable colour, which most consumers treat as black.
 */

#include <glib.h>

#include "boxed/gowl-palette.h"
#include "config/gowl-config.h"

/* ── Resolution ──────────────────────────────────────────────────── */

static void
test_resolve_literal(void)
{
	g_autoptr(GowlPalette) p = gowl_palette_new_builtin("mocha");
	g_autofree gchar *out = NULL;

	out = gowl_palette_resolve(p, "#123456");
	g_assert_cmpstr(out, ==, "#123456");
}

static void
test_resolve_literal_beats_entry(void)
{
	g_autoptr(GowlPalette) p = gowl_palette_new();
	g_autofree gchar *out = NULL;

	/* An entry may legally be named like a literal.  The literal must
	 * still win, or a palette could shadow a hex value the user wrote
	 * out in full. */
	gowl_palette_set(p, "#123456", "#abcdef");
	out = gowl_palette_resolve(p, "#123456");
	g_assert_cmpstr(out, ==, "#123456");
}

static void
test_resolve_name(void)
{
	g_autoptr(GowlPalette) p = gowl_palette_new_builtin("mocha");
	g_autofree gchar *out = NULL;

	out = gowl_palette_resolve(p, "accent");
	g_assert_cmpstr(out, ==, "#89b4fa");
}

static void
test_resolve_unknown_name_is_itself(void)
{
	g_autoptr(GowlPalette) p = gowl_palette_new_builtin("mocha");
	g_autofree gchar *out = NULL;

	/* Not an error: far more likely a colour format the consumer
	 * understands and the palette does not. */
	out = gowl_palette_resolve(p, "rebeccapurple");
	g_assert_cmpstr(out, ==, "rebeccapurple");
}

static void
test_resolve_alpha_suffix(void)
{
	g_autoptr(GowlPalette) p = gowl_palette_new_builtin("mocha");
	g_autofree gchar *out = NULL;

	out = gowl_palette_resolve(p, "base/cc");
	g_assert_cmpstr(out, ==, "#1e1e2ecc");
}

static void
test_resolve_alpha_replaces_existing(void)
{
	g_autoptr(GowlPalette) p = gowl_palette_new();
	g_autofree gchar *out = NULL;

	/* An entry that already carries alpha gets it replaced, not
	 * appended --- appending would produce a ten-digit string that
	 * parses as nothing. */
	gowl_palette_set(p, "glass", "#11223344");
	out = gowl_palette_resolve(p, "glass/ff");
	g_assert_cmpstr(out, ==, "#112233ff");
}

static void
test_resolve_bad_alpha_is_visible(void)
{
	g_autoptr(GowlPalette) p = gowl_palette_new_builtin("mocha");
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;

	/* A malformed alpha comes back unchanged so it paints as something
	 * obviously wrong.  Silently dropping it would give a perfectly
	 * opaque colour, which is indistinguishable from a config that did
	 * not apply at all. */
	a = gowl_palette_resolve(p, "base/zz");
	g_assert_cmpstr(a, ==, "base/zz");

	b = gowl_palette_resolve(p, "base/c");
	g_assert_cmpstr(b, ==, "base/c");
}

static void
test_resolve_null(void)
{
	g_autoptr(GowlPalette) p = gowl_palette_new_builtin("mocha");

	g_assert_null(gowl_palette_resolve(p, NULL));
	/* A NULL palette resolves everything to itself rather than
	 * crashing: a consumer with no config still paints. */
	g_assert_cmpstr(gowl_palette_resolve(NULL, "accent"), ==, "accent");
}

/* ── Flavours ────────────────────────────────────────────────────── */

static void
test_builtins_share_a_key_set(void)
{
	const gchar * const *names = gowl_palette_builtin_names();
	g_autoptr(GowlPalette) ref = gowl_palette_new_builtin("mocha");
	g_auto(GStrv) ref_keys = gowl_palette_names(ref);
	gsize i;

	g_assert_nonnull(names);
	g_assert_nonnull(names[0]);

	for (i = 0; names[i] != NULL; i++) {
		g_autoptr(GowlPalette) p = gowl_palette_new_builtin(names[i]);
		g_auto(GStrv) keys = gowl_palette_names(p);

		g_assert_cmpuint(gowl_palette_size(p), ==,
		                 gowl_palette_size(ref));
		g_assert_true(g_strv_equal((const gchar * const *)keys,
		                           (const gchar * const *)ref_keys));
	}
}

static void
test_unknown_builtin_falls_back(void)
{
	g_autoptr(GowlPalette) p = gowl_palette_new_builtin("no-such-flavour");
	g_autofree gchar *out = NULL;

	out = gowl_palette_resolve(p, "accent");
	g_assert_cmpstr(out, ==, "#89b4fa");
}

static void
test_dwm_flavour_restores_old_colours(void)
{
	g_autoptr(GowlPalette) p = gowl_palette_new_builtin("dwm");
	g_autofree gchar *accent = NULL;
	g_autofree gchar *urgent = NULL;

	/* The point of this flavour is that it reproduces exactly what
	 * gowl looked like before palettes existed. */
	accent = gowl_palette_resolve(p, "accent");
	urgent = gowl_palette_resolve(p, "red");
	g_assert_cmpstr(accent, ==, "#005577");
	g_assert_cmpstr(urgent, ==, "#ff0000");
}

static void
test_merge_overrides(void)
{
	g_autoptr(GowlPalette) base = gowl_palette_new_builtin("mocha");
	g_autoptr(GowlPalette) over = gowl_palette_new();
	g_autofree gchar *accent = NULL;
	g_autofree gchar *text = NULL;

	gowl_palette_set(over, "accent", "#ff00ff");
	gowl_palette_merge(base, over);

	accent = gowl_palette_resolve(base, "accent");
	text = gowl_palette_resolve(base, "text");
	g_assert_cmpstr(accent, ==, "#ff00ff");
	/* Merging one key must not disturb the rest. */
	g_assert_cmpstr(text, ==, "#cdd6f4");
}

/* ── The key-name convention ─────────────────────────────────────── */

static void
test_key_is_color(void)
{
	/* Both spellings, and anywhere in the key --- module settings are
	 * an untyped map, so this rule is the only thing standing between
	 * a module and palette support. */
	g_assert_true(gowl_palette_key_is_color("bg-color"));
	g_assert_true(gowl_palette_key_is_color("color"));
	g_assert_true(gowl_palette_key_is_color("indicator-colour"));
	g_assert_true(gowl_palette_key_is_color("cpu-color"));

	g_assert_false(gowl_palette_key_is_color("font"));
	g_assert_false(gowl_palette_key_is_color("height"));
	g_assert_false(gowl_palette_key_is_color(NULL));
}

/* ── Through the config ──────────────────────────────────────────── */

static gchar *
write_config(const gchar *body)
{
	g_autofree gchar *dir = NULL;
	gchar *path;

	dir = g_dir_make_tmp("gowl-palette-XXXXXX", NULL);
	g_assert_nonnull(dir);

	path = g_build_filename(dir, "config.yaml", NULL);
	g_assert_true(g_file_set_contents(path, body, -1, NULL));
	return path;
}

static void
test_config_default_borders_follow_palette(void)
{
	g_autoptr(GowlConfig) cfg = gowl_config_new();

	/* Nothing configured at all: the borders must still be the
	 * palette's colours rather than a hardcoded default that no theme
	 * change can reach. */
	g_assert_cmpstr(gowl_config_get_border_color_focus(cfg), ==, "#89b4fa");
	g_assert_cmpstr(gowl_config_get_border_color_unfocus(cfg), ==, "#313244");
	g_assert_cmpstr(gowl_config_get_border_color_urgent(cfg), ==, "#f38ba8");
}

static void
test_config_palette_block(void)
{
	g_autoptr(GowlConfig) cfg = gowl_config_new();
	g_autofree gchar *path = NULL;

	path = write_config(
		"palette:\n"
		"  name: latte\n"
		"  accent: \"#ff8800\"\n"
		"border-color-focus: accent\n"
		"border-color-unfocus: surface\n");

	g_assert_true(gowl_config_load_yaml(cfg, path, NULL));

	/* The override wins over the flavour, and the flavour supplies
	 * everything it did not override. */
	g_assert_cmpstr(gowl_config_get_border_color_focus(cfg), ==, "#ff8800");
	g_assert_cmpstr(gowl_config_get_border_color_unfocus(cfg), ==, "#ccd0da");
}

static void
test_config_palette_block_order_does_not_matter(void)
{
	g_autoptr(GowlConfig) cfg = gowl_config_new();
	g_autofree gchar *path = NULL;

	/* The palette is written after the key that uses it.  A parser
	 * that applied the document in order would leave the border
	 * unresolved --- with no error, and a result that looks
	 * plausible. */
	path = write_config(
		"border-color-focus: accent\n"
		"palette:\n"
		"  accent: \"#010203\"\n");

	g_assert_true(gowl_config_load_yaml(cfg, path, NULL));
	g_assert_cmpstr(gowl_config_get_border_color_focus(cfg), ==, "#010203");
}

static void
test_config_module_colors_resolve(void)
{
	g_autoptr(GowlConfig) cfg = gowl_config_new();
	g_autofree gchar *path = NULL;
	GHashTable *settings;

	path = write_config(
		"palette:\n"
		"  name: mocha\n"
		"modules:\n"
		"  bar:\n"
		"    bg-color: base/cc\n"
		"    fg-color: text\n"
		"    font: \"monospace 10\"\n");

	g_assert_true(gowl_config_load_yaml(cfg, path, NULL));

	settings = gowl_config_get_module_config(cfg, "bar");
	g_assert_nonnull(settings);

	g_assert_cmpstr(g_hash_table_lookup(settings, "bg-color"), ==,
	                "#1e1e2ecc");
	g_assert_cmpstr(g_hash_table_lookup(settings, "fg-color"), ==,
	                "#cdd6f4");
	/* A setting that is not a colour is left exactly as written --- the
	 * rule keys on the name, so a font must survive it untouched. */
	g_assert_cmpstr(g_hash_table_lookup(settings, "font"), ==,
	                "monospace 10");
}

static void
test_config_runtime_override_survives_reload(void)
{
	g_autoptr(GowlConfig) cfg = gowl_config_new();
	g_autofree gchar *path = NULL;

	path = write_config(
		"palette:\n"
		"  accent: \"#010203\"\n"
		"border-color-focus: accent\n");

	g_assert_true(gowl_config_load_yaml(cfg, path, NULL));
	g_assert_cmpstr(gowl_config_get_border_color_focus(cfg), ==, "#010203");

	/* An editor theme pushes its accent in... */
	gowl_config_set_palette_color(cfg, "accent", "#aabbcc");
	g_assert_cmpstr(gowl_config_get_border_color_focus(cfg), ==, "#aabbcc");

	/* ...and a later reload must not silently undo it.  This is the
	 * whole reason overrides are a separate layer rather than just
	 * entries written into the palette. */
	g_assert_true(gowl_config_load_yaml(cfg, path, NULL));
	g_assert_cmpstr(gowl_config_get_border_color_focus(cfg), ==, "#aabbcc");
}

static void
test_config_spec_round_trips(void)
{
	g_autoptr(GowlConfig) cfg = gowl_config_new();
	g_autofree gchar *path = NULL;
	g_autofree gchar *spec = NULL;

	path = write_config("border-color-focus: accent\n");
	g_assert_true(gowl_config_load_yaml(cfg, path, NULL));

	/* The property keeps the reference; only the getter resolves.  If
	 * the spec were overwritten with the literal, writing the config
	 * back out would bake in today's palette and the next theme change
	 * would not reach it. */
	g_object_get(cfg, "border-color-focus", &spec, NULL);
	g_assert_cmpstr(spec, ==, "accent");
}

static void
test_config_flavour_switch(void)
{
	g_autoptr(GowlConfig) cfg = gowl_config_new();

	gowl_config_set_palette_name(cfg, "latte");
	g_assert_cmpstr(gowl_config_get_palette_name(cfg), ==, "latte");
	g_assert_cmpstr(gowl_config_get_border_color_focus(cfg), ==, "#1e66f5");

	gowl_config_set_palette_name(cfg, "dwm");
	g_assert_cmpstr(gowl_config_get_border_color_focus(cfg), ==, "#005577");
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/palette/resolve/literal", test_resolve_literal);
	g_test_add_func("/palette/resolve/literal-beats-entry",
	                test_resolve_literal_beats_entry);
	g_test_add_func("/palette/resolve/name", test_resolve_name);
	g_test_add_func("/palette/resolve/unknown",
	                test_resolve_unknown_name_is_itself);
	g_test_add_func("/palette/resolve/alpha", test_resolve_alpha_suffix);
	g_test_add_func("/palette/resolve/alpha-replaces",
	                test_resolve_alpha_replaces_existing);
	g_test_add_func("/palette/resolve/alpha-bad",
	                test_resolve_bad_alpha_is_visible);
	g_test_add_func("/palette/resolve/null", test_resolve_null);

	g_test_add_func("/palette/builtins/same-keys",
	                test_builtins_share_a_key_set);
	g_test_add_func("/palette/builtins/unknown",
	                test_unknown_builtin_falls_back);
	g_test_add_func("/palette/builtins/dwm",
	                test_dwm_flavour_restores_old_colours);
	g_test_add_func("/palette/merge", test_merge_overrides);
	g_test_add_func("/palette/key-is-color", test_key_is_color);

	g_test_add_func("/palette/config/default-borders",
	                test_config_default_borders_follow_palette);
	g_test_add_func("/palette/config/block", test_config_palette_block);
	g_test_add_func("/palette/config/block-order",
	                test_config_palette_block_order_does_not_matter);
	g_test_add_func("/palette/config/module-colors",
	                test_config_module_colors_resolve);
	g_test_add_func("/palette/config/override-survives-reload",
	                test_config_runtime_override_survives_reload);
	g_test_add_func("/palette/config/spec-round-trips",
	                test_config_spec_round_trips);
	g_test_add_func("/palette/config/flavour-switch",
	                test_config_flavour_switch);

	return g_test_run();
}
