/* test-effects-render.c -- the soap film, the embers, the submerged view,
 * the dew and the bokeh
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Five effects in one file because they share a fixture.  What they do
 * NOT share is a failure mode, and each has exactly one claim that
 * cannot be checked by looking at it:
 *
 *   THE SOAP FILM CLAIMS TO MAKE COLOUR OUT OF NOTHING.  Every other
 *   backdrop in the tree moves the wallpaper's colour around or tints
 *   it; this one is supposed to CREATE colour, out of a colourless film
 *   over a colourless wall, by interference.  A shader that had quietly
 *   become a tint would still look iridescent over a photograph --
 *   photographs have colour in them -- so it is asked over a flat grey,
 *   where there is none to move.
 *
 *   THE EMBERS CLAIM A TEMPERATURE.  The colour is the Planckian locus
 *   and the brightness is its FOURTH POWER, so halving the temperature
 *   should not halve anything: it should take the blue out entirely and
 *   drop the brightness by about sixteen.  An alpha ramp with an orange
 *   gradient would pass any test that only asked "is it orange".
 *
 *   THE SUBMERGED VIEW CLAIMS BEER-LAMBERT.  Red is absorbed roughly
 *   twenty times faster than blue, so depth must not dim the picture --
 *   it must dim it UNEVENLY, in a fixed order.  A blue tint with a
 *   distance fade is the thing this is not, and the two are
 *   indistinguishable until you measure the channels separately.
 *
 *   THE DEW CLAIMS THE RAIN'S OPTICS.  A drop is a ball lens, so past a
 *   depth of two radii it turns what is behind it UPSIDE DOWN.  Asserted
 *   the way tests/test-weather-render.c asserts the bubble: a ramp
 *   behind the pane, and the sign of its slope read back inside the
 *   beads.  The bubble's answer there and the dew's answer here are
 *   opposite, which is the point -- the two shaders are the same
 *   arithmetic with one character different.
 *
 *   THE BOKEH CLAIMS TO BE A DISC.  This is the whole difference between
 *   it and the box blur beside it: an out-of-focus point of light is a
 *   FILLED SHAPE with a hard edge, not a peak that falls away.  Asserted
 *   on the profile through a single bright dot -- a disc is flat across
 *   its middle and a Gaussian is not, and no amount of looking at a
 *   screenshot settles which one is on screen.
 *
 * The lightning is at the end and needs no GPU at all: what is wrong
 * with a fake storm is the SHAPE OF THE ENVELOPE and the SPACING OF THE
 * GAPS, and both live in the advance function.
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

/*
 * Which wallpaper the fixture paints.  Three, and each exists for one
 * assertion that the others cannot make:
 *
 *   GREY    no colour anywhere, so any colour in the output was MADE
 *   RAMP    a pure horizontal gradient, so the rendered value at a pixel
 *           reads back WHERE the shader sampled
 *   DOT     black with one bright spot, so a blur kernel's shape is
 *           drawn directly on the output
 *   BLACK   nothing at all, so what is on screen was EMITTED.  The one
 *           the embers need: a spark over a mid-grey wall clips at the
 *           top of the range, and a clipped measurement of a fourth
 *           power is a measurement of the clip
 */
typedef enum {
	WP_GREY,
	WP_RAMP,
	WP_DOT,
	WP_BLACK
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

			if (kind == WP_BLACK) {
				p[0] = p[1] = p[2] = 0x00;
			} else if (kind == WP_GREY) {
				/* Exactly neutral.  Any difference between the channels
				 * in the output was made by the shader, and there is
				 * nowhere else it could have come from. */
				p[0] = p[1] = p[2] = 0x80;
			} else if (kind == WP_RAMP) {
				guint8 v = (guint8)(28 + (x * 200) / TEST_W);

				p[0] = p[1] = p[2] = v;
			} else {
				/*
				 * One bright spot on black.  A blur kernel applied to
				 * this IS the kernel, drawn.
				 *
				 * SIXTEEN PIXELS ACROSS, not three, and that is not
				 * arbitrary: a disc of radius R sampled N times has its
				 * taps about R/sqrt(N/pi) apart, which at the settings
				 * here is nine pixels.  A source smaller than the tap
				 * spacing falls BETWEEN the taps at some offsets and on
				 * them at others, and what gets drawn is a ring rather
				 * than a disc -- which is a real property of sparse
				 * sampling and is what `bokeh-samples' is for, but it
				 * is not the thing this case is asking about.
				 */
				gint dx = x - TEST_W / 2;
				gint dy = y - TEST_H / 2;

				if (dx * dx + dy * dy <= 64)
					p[0] = p[1] = p[2] = 0xff;
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

/* ARGB8888 little-endian: byte 0 is blue, 1 green, 2 red. */
#define PX_B(p) ((gint)(p)[0])
#define PX_G(p) ((gint)(p)[1])
#define PX_R(p) ((gint)(p)[2])

static gdouble
mean_channel(const guint8 *px, gint ch)
{
	gdouble sum = 0.0;
	gsize   i;

	for (i = 0; i < (gsize)TEST_W * TEST_H; i++)
		sum += px[i * 4 + ch];
	return sum / ((gdouble)TEST_W * TEST_H);
}

/*
 * Light ADDED, per pixel, over a baseline.
 *
 * Summed one-sided, so the anti-aliased border of the rounded rect --
 * which is darker than the wallpaper by construction -- cannot cancel
 * out what a handful of bright sparks put on.  A plain mean of the pane
 * did exactly that and read as zero while the peak was 255.
 */
static gdouble
added_light(const guint8 *px, gint ch, gint base)
{
	gdouble sum = 0.0;
	gsize   i;

	for (i = 0; i < (gsize)TEST_W * TEST_H; i++)
		sum += MAX((gint)px[i * 4 + ch] - base, 0);
	return sum / ((gdouble)TEST_W * TEST_H);
}

static gint
max_channel(const guint8 *px, gint ch)
{
	gint best = 0;
	gsize i;

	for (i = 0; i < (gsize)TEST_W * TEST_H; i++)
		best = MAX(best, (gint)px[i * 4 + ch]);
	return best;
}

/* How far apart the channels are, averaged: 0 for a grey image and
 * larger the more coloured it is. */
static gdouble
mean_saturation(const guint8 *px)
{
	gdouble sum = 0.0;
	gsize   i;

	for (i = 0; i < (gsize)TEST_W * TEST_H; i++) {
		const guint8 *p = px + i * 4;
		gint hi = MAX(MAX(PX_R(p), PX_G(p)), PX_B(p));
		gint lo = MIN(MIN(PX_R(p), PX_G(p)), PX_B(p));

		sum += hi - lo;
	}
	return sum / ((gdouble)TEST_W * TEST_H);
}

/* ── The soap film ───────────────────────────────────────────────── */

typedef struct {
	GowlFxSoapParams p;
	GowlFxSoapClock  c;
} SoapCase;

static gboolean
draw_soap(GowlFxPass *pass, const GowlFxTexture *soft,
          const GowlFxTexture *sharp, gpointer user)
{
	SoapCase *s = user;

	return gowl_fx_pass_soap(pass, soft, sharp, &s->p, &s->c);
}

static void
soap_case(SoapCase *s)
{
	memset(s, 0, sizeof(*s));
	gowl_fx_soap_params_init(&s->p);
	s->p.width  = TEST_W;
	s->p.height = TEST_H;
	/* Early in a film's life, well before it starts to pop. */
	s->c.life = 3.25;
}

/*
 * A soap film makes colour out of a grey wall.
 *
 * The decisive one.  The wallpaper is exactly neutral, so every channel
 * arrives at the shader equal -- there is no colour anywhere to move
 * around, tint, or sample from.  Any difference between the channels in
 * the output was computed, and the only thing in the shader that can
 * compute one is the interference formula, which divides the thickness
 * by a different wavelength per channel.
 *
 * The control is the same film with the gain at zero, which is the one
 * change that switches the interference off and nothing else.  Without
 * it this would also pass for a shader that had a rainbow painted into
 * it.
 */
static void
test_a_film_makes_colour_out_of_grey(void)
{
	Fixture f;
	SoapCase s;
	guint8 *lit, *off;
	gdouble sat_on, sat_off;

	if (!fixture_open(&f, WP_GREY)) {
		fixture_close(&f);
		g_test_skip("no GL renderer here");
		return;
	}

	soap_case(&s);
	lit = render(&f, draw_soap, &s, "soap film");
	g_assert_nonnull(lit);

	s.p.gain = 0.0f;
	off = render(&f, draw_soap, &s, "soap film");
	g_assert_nonnull(off);

	sat_on  = mean_saturation(lit);
	sat_off = mean_saturation(off);

	if (g_test_verbose())
		g_test_message("soap over grey: saturation %.2f with the "
		               "interference, %.2f without", sat_on, sat_off);

	/* Plainly coloured, and plainly not coloured without it.  The
	 * threshold is loose on purpose -- what is being asserted is that
	 * one of these is colour and the other is not, not a particular
	 * amount of it. */
	g_assert_cmpfloat(sat_on, >, 8.0);
	g_assert_cmpfloat(sat_off, <, 2.0);

	g_free(lit);
	g_free(off);
	fixture_close(&f);
}

/*
 * The bands lie ACROSS the pane, because gravity does.
 *
 * A band is a line of constant thickness, and a draining film is thin at
 * the top and thick at the bottom -- so the bands are horizontal.  A
 * shader whose thickness field had lost its vertical profile would still
 * be iridescent and would still pass the case above; it would just look
 * like an oil slick, which is a different thing that happens to a
 * different liquid.
 *
 * Measured as variance down a column against variance across a row, on
 * the same image, with the plumes turned down so what is left is the
 * profile.
 */
static void
test_the_bands_lie_across_the_pane(void)
{
	Fixture f;
	SoapCase s;
	guint8 *px;
	gdouble down = 0.0, across = 0.0;
	gint    i;

	if (!fixture_open(&f, WP_GREY)) {
		fixture_close(&f);
		g_test_skip("no GL renderer here");
		return;
	}

	soap_case(&s);
	s.p.turbulence = 0.0f;   /* the profile alone */
	s.p.meniscus   = 0.0f;   /* and not the border */
	px = render(&f, draw_soap, &s, "soap film");
	g_assert_nonnull(px);

	/* Variance of the green channel down the middle column, and across
	 * the middle row. */
	for (i = 1; i < TEST_H - 1; i++) {
		const guint8 *a = px + ((gsize)i * TEST_W + TEST_W / 2) * 4;
		const guint8 *b = px + ((gsize)(i - 1) * TEST_W + TEST_W / 2) * 4;

		down += ABS(PX_G(a) - PX_G(b));
	}
	for (i = 1; i < TEST_W - 1; i++) {
		const guint8 *a = px + ((gsize)(TEST_H / 2) * TEST_W + i) * 4;
		const guint8 *b = px + ((gsize)(TEST_H / 2) * TEST_W + i - 1) * 4;

		across += ABS(PX_G(a) - PX_G(b));
	}

	if (g_test_verbose())
		g_test_message("soap bands: %.0f total change down a column, "
		               "%.0f across a row", down, across);

	/* Several times more change down than across.  Not "any more": a
	 * factor is what says the bands have a direction. */
	g_assert_cmpfloat(down, >, across * 4.0);

	g_free(px);
	fixture_close(&f);
}

/*
 * A film thin enough stops colouring at all.
 *
 * The black-film limit, and the one headline feature in this directory
 * that is a LIMIT rather than a drawing: as the thickness goes to zero
 * the phase goes to zero and sin^2 of it goes to zero for every
 * wavelength at once, so the film stops interacting with light.  Seen
 * through, that means the wallpaper arrives untouched.
 *
 * If this were a painted rainbow it would not care what the thickness
 * was, and thinning the film would change the colours rather than
 * removing them.
 */
static void
test_a_film_thin_enough_stops_colouring(void)
{
	Fixture f;
	SoapCase s;
	guint8 *thick, *thin;
	gdouble sat_thick, sat_thin;

	if (!fixture_open(&f, WP_GREY)) {
		fixture_close(&f);
		g_test_skip("no GL renderer here");
		return;
	}

	soap_case(&s);
	s.p.turbulence = 0.0f;
	s.p.meniscus   = 0.0f;
	thick = render(&f, draw_soap, &s, "soap film");
	g_assert_nonnull(thick);

	/* Sixty nanometres is about a fifth of the way to the first
	 * constructive order in the blue, which is thin enough that all
	 * three channels are still on the flat part of the sine. */
	s.p.thickness = 60.0f;
	thin = render(&f, draw_soap, &s, "soap film");
	g_assert_nonnull(thin);

	sat_thick = mean_saturation(thick);
	sat_thin  = mean_saturation(thin);

	if (g_test_verbose())
		g_test_message("soap thinning: saturation %.2f thick, %.2f thin",
		               sat_thick, sat_thin);

	g_assert_cmpfloat(sat_thin, <, sat_thick * 0.4);

	g_free(thick);
	g_free(thin);
	fixture_close(&f);
}

/* ── Embers ──────────────────────────────────────────────────────── */

typedef struct {
	GowlFxEmbersParams p;
	GowlFxEmbersClock  c;
} EmberCase;

static gboolean
draw_embers(GowlFxPass *pass, const GowlFxTexture *soft,
            const GowlFxTexture *sharp, gpointer user)
{
	EmberCase *e = user;

	return gowl_fx_pass_embers(pass, soft, sharp, &e->p, &e->c);
}

static void
ember_case(EmberCase *e)
{
	memset(e, 0, sizeof(*e));
	gowl_fx_embers_params_init(&e->p);
	e->p.width  = TEST_W;
	e->p.height = TEST_H;
	/* Everything that is not a spark turned off, so what is measured is
	 * the sparks: no hearth glow, no haze, no warm tint over the pane. */
	e->p.hearth = 0.0f;
	e->p.haze   = 0.0f;
	e->p.glow   = 0.0f;
	e->p.tint[0] = e->p.tint[1] = e->p.tint[2] = 1.0f;
	e->p.density = 1.0f;
	e->p.ash     = 0.0f;
	e->p.flicker = 0.0f;
	e->p.ember   = 4.0f;
	/*
	 * Held well off the top of the range.  A spark at 2300 K is
	 * brighter than white on its own, so over anything but black it
	 * clips -- and a clipped reading of a fourth power is a reading of
	 * the clip.  The wallpaper is black and this keeps the peak inside
	 * the byte.
	 */
	e->p.brightness = 0.35f;
	e->c.rise[0] = 2.3;
	e->c.rise[1] = 5.1;
	e->c.rise[2] = 7.7;
}

/*
 * A cooler fire is redder AND very much dimmer.
 *
 * Two claims in one measurement, and the second is the one that matters.
 *
 * REDDER is the Planckian locus: blue does not appear below about
 * 1900 K, so a cool ember has none at all while a hot one does.
 *
 * DIMMER BY A LOT is Stefan-Boltzmann, and it is what separates this
 * from a particle system with an orange gradient.  Radiated power goes
 * as the FOURTH power of the temperature, so dropping from 2300 K to
 * 1150 K should cost about sixteen times the brightness -- not two.
 * Anything with a linear fade would pass "is it dimmer" and fail this.
 */
static void
test_a_cooler_ember_is_redder_and_much_dimmer(void)
{
	Fixture f;
	EmberCase hot, cool;
	guint8 *a, *b;
	gdouble hot_b, cool_b, hot_r, cool_r, ratio;

	if (!fixture_open(&f, WP_BLACK)) {
		fixture_close(&f);
		g_test_skip("no GL renderer here");
		return;
	}

	ember_case(&hot);
	hot.p.temperature = 2300.0f;
	hot.p.cool = 0.0f;         /* so the reading is the BIRTH temperature */
	cool = hot;
	cool.p.temperature = 1150.0f;

	a = render(&f, draw_embers, &hot, "embers");
	b = render(&f, draw_embers, &cool, "embers");
	g_assert_nonnull(a);
	g_assert_nonnull(b);

	/* Against a black wallpaper: everything on screen was emitted. */
	hot_b  = added_light(a, 0, 0);
	cool_b = added_light(b, 0, 0);
	hot_r  = added_light(a, 2, 0);
	cool_r = added_light(b, 2, 0);

	if (g_test_verbose())
		g_test_message("embers: hot adds %.2f red / %.2f blue "
		               "(peak red %d), cool adds %.2f red / %.2f blue "
		               "(peak red %d)",
		               hot_r, hot_b, max_channel(a, 2),
		               cool_r, cool_b, max_channel(b, 2));

	/* The hot one puts some blue on the pane; the cool one puts none.
	 * That is the locus, and it is a hard threshold rather than a
	 * gradient. */
	g_assert_cmpfloat(hot_b, >, 0.05);
	g_assert_cmpfloat(cool_b, <, hot_b * 0.25);

	/* And the fourth power.  Halving the temperature should cost far
	 * more than half the light; anything above a quarter means the
	 * exponent has gone. */
	g_assert_cmpfloat(hot_r, >, 0.15);
	ratio = cool_r / hot_r;
	if (g_test_verbose())
		g_test_message("embers: cool is %.3f of hot in red "
		               "(a fourth power would be about 0.06)", ratio);
	/* 0.25 is generous on purpose: what is being refused is a LINEAR
	 * fade, which would sit at 0.5.  The measured value is around a
	 * sixteenth, which is the fourth power to the accuracy a sample of
	 * a few dozen sparks allows. */
	g_assert_cmpfloat(ratio, <, 0.25);

	g_free(a);
	g_free(b);
	fixture_close(&f);
}

/*
 * The sparks go UP, and the heat haze moves.
 *
 * Two frames a moment apart, differenced: the picture has to change, and
 * the change has to be a translation upward rather than a twinkle in
 * place.  Measured by correlating the second frame against the first
 * shifted by a few rows, upward and downward, and requiring the upward
 * shift to fit better.
 */
static void
test_the_sparks_go_up(void)
{
	Fixture f;
	EmberCase e;
	guint8 *first, *later;
	gdouble best = -1.0;
	gint    best_shift = 0;
	gint    x, y, sh;
	const gint reach = 12;

	if (!fixture_open(&f, WP_BLACK)) {
		fixture_close(&f);
		g_test_skip("no GL renderer here");
		return;
	}

	ember_case(&e);
	first = render(&f, draw_embers, &e, "embers");
	g_assert_nonnull(first);

	/*
	 * A SMALL step.  A spark leaves the fire fast -- at the shipped
	 * drag it crosses a quarter of the pane in a tenth of a second --
	 * so a step chosen by eye moves it clean past the search window and
	 * every correlation comes out zero, which is what the first version
	 * of this measured.
	 */
	gowl_fx_embers_advance(&e.c, 0.02, 1.0, 1.0);
	later = render(&f, draw_embers, &e, "embers");
	g_assert_nonnull(later);

	/*
	 * Which vertical shift best explains the second frame in terms of
	 * the first.  y grows DOWNWARD, so a spark now at y was at y + s
	 * with s POSITIVE if it has risen.
	 *
	 * Searched rather than assumed: the rise decelerates, so how far
	 * anything moved depends on where it was, and a single hand-picked
	 * shift tests the guess rather than the motion.
	 */
	for (sh = -reach; sh <= reach; sh++) {
		gdouble fit = 0.0;

		for (y = reach; y < TEST_H - reach; y++) {
			for (x = 0; x < TEST_W; x++) {
				const guint8 *now = later + ((gsize)y * TEST_W + x) * 4;
				const guint8 *was =
					first + ((gsize)(y + sh) * TEST_W + x) * 4;

				/* Over a black wallpaper, every lit pixel is a spark. */
				fit += (gdouble)PX_R(now) * PX_R(was);
			}
		}
		if (fit > best) {
			best = fit;
			best_shift = sh;
		}
	}

	if (g_test_verbose())
		g_test_message("embers: the second frame fits the first shifted "
		               "by %+d rows (positive is upward motion); peaks "
		               "%d then %d", best_shift,
		               max_channel(first, 2), max_channel(later, 2));

	/* There was light to correlate at all... */
	g_assert_cmpint(max_channel(first, 2), >, 20);
	/* ...and it went up. */
	g_assert_cmpint(best_shift, >, 0);

	g_free(first);
	g_free(later);
	fixture_close(&f);
}

/* ── Under water ─────────────────────────────────────────────────── */

typedef struct {
	GowlFxSubmergedParams p;
	GowlFxSubmergedClock  c;
} DeepCase;

static gboolean
draw_submerged(GowlFxPass *pass, const GowlFxTexture *soft,
               const GowlFxTexture *sharp, gpointer user)
{
	DeepCase *d = user;

	return gowl_fx_pass_submerged(pass, soft, sharp, &d->p, &d->c);
}

static void
deep_case(DeepCase *d)
{
	memset(d, 0, sizeof(*d));
	gowl_fx_submerged_params_init(&d->p);
	d->p.width  = TEST_W;
	d->p.height = TEST_H;
	/* Everything that ADDS light switched off, so what is measured is
	 * the absorption and nothing else. */
	d->p.caustics = 0.0f;
	d->p.shafts   = 0.0f;
	d->p.motes    = 0.0f;
	d->p.surface  = 0.0f;
	d->p.sway     = 0.0f;
	d->p.murk     = 0.0f;
	/* And nothing scattered back in, which would put the colour back by
	 * a different route and hide the thing being asserted. */
	d->p.water[0] = d->p.water[1] = d->p.water[2] = 0.0f;
}

/*
 * Depth eats the red first, and the blue last.
 *
 * This is Beer-Lambert and it is the whole claim of the effect: the
 * water is not a blue filter, it is three different absorption
 * coefficients and one exponential, and the ORDER they take the colours
 * out in is what a diver sees.
 *
 * A blue tint with a distance fade would darken all three channels
 * together, and would be indistinguishable from this in a screenshot.
 * Measured on a grey wallpaper, where the three channels start equal.
 */
static void
test_depth_eats_the_red_first(void)
{
	Fixture f;
	DeepCase shallow, deep;
	guint8 *a, *b;
	gdouble r_keep, g_keep, b_keep;

	if (!fixture_open(&f, WP_GREY)) {
		fixture_close(&f);
		g_test_skip("no GL renderer here");
		return;
	}

	deep_case(&shallow);
	shallow.p.depth = 0.0f;
	deep = shallow;
	deep.p.depth = 4.0f;

	a = render(&f, draw_submerged, &shallow, "submerged");
	b = render(&f, draw_submerged, &deep, "submerged");
	g_assert_nonnull(a);
	g_assert_nonnull(b);

	r_keep = mean_channel(b, 2) / MAX(mean_channel(a, 2), 1.0);
	g_keep = mean_channel(b, 1) / MAX(mean_channel(a, 1), 1.0);
	b_keep = mean_channel(b, 0) / MAX(mean_channel(a, 0), 1.0);

	if (g_test_verbose())
		g_test_message("submerged at 4 m: red keeps %.3f, green %.3f, "
		               "blue %.3f", r_keep, g_keep, b_keep);

	/* Strictly ordered, and the red gap is large: at four metres and
	 * 0.42 per metre the red is down to about a fifth while the blue
	 * has barely moved. */
	g_assert_cmpfloat(r_keep, <, g_keep);
	g_assert_cmpfloat(g_keep, <, b_keep);
	g_assert_cmpfloat(r_keep, <, 0.55);
	g_assert_cmpfloat(b_keep, >, 0.80);

	g_free(a);
	g_free(b);
	fixture_close(&f);
}

/*
 * The caustics move, and they move by RESHAPING.
 *
 * A caustic net does not slide across the bottom of a pool -- the waves
 * pass through it and the net writhes in place.  That is why the feature
 * points of the cellular field orbit rather than translate, and it is
 * the difference between this and a scrolling texture.
 *
 * Asserted as: the picture changes a great deal between two moments, and
 * it changes MORE than it does when the whole image is shifted -- so no
 * single translation explains the change.
 */
static void
test_the_caustics_reshape_rather_than_slide(void)
{
	Fixture f;
	DeepCase d;
	guint8 *first, *later;
	gdouble same = 0.0, shifted = 0.0;
	gint    x, y;
	const gint shift = 3;

	if (!fixture_open(&f, WP_GREY)) {
		fixture_close(&f);
		g_test_skip("no GL renderer here");
		return;
	}

	deep_case(&d);
	d.p.caustics = 1.4f;
	d.p.depth = 1.0f;

	first = render(&f, draw_submerged, &d, "submerged");
	g_assert_nonnull(first);
	gowl_fx_submerged_advance(&d.c, 0.35, 1.0, 22.0);
	later = render(&f, draw_submerged, &d, "submerged");
	g_assert_nonnull(later);

	for (y = shift; y < TEST_H - shift; y++) {
		for (x = shift; x < TEST_W - shift; x++) {
			const guint8 *n = later + ((gsize)y * TEST_W + x) * 4;
			const guint8 *o = first + ((gsize)y * TEST_W + x) * 4;
			const guint8 *s = first + ((gsize)y * TEST_W + x - shift) * 4;

			same    += ABS(PX_G(n) - PX_G(o));
			shifted += ABS(PX_G(n) - PX_G(s));
		}
	}

	if (g_test_verbose())
		g_test_message("caustics: %.0f change in place, %.0f against a "
		               "shifted copy", same, shifted);

	/* It moved at all... */
	g_assert_cmpfloat(same, >, 1000.0);
	/* ...and shifting the old frame sideways does not explain it, which
	 * is what "reshaping" means.  A scrolling texture would match its
	 * own shifted copy far better than it matches itself. */
	g_assert_cmpfloat(shifted, >, same * 0.75);

	g_free(first);
	g_free(later);
	fixture_close(&f);
}

/* ── Dew on a web ────────────────────────────────────────────────── */

typedef struct {
	GowlFxDewParams p;
	GowlFxDewClock  c;
} DewCase;

static gboolean
draw_dew(GowlFxPass *pass, const GowlFxTexture *soft,
         const GowlFxTexture *sharp, gpointer user)
{
	DewCase *d = user;

	return gowl_fx_pass_dew(pass, soft, sharp, &d->p, &d->c);
}

static void
dew_case(DewCase *d)
{
	memset(d, 0, sizeof(*d));
	gowl_fx_dew_params_init(&d->p);
	d->p.width  = TEST_W;
	d->p.height = TEST_H;
	d->p.fog = 0.0f;   /* the wallpaper unblurred, so a sample reads back */
}

/*
 * A bead turns what is behind it upside down.
 *
 * The dew's one physical claim, and it is the rain's: a drop of water is
 * a BALL LENS, so past a depth of two of its own radii the image inside
 * it is inverted.  tests/test-weather-render.c asserts the opposite of
 * this for the carbonation -- a gas bubble in liquid is a DIVERGING lens
 * and never inverts -- and the two shaders differ by one sign.  Having
 * both asserted is what makes either of them mean anything.
 *
 * Read off a ramp: the wallpaper is a pure horizontal gradient, so the
 * rendered value at a pixel says where in x the shader sampled.  Inside
 * a converging lens deep enough to invert, the local slope RUNS
 * BACKWARDS.
 */
static void
test_a_bead_inverts_what_is_behind_it(void)
{
	Fixture f;
	DewCase near, far;
	guint8 *flat, *lensed;
	gint    reversed = 0, forward = 0;
	gint    x, y;

	if (!fixture_open(&f, WP_RAMP)) {
		fixture_close(&f);
		g_test_skip("no GL renderer here");
		return;
	}

	dew_case(&near);
	/* Everything that is not the lens turned off, so a slope reading is
	 * a slope reading: no silk lines, no glint, no rim. */
	near.p.silk  = 0.0f;
	near.p.glint = 0.0f;
	near.p.rim   = 0.0f;
	near.p.dispersion = 0.0f;
	near.p.drop = 9.0f;
	far = near;

	/* A depth of zero is a flat window: whatever the drops cover, they
	 * cover with an undisplaced sample.  That is the control. */
	near.p.depth = 0.0f;
	far.p.depth  = 6.0f;

	flat   = render(&f, draw_dew, &near, "dew");
	lensed = render(&f, draw_dew, &far, "dew");
	g_assert_nonnull(flat);
	g_assert_nonnull(lensed);

	/*
	 * Only where the two differ -- which is exactly the beads, because
	 * the depth is the only thing that changed.  There the local slope
	 * of the ramp is measured; a lens past its focal plane reverses it.
	 */
	for (y = 2; y < TEST_H - 2; y++) {
		for (x = 2; x < TEST_W - 2; x++) {
			const guint8 *a = flat + ((gsize)y * TEST_W + x) * 4;
			const guint8 *l = lensed + ((gsize)y * TEST_W + x) * 4;
			const guint8 *lm = lensed + ((gsize)y * TEST_W + x - 2) * 4;
			const guint8 *lp = lensed + ((gsize)y * TEST_W + x + 2) * 4;
			gint slope;

			if (ABS(PX_G(a) - PX_G(l)) < 6)
				continue;   /* not inside a bead */

			slope = PX_G(lp) - PX_G(lm);
			if (slope < -6)
				reversed++;
			else if (slope > 6)
				forward++;
		}
	}

	if (g_test_verbose())
		g_test_message("dew beads at depth 6: %d pixels with the ramp "
		               "reversed, %d with it forward", reversed, forward);

	/* There has to be something to measure at all... */
	g_assert_cmpint(reversed + forward, >, 200);
	/* ...and most of it runs backwards, which a converging lens past its
	 * focus does and nothing else in this file does. */
	g_assert_cmpint(reversed, >, forward);

	g_free(flat);
	g_free(lensed);
	fixture_close(&f);
}

/*
 * The silk is BRIGHT, and it is a web rather than a haze.
 *
 * Spider silk is a transparent fibre a couple of microns across: what
 * reaches the eye from it is scattered light, so a photographed web is
 * always PALE threads over whatever is behind.  Drawn dark it reads as a
 * crack in the screen.
 *
 * And it has to be thin and structured rather than a general lightening:
 * a small fraction of the pane is much brighter than the wallpaper, not
 * all of it slightly.
 */
static void
test_the_silk_is_bright_and_thin(void)
{
	Fixture f;
	DewCase d;
	guint8 *px;
	gint    bright = 0, i;
	gdouble mean;

	if (!fixture_open(&f, WP_GREY)) {
		fixture_close(&f);
		g_test_skip("no GL renderer here");
		return;
	}

	dew_case(&d);
	d.p.glint = 0.0f;    /* the threads, not the drops */
	d.p.drop  = 0.6f;
	d.p.silk  = 1.0f;

	px = render(&f, draw_dew, &d, "dew");
	g_assert_nonnull(px);

	for (i = 0; i < TEST_W * TEST_H; i++) {
		if (PX_G(px + (gsize)i * 4) > 128 + 18)
			bright++;
	}
	mean = mean_channel(px, 1);

	if (g_test_verbose())
		g_test_message("dew silk: %d of %d pixels clearly brighter than "
		               "the wall, mean %.1f (wall is 128)",
		               bright, TEST_W * TEST_H, mean);

	/* Some threads... */
	g_assert_cmpint(bright, >, 300);
	/* ...and not a wash: well under a fifth of the pane. */
	g_assert_cmpint(bright, <, TEST_W * TEST_H / 5);

	g_free(px);
	fixture_close(&f);
}

/* ── Bokeh ───────────────────────────────────────────────────────── */

/*
 * An out-of-focus point of light is a DISC, not a smudge.
 *
 * The entire difference between this and the box blur next to it, and
 * the reason a photograph's background looks expensive.  A Gaussian has
 * its maximum in the middle and falls away from it; an aperture is
 * evenly filled and stops at an edge.
 *
 * Measured on a single bright dot: the profile through the middle of the
 * result.  A disc is FLAT across its middle -- the value two thirds of
 * the way out is nearly the value at the centre -- and a blur is not.
 * Both are run on the same fixture, so nothing but the kernel differs.
 */
static void
test_a_bright_point_becomes_a_disc(void)
{
	Fixture f;
	GowlFxBokehParams lens;
	GowlFxTexture out;
	guint8 *px;
	struct wlr_buffer *buffer;
	struct wlr_texture *texture;
	struct wlr_texture_read_pixels_options opts;
	GowlFxPass *pass;
	GowlFxQuad quad;
	gdouble centre, mid, edge;
	gdouble bokeh_flat, blur_flat;
	gint pass_no;

	if (!fixture_open(&f, WP_DOT)) {
		fixture_close(&f);
		g_test_skip("no GL renderer here");
		return;
	}

	memset(&out, 0, sizeof(out));
	bokeh_flat = blur_flat = 0.0;

	for (pass_no = 0; pass_no < 2; pass_no++) {
		gboolean ok;

		if (pass_no == 0) {
			gowl_fx_bokeh_params_init(&lens);
			lens.radius    = 40.0f;
			lens.downscale = 1;
			lens.samples   = 64;
			lens.blades    = 0;     /* a circle, so the profile is radial */
			lens.edge      = 0.0f;  /* no rim, so flatness is flatness */
			ok = gowl_fx_texture_bokeh(f.gl, &out, &f.wallpaper, &lens);
			if (!ok)
				g_error("the bokeh shader would not build");
		} else {
			ok = gowl_fx_texture_blur(f.gl, &out, &f.wallpaper, 1, 4);
			g_assert_true(ok);
		}

		/* Read the softened texture back by drawing it into a buffer. */
		buffer = wlr_swapchain_acquire(f.swapchain);
		g_assert_nonnull(buffer);
		pass = gowl_fx_pass_begin(f.gl, buffer);
		g_assert_nonnull(pass);
		{
			gfloat clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

			gowl_fx_pass_clear(pass, clear);
		}
		gowl_fx_quad_init(&quad);
		quad.texture = out.tex;
		gowl_fx_pass_quad(pass, &quad);
		gowl_fx_pass_end(pass);

		texture = wlr_texture_from_buffer(f.renderer, buffer);
		g_assert_nonnull(texture);
		px = g_malloc0((gsize)TEST_W * TEST_H * 4);
		memset(&opts, 0, sizeof(opts));
		opts.data   = px;
		opts.format = DRM_FORMAT_ARGB8888;
		opts.stride = TEST_W * 4;
		g_assert_true(wlr_texture_read_pixels(texture, &opts));
		wlr_texture_destroy(texture);
		wlr_buffer_unlock(buffer);

		centre = PX_G(px + ((gsize)(TEST_H / 2) * TEST_W + TEST_W / 2) * 4);
		mid    = PX_G(px + ((gsize)(TEST_H / 2) * TEST_W + TEST_W / 2 + 24) * 4);
		edge   = PX_G(px + ((gsize)(TEST_H / 2) * TEST_W + TEST_W / 2 + 60) * 4);

		if (g_test_verbose())
			g_test_message("%s: centre %.0f, 24 px out %.0f, 60 px out %.0f",
			               pass_no == 0 ? "bokeh" : "box blur",
			               centre, mid, edge);

		g_assert_cmpfloat(centre, >, 8.0);
		/* Well outside the disc it must be dark either way, or the
		 * comparison below is between two washes. */
		g_assert_cmpfloat(edge, <, centre * 0.35);

		if (pass_no == 0)
			bokeh_flat = mid / centre;
		else
			blur_flat = mid / centre;

		g_free(px);
	}

	if (g_test_verbose())
		g_test_message("flatness at 24 px: bokeh %.3f, box blur %.3f "
		               "(1.0 is a perfectly filled disc)",
		               bokeh_flat, blur_flat);

	/* The disc is nearly as bright two thirds of the way out as it is in
	 * the middle... */
	g_assert_cmpfloat(bokeh_flat, >, 0.7);
	/* ...and the Gaussian is plainly not, which is the whole point. */
	g_assert_cmpfloat(bokeh_flat, >, blur_flat * 1.4);

	gowl_fx_texture_drop(f.gl, &out);
	fixture_close(&f);
}

/* ── The storm ───────────────────────────────────────────────────── */

/*
 * A flash is SEVERAL STROKES, not one fade.
 *
 * What people picture as a lightning flash is three to five return
 * strokes down the same channel tens of milliseconds apart, and the
 * flicker between them is the single most recognisable thing about it.
 * A smooth rise and fall reads as somebody turning a lamp up -- and it
 * is what almost every implementation of this does, because one
 * envelope is easier than several.
 *
 * Counted as local maxima in the envelope, sampled finely enough to see
 * them.  No GPU.
 */
static void
test_a_flash_is_several_strokes(void)
{
	GowlFxRainClock clock;
	gdouble prev = 0.0, cur = 0.0;
	gint    peaks = 0, i;
	gboolean rising = FALSE;

	memset(&clock, 0, sizeof(clock));
	/* Start one directly rather than waiting for the dice. */
	clock.strike   = 5.0;
	clock.strike_t = 0.0;
	clock.wait     = 1e9;

	for (i = 0; i < 400; i++) {     /* 400 x 2 ms = 0.8 s */
		gowl_fx_rain_lightning_advance(&clock, 0.002, 9.0, 1.0);
		prev = cur;
		cur  = clock.flash;

		if (cur > prev + 1e-4) {
			rising = TRUE;
		} else if (rising && cur < prev - 1e-4) {
			peaks++;
			rising = FALSE;
		}
	}

	if (g_test_verbose())
		g_test_message("one flash had %d distinct strokes", peaks);

	/* More than one, and not a strobe.  The count is hashed per strike,
	 * so a range is what can be asserted -- but "more than one" is the
	 * whole claim. */
	g_assert_cmpint(peaks, >=, 2);
	g_assert_cmpint(peaks, <=, 6);
}

/*
 * The gaps between flashes are EXPONENTIAL, not a metronome.
 *
 * A storm is a Poisson process: sometimes two almost on top of each
 * other, sometimes a long wait, and the mean is the only thing that is
 * fixed.  A fixed interval is the other half of what gives a fake storm
 * away, and it is the half people notice second -- after about a minute
 * of looking at it.
 *
 * Asserted on both: the mean lands near the rate, and the spread is
 * large.  For an exponential the standard deviation EQUALS the mean, so
 * anything much under that is a metronome with jitter.
 */
static void
test_the_gaps_between_flashes_are_not_a_metronome(void)
{
	GowlFxRainClock clock;
	gdouble last = 0.0, t = 0.0;
	gdouble gaps[64];
	gdouble sum = 0.0, var = 0.0, mean;
	gint    n = 0, i;
	gdouble seen = 0.0;

	memset(&clock, 0, sizeof(clock));

	for (i = 0; i < 60 * 1200 && n < 64; i++) {   /* up to 20 minutes */
		gowl_fx_rain_lightning_advance(&clock, 1.0 / 60.0, 6.0, 1.0);
		t += 1.0 / 60.0;
		if (clock.strike != seen) {
			seen = clock.strike;
			if (last > 0.0)
				gaps[n++] = t - last;
			last = t;
		}
	}

	g_assert_cmpint(n, >=, 24);

	for (i = 0; i < n; i++)
		sum += gaps[i];
	mean = sum / n;
	for (i = 0; i < n; i++)
		var += (gaps[i] - mean) * (gaps[i] - mean);
	var /= n;

	if (g_test_verbose())
		g_test_message("storm: %d gaps, mean %.2f s (asked for 6), "
		               "sd %.2f s (an exponential's sd equals its mean)",
		               n, mean, sqrt(var));

	/* The mean is the rate, near enough for a sample this size. */
	g_assert_cmpfloat(mean, >, 3.5);
	g_assert_cmpfloat(mean, <, 9.5);
	/* And the spread is exponential rather than tight.  Half the mean is
	 * a long way under the true value of 1.0 and still far above
	 * anything a jittered fixed interval would reach. */
	g_assert_cmpfloat(sqrt(var), >, mean * 0.5);
}

/*
 * Switching the storm off does not cut a flash in half.
 *
 * A rate of zero is what the module passes when the style moves away
 * from `storm', and it can happen mid-strike.  Decaying is the
 * difference between the flash ending and the compositor appearing to
 * drop a frame.
 */
static void
test_switching_the_storm_off_lets_the_flash_finish(void)
{
	GowlFxRainClock clock;
	gdouble first, second;

	memset(&clock, 0, sizeof(clock));
	clock.strike   = 2.0;
	clock.strike_t = 0.0;
	clock.wait     = 1e9;
	gowl_fx_rain_lightning_advance(&clock, 0.004, 9.0, 1.0);
	g_assert_cmpfloat(clock.flash, >, 0.3);

	gowl_fx_rain_lightning_advance(&clock, 0.016, 0.0, 1.0);
	first = clock.flash;
	gowl_fx_rain_lightning_advance(&clock, 0.016, 0.0, 1.0);
	second = clock.flash;

	/* Falling, not gone. */
	g_assert_cmpfloat(first, >, 0.0);
	g_assert_cmpfloat(second, <, first);

	/* And it does reach nothing rather than decaying forever. */
	{
		gint i;

		for (i = 0; i < 40; i++)
			gowl_fx_rain_lightning_advance(&clock, 0.016, 0.0, 1.0);
		g_assert_cmpfloat(clock.flash, ==, 0.0);
	}
}

gint
main(gint argc, gchar **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/effects/soap/colour-out-of-grey",
	                test_a_film_makes_colour_out_of_grey);
	g_test_add_func("/effects/soap/bands-lie-across-the-pane",
	                test_the_bands_lie_across_the_pane);
	g_test_add_func("/effects/soap/thin-enough-stops-colouring",
	                test_a_film_thin_enough_stops_colouring);

	g_test_add_func("/effects/embers/cooler-is-redder-and-much-dimmer",
	                test_a_cooler_ember_is_redder_and_much_dimmer);
	g_test_add_func("/effects/embers/the-sparks-go-up",
	                test_the_sparks_go_up);

	g_test_add_func("/effects/submerged/depth-eats-the-red-first",
	                test_depth_eats_the_red_first);
	g_test_add_func("/effects/submerged/caustics-reshape",
	                test_the_caustics_reshape_rather_than_slide);

	g_test_add_func("/effects/dew/a-bead-inverts",
	                test_a_bead_inverts_what_is_behind_it);
	g_test_add_func("/effects/dew/the-silk-is-bright-and-thin",
	                test_the_silk_is_bright_and_thin);

	g_test_add_func("/effects/bokeh/a-point-becomes-a-disc",
	                test_a_bright_point_becomes_a_disc);

	g_test_add_func("/effects/storm/a-flash-is-several-strokes",
	                test_a_flash_is_several_strokes);
	g_test_add_func("/effects/storm/gaps-are-not-a-metronome",
	                test_the_gaps_between_flashes_are_not_a_metronome);
	g_test_add_func("/effects/storm/switching-off-lets-it-finish",
	                test_switching_the_storm_off_lets_the_flash_finish);

	return g_test_run();
}
