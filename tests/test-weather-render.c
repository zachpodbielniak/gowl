/* test-weather-render.c -- the carbonation, leaves and snow shaders
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Three backdrops in one file because they share a fixture and, more to
 * the point, they share their failure modes.  tests/test-rain-render.c
 * sets those out at length for the rain; every one of them applies here
 * and is not repeated.  What IS here is the part of each that the rain's
 * tests could not have caught, because it is the part that is different:
 *
 *   A BUBBLE COULD REFRACT THE WRONG WAY.  This is the one real physical
 *   claim the carbonation makes.  Gas in liquid is a DIVERGING lens: it
 *   minifies and it never inverts, which is the exact opposite of a
 *   raindrop.  Getting the sign wrong gives a picture that still looks
 *   like something -- glass marbles in juice -- so nobody files it, and
 *   it is invisible in a diff because it is one character.  Asserted by
 *   putting a RAMP behind the pane and reading the sign of its slope
 *   inside the bubbles: upright and steeper is a diverging lens, and
 *   reversed is a converging one.
 *
 *   THE WIND COULD BE NOISE.  Every leaf on screen reads one gust, which
 *   is the whole reason a gust looks like one gust.  If it were merely a
 *   sine it would also be a fan: wind is calm most of the time and
 *   strong briefly, and that shape is a property of the advance
 *   function, so it is asserted on the CPU with no GPU at all.
 *
 *   A FLAKE COULD NEVER BECOME WATER.  The snow's headline is that a
 *   crystal lands, sits, melts into a bead a third its size and runs
 *   off.  A cross-fade from white to nothing would look like a flake
 *   going invisible and would pass every test the rain has.  Asserted by
 *   walking a settled flake's whole life and checking that what the pane
 *   does EARLY (scatter: brighter than the wallpaper) and what it does
 *   LATE (refract: displaced from the wallpaper) are different things
 *   that peak at different times.
 *
 *   THE FROST COULD NOT GROW.  It is the one clock here that is not
 *   wrapped, because ice does not repeat -- so it is also the one that
 *   could quietly saturate, or run backwards, without anything else
 *   noticing.
 *
 * Needs a render node for all but the two clock tests.
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

/* Which wallpaper the fixture paints.  The ramp exists for one test and
 * is worth a whole second texture: a checkerboard cannot tell an upright
 * lens from an inverted one, because it looks the same either way. */
typedef enum {
	WP_CHECKER,   /* structure at a few pixels, for "did it move" */
	WP_RAMP       /* a pure horizontal gradient, for "which way does it bend" */
} Wallpaper;

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
fixture_open(Fixture *f, Wallpaper kind)
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

	pixels = g_malloc0((gsize)TEST_W * TEST_H * 4);
	for (y = 0; y < TEST_H; y++) {
		for (x = 0; x < TEST_W; x++) {
			guint8 *p = pixels + ((gsize)y * TEST_W + x) * 4;

			if (kind == WP_CHECKER) {
				/* A fine checkerboard.  Anything here moves the picture
				 * by a few pixels, so the wallpaper has to CHANGE within
				 * a few pixels or a displacement and no displacement
				 * look identical. */
				gboolean on = ((x / 5) + (y / 5)) % 2 == 0;

				p[0] = on ? 0xf0 : 0x10;
				p[1] = on ? 0x30 : 0xd0;
				p[2] = on ? 0x80 : 0x20;
			} else {
				/*
				 * A pure horizontal ramp, identical down every column.
				 *
				 * Every channel the same and nothing varying in y, so
				 * the rendered value at a pixel is a direct reading of
				 * WHERE THE SHADER SAMPLED, in x, to within a quantum.
				 * The slope is 200 levels across 256 pixels -- steep
				 * enough that a five-pixel window separates a doubled
				 * gradient from a reversed one by tens of levels.
				 */
				guint8 v = (guint8)(28 + (x * 200) / TEST_W);

				p[0] = v;
				p[1] = v;
				p[2] = v;
			}
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

/*
 * One frame, whichever pass.
 *
 * The three take different parameter and clock types, so the caller
 * hands over a closure rather than a struct: dispatching on an enum here
 * would mean this function carrying all three parameter types, and every
 * test then filling in two it does not use.
 */
typedef gboolean (*DrawFn) (GowlFxPass *pass, const GowlFxTexture *soft,
                            const GowlFxTexture *sharp, gpointer user);

static guint8 *
render(Fixture *f, DrawFn draw, gpointer user, const gchar *what)
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
	drew = draw(pass, &f->wallpaper, &f->wallpaper, user);
	gowl_fx_pass_end(pass);
	if (!drew) {
		wlr_buffer_unlock(buffer);
		/* Survivable in production and fatal here: a shader that stopped
		 * compiling shows up on a desktop as "the effect quietly went
		 * away", with nothing in any log a user reads. */
		g_error("the %s shader would not build -- the module would "
		        "silently show nothing at all", what);
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

/* Mean of the green channel, as a stand-in for brightness. */
static gdouble
mean_green(const guint8 *px)
{
	gdouble sum = 0.0;
	gsize   i;

	for (i = 0; i < (gsize)TEST_W * TEST_H; i++)
		sum += px[i * 4 + 1];
	return sum / ((gdouble)TEST_W * TEST_H);
}

/*
 * Which way a field moved between two frames, vertically.
 *
 * The same trick tests/test-rain-render.c uses and for the same reason:
 * both frames are first differenced against an EMPTY pane, because the
 * wallpaper behind them is identical in the two and sliding one over the
 * other would otherwise find zero every time -- the static background
 * outvotes the moving objects by orders of magnitude.  Differencing
 * leaves a picture that is zero everywhere except where something is,
 * and correlating THOSE finds where it went.
 *
 * A positive answer means what was at y in @a is at a larger y in @b:
 * the field went DOWN.
 */
static gint
best_vertical_shift(const guint8 *a, const guint8 *b, const guint8 *empty,
                    gint reach)
{
	gfloat  *da = g_malloc0(sizeof(gfloat) * TEST_W * TEST_H);
	gfloat  *db = g_malloc0(sizeof(gfloat) * TEST_W * TEST_H);
	gint     best_s = 0;
	gdouble  best = -1.0;
	gint     s, x, y;
	gsize    i;

	for (i = 0; i < (gsize)TEST_W * TEST_H; i++) {
		const guint8 *pa = a + i * 4, *pb = b + i * 4, *pe = empty + i * 4;

		da[i] = (gfloat)(ABS((gint)pa[0] - (gint)pe[0])
		                 + ABS((gint)pa[1] - (gint)pe[1])
		                 + ABS((gint)pa[2] - (gint)pe[2]));
		db[i] = (gfloat)(ABS((gint)pb[0] - (gint)pe[0])
		                 + ABS((gint)pb[1] - (gint)pe[1])
		                 + ABS((gint)pb[2] - (gint)pe[2]));
	}

	for (s = -reach; s <= reach; s++) {
		gdouble sum = 0.0;

		for (y = reach; y < TEST_H - reach; y++) {
			for (x = 0; x < TEST_W; x++) {
				sum += (gdouble)da[(gsize)y * TEST_W + x]
				       * (gdouble)db[(gsize)(y + s) * TEST_W + x];
			}
		}
		if (sum > best) {
			best = sum;
			best_s = s;
		}
	}
	g_free(da);
	g_free(db);
	return best_s;
}

/* ── Carbonation ─────────────────────────────────────────────────── */

typedef struct {
	GowlFxFizzParams p;
	GowlFxFizzClock  c;
} FizzShot;

static gboolean
draw_fizz(GowlFxPass *pass, const GowlFxTexture *soft,
          const GowlFxTexture *sharp, gpointer user)
{
	FizzShot *s = user;

	return gowl_fx_pass_fizz(pass, soft, sharp, &s->p, &s->c);
}

/*
 * A drink with everything but the refraction switched off, so what comes
 * out is the lens and nothing else.
 *
 * `fog' in particular: with it on, the flat drink is the blurred texture
 * and a bubble is the sharp one, so every bubble would "differ" whether
 * or not it bent anything.  The mirror and the glint are off for the
 * same reason -- both write pixels that have nothing to do with where
 * the shader sampled.
 */
static void
plain_fizz(GowlFxFizzParams *p)
{
	gowl_fx_fizz_params_init(p);
	p->width      = TEST_W;
	p->height     = TEST_H;
	p->radius     = 0.0f;
	p->fog        = 0.0f;
	p->clarity    = 1.0f;
	p->absorption = 0.0f;
	p->specular   = 0.0f;
	p->rim        = 0.0f;
	p->mirror     = 0.0f;
	p->dispersion = 0.0f;
	p->foam       = 0.0f;
	p->cell       = 60.0f;
	p->site_width = 52.0f;
}

static void
test_a_flat_drink_passes_straight_through(void)
{
	Fixture f;
	FizzShot a, b;
	guint8 *flat, *direct;

	if (!fixture_open(&f, WP_CHECKER)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	plain_fizz(&a.p);
	a.p.sites = 0.0f;
	a.p.stray = 0.0f;
	a.p.cling = 0.0f;
	memset(&a.c, 0, sizeof(a.c));
	flat = render(&f, draw_fizz, &a, "carbonation");
	g_assert_nonnull(flat);

	/*
	 * Nothing in the glass means nothing to bend the ray, so the same
	 * parameters with a depth of zero must give the identical picture.
	 * If they do not, a flat drink is distorting the desktop and every
	 * preset's quiet end is wrong.
	 */
	b = a;
	b.p.depth = 0.0f;
	direct = render(&f, draw_fizz, &b, "carbonation");
	g_assert_nonnull(direct);
	g_assert_cmpuint(differing(flat, direct), ==, 0);

	g_free(flat);
	g_free(direct);
	fixture_close(&f);
}

static void
test_the_bubbles_bend_what_is_behind_them(void)
{
	Fixture f;
	FizzShot a, b;
	guint8 *flat, *fizzy;
	guint n;

	if (!fixture_open(&f, WP_CHECKER)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	plain_fizz(&a.p);
	memset(&a.c, 0, sizeof(a.c));
	gowl_fx_fizz_advance(&a.c, 2.0, 1.0, 14.0);
	b = a;
	b.p.sites = 0.0f;
	b.p.stray = 0.0f;
	b.p.cling = 0.0f;

	flat = render(&f, draw_fizz, &b, "carbonation");
	g_assert_nonnull(flat);
	fizzy = render(&f, draw_fizz, &a, "carbonation");
	g_assert_nonnull(fizzy);

	/*
	 * Some real share of the window has to have moved, and not most of
	 * it: the bubbles are lenses scattered through a drink, so a figure
	 * near zero means they refract nothing and a figure near everything
	 * means the whole surface is being displaced, which is the water's
	 * job and not this one's.
	 */
	n = differing(flat, fizzy);
	g_assert_cmpuint(n, >, (guint)(TEST_W * TEST_H / 60));
	g_assert_cmpuint(n, <, (guint)(TEST_W * TEST_H * 4 / 5));

	g_free(flat);
	g_free(fizzy);
	fixture_close(&f);
}

/*
 * THE ONE THAT MATTERS: a bubble is a DIVERGING lens.
 *
 * Gas in liquid bends the ray the other way from a raindrop, so a bubble
 * MINIFIES what is behind it and never inverts however large it gets.
 * Written with the rain's sign it still looks like something -- glass
 * beads in juice -- so nobody reports it, and the difference in the
 * source is one character.
 *
 * The wallpaper is a pure horizontal ramp, so the rendered value at a
 * pixel says where in x the shader sampled.  Its local slope is then the
 * magnification, with a sign:
 *
 *   diverging (this)          slope > 1, always positive
 *   converging, depth < 2     slope between 0 and 1
 *   converging, depth > 2     slope NEGATIVE -- the image is upside down
 *
 * AND THE RAIN IS THE CONTROL.  Asserting only that the fizz never
 * reverses would pass just as well on a shader that had stopped bending
 * anything at all, and it would pass on a measurement that could not
 * detect reversal in the first place.  So the same ramp is put behind
 * the RAIN at the same depth, where an inversion is the documented
 * behaviour, and the two counts are compared.  The test then says what
 * it means: one of these turns the world upside down and the other does
 * not.
 *
 * The lenses are made LARGE here on purpose.  The slope is read over a
 * five-pixel window, so a bubble a dozen pixels across is all rim as far
 * as this measurement is concerned -- and a rim, where the coverage
 * falls from one to zero within a pixel or two, genuinely does reverse
 * the local gradient whichever way the lens bends.  The first version of
 * this test measured exactly that and reported that a diverging lens
 * inverts.
 */
static void
count_ramp_slopes(const guint8 *img, guint *steeper, guint *reversed)
{
	/* The wallpaper's own slope over the same five-pixel window, in
	 * levels per window. */
	const gdouble bg = 200.0 * 4.0 / TEST_W;
	gint x, y;

	*steeper = 0;
	*reversed = 0;
	for (y = 8; y < TEST_H - 8; y++) {
		for (x = 8; x < TEST_W - 8; x++) {
			const guint8 *l = img + ((gsize)y * TEST_W + (x - 2)) * 4;
			const guint8 *r = img + ((gsize)y * TEST_W + (x + 2)) * 4;
			gdouble slope = (gdouble)r[1] - (gdouble)l[1];

			if (slope > bg * 1.8)
				(*steeper)++;
			else if (slope < -bg * 0.8)
				(*reversed)++;
		}
	}
}

typedef struct {
	GowlFxRainParams p;
	GowlFxRainClock  c;
} RainShot;

static gboolean
draw_rain(GowlFxPass *pass, const GowlFxTexture *soft,
          const GowlFxTexture *sharp, gpointer user)
{
	RainShot *s = user;

	return gowl_fx_pass_rain(pass, soft, sharp, &s->p, &s->c);
}

static void
test_a_bubble_minifies_where_a_drop_inverts(void)
{
	Fixture f;
	FizzShot a;
	RainShot r;
	guint8 *bubbles, *drops;
	guint b_steep, b_rev, d_steep, d_rev;

	if (!fixture_open(&f, WP_RAMP)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	/*
	 * Big lenses, few of them, and nothing drawn that is not refraction.
	 * The bubble radius is a fraction of the COLUMN and is additionally
	 * capped against the growth by the pass, so the column is what has to
	 * be widened to get a lens tens of pixels across.
	 */
	plain_fizz(&a.p);
	a.p.depth      = 6.0f;
	a.p.growth     = 0.4f;
	a.p.bubble     = 0.19f;
	a.p.site_width = 170.0f;
	a.p.sites      = 1.0f;
	a.p.spacing    = 0.3f;
	a.p.stray      = 0.0f;
	a.p.cell       = 210.0f;
	a.p.cling      = 0.8f;
	a.p.wobble     = 0.0f;
	memset(&a.c, 0, sizeof(a.c));
	gowl_fx_fizz_advance(&a.c, 3.0, 1.0, 14.0);

	bubbles = render(&f, draw_fizz, &a, "carbonation");
	g_assert_nonnull(bubbles);
	count_ramp_slopes(bubbles, &b_steep, &b_rev);

	/* The same ramp, the same depth, through the rain's drops. */
	gowl_fx_rain_params_init(&r.p);
	r.p.width      = TEST_W;
	r.p.height     = TEST_H;
	r.p.radius     = 0.0f;
	r.p.depth      = 6.0f;
	r.p.fog        = 0.0f;
	r.p.clarity    = 1.0f;
	r.p.absorption = 0.0f;
	r.p.specular   = 0.0f;
	r.p.rim        = 0.0f;
	r.p.impact     = 0.0f;
	r.p.dispersion = 0.0f;
	r.p.beads      = 0.0f;
	r.p.runs       = 0.0f;
	r.p.cell       = 150.0f;
	r.p.density    = 0.8f;
	memset(&r.c, 0, sizeof(r.c));
	gowl_fx_rain_advance(&r.c, 3.0, 1.0, 8.0);

	drops = render(&f, draw_rain, &r, "liquid-rain");
	g_assert_nonnull(drops);
	count_ramp_slopes(drops, &d_steep, &d_rev);

	if (g_test_verbose()) {
		g_print("bubbles: steeper %u reversed %u\n", b_steep, b_rev);
		g_print("drops:   steeper %u reversed %u\n", d_steep, d_rev);
	}

	/* Both really are bending something: a count near zero on either side
	 * would make the comparison below meaningless. */
	g_assert_cmpuint(b_steep, >, 800);
	g_assert_cmpuint(d_rev, >, 800);

	/*
	 * And they bend it OPPOSITE WAYS.
	 *
	 * The test is on each lens's OWN balance of the two, not on the raw
	 * counts: every lens of either kind reverses the ramp along its own
	 * antialiased rim, where the coverage falls from one to zero within a
	 * pixel, so neither count is ever zero and comparing them across the
	 * two shaders compares their rim lengths as much as their optics.
	 *
	 * What is unambiguous is which way each one leans.  A bubble is
	 * mostly minification with a rim; a raindrop past its focal plane is
	 * mostly inversion with a rim.  Measured, that is four to one one way
	 * and better than two to one the other -- and a sign error in either
	 * shader swaps them outright rather than shading the margin.
	 */
	g_assert_cmpuint(b_steep, >, b_rev * 2);
	g_assert_cmpuint(d_rev, >, d_steep * 2);
}

static void
test_the_bubbles_rise(void)
{
	Fixture f;
	FizzShot a, b, empty;
	guint8 *fa, *fb, *fe;
	gint shift;

	if (!fixture_open(&f, WP_CHECKER)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	/*
	 * Trains only, and the clinging bubbles off: those grow in place, and
	 * a thing that changes size without moving pulls the match back
	 * towards no movement at all.
	 */
	plain_fizz(&a.p);
	a.p.sites = 1.0f;
	a.p.stray = 0.0f;
	a.p.cling = 0.0f;
	a.p.wobble = 0.0f;
	memset(&a.c, 0, sizeof(a.c));
	/* Let the trains fill the glass before timing them. */
	gowl_fx_fizz_advance(&a.c, 4.0, 1.0, 14.0);

	b = a;
	/*
	 * A short step, and how short is arithmetic.  A bubble crosses the
	 * glass in about three seconds at the tuned rate and the fastest
	 * columns run at three times that, so a tenth of a second moves the
	 * slowest about nine pixels and the fastest nearer thirty.  The
	 * search has to reach past BOTH: a step that outruns it matches at
	 * zero, and a test whose answer is zero whatever the shader does is
	 * not a test.
	 */
	gowl_fx_fizz_advance(&b.c, 0.10, 1.0, 14.0);

	fa = render(&f, draw_fizz, &a, "carbonation");
	fb = render(&f, draw_fizz, &b, "carbonation");
	g_assert_nonnull(fa);
	g_assert_nonnull(fb);

	empty = a;
	empty.p.sites = 0.0f;
	fe = render(&f, draw_fizz, &empty, "carbonation");
	g_assert_nonnull(fe);

	shift = best_vertical_shift(fa, fb, fe, 34);
	if (g_test_verbose())
		g_print("the bubbles moved %d pixels down\n", shift);

	/* Buoyancy has a sign, and it is the opposite of gravity's.  Bubbles
	 * sinking through a drink is obvious in a screenshot and invisible in
	 * a diff.  Several pixels, not one: a single pixel is where the
	 * answer lands when the correlation has found nothing. */
	g_assert_cmpint(shift, <, -3);

	g_free(fa);
	g_free(fb);
	g_free(fe);
	fixture_close(&f);
}

/* ── Falling leaves ──────────────────────────────────────────────── */

typedef struct {
	GowlFxLeafParams p;
	GowlFxLeafClock  c;
} LeafShot;

static gboolean
draw_leaves(GowlFxPass *pass, const GowlFxTexture *soft,
            const GowlFxTexture *sharp, gpointer user)
{
	LeafShot *s = user;

	return gowl_fx_pass_leaves(pass, soft, sharp, &s->p, &s->c);
}

static void
plain_leaves(GowlFxLeafParams *p)
{
	gowl_fx_leaf_params_init(p);
	p->width  = TEST_W;
	p->height = TEST_H;
	p->radius = 0.0f;
	/* The pane itself out of the way: a leaf is an opaque object, so
	 * everything that survives here is one. */
	p->fog    = 0.0f;
	p->shadow = 0.0f;
	p->gloss  = 0.0f;
	p->leaf   = 26.0f;
	p->cell   = 84.0f;
	p->column = 70.0f;
}

static void
test_a_bare_window_has_no_leaves_on_it(void)
{
	Fixture f;
	LeafShot a, b;
	guint8 *bare, *plain;

	if (!fixture_open(&f, WP_CHECKER)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	plain_leaves(&a.p);
	a.p.falling = 0.0f;
	a.p.stuck   = 0.0f;
	memset(&a.c, 0, sizeof(a.c));
	bare = render(&f, draw_leaves, &a, "falling-leaves");
	g_assert_nonnull(bare);

	/*
	 * With nothing on the glass and no haze, the pane must be the
	 * wallpaper untouched -- so changing the one knob that only affects
	 * leaves must change nothing at all.  If it does, an empty tree is
	 * tinting the desktop.
	 */
	b = a;
	b.p.translucency = 0.0f;
	b.p.veins        = 0.0f;
	plain = render(&f, draw_leaves, &b, "falling-leaves");
	g_assert_nonnull(plain);
	g_assert_cmpuint(differing(bare, plain), ==, 0);

	g_free(bare);
	g_free(plain);
	fixture_close(&f);
}

static void
test_the_leaves_cover_the_wallpaper(void)
{
	Fixture f;
	LeafShot a, b;
	guint8 *bare, *leafy;
	guint n;

	if (!fixture_open(&f, WP_CHECKER)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	plain_leaves(&a.p);
	memset(&a.c, 0, sizeof(a.c));
	gowl_fx_leaf_advance(&a.c, 3.0, 1.0, 26.0, 1.0);
	b = a;
	b.p.falling = 0.0f;
	b.p.stuck   = 0.0f;

	bare  = render(&f, draw_leaves, &b, "falling-leaves");
	leafy = render(&f, draw_leaves, &a, "falling-leaves");
	g_assert_nonnull(bare);
	g_assert_nonnull(leafy);

	/*
	 * A real share of the window, and nowhere near all of it.  Leaves are
	 * the one thing in this family that OCCLUDES rather than refracting,
	 * so "some of the pixels changed" here means something different from
	 * what it means for the rain: it is the coverage.  Near zero is a
	 * tree with no leaves on it; near everything is a compost heap.
	 */
	n = differing(bare, leafy);
	g_assert_cmpuint(n, >, (guint)(TEST_W * TEST_H / 40));
	g_assert_cmpuint(n, <, (guint)(TEST_W * TEST_H * 3 / 4));

	g_free(bare);
	g_free(leafy);
	fixture_close(&f);
}

static void
test_the_leaves_fall_downwards(void)
{
	Fixture f;
	LeafShot a, b, empty;
	guint8 *fa, *fb, *fe;
	gint shift;

	if (!fixture_open(&f, WP_CHECKER)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	/*
	 * Falling leaves only, and the tumble off.
	 *
	 * A tumbling leaf periodically collapses to a line and swells back,
	 * which is a change in SHAPE rather than in position -- it pulls the
	 * correlation towards zero for the same reason a growing drop does
	 * next door.  The stuck ones are off because they do not move at all.
	 */
	plain_leaves(&a.p);
	a.p.stuck   = 0.0f;
	a.p.falling = 1.0f;
	a.p.tumble  = 0.0f;
	a.p.flutter = 0.0f;
	a.p.wind    = 0.0f;
	memset(&a.c, 0, sizeof(a.c));
	gowl_fx_leaf_advance(&a.c, 3.0, 1.0, 26.0, 0.0);

	b = a;
	/*
	 * A quarter second.  A leaf crosses the window in about eight seconds
	 * at the tuned rate and the fastest columns at three times that, so
	 * this moves the slowest eight pixels and the fastest twenty-four --
	 * both inside the search, which has to reach past the fastest or it
	 * matches at zero.
	 */
	gowl_fx_leaf_advance(&b.c, 0.25, 1.0, 26.0, 0.0);

	fa = render(&f, draw_leaves, &a, "falling-leaves");
	fb = render(&f, draw_leaves, &b, "falling-leaves");
	g_assert_nonnull(fa);
	g_assert_nonnull(fb);

	empty = a;
	empty.p.falling = 0.0f;
	fe = render(&f, draw_leaves, &empty, "falling-leaves");
	g_assert_nonnull(fe);

	shift = best_vertical_shift(fa, fb, fe, 30);
	if (g_test_verbose())
		g_print("the leaves moved %d pixels down\n", shift);
	g_assert_cmpint(shift, >, 2);

	g_free(fa);
	g_free(fb);
	g_free(fe);
	fixture_close(&f);
}

/*
 * THE WIND IS WEATHER, NOT A FAN.
 *
 * Every leaf on screen reads one gust, which is the whole reason a gust
 * looks like one gust rather than like each leaf deciding for itself.
 * But a gust that is merely a sine is a fan: it would spend half its life
 * at half strength, and the picture would never be still.
 *
 * What makes it read as wind is that it is calm MOST of the time and
 * strong briefly, which is a property of the advance function alone and
 * needs no GPU to check.  Asserted three ways: it stays in range, it
 * actually gets up at some point, and its time-average sits well below
 * the middle of its range -- which a sine's would not.
 */
static void
test_the_wind_is_calm_more_often_than_not(void)
{
	GowlFxLeafClock c;
	gdouble sum = 0.0, peak = 0.0;
	gint    i, n = 40000, calm = 0;

	memset(&c, 0, sizeof(c));
	for (i = 0; i < n; i++) {
		gowl_fx_leaf_advance(&c, 1.0 / 60.0, 1.0, 26.0, 1.0);
		g_assert_cmpfloat(c.gust, >=, 0.0);
		g_assert_cmpfloat(c.gust, <=, 1.0);
		sum += c.gust;
		if (c.gust > peak)
			peak = c.gust;
		if (c.gust < 0.2)
			calm++;
	}

	if (g_test_verbose())
		g_print("gust: mean %.3f peak %.3f calm %d%%\n",
		        sum / n, peak, calm * 100 / n);

	/* It really does blow. */
	g_assert_cmpfloat(peak, >, 0.75);
	/* And it really is calm most of the time.  A raw sine would sit at
	 * 0.5 and spend under a fifth of its life below 0.2. */
	g_assert_cmpfloat(sum / n, <, 0.40);
	g_assert_cmpint(calm, >, n / 3);

	/* Zero gustiness is a still day, not a slow one. */
	memset(&c, 0, sizeof(c));
	for (i = 0; i < 600; i++) {
		gowl_fx_leaf_advance(&c, 1.0 / 60.0, 1.0, 26.0, 0.0);
		g_assert_cmpfloat(c.gust, ==, 0.0);
	}
}

/* ── Snow ────────────────────────────────────────────────────────── */

typedef struct {
	GowlFxSnowParams p;
	GowlFxSnowClock  c;
} SnowShot;

static gboolean
draw_snow(GowlFxPass *pass, const GowlFxTexture *soft,
          const GowlFxTexture *sharp, gpointer user)
{
	SnowShot *s = user;

	return gowl_fx_pass_snow(pass, soft, sharp, &s->p, &s->c);
}

static void
plain_snow(GowlFxSnowParams *p)
{
	gowl_fx_snow_params_init(p);
	p->width      = TEST_W;
	p->height     = TEST_H;
	p->radius     = 0.0f;
	p->fog        = 0.0f;
	p->clarity    = 1.0f;
	p->absorption = 0.0f;
	p->rim        = 0.0f;
	p->dispersion = 0.0f;
	p->sparkle    = 0.0f;
	p->frost      = 0.0f;
	p->flake      = 22.0f;
	p->cell       = 78.0f;
	p->column     = 66.0f;
	p->run_width  = 52.0f;
}

static void
test_a_warm_dry_pane_is_untouched(void)
{
	Fixture f;
	SnowShot a, b;
	guint8 *bare, *direct;

	if (!fixture_open(&f, WP_CHECKER)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	plain_snow(&a.p);
	a.p.falling = 0.0f;
	a.p.settled = 0.0f;
	a.p.runs    = 0.0f;
	memset(&a.c, 0, sizeof(a.c));
	bare = render(&f, draw_snow, &a, "snow");
	g_assert_nonnull(bare);

	/* Nothing on the glass means nothing to bend the ray, so the same
	 * parameters with a depth of zero must give the identical picture. */
	b = a;
	b.p.depth = 0.0f;
	direct = render(&f, draw_snow, &b, "snow");
	g_assert_nonnull(direct);
	g_assert_cmpuint(differing(bare, direct), ==, 0);

	g_free(bare);
	g_free(direct);
	fixture_close(&f);
}

static void
test_the_flakes_fall_downwards(void)
{
	Fixture f;
	SnowShot a, b, empty;
	guint8 *fa, *fb, *fe;
	gint shift;

	if (!fixture_open(&f, WP_CHECKER)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	/* Falling flakes only: settled ones sit still and melt in place, and
	 * the spin changes a flake's shape without moving it. */
	plain_snow(&a.p);
	a.p.settled = 0.0f;
	a.p.runs    = 0.0f;
	a.p.falling = 1.0f;
	a.p.spin    = 0.0f;
	a.p.flutter = 0.0f;
	a.p.drift   = 0.0f;
	/*
	 * Plates, not dendrites, and it is the measurement that asks for it
	 * rather than taste.  A stellar crystal is six arms a couple of
	 * pixels wide; slide two frames of those over each other and almost
	 * nothing overlaps at ANY offset, so the correlation has no peak to
	 * find and the answer is whatever the noise says.  A plate is a
	 * compact disc, which is what a displacement measurement wants.  The
	 * shape is asserted nowhere here -- this test is only about which way
	 * the field went.
	 */
	a.p.arms    = 0.0f;
	memset(&a.c, 0, sizeof(a.c));
	gowl_fx_snow_advance(&a.c, 3.0, 1.0, 30.0, 0.0);

	b = a;
	/*
	 * A fifth of a second, and it is bounded from BOTH ends.
	 *
	 * A flake takes about twelve seconds to cross at the tuned rate and
	 * the fastest columns a third of that, so this moves the slowest
	 * layer about five pixels and the fastest around fifteen.  The search
	 * has to reach past the fastest or it matches at zero -- and the step
	 * has to stay well inside a flake's own diameter at the other end,
	 * because two frames in which nothing overlaps itself correlate no
	 * better at the true shift than anywhere else.
	 */
	gowl_fx_snow_advance(&b.c, 0.20, 1.0, 30.0, 0.0);

	fa = render(&f, draw_snow, &a, "snow");
	fb = render(&f, draw_snow, &b, "snow");
	g_assert_nonnull(fa);
	g_assert_nonnull(fb);

	empty = a;
	empty.p.falling = 0.0f;
	fe = render(&f, draw_snow, &empty, "snow");
	g_assert_nonnull(fe);

	shift = best_vertical_shift(fa, fb, fe, 26);
	if (g_test_verbose())
		g_print("the snow moved %d pixels down\n", shift);
	/*
	 * A SMALL number, and it should be.  Three layers are on screen at
	 * once at rates in the ratio 1 : 1.6 : 2.6, each with a per-column
	 * multiplier of one, two or three -- so what is being measured is a
	 * mixture of nine different true shifts between two and sixteen
	 * pixels, and the peak lands at the weighted mode rather than at the
	 * fastest.  It is the SIGN that this test is for.
	 */
	g_assert_cmpint(shift, >, 2);

	/* And the correlator itself is not simply answering the same thing
	 * whatever it is given: swapping the two frames has to swap the sign.
	 * A measurement with a margin this narrow is worth checking against
	 * its own null. */
	g_assert_cmpint(best_vertical_shift(fb, fa, fe, 26), <, 0);

	g_free(fa);
	g_free(fb);
	g_free(fe);
	fixture_close(&f);
}

/*
 * THE ONE THAT MATTERS: a settled flake turns into water.
 *
 * The headline claim is that a crystal lands, sits, melts into a bead a
 * third its size and runs off.  A shader that merely cross-faded the
 * crystal to nothing would look like a flake going invisible and would
 * pass every other test in this file.
 *
 * What separates the two is that a crystal and a bead do DIFFERENT
 * THINGS to the light.  A crystal scatters: it is brighter than the
 * wallpaper behind it and it displaces nothing.  A bead refracts: it is
 * not especially bright and it moves what is behind it.  And the bead is
 * a fraction of the size, because a snowflake is mostly air.
 *
 * SO THE TEST VARIES `melt' AND NOT THE CLOCK, and the first version of
 * it got that wrong in a way worth recording.  Walking the clock through
 * a life looks like the obvious thing to do and measures NOTHING: every
 * cell carries its own phase offset, so the population is spread evenly
 * across the whole lifecycle at every instant, and advancing the clock
 * only rotates which cell is where.  The aggregate is invariant by
 * construction, and the eight numbers it printed were flat to two
 * significant figures.
 *
 * `melt' says where in a life the melt BEGINS, so pushing it to either
 * end makes the whole population crystal or the whole population water
 * -- which is the comparison the claim is actually about.
 */
static void
test_a_settled_flake_melts_into_water(void)
{
	Fixture f;
	SnowShot ice, water, big, bare;
	guint8  *a, *b, *c, *empty, *flat_a, *flat_b;
	gdouble  base, bright_ice, bright_water;
	guint    moved_ice, moved_water, moved_big;

	if (!fixture_open(&f, WP_CHECKER)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	/* Settled flakes only, packed: the falling ones and the runs would
	 * both write over the layer being watched. */
	plain_snow(&ice.p);
	ice.p.falling = 0.0f;
	ice.p.runs    = 0.0f;
	ice.p.settled = 1.0f;
	ice.p.glow    = 1.15f;
	memset(&ice.c, 0, sizeof(ice.c));
	gowl_fx_snow_advance(&ice.c, 4.0, 1.0, 30.0, 0.0);

	/* Nearly all crystal: the melt does not start until the very end of
	 * a life, so almost every settled flake on the pane is still ice. */
	ice.p.melt = 0.78f;

	/* And nearly all water: the melt starts almost at once. */
	water = ice;
	water.p.melt = 0.02f;

	/* The same water, but not collapsed.  A flake that melted to a
	 * puddle its own size would refract over a much larger area, which is
	 * how the collapse itself gets asserted rather than assumed. */
	big = water;
	big.p.shrink = 1.0f;

	bare = ice;
	bare.p.settled = 0.0f;
	empty = render(&f, draw_snow, &bare, "snow");
	g_assert_nonnull(empty);
	base = mean_green(empty);

	a = render(&f, draw_snow, &ice, "snow");
	b = render(&f, draw_snow, &water, "snow");
	c = render(&f, draw_snow, &big, "snow");
	g_assert_nonnull(a);
	g_assert_nonnull(b);
	g_assert_nonnull(c);

	/*
	 * Displacement is measured against the SAME frame rendered at depth
	 * zero, which is the only way to separate "moved the picture" from
	 * "painted over it": a white crystal differs from the wallpaper too,
	 * and differencing against the bare pane would count it as
	 * refraction.
	 */
	ice.p.depth = 0.0f;
	water.p.depth = 0.0f;
	big.p.depth = 0.0f;
	flat_a = render(&f, draw_snow, &ice, "snow");
	flat_b = render(&f, draw_snow, &water, "snow");
	g_assert_nonnull(flat_a);
	g_assert_nonnull(flat_b);

	bright_ice   = mean_green(a) - base;
	bright_water = mean_green(b) - base;
	moved_ice    = differing(a, flat_a);
	moved_water  = differing(b, flat_b);
	{
		guint8 *flat_c;

		flat_c = render(&f, draw_snow, &big, "snow");
		g_assert_nonnull(flat_c);
		moved_big = differing(c, flat_c);
		g_free(flat_c);
	}

	if (g_test_verbose()) {
		g_print("crystal: brighter by %.2f, displaced %u px\n",
		        bright_ice, moved_ice);
		g_print("water:   brighter by %.2f, displaced %u px\n",
		        bright_water, moved_water);
		g_print("water at full size: displaced %u px\n", moved_big);
	}

	/*
	 * Ice SCATTERS.  A pane of crystals is measurably brighter than the
	 * bare wallpaper, and a pane of water is not -- which is the claim
	 * that drawing snow as a lens gets exactly backwards.
	 */
	g_assert_cmpfloat(bright_ice, >, 1.0);
	g_assert_cmpfloat(bright_ice, >, bright_water * 1.8);

	/* And water REFRACTS, where ice does not: the crystals sit on top of
	 * the optics beneath them and hide whatever the melt has started. */
	g_assert_cmpuint(moved_water, >, (guint)(TEST_W * TEST_H) / 100);
	g_assert_cmpuint(moved_water, >, moved_ice * 2);

	/*
	 * And the bead really is a fraction of the crystal.  Left at its own
	 * size it refracts over a great deal more of the pane; `shrink' is
	 * not decoration, it is the difference between a flake turning into
	 * water and a flake turning into a puddle.
	 */
	g_assert_cmpuint(moved_big, >, moved_water * 3 / 2);

	g_free(a);
	g_free(b);
	g_free(c);
	g_free(flat_a);
	g_free(flat_b);
	g_free(empty);
	fixture_close(&f);
}

/*
 * The frost grows, and it is the one clock here that is not wrapped.
 *
 * Ice does not repeat, so its clock saturates instead -- which means it
 * is also the one that could quietly stick at zero, or run away, without
 * any of the wrapped clocks noticing.  Checked on the CPU first (it is
 * pure arithmetic) and then on the glass, where a pane left to freeze
 * must end up with more ice on it than one that was not.
 */
static void
test_the_frost_grows_and_saturates(void)
{
	GowlFxSnowClock c;
	Fixture f;
	SnowShot warm, cold;
	guint8 *a, *b;
	gint i;

	memset(&c, 0, sizeof(c));
	g_assert_cmpfloat(c.frost, ==, 0.0);
	for (i = 0; i < 600; i++)
		gowl_fx_snow_advance(&c, 1.0 / 60.0, 1.0, 30.0, 0.0);
	/* No frost rate is a warm pane, and it stays clean. */
	g_assert_cmpfloat(c.frost, ==, 0.0);

	for (i = 0; i < 60; i++)
		gowl_fx_snow_advance(&c, 1.0 / 60.0, 1.0, 30.0, 0.05);
	g_assert_cmpfloat(c.frost, >, 0.0);
	g_assert_cmpfloat(c.frost, <, 1.0);

	/* And it stops at fully frosted rather than running away, which is
	 * what a clock that is not wrapped has to do instead. */
	for (i = 0; i < 6000; i++)
		gowl_fx_snow_advance(&c, 1.0 / 60.0, 1.0, 30.0, 0.5);
	g_assert_cmpfloat(c.frost, ==, 1.0);

	/* Turning it off melts back rather than snapping, so changing the
	 * preset does not take a window's frost away between frames. */
	for (i = 0; i < 30; i++)
		gowl_fx_snow_advance(&c, 1.0 / 60.0, 1.0, 30.0, 0.0);
	g_assert_cmpfloat(c.frost, <, 1.0);
	g_assert_cmpfloat(c.frost, >, 0.0);

	if (!fixture_open(&f, WP_CHECKER)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	plain_snow(&warm.p);
	warm.p.falling = 0.0f;
	warm.p.settled = 0.0f;
	warm.p.runs    = 0.0f;
	warm.p.frost   = 0.9f;
	memset(&warm.c, 0, sizeof(warm.c));
	cold = warm;
	cold.c.frost = 1.0;

	a = render(&f, draw_snow, &warm, "snow");
	b = render(&f, draw_snow, &cold, "snow");
	g_assert_nonnull(a);
	g_assert_nonnull(b);

	/* A pane that has been freezing for a while is visibly different
	 * from one that has not.  Without this the clock could advance
	 * perfectly and reach no uniform. */
	g_assert_cmpuint(differing(a, b), >, (guint)(TEST_W * TEST_H / 50));

	g_free(a);
	g_free(b);
	fixture_close(&f);
}

/* ── The clocks ──────────────────────────────────────────────────── */

/*
 * The wrap has to land on a whole number of cycles.
 *
 * Every one of these clocks carries a position in its fraction and an
 * IDENTITY in its whole part -- which bubble, which leaf, which flake --
 * and the shader hashes against that identity mod the cycle count.  A
 * wrap anywhere but a whole multiple of it changes both at once: every
 * object in the field jumps sideways and becomes a different object, once,
 * some tens of minutes into a session.  It is the hardest kind of bug to
 * catch by looking and the easiest to assert.
 *
 * No GPU needed.
 */
static void
test_the_clocks_wrap_on_a_whole_cycle(void)
{
	GowlFxFizzClock fz;
	GowlFxLeafClock lf;
	GowlFxSnowClock sn;
	gint i, k;

	memset(&fz, 0, sizeof(fz));
	memset(&lf, 0, sizeof(lf));
	memset(&sn, 0, sizeof(sn));

	/* Long enough that every clock has been round at least once. */
	for (i = 0; i < 400000; i++) {
		gowl_fx_fizz_advance(&fz, 1.0 / 60.0, 3.0, 4.0);
		gowl_fx_leaf_advance(&lf, 1.0 / 60.0, 3.0, 4.0, 1.0);
		gowl_fx_snow_advance(&sn, 1.0 / 60.0, 3.0, 4.0, 0.0);
	}

	g_assert_cmpfloat(fz.cling, >=, 0.0);
	g_assert_cmpfloat(fz.cling, <, GOWL_FX_FIZZ_CYCLES);
	for (k = 0; k < 3; k++) {
		g_assert_cmpfloat(fz.rise[k], >=, 0.0);
		g_assert_cmpfloat(fz.rise[k], <, GOWL_FX_FIZZ_CYCLES);
	}

	g_assert_cmpfloat(lf.stick, >=, 0.0);
	g_assert_cmpfloat(lf.stick, <, GOWL_FX_LEAF_CYCLES);
	for (k = 0; k < 3; k++) {
		g_assert_cmpfloat(lf.fall[k], >=, 0.0);
		g_assert_cmpfloat(lf.fall[k], <, GOWL_FX_LEAF_CYCLES);
	}
	/* The sway is radians and wrapped at a turn, which is what keeps a
	 * float exact for as long as a session lasts. */
	g_assert_cmpfloat(lf.sway, >=, 0.0);
	g_assert_cmpfloat(lf.sway, <, 2.0 * G_PI);

	g_assert_cmpfloat(sn.settle, >=, 0.0);
	g_assert_cmpfloat(sn.settle, <, GOWL_FX_SNOW_CYCLES);
	for (k = 0; k < 3; k++) {
		g_assert_cmpfloat(sn.fall[k], >=, 0.0);
		g_assert_cmpfloat(sn.fall[k], <, GOWL_FX_SNOW_CYCLES);
	}
	for (k = 0; k < 2; k++) {
		g_assert_cmpfloat(sn.run[k], >=, 0.0);
		g_assert_cmpfloat(sn.run[k], <, GOWL_FX_SNOW_CYCLES);
	}
}

/*
 * A stall is not a fast-forward.
 *
 * Coming back from a tag switch, a VT switch or a laptop lid hands the
 * advance however long it was away.  Every one of these caps the step,
 * because the alternative is that every bubble, leaf and flake teleports
 * the length of the window in one frame the moment somebody comes back
 * to their desk.
 */
static void
test_a_long_stall_is_not_a_long_storm(void)
{
	GowlFxFizzClock a, b;
	GowlFxLeafClock la, lb;
	GowlFxSnowClock sa, sb;

	memset(&a, 0, sizeof(a));
	b = a;
	gowl_fx_fizz_advance(&a, 0.25, 1.0, 14.0);
	gowl_fx_fizz_advance(&b, 600.0, 1.0, 14.0);
	g_assert_cmpfloat(ABS(a.rise[0] - b.rise[0]), <, 1e-9);

	memset(&la, 0, sizeof(la));
	lb = la;
	gowl_fx_leaf_advance(&la, 0.25, 1.0, 26.0, 1.0);
	gowl_fx_leaf_advance(&lb, 600.0, 1.0, 26.0, 1.0);
	g_assert_cmpfloat(ABS(la.fall[0] - lb.fall[0]), <, 1e-9);

	memset(&sa, 0, sizeof(sa));
	sb = sa;
	gowl_fx_snow_advance(&sa, 0.25, 1.0, 30.0, 0.1);
	gowl_fx_snow_advance(&sb, 600.0, 1.0, 30.0, 0.1);
	g_assert_cmpfloat(ABS(sa.fall[0] - sb.fall[0]), <, 1e-9);
	g_assert_cmpfloat(ABS(sa.frost - sb.frost), <, 1e-9);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/weather/fizz/a-flat-drink-passes-straight-through",
	                test_a_flat_drink_passes_straight_through);
	g_test_add_func("/weather/fizz/the-bubbles-bend-what-is-behind-them",
	                test_the_bubbles_bend_what_is_behind_them);
	g_test_add_func("/weather/fizz/a-bubble-minifies-where-a-drop-inverts",
	                test_a_bubble_minifies_where_a_drop_inverts);
	g_test_add_func("/weather/fizz/the-bubbles-rise",
	                test_the_bubbles_rise);

	g_test_add_func("/weather/leaves/a-bare-window-has-no-leaves-on-it",
	                test_a_bare_window_has_no_leaves_on_it);
	g_test_add_func("/weather/leaves/the-leaves-cover-the-wallpaper",
	                test_the_leaves_cover_the_wallpaper);
	g_test_add_func("/weather/leaves/the-leaves-fall-downwards",
	                test_the_leaves_fall_downwards);
	g_test_add_func("/weather/leaves/the-wind-is-calm-more-often-than-not",
	                test_the_wind_is_calm_more_often_than_not);

	g_test_add_func("/weather/snow/a-warm-dry-pane-is-untouched",
	                test_a_warm_dry_pane_is_untouched);
	g_test_add_func("/weather/snow/the-flakes-fall-downwards",
	                test_the_flakes_fall_downwards);
	g_test_add_func("/weather/snow/a-settled-flake-melts-into-water",
	                test_a_settled_flake_melts_into_water);
	g_test_add_func("/weather/snow/the-frost-grows-and-saturates",
	                test_the_frost_grows_and_saturates);

	g_test_add_func("/weather/clocks/wrap-on-a-whole-cycle",
	                test_the_clocks_wrap_on_a_whole_cycle);
	g_test_add_func("/weather/clocks/a-long-stall-is-not-a-long-storm",
	                test_a_long_stall_is_not_a_long_storm);

	return g_test_run();
}
