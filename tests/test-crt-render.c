/* test-crt-render.c -- the cathode ray tube
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything here is a claim that cannot be settled by looking at a
 * screenshot, because the wrong implementation of each looks right.
 *
 *   THE SCAN LINES COST NO LIGHT.  The beam is a Gaussian periodised
 *   over the line spacing and divided by its own area, so its mean over
 *   a period is exactly one and turning the lines up cannot dim the
 *   desktop.  A dark stripe multiplied over the picture -- which is what
 *   almost every CRT filter in the world is -- looks identical in a
 *   screenshot and takes a third of the brightness with it.  The same
 *   claim, separately, for the phosphor mask.
 *
 *   THE BEAM FATTENS WITH THE SIGNAL.  This is the difference between a
 *   tube and a striped overlay: the spot grows with beam current, so a
 *   bright line is wide and nearly fills the gap while a dim one is a
 *   thread.  Measured as the modulation depth at the dark end of a ramp
 *   against the depth at the bright end OF THE SAME RENDER, so no
 *   difference in settings can explain it.
 *
 *   THE GLASS IS CURVED, NOT JUST SQUEEZED.  A sphere magnifies its
 *   middle and compresses its rim; a picture merely scaled down does
 *   neither.  Read off a horizontal ramp, where the value at a pixel
 *   says where the shader sampled: the local scale near the middle must
 *   be smaller than the local scale near the edge, and both edges of the
 *   raster must still land on the edges of the screen, because a desktop
 *   that loses its corners has lost window buttons.
 *
 *   A BEAM FINER THAN THE PIXEL GRID IS NOT DRAWN.  Asked for one line
 *   per pixel, the shader must fade the pattern out rather than alias it
 *   into a moire that crawls.  The failure it guards against is the
 *   reason `crt-lines' can be set at all.
 *
 *   THE GUNS MISS RADIALLY.  Convergence error grows from the middle
 *   outwards and points along the radius, so red leads blue on the right
 *   and TRAILS it on the left.  A fixed sideways offset -- the usual
 *   "chromatic aberration" -- gives the same fringe everywhere and is
 *   what this separates itself from.
 *
 *   A GRILLE HAS NO HORIZONTAL STRUCTURE.  A Trinitron's stripes run the
 *   whole height of the tube; a shadow mask's dots do not.  Two shaders
 *   that differ only in that are told apart here and nowhere else.
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

/* The ramp's ends, so a rendered value can be turned back into the
 * position it was sampled from. */
#define RAMP_LO 20
#define RAMP_HI 235

typedef enum {
	WP_GREY,    /* flat 0x80: any change in the mean was made here */
	WP_RAMP,    /* left to right: a rendered value IS a position */
	WP_BRIGHT   /* flat 0xf0: the beam at full current */
} Wallpaper;

typedef struct {
	struct wl_event_loop *loop;
	struct wlr_backend   *backend;
	struct wlr_renderer  *renderer;
	struct wlr_allocator *allocator;
	struct wlr_swapchain *swapchain;
	GowlFxGl             *gl;
	GowlFxTexture         screen;
	GowlFxTexture         glow;
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
			guint8  v;

			if (kind == WP_GREY)
				v = 0x80;
			else if (kind == WP_BRIGHT)
				v = 0xf0;
			else
				v = (guint8)(RAMP_LO
				             + ((RAMP_HI - RAMP_LO) * x) / (TEST_W - 1));
			p[0] = p[1] = p[2] = v;
			p[3] = 0xff;
		}
	}
	tex = wlr_texture_from_pixels(f->renderer, DRM_FORMAT_ARGB8888,
	                              TEST_W * 4, TEST_W, TEST_H, pixels);
	g_free(pixels);
	if (tex == NULL)
		return FALSE;
	ok = gowl_fx_texture_store(f->gl, &f->screen, tex, TEST_W, TEST_H);
	wlr_texture_destroy(tex);
	if (ok)
		gowl_fx_texture_set_filter(f->gl, &f->screen, TRUE);
	return ok;
}

static void
fixture_close(Fixture *f)
{
	if (f->gl != NULL) {
		gowl_fx_texture_drop(f->gl, &f->screen);
		gowl_fx_texture_drop(f->gl, &f->glow);
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
render_full(Fixture *f, const GowlFxCrtParams *params, gboolean with_glow)
{
	struct wlr_buffer  *buffer;
	struct wlr_texture *texture;
	struct wlr_texture_read_pixels_options opts;
	GowlFxPass *pass;
	gfloat clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	const GowlFxTexture *glow = NULL;
	guint8 *out;
	gboolean drew;

	/*
	 * BEFORE the pass, never inside it.  gowl_fx_texture_blur() binds
	 * the scratch framebuffer and leaves framebuffer 0 bound when it is
	 * done, so a blur taken in the middle of a pass sends everything
	 * after it somewhere that is not the pass's target.
	 */
	if (with_glow
	    && gowl_fx_texture_blur(f->gl, &f->glow, &f->screen, 6, 3))
		glow = &f->glow;

	buffer = wlr_swapchain_acquire(f->swapchain);
	if (buffer == NULL)
		return NULL;
	pass = gowl_fx_pass_begin(f->gl, buffer);
	if (pass == NULL) {
		wlr_buffer_unlock(buffer);
		return NULL;
	}
	gowl_fx_pass_clear(pass, clear);
	drew = gowl_fx_pass_crt(pass, &f->screen, glow, params, NULL);
	gowl_fx_pass_end(pass);
	if (!drew) {
		wlr_buffer_unlock(buffer);
		/* Survivable on a desktop and fatal here: a shader that stopped
		 * compiling shows up as "the tube quietly went away", with
		 * nothing in any log a user reads. */
		g_error("the CRT shader would not build -- the module would "
		        "silently show nothing at all");
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

static guint8 *
render(Fixture *f, const GowlFxCrtParams *params)
{
	return render_full(f, params, FALSE);
}

/* ARGB8888 little-endian: byte 0 is blue, 1 green, 2 red. */
#define PX_B(p) ((gdouble)(p)[0])
#define PX_G(p) ((gdouble)(p)[1])
#define PX_R(p) ((gdouble)(p)[2])

static const guint8 *
at(const guint8 *px, gint x, gint y)
{
	return px + (((gsize)y * TEST_W) + x) * 4;
}

/*
 * A tube with everything switched off.
 *
 * The starting point for every case here: each one turns on exactly the
 * one thing it is asking about, so a failure names it.
 */
static void
bare(GowlFxCrtParams *p)
{
	gowl_fx_crt_params_init(p);
	/* Said outright rather than left to the output, so that a change to
	 * how the raster is derived cannot silently change what every case
	 * below is measuring.  Four pixels to a line, which is what the
	 * derivation gives for a screen this tall anyway. */
	p->lines       = (gfloat)(TEST_H / 4);
	p->curvature   = 0.0f;
	p->scanline    = 0.0f;
	p->mask        = 0.0f;
	p->bloom       = 0.0f;
	p->vignette    = 0.0f;
	p->corner      = 0.0f;
	p->convergence = 0.0f;
	p->hum         = 0.0f;
	p->brightness  = 1.0f;
}

/*
 * The mean LIGHT of the green channel over a block.
 *
 * Through the tube's own exponent, and that is the whole point: the beam
 * and the mask multiply LIGHT, and it is the mean light that they are
 * built to preserve.  A mean of code values does not stay put under
 * either of them -- the encoding is concave, so a swing about a mean
 * comes back as a number above it -- and averaging those instead would
 * report a twenty per cent gain from a shader that is exactly neutral.
 */
static gdouble
mean_light(const guint8 *px, gint x0, gint y0, gint w, gint h, gdouble gamma)
{
	gdouble sum = 0.0;
	gint    x, y;

	for (y = y0; y < y0 + h; y++) {
		for (x = x0; x < x0 + w; x++)
			sum += pow(PX_G(at(px, x, y)) / 255.0, gamma);
	}
	return sum / ((gdouble)w * h);
}

/*
 * How deeply the scan lines cut, in a column of the picture.
 *
 * Averaged across @w columns first, so the phosphor's vertical stripes
 * cannot be read as scan lines: they are a pattern in x and this is a
 * measurement in y.
 */
static gdouble
row_modulation(const guint8 *px, gint x0, gint w, gint y0, gint y1)
{
	gdouble hi = -1.0, lo = 1.0e9, sum = 0.0, v;
	gint    x, y;

	for (y = y0; y < y1; y++) {
		v = 0.0;
		for (x = x0; x < x0 + w; x++)
			v += PX_G(at(px, x, y));
		v /= (gdouble)w;
		sum += v;
		if (v > hi) hi = v;
		if (v < lo) lo = v;
	}
	return (hi - lo) / MAX(sum / (gdouble)(y1 - y0), 1.0e-6);
}

/* ── The light the lines and the mask cost ───────────────────────── */

static void
test_the_scan_lines_do_not_dim_the_desktop(void)
{
	Fixture f;
	GowlFxCrtParams p;
	guint8 *flat, *lined;
	gdouble a, b;

	if (!fixture_open(&f, WP_GREY)) {
		g_test_skip("no GL");
		fixture_close(&f);
		return;
	}

	bare(&p);
	flat = render(&f, &p);
	p.scanline = 1.0f;
	lined = render(&f, &p);
	g_assert_nonnull(flat);
	g_assert_nonnull(lined);

	a = mean_light(flat, 16, 16, TEST_W - 32, TEST_H - 32, p.gamma);
	b = mean_light(lined, 16, 16, TEST_W - 32, TEST_H - 32, p.gamma);

	/*
	 * Within a percent.  A periodised Gaussian divided by its own area
	 * has mean one however wide it is, so the desktop keeps every bit of
	 * its brightness at any depth of scan line.  A stripe texture
	 * multiplied over the picture -- which is what almost every CRT
	 * filter does -- lands a third low here.
	 */
	g_assert_cmpfloat(fabs(a - b) / a, <, 0.01);

	g_free(flat);
	g_free(lined);
	fixture_close(&f);
}

static void
test_the_phosphor_mask_does_not_dim_the_desktop(void)
{
	Fixture f;
	GowlFxCrtParams p;
	guint8 *flat, *masked;
	gdouble a, b;

	if (!fixture_open(&f, WP_GREY)) {
		g_test_skip("no GL");
		fixture_close(&f);
		return;
	}

	bare(&p);
	flat = render(&f, &p);
	p.mask      = 1.0f;
	p.mask_kind = GOWL_CRT_MASK_GRILLE;
	masked = render(&f, &p);
	g_assert_nonnull(flat);
	g_assert_nonnull(masked);

	a = mean_light(flat, 16, 16, TEST_W - 32, TEST_H - 32, p.gamma);
	b = mean_light(masked, 16, 16, TEST_W - 32, TEST_H - 32, p.gamma);

	/*
	 * A real grille blocks two thirds of the light and a real tube
	 * answers with more beam current.  Modelling the loss and not the
	 * answer would make the whole desktop dark, so the gains are a
	 * raised cosine with mean one: a colour texture, not a dimmer.
	 */
	g_assert_cmpfloat(fabs(a - b) / a, <, 0.01);

	g_free(flat);
	g_free(masked);
	fixture_close(&f);
}

/* ── The spot grows with the current ─────────────────────────────── */

static void
test_a_bright_line_is_fatter_than_a_dim_one(void)
{
	Fixture f;
	GowlFxCrtParams p;
	guint8 *px;
	gdouble dark, bright;

	if (!fixture_open(&f, WP_RAMP)) {
		g_test_skip("no GL");
		fixture_close(&f);
		return;
	}

	bare(&p);
	p.scanline = 1.0f;
	px = render(&f, &p);
	g_assert_nonnull(px);

	/*
	 * Both measurements come out of ONE render with ONE set of
	 * settings; the only thing that differs between them is how hard
	 * the gun is being driven, which is the entire claim.
	 */
	dark   = row_modulation(px, 8, 24, 32, TEST_H - 32);
	bright = row_modulation(px, TEST_W - 32, 24, 32, TEST_H - 32);

	g_assert_cmpfloat(dark, >, 0.20);
	/* Comfortably apart, not merely ordered: a shader whose sigma had
	 * stopped depending on the luminance would put these within noise
	 * of each other. */
	g_assert_cmpfloat(bright, <, dark * 0.6);

	g_free(px);
	fixture_close(&f);
}

/* ── The glass ───────────────────────────────────────────────────── */

static void
test_the_fit_is_arithmetic(void)
{
	gfloat x, y;

	/* Flat glass stretches nothing. */
	gowl_fx_crt_fit(0.0, 1.0, 0.5625, &x, &y);
	g_assert_cmpfloat(fabs(x - 1.0f), <, 1.0e-6);
	g_assert_cmpfloat(fabs(y - 1.0f), <, 1.0e-6);

	/* The horizontal stretch is asin(k)/k, which is what the arc of a
	 * sphere of radius 1/k over a chord of 1 comes to. */
	gowl_fx_crt_fit(0.45, 1.0, 0.5625, &x, &y);
	g_assert_cmpfloat(fabs(x - (gfloat)(asin(0.45) / 0.45)), <, 1.0e-5);
	g_assert_cmpfloat(x, >, 1.0f);
	g_assert_cmpfloat(y, >, 1.0f);

	/* A cylinder is curved across and flat down.  A Trinitron's picture
	 * is not stretched vertically at all, and a shader that applied the
	 * radius to both axes whatever the setting said would fail here and
	 * nowhere else. */
	gowl_fx_crt_fit(0.45, 0.0, 0.5625, &x, &y);
	g_assert_cmpfloat(fabs(x - (gfloat)(asin(0.45) / 0.45)), <, 1.0e-5);
	g_assert_cmpfloat(fabs(y - 1.0f), <, 1.0e-6);
}

static void
test_the_curve_magnifies_the_middle_and_keeps_the_edges(void)
{
	Fixture f;
	GowlFxCrtParams p;
	guint8 *px;
	gint y = TEST_H / 2;
	gdouble mid_scale, edge_scale;
	gdouble left_edge, right_edge;

	if (!fixture_open(&f, WP_RAMP)) {
		g_test_skip("no GL");
		fixture_close(&f);
		return;
	}

	bare(&p);
	p.curvature = 0.60f;
	px = render(&f, &p);
	g_assert_nonnull(px);

	/*
	 * The ramp's value at a pixel says where the shader sampled, so the
	 * DIFFERENCE between two nearby pixels is the local scale.  On a
	 * sphere the middle is magnified (a small step in position covers a
	 * small step of the picture) and the rim is compressed.
	 */
	mid_scale  = PX_G(at(px, TEST_W / 2 + 16, y))
	             - PX_G(at(px, TEST_W / 2 - 16, y));
	edge_scale = PX_G(at(px, TEST_W - 8, y))
	             - PX_G(at(px, TEST_W - 40, y));

	g_assert_cmpfloat(mid_scale, >, 0.0);
	g_assert_cmpfloat(edge_scale, >, mid_scale * 1.15);

	/*
	 * And nothing was pushed off.  The far left and far right of the
	 * screen must still be the far left and far right of the desktop:
	 * scaling to the corners instead would fill the screen and eat the
	 * four corners of somebody's windows.
	 */
	left_edge  = PX_G(at(px, 1, y));
	right_edge = PX_G(at(px, TEST_W - 2, y));
	g_assert_cmpfloat(left_edge, <, RAMP_LO + 14);
	g_assert_cmpfloat(right_edge, >, RAMP_HI - 14);

	g_free(px);
	fixture_close(&f);
}

/* ── What the pixel grid cannot carry ────────────────────────────── */

static void
test_a_raster_finer_than_the_screen_fades_out(void)
{
	Fixture f;
	GowlFxCrtParams p;
	guint8 *coarse, *mid, *fine;
	gdouble a, b, c;

	if (!fixture_open(&f, WP_GREY)) {
		g_test_skip("no GL");
		fixture_close(&f);
		return;
	}

	bare(&p);
	p.scanline = 1.0f;

	/*
	 * Line counts that do NOT divide the height, which is the whole
	 * reason for the odd numbers.  At 128 lines on a 256-pixel screen
	 * every row lands on the same two phases of the beam and at 256 on
	 * the same one, so the measurement comes back as exactly zero
	 * whatever the shader does and a broken one passes.  61, 101 and
	 * 171 are coprime with 256 and walk the phase, which is also what a
	 * real screen height does to a real line count.
	 */

	p.lines = 61.0f;             /* 4.2 pixels a line: drawable */
	coarse = render(&f, &p);
	p.lines = 101.0f;            /* 2.5: the limit */
	mid = render(&f, &p);
	p.lines = 171.0f;            /* 1.5: past it */
	fine = render(&f, &p);
	g_assert_nonnull(coarse);
	g_assert_nonnull(mid);
	g_assert_nonnull(fine);

	a = row_modulation(coarse, 32, 32, 32, TEST_H - 32);
	b = row_modulation(mid, 32, 32, 32, TEST_H - 32);
	c = row_modulation(fine, 32, 32, 32, TEST_H - 32);

	/* Present, and strongly, wherever the grid can carry it. */
	g_assert_cmpfloat(a, >, 0.30);

	/*
	 * And damped where it cannot.  TWO mechanisms hold this down and
	 * the middle case needs both of them: the sigma FLOOR, which never
	 * lets the spot be narrower than about a pixel, and the FADE, which
	 * takes the depth to nothing as the spacing closes on two pixels.
	 * Take either one out and this reads 0.21 instead of 0.06 -- a
	 * beat between the raster and the pixel grid that crawls up the
	 * screen whenever anything moves, and is louder than the pattern it
	 * is supposed to be.
	 */
	g_assert_cmpfloat(b, <, 0.12);
	g_assert_cmpfloat(c, <, 0.02);

	g_free(coarse);
	g_free(mid);
	g_free(fine);
	fixture_close(&f);
}

/* ── The glass scatters ──────────────────────────────────────────── */

static void
test_the_glow_moves_light_rather_than_making_it(void)
{
	Fixture f;
	GowlFxCrtParams p;
	guint8 *dark, *lit;
	gdouble a, b;

	if (!fixture_open(&f, WP_BRIGHT)) {
		g_test_skip("no GL");
		fixture_close(&f);
		return;
	}

	bare(&p);
	dark = render_full(&f, &p, FALSE);
	p.bloom     = 1.5f;
	p.bloom_cut = 0.20f;
	lit = render_full(&f, &p, TRUE);
	g_assert_nonnull(dark);
	g_assert_nonnull(lit);

	a = mean_light(dark, 48, 48, TEST_W - 96, TEST_H - 96, p.gamma);
	b = mean_light(lit, 48, 48, TEST_W - 96, TEST_H - 96, p.gamma);

	/*
	 * Halation is light that went somewhere else, so over a field where
	 * everywhere is as bright as everywhere else there is nowhere else
	 * for it to go and the picture must come back untouched.
	 *
	 * A bloom that adds only what ARRIVES -- the usual one -- puts a
	 * flat 0.18 on top of every pixel of this field at once, which
	 * clips the whole of it to white and takes the scan lines and the
	 * phosphor with it.  That is not a subtle difference on a desktop:
	 * it is what happens to every maximised document window.
	 */
	g_assert_cmpfloat(fabs(a - b) / a, <, 0.02);

	g_free(dark);
	g_free(lit);
	fixture_close(&f);
}

/* ── The guns ────────────────────────────────────────────────────── */

static void
test_the_guns_miss_radially(void)
{
	Fixture f;
	GowlFxCrtParams p;
	guint8 *px;
	gint y = TEST_H / 2;
	gdouble left, middle, right;

	if (!fixture_open(&f, WP_RAMP)) {
		g_test_skip("no GL");
		fixture_close(&f);
		return;
	}

	bare(&p);
	p.convergence = 12.0f;
	px = render(&f, &p);
	g_assert_nonnull(px);

	/*
	 * Red is sampled outward along the radius and blue inward, so on a
	 * ramp that rises to the right red reads HIGH on the right and LOW
	 * on the left, and the difference passes through zero in the
	 * middle.  A fixed sideways offset -- what a chromatic aberration
	 * filter does -- would give the same sign at both ends.
	 */
	left   = PX_R(at(px, 24, y))            - PX_B(at(px, 24, y));
	middle = PX_R(at(px, TEST_W / 2, y))    - PX_B(at(px, TEST_W / 2, y));
	right  = PX_R(at(px, TEST_W - 25, y))   - PX_B(at(px, TEST_W - 25, y));

	g_assert_cmpfloat(right, >, 6.0);
	g_assert_cmpfloat(left, <, -6.0);
	g_assert_cmpfloat(fabs(middle), <, 2.0);

	g_free(px);
	fixture_close(&f);
}

/* ── Which tube ──────────────────────────────────────────────────── */

static void
test_a_grille_runs_the_whole_height_and_a_shadow_mask_does_not(void)
{
	Fixture f;
	GowlFxCrtParams p;
	guint8 *grille, *shadow;
	gdouble a, b;

	if (!fixture_open(&f, WP_GREY)) {
		g_test_skip("no GL");
		fixture_close(&f);
		return;
	}

	bare(&p);
	p.mask = 1.0f;

	p.mask_kind = GOWL_CRT_MASK_GRILLE;
	grille = render(&f, &p);
	p.mask_kind = GOWL_CRT_MASK_SHADOW;
	shadow = render(&f, &p);
	g_assert_nonnull(grille);
	g_assert_nonnull(shadow);

	/*
	 * Down a single column.  An aperture grille is a set of wires
	 * running the whole height of the tube, so a column of it is
	 * constant; a shadow mask is an array of holes and a column of it
	 * is not.  The two shaders differ by four lines and by nothing a
	 * screenshot at desktop scale can show.
	 */
	a = row_modulation(grille, 40, 1, 32, TEST_H - 32);
	b = row_modulation(shadow, 40, 1, 32, TEST_H - 32);

	g_assert_cmpfloat(a, <, 0.02);
	g_assert_cmpfloat(b, >, 0.15);

	g_free(grille);
	g_free(shadow);
	fixture_close(&f);
}

/* ── The clock ───────────────────────────────────────────────────── */

static void
test_the_hum_bar_drifts_and_stays_a_phase(void)
{
	GowlFxCrtClock clock;
	gint i;

	memset(&clock, 0, sizeof(clock));

	gowl_fx_crt_advance(&clock, 1.0);
	g_assert_cmpfloat(clock.hum_phase, >, 0.0);
	g_assert_cmpfloat(clock.hum_phase, <, 1.0);

	/* A stall is not a fast-forward: a frame that took a second and a
	 * half must not jump the bar a fifth of the way up the screen. */
	{
		GowlFxCrtClock stalled;
		gdouble before;

		memset(&stalled, 0, sizeof(stalled));
		gowl_fx_crt_advance(&stalled, 1.5);
		before = stalled.hum_phase;
		g_assert_cmpfloat(before, <=, 0.25 / 7.0 + 1.0e-9);
	}

	/* An hour of frames leaves a phase, not a number that has run out
	 * of mantissa by the time it reaches a uniform. */
	for (i = 0; i < 60 * 60 * 60; i++)
		gowl_fx_crt_advance(&clock, 1.0 / 60.0);
	g_assert_cmpfloat(clock.hum_phase, >=, 0.0);
	g_assert_cmpfloat(clock.hum_phase, <, 1.0);
}

/* ── What the output decides ─────────────────────────────────────── */

static void
test_the_raster_and_the_phosphor_are_scaled_to_the_screen(void)
{
	/*
	 * Four output pixels to a scan line and a triad about the size the
	 * triads on a 13" tube were, which is a fixed number of pixels only
	 * if every screen has the same resolution.  A three-pixel pitch on
	 * a 4K panel is under half a millimetre: invisible, and the phosphor
	 * is the most recognisable thing about a CRT.
	 */
	g_assert_cmpint(gowl_fx_crt_auto_lines(1080), ==, 270);
	g_assert_cmpint(gowl_fx_crt_auto_lines(2160), ==, 540);
	g_assert_cmpint(gowl_fx_crt_auto_lines(4320), ==, 1080);   /* capped */
	g_assert_cmpint(gowl_fx_crt_auto_lines(TEST_H), ==, TEST_H / 4);
	g_assert_cmpint(gowl_fx_crt_auto_lines(0), ==, 240);

	g_assert_cmpint(gowl_fx_crt_auto_mask_size(1920), ==, 3);
	g_assert_cmpint(gowl_fx_crt_auto_mask_size(3840), ==, 6);
	g_assert_cmpint(gowl_fx_crt_auto_mask_size(800), ==, 3);   /* floored */
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/crt/scanlines-cost-no-light",
	                test_the_scan_lines_do_not_dim_the_desktop);
	g_test_add_func("/crt/mask-costs-no-light",
	                test_the_phosphor_mask_does_not_dim_the_desktop);
	g_test_add_func("/crt/bright-lines-are-fatter",
	                test_a_bright_line_is_fatter_than_a_dim_one);
	g_test_add_func("/crt/fit-arithmetic", test_the_fit_is_arithmetic);
	g_test_add_func("/crt/curve-magnifies-the-middle",
	                test_the_curve_magnifies_the_middle_and_keeps_the_edges);
	g_test_add_func("/crt/too-fine-a-raster-fades",
	                test_a_raster_finer_than_the_screen_fades_out);
	g_test_add_func("/crt/halation-moves-light",
	                test_the_glow_moves_light_rather_than_making_it);
	g_test_add_func("/crt/convergence-is-radial", test_the_guns_miss_radially);
	g_test_add_func("/crt/grille-against-shadow-mask",
	                test_a_grille_runs_the_whole_height_and_a_shadow_mask_does_not);
	g_test_add_func("/crt/hum-is-a-phase",
	                test_the_hum_bar_drifts_and_stays_a_phase);
	g_test_add_func("/crt/raster-scales-to-the-screen",
	                test_the_raster_and_the_phosphor_are_scaled_to_the_screen);

	return g_test_run();
}
