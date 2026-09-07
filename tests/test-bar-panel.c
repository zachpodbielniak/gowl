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
 * The panel model and its renderer.
 *
 * The measure/render agreement is the load-bearing assertion here: the
 * two share a walk precisely so they cannot disagree, and a regression
 * that split them would show up as a panel that clips its last row or
 * leaves a gap under it -- visible only if you happen to open the one
 * panel long enough to notice.
 */

#include <cairo.h>
#include <pango/pangocairo.h>

#include "barkit/gowl-bar-panel.h"
#include "barkit/gowl-bar-panel-render.h"
#include "barkit/gowl-bar-theme.h"

#define PANEL_W 400

typedef struct {
	cairo_surface_t *surface;
	cairo_t         *cr;
	PangoLayout     *layout;
	GowlBarTheme    *theme;
} Fixture;

static void
fixture_init(Fixture *f)
{
	f->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
	                                        PANEL_W, 900);
	f->cr      = cairo_create(f->surface);
	f->layout  = pango_cairo_create_layout(f->cr);
	f->theme   = gowl_bar_theme_new();
}

static void
fixture_clear(Fixture *f)
{
	g_object_unref(f->layout);
	cairo_destroy(f->cr);
	cairo_surface_destroy(f->surface);
	gowl_bar_theme_free(f->theme);
}

/* ---- Model ---- */

static void
test_item_defaults_are_neutral(void)
{
	GowlBarPanelItem *item;

	item = gowl_bar_panel_item_new(GOWL_BAR_ITEM_ROW);
	g_assert_cmpint(gowl_bar_panel_item_get_kind(item), ==,
	                GOWL_BAR_ITEM_ROW);
	g_assert_null(gowl_bar_panel_item_get_id(item));
	g_assert_false(gowl_bar_panel_item_has_color(item));
	g_assert_false(gowl_bar_panel_item_get_active(item));
	g_assert_cmpfloat(gowl_bar_panel_item_get_fraction(item), ==, 0.0);
	gowl_bar_panel_item_free(item);
}

static void
test_fraction_is_clamped(void)
{
	GowlBarPanelItem *item;

	/* A plugin computing a fraction from a reading it does not
	   control will overshoot; a slider drawn past its own track is
	   worse than one pinned at the end. */
	item = gowl_bar_panel_item_new(GOWL_BAR_ITEM_SLIDER);
	gowl_bar_panel_item_set_fraction(item, 1.8);
	g_assert_cmpfloat(gowl_bar_panel_item_get_fraction(item), ==, 1.0);
	gowl_bar_panel_item_set_fraction(item, -0.5);
	g_assert_cmpfloat(gowl_bar_panel_item_get_fraction(item), ==, 0.0);
	gowl_bar_panel_item_free(item);
}

static void
test_item_copy_is_deep(void)
{
	GowlBarPanelItem *item, *copy;
	gdouble samples[3] = { 0.1, 0.5, 0.9 };
	guint n;

	item = gowl_bar_panel_item_new(GOWL_BAR_ITEM_GRAPH);
	gowl_bar_panel_item_set_title(item, "original");
	gowl_bar_panel_item_set_prop(item, "ssid", "somewhere");
	gowl_bar_panel_item_set_samples(item, samples, 3);
	gowl_bar_panel_item_add_child(item,
		gowl_bar_panel_item_new(GOWL_BAR_ITEM_LABEL));

	copy = gowl_bar_panel_item_copy(item);
	gowl_bar_panel_item_set_title(item, "changed");

	g_assert_cmpstr(gowl_bar_panel_item_get_title(copy), ==, "original");
	g_assert_cmpstr(gowl_bar_panel_item_get_prop(copy, "ssid"), ==,
	                "somewhere");
	g_assert_cmpuint(gowl_bar_panel_item_n_children(copy), ==, 1);
	g_assert_nonnull(gowl_bar_panel_item_get_samples(copy, &n));
	g_assert_cmpuint(n, ==, 3);

	gowl_bar_panel_item_free(item);
	gowl_bar_panel_item_free(copy);
}

static void
test_find_by_id(void)
{
	GowlBarPanel *panel;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_add_label(panel, "unrelated");
	gowl_bar_panel_add_toggle(panel, "wifi", "Wi-Fi", TRUE);

	g_assert_nonnull(gowl_bar_panel_find(panel, "wifi"));
	g_assert_null(gowl_bar_panel_find(panel, "bluetooth"));
	g_assert_cmpuint(gowl_bar_panel_n_items(panel), ==, 2);

	g_object_unref(panel);
}

/* ---- Rendering ---- */

/* Build one panel using every item kind, so a kind the measure pass
   forgets is caught here rather than by somebody opening that panel. */
static GowlBarPanel *
build_kitchen_sink(void)
{
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	gdouble samples[8] = { 0.1, 0.4, 0.2, 0.9, 0.5, 0.3, 0.7, 0.6 };
	gint i;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, PANEL_W);

	item = gowl_bar_panel_add_hero(panel, "\xef\x87\xab", "Network",
	                               "Routing crumbs");
	gowl_bar_panel_item_set_id(item, "power");
	gowl_bar_panel_item_set_active(item, TRUE);

	gowl_bar_panel_add_section(panel, "Readings");
	gowl_bar_panel_add_field(panel, "Ping", "9.8 ms");
	gowl_bar_panel_add_field_pair(panel, "Down", "3.2 KB/s", "Up",
	                              "834 B/s");
	gowl_bar_panel_add_separator(panel);
	gowl_bar_panel_add_progress(panel, "Signal", 0.72);
	gowl_bar_panel_add_graph(panel, "Throughput", samples, 8);
	gowl_bar_panel_add_slider(panel, "volume", "Output", 0.4);
	gowl_bar_panel_add_toggle(panel, "mute", "Muted", FALSE);
	gowl_bar_panel_add_row(panel, "net-1", "\xef\x87\xab", "Pumpkin",
	                       "Connected");
	gowl_bar_panel_add_label(panel, "A plain line");
	/* Long enough to wrap, so the measure/render agreement below
	   covers a paragraph rather than only single lines. */
	gowl_bar_panel_add_label(panel,
		"Bringing a device up usually needs root. Granting your "
		"user the operator role does that once, and then joining "
		"works from here on its own.");
	gowl_bar_panel_add_spacer(panel, 12);

	item = gowl_bar_panel_add_buttons(panel, "dns");
	gowl_bar_panel_add_button(item, "DHCP", TRUE);
	gowl_bar_panel_add_button(item, "Cloudflare", FALSE);
	gowl_bar_panel_add_button(item, "Google", FALSE);

	item = gowl_bar_panel_item_new(GOWL_BAR_ITEM_CALENDAR);
	gowl_bar_panel_item_set_id(item, "day");
	for (i = 0; i < 8 * 7; i++) {
		GowlBarPanelItem *cell;
		gchar buf[8];

		g_snprintf(buf, sizeof(buf), "%d", i);
		cell = gowl_bar_panel_item_new(GOWL_BAR_ITEM_LABEL);
		gowl_bar_panel_item_set_title(cell, buf);
		gowl_bar_panel_item_add_child(item, cell);
	}
	gowl_bar_panel_add(panel, item);

	return panel;
}

static void
test_measure_matches_render(void)
{
	Fixture f;
	GowlBarPanel *panel;
	GowlBarPanelRenderCtx ctx;
	gint measured;

	fixture_init(&f);
	panel = build_kitchen_sink();

	measured = gowl_bar_panel_measure(panel, f.layout, f.theme, PANEL_W);
	g_assert_cmpint(measured, >, 0);

	gowl_bar_panel_render_ctx_init(&ctx, PANEL_W);
	ctx.hits = g_array_new(FALSE, FALSE, sizeof(GowlBarHitRect));
	gowl_bar_panel_render(panel, f.cr, f.layout, f.theme, &ctx);

	g_assert_cmpint(ctx.content_height, ==, measured);

	g_array_unref(ctx.hits);
	g_object_unref(panel);
	fixture_clear(&f);
}

static void
test_render_leaves_the_model_untouched(void)
{
	Fixture f;
	GowlBarPanel *panel;
	GowlBarPanelRenderCtx ctx;
	GowlBarPanelItem *row;

	fixture_init(&f);
	panel = build_kitchen_sink();
	row = gowl_bar_panel_find(panel, "net-1");
	g_assert_nonnull(row);
	g_assert_false(gowl_bar_panel_item_get_selected(row));

	/* The keyboard cursor is expressed by temporarily marking an item
	   selected during the draw.  The panel belongs to the plugin, so
	   it must come back exactly as it went in. */
	gowl_bar_panel_render_ctx_init(&ctx, PANEL_W);
	ctx.hits = g_array_new(FALSE, FALSE, sizeof(GowlBarHitRect));
	ctx.focus_item = 8;
	gowl_bar_panel_render(panel, f.cr, f.layout, f.theme, &ctx);

	g_assert_false(gowl_bar_panel_item_get_selected(row));

	g_array_unref(ctx.hits);
	g_object_unref(panel);
	fixture_clear(&f);
}

static void
test_only_interactive_items_are_hit_testable(void)
{
	Fixture f;
	GowlBarPanel *panel;
	GowlBarPanelRenderCtx ctx;
	guint i;
	gboolean saw_slider, saw_button, saw_row;

	fixture_init(&f);
	panel = build_kitchen_sink();

	gowl_bar_panel_render_ctx_init(&ctx, PANEL_W);
	ctx.hits = g_array_new(FALSE, FALSE, sizeof(GowlBarHitRect));
	gowl_bar_panel_render(panel, f.cr, f.layout, f.theme, &ctx);

	g_assert_cmpuint(ctx.hits->len, >, 0);

	saw_slider = saw_button = saw_row = FALSE;
	for (i = 0; i < ctx.hits->len; i++) {
		GowlBarHitRect *r;

		r = &g_array_index(ctx.hits, GowlBarHitRect, i);
		/* A region with no id could never be acted on, so emitting
		   one would only make the panel swallow clicks. */
		g_assert_nonnull(r->id);
		g_assert_cmpint(r->width, >, 0);
		g_assert_cmpint(r->height, >, 0);

		if (r->kind == GOWL_BAR_ITEM_SLIDER)
			saw_slider = TRUE;
		if (r->kind == GOWL_BAR_ITEM_BUTTONS) {
			saw_button = TRUE;
			g_assert_cmpint(r->child_index, >=, 0);
		}
		if (r->kind == GOWL_BAR_ITEM_ROW)
			saw_row = TRUE;
	}
	g_assert_true(saw_slider);
	g_assert_true(saw_button);
	g_assert_true(saw_row);

	g_array_unref(ctx.hits);
	g_object_unref(panel);
	fixture_clear(&f);
}

static void
test_hit_find_prefers_the_topmost(void)
{
	GArray *hits;
	GowlBarHitRect a, b;

	hits = g_array_new(FALSE, FALSE, sizeof(GowlBarHitRect));

	a.item_index = 0; a.child_index = -1; a.kind = GOWL_BAR_ITEM_ROW;
	a.id = "under"; a.x = 0; a.y = 0; a.width = 100; a.height = 100;
	b = a;
	b.item_index = 1;
	b.id = "over";
	b.x = 40; b.y = 40; b.width = 20; b.height = 20;

	g_array_append_val(hits, a);
	g_array_append_val(hits, b);

	/* Later regions are drawn later, so they win -- a button inside a
	   row has to be reachable. */
	g_assert_cmpint(gowl_bar_hit_find(hits, 50, 50), ==, 1);
	g_assert_cmpint(gowl_bar_hit_find(hits, 10, 10), ==, 0);
	g_assert_cmpint(gowl_bar_hit_find(hits, 200, 200), ==, -1);

	g_array_unref(hits);
}

static void
test_scroll_offsets_the_hit_regions(void)
{
	Fixture f;
	GowlBarPanel *panel;
	GowlBarPanelRenderCtx ctx;
	gint first_y_unscrolled, first_y_scrolled;

	fixture_init(&f);
	panel = build_kitchen_sink();

	gowl_bar_panel_render_ctx_init(&ctx, PANEL_W);
	ctx.hits = g_array_new(FALSE, FALSE, sizeof(GowlBarHitRect));
	gowl_bar_panel_render(panel, f.cr, f.layout, f.theme, &ctx);
	first_y_unscrolled =
		g_array_index(ctx.hits, GowlBarHitRect, 0).y;

	ctx.scroll = 60;
	gowl_bar_panel_render(panel, f.cr, f.layout, f.theme, &ctx);
	first_y_scrolled = g_array_index(ctx.hits, GowlBarHitRect, 0).y;

	/* Hit regions come out of the same walk that drew the pixels, so
	   scrolling moves both together or neither. */
	g_assert_cmpint(first_y_unscrolled - first_y_scrolled, ==, 60);

	g_array_unref(ctx.hits);
	g_object_unref(panel);
	fixture_clear(&f);
}

/* Nothing may be painted outside the items' own rows.
 *
 * A height calculation that draws is the specific way this broke: the
 * wrapped-label height was measured by calling the drawing helper, and
 * because the height pass runs during the draw pass too, every
 * paragraph was also stamped at the panel's origin, on top of the
 * hero.  A measure that paints is invisible in any assertion about
 * heights -- only the pixels show it. */
static void
test_the_measure_pass_does_not_draw(void)
{
	Fixture f;
	GowlBarPanel *panel;
	GowlBarPanelRenderCtx ctx;
	unsigned char *data;
	gint stride, x, y;
	gboolean painted;

	fixture_init(&f);

	/* Clear to fully transparent so any stray ink is unmistakable. */
	cairo_save(f.cr);
	cairo_set_operator(f.cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(f.cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(f.cr);
	cairo_restore(f.cr);

	/* A tall spacer first, so the whole top band of the panel is
	   guaranteed to belong to no item that draws anything. */
	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, PANEL_W);
	gowl_bar_panel_add_spacer(panel, 120);
	gowl_bar_panel_add_label(panel,
		"A paragraph long enough that it has to wrap onto several "
		"lines inside the panel, which is what makes its height "
		"something that must be measured rather than assumed.");

	gowl_bar_panel_render_ctx_init(&ctx, PANEL_W);
	ctx.hits = g_array_new(FALSE, FALSE, sizeof(GowlBarHitRect));
	gowl_bar_panel_render(panel, f.cr, f.layout, f.theme, &ctx);

	cairo_surface_flush(f.surface);
	data = cairo_image_surface_get_data(f.surface);
	stride = cairo_image_surface_get_stride(f.surface);

	painted = FALSE;
	for (y = 0; y < 100 && !painted; y++) {
		for (x = 0; x < PANEL_W; x++) {
			/* ARGB32 is premultiplied; a non-zero alpha is ink. */
			if (data[y * stride + x * 4 + 3] != 0) {
				painted = TRUE;
				break;
			}
		}
	}
	g_assert_false(painted);

	g_array_unref(ctx.hits);
	g_object_unref(panel);
	fixture_clear(&f);
}

/* A paragraph gets the height it actually needs, not one line's worth. */
static void
test_a_label_wraps(void)
{
	Fixture f;
	GowlBarPanel *short_panel;
	GowlBarPanel *long_panel;
	gint short_h, long_h;

	fixture_init(&f);

	short_panel = gowl_bar_panel_new();
	gowl_bar_panel_add_label(short_panel, "One line.");

	long_panel = gowl_bar_panel_new();
	gowl_bar_panel_add_label(long_panel,
		"A paragraph long enough that it has to wrap onto several "
		"lines inside the panel, which is the whole point: an "
		"explanation should be a sentence, not a truncated one.");

	short_h = gowl_bar_panel_measure(short_panel, f.layout, f.theme,
	                                 PANEL_W);
	long_h = gowl_bar_panel_measure(long_panel, f.layout, f.theme,
	                                PANEL_W);

	g_assert_cmpint(long_h, >, short_h);

	g_object_unref(short_panel);
	g_object_unref(long_panel);
	fixture_clear(&f);
}

static void
test_empty_panel_renders(void)
{
	Fixture f;
	GowlBarPanel *panel;
	GowlBarPanelRenderCtx ctx;

	fixture_init(&f);
	panel = gowl_bar_panel_new();

	gowl_bar_panel_render_ctx_init(&ctx, PANEL_W);
	ctx.hits = g_array_new(FALSE, FALSE, sizeof(GowlBarHitRect));
	gowl_bar_panel_render(panel, f.cr, f.layout, f.theme, &ctx);

	g_assert_cmpuint(ctx.hits->len, ==, 0);
	g_assert_cmpint(ctx.content_height, >, 0);

	g_array_unref(ctx.hits);
	g_object_unref(panel);
	fixture_clear(&f);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-panel/item-defaults",
	                test_item_defaults_are_neutral);
	g_test_add_func("/bar-panel/fraction-clamped", test_fraction_is_clamped);
	g_test_add_func("/bar-panel/copy-is-deep", test_item_copy_is_deep);
	g_test_add_func("/bar-panel/find-by-id", test_find_by_id);
	g_test_add_func("/bar-panel/measure-matches-render",
	                test_measure_matches_render);
	g_test_add_func("/bar-panel/render-does-not-mutate",
	                test_render_leaves_the_model_untouched);
	g_test_add_func("/bar-panel/hits-are-interactive",
	                test_only_interactive_items_are_hit_testable);
	g_test_add_func("/bar-panel/hit-find-topmost",
	                test_hit_find_prefers_the_topmost);
	g_test_add_func("/bar-panel/scroll-offsets-hits",
	                test_scroll_offsets_the_hit_regions);
	g_test_add_func("/bar-panel/measure-does-not-draw",
	                test_the_measure_pass_does_not_draw);
	g_test_add_func("/bar-panel/label-wraps", test_a_label_wraps);
	g_test_add_func("/bar-panel/empty-renders", test_empty_panel_renders);

	return g_test_run();
}
