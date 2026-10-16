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
 * Tests for the `keywheel' bar widget.
 *
 * Two halves.  The model (bar-keywheel-model.c) is compiled straight
 * into this binary, so its classification, layout, geometry, key map,
 * search and parsers are tested as plain functions against gowl's own
 * keybind parser and the shipped default config's binds (captured in
 * tests/fixtures/bar/keywheel/default-keybinds.tsv).  The widget is
 * then driven through bar.so with the stub host: with no compositor
 * (it must say so, not crash), and with a fixture model installed
 * through bar_keywheel_widget_install() -- the same call the widget's
 * own config snapshot goes through.
 */

#include <glib.h>
#include <string.h>
#include <cairo.h>

#include "../modules/bar/bar-keywheel-model.c"

#include "gowl-enums.h"
#include "config/gowl-config.h"
#include "config/gowl-keybind.h"

#define BAR_HARNESS_MODULE GOWL_TEST_BAR_MODULE
#include "bar-widget-harness.h"
#include "barkit/gowl-bar-plugin-proxy.h"

#ifndef GOWL_TEST_BAR_MODULE
#define GOWL_TEST_BAR_MODULE "build/release/modules/bar.so"
#endif
#ifndef GOWL_TEST_FIXTURES
#define GOWL_TEST_FIXTURES "tests/fixtures/bar"
#endif

typedef void (*InstallFn) (gpointer, BarKeywheelModel *);

/* ----------------------------------------------------------------
 * Fixtures
 * ---------------------------------------------------------------- */

/* One bind, parsed by gowl's own keybind parser. */
static void
add_combo(BarKeywheelModel *m, const gchar *combo, const gchar *action,
          const gchar *arg, const gchar *desc, guint flags)
{
	guint mods = 0, keysym = 0;

	g_assert_true(gowl_keybind_parse(combo, &mods, &keysym));
	bar_keywheel_model_add_bind(m, mods, keysym, flags, action, arg, desc);
}

/* The shipped default config's keybinds, as a finished model. */
static BarKeywheelModel *
default_model(void)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	g_auto(GStrv) lines = NULL;
	BarKeywheelModel *m;
	guint i;

	path = g_build_filename(GOWL_TEST_FIXTURES, "keywheel",
	                        "default-keybinds.tsv", NULL);
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	lines = g_strsplit(text, "\n", -1);

	m = bar_keywheel_model_new();
	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) f = NULL;

		if (lines[i][0] == '\0' || lines[i][0] == '#')
			continue;
		f = g_strsplit(lines[i], "\t", 5);
		g_assert_cmpuint(g_strv_length(f), ==, 5);
		add_combo(m, f[0], f[1], f[2], f[3],
		          g_str_equal(f[4], "true") ? BAR_KEYWHEEL_FLAG_RELEASE
		                                    : 0);
	}
	bar_keywheel_model_finish(m);
	return m;
}

static gchar **
spoke_labels(const BarKeywheelModel *m, BarKeywheelFace face)
{
	GPtrArray *out = g_ptr_array_new();
	guint i;

	for (i = 0; i < bar_keywheel_n_spokes(m, face); i++)
		g_ptr_array_add(out,
			g_strdup(bar_keywheel_spoke(m, face, i)->label));
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

/* ----------------------------------------------------------------
 * Chords
 * ---------------------------------------------------------------- */

/* Catches: the mirrored modifier bits drifting from gowl's, which
   would file every chord on the wrong ring. */
static void
test_mods_mirror(void)
{
	g_assert_cmpuint(BAR_KEYWHEEL_MOD_SHIFT, ==, GOWL_KEY_MOD_SHIFT);
	g_assert_cmpuint(BAR_KEYWHEEL_MOD_CTRL,  ==, GOWL_KEY_MOD_CTRL);
	g_assert_cmpuint(BAR_KEYWHEEL_MOD_ALT,   ==, GOWL_KEY_MOD_ALT);
	g_assert_cmpuint(BAR_KEYWHEEL_MOD_LOGO,  ==, GOWL_KEY_MOD_LOGO);
	g_assert_cmpuint(BAR_KEYWHEEL_MOD_MOD3,  ==, GOWL_KEY_MOD_MOD3);
	g_assert_cmpuint(BAR_KEYWHEEL_MOD_MOD5,  ==, GOWL_KEY_MOD_MOD5);
	g_assert_cmpuint(BAR_KEYWHEEL_FLAG_RELEASE, ==,
	                 GOWL_KEYBIND_FLAG_RELEASE);
}

/* Catches: a chord landing on the wrong ring or face -- the whole
   point of the wheel is that position means modifier set. */
static void
test_classify(void)
{
	static const struct {
		const gchar *combo;
		gboolean     wheel;
		gint         ring;
		gboolean     alt;
	} cases[] = {
		{ "Super+Return",            TRUE,  BAR_KEYWHEEL_RING_SUPER, FALSE },
		{ "Super+Shift+Return",      TRUE,  BAR_KEYWHEEL_RING_SHIFT, FALSE },
		{ "Super+Ctrl+h",            TRUE,  BAR_KEYWHEEL_RING_CTRL,  FALSE },
		{ "Super+Ctrl+Shift+q",      TRUE,  BAR_KEYWHEEL_RING_HUB,   FALSE },
		{ "Super+Shift+Ctrl+1",      TRUE,  BAR_KEYWHEEL_RING_HUB,   FALSE },
		{ "Super+Alt+b",             TRUE,  BAR_KEYWHEEL_RING_SUPER, TRUE },
		{ "Super+Ctrl+Alt+b",        TRUE,  BAR_KEYWHEEL_RING_CTRL,  TRUE },
		{ "Super+Alt+Shift+s",       TRUE,  BAR_KEYWHEEL_RING_SHIFT, TRUE },
		{ "Alt+Tab",                 FALSE, 0, FALSE },
		{ "XF86AudioRaiseVolume",    FALSE, 0, FALSE },
		{ "Print",                   FALSE, 0, FALSE },
		{ "Ctrl+Shift+Escape",       FALSE, 0, FALSE },
		{ "Super+Mod3+x",            FALSE, 0, FALSE },
		{ "Super+Mod5+x",            FALSE, 0, FALSE }
	};
	guint i;
	gint r;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		guint mods = 0, keysym = 0;
		BarKeywheelRing ring = BAR_KEYWHEEL_RING_COUNT;
		gboolean alt = FALSE, wheel;

		g_assert_true(gowl_keybind_parse(cases[i].combo, &mods,
		                                 &keysym));
		wheel = bar_keywheel_classify(mods, &ring, &alt);
		g_assert_cmpint(wheel, ==, cases[i].wheel);
		if (!wheel)
			continue;
		g_assert_cmpint(ring, ==, cases[i].ring);
		g_assert_cmpint(alt, ==, cases[i].alt);
		/* ring_mods is the exact inverse. */
		g_assert_cmpuint(bar_keywheel_ring_mods(ring, alt), ==, mods);
	}

	/* NumLock (Mod2) and CapsLock never move a chord. */
	{
		BarKeywheelRing ring = BAR_KEYWHEEL_RING_COUNT;

		g_assert_true(bar_keywheel_classify(GOWL_KEY_MOD_LOGO |
			GOWL_KEY_MOD_MOD2 | (1u << 1), &ring, NULL));
		g_assert_cmpint(ring, ==, BAR_KEYWHEEL_RING_SUPER);
	}
	for (r = 0; r < BAR_KEYWHEEL_RING_COUNT; r++) {
		BarKeywheelRing back;
		gboolean alt;

		g_assert_true(bar_keywheel_classify(
			bar_keywheel_ring_mods((BarKeywheelRing)r, TRUE),
			&back, &alt));
		g_assert_cmpint(back, ==, r);
		g_assert_true(alt);
	}
}

/* Catches: a press of Shift+q or Shift+/ missing its spoke because the
   panel sees the SHIFTED symbol and the config names the key. */
static void
test_normalize(void)
{
	g_assert_cmpuint(bar_keywheel_normalize_keysym(XKB_KEY_Q,
		GOWL_KEY_MOD_SHIFT), ==, XKB_KEY_q);
	g_assert_cmpuint(bar_keywheel_normalize_keysym(XKB_KEY_question,
		GOWL_KEY_MOD_SHIFT), ==, XKB_KEY_slash);
	g_assert_cmpuint(bar_keywheel_normalize_keysym(XKB_KEY_exclam,
		GOWL_KEY_MOD_SHIFT), ==, XKB_KEY_1);
	g_assert_cmpuint(bar_keywheel_normalize_keysym(XKB_KEY_ISO_Left_Tab,
		GOWL_KEY_MOD_SHIFT), ==, XKB_KEY_Tab);
	g_assert_cmpuint(bar_keywheel_normalize_keysym(XKB_KEY_Return, 0),
	                 ==, XKB_KEY_Return);
}

/* Catches: labels too long for a cell, and chords spelled differently
   from gowl_keybind_to_string() -- the wheel's text must paste back
   into a config. */
static void
test_labels_and_combo(void)
{
	static const gchar *combos[] = {
		"Super+Shift+Return", "Super+Ctrl+Alt+b", "XF86AudioMute",
		"Super+Shift+slash", "Alt+Tab", "Super+Ctrl+Shift+period"
	};
	guint i;
	g_autofree gchar *a = bar_keywheel_key_label(XKB_KEY_Return);
	g_autofree gchar *b = bar_keywheel_key_label(XKB_KEY_XF86Calculator);
	g_autofree gchar *c = bar_keywheel_key_label(XKB_KEY_F11);
	g_autofree gchar *d = bar_keywheel_key_label(XKB_KEY_q);

	g_assert_cmpstr(a, ==, "Ret");
	g_assert_cmpstr(b, ==, "Calcul");
	g_assert_cmpstr(c, ==, "F11");
	g_assert_cmpstr(d, ==, "q");

	for (i = 0; i < G_N_ELEMENTS(combos); i++) {
		guint mods, keysym;
		g_autofree gchar *ours = NULL;
		g_autofree gchar *gowls = NULL;

		g_assert_true(gowl_keybind_parse(combos[i], &mods, &keysym));
		ours  = bar_keywheel_combo(mods, keysym);
		gowls = gowl_keybind_to_string(mods, keysym);
		g_assert_cmpstr(ours, ==, gowls);
	}
}

/* ----------------------------------------------------------------
 * Faces
 * ---------------------------------------------------------------- */

/* Catches: the Core face choosing different keys than documented for
   the shipped config, 1-9 not collapsing into one spoke, and the
   other faces losing keys. */
static void
test_faces_default(void)
{
	BarKeywheelModel *m = default_model();
	g_auto(GStrv) core = spoke_labels(m, BAR_KEYWHEEL_FACE_CORE);
	g_autofree gchar *joined = g_strjoinv(" ", core);
	g_auto(GStrv) keys = spoke_labels(m, BAR_KEYWHEEL_FACE_KEYS);
	g_autofree gchar *kjoined = g_strjoinv(" ", keys);
	const BarKeywheelSpoke *digits;
	const BarKeywheelBind *b;
	gint idx;
	guint i;

	g_assert_cmpstr(joined, ==, "Ret q c Spc j k h l 1-9 Tab f p");

	idx = bar_keywheel_find_spoke(m, BAR_KEYWHEEL_FACE_CORE, XKB_KEY_5);
	g_assert_cmpint(idx, ==, 8);
	digits = bar_keywheel_spoke(m, BAR_KEYWHEEL_FACE_CORE, 8);
	g_assert_cmpuint(digits->n_keysyms, ==, 9);

	g_assert_cmpuint(bar_keywheel_n_spokes(m, BAR_KEYWHEEL_FACE_LETTERS),
	                 ==, 26);

	/* Keys: digits first in keyboard order, then named keys. */
	g_assert_true(g_str_has_prefix(kjoined, "1 2 3 4 5 6 7 8 9 0 "));
	g_assert_nonnull(strstr(kjoined, "Ret"));
	g_assert_nonnull(strstr(kjoined, ","));
	g_assert_nonnull(strstr(kjoined, "/"));
	g_assert_null(strstr(kjoined, " q "));
	g_assert_cmpuint(m->keys_overflow, ==, 0);

	/* Beyond Super: Print and the ten media keys. */
	g_assert_cmpuint(bar_keywheel_list_len(m, BAR_KEYWHEEL_FACE_BEYOND),
	                 ==, 11);
	for (i = 0; i < m->beyond->len; i++) {
		b = bar_keywheel_bind(m, g_array_index(m->beyond, guint, i));
		g_assert_false(bar_keywheel_classify(b->modifiers, NULL, NULL));
	}

	/* Cells. */
	b = bar_keywheel_cell(m, BAR_KEYWHEEL_FACE_CORE, 0,
	                      BAR_KEYWHEEL_RING_SUPER, FALSE, 0);
	g_assert_nonnull(b);
	g_assert_cmpstr(b->action, ==, "spawn");
	g_assert_cmpstr(b->arg, ==, "gst");
	b = bar_keywheel_cell(m, BAR_KEYWHEEL_FACE_CORE, 0,
	                      BAR_KEYWHEEL_RING_SHIFT, FALSE, 0);
	g_assert_cmpstr(b->action, ==, "zoom");
	b = bar_keywheel_cell(m, BAR_KEYWHEEL_FACE_CORE, 8,
	                      BAR_KEYWHEEL_RING_SHIFT, FALSE, XKB_KEY_5);
	g_assert_cmpstr(b->action, ==, "tag_set");
	g_assert_cmpstr(b->arg, ==, "16");
	/* With no preference the group answers for its first key. */
	b = bar_keywheel_cell(m, BAR_KEYWHEEL_FACE_CORE, 8,
	                      BAR_KEYWHEEL_RING_SUPER, FALSE, 0);
	g_assert_cmpstr(b->arg, ==, "1");
	/* The Alt flip: Super+Alt+b is on Letters, outer ring, flipped. */
	idx = bar_keywheel_find_spoke(m, BAR_KEYWHEEL_FACE_LETTERS, XKB_KEY_b);
	g_assert_nonnull(bar_keywheel_cell(m, BAR_KEYWHEEL_FACE_LETTERS,
		(guint)idx, BAR_KEYWHEEL_RING_SUPER, TRUE, 0));
	g_assert_null(bar_keywheel_cell(m, BAR_KEYWHEEL_FACE_LETTERS,
		(guint)idx, BAR_KEYWHEEL_RING_HUB, TRUE, 0));

	bar_keywheel_model_free(m);
}

/* Catches: a Core face that comes out short (or empty) when the user
   moved the usual keys, or one that grows past twelve. */
static void
test_core_fallback(void)
{
	BarKeywheelModel *m;
	guint i;
	static const gchar *letters = "mtwsvrdinebg";

	m = bar_keywheel_model_new();
	add_combo(m, "Super+Return", "spawn", "foot", NULL, 0);
	for (i = 0; letters[i] != '\0'; i++) {
		g_autofree gchar *c = g_strdup_printf("Super+%c", letters[i]);

		add_combo(m, c, "spawn", "x", NULL, 0);
	}
	bar_keywheel_model_finish(m);
	g_assert_cmpuint(bar_keywheel_n_spokes(m, BAR_KEYWHEEL_FACE_CORE),
	                 ==, BAR_KEYWHEEL_CORE_SPOKES);
	g_assert_cmpstr(bar_keywheel_spoke(m, BAR_KEYWHEEL_FACE_CORE, 0)->label,
	                ==, "Ret");
	g_assert_cmpstr(bar_keywheel_spoke(m, BAR_KEYWHEEL_FACE_CORE, 1)->label,
	                ==, "m");
	bar_keywheel_model_free(m);

	/* No Super binds at all: an empty Core, not a crash. */
	m = bar_keywheel_model_new();
	add_combo(m, "Alt+Tab", "focus_stack", "+1", NULL, 0);
	bar_keywheel_model_finish(m);
	g_assert_cmpuint(bar_keywheel_n_spokes(m, BAR_KEYWHEEL_FACE_CORE),
	                 ==, 0);
	g_assert_cmpuint(bar_keywheel_list_len(m, BAR_KEYWHEEL_FACE_BEYOND),
	                 ==, 1);
	bar_keywheel_model_free(m);
}

/* Catches: an unbounded Keys face whose cells shrink below clickable. */
static void
test_keys_overflow(void)
{
	BarKeywheelModel *m = bar_keywheel_model_new();
	guint i;

	for (i = 0; i < 35; i++)
		bar_keywheel_model_add_bind(m, GOWL_KEY_MOD_LOGO, XKB_KEY_F1 + i,
		                            0, "spawn", "x", NULL);
	for (i = 0; i < 25; i++)
		bar_keywheel_model_add_bind(m, GOWL_KEY_MOD_LOGO,
		                            XKB_KEY_XF86Launch0 + i, 0, "spawn",
		                            "y", NULL);
	bar_keywheel_model_finish(m);
	g_assert_cmpuint(bar_keywheel_n_spokes(m, BAR_KEYWHEEL_FACE_KEYS), ==,
	                 BAR_KEYWHEEL_MAX_SPOKES);
	g_assert_cmpuint(m->keys_overflow, ==, 60 - BAR_KEYWHEEL_MAX_SPOKES);
	bar_keywheel_model_free(m);
}

/* Catches: hostile text in a config's desc reaching pango unvalidated
   or unbounded. */
static void
test_bind_text_cleaned(void)
{
	BarKeywheelModel *m = bar_keywheel_model_new();
	g_autofree gchar *huge = g_strnfill(5000, 'x');
	const BarKeywheelBind *b;

	bar_keywheel_model_add_bind(m, GOWL_KEY_MOD_LOGO, XKB_KEY_a, 0,
	                            "spawn", huge, "bad \xff\xfe utf8");
	bar_keywheel_model_add_bind(m, GOWL_KEY_MOD_LOGO, XKB_KEY_b, 0,
	                            NULL, NULL, "");
	bar_keywheel_model_finish(m);
	b = bar_keywheel_bind(m, 0);
	g_assert_true(g_utf8_validate(b->desc, -1, NULL));
	g_assert_cmpuint(strlen(b->arg), <=, BAR_KEYWHEEL_FIELD_MAX);
	b = bar_keywheel_bind(m, 1);
	g_assert_cmpstr(b->action, ==, "none");
	g_assert_null(b->desc);
	bar_keywheel_model_free(m);
}

/* ----------------------------------------------------------------
 * Geometry
 * ---------------------------------------------------------------- */

/* Catches: a cell that cannot be clicked where it is drawn, rings that
   overlap, and a layout that spills off a small canvas. */
static void
test_geometry(void)
{
	static const gint sizes[][2] = {
		{ 640, 560 }, { 360, 320 }, { 1600, 1400 }, { 900, 500 },
		{ 400, 900 }
	};
	static const guint spokes[] = { 1, 12, 26, 40 };
	guint si, ni, s, r, f;

	for (si = 0; si < G_N_ELEMENTS(sizes); si++) {
		for (ni = 0; ni < G_N_ELEMENTS(spokes); ni++) {
			BarKeywheelGeom g;
			gdouble w = sizes[si][0], h = sizes[si][1];
			gint hs, hf;
			BarKeywheelRing hr;

			bar_keywheel_geom(w, h, spokes[ni], 5, TRUE, &g);

			/* Inside the canvas, below the header, above the
			   dots and the details box. */
			g_assert_cmpfloat(g.cx - g.radius, >=, 0.0);
			g_assert_cmpfloat(g.cx + g.radius, <=, w);
			g_assert_cmpfloat(g.cy - g.radius, >=, g.header_h);
			g_assert_cmpfloat(g.cy + g.radius, <, g.dots_y - g.dot_r);
			g_assert_cmpfloat(g.dots_y + g.dot_r, <, g.details_y);
			g_assert_cmpfloat(g.details_y + g.details_h, <=, h);
			g_assert_cmpfloat(g.details_x + g.details_w, <=, w);
			g_assert_cmpfloat(g.alt_x, >=, 0.0);

			/* Rings disjoint, outermost first. */
			for (r = 0; r + 1 < BAR_KEYWHEEL_RING_COUNT; r++)
				g_assert_cmpfloat(g.ring_out[r + 1], <,
				                  g.ring_in[r]);
			g_assert_cmpfloat(g.hub_r, <,
			                  g.ring_in[BAR_KEYWHEEL_RING_HUB]);

			/* Every cell is hit at its centre, as itself. */
			for (s = 0; s < spokes[ni]; s++) {
				for (r = 0; r < BAR_KEYWHEEL_RING_COUNT; r++) {
					gdouble x, y;

					bar_keywheel_cell_center(&g, s,
						(BarKeywheelRing)r, &x, &y);
					g_assert_cmpint(bar_keywheel_hit(&g, x, y,
						&hs, &hr, &hf), ==,
						BAR_KEYWHEEL_HIT_CELL);
					g_assert_cmpint(hs, ==, (gint)s);
					g_assert_cmpint(hr, ==, (gint)r);
				}
			}

			/* The hub is the Alt switch, the dots are faces. */
			g_assert_cmpint(bar_keywheel_hit(&g, g.cx, g.cy, &hs, &hr,
				&hf), ==, BAR_KEYWHEEL_HIT_ALT);
			g_assert_cmpint(bar_keywheel_hit(&g,
				g.alt_x + g.alt_w / 2.0, g.alt_y + g.alt_h / 2.0,
				&hs, &hr, &hf), ==, BAR_KEYWHEEL_HIT_ALT);
			for (f = 0; f < 5; f++) {
				gdouble x, y;

				bar_keywheel_dot_center(&g, f, &x, &y);
				g_assert_cmpfloat(x, >, 0.0);
				g_assert_cmpfloat(x, <, w);
				g_assert_cmpint(bar_keywheel_hit(&g, x, y, &hs,
					&hr, &hf), ==, BAR_KEYWHEEL_HIT_DOT);
				g_assert_cmpint(hf, ==, (gint)f);
			}
			/* A corner is nothing. */
			g_assert_cmpint(bar_keywheel_hit(&g, 1.0, h / 2.0, &hs,
				&hr, &hf), ==, BAR_KEYWHEEL_HIT_NONE);
		}
	}

	/* A list face has no wheel: clicks inside where one would be
	   hit nothing, but the dots still work. */
	{
		BarKeywheelGeom g;
		gint hs, hf;
		BarKeywheelRing hr;
		gdouble x, y;

		bar_keywheel_geom(640, 56, 0, 4, FALSE, &g);
		g_assert_false(g.wheel);
		bar_keywheel_dot_center(&g, 3, &x, &y);
		g_assert_cmpfloat(y, <, 56.0);
		g_assert_cmpint(bar_keywheel_hit(&g, x, y, &hs, &hr, &hf), ==,
		                BAR_KEYWHEEL_HIT_DOT);
		g_assert_cmpint(hf, ==, 3);
	}
}

/* ----------------------------------------------------------------
 * Keys
 * ---------------------------------------------------------------- */

/* Catches: keys landing on the wrong spoke, ring or face; modifier
   presses closing the panel (unclaimed); Super chords being swallowed
   instead of reaching the window manager. */
static void
test_key_jump(void)
{
	BarKeywheelModel *m = default_model();
	BarKeywheelCursor c;
	BarKeywheelKeyResult r;
	gint q_core;

	bar_keywheel_cursor_reset(&c);
	q_core = bar_keywheel_find_spoke(m, BAR_KEYWHEEL_FACE_CORE, XKB_KEY_q);

	r = bar_keywheel_handle_key(m, &c, XKB_KEY_q, 0);
	g_assert_cmpint(r, ==, BAR_KEYWHEEL_KEY_MOVED);
	g_assert_cmpint(c.face, ==, BAR_KEYWHEEL_FACE_CORE);
	g_assert_cmpint(c.spoke, ==, q_core);
	g_assert_cmpint(c.ring, ==, BAR_KEYWHEEL_RING_SUPER);

	/* Shift+q arrives as Q: same spoke, Shift ring. */
	bar_keywheel_handle_key(m, &c, XKB_KEY_Q, GOWL_KEY_MOD_SHIFT);
	g_assert_cmpint(c.spoke, ==, q_core);
	g_assert_cmpint(c.ring, ==, BAR_KEYWHEEL_RING_SHIFT);

	/* Ctrl+Shift+1 arrives as exclam: the 1-9 spoke, the hub. */
	bar_keywheel_handle_key(m, &c, XKB_KEY_exclam,
	                        GOWL_KEY_MOD_SHIFT | GOWL_KEY_MOD_CTRL);
	g_assert_cmpint(c.spoke, ==, 8);
	g_assert_cmpint(c.ring, ==, BAR_KEYWHEEL_RING_HUB);
	g_assert_cmpuint(c.keysym, ==, XKB_KEY_1);

	/* Alt+b: not on Core, so Letters, flipped. */
	bar_keywheel_handle_key(m, &c, XKB_KEY_b, GOWL_KEY_MOD_ALT);
	g_assert_cmpint(c.face, ==, BAR_KEYWHEEL_FACE_LETTERS);
	g_assert_true(c.alt);
	g_assert_cmpint(c.spoke, ==, 1);

	/* On Letters, q stays on Letters: current face first. */
	bar_keywheel_handle_key(m, &c, XKB_KEY_q, 0);
	g_assert_cmpint(c.face, ==, BAR_KEYWHEEL_FACE_LETTERS);
	g_assert_false(c.alt);

	/* A symbol only on Keys. */
	bar_keywheel_cursor_reset(&c);
	bar_keywheel_handle_key(m, &c, XKB_KEY_comma, 0);
	g_assert_cmpint(c.face, ==, BAR_KEYWHEEL_FACE_KEYS);

	/* A media key: the Beyond list, its row. */
	bar_keywheel_handle_key(m, &c, XKB_KEY_XF86AudioPlay, 0);
	g_assert_cmpint(c.face, ==, BAR_KEYWHEEL_FACE_BEYOND);
	g_assert_cmpint(c.row, >=, 0);
	g_assert_cmpuint(bar_keywheel_bind(m, g_array_index(m->beyond, guint,
		c.row))->keysym, ==, XKB_KEY_XF86AudioPlay);

	/* Super chords are the window manager's. */
	g_assert_cmpint(bar_keywheel_handle_key(m, &c, XKB_KEY_q,
		GOWL_KEY_MOD_LOGO), ==, BAR_KEYWHEEL_KEY_IGNORED);

	/* Modifiers alone: claimed, and they preview rings. */
	bar_keywheel_cursor_reset(&c);
	g_assert_cmpint(bar_keywheel_handle_key(m, &c, XKB_KEY_Shift_L, 0),
	                ==, BAR_KEYWHEEL_KEY_MOVED);
	g_assert_cmpint(c.ring, ==, BAR_KEYWHEEL_RING_SHIFT);
	bar_keywheel_handle_key(m, &c, XKB_KEY_Control_R, GOWL_KEY_MOD_SHIFT);
	g_assert_cmpint(c.ring, ==, BAR_KEYWHEEL_RING_HUB);
	bar_keywheel_handle_key(m, &c, XKB_KEY_Alt_L, 0);
	g_assert_true(c.alt);
	bar_keywheel_handle_key(m, &c, XKB_KEY_Alt_L, 0);
	g_assert_false(c.alt);
	g_assert_cmpint(bar_keywheel_handle_key(m, &c, XKB_KEY_Super_L, 0),
	                ==, BAR_KEYWHEEL_KEY_CLAIMED);
	g_assert_cmpint(bar_keywheel_handle_key(m, &c, XKB_KEY_Caps_Lock, 0),
	                ==, BAR_KEYWHEEL_KEY_CLAIMED);

	/* Faces wrap both ways over four faces. */
	bar_keywheel_cursor_reset(&c);
	bar_keywheel_handle_key(m, &c, XKB_KEY_Prior, 0);
	g_assert_cmpint(c.face, ==, BAR_KEYWHEEL_FACE_BEYOND);
	bar_keywheel_handle_key(m, &c, XKB_KEY_Next, 0);
	g_assert_cmpint(c.face, ==, BAR_KEYWHEEL_FACE_CORE);

	/* Around the ring, wrapping; Home/End clamp. */
	bar_keywheel_handle_key(m, &c, XKB_KEY_Left, 0);
	g_assert_cmpint(c.spoke, ==, 0);
	bar_keywheel_handle_key(m, &c, XKB_KEY_Left, 0);
	g_assert_cmpint(c.spoke, ==, 11);
	bar_keywheel_handle_key(m, &c, XKB_KEY_Right, 0);
	g_assert_cmpint(c.spoke, ==, 0);
	bar_keywheel_handle_key(m, &c, XKB_KEY_Home, 0);
	g_assert_cmpint(c.ring, ==, BAR_KEYWHEEL_RING_SUPER);
	bar_keywheel_handle_key(m, &c, XKB_KEY_End, 0);
	bar_keywheel_handle_key(m, &c, XKB_KEY_End, 0);
	bar_keywheel_handle_key(m, &c, XKB_KEY_End, 0);
	bar_keywheel_handle_key(m, &c, XKB_KEY_End, 0);
	g_assert_cmpint(c.ring, ==, BAR_KEYWHEEL_RING_HUB);

	/* / searches; Shift+/ is the slash KEY on the Shift ring. */
	g_assert_cmpint(bar_keywheel_handle_key(m, &c, XKB_KEY_slash, 0), ==,
	                BAR_KEYWHEEL_KEY_SEARCH);
	g_assert_cmpint(bar_keywheel_handle_key(m, &c, XKB_KEY_question,
		GOWL_KEY_MOD_SHIFT), ==, BAR_KEYWHEEL_KEY_MOVED);
	g_assert_cmpint(c.face, ==, BAR_KEYWHEEL_FACE_KEYS);
	g_assert_cmpint(c.ring, ==, BAR_KEYWHEEL_RING_SHIFT);

	/* A key nobody bound is still claimed: a stray key must not
	   close the overlay. */
	g_assert_cmpint(bar_keywheel_handle_key(m, &c, XKB_KEY_XF86Calculator,
		0), ==, BAR_KEYWHEEL_KEY_CLAIMED);

	/* With no model at all nothing crashes. */
	bar_keywheel_cursor_reset(&c);
	g_assert_cmpint(bar_keywheel_handle_key(NULL, &c, XKB_KEY_q, 0), ==,
	                BAR_KEYWHEEL_KEY_CLAIMED);

	bar_keywheel_model_free(m);
}

/* Catches: Enter dispatching the cursor's normalised key instead of the
   configured one (a `Super+Shift+Q' config stores Q, and the
   compositor matches it exactly), and release binds losing their flag. */
static void
test_run_target(void)
{
	BarKeywheelModel *m = bar_keywheel_model_new();
	BarKeywheelCursor c;
	guint mods = 0, keysym = 0, flags = 0;

	bar_keywheel_model_add_bind(m, GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_SHIFT,
	                            XKB_KEY_Q, 0, "quit", NULL, "Quit");
	add_combo(m, "Super+Return", "spawn", "gst", "Terminal",
	          BAR_KEYWHEEL_FLAG_RELEASE);
	add_combo(m, "XF86AudioMute", "spawn", "wpctl", "Mute", 0);
	bar_keywheel_model_finish(m);

	bar_keywheel_cursor_reset(&c);
	g_assert_false(bar_keywheel_run_target(m, &c, &mods, &keysym, &flags));

	bar_keywheel_handle_key(m, &c, XKB_KEY_Q, GOWL_KEY_MOD_SHIFT);
	g_assert_true(bar_keywheel_run_target(m, &c, &mods, &keysym, &flags));
	g_assert_cmpuint(mods, ==, GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_SHIFT);
	g_assert_cmpuint(keysym, ==, XKB_KEY_Q);

	/* The same spoke on an empty ring: nothing to run. */
	bar_keywheel_handle_key(m, &c, XKB_KEY_q, GOWL_KEY_MOD_CTRL);
	g_assert_false(bar_keywheel_run_target(m, &c, &mods, &keysym, NULL));

	bar_keywheel_handle_key(m, &c, XKB_KEY_Return, 0);
	g_assert_true(bar_keywheel_run_target(m, &c, &mods, &keysym, &flags));
	g_assert_cmpuint(keysym, ==, XKB_KEY_Return);
	g_assert_cmpuint(flags, ==, BAR_KEYWHEEL_FLAG_RELEASE);

	bar_keywheel_handle_key(m, &c, XKB_KEY_XF86AudioMute, 0);
	g_assert_true(bar_keywheel_run_target(m, &c, &mods, &keysym, &flags));
	g_assert_cmpuint(mods, ==, 0);
	g_assert_cmpuint(keysym, ==, XKB_KEY_XF86AudioMute);

	/* A click on a cell selects; the second click is the run. */
	bar_keywheel_cursor_reset(&c);
	g_assert_false(bar_keywheel_select_cell(m, &c, 0,
	                                        BAR_KEYWHEEL_RING_SUPER));
	g_assert_true(bar_keywheel_select_cell(m, &c, 0,
	                                       BAR_KEYWHEEL_RING_SUPER));
	bar_keywheel_model_free(m);
}

/* ----------------------------------------------------------------
 * Search
 * ---------------------------------------------------------------- */

/* Catches: search missing on the obvious fields, terms ORed instead of
   ANDed, the cap dropping the count, and invalid UTF-8 crashing the
   casefold. */
static void
test_search(void)
{
	BarKeywheelModel *m = default_model();
	g_autoptr(GArray) b = g_array_new(FALSE, FALSE, sizeof(guint));
	g_autoptr(GArray) e = g_array_new(FALSE, FALSE, sizeof(guint));
	GPtrArray *emacs;
	guint n;

	n = bar_keywheel_search(m, "terminal", 10, b, e);
	g_assert_cmpuint(n, ==, 1);
	g_assert_cmpstr(bar_keywheel_bind(m, g_array_index(b, guint, 0))->combo,
	                ==, "Super+Return");

	g_array_set_size(b, 0);
	n = bar_keywheel_search(m, "  VOLUME   up ", 10, b, e);
	g_assert_cmpuint(n, ==, 1);
	g_assert_cmpstr(bar_keywheel_bind(m, g_array_index(b, guint, 0))->combo,
	                ==, "XF86AudioRaiseVolume");

	/* The action, and the chord itself. */
	g_array_set_size(b, 0);
	n = bar_keywheel_search(m, "TAG_TOGGLE_VIEW", 3, b, e);
	g_assert_cmpuint(n, ==, 9);
	g_assert_cmpuint(b->len, ==, 3);
	g_array_set_size(b, 0);
	g_assert_cmpuint(bar_keywheel_search(m, "super+shift+return", 10, b, e),
	                 ==, 1);

	g_array_set_size(b, 0);
	g_assert_cmpuint(bar_keywheel_search(m, "", 10, b, e), ==, 0);
	g_assert_cmpuint(bar_keywheel_search(m, "   ", 10, b, e), ==, 0);
	g_assert_cmpuint(bar_keywheel_search(m, NULL, 10, b, e), ==, 0);
	g_assert_cmpuint(bar_keywheel_search(m, "zzqqxx", 10, b, e), ==, 0);
	(void)bar_keywheel_search(m, "\xff\xfe\xc3", 10, b, e);

	/* Emacs entries come after binds. */
	emacs = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_keywheel_emacs_free);
	g_ptr_array_add(emacs, bar_keywheel_emacs_parse(
		"C-c a\tcmacs-ai-menu\tOpen the terminal AI menu"));
	bar_keywheel_model_set_emacs(m, emacs);
	g_ptr_array_unref(emacs);
	g_array_set_size(b, 0);
	n = bar_keywheel_search(m, "terminal", 10, b, e);
	g_assert_cmpuint(n, ==, 2);
	g_assert_cmpuint(b->len, ==, 1);
	g_assert_cmpuint(e->len, ==, 1);
	g_assert_cmpuint(bar_keywheel_face_count(m), ==, 5);

	bar_keywheel_model_free(m);
}

/* ----------------------------------------------------------------
 * Settings
 * ---------------------------------------------------------------- */

/* Catches: the role table drifting from barkit's (a role name that
   silently paints the wrong colour), hex literals misparsed, and
   garbage painting black instead of the default. */
static void
test_colors(void)
{
	gdouble rgba[4] = { 0, 0, 0, 0 };
	gint role, i;

	for (i = 0; i < GOWL_BAR_COLOR_COUNT; i++) {
		g_assert_cmpstr(bar_keywheel_role_name(i), ==,
		                gowl_bar_theme_color_name((GowlBarColor)i));
		g_assert_true(bar_keywheel_role_from_name(
			gowl_bar_theme_color_name((GowlBarColor)i), &role));
		g_assert_cmpint(role, ==, i);
	}
	g_assert_null(bar_keywheel_role_name(GOWL_BAR_COLOR_COUNT));

	g_assert_cmpint(bar_keywheel_color_resolve("teal", GOWL_BAR_COLOR_BLUE,
		&role, rgba), ==, BAR_KEYWHEEL_COLOR_ROLE);
	g_assert_cmpint(role, ==, GOWL_BAR_COLOR_TEAL);
	g_assert_cmpint(bar_keywheel_color_resolve("Surface-Alt",
		GOWL_BAR_COLOR_BLUE, &role, rgba), ==, BAR_KEYWHEEL_COLOR_ROLE);
	g_assert_cmpint(role, ==, GOWL_BAR_COLOR_SURFACE_ALT);

	g_assert_cmpint(bar_keywheel_color_resolve("#ff8000",
		GOWL_BAR_COLOR_BLUE, &role, rgba), ==, BAR_KEYWHEEL_COLOR_HEX);
	g_assert_cmpfloat_with_epsilon(rgba[0], 1.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(rgba[1], 128.0 / 255.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(rgba[2], 0.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(rgba[3], 1.0, 1e-9);
	g_assert_cmpint(bar_keywheel_color_resolve("#00000080",
		GOWL_BAR_COLOR_BLUE, &role, rgba), ==, BAR_KEYWHEEL_COLOR_HEX);
	g_assert_cmpfloat_with_epsilon(rgba[3], 128.0 / 255.0, 1e-9);

	{
		static const gchar *bad[] = {
			"#zzzzzz", "#12345", "#-12345", "bluish", "", "#",
			"red; rm -rf /", "#ff00ff00ff"
		};
		guint j;

		for (j = 0; j < G_N_ELEMENTS(bad); j++) {
			g_assert_cmpint(bar_keywheel_color_resolve(bad[j],
				GOWL_BAR_COLOR_PEACH, &role, rgba), ==,
				BAR_KEYWHEEL_COLOR_FALLBACK);
			g_assert_cmpint(role, ==, GOWL_BAR_COLOR_PEACH);
		}
	}
	g_assert_cmpint(bar_keywheel_color_resolve(NULL, GOWL_BAR_COLOR_RED,
		&role, rgba), ==, BAR_KEYWHEEL_COLOR_FALLBACK);
	g_assert_cmpint(role, ==, GOWL_BAR_COLOR_RED);
}

/* Catches: a `size' setting that makes a canvas too small to draw or
   larger than any screen, and garbage being taken literally. */
static void
test_size(void)
{
	static const struct {
		const gchar *spec;
		gint w, h;
	} cases[] = {
		{ "640x560", 640, 560 }, { "800X600", 800, 600 },
		{ "10x10", 360, 320 }, { "99999x99999", 1600, 1400 },
		{ "huge", 640, 560 }, { "", 640, 560 }, { "640x", 640, 560 },
		{ "x480", 640, 560 }, { "640x480junk", 640, 560 },
		{ "-5x-5", 360, 320 }, { NULL, 640, 560 }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		gint w = 0, h = 0;

		bar_keywheel_parse_size(cases[i].spec, &w, &h);
		g_assert_cmpint(w, ==, cases[i].w);
		g_assert_cmpint(h, ==, cases[i].h);
	}
}

/* Catches: the Emacs face's wire format mis-split on escaped tabs,
   escapes left in the text, and hostile pushes reaching pango. */
static void
test_emacs_parse(void)
{
	BarKeywheelEmacs *e;
	g_autofree gchar *u = NULL;
	g_autofree gchar *huge = NULL;
	g_autofree gchar *line = NULL;

	e = bar_keywheel_emacs_parse("C-c a\tcmacs-ai-menu\tOpen the AI menu");
	g_assert_nonnull(e);
	g_assert_cmpstr(e->key, ==, "C-c a");
	g_assert_cmpstr(e->command, ==, "cmacs-ai-menu");
	g_assert_cmpstr(e->doc, ==, "Open the AI menu");
	bar_keywheel_emacs_free(e);

	/* Escaped tab and newline inside a field stay inside it. */
	e = bar_keywheel_emacs_parse(
		"C-c \\\\\tmy-cmd\tFirst\\tpart\\nsecond \\\\ end");
	g_assert_nonnull(e);
	g_assert_cmpstr(e->key, ==, "C-c \\");
	g_assert_cmpstr(e->doc, ==, "First part second \\ end");
	bar_keywheel_emacs_free(e);

	/* No doc is fine; no command is not. */
	e = bar_keywheel_emacs_parse("C-x C-f\tfind-file");
	g_assert_nonnull(e);
	g_assert_null(e->doc);
	bar_keywheel_emacs_free(e);
	g_assert_null(bar_keywheel_emacs_parse("C-x C-f"));
	g_assert_null(bar_keywheel_emacs_parse("\tfind-file\tdoc"));
	g_assert_null(bar_keywheel_emacs_parse(""));
	g_assert_null(bar_keywheel_emacs_parse(NULL));

	e = bar_keywheel_emacs_parse("C-c \xff\tcmd\t\xc3\x28 bad");
	g_assert_nonnull(e);
	g_assert_true(g_utf8_validate(e->key, -1, NULL));
	g_assert_true(g_utf8_validate(e->doc, -1, NULL));
	bar_keywheel_emacs_free(e);

	huge = g_strnfill(4000, 'd');
	line = g_strdup_printf("C-c d\tcmd\t%s", huge);
	e = bar_keywheel_emacs_parse(line);
	g_assert_cmpuint(strlen(e->doc), <=, BAR_KEYWHEEL_FIELD_MAX);
	bar_keywheel_emacs_free(e);

	u = bar_keywheel_unescape("a\\qb\\");
	g_assert_cmpstr(u, ==, "aqb\\");
}

/* ----------------------------------------------------------------
 * The widget, through bar.so
 * ---------------------------------------------------------------- */

static gpointer
widget_data(BarHarness *h)
{
	return gowl_bar_plugin_proxy_get_data(GOWL_BAR_PLUGIN_PROXY(h->plugin));
}

static gboolean
install_default(BarHarness *h)
{
	InstallFn install;

	install = (InstallFn)bar_harness_sym(h, "bar_keywheel_widget_install");
	if (install == NULL) {
		g_test_skip("bar.so has no bar_keywheel_widget_install");
		return FALSE;
	}
	install(widget_data(h), default_model());
	return TRUE;
}

/* Paint the canvas into an image; returns the surface. */
static cairo_surface_t *
paint(BarHarness *h, gint w, gint hgt)
{
	cairo_surface_t *s;
	cairo_t *cr;

	s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, hgt);
	cr = cairo_create(s);
	gowl_bar_plugin_panel_canvas(h->plugin, "wheel", cr, h->host->theme,
	                             w, hgt);
	cairo_destroy(cr);
	cairo_surface_flush(s);
	return s;
}

static void
pixel(cairo_surface_t *s, gint x, gint y, guint8 out[3])
{
	guint8 *data = cairo_image_surface_get_data(s);
	gint stride = cairo_image_surface_get_stride(s);
	guint32 p = *(guint32 *)(data + y * stride + x * 4);

	out[0] = (guint8)((p >> 16) & 0xff);
	out[1] = (guint8)((p >> 8) & 0xff);
	out[2] = (guint8)(p & 0xff);
}

static GowlBarPanelItem *
find_item(GowlBarPanel *panel, const gchar *id)
{
	return gowl_bar_panel_find(panel, id);
}

/* Catches: the widget crashing or showing an empty overlay when there
   is no compositor (standalone tests, a bar not yet started), and the
   panel losing its shape: a centred overlay whose first item is the
   canvas. */
static void
test_widget_no_compositor(void)
{
	BarHarness h;
	g_autoptr(GowlBarPanel) panel = NULL;
	g_autofree gchar *icon = NULL;
	g_autofree gchar *tip = NULL;
	cairo_surface_t *s;

	if (!bar_harness_up(&h, "bar_register_keywheel_plugins", "keywheel"))
		return;
	bar_harness_activate(&h);
	bar_harness_poll(&h);
	icon = gowl_bar_plugin_dup_icon(h.plugin);
	tip  = gowl_bar_plugin_dup_tooltip(h.plugin);
	g_assert_cmpstr(icon, ==, "\xe2\x8c\xa8");
	g_assert_cmpstr(tip, ==, "Key wheel");

	panel = bar_harness_panel(&h);
	g_assert_true(gowl_bar_panel_get_centered(panel));
	g_assert_cmpint(gowl_bar_panel_item_get_kind(
		gowl_bar_panel_get_item(panel, 0)), ==, GOWL_BAR_ITEM_CANVAS);
	g_assert_cmpstr(gowl_bar_panel_item_get_id(
		gowl_bar_panel_get_item(panel, 0)), ==, "wheel");
	g_assert_true(bar_harness_panel_mentions(panel,
		"No keybinds available"));
	g_assert_nonnull(find_item(panel, "search"));
	g_assert_nonnull(find_item(panel, "nav"));

	/* Paints without a model's binds, keys do not crash, Enter with
	   nothing selected does nothing. */
	s = paint(&h, 640, 560);
	cairo_surface_destroy(s);
	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_q, 0, 0));
	gowl_bar_plugin_panel_action(h.plugin, "wheel", -1, -1.0, 1);
	g_assert_cmpuint(h.host->toasts->len, ==, 0);
	g_assert_cmpuint(h.host->closed, ==, 0);

	bar_harness_down(&h);
}

/* Catches: a key, a click or Enter not reaching the compositor's
   dispatch (here: the panel is closed and, with no compositor, the
   user is told so rather than nothing happening), a second click not
   running, and dots not turning the face. */
static void
test_widget_drive(void)
{
	BarHarness h;
	BarKeywheelGeom g;
	gdouble x, y;
	cairo_surface_t *s;
	guint closed;

	if (!bar_harness_up(&h, "bar_register_keywheel_plugins", "keywheel"))
		return;
	bar_harness_activate(&h);
	if (!install_default(&h)) {
		bar_harness_down(&h);
		return;
	}

	{
		g_autoptr(GowlBarPanel) panel = bar_harness_panel(&h);

		g_assert_false(bar_harness_panel_mentions(panel,
			"No keybinds available"));
		g_assert_cmpint(gowl_bar_panel_item_get_height(
			find_item(panel, "wheel")), ==, 560);
	}

	/* A key moves the cursor and asks for a repaint.  Super+p is the
	   launcher in the shipped config. */
	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_p, 0, 0));
	g_assert_cmpuint(h.host->refreshes, >=, 1);
	/* Super chords are refused, so the WM still gets them. */
	g_assert_false(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_q,
	                                         GOWL_KEY_MOD_LOGO, 0));

	/* Enter on the canvas runs the selection. */
	gowl_bar_plugin_panel_action(h.plugin, "wheel", -1, -1.0, 1);
	g_assert_cmpuint(h.host->closed, ==, 1);
	g_assert_true(bar_harness_toasted(&h, "no compositor"));

	/* Clicks: geometry from the painted size, as the widget uses. */
	gowl_bar_plugin_panel_closed(h.plugin);
	s = paint(&h, 640, 560);
	cairo_surface_destroy(s);
	bar_keywheel_geom(640, 560, 12, 4, TRUE, &g);
	bar_keywheel_cell_center(&g, 0, BAR_KEYWHEEL_RING_SHIFT, &x, &y);
	closed = h.host->closed;
	gowl_bar_plugin_panel_action(h.plugin, "wheel", (gint)x, y, 1);
	g_assert_cmpuint(h.host->closed, ==, closed);
	gowl_bar_plugin_panel_action(h.plugin, "wheel", (gint)x, y, 1);
	g_assert_cmpuint(h.host->closed, ==, closed + 1);

	/* The fourth dot is Beyond Super: its rows appear. */
	gowl_bar_plugin_panel_closed(h.plugin);
	bar_keywheel_dot_center(&g, 3, &x, &y);
	gowl_bar_plugin_panel_action(h.plugin, "wheel", (gint)x, y, 1);
	{
		g_autoptr(GowlBarPanel) panel = bar_harness_panel(&h);

		g_assert_true(bar_harness_panel_mentions(panel, "Volume up"));
		g_assert_true(bar_harness_panel_mentions(panel,
		                                         "XF86AudioMute"));
		g_assert_cmpint(gowl_bar_panel_item_get_height(
			find_item(panel, "wheel")), <, 100);
	}

	/* The nav row turns faces too, and wraps. */
	gowl_bar_plugin_panel_action(h.plugin, "nav", 2, 0.0, 1);
	{
		g_autoptr(GowlBarPanel) panel = bar_harness_panel(&h);

		g_assert_false(bar_harness_panel_mentions(panel, "Volume up"));
		g_assert_cmpint(gowl_bar_panel_item_get_height(
			find_item(panel, "wheel")), ==, 560);
	}

	/* A list row runs its bind. */
	closed = h.host->closed;
	gowl_bar_plugin_panel_action(h.plugin, "run:0", 0, 0.0, 1);
	g_assert_cmpuint(h.host->closed, ==, closed + 1);
	/* Out-of-range and malformed ids are ignored. */
	gowl_bar_plugin_panel_action(h.plugin, "run:99999", 0, 0.0, 1);
	gowl_bar_plugin_panel_action(h.plugin, "run:-1", 0, 0.0, 1);
	gowl_bar_plugin_panel_action(h.plugin, "run:", 0, 0.0, 1);
	g_assert_cmpuint(h.host->closed, ==, closed + 1);

	bar_harness_down(&h);
}

/* Catches: search not filtering, Return in it not running the top
   hit, `/' failing to put the keyboard in the field, and a close
   forgetting to reset the wheel. */
static void
test_widget_search(void)
{
	BarHarness h;
	GowlBarPanelItem *entry;
	guint closed, opened;

	if (!bar_harness_up(&h, "bar_register_keywheel_plugins", "keywheel"))
		return;
	bar_harness_activate(&h);
	if (!install_default(&h)) {
		bar_harness_down(&h);
		return;
	}

	gowl_bar_plugin_panel_text(h.plugin, "search", "volume", FALSE);
	{
		g_autoptr(GowlBarPanel) panel = bar_harness_panel(&h);

		g_assert_true(bar_harness_panel_mentions(panel, "Volume up"));
		g_assert_true(bar_harness_panel_mentions(panel, "Volume down"));
		g_assert_false(bar_harness_panel_mentions(panel, "Terminal"));
	}
	gowl_bar_plugin_panel_text(h.plugin, "search", "nothing-matches-this",
	                           FALSE);
	{
		g_autoptr(GowlBarPanel) panel = bar_harness_panel(&h);

		g_assert_true(bar_harness_panel_mentions(panel,
		                                         "No keybinding"));
	}
	closed = h.host->closed;
	gowl_bar_plugin_panel_text(h.plugin, "search", "terminal", TRUE);
	g_assert_cmpuint(h.host->closed, ==, closed + 1);

	/* `/': close and reopen, the field marked to take the keyboard,
	   the wheel's state intact. */
	gowl_bar_plugin_panel_closed(h.plugin);
	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_Next, 0, 0));
	closed = h.host->closed;
	opened = h.host->opened;
	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_slash, 0,
	                                        0));
	g_assert_cmpuint(h.host->closed, ==, closed + 1);
	g_assert_cmpuint(h.host->opened, ==, opened + 1);
	{
		g_autoptr(GowlBarPanel) panel = bar_harness_panel(&h);

		entry = find_item(panel, "search");
		g_assert_cmpstr(gowl_bar_panel_item_get_prop(entry,
			GOWL_BAR_ENTRY_PROP_AUTOFOCUS), ==, "true");
	}
	gowl_bar_plugin_panel_opened(h.plugin);
	{
		g_autoptr(GowlBarPanel) panel = bar_harness_panel(&h);

		entry = find_item(panel, "search");
		g_assert_null(gowl_bar_panel_item_get_prop(entry,
			GOWL_BAR_ENTRY_PROP_AUTOFOCUS));
	}

	bar_harness_down(&h);
}

/* Catches: the Emacs face not appearing from a push, a hostile command
   name breaking out of the emacsctl command line, and an absurd count
   being trusted. */
static void
test_widget_emacs(void)
{
	BarHarness h;
	g_autofree gchar *cmd = NULL;
	g_auto(GStrv) argv = NULL;
	gint argc = 0;
	guint i;

	if (!bar_harness_up(&h, "bar_register_keywheel_plugins", "keywheel"))
		return;
	bar_harness_activate(&h);
	if (!install_default(&h)) {
		bar_harness_down(&h);
		return;
	}

	gowl_bar_plugin_set_setting(h.plugin, "emacs.0",
		"C-c a\tcmacs-ai-menu\tOpen the AI menu");
	gowl_bar_plugin_set_setting(h.plugin, "emacs.1",
		"C-c x\tevil\"); (shell-command \"rm -rf ~\") ('\tHostile");
	bar_harness_set(&h, "emacs.count", "2");

	for (i = 0; i < 4; i++)
		gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_Next, 0, 0);
	{
		g_autoptr(GowlBarPanel) panel = bar_harness_panel(&h);

		g_assert_true(bar_harness_panel_mentions(panel, "cmacs-ai-menu"));
		g_assert_true(bar_harness_panel_mentions(panel, "emacs:1"));
	}

	gowl_bar_plugin_panel_action(h.plugin, "emacs:0", 0, 0.0, 1);
	g_assert_cmpuint(h.host->spawned->len, ==, 1);
	g_assert_true(bar_harness_spawned(&h,
		"(cmacs-gowl-bar-keywheel-run \"cmacs-ai-menu\")"));

	/* The hostile name: exactly three words, and the name stays one
	   Elisp string. */
	bar_harness_set(&h, "emacsctl", "/opt/my tools/emacsctl");
	gowl_bar_plugin_panel_action(h.plugin, "emacs:1", 0, 0.0, 1);
	cmd = g_strdup(g_ptr_array_index(h.host->spawned, 1));
	g_assert_true(g_shell_parse_argv(cmd, &argc, &argv, NULL));
	g_assert_cmpint(argc, ==, 3);
	g_assert_cmpstr(argv[0], ==, "/opt/my tools/emacsctl");
	g_assert_cmpstr(argv[1], ==, "eval");
	g_assert_cmpstr(argv[2], ==,
		"(cmacs-gowl-bar-keywheel-run \"evil\\\"); (shell-command "
		"\\\"rm -rf ~\\\") ('\")");

	/* An absurd count is clamped, and a count above what was pushed
	   reads nothing extra. */
	bar_harness_set(&h, "emacs.count", "999999999");
	{
		g_autoptr(GowlBarPanel) panel = bar_harness_panel(&h);

		g_assert_true(bar_harness_panel_mentions(panel, "emacs:1"));
		g_assert_false(bar_harness_panel_mentions(panel, "emacs:2"));
	}
	/* Withdrawn: the face goes, and the wheel falls back to Core. */
	bar_harness_set(&h, "emacs.count", "0");
	{
		g_autoptr(GowlBarPanel) panel = bar_harness_panel(&h);

		g_assert_false(bar_harness_panel_mentions(panel, "emacs:0"));
		g_assert_cmpint(gowl_bar_panel_item_get_height(
			find_item(panel, "wheel")), ==, 560);
	}

	bar_harness_down(&h);
}

/* Catches: colour settings that do nothing on the canvas -- a hex
   literal ignored, a role name not following the theme, garbage
   painting something other than the default. */
static void
test_widget_colors(void)
{
	BarHarness h;
	BarKeywheelGeom g;
	cairo_surface_t *s;
	gdouble a0, a1, rad;
	gint px, py;
	guint8 rgb[3];
	const gdouble *want;

	if (!bar_harness_up(&h, "bar_register_keywheel_plugins", "keywheel"))
		return;
	bar_harness_activate(&h);
	if (!install_default(&h)) {
		bar_harness_down(&h);
		return;
	}

	/* A point inside spoke 0's rim cell (Super+Return, bound), clear
	   of its label. */
	bar_keywheel_geom(640, 560, 12, 4, TRUE, &g);
	bar_keywheel_spoke_span(&g, 0, &a0, &a1);
	rad = g.ring_out[BAR_KEYWHEEL_RING_SUPER] - 4.0;
	px = (gint)(g.cx + rad * cos(a0 + 0.12));
	py = (gint)(g.cy + rad * sin(a0 + 0.12));

	bar_harness_set(&h, "color-ring-super", "#ff0000");
	bar_harness_set(&h, "color-bg", "#00ff00");
	s = paint(&h, 640, 560);
	pixel(s, px, py, rgb);
	g_assert_cmpuint(rgb[0], ==, 255);
	g_assert_cmpuint(rgb[1], ==, 0);
	g_assert_cmpuint(rgb[2], ==, 0);
	/* Background: right of the wheel, level with its centre. */
	pixel(s, 636, (gint)g.cy, rgb);
	g_assert_cmpuint(rgb[1], ==, 255);
	cairo_surface_destroy(s);

	/* A role name: the theme's colour for it. */
	bar_harness_set(&h, "color-ring-super", "green");
	s = paint(&h, 640, 560);
	pixel(s, px, py, rgb);
	want = gowl_bar_theme_color(h.host->theme, GOWL_BAR_COLOR_GREEN);
	g_assert_cmpint(ABS((gint)rgb[1] - (gint)(want[1] * 255.0 + 0.5)), <=,
	                1);
	cairo_surface_destroy(s);

	/* Garbage: the default role, blue. */
	bar_harness_set(&h, "color-ring-super", "not-a-colour");
	s = paint(&h, 640, 560);
	pixel(s, px, py, rgb);
	want = gowl_bar_theme_color(h.host->theme, GOWL_BAR_COLOR_BLUE);
	g_assert_cmpint(ABS((gint)rgb[2] - (gint)(want[2] * 255.0 + 0.5)), <=,
	                1);
	g_assert_cmpint(ABS((gint)rgb[0] - (gint)(want[0] * 255.0 + 0.5)), <=,
	                1);
	cairo_surface_destroy(s);

	/* The bar label's colour goes through the shared rule too. */
	bar_harness_set(&h, "color", "#123456");
	bar_harness_poll(&h);

	bar_harness_down(&h);
}

/* Catches: the aliases the docs promise not resolving. */
static void
test_widget_aliases(void)
{
	BarHarness h;
	GowlBarPlugin *p;
	GError *error = NULL;

	if (!bar_harness_up(&h, "bar_register_keywheel_plugins", "keywheel"))
		return;
	p = gowl_bar_registry_instantiate(h.registry, "keybinds", &error);
	g_assert_no_error(error);
	g_assert_nonnull(p);
	g_object_unref(p);
	p = gowl_bar_registry_instantiate(h.registry, "cheatsheet", &error);
	g_assert_no_error(error);
	g_assert_nonnull(p);
	g_object_unref(p);
	bar_harness_down(&h);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar/keywheel/mods-mirror", test_mods_mirror);
	g_test_add_func("/bar/keywheel/classify", test_classify);
	g_test_add_func("/bar/keywheel/normalize", test_normalize);
	g_test_add_func("/bar/keywheel/labels-combo", test_labels_and_combo);
	g_test_add_func("/bar/keywheel/faces-default", test_faces_default);
	g_test_add_func("/bar/keywheel/core-fallback", test_core_fallback);
	g_test_add_func("/bar/keywheel/keys-overflow", test_keys_overflow);
	g_test_add_func("/bar/keywheel/bind-text", test_bind_text_cleaned);
	g_test_add_func("/bar/keywheel/geometry", test_geometry);
	g_test_add_func("/bar/keywheel/key-jump", test_key_jump);
	g_test_add_func("/bar/keywheel/run-target", test_run_target);
	g_test_add_func("/bar/keywheel/search", test_search);
	g_test_add_func("/bar/keywheel/colors", test_colors);
	g_test_add_func("/bar/keywheel/size", test_size);
	g_test_add_func("/bar/keywheel/emacs-parse", test_emacs_parse);
	g_test_add_func("/bar/keywheel/widget/no-compositor",
	                test_widget_no_compositor);
	g_test_add_func("/bar/keywheel/widget/drive", test_widget_drive);
	g_test_add_func("/bar/keywheel/widget/search", test_widget_search);
	g_test_add_func("/bar/keywheel/widget/emacs", test_widget_emacs);
	g_test_add_func("/bar/keywheel/widget/colors", test_widget_colors);
	g_test_add_func("/bar/keywheel/widget/aliases", test_widget_aliases);

	return g_test_run();
}
