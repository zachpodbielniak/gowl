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

/* Unit tests for the raw-frame sink: the pure geometry/format helpers, the
 * GowlRawBuffer wlr_buffer wrapper, and the per-monitor GowlFrameSink
 * lifecycle (exercised against a headless wlr_scene -- no renderer/display). */

#include "core/gowl-frame-sink.h"

#include <glib.h>
#include <string.h>
#include <drm_fourcc.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

/* ---- pure helpers -------------------------------------------------------- */

static void
test_default_stride(void)
{
	g_assert_cmpint(gowl_frame_default_stride(1920), ==, 1920 * 4);
	g_assert_cmpint(gowl_frame_default_stride(1), ==, 4);
	g_assert_cmpint(gowl_frame_default_stride(0), ==, 0);
	g_assert_cmpint(gowl_frame_default_stride(-5), ==, 0);
}

static void
test_dims_valid(void)
{
	/* width>0, height>0, stride>=width*4 */
	g_assert_true(gowl_frame_dims_valid(10, 10, 40));
	g_assert_true(gowl_frame_dims_valid(10, 10, 64));   /* padded stride */
	g_assert_true(gowl_frame_dims_valid(1, 1, 4));
	/* degenerate / invalid */
	g_assert_false(gowl_frame_dims_valid(10, 10, 39));  /* stride < w*4 */
	g_assert_false(gowl_frame_dims_valid(0, 10, 0));
	g_assert_false(gowl_frame_dims_valid(10, 0, 40));
	g_assert_false(gowl_frame_dims_valid(-1, 10, 40));
	g_assert_false(gowl_frame_dims_valid(10, -1, 40));
	g_assert_false(gowl_frame_dims_valid(10, 10, 0));
}

static void
test_buffer_size(void)
{
	g_assert_cmpuint(gowl_frame_buffer_size(10, 40), ==, 400);
	g_assert_cmpuint(gowl_frame_buffer_size(1080, 1920 * 4), ==,
	                 (gsize) 1080 * 1920 * 4);
	g_assert_cmpuint(gowl_frame_buffer_size(0, 40), ==, 0);
	g_assert_cmpuint(gowl_frame_buffer_size(10, 0), ==, 0);
	g_assert_cmpuint(gowl_frame_buffer_size(-1, 40), ==, 0);
}

/* ---- GowlRawBuffer ------------------------------------------------------- */

static void
test_raw_buffer_valid(void)
{
	guint8 px[4 * 2 * 2];
	struct wlr_buffer *b;
	void *data = NULL;
	uint32_t fmt = 0;
	size_t stride = 0;

	memset(px, 0xAB, sizeof px);
	b = gowl_raw_buffer_create(px, 2, 2, 8);
	g_assert_nonnull(b);
	g_assert_cmpint(b->width, ==, 2);
	g_assert_cmpint(b->height, ==, 2);

	/* Always ARGB8888, honoring the given stride. */
	g_assert_true(wlr_buffer_begin_data_ptr_access(
		b, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt, &stride));
	g_assert_cmpuint(fmt, ==, DRM_FORMAT_ARGB8888);
	g_assert_cmpuint(stride, ==, 8);
	g_assert_nonnull(data);
	/* Pixels are copied, not aliased. */
	g_assert_cmpint(((guint8 *) data)[0], ==, 0xAB);
	g_assert_true((guint8 *) data != px);
	wlr_buffer_end_data_ptr_access(b);

	/* Mutating the source after creation must not change the buffer. */
	memset(px, 0x00, sizeof px);
	g_assert_true(wlr_buffer_begin_data_ptr_access(
		b, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt, &stride));
	g_assert_cmpint(((guint8 *) data)[0], ==, 0xAB);
	wlr_buffer_end_data_ptr_access(b);

	wlr_buffer_drop(b);
}

static void
test_raw_buffer_padded_stride(void)
{
	guint8 px[64 * 2];          /* 2 rows of 64-byte stride */
	struct wlr_buffer *b;

	memset(px, 0x11, sizeof px);
	b = gowl_raw_buffer_create(px, 10, 2, 64);  /* 40 < 64 padded */
	g_assert_nonnull(b);
	g_assert_cmpint(b->width, ==, 10);
	g_assert_cmpint(b->height, ==, 2);
	wlr_buffer_drop(b);
}

static void
test_raw_buffer_invalid(void)
{
	guint8 px[16];

	memset(px, 0, sizeof px);
	g_assert_null(gowl_raw_buffer_create(NULL, 2, 2, 8));   /* NULL pixels */
	g_assert_null(gowl_raw_buffer_create(px, 0, 2, 8));     /* w <= 0 */
	g_assert_null(gowl_raw_buffer_create(px, 2, 0, 8));     /* h <= 0 */
	g_assert_null(gowl_raw_buffer_create(px, -1, 2, 8));
	g_assert_null(gowl_raw_buffer_create(px, 2, 2, 4));     /* stride < w*4 */
}

/* ---- GowlFrameSink (headless scene) -------------------------------------- */

static void
test_sink_lifecycle(void)
{
	struct wlr_scene *scene;
	GowlFrameSink *sink;
	guint8 px[4 * 2 * 2];

	scene = wlr_scene_create();
	g_assert_nonnull(scene);
	memset(px, 0x7F, sizeof px);

	sink = gowl_frame_sink_new(&scene->tree, FALSE);
	g_assert_nonnull(sink);
	g_assert_true(gowl_frame_sink_is_empty(sink));

	/* First push for a monitor creates a node. */
	g_assert_true(gowl_frame_sink_push(sink, "DP-1", 0, 0, px, 2, 2, 8));
	g_assert_false(gowl_frame_sink_is_empty(sink));

	/* Re-push to the same monitor swaps in place (still one entry). */
	g_assert_true(gowl_frame_sink_push(sink, "DP-1", 100, 50, px, 2, 2, 8));

	/* A second monitor adds a second entry. */
	g_assert_true(gowl_frame_sink_push(sink, "HDMI-A-1", 0, 0, px, 2, 2, 8));

	/* Invalid pushes are graceful no-ops. */
	g_assert_false(gowl_frame_sink_push(sink, "DP-1", 0, 0, NULL, 2, 2, 8));
	g_assert_false(gowl_frame_sink_push(sink, NULL, 0, 0, px, 2, 2, 8));
	g_assert_false(gowl_frame_sink_push(sink, "DP-1", 0, 0, px, 2, 2, 4));
	g_assert_false(gowl_frame_sink_push(NULL, "DP-1", 0, 0, px, 2, 2, 8));

	/* Clearing one leaves the other. */
	gowl_frame_sink_clear(sink, "DP-1");
	g_assert_false(gowl_frame_sink_is_empty(sink));
	/* Clearing an unknown monitor is a no-op. */
	gowl_frame_sink_clear(sink, "does-not-exist");
	g_assert_false(gowl_frame_sink_is_empty(sink));

	gowl_frame_sink_clear_all(sink);
	g_assert_true(gowl_frame_sink_is_empty(sink));

	/* NULL-safety. */
	g_assert_true(gowl_frame_sink_is_empty(NULL));
	gowl_frame_sink_clear(NULL, "x");
	gowl_frame_sink_clear_all(NULL);

	gowl_frame_sink_free(sink);
	wlr_scene_node_destroy(&scene->tree.node);
}

static void
test_sink_keep_at_bottom(void)
{
	struct wlr_scene *scene;
	GowlFrameSink *sink;
	guint8 px[4 * 2 * 2];

	/* keep_at_bottom variant (used for the lock background) must still push
	 * and clear correctly; the z-order lowering is a scene-graph side effect
	 * we cannot assert headlessly, but the lifecycle must hold. */
	scene = wlr_scene_create();
	g_assert_nonnull(scene);
	memset(px, 0, sizeof px);

	sink = gowl_frame_sink_new(&scene->tree, TRUE);
	g_assert_nonnull(sink);
	g_assert_true(gowl_frame_sink_push(sink, "eDP-1", 0, 0, px, 2, 2, 8));
	g_assert_true(gowl_frame_sink_push(sink, "eDP-1", 0, 0, px, 2, 2, 8));
	g_assert_false(gowl_frame_sink_is_empty(sink));
	gowl_frame_sink_free(sink);
	wlr_scene_node_destroy(&scene->tree.node);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/frame-sink/default-stride", test_default_stride);
	g_test_add_func("/frame-sink/dims-valid", test_dims_valid);
	g_test_add_func("/frame-sink/buffer-size", test_buffer_size);
	g_test_add_func("/frame-sink/raw-buffer-valid", test_raw_buffer_valid);
	g_test_add_func("/frame-sink/raw-buffer-padded-stride",
	                test_raw_buffer_padded_stride);
	g_test_add_func("/frame-sink/raw-buffer-invalid", test_raw_buffer_invalid);
	g_test_add_func("/frame-sink/sink-lifecycle", test_sink_lifecycle);
	g_test_add_func("/frame-sink/sink-keep-at-bottom",
	                test_sink_keep_at_bottom);
	return g_test_run();
}
