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

#include "core/gowl-bar.h"
#include "interfaces/gowl-bar-provider.h"
#include "module/gowl-module.h"
#include "module/gowl-module-manager.h"

/* ====================================================================
 * gowl_bar_tag_hit() — pure tag-indicator hit-test geometry
 *
 * Layout under test: tag boxes are @box_w (= bar height) wide, laid
 * out left to right starting at @pad.  A top bar occupies the band
 * [0, top_h); a bottom bar occupies [mon_h - bot_h, mon_h).
 * ==================================================================== */

/* Standard config: 1080-tall monitor, 28px top bar, 9 tags, pad 10.
   Boxes: tag N spans x in [10 + N*28, 10 + (N+1)*28). */
#define MON_H 1080
#define TOP   28
#define PAD   10
#define NTAGS 9

static void
test_hit_top_first_tag(void)
{
	/* Left edge of box 0, middle, and last pixel of box 0. */
	g_assert_cmpint(gowl_bar_tag_hit(10, 0, MON_H, TOP, 0, PAD, NTAGS),
	                ==, 0);
	g_assert_cmpint(gowl_bar_tag_hit(24, 14, MON_H, TOP, 0, PAD, NTAGS),
	                ==, 0);
	g_assert_cmpint(gowl_bar_tag_hit(37, 27, MON_H, TOP, 0, PAD, NTAGS),
	                ==, 0);
}

static void
test_hit_top_middle_tags(void)
{
	/* Box 1 starts at x=38; box 4 starts at x=10+4*28=122. */
	g_assert_cmpint(gowl_bar_tag_hit(38, 5, MON_H, TOP, 0, PAD, NTAGS),
	                ==, 1);
	g_assert_cmpint(gowl_bar_tag_hit(65, 5, MON_H, TOP, 0, PAD, NTAGS),
	                ==, 1);
	g_assert_cmpint(gowl_bar_tag_hit(122, 5, MON_H, TOP, 0, PAD, NTAGS),
	                ==, 4);
}

static void
test_hit_top_last_tag(void)
{
	/* Box 8 spans [10+8*28, 10+9*28) = [234, 262). */
	g_assert_cmpint(gowl_bar_tag_hit(234, 5, MON_H, TOP, 0, PAD, NTAGS),
	                ==, 8);
	g_assert_cmpint(gowl_bar_tag_hit(261, 5, MON_H, TOP, 0, PAD, NTAGS),
	                ==, 8);
}

static void
test_hit_left_of_padding(void)
{
	/* Anything left of @pad is title/empty space, not a tag. */
	g_assert_cmpint(gowl_bar_tag_hit(0, 5, MON_H, TOP, 0, PAD, NTAGS),
	                ==, -1);
	g_assert_cmpint(gowl_bar_tag_hit(9, 5, MON_H, TOP, 0, PAD, NTAGS),
	                ==, -1);
}

static void
test_hit_past_last_tag(void)
{
	/* x == pad + NTAGS*box_w is the first pixel past box 8. */
	g_assert_cmpint(gowl_bar_tag_hit(262, 5, MON_H, TOP, 0, PAD, NTAGS),
	                ==, -1);
	g_assert_cmpint(gowl_bar_tag_hit(900, 5, MON_H, TOP, 0, PAD, NTAGS),
	                ==, -1);
}

static void
test_hit_wrong_vertical_band(void)
{
	/* y at/after the bar height, negative y, and mid-screen all miss. */
	g_assert_cmpint(gowl_bar_tag_hit(24, 28, MON_H, TOP, 0, PAD, NTAGS),
	                ==, -1);
	g_assert_cmpint(gowl_bar_tag_hit(24, -1, MON_H, TOP, 0, PAD, NTAGS),
	                ==, -1);
	g_assert_cmpint(gowl_bar_tag_hit(24, 540, MON_H, TOP, 0, PAD, NTAGS),
	                ==, -1);
}

static void
test_hit_no_bars(void)
{
	/* No top and no bottom tag bar: every point misses. */
	g_assert_cmpint(gowl_bar_tag_hit(24, 0, MON_H, 0, 0, PAD, NTAGS),
	                ==, -1);
	g_assert_cmpint(gowl_bar_tag_hit(24, 1079, MON_H, 0, 0, PAD, NTAGS),
	                ==, -1);
}

static void
test_hit_bottom_bar(void)
{
	/* Bottom bar of 24px: band is [1056, 1080), box_w = 24.
	   Box 0 spans x in [10, 34). */
	g_assert_cmpint(gowl_bar_tag_hit(10, 1056, MON_H, 0, 24, PAD, NTAGS),
	                ==, 0);
	g_assert_cmpint(gowl_bar_tag_hit(20, 1079, MON_H, 0, 24, PAD, NTAGS),
	                ==, 0);
	/* Box 2 spans [10+48, 10+72) = [58, 82). */
	g_assert_cmpint(gowl_bar_tag_hit(60, 1070, MON_H, 0, 24, PAD, NTAGS),
	                ==, 2);
	/* Just above the bottom band misses. */
	g_assert_cmpint(gowl_bar_tag_hit(20, 1055, MON_H, 0, 24, PAD, NTAGS),
	                ==, -1);
	/* Off the bottom edge misses. */
	g_assert_cmpint(gowl_bar_tag_hit(20, 1080, MON_H, 0, 24, PAD, NTAGS),
	                ==, -1);
}

static void
test_hit_both_bars_select_correct_box_width(void)
{
	/* Top 28px and bottom 40px coexist.  The matched slot's height is
	   the box width, so the same x maps to different tags per band. */
	gint top_tag, bot_tag;

	/* x = 100: top box_w 28 → (100-10)/28 = 3; bottom box_w 40 →
	   (100-10)/40 = 2. */
	top_tag = gowl_bar_tag_hit(100, 0, MON_H, 28, 40, PAD, NTAGS);
	bot_tag = gowl_bar_tag_hit(100, 1079, MON_H, 28, 40, PAD, NTAGS);
	g_assert_cmpint(top_tag, ==, 3);
	g_assert_cmpint(bot_tag, ==, 2);
}

static void
test_hit_tag_count_clamps(void)
{
	/* With fewer tags, a click past the last box misses even though it
	   would land inside a box if more tags existed. */
	/* 3 tags: boxes span [10, 94); x=100 is past them. */
	g_assert_cmpint(gowl_bar_tag_hit(100, 5, MON_H, TOP, 0, PAD, 3),
	                ==, -1);
	/* x=80 is inside box 2 ([66,94)). */
	g_assert_cmpint(gowl_bar_tag_hit(80, 5, MON_H, TOP, 0, PAD, 3),
	                ==, 2);
	/* Zero tags: always -1. */
	g_assert_cmpint(gowl_bar_tag_hit(24, 5, MON_H, TOP, 0, PAD, 0),
	                ==, -1);
}

static void
test_hit_zero_padding(void)
{
	/* pad = 0: box 0 starts at the very left edge. */
	g_assert_cmpint(gowl_bar_tag_hit(0, 5, MON_H, TOP, 0, 0, NTAGS),
	                ==, 0);
	g_assert_cmpint(gowl_bar_tag_hit(28, 5, MON_H, TOP, 0, 0, NTAGS),
	                ==, 1);
	/* Negative x still misses. */
	g_assert_cmpint(gowl_bar_tag_hit(-1, 5, MON_H, TOP, 0, 0, NTAGS),
	                ==, -1);
}

/* ====================================================================
 * Mock GowlBarProvider modules for the dispatch tests
 * ==================================================================== */

/* A provider whose tag_at returns a stored value (default -1). */
#define TEST_TYPE_BAR_PROVIDER (test_bar_provider_get_type())
G_DECLARE_FINAL_TYPE(TestBarProvider, test_bar_provider,
                     TEST, BAR_PROVIDER, GowlModule)

struct _TestBarProvider {
	GowlModule parent_instance;
	gint       tag_result;
};

static void test_bar_provider_iface_init(GowlBarProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(TestBarProvider, test_bar_provider, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_BAR_PROVIDER,
		test_bar_provider_iface_init))

static gint
test_bar_provider_tag_at(GowlBarProvider *p, gpointer monitor,
                         gint x, gint y)
{
	(void)monitor; (void)x; (void)y;
	return TEST_BAR_PROVIDER(p)->tag_result;
}

static const gchar *
test_bar_provider_get_name(GowlModule *mod)
{
	(void)mod;
	return "test-bar-provider";
}

static void
test_bar_provider_iface_init(GowlBarProviderInterface *iface)
{
	iface->tag_at = test_bar_provider_tag_at;
}

/* Shared module vfuncs.  The base activate returns FALSE, so mocks
   must override it for gowl_module_manager_activate_all() to mark them
   active (and thus visible to the bar_tag_at dispatch). */
static gboolean
test_bar_module_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
test_bar_module_deactivate(GowlModule *mod)
{
	(void)mod;
}

static void
test_bar_provider_class_init(TestBarProviderClass *klass)
{
	GowlModuleClass *mc = GOWL_MODULE_CLASS(klass);

	mc->get_name   = test_bar_provider_get_name;
	mc->activate   = test_bar_module_activate;
	mc->deactivate = test_bar_module_deactivate;
}

static void
test_bar_provider_init(TestBarProvider *self)
{
	self->tag_result = -1;
}

/* A provider that implements GowlBarProvider but NOT tag_at, to verify
   the dispatcher returns -1 for the unimplemented optional method. */
#define TEST_TYPE_BAR_PLAIN (test_bar_plain_get_type())
G_DECLARE_FINAL_TYPE(TestBarPlain, test_bar_plain,
                     TEST, BAR_PLAIN, GowlModule)

struct _TestBarPlain {
	GowlModule parent_instance;
};

static void test_bar_plain_iface_init(GowlBarProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(TestBarPlain, test_bar_plain, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_BAR_PROVIDER,
		test_bar_plain_iface_init))

static const gchar *
test_bar_plain_get_name(GowlModule *mod)
{
	(void)mod;
	return "test-bar-plain";
}

static void
test_bar_plain_iface_init(GowlBarProviderInterface *iface)
{
	/* Intentionally leaves iface->tag_at == NULL. */
	(void)iface;
}

static void
test_bar_plain_class_init(TestBarPlainClass *klass)
{
	GowlModuleClass *mc = GOWL_MODULE_CLASS(klass);

	mc->get_name   = test_bar_plain_get_name;
	mc->activate   = test_bar_module_activate;
	mc->deactivate = test_bar_module_deactivate;
}

static void
test_bar_plain_init(TestBarPlain *self)
{
	(void)self;
}

/* ---- Interface dispatcher: gowl_bar_provider_tag_at() ---- */

static void
test_dispatch_forwards_to_impl(void)
{
	TestBarProvider *p = g_object_new(TEST_TYPE_BAR_PROVIDER, NULL);

	p->tag_result = 7;
	g_assert_cmpint(
		gowl_bar_provider_tag_at(GOWL_BAR_PROVIDER(p), NULL, 0, 0),
		==, 7);

	p->tag_result = -1;
	g_assert_cmpint(
		gowl_bar_provider_tag_at(GOWL_BAR_PROVIDER(p), NULL, 0, 0),
		==, -1);

	g_object_unref(p);
}

static void
test_dispatch_missing_method(void)
{
	TestBarPlain *p = g_object_new(TEST_TYPE_BAR_PLAIN, NULL);

	/* No tag_at implementation → dispatcher returns -1. */
	g_assert_cmpint(
		gowl_bar_provider_tag_at(GOWL_BAR_PROVIDER(p), NULL, 0, 0),
		==, -1);

	g_object_unref(p);
}

/* ---- Manager fan-out: gowl_module_manager_bar_tag_at() ---- */

static TestBarProvider *
find_bar_provider(GowlModuleManager *mgr)
{
	GowlModule *m;

	/* get_modules() returns GowlModuleInfo boxes, not instances, so we
	   look the live module up by name instead. */
	m = gowl_module_manager_find_module(mgr, "test-bar-provider");
	return m != NULL ? TEST_BAR_PROVIDER(m) : NULL;
}

static void
test_manager_no_providers(void)
{
	GowlModuleManager *mgr = gowl_module_manager_new();

	g_assert_cmpint(gowl_module_manager_bar_tag_at(mgr, NULL, 24, 5),
	                ==, -1);
	g_object_unref(mgr);
}

static void
test_manager_active_provider(void)
{
	GowlModuleManager *mgr = gowl_module_manager_new();
	TestBarProvider *p;

	gowl_module_manager_register(mgr, TEST_TYPE_BAR_PROVIDER, NULL);
	p = find_bar_provider(mgr);
	g_assert_nonnull(p);
	p->tag_result = 5;

	/* Inactive providers are skipped → -1. */
	g_assert_cmpint(gowl_module_manager_bar_tag_at(mgr, NULL, 24, 5),
	                ==, -1);

	/* Once active, its hit is returned. */
	gowl_module_manager_activate_all(mgr);
	g_assert_cmpint(gowl_module_manager_bar_tag_at(mgr, NULL, 24, 5),
	                ==, 5);

	gowl_module_manager_deactivate_all(mgr);
	g_object_unref(mgr);
}

static void
test_manager_miss_provider(void)
{
	GowlModuleManager *mgr = gowl_module_manager_new();
	TestBarProvider *p;

	gowl_module_manager_register(mgr, TEST_TYPE_BAR_PROVIDER, NULL);
	p = find_bar_provider(mgr);
	p->tag_result = -1;   /* always misses */
	gowl_module_manager_activate_all(mgr);

	g_assert_cmpint(gowl_module_manager_bar_tag_at(mgr, NULL, 24, 5),
	                ==, -1);

	gowl_module_manager_deactivate_all(mgr);
	g_object_unref(mgr);
}

static void
test_manager_skips_to_hit(void)
{
	/* A plain provider (no tag_at → always -1) plus a hitting provider:
	   the fan-out skips the miss and returns the hit, regardless of the
	   order the two providers are dispatched in. */
	GowlModuleManager *mgr = gowl_module_manager_new();
	TestBarProvider *p;

	gowl_module_manager_register(mgr, TEST_TYPE_BAR_PLAIN, NULL);
	gowl_module_manager_register(mgr, TEST_TYPE_BAR_PROVIDER, NULL);
	p = find_bar_provider(mgr);
	g_assert_nonnull(p);
	p->tag_result = 6;
	gowl_module_manager_activate_all(mgr);

	g_assert_cmpint(gowl_module_manager_bar_tag_at(mgr, NULL, 24, 5),
	                ==, 6);

	gowl_module_manager_deactivate_all(mgr);
	g_object_unref(mgr);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	/* Pure hit-test geometry. */
	g_test_add_func("/bar/hit/top-first-tag", test_hit_top_first_tag);
	g_test_add_func("/bar/hit/top-middle-tags", test_hit_top_middle_tags);
	g_test_add_func("/bar/hit/top-last-tag", test_hit_top_last_tag);
	g_test_add_func("/bar/hit/left-of-padding", test_hit_left_of_padding);
	g_test_add_func("/bar/hit/past-last-tag", test_hit_past_last_tag);
	g_test_add_func("/bar/hit/wrong-band", test_hit_wrong_vertical_band);
	g_test_add_func("/bar/hit/no-bars", test_hit_no_bars);
	g_test_add_func("/bar/hit/bottom-bar", test_hit_bottom_bar);
	g_test_add_func("/bar/hit/both-bars-box-width",
	                test_hit_both_bars_select_correct_box_width);
	g_test_add_func("/bar/hit/tag-count-clamps", test_hit_tag_count_clamps);
	g_test_add_func("/bar/hit/zero-padding", test_hit_zero_padding);

	/* Provider interface dispatcher. */
	g_test_add_func("/bar/provider/forwards", test_dispatch_forwards_to_impl);
	g_test_add_func("/bar/provider/missing-method",
	                test_dispatch_missing_method);

	/* Module-manager fan-out. */
	g_test_add_func("/bar/manager/no-providers", test_manager_no_providers);
	g_test_add_func("/bar/manager/active-provider",
	                test_manager_active_provider);
	g_test_add_func("/bar/manager/miss-provider", test_manager_miss_provider);
	g_test_add_func("/bar/manager/skips-to-hit", test_manager_skips_to_hit);

	return g_test_run();
}
