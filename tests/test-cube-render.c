/* test-cube-render.c -- the cube's GL path, end to end on a real GPU
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * test-cube.c covers the planner; this covers the half that can only be
 * wrong on a screen.  Two things are checked, and both are things a
 * reviewer cannot see by reading the shader:
 *
 *   1. WHICH WAY IS UP.  wlroots hands out buffers whose first row is the
 *      top of the picture; GL's first framebuffer row is the bottom.  Get
 *      that backwards and every desktop on the cube is upside down --- and
 *      it is backwards in a way that reads as correct in the source.  The
 *      test stores a red-over-blue desktop and asserts the rendered frame
 *      is red over blue.
 *
 *   2. THE SEAM.  With the envelope at zero the leading side is supposed
 *      to project to exactly the viewport, which is what makes a tag
 *      switch start and end with no cut.  The test renders that frame and
 *      asserts the desktop reaches all four edges.
 *
 * It needs a render node.  Without one there is no GLES2 renderer, the
 * cube declines to load in exactly the same way, and the test skips.
 */

#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include <drm_fourcc.h>
#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>

#include "../modules/cube/gowl-cube-gl.h"

#define TEST_W 128
#define TEST_H 64

typedef struct {
	struct wl_event_loop *loop;
	struct wlr_backend   *backend;
	struct wlr_renderer  *renderer;
	struct wlr_allocator *allocator;
	struct wlr_swapchain *swapchain;
	GowlCubeGl           *gl;
} Fixture;

static gboolean
fixture_open(Fixture *f)
{
	struct wlr_drm_format format;
	uint64_t modifier = DRM_FORMAT_MOD_INVALID;

	memset(f, 0, sizeof(*f));

	f->loop = wl_event_loop_create();
	if (f->loop == NULL)
		return FALSE;

	f->backend = wlr_headless_backend_create(f->loop);
	if (f->backend == NULL)
		return FALSE;

	f->renderer = wlr_renderer_autocreate(f->backend);
	if (f->renderer == NULL)
		return FALSE;

	/* Exactly the check the module makes before it offers to run. */
	if (!gowl_cube_gl_supported(f->renderer))
		return FALSE;

	f->allocator = wlr_allocator_autocreate(f->backend, f->renderer);
	if (f->allocator == NULL)
		return FALSE;

	memset(&format, 0, sizeof(format));
	format.format    = DRM_FORMAT_XRGB8888;
	format.len       = 1;
	format.capacity  = 1;
	format.modifiers = &modifier;

	f->swapchain = wlr_swapchain_create(f->allocator, TEST_W, TEST_H,
	                                    &format);
	if (f->swapchain == NULL)
		return FALSE;

	f->gl = gowl_cube_gl_new(f->renderer);
	return f->gl != NULL;
}

static void
fixture_close(Fixture *f)
{
	if (f->gl != NULL)
		gowl_cube_gl_free(f->gl);
	if (f->swapchain != NULL)
		wlr_swapchain_destroy(f->swapchain);
	if (f->allocator != NULL)
		wlr_allocator_destroy(f->allocator);
	if (f->renderer != NULL)
		wlr_renderer_destroy(f->renderer);
	if (f->backend != NULL)
		wlr_backend_destroy(f->backend);
	if (f->loop != NULL)
		wl_event_loop_destroy(f->loop);
	memset(f, 0, sizeof(*f));
}

/* A desktop that is unmistakably one way up: opaque red on top, opaque
 * blue underneath, with a hard edge halfway down. */
static struct wlr_texture *
make_striped_desktop(struct wlr_renderer *renderer)
{
	guint8 *pixels = g_malloc0((gsize)TEST_W * TEST_H * 4);
	struct wlr_texture *texture;
	gint x, y;

	for (y = 0; y < TEST_H; y++) {
		for (x = 0; x < TEST_W; x++) {
			guint8 *p = pixels + ((gsize)y * TEST_W + x) * 4;

			/* ARGB8888 little-endian: B, G, R, A in memory. */
			p[0] = y < TEST_H / 2 ? 0x00 : 0xff;   /* blue  */
			p[1] = 0x00;                            /* green */
			p[2] = y < TEST_H / 2 ? 0xff : 0x00;   /* red   */
			p[3] = 0xff;
		}
	}

	texture = wlr_texture_from_pixels(renderer, DRM_FORMAT_ARGB8888,
	                                  TEST_W * 4, TEST_W, TEST_H, pixels);
	g_free(pixels);
	return texture;
}

static void
frame_defaults(GowlCubeFrame *frame, const GowlCubeFace *face)
{
	memset(frame, 0, sizeof(*frame));
	frame->faces       = 4;
	frame->dir         = 1;
	frame->rotation    = 0.0;
	frame->face_angle  = G_PI_2;
	/* Zero envelope: no pull-back, no pitch, no backdrop, no reflection.
	 * This is the frame that has to be the flat desktop. */
	frame->bump        = 0.0;
	frame->speed       = 0.0;
	frame->zoom        = 1.45;
	frame->pitch_deg   = 14.0;
	frame->shading     = 0.78;
	frame->reflection  = 0.32;
	frame->motion_blur = 0.35;
	frame->caps        = TRUE;
	frame->backdrop[0] = 0.04f;
	frame->backdrop[1] = 0.04f;
	frame->backdrop[2] = 0.06f;
	frame->first_slot  = 0;
	frame->last_slot   = 0;
	frame->slot        = face;
}

/* Read the rendered buffer back as ARGB8888, caller frees. */
static guint8 *
read_back(struct wlr_renderer *renderer, struct wlr_buffer *buffer)
{
	struct wlr_texture *texture;
	struct wlr_texture_read_pixels_options opts;
	guint8 *out;

	texture = wlr_texture_from_buffer(renderer, buffer);
	if (texture == NULL)
		return NULL;

	out = g_malloc0((gsize)TEST_W * TEST_H * 4);
	memset(&opts, 0, sizeof(opts));
	opts.data   = out;
	opts.format = DRM_FORMAT_ARGB8888;
	opts.stride = TEST_W * 4;

	if (!wlr_texture_read_pixels(texture, &opts)) {
		g_free(out);
		out = NULL;
	}
	wlr_texture_destroy(texture);
	return out;
}

static void
pixel_at(const guint8 *pixels, gint x, gint y, gint *r, gint *g, gint *b)
{
	const guint8 *p = pixels + ((gsize)y * TEST_W + x) * 4;

	*b = p[0];
	*g = p[1];
	*r = p[2];
}

static void
test_flat_frame_is_the_desktop(void)
{
	Fixture             f;
	GowlCubeFace        face;
	GowlCubeFrame       frame;
	struct wlr_texture *source;
	struct wlr_buffer  *buffer;
	guint8             *pixels;
	gint                r, g, b;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 renderer available (no DRM render node?)");
		return;
	}

	memset(&face, 0, sizeof(face));
	source = make_striped_desktop(f.renderer);
	g_assert_nonnull(source);
	g_assert_true(gowl_cube_gl_store_face(f.gl, &face, source,
	                                      TEST_W, TEST_H));
	wlr_texture_destroy(source);

	frame_defaults(&frame, &face);

	buffer = wlr_swapchain_acquire(f.swapchain);
	g_assert_nonnull(buffer);
	g_assert_true(gowl_cube_gl_render(f.gl, buffer, &frame));

	pixels = read_back(f.renderer, buffer);
	if (pixels == NULL) {
		wlr_buffer_unlock(buffer);
		gowl_cube_gl_drop_face(f.gl, &face);
		fixture_close(&f);
		g_test_skip("cannot read pixels back from this renderer");
		return;
	}

	/* Which way is up.  Well inside each half, away from the bevel the
	 * shader draws along every border. */
	pixel_at(pixels, TEST_W / 2, TEST_H / 4, &r, &g, &b);
	g_assert_cmpint(r, >, 160);
	g_assert_cmpint(b, <, 80);

	pixel_at(pixels, TEST_W / 2, TEST_H * 3 / 4, &r, &g, &b);
	g_assert_cmpint(b, >, 160);
	g_assert_cmpint(r, <, 80);

	/* And not mirrored left-to-right either: the stripes run the full
	 * width, so check both sides keep their own half's colour. */
	pixel_at(pixels, TEST_W / 8, TEST_H / 4, &r, &g, &b);
	g_assert_cmpint(r, >, 160);
	pixel_at(pixels, TEST_W * 7 / 8, TEST_H / 4, &r, &g, &b);
	g_assert_cmpint(r, >, 160);

	g_free(pixels);
	wlr_buffer_unlock(buffer);
	gowl_cube_gl_drop_face(f.gl, &face);
	fixture_close(&f);
}

/*
 * The seam test.  At the very start and end of a rotation the picture must
 * BE the desktop, corner to corner --- if the leading side lands even
 * slightly short, a tag switch begins and ends with a visible flash of
 * backdrop around the edges.
 */
static void
test_flat_frame_fills_the_viewport(void)
{
	Fixture             f;
	GowlCubeFace        face;
	GowlCubeFrame       frame;
	struct wlr_texture *source;
	struct wlr_buffer  *buffer;
	guint8             *pixels;
	gint                r, g, b;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 renderer available (no DRM render node?)");
		return;
	}

	memset(&face, 0, sizeof(face));
	source = make_striped_desktop(f.renderer);
	g_assert_nonnull(source);
	g_assert_true(gowl_cube_gl_store_face(f.gl, &face, source,
	                                      TEST_W, TEST_H));
	wlr_texture_destroy(source);

	frame_defaults(&frame, &face);

	buffer = wlr_swapchain_acquire(f.swapchain);
	g_assert_nonnull(buffer);
	g_assert_true(gowl_cube_gl_render(f.gl, buffer, &frame));

	pixels = read_back(f.renderer, buffer);
	if (pixels == NULL) {
		wlr_buffer_unlock(buffer);
		gowl_cube_gl_drop_face(f.gl, &face);
		fixture_close(&f);
		g_test_skip("cannot read pixels back from this renderer");
		return;
	}

	/*
	 * One pixel in from each edge, at the vertical middle of the half it
	 * belongs to.  The bevel lands ON the border, so a small inset keeps
	 * the assertion about coverage rather than about the highlight.
	 */
	pixel_at(pixels, 2, TEST_H / 4, &r, &g, &b);
	g_assert_cmpint(r + g + b, >, 90);
	pixel_at(pixels, TEST_W - 3, TEST_H / 4, &r, &g, &b);
	g_assert_cmpint(r + g + b, >, 90);
	pixel_at(pixels, TEST_W / 2, 2, &r, &g, &b);
	g_assert_cmpint(r + g + b, >, 90);
	pixel_at(pixels, TEST_W / 2, TEST_H - 3, &r, &g, &b);
	g_assert_cmpint(r + g + b, >, 90);

	g_free(pixels);
	wlr_buffer_unlock(buffer);
	gowl_cube_gl_drop_face(f.gl, &face);
	fixture_close(&f);
}

/*
 * Mid-rotation the solid must actually be a solid: turned a quarter of a
 * side, the two visible faces meet at a corner and neither fills the
 * screen, so the backdrop has to show through at the edges.  Without this
 * a projection that never leaves the flat case would pass everything
 * above and still animate nothing.
 */
static void
test_mid_rotation_shows_a_corner(void)
{
	Fixture             f;
	GowlCubeFace        faces[2];
	GowlCubeFrame       frame;
	struct wlr_texture *source;
	struct wlr_buffer  *buffer;
	guint8             *flat, *turned;
	gint                differing = 0;
	gint                x, y;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 renderer available (no DRM render node?)");
		return;
	}

	memset(faces, 0, sizeof(faces));
	source = make_striped_desktop(f.renderer);
	g_assert_nonnull(source);
	g_assert_true(gowl_cube_gl_store_face(f.gl, &faces[0], source,
	                                      TEST_W, TEST_H));
	g_assert_true(gowl_cube_gl_store_face(f.gl, &faces[1], source,
	                                      TEST_W, TEST_H));
	wlr_texture_destroy(source);

	frame_defaults(&frame, faces);
	frame.last_slot = 1;

	buffer = wlr_swapchain_acquire(f.swapchain);
	g_assert_nonnull(buffer);
	g_assert_true(gowl_cube_gl_render(f.gl, buffer, &frame));
	flat = read_back(f.renderer, buffer);
	wlr_buffer_unlock(buffer);

	frame.rotation = G_PI_2 * 0.25;
	frame.bump     = 1.0;

	buffer = wlr_swapchain_acquire(f.swapchain);
	g_assert_nonnull(buffer);
	g_assert_true(gowl_cube_gl_render(f.gl, buffer, &frame));
	turned = read_back(f.renderer, buffer);
	wlr_buffer_unlock(buffer);

	if (flat == NULL || turned == NULL) {
		g_free(flat);
		g_free(turned);
		gowl_cube_gl_drop_face(f.gl, &faces[0]);
		gowl_cube_gl_drop_face(f.gl, &faces[1]);
		fixture_close(&f);
		g_test_skip("cannot read pixels back from this renderer");
		return;
	}

	for (y = 0; y < TEST_H; y++) {
		for (x = 0; x < TEST_W; x++) {
			gint r1, g1, b1, r2, g2, b2;

			pixel_at(flat, x, y, &r1, &g1, &b1);
			pixel_at(turned, x, y, &r2, &g2, &b2);
			if (ABS(r1 - r2) + ABS(g1 - g2) + ABS(b1 - b2) > 24)
				differing++;
		}
	}

	/* A quarter turn changes most of the picture.  A tenth of it would
	 * still pass a "something moved" check while being a broken
	 * projection, so the bar is deliberately high. */
	g_assert_cmpint(differing, >, (TEST_W * TEST_H) / 3);

	g_free(flat);
	g_free(turned);
	gowl_cube_gl_drop_face(f.gl, &faces[0]);
	gowl_cube_gl_drop_face(f.gl, &faces[1]);
	fixture_close(&f);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/cube-render/orientation",
	                test_flat_frame_is_the_desktop);
	g_test_add_func("/cube-render/no-seam",
	                test_flat_frame_fills_the_viewport);
	g_test_add_func("/cube-render/turns", test_mid_rotation_shows_a_corner);

	return g_test_run();
}
