/* test-rain-render.c -- the liquid-rain shader and its clock
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything here is a way for rain to fail SILENTLY, and one that is
 * not silent at all but is impossible to assert by looking.
 *
 *   IT COULD NOT BUILD.  gowl_fx_pass_rain() is deliberately survivable,
 *   so a shader that stopped compiling shows up as "the rain quietly went
 *   away" with nothing in any log a user reads.
 *
 *   A DRY PANE COULD BEND SOMETHING.  With no drops on it the glass is
 *   flat, and a ray must pass straight through.  If it does not,
 *   `rain-density: 0' is not a clean window but a fixed distortion.
 *
 *   THE DROPS COULD NOT REFRACT.  A drop that does not bend what is
 *   behind it is a grey blob, and a field of grey blobs looks enough like
 *   a deliberate texture that nobody files it.
 *
 *   IT COULD FALL UPWARDS.  A sign error in the running-drop clock gives
 *   rain running up the window.  It is obvious in a screenshot and
 *   invisible in a diff, so it is asserted here by CORRELATING two frames
 *   and finding which way the picture moved.
 *
 *   IT COULD BE A GRID.  This is the one that has actually happened, next
 *   door in the water, twice.  A drop field laid on a lattice reads as
 *   wallpaper rather than as weather, and "it looks a bit regular" is not
 *   something a test can be told -- so the lattice is looked for the way
 *   it would be found in a signal: by autocorrelating the drop mask and
 *   checking there is no peak at the cell pitch.
 *
 *   THE CLOCK COULD JUMP.  The clocks are wrapped so a float holds them
 *   exactly; a wrap anywhere but a whole cycle snaps every drop sideways,
 *   once, some tens of minutes in -- the hardest kind of bug to catch by
 *   looking.
 *
 * Needs a render node for all but the clock.
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

	/* A fine checkerboard.  A drop moves the picture by a few pixels, so
	 * the wallpaper has to CHANGE within a few pixels or a displacement
	 * and no displacement look identical. */
	pixels = g_malloc0((gsize)TEST_W * TEST_H * 4);
	for (y = 0; y < TEST_H; y++) {
		for (x = 0; x < TEST_W; x++) {
			guint8 *p = pixels + ((gsize)y * TEST_W + x) * 4;
			gboolean on = ((x / 5) + (y / 5)) % 2 == 0;

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
render(Fixture *f, const GowlFxRainParams *p, const GowlFxRainClock *clock)
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
	drew = gowl_fx_pass_rain(pass, &f->wallpaper, &f->wallpaper, p, clock);
	gowl_fx_pass_end(pass);
	if (!drew) {
		wlr_buffer_unlock(buffer);
		g_error("the liquid-rain shader would not build -- the module "
		        "would silently show no rain at all");
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

/*
 * Rain with everything but the water switched off, so what comes out is
 * the refraction and nothing else.
 *
 * `fog' in particular: with it on, the dry pane is the blurred texture
 * and a drop is the sharp one, so every drop would "differ" whether or
 * not it bent anything.  At zero the only thing that can move a pixel is
 * a ray that was bent.
 */
static void
plain_rain(GowlFxRainParams *p)
{
	gowl_fx_rain_params_init(p);
	p->width  = TEST_W;
	p->height = TEST_H;
	p->radius = 0.0f;
	p->fog        = 0.0f;
	p->clarity    = 1.0f;
	p->absorption = 0.0f;
	p->specular   = 0.0f;
	p->rim        = 0.0f;
	p->impact     = 0.0f;
	p->dispersion = 0.0f;
	p->beads      = 0.0f;
	p->runs       = 0.0f;
	p->cell       = 40.0f;
	p->density    = 0.34f;
}

static void
test_a_dry_pane_passes_straight_through(void)
{
	Fixture f;
	GowlFxRainParams p;
	GowlFxRainClock clock;
	guint8 *dry, *direct;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	memset(&clock, 0, sizeof(clock));
	plain_rain(&p);
	p.density = 0.0f;
	dry = render(&f, &p, &clock);
	g_assert_nonnull(dry);

	/*
	 * Nothing on the glass means nothing to bend the ray, so the same
	 * parameters with a depth of zero must give the identical picture.
	 * If they do not, a clean window is distorting the desktop and every
	 * preset's quiet end is wrong.
	 */
	p.depth = 0.0f;
	direct = render(&f, &p, &clock);
	g_assert_nonnull(direct);
	g_assert_cmpuint(differing(dry, direct), ==, 0);

	g_free(dry);
	g_free(direct);
	fixture_close(&f);
}

static void
test_the_drops_bend_what_is_behind_them(void)
{
	Fixture f;
	GowlFxRainParams p;
	GowlFxRainClock clock;
	guint8 *dry, *wet;
	guint n;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	memset(&clock, 0, sizeof(clock));
	plain_rain(&p);

	p.density = 0.0f;
	dry = render(&f, &p, &clock);
	g_assert_nonnull(dry);

	p.density = 0.34f;
	wet = render(&f, &p, &clock);
	g_assert_nonnull(wet);

	/*
	 * Some real share of the window has to have moved, and not most of
	 * it: the drops are lenses scattered on a pane, so a figure near zero
	 * means they refract nothing and a figure near everything means the
	 * whole surface is being displaced, which is the water's job and not
	 * this one's.
	 */
	n = differing(dry, wet);
	g_assert_cmpuint(n, >, (guint)(TEST_W * TEST_H / 50));
	g_assert_cmpuint(n, <, (guint)(TEST_W * TEST_H * 4 / 5));

	g_free(dry);
	g_free(wet);
	fixture_close(&f);
}

static void
test_the_rain_actually_moves(void)
{
	Fixture f;
	GowlFxRainParams p;
	GowlFxRainClock a, b, c;
	guint8 *fa, *fb, *fc;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	plain_rain(&p);
	p.runs      = 1.0f;
	p.run_width = 70.0f;

	memset(&a, 0, sizeof(a));
	b = a;
	gowl_fx_rain_advance(&b, 0.5, 1.0, 8.0);
	c = b;
	gowl_fx_rain_advance(&c, 0.5, 1.0, 8.0);

	fa = render(&f, &p, &a);
	fb = render(&f, &p, &b);
	fc = render(&f, &p, &c);
	g_assert_nonnull(fa);
	g_assert_nonnull(fb);
	g_assert_nonnull(fc);

	/*
	 * Three clocks, three different pictures --- and the first and third
	 * differ too.  Two comparisons would pass on a field that merely
	 * flickered between two states, which a clock wrapping to the wrong
	 * place would do.
	 */
	g_assert_cmpuint(differing(fa, fb), >, (guint)(TEST_W * TEST_H / 100));
	g_assert_cmpuint(differing(fb, fc), >, (guint)(TEST_W * TEST_H / 100));
	g_assert_cmpuint(differing(fa, fc), >, (guint)(TEST_W * TEST_H / 100));

	g_free(fa);
	g_free(fb);
	g_free(fc);
	fixture_close(&f);
}

/*
 * Which way the RAIN moved between two frames.
 *
 * Both frames are first differenced against a dry pane, which is the
 * whole trick: the wallpaper behind is identical in the two, so sliding
 * one frame over the other to find the best match finds zero every time
 * -- the static background outvotes the drops by orders of magnitude.
 * Differencing leaves a picture that is zero everywhere except where
 * there is water, and correlating THOSE finds where the water went.
 *
 * A positive answer means what was at y in @a is at a larger y in @b,
 * which is to say the drops went down the window.
 */
static gint
best_vertical_shift(const guint8 *a, const guint8 *b, const guint8 *dry,
                    gint reach)
{
	gfloat  *da = g_malloc0(sizeof(gfloat) * TEST_W * TEST_H);
	gfloat  *db = g_malloc0(sizeof(gfloat) * TEST_W * TEST_H);
	gint     best_s = 0;
	gdouble  best = -1.0;
	gint     s, x, y;
	gsize    i;

	for (i = 0; i < (gsize)TEST_W * TEST_H; i++) {
		const guint8 *pa = a + i * 4, *pb = b + i * 4, *pd = dry + i * 4;

		da[i] = (gfloat)(ABS((gint)pa[0] - (gint)pd[0])
		                 + ABS((gint)pa[1] - (gint)pd[1])
		                 + ABS((gint)pa[2] - (gint)pd[2]));
		db[i] = (gfloat)(ABS((gint)pb[0] - (gint)pd[0])
		                 + ABS((gint)pb[1] - (gint)pd[1])
		                 + ABS((gint)pb[2] - (gint)pd[2]));
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

static void
test_the_drops_run_downwards(void)
{
	Fixture f;
	GowlFxRainParams p;
	GowlFxRainClock a, b;
	guint8 *fa, *fb, *dry;
	gint shift;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	/* Running drops only: the resting ones grow and fade in place, which
	 * would pull the match back towards no movement at all. */
	/*
	 * Running drops only, and their trails cut SHORT.
	 *
	 * A long trail is a long vertical stripe, and a vertical stripe
	 * correlates with itself just as well at every shift -- with the tie
	 * broken at zero, where the overlap is greatest.  It drowns the head,
	 * which is the only part of the picture that actually says which way
	 * the water went.  The resting drops are off for the same reason:
	 * they grow and fade in place.
	 */
	plain_rain(&p);
	p.density   = 0.0f;
	p.runs      = 1.0f;
	p.run_width = 64.0f;
	p.run_len   = 30.0f;
	p.beads     = 0.0f;

	memset(&a, 0, sizeof(a));
	b = a;
	/*
	 * A short step, and how short is arithmetic.  A head crosses the
	 * window plus two trail lengths in one turn of its clock, so at these
	 * numbers this step moves the slow columns about nine pixels and the
	 * doubled ones twice that.  The search has to reach past BOTH: a
	 * step that outruns it matches at zero, and a test whose answer is
	 * zero whatever the shader does is not a test.
	 */
	gowl_fx_rain_advance(&b, 0.02, 1.0, 8.0);

	fa = render(&f, &p, &a);
	fb = render(&f, &p, &b);
	g_assert_nonnull(fa);
	g_assert_nonnull(fb);

	/* The same pane with no water on it, to difference against. */
	p.runs = 0.0f;
	dry = render(&f, &p, &a);
	g_assert_nonnull(dry);

	shift = best_vertical_shift(fa, fb, dry, 30);
	if (g_test_verbose())
		g_print("the rain moved %d pixels down\n", shift);
	/* Gravity has a sign.  Rain running up the window is obvious in a
	 * screenshot and invisible in a diff. */
	/* Several pixels, not one: a single pixel is where the answer lands
	 * when the correlation has found nothing and the peak is noise. */
	g_assert_cmpint(shift, >, 3);

	g_free(fa);
	g_free(fb);
	g_free(dry);
	fixture_close(&f);
}

/*
 * How much more the drop mask looks like itself at @period than at a
 * typical offset, over both axes.
 *
 * A field laid on a lattice of pitch @period agrees with itself strongly
 * when slid by exactly that and weakly at anything else; a scattered
 * field agrees about equally badly everywhere.  The ratio of the one to
 * the mean of the others is therefore a number that says "grid" without
 * anybody having to look at a picture.
 *
 * Shifts start at @period/2 rather than at zero because a drop overlaps
 * ITSELF at small offsets, which says nothing about where the drops are.
 * Both axes are summed, since a lattice is a lattice in each.
 */
static gdouble
lattice_ratio(const guint8 *mask, gint period)
{
	gdouble at_pitch = 0.0, elsewhere = 0.0;
	gint    others = 0;
	gint    s;

	for (s = period / 2; s <= period * 3; s++) {
		gdouble agree = 0.0;
		gint    x, y;

		for (y = 0; y < TEST_H; y++) {
			for (x = 0; x + s < TEST_W; x++) {
				gsize i = (gsize)y * TEST_W + x;

				agree += (gdouble)(mask[i] * mask[i + s]);
			}
		}
		for (y = 0; y + s < TEST_H; y++) {
			for (x = 0; x < TEST_W; x++) {
				gsize i = (gsize)y * TEST_W + x;

				agree += (gdouble)(mask[i]
				                   * mask[i + (gsize)s * TEST_W]);
			}
		}
		if (s == period) {
			at_pitch = agree;
		} else {
			elsewhere += agree;
			others++;
		}
	}
	if (others == 0 || elsewhere <= 0.0)
		return 0.0;
	return at_pitch / (elsewhere / (gdouble)others);
}

/*
 * The metric, tested against a field that IS a grid.
 *
 * Without this the lattice assertion below could be passing because the
 * measure never fires -- which is exactly how a regression test for
 * "it looks regular" fails to do its job.  A drop at the centre of every
 * cell is the thing the shader must never produce, and the number it
 * scores is what makes the real field's number mean something.
 */
static void
test_the_lattice_measure_catches_a_lattice(void)
{
	guint8  *mask = g_malloc0((gsize)TEST_W * TEST_H);
	gint     period = 16;
	gint     cx, cy, x, y;
	gdouble  ratio;

	for (cy = 0; cy * period < TEST_H; cy++) {
		for (cx = 0; cx * period < TEST_W; cx++) {
			for (y = -3; y <= 3; y++) {
				for (x = -3; x <= 3; x++) {
					gint px = cx * period + period / 2 + x;
					gint py = cy * period + period / 2 + y;

					if (px < 0 || px >= TEST_W
					    || py < 0 || py >= TEST_H)
						continue;
					if (x * x + y * y > 9)
						continue;
					mask[(gsize)py * TEST_W + px] = 1;
				}
			}
		}
	}

	ratio = lattice_ratio(mask, period);
	if (g_test_verbose())
		g_print("a real lattice scores %.3f\n", ratio);
	g_assert_cmpfloat(ratio, >, 3.0);
	g_free(mask);
}

static void
test_the_drops_are_not_a_lattice(void)
{
	Fixture f;
	GowlFxRainParams p;
	GowlFxRainClock clock;
	guint8 *dry, *wet, *mask;
	gdouble ratio;
	gsize i, set = 0;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	memset(&clock, 0, sizeof(clock));
	plain_rain(&p);
	/*
	 * A small cell on purpose: the shipped presets put a dozen drops on a
	 * window this size, and a dozen of anything is too few to say whether
	 * they are on a lattice.  Sixteen pixels fills the same window with
	 * some hundreds, which is a sample the measure can speak about.
	 */
	p.cell    = 16.0f;
	p.density = 0.5f;

	p.density = 0.0f;
	dry = render(&f, &p, &clock);
	g_assert_nonnull(dry);
	p.density = 0.5f;
	wet = render(&f, &p, &clock);
	g_assert_nonnull(wet);

	/* Where there is a drop, in one bit per pixel. */
	mask = g_malloc0((gsize)TEST_W * TEST_H);
	for (i = 0; i < (gsize)TEST_W * TEST_H; i++) {
		const guint8 *a = dry + i * 4, *b = wet + i * 4;

		mask[i] = (ABS((gint)a[0] - (gint)b[0]) > 4
		           || ABS((gint)a[1] - (gint)b[1]) > 4
		           || ABS((gint)a[2] - (gint)b[2]) > 4) ? 1 : 0;
		set += mask[i];
	}
	/* Enough of them to be measuring the field rather than the noise. */
	g_assert_cmpuint(set, >, 1500);

	ratio = lattice_ratio(mask, (gint)p.cell);
	if (g_test_verbose())
		g_print("drop pixels: %zu; lattice ratio at the cell pitch: "
		        "%.3f\n", set, ratio);

	/*
	 * 1.0 is "the field agrees with itself at the cell pitch exactly as
	 * much as anywhere else", which is what scattered drops do.  The
	 * synthetic lattice above scores several times that.  The bound is
	 * generous because two turned layers at different pitches still leave
	 * a little structure, and because the point is to catch the effect
	 * REGRESSING to a lattice, not to police the last few percent.
	 */
	g_assert_cmpfloat(ratio, <, 1.6);

	g_free(mask);
	g_free(dry);
	g_free(wet);
	fixture_close(&f);
}

static void
test_the_corners_are_cut_out(void)
{
	Fixture f;
	GowlFxRainParams p;
	GowlFxRainClock clock;
	guint8 *frame;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	memset(&clock, 0, sizeof(clock));
	plain_rain(&p);
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
 * The clocks, which need no GPU.
 *
 * Their whole job is to stay in a range a 32-bit float represents exactly
 * while never landing anywhere but a whole cycle.  Both halves are
 * invisible until they are not: a value outside the range makes the drops
 * step, and a wrap to the wrong place snaps every one of them sideways,
 * once, some tens of minutes in.
 */
static void
test_the_clock_stays_where_a_float_can_hold_it(void)
{
	GowlFxRainClock clock;
	gint i;

	memset(&clock, 0, sizeof(clock));

	/* Ten minutes at sixty hertz, in a downpour. */
	for (i = 0; i < 36000; i++)
		gowl_fx_rain_advance(&clock, 1.0 / 60.0, 1.5, 5.0);

	g_assert_cmpfloat(clock.life, >=, 0.0);
	g_assert_cmpfloat(clock.life, <, 1.0);
	g_assert_cmpfloat(fabs((gdouble)(gfloat)clock.life - clock.life),
	                  <, 1e-6);

	for (i = 0; i < 3; i++) {
		g_assert_cmpfloat(clock.run[i], >=, 0.0);
		g_assert_cmpfloat(clock.run[i], <, 1.0);
		g_assert_cmpfloat(fabs((gdouble)(gfloat)clock.run[i]
		                       - clock.run[i]), <, 1e-6);
	}

	/* The three run at different rates, or the three column layers fall
	 * in step and the window has visible ranks of drops. */
	g_assert_cmpfloat(fabs(clock.run[0] - clock.run[1]), >, 1e-6);
	g_assert_cmpfloat(fabs(clock.run[1] - clock.run[2]), >, 1e-6);
}

static void
test_a_stall_is_not_a_cloudburst(void)
{
	GowlFxRainClock slow, stalled;
	gint i;

	/* Coming back from a VT switch or a closed lid hands the clock a gap
	 * of however long that was.  A quarter of a second is the most it may
	 * act on: past that every drop teleports down the pane, which is far
	 * more jarring than a moment of slow motion. */
	memset(&slow, 0, sizeof(slow));
	memset(&stalled, 0, sizeof(stalled));

	for (i = 0; i < 15; i++)
		gowl_fx_rain_advance(&slow, 1.0 / 60.0, 1.0, 8.0);
	gowl_fx_rain_advance(&stalled, 900.0, 1.0, 8.0);

	g_assert_cmpfloat(fabs(slow.life - stalled.life), <, 1e-9);
	for (i = 0; i < 3; i++)
		g_assert_cmpfloat(fabs(slow.run[i] - stalled.run[i]), <, 1e-9);

	/* And time that has not passed does not move it. */
	gowl_fx_rain_advance(&stalled, 0.0, 1.0, 8.0);
	gowl_fx_rain_advance(&stalled, -5.0, 1.0, 8.0);
	g_assert_cmpfloat(fabs(slow.life - stalled.life), <, 1e-9);
	for (i = 0; i < 3; i++)
		g_assert_cmpfloat(fabs(slow.run[i] - stalled.run[i]), <, 1e-9);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/rain-render/dry-pane",
	                test_a_dry_pane_passes_straight_through);
	g_test_add_func("/rain-render/bends",
	                test_the_drops_bend_what_is_behind_them);
	g_test_add_func("/rain-render/moves", test_the_rain_actually_moves);
	g_test_add_func("/rain-render/downwards", test_the_drops_run_downwards);
	g_test_add_func("/rain-render/lattice-measure",
	                test_the_lattice_measure_catches_a_lattice);
	g_test_add_func("/rain-render/not-a-lattice",
	                test_the_drops_are_not_a_lattice);
	g_test_add_func("/rain-render/corners", test_the_corners_are_cut_out);
	g_test_add_func("/rain-render/clock",
	                test_the_clock_stays_where_a_float_can_hold_it);
	g_test_add_func("/rain-render/stall", test_a_stall_is_not_a_cloudburst);
	return g_test_run();
}
