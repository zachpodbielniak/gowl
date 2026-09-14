/* test-pq-encode.c -- the desktop, encoded for an HDR output
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This one has a RIGHT ANSWER, which almost nothing else in the effect
 * layer does.  SMPTE ST 2084 says exactly what code value a given number
 * of candelas is, and ITU-R BT.2408 says SDR white is 203 of them -- so
 * the expected output is arithmetic rather than judgement, and it is
 * computed here from the standard's own constants rather than compared
 * against a picture somebody once approved.
 *
 * Which matters more than usual, because getting it wrong is invisible
 * in the only way anybody would check it.  A desktop encoded with the
 * wrong reference white looks like a desktop: no artefacts, no banding,
 * nothing to point at.  It is merely three to fifty times too bright,
 * which reads as "HDR is like that" -- and it costs a laptop most of its
 * battery, because the panel spends the day at its peak.  That is the
 * bug this file exists to stop coming back.
 *
 * Needs a render node.
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

#define TEST_W 64
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

/*
 * Encode a flat sRGB grey and read back what the shader made of it.
 *
 * Flat, so a single pixel is the whole answer and a texture filter
 * cannot be blamed for a disagreement.
 */
static gboolean
encode_grey(Fixture *f, guint8 level, gdouble white, gdouble peak,
            guint8 out_rgb[3])
{
	struct wlr_buffer  *buffer;
	struct wlr_texture *tex, *result;
	struct wlr_texture_read_pixels_options opts;
	GowlFxTexture       scene;
	GowlFxPass         *pass;
	guint8             *pixels;
	guint8             *whole;
	const guint8       *read;
	gboolean            ok;
	gsize               i;

	pixels = g_malloc0((gsize)TEST_W * TEST_H * 4);
	for (i = 0; i < (gsize)TEST_W * TEST_H; i++) {
		pixels[i * 4 + 0] = level;
		pixels[i * 4 + 1] = level;
		pixels[i * 4 + 2] = level;
		pixels[i * 4 + 3] = 0xff;
	}
	tex = wlr_texture_from_pixels(f->renderer, DRM_FORMAT_ARGB8888,
	                              TEST_W * 4, TEST_W, TEST_H, pixels);
	g_free(pixels);
	if (tex == NULL)
		return FALSE;

	memset(&scene, 0, sizeof(scene));
	ok = gowl_fx_texture_store(f->gl, &scene, tex, TEST_W, TEST_H);
	wlr_texture_destroy(tex);
	if (!ok)
		return FALSE;

	buffer = wlr_swapchain_acquire(f->swapchain);
	if (buffer == NULL) {
		gowl_fx_texture_drop(f->gl, &scene);
		return FALSE;
	}
	pass = gowl_fx_pass_begin(f->gl, buffer);
	if (pass == NULL) {
		wlr_buffer_unlock(buffer);
		gowl_fx_texture_drop(f->gl, &scene);
		return FALSE;
	}
	ok = gowl_fx_pass_pq(pass, &scene, white, peak);
	gowl_fx_pass_end(pass);
	gowl_fx_texture_drop(f->gl, &scene);
	if (!ok) {
		wlr_buffer_unlock(buffer);
		g_error("the PQ encode shader would not build -- an HDR output "
		        "would show SDR content uncorrected and nothing would "
		        "say so on screen");
	}

	result = wlr_texture_from_buffer(f->renderer, buffer);
	if (result == NULL) {
		wlr_buffer_unlock(buffer);
		return FALSE;
	}
	/* The whole buffer rather than one pixel: src_box is const in the
	 * options struct, and sixteen kilobytes is not worth a cast. */
	whole = g_malloc0((gsize)TEST_W * TEST_H * 4);
	memset(&opts, 0, sizeof(opts));
	opts.data   = whole;
	opts.format = DRM_FORMAT_ARGB8888;
	opts.stride = TEST_W * 4;
	ok = wlr_texture_read_pixels(result, &opts);
	wlr_texture_destroy(result);
	wlr_buffer_unlock(buffer);
	if (!ok) {
		g_free(whole);
		return FALSE;
	}

	/* ARGB8888 little-endian: the bytes come back B, G, R, A. */
	read = whole + (((gsize)TEST_H / 2) * TEST_W + TEST_W / 2) * 4;
	out_rgb[0] = read[2];
	out_rgb[1] = read[1];
	out_rgb[2] = read[0];
	g_free(whole);
	return TRUE;
}

/* ── The standard, in C, so the expectation is derived and not guessed ── */

static gdouble
srgb_to_linear(gdouble c)
{
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

/* SMPTE ST 2084, the inverse EOTF: absolute cd/m2 to a code value. */
static gdouble
pq_encode(gdouble nits)
{
	const gdouble m1 = 2610.0 / 16384.0;
	const gdouble m2 = 2523.0 / 4096.0 * 128.0;
	const gdouble c1 = 3424.0 / 4096.0;
	const gdouble c2 = 2413.0 / 4096.0 * 32.0;
	const gdouble c3 = 2392.0 / 4096.0 * 32.0;
	gdouble y = pow(MAX(nits, 0.0) / 10000.0, m1);

	return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

/*
 * A grey is a grey in both spaces, which is why the cases are greys: the
 * BT.709 to BT.2020 matrix has rows that sum to one, so it moves the
 * primaries and leaves the neutral axis exactly where it was.  That
 * makes the expected value a one-line calculation instead of a matrix
 * multiplication repeated in the test.
 */
static gdouble
expected_code(guint8 level, gdouble white, gdouble peak)
{
	gdouble lin = srgb_to_linear((gdouble)level / 255.0);

	return pq_encode(MIN(lin * white, peak));
}

static void
test_the_encode_matches_the_standard(void)
{
	Fixture f;
	const guint8 levels[] = { 0, 16, 64, 128, 192, 255 };
	gsize i;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	for (i = 0; i < G_N_ELEMENTS(levels); i++) {
		guint8  got[3];
		gdouble want;
		gdouble diff;

		g_assert_true(encode_grey(&f, levels[i], GOWL_FX_PQ_SDR_WHITE,
		                          10000.0, got));
		want = expected_code(levels[i], GOWL_FX_PQ_SDR_WHITE, 10000.0);
		diff = fabs((gdouble)got[1] / 255.0 - want);

		if (g_test_verbose()) {
			g_print("  sRGB %3u -> %6.1f cd/m2 -> code %.4f "
			        "(shader %.4f, off by %.4f)\n",
			        levels[i],
			        srgb_to_linear((gdouble)levels[i] / 255.0)
			        * GOWL_FX_PQ_SDR_WHITE,
			        want, (gdouble)got[1] / 255.0, diff);
		}
		/* Two code values of an eight-bit readback.  The shader works
		 * in floats and the buffer this lands in is ten bits wide in
		 * the real path; the slack is the readback, not the maths. */
		g_assert_cmpfloat(diff, <, 2.5 / 255.0);
		/* Grey in, grey out: the matrix must not tint the neutral
		 * axis, which is the first thing a wrong one does. */
		g_assert_cmpint(ABS((gint)got[0] - (gint)got[1]), <=, 2);
		g_assert_cmpint(ABS((gint)got[2] - (gint)got[1]), <=, 2);
	}
	fixture_close(&f);
}

/*
 * White lands on the reference white, and that is the whole point.
 *
 * Without the scale, sRGB white goes out as code 1.0 -- which in PQ is a
 * request for ten thousand candelas, forty-nine times what it should be,
 * and is why the panel runs at its peak all day.
 */
static void
test_white_is_two_hundred_and_three_candelas(void)
{
	Fixture f;
	guint8  got[3];
	gdouble code;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	g_assert_true(encode_grey(&f, 255, GOWL_FX_PQ_SDR_WHITE, 10000.0, got));
	code = (gdouble)got[1] / 255.0;
	if (g_test_verbose())
		g_print("  white encodes to %.4f (203 cd/m2 is %.4f, "
		        "uncorrected would be 1.0)\n",
		        code, pq_encode(GOWL_FX_PQ_SDR_WHITE));

	g_assert_cmpfloat(fabs(code - pq_encode(GOWL_FX_PQ_SDR_WHITE)),
	                  <, 2.5 / 255.0);
	/* And nowhere near the uncorrected value, which is the failure. */
	g_assert_cmpfloat(code, <, 0.75);

	fixture_close(&f);
}

/*
 * The panel's own peak is a ceiling, not a suggestion.
 *
 * A display asked for more light than it makes does not fail, it clips
 * -- and clipping in the PANEL is what keeps the backlight at maximum.
 * Clipping here instead is the difference between a bright desktop and a
 * flat battery.
 */
static void
test_the_panels_peak_is_a_ceiling(void)
{
	Fixture f;
	guint8  dim[3], bright[3];

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	/* A panel that reaches 100 cd/m2 cannot show 203, so white has to
	 * come back as 100 rather than as the reference white. */
	g_assert_true(encode_grey(&f, 255, GOWL_FX_PQ_SDR_WHITE, 100.0, dim));
	g_assert_cmpfloat(fabs((gdouble)dim[1] / 255.0 - pq_encode(100.0)),
	                  <, 2.5 / 255.0);

	/* And one that reaches plenty is not held back. */
	g_assert_true(encode_grey(&f, 255, GOWL_FX_PQ_SDR_WHITE, 1000.0,
	                          bright));
	g_assert_cmpint((gint)bright[1], >, (gint)dim[1]);

	fixture_close(&f);
}

/*
 * A brighter reference white is a brighter desktop, monotonically.
 *
 * `hdr-sdr-white' is the one knob a person has over how bright an HDR
 * desktop is, and a knob that does not move the picture in one direction
 * is worse than no knob.
 */
static void
test_the_reference_white_is_a_knob(void)
{
	Fixture f;
	const gdouble whites[] = { 80.0, 120.0, 203.0, 300.0, 500.0 };
	gint    last = -1;
	gsize   i;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 render node");
		return;
	}

	for (i = 0; i < G_N_ELEMENTS(whites); i++) {
		guint8 got[3];

		g_assert_true(encode_grey(&f, 255, whites[i], 10000.0, got));
		g_assert_cmpint((gint)got[1], >, last);
		last = (gint)got[1];
	}
	fixture_close(&f);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/pq-encode/matches-the-standard",
	                test_the_encode_matches_the_standard);
	g_test_add_func("/pq-encode/white-is-203-nits",
	                test_white_is_two_hundred_and_three_candelas);
	g_test_add_func("/pq-encode/peak-is-a-ceiling",
	                test_the_panels_peak_is_a_ceiling);
	g_test_add_func("/pq-encode/reference-white-is-a-knob",
	                test_the_reference_white_is_a_knob);
	return g_test_run();
}
