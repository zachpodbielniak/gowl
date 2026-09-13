/* test-water-render.c -- the liquid-water shader and its clock
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything here is a way for moving water to fail SILENTLY.
 *
 *   IT COULD NOT BUILD.  gowl_fx_pass_water() is deliberately survivable,
 *   so a shader that stopped compiling shows up as "the water quietly
 *   went away" with nothing in any log a user reads.
 *
 *   IT COULD NOT MOVE.  A clock that is not advancing, or phases that are
 *   not reaching the shader, leave a still surface --- which is a
 *   perfectly plausible-looking frozen refraction.  Nobody reports
 *   "the water is beautiful but it is a photograph"; they assume it is
 *   meant to be subtle.
 *
 *   IT COULD BEND A FLAT SURFACE.  Still water must pass a ray straight
 *   through.  If it does not, `water-intensity: 0' is not calm water, it
 *   is a fixed distortion, and the calm end of every preset is wrong.
 *
 *   THE CLOCK COULD JUMP.  The phases are wrapped so a float can hold
 *   them exactly; a wrap that lands anywhere but a whole cycle snaps the
 *   whole surface sideways, once, some tens of minutes in --- which is
 *   the single hardest kind of bug to catch by looking.
 *
 * Needs a render node for the first four; the clock needs nothing.
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
	guint8 *pixels;
	struct wlr_texture *tex;
	gint x, y;
	gboolean ok;

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
	if (f->gl == NULL)
		return FALSE;

	/* A fine checkerboard.  Water moves the picture by a few pixels, so
	 * the wallpaper has to CHANGE within a few pixels or a displacement
	 * and no displacement look identical. */
	pixels = g_malloc0((gsize)TEST_W * TEST_H * 4);
	for (y = 0; y < TEST_H; y++) {
		for (x = 0; x < TEST_W; x++) {
			guint8 *p = pixels + ((gsize)y * TEST_W + x) * 4;
			gboolean on = ((x / 6) + (y / 6)) % 2 == 0;

			p[0] = on ? 0xf0 : 0x10;
			p[1] = on ? 0x30 : 0xd0;
			p[2] = on ? 0x80 : 0x20;
			p[3] = 0xff;
		}
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

static guint8 *
render(Fixture *f, const GowlFxWaterParams *p, const GowlFxWaterClock *clock)
{
	struct wlr_buffer  *buffer = wlr_swapchain_acquire(f->swapchain);
	struct wlr_texture *texture;
	struct wlr_texture_read_pixels_options opts;
	GowlFxPass *pass;
	gfloat clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	guint8 *out;
	gboolean drew;

	if (buffer == NULL)
		return NULL;
	pass = gowl_fx_pass_begin(f->gl, buffer);
	if (pass == NULL) {
		wlr_buffer_unlock(buffer);
		return NULL;
	}
	gowl_fx_pass_clear(pass, clear);
	drew = gowl_fx_pass_water(pass, &f->wallpaper, &f->wallpaper, p, clock);
	gowl_fx_pass_end(pass);
	if (!drew) {
		wlr_buffer_unlock(buffer);
		g_error("the liquid-water shader would not build -- the module "
		        "would silently show no water at all");
	}

	texture = wlr_texture_from_buffer(f->renderer, buffer);
	if (texture == NULL) {
		wlr_buffer_unlock(buffer);
		return NULL;
	}
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
	wlr_buffer_unlock(buffer);
	return out;
}

/* How many pixels differ by more than a rounding step. */
static guint
differing(const guint8 *a, const guint8 *b)
{
	guint n = 0;
	gsize i;

	for (i = 0; i < (gsize)TEST_W * TEST_H; i++) {
		const guint8 *p = a + i * 4, *q = b + i * 4;

		if (ABS((gint)p[0] - (gint)q[0]) > 2
		    || ABS((gint)p[1] - (gint)q[1]) > 2
		    || ABS((gint)p[2] - (gint)q[2]) > 2)
			n++;
	}
	return n;
}

/* Water with everything but the surface switched off, so what comes out
 * is the refraction and nothing else. */
static void
plain_water(GowlFxWaterParams *p)
{
	gowl_fx_water_params_init(p);
	p->width  = TEST_W;
	p->height = TEST_H;
	p->radius = 0.0f;
	p->clarity    = 1.0f;   /* the sharp texture, so a tap is a position */
	p->absorption = 0.0f;
	p->specular   = 0.0f;
	p->caustics   = 0.0f;
	p->fresnel    = 0.0f;
	p->foam       = 0.0f;
	p->meniscus   = 0.0f;
	p->dispersion = 0.0f;
	p->shore      = 0.0f;   /* no edge damping: the whole surface moves */
	p->drops      = 0.0f;
}

static void
test_still_water_passes_straight_through(void)
{
	Fixture f;
	GowlFxWaterParams p;
	GowlFxWaterClock clock;
	guint8 *flat, *direct;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	memset(&clock, 0, sizeof(clock));
	plain_water(&p);
	p.amplitude = 0.0f;
	flat = render(&f, &p, &clock);
	g_assert_nonnull(flat);

	/*
	 * A flat surface has a vertical normal, so the ray is not bent and
	 * every pixel shows the wallpaper directly behind it.  The same
	 * parameters with a depth of zero must give the identical picture:
	 * if they do not, a flat surface is bending something, and
	 * `water-intensity: 0' is not still water but a fixed distortion.
	 */
	p.depth = 0.0f;
	direct = render(&f, &p, &clock);
	g_assert_nonnull(direct);
	g_assert_cmpuint(differing(flat, direct), ==, 0);

	g_free(flat);
	g_free(direct);
	fixture_close(&f);
}

static void
test_the_surface_bends_what_is_behind_it(void)
{
	Fixture f;
	GowlFxWaterParams p;
	GowlFxWaterClock clock;
	guint8 *still, *moving;
	guint n;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	memset(&clock, 0, sizeof(clock));
	plain_water(&p);

	p.amplitude = 0.0f;
	still = render(&f, &p, &clock);
	g_assert_nonnull(still);

	p.amplitude = 8.0f;
	moving = render(&f, &p, &clock);
	g_assert_nonnull(moving);

	/* Most of the picture must have moved.  A surface that refracts
	 * nothing is indistinguishable from a frosted backdrop, which is a
	 * bug nobody files. */
	n = differing(still, moving);
	g_assert_cmpuint(n, >, (guint)(TEST_W * TEST_H / 4));

	g_free(still);
	g_free(moving);
	fixture_close(&f);
}

static void
test_the_water_actually_moves(void)
{
	Fixture f;
	GowlFxWaterParams p;
	GowlFxWaterClock a, b, c;
	guint8 *fa, *fb, *fc;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	plain_water(&p);
	p.amplitude = 8.0f;

	memset(&a, 0, sizeof(a));
	b = a;
	gowl_fx_water_advance(&b, 0.5, 1.0);
	c = b;
	gowl_fx_water_advance(&c, 0.5, 1.0);

	fa = render(&f, &p, &a);
	fb = render(&f, &p, &b);
	fc = render(&f, &p, &c);
	g_assert_nonnull(fa);
	g_assert_nonnull(fb);
	g_assert_nonnull(fc);

	/*
	 * Two different clocks, two different pictures --- and a third that
	 * differs from BOTH.  One comparison would pass on a surface that
	 * merely oscillated between two states, which a phase that wrapped to
	 * the wrong place would do.
	 */
	g_assert_cmpuint(differing(fa, fb), >, (guint)(TEST_W * TEST_H / 8));
	g_assert_cmpuint(differing(fb, fc), >, (guint)(TEST_W * TEST_H / 8));
	g_assert_cmpuint(differing(fa, fc), >, (guint)(TEST_W * TEST_H / 8));

	g_free(fa);
	g_free(fb);
	g_free(fc);
	fixture_close(&f);
}

static void
test_the_corners_are_cut_out(void)
{
	Fixture f;
	GowlFxWaterParams p;
	GowlFxWaterClock clock;
	guint8 *frame;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	memset(&clock, 0, sizeof(clock));
	plain_water(&p);
	p.radius = 48.0f;
	frame = render(&f, &p, &clock);
	g_assert_nonnull(frame);

	g_assert_cmpint(frame[3], <, 8);                       /* 0,0 */
	g_assert_cmpint(frame[(((gsize)TEST_H / 2) * TEST_W
	                       + TEST_W / 2) * 4 + 3], >, 250); /* middle */

	g_free(frame);
	fixture_close(&f);
}

/*
 * The clock, which needs no GPU.
 *
 * Its whole job is to stay in a range a 32-bit float represents exactly
 * while never landing anywhere but a whole cycle.  Both halves of that
 * are invisible until they are not: a phase outside the range stutters
 * after an hour, and a wrap to the wrong place snaps the entire surface
 * sideways, once, some tens of minutes in.
 */
static void
test_the_clock_stays_where_a_float_can_hold_it(void)
{
	GowlFxWaterClock clock;
	gint i;

	memset(&clock, 0, sizeof(clock));

	/* Ten minutes at sixty hertz, at a brisk speed. */
	for (i = 0; i < 36000; i++)
		gowl_fx_water_advance(&clock, 1.0 / 60.0, 1.4);

	for (i = 0; i < 4; i++) {
		g_assert_cmpfloat(clock.phase[i], >=, 0.0);
		g_assert_cmpfloat(clock.phase[i], <, 2.0 * G_PI);
		/* And exactly representable: a float must round-trip it, or the
		 * shader sees a different surface than the one computed. */
		g_assert_cmpfloat(fabs((gdouble)(gfloat)clock.phase[i]
		                       - clock.phase[i]), <, 1e-5);
	}
	g_assert_cmpfloat(clock.drop, >=, 0.0);
	g_assert_cmpfloat(clock.drop, <, 1024.0);

	/* The four run at different rates, or the four octaves move as one
	 * and the surface marches instead of churning. */
	g_assert_cmpfloat(fabs(clock.phase[0] - clock.phase[1]), >, 1e-6);
	g_assert_cmpfloat(fabs(clock.phase[1] - clock.phase[2]), >, 1e-6);
}

static void
test_a_stall_is_not_a_tidal_wave(void)
{
	GowlFxWaterClock slow, stalled;
	gint i;

	/* Coming back from a VT switch or a closed lid hands the clock a gap
	 * of however long that was.  A quarter of a second is the most it may
	 * act on: past that the surface teleports, which is far more jarring
	 * than a moment of slow motion. */
	memset(&slow, 0, sizeof(slow));
	memset(&stalled, 0, sizeof(stalled));

	for (i = 0; i < 15; i++)
		gowl_fx_water_advance(&slow, 1.0 / 60.0, 1.0);
	gowl_fx_water_advance(&stalled, 900.0, 1.0);

	for (i = 0; i < 4; i++)
		g_assert_cmpfloat(fabs(slow.phase[i] - stalled.phase[i]), <, 1e-9);

	/* And time that has not passed does not move it. */
	gowl_fx_water_advance(&stalled, 0.0, 1.0);
	gowl_fx_water_advance(&stalled, -5.0, 1.0);
	for (i = 0; i < 4; i++)
		g_assert_cmpfloat(fabs(slow.phase[i] - stalled.phase[i]), <, 1e-9);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/water-render/still-water",
	                test_still_water_passes_straight_through);
	g_test_add_func("/water-render/bends",
	                test_the_surface_bends_what_is_behind_it);
	g_test_add_func("/water-render/moves", test_the_water_actually_moves);
	g_test_add_func("/water-render/corners", test_the_corners_are_cut_out);
	g_test_add_func("/water-render/clock",
	                test_the_clock_stays_where_a_float_can_hold_it);
	g_test_add_func("/water-render/stall", test_a_stall_is_not_a_tidal_wave);
	return g_test_run();
}
