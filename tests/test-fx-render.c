/* test-fx-render.c -- the shared effect layer, against a real GPU
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Five modules draw through src/fx, so a mistake here is five bugs that
 * all look different.  The three things checked are the ones that are
 * invisible in source and obvious on screen:
 *
 *   WHICH WAY IS UP.  wlroots hands out buffers whose first row is the
 *   top of the picture; GL's first framebuffer row is the bottom.  The
 *   default screen quad and the orthographic projection each have to
 *   agree with that, and with each other -- if they disagree, one effect
 *   comes out upside down and the rest look fine, which is the worst
 *   possible symptom.
 *
 *   THAT THE BLUR BLURS.  A blur that silently did nothing would leave
 *   the frosted-glass effect looking like plain transparency, which
 *   nobody would file a bug about.
 *
 *   THAT ROUNDED CORNERS ROUND.  Same reasoning.
 *
 * Needs a render node.  Without one there is no GLES2 renderer, the
 * effect modules decline to load in exactly the same way, and the test
 * skips.
 */

#include <glib.h>
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

#include "fx/gowl-fx.h"

#define TEST_W 128
#define TEST_H 64

typedef struct {
	struct wl_event_loop *loop;
	struct wlr_backend   *backend;
	struct wlr_renderer  *renderer;
	struct wlr_allocator *allocator;
	struct wlr_swapchain *swapchain;
	GowlFxGl             *gl;
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
	if (f->renderer == NULL || !gowl_fx_gl_supported(f->renderer))
		return FALSE;
	f->allocator = wlr_allocator_autocreate(f->backend, f->renderer);
	if (f->allocator == NULL)
		return FALSE;

	memset(&format, 0, sizeof(format));
	format.format    = DRM_FORMAT_XRGB8888;
	format.len       = 1;
	format.capacity  = 1;
	format.modifiers = &modifier;

	f->swapchain = wlr_swapchain_create(f->allocator, TEST_W, TEST_H, &format);
	if (f->swapchain == NULL)
		return FALSE;

	f->gl = gowl_fx_gl_new(f->renderer);
	return f->gl != NULL;
}

static void
fixture_close(Fixture *f)
{
	if (f->gl != NULL)
		gowl_fx_gl_free(f->gl);
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

/* Red over blue, with a hard edge halfway down. */
static struct wlr_texture *
striped(struct wlr_renderer *renderer)
{
	guint8 *pixels = g_malloc0((gsize)TEST_W * TEST_H * 4);
	struct wlr_texture *texture;
	gint x, y;

	for (y = 0; y < TEST_H; y++) {
		for (x = 0; x < TEST_W; x++) {
			guint8 *p = pixels + ((gsize)y * TEST_W + x) * 4;

			p[0] = y < TEST_H / 2 ? 0x00 : 0xff;   /* blue  */
			p[1] = 0x00;
			p[2] = y < TEST_H / 2 ? 0xff : 0x00;   /* red   */
			p[3] = 0xff;
		}
	}
	texture = wlr_texture_from_pixels(renderer, DRM_FORMAT_ARGB8888,
	                                  TEST_W * 4, TEST_W, TEST_H, pixels);
	g_free(pixels);
	return texture;
}

static guint8 *
read_back(struct wlr_renderer *renderer, struct wlr_buffer *buffer)
{
	struct wlr_texture *texture = wlr_texture_from_buffer(renderer, buffer);
	struct wlr_texture_read_pixels_options opts;
	guint8 *out;

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

/* Draw one quad and hand back the frame; NULL when the GPU is missing. */
static guint8 *
render_quad(Fixture *f, const GowlFxQuad *quad)
{
	struct wlr_buffer *buffer = wlr_swapchain_acquire(f->swapchain);
	GowlFxPass *pass;
	gfloat clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	guint8 *pixels;

	if (buffer == NULL)
		return NULL;
	pass = gowl_fx_pass_begin(f->gl, buffer);
	if (pass == NULL) {
		wlr_buffer_unlock(buffer);
		return NULL;
	}
	gowl_fx_pass_clear(pass, clear);
	gowl_fx_pass_quad(pass, quad);
	gowl_fx_pass_end(pass);

	pixels = read_back(f->renderer, buffer);
	wlr_buffer_unlock(buffer);
	return pixels;
}

/* The default screen quad, with the identity matrix, must come out the
 * right way up. */
static void
test_default_quad_orientation(void)
{
	Fixture f;
	GowlFxTexture tex;
	GowlFxQuad quad;
	struct wlr_texture *src;
	guint8 *pixels;
	gint r, g, b;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 renderer available (no DRM render node?)");
		return;
	}

	memset(&tex, 0, sizeof(tex));
	src = striped(f.renderer);
	g_assert_nonnull(src);
	g_assert_true(gowl_fx_texture_store(f.gl, &tex, src, TEST_W, TEST_H));
	wlr_texture_destroy(src);

	gowl_fx_quad_init(&quad);
	quad.texture = tex.tex;
	pixels = render_quad(&f, &quad);
	g_assert_nonnull(pixels);

	pixel_at(pixels, TEST_W / 2, TEST_H / 4, &r, &g, &b);
	g_assert_cmpint(r, >, 160);
	g_assert_cmpint(b, <, 80);
	pixel_at(pixels, TEST_W / 2, TEST_H * 3 / 4, &r, &g, &b);
	g_assert_cmpint(b, >, 160);
	g_assert_cmpint(r, <, 80);

	g_free(pixels);
	gowl_fx_texture_drop(f.gl, &tex);
	fixture_close(&f);
}

/*
 * The orthographic projection must agree with the default quad: y = 0 is
 * the TOP.  The expo module lays its grid out in these coordinates, so a
 * disagreement here puts the overview upside down while everything drawn
 * with the perspective matrix stays correct.
 */
static void
test_ortho_puts_zero_at_the_top(void)
{
	Fixture f;
	GowlFxTexture tex;
	GowlFxQuad quad;
	struct wlr_texture *src;
	guint8 *pixels;
	gfloat ortho[16];
	gfloat pos[12];
	gint r, g, b;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 renderer available (no DRM render node?)");
		return;
	}

	memset(&tex, 0, sizeof(tex));
	src = striped(f.renderer);
	g_assert_nonnull(src);
	g_assert_true(gowl_fx_texture_store(f.gl, &tex, src, TEST_W, TEST_H));
	wlr_texture_destroy(src);

	gowl_fx_mat4_ortho(ortho, (gdouble)TEST_W, (gdouble)TEST_H);

	/* The top half of the screen, in pixel coordinates. */
	pos[0]  = 0.0f;            pos[1]  = 0.0f;               pos[2]  = 0.0f;
	pos[3]  = 0.0f;            pos[4]  = TEST_H / 2.0f;      pos[5]  = 0.0f;
	pos[6]  = (gfloat)TEST_W;  pos[7]  = 0.0f;               pos[8]  = 0.0f;
	pos[9]  = (gfloat)TEST_W;  pos[10] = TEST_H / 2.0f;      pos[11] = 0.0f;

	gowl_fx_quad_init(&quad);
	quad.texture = tex.tex;
	quad.mvp = ortho;
	quad.pos = pos;

	pixels = render_quad(&f, &quad);
	g_assert_nonnull(pixels);

	/* The quad covers the upper half of the image, showing the whole
	 * texture squeezed into it -- so the top quarter is red... */
	pixel_at(pixels, TEST_W / 2, TEST_H / 8, &r, &g, &b);
	g_assert_cmpint(r, >, 160);
	/* ...and the bottom half is the cleared background, not the quad. */
	pixel_at(pixels, TEST_W / 2, TEST_H * 3 / 4, &r, &g, &b);
	g_assert_cmpint(r + g + b, <, 30);

	g_free(pixels);
	gowl_fx_texture_drop(f.gl, &tex);
	fixture_close(&f);
}

/* The blur has to actually soften the hard edge. */
static void
test_blur_softens_an_edge(void)
{
	Fixture f;
	GowlFxTexture tex, soft;
	GowlFxQuad quad;
	struct wlr_texture *src;
	guint8 *sharp_frame, *soft_frame;
	gint sharp_r, sharp_g, sharp_b;
	gint soft_r, soft_g, soft_b;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 renderer available (no DRM render node?)");
		return;
	}

	memset(&tex, 0, sizeof(tex));
	memset(&soft, 0, sizeof(soft));
	src = striped(f.renderer);
	g_assert_nonnull(src);
	g_assert_true(gowl_fx_texture_store(f.gl, &tex, src, TEST_W, TEST_H));
	wlr_texture_destroy(src);

	gowl_fx_quad_init(&quad);
	quad.texture = tex.tex;
	sharp_frame = render_quad(&f, &quad);
	g_assert_nonnull(sharp_frame);

	g_assert_true(gowl_fx_texture_blur(f.gl, &soft, &tex, 2, 3));
	quad.texture = soft.tex;
	soft_frame = render_quad(&f, &quad);
	g_assert_nonnull(soft_frame);

	/*
	 * One row above the seam.  Sharp, it is pure red; blurred, blue has
	 * bled into it.  Comparing the BLUE channel specifically is what
	 * makes this a test of blurring rather than of dimming: a blur that
	 * merely darkened would leave blue at zero.
	 */
	pixel_at(sharp_frame, TEST_W / 2, TEST_H / 2 - 2,
	         &sharp_r, &sharp_g, &sharp_b);
	pixel_at(soft_frame, TEST_W / 2, TEST_H / 2 - 2,
	         &soft_r, &soft_g, &soft_b);

	g_assert_cmpint(sharp_b, <, 30);
	g_assert_cmpint(soft_b, >, sharp_b + 20);

	/* And it stays a picture: the far corners keep their own colours. */
	pixel_at(soft_frame, TEST_W / 2, 2, &soft_r, &soft_g, &soft_b);
	g_assert_cmpint(soft_r, >, 120);

	g_free(sharp_frame);
	g_free(soft_frame);
	gowl_fx_texture_drop(f.gl, &tex);
	gowl_fx_texture_drop(f.gl, &soft);
	fixture_close(&f);
}

/* Rounded corners actually cut the corners off. */
static void
test_corner_rounding_cuts_corners(void)
{
	Fixture f;
	GowlFxTexture tex;
	GowlFxQuad quad;
	struct wlr_texture *src;
	guint8 *square, *rounded;
	gint r, g, b;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 renderer available (no DRM render node?)");
		return;
	}

	memset(&tex, 0, sizeof(tex));
	src = striped(f.renderer);
	g_assert_nonnull(src);
	g_assert_true(gowl_fx_texture_store(f.gl, &tex, src, TEST_W, TEST_H));
	wlr_texture_destroy(src);

	gowl_fx_quad_init(&quad);
	quad.texture = tex.tex;
	square = render_quad(&f, &quad);
	g_assert_nonnull(square);

	quad.corner = 0.25f;
	rounded = render_quad(&f, &quad);
	g_assert_nonnull(rounded);

	/* The very corner is painted when square and cleared when rounded. */
	pixel_at(square, 1, 1, &r, &g, &b);
	g_assert_cmpint(r + g + b, >, 100);
	pixel_at(rounded, 1, 1, &r, &g, &b);
	g_assert_cmpint(r + g + b, <, 30);

	/* The middle is untouched either way. */
	pixel_at(rounded, TEST_W / 2, TEST_H / 4, &r, &g, &b);
	g_assert_cmpint(r, >, 160);

	g_free(square);
	g_free(rounded);
	gowl_fx_texture_drop(f.gl, &tex);
	fixture_close(&f);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/fx/quad-orientation", test_default_quad_orientation);
	g_test_add_func("/fx/ortho-origin", test_ortho_puts_zero_at_the_top);
	g_test_add_func("/fx/blur", test_blur_softens_an_edge);
	g_test_add_func("/fx/corners", test_corner_rounding_cuts_corners);

	return g_test_run();
}
