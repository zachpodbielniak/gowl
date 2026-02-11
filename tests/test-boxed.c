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

#include "boxed/gowl-geometry.h"
#include "boxed/gowl-color.h"
#include "boxed/gowl-key-combo.h"
#include "boxed/gowl-tag-mask.h"
#include "boxed/gowl-gaps.h"
#include "boxed/gowl-border-spec.h"
#include "boxed/gowl-rule.h"
#include "boxed/gowl-output-mode.h"

/* ---- Geometry tests ---- */

static void
test_geometry_new(void)
{
	GowlGeometry *g;

	g = gowl_geometry_new(10, 20, 800, 600);
	g_assert_nonnull(g);
	g_assert_cmpint(g->x, ==, 10);
	g_assert_cmpint(g->y, ==, 20);
	g_assert_cmpint(g->width, ==, 800);
	g_assert_cmpint(g->height, ==, 600);
	gowl_geometry_free(g);
}

static void
test_geometry_copy(void)
{
	GowlGeometry *g;
	GowlGeometry *c;

	g = gowl_geometry_new(5, 10, 100, 200);
	c = gowl_geometry_copy(g);
	g_assert_nonnull(c);
	g_assert_true(gowl_geometry_equals(g, c));
	gowl_geometry_free(g);
	gowl_geometry_free(c);
}

static void
test_geometry_contains(void)
{
	GowlGeometry *g;

	g = gowl_geometry_new(0, 0, 100, 100);
	g_assert_true(gowl_geometry_contains(g, 50, 50));
	g_assert_true(gowl_geometry_contains(g, 0, 0));
	g_assert_false(gowl_geometry_contains(g, 100, 100));
	g_assert_false(gowl_geometry_contains(g, -1, 50));
	gowl_geometry_free(g);
}

static void
test_geometry_type(void)
{
	GType type;

	type = gowl_geometry_get_type();
	g_assert_true(type != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_BOXED(type));
}

/* ---- Color tests ---- */

static void
test_color_new(void)
{
	GowlColor *c;

	c = gowl_color_new(1.0, 0.5, 0.0, 1.0);
	g_assert_nonnull(c);
	g_assert_cmpfloat_with_epsilon(c->r, 1.0, 0.001);
	g_assert_cmpfloat_with_epsilon(c->g, 0.5, 0.001);
	g_assert_cmpfloat_with_epsilon(c->b, 0.0, 0.001);
	g_assert_cmpfloat_with_epsilon(c->a, 1.0, 0.001);
	gowl_color_free(c);
}

static void
test_color_from_hex(void)
{
	GowlColor *c;

	c = gowl_color_new_from_hex("#ff8000");
	g_assert_nonnull(c);
	g_assert_cmpfloat_with_epsilon(c->r, 1.0, 0.01);
	g_assert_cmpfloat_with_epsilon(c->g, 0.502, 0.01);
	g_assert_cmpfloat_with_epsilon(c->b, 0.0, 0.01);
	g_assert_cmpfloat_with_epsilon(c->a, 1.0, 0.01);
	gowl_color_free(c);
}

static void
test_color_from_hex_with_alpha(void)
{
	GowlColor *c;

	c = gowl_color_new_from_hex("#ff000080");
	g_assert_nonnull(c);
	g_assert_cmpfloat_with_epsilon(c->r, 1.0, 0.01);
	g_assert_cmpfloat_with_epsilon(c->a, 0.502, 0.01);
	gowl_color_free(c);
}

static void
test_color_to_hex(void)
{
	GowlColor *c;
	gchar *hex;

	c = gowl_color_new(1.0, 0.0, 0.0, 1.0);
	hex = gowl_color_to_hex(c);
	g_assert_nonnull(hex);
	g_assert_cmpstr(hex, ==, "#ff0000");
	g_free(hex);
	gowl_color_free(c);
}

static void
test_color_type(void)
{
	GType type;

	type = gowl_color_get_type();
	g_assert_true(type != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_BOXED(type));
}

/* ---- TagMask tests ---- */

static void
test_tag_mask_new(void)
{
	GowlTagMask *tm;

	tm = gowl_tag_mask_new(0);
	g_assert_nonnull(tm);
	g_assert_true(gowl_tag_mask_is_empty(tm));
	gowl_tag_mask_free(tm);
}

static void
test_tag_mask_operations(void)
{
	GowlTagMask *tm;

	tm = gowl_tag_mask_new(0);

	gowl_tag_mask_set_tag(tm, 0);
	g_assert_true(gowl_tag_mask_has_tag(tm, 0));
	g_assert_false(gowl_tag_mask_is_empty(tm));
	g_assert_cmpint(gowl_tag_mask_count_tags(tm), ==, 1);

	gowl_tag_mask_set_tag(tm, 3);
	g_assert_true(gowl_tag_mask_has_tag(tm, 3));
	g_assert_cmpint(gowl_tag_mask_count_tags(tm), ==, 2);

	gowl_tag_mask_toggle_tag(tm, 0);
	g_assert_false(gowl_tag_mask_has_tag(tm, 0));
	g_assert_cmpint(gowl_tag_mask_count_tags(tm), ==, 1);

	gowl_tag_mask_clear_tag(tm, 3);
	g_assert_true(gowl_tag_mask_is_empty(tm));

	gowl_tag_mask_free(tm);
}

static void
test_tag_mask_type(void)
{
	GType type;

	type = gowl_tag_mask_get_type();
	g_assert_true(type != G_TYPE_INVALID);
	g_assert_true(G_TYPE_IS_BOXED(type));
}

/* ---- Gaps tests ---- */

static void
test_gaps_new(void)
{
	GowlGaps *g;

	g = gowl_gaps_new(10, 10, 20, 20);
	g_assert_nonnull(g);
	g_assert_cmpint(g->inner_h, ==, 10);
	g_assert_cmpint(g->inner_v, ==, 10);
	g_assert_cmpint(g->outer_h, ==, 20);
	g_assert_cmpint(g->outer_v, ==, 20);
	gowl_gaps_free(g);
}

static void
test_gaps_equals(void)
{
	GowlGaps *a;
	GowlGaps *b;

	a = gowl_gaps_new(5, 5, 10, 10);
	b = gowl_gaps_new(5, 5, 10, 10);
	g_assert_true(gowl_gaps_equals(a, b));

	b->inner_h = 6;
	g_assert_false(gowl_gaps_equals(a, b));

	gowl_gaps_free(a);
	gowl_gaps_free(b);
}

/* ---- Rule tests ---- */

static void
test_rule_new(void)
{
	GowlRule *r;

	r = gowl_rule_new("firefox", NULL, 1 << 1, FALSE, -1);
	g_assert_nonnull(r);
	g_assert_cmpstr(r->app_id_pattern, ==, "firefox");
	g_assert_null(r->title_pattern);
	g_assert_cmpuint(r->tags, ==, 2);
	g_assert_false(r->floating);
	g_assert_cmpint(r->monitor, ==, -1);
	gowl_rule_free(r);
}

static void
test_rule_matches(void)
{
	GowlRule *r;

	r = gowl_rule_new("firefox", NULL, 0, FALSE, -1);
	g_assert_true(gowl_rule_matches(r, "firefox", "Some Page"));
	g_assert_false(gowl_rule_matches(r, "chromium", "Some Page"));
	gowl_rule_free(r);

	/* Wildcard pattern */
	r = gowl_rule_new("fire*", NULL, 0, FALSE, -1);
	g_assert_true(gowl_rule_matches(r, "firefox", "Any"));
	g_assert_true(gowl_rule_matches(r, "firebird", "Any"));
	g_assert_false(gowl_rule_matches(r, "chrome", "Any"));
	gowl_rule_free(r);

	/* Title pattern */
	r = gowl_rule_new(NULL, "*popup*", 0, TRUE, -1);
	g_assert_true(gowl_rule_matches(r, "anything", "a popup window"));
	g_assert_false(gowl_rule_matches(r, "anything", "main window"));
	gowl_rule_free(r);
}

/* ---- OutputMode tests ---- */

static void
test_output_mode_new(void)
{
	GowlOutputMode *m;

	m = gowl_output_mode_new(1920, 1080, 60000);
	g_assert_nonnull(m);
	g_assert_cmpint(m->width, ==, 1920);
	g_assert_cmpint(m->height, ==, 1080);
	g_assert_cmpint(m->refresh_mhz, ==, 60000);
	gowl_output_mode_free(m);
}

/* ---- BorderSpec tests ---- */

static void
test_border_spec_new(void)
{
	GowlColor fc = { 0.7, 0.7, 0.7, 1.0 };
	GowlColor uc = { 0.3, 0.3, 0.3, 1.0 };
	GowlColor urgc = { 1.0, 0.0, 0.0, 1.0 };
	GowlBorderSpec *bs;

	bs = gowl_border_spec_new(2, &fc, &uc, &urgc);
	g_assert_nonnull(bs);
	g_assert_cmpint(bs->width, ==, 2);
	g_assert_cmpfloat_with_epsilon(bs->focus_color.r, 0.7, 0.001);
	gowl_border_spec_free(bs);
}

/* ---- KeyCombo tests ---- */

static void
test_key_combo_new(void)
{
	GowlKeyCombo *kc;

	kc = gowl_key_combo_new(64 | 1, 0xff0d); /* Logo+Shift, Return */
	g_assert_nonnull(kc);
	g_assert_cmpuint(kc->modifiers, ==, 65);
	g_assert_cmpuint(kc->keysym, ==, 0xff0d);
	gowl_key_combo_free(kc);
}

static void
test_key_combo_equals(void)
{
	GowlKeyCombo *a;
	GowlKeyCombo *b;

	a = gowl_key_combo_new(64, 0xff0d);
	b = gowl_key_combo_new(64, 0xff0d);
	g_assert_true(gowl_key_combo_equals(a, b));

	b->keysym = 0xff0e;
	g_assert_false(gowl_key_combo_equals(a, b));

	gowl_key_combo_free(a);
	gowl_key_combo_free(b);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	/* Geometry */
	g_test_add_func("/boxed/geometry/new", test_geometry_new);
	g_test_add_func("/boxed/geometry/copy", test_geometry_copy);
	g_test_add_func("/boxed/geometry/contains", test_geometry_contains);
	g_test_add_func("/boxed/geometry/type", test_geometry_type);

	/* Color */
	g_test_add_func("/boxed/color/new", test_color_new);
	g_test_add_func("/boxed/color/from-hex", test_color_from_hex);
	g_test_add_func("/boxed/color/from-hex-alpha", test_color_from_hex_with_alpha);
	g_test_add_func("/boxed/color/to-hex", test_color_to_hex);
	g_test_add_func("/boxed/color/type", test_color_type);

	/* TagMask */
	g_test_add_func("/boxed/tag-mask/new", test_tag_mask_new);
	g_test_add_func("/boxed/tag-mask/operations", test_tag_mask_operations);
	g_test_add_func("/boxed/tag-mask/type", test_tag_mask_type);

	/* Gaps */
	g_test_add_func("/boxed/gaps/new", test_gaps_new);
	g_test_add_func("/boxed/gaps/equals", test_gaps_equals);

	/* Rule */
	g_test_add_func("/boxed/rule/new", test_rule_new);
	g_test_add_func("/boxed/rule/matches", test_rule_matches);

	/* OutputMode */
	g_test_add_func("/boxed/output-mode/new", test_output_mode_new);

	/* BorderSpec */
	g_test_add_func("/boxed/border-spec/new", test_border_spec_new);

	/* KeyCombo */
	g_test_add_func("/boxed/key-combo/new", test_key_combo_new);
	g_test_add_func("/boxed/key-combo/equals", test_key_combo_equals);

	return g_test_run();
}
