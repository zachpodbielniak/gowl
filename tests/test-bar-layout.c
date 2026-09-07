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
 * The bar's three-region layout.  Every case here is one that looks
 * fine in a screenshot taken at one window size and wrong at another:
 * a centre group that drifts, an anchor that is not actually centred,
 * a right-hand list that runs off the edge.  They are cheap to assert
 * and expensive to notice by eye.
 */

#include <stdarg.h>

#include "barkit/gowl-bar-layout.h"

#define BAR_W 1000
#define PAD   8
#define GAP   6

static void
test_regions_do_not_overlap(void)
{
	gint widths[] = { 100, 100, 120, 90, 90 };
	GowlBarRegion regions[] = {
		GOWL_BAR_REGION_LEFT, GOWL_BAR_REGION_LEFT,
		GOWL_BAR_REGION_CENTER,
		GOWL_BAR_REGION_RIGHT, GOWL_BAR_REGION_RIGHT
	};
	GowlBarSlot slots[5];
	gint visible, i, j;

	visible = gowl_bar_layout_run(BAR_W, PAD, GAP, widths, regions, 5,
	                              -1, slots);
	g_assert_cmpint(visible, ==, 5);

	for (i = 0; i < 5; i++) {
		g_assert_true(slots[i].visible);
		g_assert_cmpint(slots[i].x, >=, 0);
		g_assert_cmpint(slots[i].x + slots[i].width, <=, BAR_W);

		for (j = i + 1; j < 5; j++) {
			gboolean disjoint;

			disjoint =
				(slots[i].x + slots[i].width <= slots[j].x) ||
				(slots[j].x + slots[j].width <= slots[i].x);
			g_assert_true(disjoint);
		}
	}
}

static void
test_left_packs_from_the_left_edge(void)
{
	gint widths[] = { 60, 40 };
	GowlBarRegion regions[] = { GOWL_BAR_REGION_LEFT,
	                            GOWL_BAR_REGION_LEFT };
	GowlBarSlot slots[2];

	gowl_bar_layout_run(BAR_W, PAD, GAP, widths, regions, 2, -1, slots);

	g_assert_cmpint(slots[0].x, ==, PAD);
	g_assert_cmpint(slots[1].x, ==, PAD + 60 + GAP);
}

static void
test_right_list_reads_outward(void)
{
	gint widths[] = { 60, 40 };
	GowlBarRegion regions[] = { GOWL_BAR_REGION_RIGHT,
	                            GOWL_BAR_REGION_RIGHT };
	GowlBarSlot slots[2];

	gowl_bar_layout_run(BAR_W, PAD, GAP, widths, regions, 2, -1, slots);

	/* The first entry in the config sits furthest right, the way a
	   dwm-descended status list has always read. */
	g_assert_cmpint(slots[0].x + slots[0].width, ==, BAR_W - PAD);
	g_assert_cmpint(slots[1].x + slots[1].width, ==,
	                slots[0].x - GAP);
}

static void
test_centre_group_is_centred(void)
{
	gint widths[] = { 100, 100 };
	GowlBarRegion regions[] = { GOWL_BAR_REGION_CENTER,
	                            GOWL_BAR_REGION_CENTER };
	GowlBarSlot slots[2];
	gint group_start, group_end, group_mid;

	gowl_bar_layout_run(BAR_W, PAD, GAP, widths, regions, 2, -1, slots);

	group_start = slots[0].x;
	group_end   = slots[1].x + slots[1].width;
	group_mid   = (group_start + group_end) / 2;

	g_assert_cmpint(ABS(group_mid - BAR_W / 2), <=, 1);
}

static void
test_anchor_is_dead_centre_not_the_group(void)
{
	/* The point of an anchor: the clock stays put no matter what
	   appears beside it.  A centred *group* would slide the clock
	   left as soon as something was added to its right. */
	gint widths[] = { 200, 100, 40 };
	GowlBarRegion regions[] = {
		GOWL_BAR_REGION_CENTER, GOWL_BAR_REGION_CENTER,
		GOWL_BAR_REGION_CENTER
	};
	GowlBarSlot slots[3];
	gint anchor_mid;

	gowl_bar_layout_run(BAR_W, PAD, GAP, widths, regions, 3, 1, slots);

	anchor_mid = slots[1].x + slots[1].width / 2;
	g_assert_cmpint(ABS(anchor_mid - BAR_W / 2), <=, 1);

	/* And the neighbours line up either side of it. */
	g_assert_cmpint(slots[0].x + slots[0].width + GAP, ==, slots[1].x);
	g_assert_cmpint(slots[1].x + slots[1].width + GAP, ==, slots[2].x);
}

static void
test_anchor_stays_put_when_a_neighbour_appears(void)
{
	gint widths_before[] = { 100 };
	gint widths_after[]  = { 100, 300 };
	GowlBarRegion regions[] = { GOWL_BAR_REGION_CENTER,
	                            GOWL_BAR_REGION_CENTER };
	GowlBarSlot before[1], after[2];

	gowl_bar_layout_run(BAR_W, PAD, GAP, widths_before, regions, 1, 0,
	                    before);
	gowl_bar_layout_run(BAR_W, PAD, GAP, widths_after, regions, 2, 0,
	                    after);

	g_assert_cmpint(before[0].x, ==, after[0].x);
}

static void
test_centre_shifts_rather_than_overlapping(void)
{
	/* A long window title on the left must push the centre aside, not
	   be drawn over by it. */
	gint widths[] = { 600, 200, 100 };
	GowlBarRegion regions[] = {
		GOWL_BAR_REGION_LEFT, GOWL_BAR_REGION_CENTER,
		GOWL_BAR_REGION_RIGHT
	};
	GowlBarSlot slots[3];

	gowl_bar_layout_run(BAR_W, PAD, GAP, widths, regions, 3, -1, slots);

	g_assert_true(slots[1].visible);
	g_assert_cmpint(slots[1].x, >=, slots[0].x + slots[0].width);
	g_assert_cmpint(slots[1].x + slots[1].width, <=, slots[2].x);
}

static void
test_zero_width_items_leave_no_gap(void)
{
	/* A plugin with nothing to show measures zero; it must not
	   reserve a gap, or the bar develops holes as widgets go quiet. */
	gint widths[] = { 50, 0, 50 };
	GowlBarRegion regions[] = {
		GOWL_BAR_REGION_LEFT, GOWL_BAR_REGION_LEFT,
		GOWL_BAR_REGION_LEFT
	};
	GowlBarSlot slots[3];
	gint visible;

	visible = gowl_bar_layout_run(BAR_W, PAD, GAP, widths, regions, 3,
	                              -1, slots);

	g_assert_cmpint(visible, ==, 2);
	g_assert_false(slots[1].visible);
	g_assert_cmpint(slots[2].x, ==, PAD + 50 + GAP);
}

static void
test_hit_testing_matches_the_layout(void)
{
	gint widths[] = { 60, 40, 80 };
	GowlBarRegion regions[] = {
		GOWL_BAR_REGION_LEFT, GOWL_BAR_REGION_CENTER,
		GOWL_BAR_REGION_RIGHT
	};
	GowlBarSlot slots[3];
	gint i;

	gowl_bar_layout_run(BAR_W, PAD, GAP, widths, regions, 3, -1, slots);

	for (i = 0; i < 3; i++) {
		g_assert_cmpint(gowl_bar_layout_hit(slots, 3, slots[i].x),
		                ==, i);
		g_assert_cmpint(gowl_bar_layout_hit(slots, 3,
			slots[i].x + slots[i].width - 1), ==, i);
		/* One past the right edge belongs to nobody, or to the
		   next widget -- never still to this one. */
		g_assert_cmpint(gowl_bar_layout_hit(slots, 3,
			slots[i].x + slots[i].width), !=, i);
	}

	g_assert_cmpint(gowl_bar_layout_hit(slots, 3, 0), ==, -1);
}

static void
test_region_names_round_trip(void)
{
	GowlBarRegion region;

	g_assert_true(gowl_bar_layout_region_from_string("left", &region));
	g_assert_cmpint(region, ==, GOWL_BAR_REGION_LEFT);
	g_assert_true(gowl_bar_layout_region_from_string("CENTER", &region));
	g_assert_cmpint(region, ==, GOWL_BAR_REGION_CENTER);
	/* Both spellings, because the config is read by a person. */
	g_assert_true(gowl_bar_layout_region_from_string("centre", &region));
	g_assert_cmpint(region, ==, GOWL_BAR_REGION_CENTER);
	g_assert_true(gowl_bar_layout_region_from_string("right", &region));
	g_assert_cmpint(region, ==, GOWL_BAR_REGION_RIGHT);

	g_assert_false(gowl_bar_layout_region_from_string("elsewhere",
	                                                  &region));
	g_assert_cmpstr(gowl_bar_layout_region_name(GOWL_BAR_REGION_CENTER),
	                ==, "center");
}

static void
test_a_bar_too_narrow_drops_the_centre(void)
{
	/* Nothing may be drawn on top of anything else, even when there
	   is genuinely no room. */
	gint widths[] = { 400, 400, 400 };
	GowlBarRegion regions[] = {
		GOWL_BAR_REGION_LEFT, GOWL_BAR_REGION_CENTER,
		GOWL_BAR_REGION_RIGHT
	};
	GowlBarSlot slots[3];

	gowl_bar_layout_run(500, PAD, GAP, widths, regions, 3, -1, slots);

	g_assert_false(slots[1].visible);
}

/* ---- Which era a configuration was written for ---- */

static GHashTable *
settings_new(const gchar *first_key, ...)
{
	GHashTable *table;
	va_list args;
	const gchar *key;

	table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                              g_free);
	if (first_key == NULL)
		return table;

	va_start(args, first_key);
	key = first_key;
	while (key != NULL) {
		const gchar *value = va_arg(args, const gchar *);

		g_hash_table_insert(table, g_strdup(key), g_strdup(value));
		key = va_arg(args, const gchar *);
	}
	va_end(args);
	return table;
}

static void
test_config_kind(void)
{
	g_autoptr(GHashTable) none = NULL;
	g_autoptr(GHashTable) legacy = NULL;
	g_autoptr(GHashTable) regions = NULL;
	g_autoptr(GHashTable) centre = NULL;
	g_autoptr(GHashTable) both = NULL;

	/*
	 * This is the duplicate-clock regression.  The bar ships a layout
	 * with a clock in the centre; a configuration written before
	 * regions puts its own clock at the end of `widgets'.  Unless the
	 * shipped layout is recognised as replaceable, the bar shows the
	 * time twice -- which is exactly what happened on the first
	 * upgrade.
	 */
	g_assert_cmpint(gowl_bar_layout_config_kind(NULL), ==,
	                GOWL_BAR_CONFIG_NONE);

	none = settings_new("height", "30", "bg-color", "base", NULL);
	g_assert_cmpint(gowl_bar_layout_config_kind(none), ==,
	                GOWL_BAR_CONFIG_NONE);

	legacy = settings_new("widgets", "cpu memory disk battery clock",
	                      NULL);
	g_assert_cmpint(gowl_bar_layout_config_kind(legacy), ==,
	                GOWL_BAR_CONFIG_LEGACY);

	regions = settings_new("widgets-right", "cpu memory", NULL);
	g_assert_cmpint(gowl_bar_layout_config_kind(regions), ==,
	                GOWL_BAR_CONFIG_REGIONS);

	/* Both spellings of the middle. */
	centre = settings_new("widgets-centre", "clock", NULL);
	g_assert_cmpint(gowl_bar_layout_config_kind(centre), ==,
	                GOWL_BAR_CONFIG_REGIONS);

	/* A config carrying both is one mid-migration; the region keys
	   are the half that was updated deliberately. */
	both = settings_new("widgets", "cpu clock",
	                    "widgets-center", "clock", NULL);
	g_assert_cmpint(gowl_bar_layout_config_kind(both), ==,
	                GOWL_BAR_CONFIG_REGIONS);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-layout/regions-do-not-overlap",
	                test_regions_do_not_overlap);
	g_test_add_func("/bar-layout/left-packs-from-left",
	                test_left_packs_from_the_left_edge);
	g_test_add_func("/bar-layout/right-reads-outward",
	                test_right_list_reads_outward);
	g_test_add_func("/bar-layout/centre-group-centred",
	                test_centre_group_is_centred);
	g_test_add_func("/bar-layout/anchor-dead-centre",
	                test_anchor_is_dead_centre_not_the_group);
	g_test_add_func("/bar-layout/anchor-stays-put",
	                test_anchor_stays_put_when_a_neighbour_appears);
	g_test_add_func("/bar-layout/centre-shifts",
	                test_centre_shifts_rather_than_overlapping);
	g_test_add_func("/bar-layout/zero-width-no-gap",
	                test_zero_width_items_leave_no_gap);
	g_test_add_func("/bar-layout/hit-matches-layout",
	                test_hit_testing_matches_the_layout);
	g_test_add_func("/bar-layout/region-names", test_region_names_round_trip);
	g_test_add_func("/bar-layout/narrow-drops-centre",
	                test_a_bar_too_narrow_drops_the_centre);
	g_test_add_func("/bar-layout/config-kind", test_config_kind);

	return g_test_run();
}
