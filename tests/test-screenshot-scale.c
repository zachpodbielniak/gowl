/* test-screenshot-scale.c -- a screenshot of the place that was asked for
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * test-capture-scale covers the arithmetic on its own.  This runs it
 * through a real compositor with a real renderer, because the bug it
 * exists for was never in the arithmetic --- it was in nobody doing
 * any.
 *
 * The setup is a headless output at scale 2, so its layout is 960x540
 * and its framebuffer is 1920x1080.  A white marker rectangle sits at
 * a known place in layout coordinates on a black desktop, and the
 * tests ask for regions and look at what comes back.
 *
 * What is asserted:
 *
 *   THE IMAGE IS THE SIZE OF THE PIXELS, NOT OF THE REQUEST.  A 100x80
 *   region on a scale-2 screen is 200x160 real pixels, and a caller
 *   that is told otherwise (ffmpeg, say) will tear every frame.
 *
 *   THE PICTURE IS OF THE RIGHT PLACE.  Asking for the marker's
 *   rectangle gets an all-white image.  Cropping with the layout
 *   numbers straight off --- what used to happen --- lands at half the
 *   distance in and photographs the desktop instead.
 *
 *   AND OF NOWHERE ELSE.  One region is chosen so that the OLD,
 *   unscaled crop would have landed squarely on the marker while the
 *   correct one lands well clear of it.  It must come back black.  A
 *   fix that scaled the size but not the offset would pass every other
 *   test here and fail this one.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "config/gowl-config.h"

/* The marker, in layout coordinates. */
#define MARK_X (200)
#define MARK_Y (150)
#define MARK_W (100)
#define MARK_H  (80)

/* The output's scale, and the layout it implies for a 1920x1080 panel. */
#define SCALE     (2.0)
#define LAYOUT_W  (960)
#define LAYOUT_H  (540)

typedef struct {
	gchar          *parent;
	gchar          *runtime;
	GowlConfig     *config;
	GowlCompositor *compositor;
	GowlMonitor    *mon;
	gboolean        started;
} Rig;

static gboolean
rig_up(Rig *r)
{
	const gchar *parent;
	GError *error = NULL;
	gint mw, mh;

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	/* Nothing may reach the session this runs in. */
	r->parent = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	parent = r->parent;
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-shot-scale-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "gles2", TRUE);

	r->config = gowl_config_new();
	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_clear_error(&error);
		return FALSE;
	}
	r->started = TRUE;

	r->mon = r->compositor->selmon;
	if (r->mon == NULL)
		return FALSE;

	/* The headless backend's default mode is not 1920x1080, and
	 * every number in this file is written around one that is. */
	if (!gowl_monitor_set_mode(r->mon, 1920, 1080, 60000))
		return FALSE;
	if (!gowl_monitor_set_scale(r->mon, SCALE))
		return FALSE;

	/* A headless output that is not the size this file is written
	 * around makes every number below meaningless; say so rather
	 * than assert something unrelated. */
	gowl_monitor_get_geometry(r->mon, NULL, NULL, &mw, &mh);
	if (mw != LAYOUT_W || mh != LAYOUT_H) {
		g_test_message("headless output is %dx%d of layout, "
		               "expected %dx%d", mw, mh, LAYOUT_W, LAYOUT_H);
		return FALSE;
	}

	/* A black desktop with one white rectangle on it, both in layout
	 * coordinates like everything else in the scene. */
	{
		struct wlr_scene_tree *bg;
		struct wlr_scene_rect  *ground, *mark;
		float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

		bg = gowl_compositor_get_scene_layer(r->compositor,
		                                     GOWL_SCENE_LAYER_BG);
		g_assert_nonnull(bg);

		ground = wlr_scene_rect_create(bg, LAYOUT_W, LAYOUT_H, black);
		wlr_scene_node_set_position(&ground->node, 0, 0);

		mark = wlr_scene_rect_create(bg, MARK_W, MARK_H, white);
		wlr_scene_node_set_position(&mark->node, MARK_X, MARK_Y);
	}

	return TRUE;
}

static void
rig_down(Rig *r)
{
	g_clear_object(&r->compositor);
	g_clear_object(&r->config);
	g_rmdir(r->runtime);
	g_free(r->runtime);
	if (r->parent != NULL)
		g_setenv("XDG_RUNTIME_DIR", r->parent, TRUE);
	g_free(r->parent);
}

/*
 * Is every pixel of @data white (or black)?  The buffer's channel
 * order depends on the renderer, so the marker and the desktop are
 * chosen to be all-ones and all-zeroes: which byte is which stops
 * mattering.  Alpha is skipped for the same reason.
 */
static gboolean
all_of_colour(GBytes *data, gint w, gint h, gboolean white)
{
	const guint8 *p;
	gsize size;
	gint stride, x, y, c;

	p = g_bytes_get_data(data, &size);
	if (p == NULL || h <= 0)
		return FALSE;
	stride = (gint)(size / (gsize)h);
	if (stride < w * 4)
		return FALSE;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			const guint8 *px = p + (gsize)y * (gsize)stride
			                     + (gsize)x * 4;
			for (c = 0; c < 3; c++) {
				if (white && px[c] < 200)
					return FALSE;
				if (!white && px[c] > 55)
					return FALSE;
			}
		}
	}
	return TRUE;
}

/* ------------------------------------------------------------------ */

static void
test_region_is_device_sized(void)
{
	Rig r;
	g_autoptr(GBytes) shot = NULL;
	gint w = 0, h = 0;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor at scale 2");
		rig_down(&r);
		return;
	}

	shot = gowl_compositor_screenshot_region(r.compositor, NULL,
	           MARK_X, MARK_Y, MARK_W, MARK_H, &w, &h, NULL);
	g_assert_nonnull(shot);

	/* Twice the request, because the screen has twice the pixels. */
	g_assert_cmpint(w, ==, MARK_W * 2);
	g_assert_cmpint(h, ==, MARK_H * 2);

	rig_down(&r);
}

static void
test_region_is_the_place_asked_for(void)
{
	Rig r;
	g_autoptr(GBytes) shot = NULL;
	gint w = 0, h = 0;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor at scale 2");
		rig_down(&r);
		return;
	}

	/* Exactly the marker: all of it, and nothing around it. */
	shot = gowl_compositor_screenshot_region(r.compositor, NULL,
	           MARK_X, MARK_Y, MARK_W, MARK_H, &w, &h, NULL);
	g_assert_nonnull(shot);
	g_assert_true(all_of_colour(shot, w, h, TRUE));

	rig_down(&r);
}

static void
test_region_is_not_the_old_place(void)
{
	Rig r;
	g_autoptr(GBytes) shot = NULL;
	gint w = 0, h = 0;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor at scale 2");
		rig_down(&r);
		return;
	}

	/*
	 * Layout (450,350) is device (900,700) -- empty desktop.  Used
	 * as a device offset, which is what the crop used to do, it is
	 * (450,350): inside the marker, which lives at device
	 * (400,300)-(600,460).  So this returns black now and returned
	 * white before.
	 */
	shot = gowl_compositor_screenshot_region(r.compositor, NULL,
	           450, 350, 50, 50, &w, &h, NULL);
	g_assert_nonnull(shot);
	g_assert_true(all_of_colour(shot, w, h, FALSE));

	rig_down(&r);
}

static void
test_desktop_region_is_black(void)
{
	Rig r;
	g_autoptr(GBytes) shot = NULL;
	gint w = 0, h = 0;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor at scale 2");
		rig_down(&r);
		return;
	}

	/* The top-left corner, which the marker is nowhere near. */
	shot = gowl_compositor_screenshot_region(r.compositor, NULL,
	           0, 0, 100, 80, &w, &h, NULL);
	g_assert_nonnull(shot);
	g_assert_cmpint(w, ==, 200);
	g_assert_cmpint(h, ==, 160);
	g_assert_true(all_of_colour(shot, w, h, FALSE));

	rig_down(&r);
}

static void
test_all_is_device_sized(void)
{
	Rig r;
	g_autoptr(GBytes) shot = NULL;
	gint w = 0, h = 0;

	if (!rig_up(&r)) {
		g_test_skip("no headless compositor at scale 2");
		rig_down(&r);
		return;
	}

	/* The stitched image is the framebuffer, not the layout: a
	 * canvas sized in layout units held a quarter of the desktop
	 * and left the rest of it black. */
	shot = gowl_compositor_screenshot_all(r.compositor, &w, &h, NULL);
	g_assert_nonnull(shot);
	g_assert_cmpint(w, ==, LAYOUT_W * 2);
	g_assert_cmpint(h, ==, LAYOUT_H * 2);

	rig_down(&r);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/screenshot-scale/region/is-device-sized",
	                test_region_is_device_sized);
	g_test_add_func("/screenshot-scale/region/is-the-place-asked-for",
	                test_region_is_the_place_asked_for);
	g_test_add_func("/screenshot-scale/region/is-not-the-old-place",
	                test_region_is_not_the_old_place);
	g_test_add_func("/screenshot-scale/region/desktop-is-black",
	                test_desktop_region_is_black);
	g_test_add_func("/screenshot-scale/all/is-device-sized",
	                test_all_is_device_sized);

	return g_test_run();
}
