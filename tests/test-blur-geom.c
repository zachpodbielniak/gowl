/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Where the blur backdrop crops the wallpaper.
 *
 * This exists because getting it wrong does not draw badly, it ABORTS.
 * wlr_render_pass_add_texture() asserts the source box lies inside the
 * texture, on the compositor thread, during a page flip -- and under
 * `cmacs --gowl' that process is the user's entire desktop session.
 * Emacs then re-raises the fatal signal on its main thread, so the crash
 * is reported with an idle pselect backtrace naming nothing to do with
 * blur.  A whole core dump was needed to find it the first time.
 *
 * So the invariant is asserted directly, for every case that can reach
 * it, rather than trusted.
 */

#include <glib.h>

#include "../modules/blur/gowl-blur-geom.c"

/* The exact condition wlroots asserts. */
static void
assert_inside_texture(const struct wlr_fbox *src, gint tex_w, gint tex_h,
                      const gchar *what)
{
	if (src->x < 0.0 || src->y < 0.0
	    || src->x + src->width  > (gdouble)tex_w
	    || src->y + src->height > (gdouble)tex_h) {
		g_error("%s: src {%g,%g %gx%g} outside %dx%d texture "
		        "-- wlroots would abort the session here",
		        what, src->x, src->y, src->width, src->height,
		        tex_w, tex_h);
	}
	g_assert_cmpfloat(src->width, >=, 0.0);
	g_assert_cmpfloat(src->height, >=, 0.0);
}

/*
 * The crash, reproduced from the core dump.
 *
 * Client "gst" under the scrolling layout: geom {2848, 62, 1376, 1800}
 * on a 2880x1920 output.  2848 + 1376 = 4224 > 2880 fails exactly one
 * clause of the assert.  Five other backdrops on the same frame were in
 * bounds, which is why it took a specific window in a specific place.
 */
static void
test_regression_scrolling_overflow(void)
{
	struct wlr_box  mon = { 0, 0, 2880, 1920 };
	struct wlr_box  geom = { 2848, 62, 1376, 1800 };
	struct wlr_fbox src;
	struct wlr_box  vis;

	g_assert_true(gowl_blur_backdrop_box(&geom, &mon, 2880, 1920,
	                                     &src, &vis));
	assert_inside_texture(&src, 2880, 1920, "scrolling overflow");

	/* Only the sliver actually on screen is shown. */
	g_assert_cmpint(vis.x, ==, 2848);
	g_assert_cmpint(vis.width, ==, 2880 - 2848);
}

/* A window scrolled off the LEFT edge: a width-only clamp misses this,
 * because the failing clause is src.x >= 0, not x + width <= w. */
static void
test_negative_origin(void)
{
	struct wlr_box  mon = { 0, 0, 2880, 1920 };
	struct wlr_box  geom = { -900, -40, 1376, 1800 };
	struct wlr_fbox src;
	struct wlr_box  vis;

	g_assert_true(gowl_blur_backdrop_box(&geom, &mon, 2880, 1920,
	                                     &src, &vis));
	assert_inside_texture(&src, 2880, 1920, "negative origin");
	g_assert_cmpint(vis.x, ==, 0);
	g_assert_cmpint(vis.y, ==, 0);
}

/* Entirely off the monitor: nothing to show, and the caller must be told
 * so rather than handed an empty box to render. */
static void
test_fully_offscreen(void)
{
	struct wlr_box mon = { 0, 0, 2880, 1920 };
	struct wlr_box a = { 4000, 0, 400, 400 };
	struct wlr_box b = { -900, 0, 400, 400 };

	g_assert_false(gowl_blur_backdrop_box(&a, &mon, 2880, 1920, NULL, NULL));
	g_assert_false(gowl_blur_backdrop_box(&b, &mon, 2880, 1920, NULL, NULL));
}

/* A second monitor is not at the layout origin; the crop is relative to
 * ITS corner, not the layout's. */
static void
test_monitor_offset(void)
{
	struct wlr_box  mon = { 2880, 0, 1920, 1080 };
	struct wlr_box  geom = { 2880 + 100, 50, 400, 300 };
	struct wlr_fbox src;
	struct wlr_box  vis;

	g_assert_true(gowl_blur_backdrop_box(&geom, &mon, 1920, 1080,
	                                     &src, &vis));
	assert_inside_texture(&src, 1920, 1080, "monitor offset");
	g_assert_cmpfloat(src.x, ==, 100.0);
	g_assert_cmpfloat(src.y, ==, 50.0);
}

/* HiDPI: buffer pixels are twice the logical extent, so the crop must
 * scale or it comes from the top-left quarter of the wallpaper. */
static void
test_hidpi_scale(void)
{
	struct wlr_box  mon = { 0, 0, 1440, 960 };
	struct wlr_box  geom = { 100, 50, 400, 300 };
	struct wlr_fbox src;

	g_assert_true(gowl_blur_backdrop_box(&geom, &mon, 2880, 1920,
	                                     &src, NULL));
	assert_inside_texture(&src, 2880, 1920, "hidpi");
	g_assert_cmpfloat(src.x, ==, 200.0);
	g_assert_cmpfloat(src.y, ==, 100.0);
	g_assert_cmpfloat(src.width, ==, 800.0);
	g_assert_cmpfloat(src.height, ==, 600.0);
}

/* Non-square pixels: the vertical scale must come from the height.
 * Deriving both axes from the width was the latent half of the bug. */
static void
test_anisotropic_scale(void)
{
	struct wlr_box  mon = { 0, 0, 1440, 1080 };
	struct wlr_box  geom = { 0, 0, 1440, 1080 };
	struct wlr_fbox src;

	g_assert_true(gowl_blur_backdrop_box(&geom, &mon, 2880, 1080,
	                                     &src, NULL));
	assert_inside_texture(&src, 2880, 1080, "anisotropic");
	g_assert_cmpfloat(src.width, ==, 2880.0);
	/* Width-derived scaling would make this 2160 and assert. */
	g_assert_cmpfloat(src.height, ==, 1080.0);
}

/* Degenerate inputs must not divide by zero or escape the texture. */
static void
test_degenerate(void)
{
	struct wlr_box  zero_mon = { 0, 0, 0, 0 };
	struct wlr_box  geom = { 0, 0, 100, 100 };
	struct wlr_fbox src;

	g_assert_false(gowl_blur_backdrop_box(&geom, &zero_mon, 100, 100,
	                                      &src, NULL));
	g_assert_false(gowl_blur_backdrop_box(&geom, &geom, 0, 0, &src, NULL));
	g_assert_false(gowl_blur_backdrop_box(NULL, &geom, 10, 10, &src, NULL));
}

/*
 * Sweep every position a window can take relative to a monitor,
 * including fractional output scales.  The point is not any single case
 * but that no reachable input escapes the texture.
 */
static void
test_sweep_never_escapes(void)
{
	static const gint tex[][2] = {
		{ 2880, 1920 }, { 1920, 1080 }, { 2256, 1504 }, { 3000, 2000 }
	};
	struct wlr_box mon = { 0, 0, 1440, 960 };
	gsize t;
	gint  x, y;

	for (t = 0; t < G_N_ELEMENTS(tex); t++) {
		for (x = -2000; x <= 2000; x += 37) {
			for (y = -1500; y <= 1500; y += 41) {
				struct wlr_box  geom = { x, y, 700, 500 };
				struct wlr_fbox src;

				if (!gowl_blur_backdrop_box(&geom, &mon,
				                            tex[t][0], tex[t][1],
				                            &src, NULL))
					continue;
				assert_inside_texture(&src, tex[t][0],
				                      tex[t][1], "sweep");
			}
		}
	}
}

/*
 * When the cached backdrop has to be captured again.
 *
 * The backdrop is the wallpaper with every client layer hidden, so
 * windows moving or opening do not change it -- which is why it is
 * cached at all.  Tags do change it (wallpapers are per-tag), and so
 * does the output size.
 */
static void
test_stale_rebuilds_on_tag_change(void)
{
	/* The reported case: leave tag 2 for tag 4, come back. */
	g_assert_true(gowl_blur_backdrop_stale(TRUE, 1u << 1, 1u << 3,
	                                       2880, 1920, 2880, 1920));
	g_assert_true(gowl_blur_backdrop_stale(TRUE, 1u << 3, 1u << 1,
	                                       2880, 1920, 2880, 1920));
	/* Same tag, nothing else moved: keep the cache. */
	g_assert_false(gowl_blur_backdrop_stale(TRUE, 1u << 1, 1u << 1,
	                                        2880, 1920, 2880, 1920));
	/* A multi-tag view is its own tag set. */
	g_assert_true(gowl_blur_backdrop_stale(TRUE, 1u << 1,
	                                       (1u << 1) | (1u << 3),
	                                       2880, 1920, 2880, 1920));
}

static void
test_stale_rebuilds_on_resize_or_empty(void)
{
	g_assert_true(gowl_blur_backdrop_stale(FALSE, 1, 1,
	                                       2880, 1920, 2880, 1920));
	g_assert_true(gowl_blur_backdrop_stale(TRUE, 1, 1,
	                                       2880, 1920, 1920, 1080));
	g_assert_true(gowl_blur_backdrop_stale(TRUE, 1, 1,
	                                       2880, 1920, 2880, 1200));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/blur-geom/regression-scrolling-overflow",
	                test_regression_scrolling_overflow);
	g_test_add_func("/blur-geom/negative-origin", test_negative_origin);
	g_test_add_func("/blur-geom/fully-offscreen", test_fully_offscreen);
	g_test_add_func("/blur-geom/monitor-offset", test_monitor_offset);
	g_test_add_func("/blur-geom/hidpi-scale", test_hidpi_scale);
	g_test_add_func("/blur-geom/anisotropic-scale", test_anisotropic_scale);
	g_test_add_func("/blur-geom/degenerate", test_degenerate);
	g_test_add_func("/blur-geom/sweep-never-escapes",
	                test_sweep_never_escapes);
	g_test_add_func("/blur-geom/stale-on-tag-change",
	                test_stale_rebuilds_on_tag_change);
	g_test_add_func("/blur-geom/stale-on-resize",
	                test_stale_rebuilds_on_resize_or_empty);
	return g_test_run();
}
