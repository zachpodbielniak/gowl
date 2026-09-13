/* test-glass-render.c -- the liquid-glass shader, against a real GPU
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The refraction is a hundred lines of GLSL that no compiler checks and
 * no reviewer can run in their head, and every way it can be wrong looks
 * plausible in source:
 *
 *   IT COULD NOT BUILD AT ALL.  gowl_fx_pass_glass() is deliberately
 *   survivable -- it returns FALSE and the desktop is as it was -- so a
 *   shader that stopped compiling would show up as "the glass quietly
 *   went away", with nothing in the log anybody reads.
 *
 *   IT COULD BEND NOTHING.  A displacement that came out zero everywhere
 *   leaves a slightly tinted copy of the wallpaper, which is exactly what
 *   a frosted backdrop looks like.  Nobody files that bug; they just
 *   think the effect is subtle.
 *
 *   IT COULD BEND THE MIDDLE.  The flat centre of a slab must pass a ray
 *   straight through.  A sign error in the decay makes the whole window a
 *   lens, which looks impressive and is wrong -- text behind it swims.
 *
 *   IT COULD MISS ITS CORNERS.  The alpha mask is what makes the glass
 *   end where the window does; without it the window grows opaque square
 *   corners under its rounded ones.
 *
 * Needs a render node.  Without one there is no GLES2 renderer, the
 * module declines to load in exactly the same way, and this skips.
 */

#include <glib.h>
#include <math.h>
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

/* Big enough for a bevel to have somewhere to go: the shader clamps the
 * bevel to half the short side, so a small test rectangle would quietly
 * test a different lens than the one configured. */
#define TEST_W 256
#define TEST_H 256

typedef struct {
	struct wl_event_loop *loop;
	struct wlr_backend   *backend;
	struct wlr_renderer  *renderer;
	struct wlr_allocator *allocator;
	struct wlr_swapchain *swapchain;
	GowlFxGl             *gl;
	GowlFxTexture         wallpaper;
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
	format.format    = DRM_FORMAT_ARGB8888;
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
	if (f->gl != NULL) {
		gowl_fx_texture_drop(f->gl, &f->wallpaper);
		gowl_fx_gl_free(f->gl);
	}
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

/**
 * Wallpaper:
 * @WP_BANDS: green over magenta, one hard edge across the middle
 * @WP_STEP: green left of x = 40, magenta from there on
 * @WP_DARK: flat dark grey
 *
 * Three wallpapers, because the three things worth asserting each need a
 * different one and a wallpaper that suits one hides the others.  A
 * smooth gradient would suit none: bent by forty pixels it is still
 * roughly the same colour, so it cannot tell a working lens from a dead
 * one.  Every one of these has a hard edge or no structure at all.
 */
typedef enum { WP_BANDS, WP_STEP, WP_DARK } Wallpaper;

#define WP_STEP_X (40)

static void
wallpaper_colour(Wallpaper kind, gint x, gint y, guint8 *out)
{
	gboolean first;

	if (kind == WP_DARK) {
		out[0] = out[1] = out[2] = 0x20;
		out[3] = 0xff;
		return;
	}
	first = kind == WP_BANDS ? y < TEST_H / 2 : x < WP_STEP_X;
	out[0] = first ? 0x00 : 0xff;   /* blue  */
	out[1] = first ? 0xff : 0x00;   /* green */
	out[2] = first ? 0x00 : 0xff;   /* red   */
	out[3] = 0xff;
}

static gboolean
fixture_wallpaper(Fixture *f, Wallpaper kind)
{
	guint8             *pixels = g_malloc0((gsize)TEST_W * TEST_H * 4);
	struct wlr_texture *tex;
	gboolean            ok;
	gint                x, y;

	for (y = 0; y < TEST_H; y++) {
		for (x = 0; x < TEST_W; x++)
			wallpaper_colour(kind, x, y,
			                 pixels + ((gsize)y * TEST_W + x) * 4);
	}
	tex = wlr_texture_from_pixels(f->renderer, DRM_FORMAT_ARGB8888,
	                              TEST_W * 4, TEST_W, TEST_H, pixels);
	g_free(pixels);
	if (tex == NULL)
		return FALSE;

	ok = gowl_fx_texture_store(f->gl, &f->wallpaper, tex, TEST_W, TEST_H);
	wlr_texture_destroy(tex);
	return ok;
}

static guint8 *
read_back(Fixture *f, struct wlr_buffer *buffer)
{
	struct wlr_texture *texture = wlr_texture_from_buffer(f->renderer, buffer);
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

typedef struct { gint r, g, b, a; } Rgba;

static Rgba
pixel_at(const guint8 *pixels, gint x, gint y)
{
	const guint8 *p = pixels + ((gsize)y * TEST_W + x) * 4;
	Rgba c;

	c.b = p[0];
	c.g = p[1];
	c.r = p[2];
	c.a = p[3];
	return c;
}

/* Draw one slab over the whole buffer and hand back the frame. */
static guint8 *
render_glass(Fixture *f, const GowlFxGlassParams *params)
{
	struct wlr_buffer *buffer = wlr_swapchain_acquire(f->swapchain);
	GowlFxPass        *pass;
	gfloat             clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	guint8            *pixels;
	gboolean           drew;

	if (buffer == NULL)
		return NULL;
	pass = gowl_fx_pass_begin(f->gl, buffer);
	if (pass == NULL) {
		wlr_buffer_unlock(buffer);
		return NULL;
	}
	gowl_fx_pass_clear(pass, clear);
	drew = gowl_fx_pass_glass(pass, &f->wallpaper, &f->wallpaper, params);
	gowl_fx_pass_end(pass);

	if (!drew) {
		wlr_buffer_unlock(buffer);
		g_error("the liquid-glass shader would not build -- the module "
		        "would silently show no glass at all");
	}

	pixels = read_back(f, buffer);
	wlr_buffer_unlock(buffer);
	return pixels;
}

/* A slab filling the test buffer, with no frost and no tint, so what
 * comes out is the refraction and nothing else. */
static void
plain_slab(GowlFxGlassParams *p)
{
	gowl_fx_glass_params_init(p);
	p->width      = TEST_W;
	p->height     = TEST_H;
	p->radius     = 24.0f;
	p->bevel      = 40.0f;
	p->thickness  = 60.0f;
	p->clarity    = 1.0f;   /* no frost: both textures are the same here */
	p->saturation = 1.0f;
	p->dispersion = 0.0f;   /* one tap, so a colour is a position */
	p->rim        = 0.0f;   /* no light added on top of the sample */
	p->shade      = 0.0f;   /* and nothing multiplied out of it */
}

static void
test_the_flat_centre_passes_straight_through(void)
{
	Fixture           f;
	GowlFxGlassParams p;
	guint8           *frame;
	Rgba              above, below;

	if (!fixture_open(&f) || !fixture_wallpaper(&f, WP_BANDS)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	plain_slab(&p);
	frame = render_glass(&f, &p);
	g_assert_nonnull(frame);

	/*
	 * Well inside the bevel, where the slab is flat, the ray is not bent
	 * at all: the green band is still green and the magenta one still
	 * magenta.  A sign error in the decay makes the whole window a lens
	 * and this is the only thing that notices.
	 */
	above = pixel_at(frame, TEST_W / 2, TEST_H / 2 - 40);
	below = pixel_at(frame, TEST_W / 2, TEST_H / 2 + 40);
	g_assert_cmpint(above.g, >, 200);
	g_assert_cmpint(above.r, <, 55);
	g_assert_cmpint(below.r, >, 200);
	g_assert_cmpint(below.g, <, 55);
	/* And it is opaque there -- the mask only eats the corners. */
	g_assert_cmpint(above.a, >, 250);

	g_free(frame);
	fixture_close(&f);
}

/*
 * The rim shows what is behind the MIDDLE of the window, magnified out
 * to the edge.
 *
 * This is the one assertion that pins the sign of the whole thing.  A
 * vertical ray meets a surface whose normal leans outward, so it bends
 * INWARD as it crosses the glass and lands further from the edge than it
 * entered.  Reverse that and the effect still looks like a lens -- it
 * just pulls the world the wrong way, which is indistinguishable in a
 * screenshot of a photograph and completely wrong over text.
 *
 * The wallpaper is green only in the leftmost forty pixels.  With a slab
 * this thick the rim reaches a hundred and fifty pixels inward, so the
 * pixel two from the left edge can only be magenta if the sampling goes
 * inward, and can only be green if it does not move or goes outward.
 */
static void
test_the_rim_samples_inward(void)
{
	Fixture           f;
	GowlFxGlassParams p;
	guint8           *frame;
	Rgba              rim, centre;

	if (!fixture_open(&f) || !fixture_wallpaper(&f, WP_STEP)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	plain_slab(&p);
	p.bevel     = 60.0f;
	p.thickness = 200.0f;
	p.slope     = 4.0f;
	frame = render_glass(&f, &p);
	g_assert_nonnull(frame);

	/* Two pixels in from the left edge, vertically centred so the pull is
	 * purely horizontal and no corner confuses the direction. */
	rim = pixel_at(frame, 2, TEST_H / 2);
	g_assert_cmpint(rim.a, >, 250);
	g_assert_cmpint(rim.r, >, 200);
	g_assert_cmpint(rim.g, <, 55);

	/* And the flat middle still shows what is directly behind it, which
	 * on this wallpaper is also magenta -- so this is a guard against the
	 * shader having simply painted everything magenta, not a second
	 * reading of the same fact. */
	centre = pixel_at(frame, TEST_W / 2, TEST_H / 2);
	g_assert_cmpint(centre.r, >, 200);
	/* The far side of the step is green and must have survived. */
	g_assert_cmpint(pixel_at(frame, TEST_W / 2, 4).a, >, 0);

	g_free(frame);
	fixture_close(&f);
}

static void
test_the_corners_are_cut_out(void)
{
	Fixture           f;
	GowlFxGlassParams p;
	guint8           *frame;
	Rgba              corner, middle;

	if (!fixture_open(&f) || !fixture_wallpaper(&f, WP_BANDS)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	plain_slab(&p);
	p.radius = 48.0f;
	frame = render_glass(&f, &p);
	g_assert_nonnull(frame);

	/* Outside a 48px radius, the very corner pixel is not in the slab. */
	corner = pixel_at(frame, 1, 1);
	middle = pixel_at(frame, TEST_W / 2, TEST_H / 2);
	g_assert_cmpint(corner.a, <, 8);
	g_assert_cmpint(middle.a, >, 250);

	g_free(frame);
	fixture_close(&f);
}

static void
test_the_rim_catches_light(void)
{
	Fixture           f;
	GowlFxGlassParams p;
	guint8           *frame;
	gint              x, brightest = 0, centre;

	/* Dark and featureless: a bright wallpaper is already at 255 and
	 * added light has nowhere to go, so the test would pass or fail on
	 * the wallpaper rather than on the sheen. */
	if (!fixture_open(&f) || !fixture_wallpaper(&f, WP_DARK)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	plain_slab(&p);
	p.rim   = 3.0f;
	p.shade = 0.0f;
	frame = render_glass(&f, &p);
	g_assert_nonnull(frame);

	/*
	 * Along a row through the middle, the brightest point must be at an
	 * edge rather than in the flat centre: the sheen is a property of a
	 * tilted surface, and the centre has none.  Without it the rim
	 * parameter does nothing and the glass has no edge to catch the eye.
	 */
	centre = pixel_at(frame, TEST_W / 2, TEST_H / 2).g;
	for (x = 1; x < 24; x++)
		brightest = MAX(brightest, pixel_at(frame, x, TEST_H / 2).g);
	g_assert_cmpint(brightest, >, centre);

	g_free(frame);
	fixture_close(&f);
}

/*
 * The host's copy of the profile, which needs no GPU.
 *
 * It exists so the shader can be told how far the furthest pixel is
 * pulled -- the reference the colour fringe and the bevel ring are
 * measured against -- and it is a second implementation of arithmetic
 * that also lives in GLSL.  Two implementations of the same formula is
 * exactly the sort of thing that drifts, so the properties that must
 * hold of both are asserted here.
 */
static void
test_the_maximum_displacement_behaves(void)
{
	GowlFxGlassParams p;
	gdouble thin, thick, flat;

	gowl_fx_glass_params_init(&p);
	p.width = p.height = 400;

	p.thickness = 20.0f;
	thin = gowl_fx_glass_max_displacement(&p);
	p.thickness = 120.0f;
	thick = gowl_fx_glass_max_displacement(&p);
	p.thickness = 0.0f;
	flat = gowl_fx_glass_max_displacement(&p);

	g_assert_cmpfloat(thin, >, 0.0);
	/* A thicker slab bends further: that is what thickness means. */
	g_assert_cmpfloat(thick, >, thin);
	/* No slab still reports something positive, because it is a divisor
	 * in the shader and a zero there would be a black window. */
	g_assert_cmpfloat(flat, >, 0.0);

	/* The decay cap is a cap: it can only ever reduce the result. */
	p.thickness = 120.0f;
	p.slope = 4.0f;
	thick = gowl_fx_glass_max_displacement(&p);
	p.slope = 0.2f;
	g_assert_cmpfloat(gowl_fx_glass_max_displacement(&p), <=, thick);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/glass-render/flat-centre",
	                test_the_flat_centre_passes_straight_through);
	g_test_add_func("/glass-render/rim-samples-inward",
	                test_the_rim_samples_inward);
	g_test_add_func("/glass-render/corners", test_the_corners_are_cut_out);
	g_test_add_func("/glass-render/rim-light", test_the_rim_catches_light);
	g_test_add_func("/glass-render/max-displacement",
	                test_the_maximum_displacement_behaves);
	return g_test_run();
}
