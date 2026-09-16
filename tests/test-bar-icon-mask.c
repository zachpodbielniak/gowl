/* test-bar-icon-mask.c -- a symbolic tray icon you can actually see
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Deskflow registers a tray item, gowl holds it, the bar draws it ---
 * and nothing appears on screen.  The item is not missing: its icon is
 * a symbolic one, which means the shape is in the alpha channel and
 * the colour channel is a flat black, and a flat black glyph on a dark
 * bar is a slot that looks empty.  Proton Mail Bridge, whose icon is
 * purple, was visible the whole time.  GNOME's AppIndicator extension
 * shows both because it repaints a symbolic icon in the panel's own
 * foreground; this is that.
 *
 * What is asserted:
 *
 *   A FLAT GLYPH IS A MASK.  Whatever alpha it has, whatever it is
 *   called, an image whose visible pixels are all one achromatic tone
 *   carries no colour of its own and has to be given one.
 *
 *   A PICTURE IS NOT.  Proton's purple, a red dot, a greyscale
 *   photograph: each has something to say in its colour channel and
 *   must come out of the bar exactly as the application sent it.
 *   Repainting those would be worse than the bug.
 *
 *   AND IT IS DECIDED ON THE SURFACE THAT IS ACTUALLY DRAWN.  The
 *   first version of this file built its surfaces by hand and asked
 *   about those.  The bar does not draw those: it premultiplies the
 *   application's pixmap and scales it to the bar's height, and
 *   scaling resamples a glyph's antialiased edges.  Deskflow's icon
 *   is a flat dark tone in the pixmap and spans fourteen shades by the
 *   time it is drawn --- which the first rule, asking for twelve,
 *   rejected.  The icon stayed invisible and every test passed.  So
 *   the last two cases here go through gowl_tray_item_argb32() and
 *   gowl_bar_icon_scale() first, as the bar does.
 *
 *   THE TINT KEEPS THE SHAPE.  Alpha is untouched, the colour is the
 *   one asked for, and the result stays premultiplied --- cairo does
 *   not check, it just renders the mistake.
 */

#include <glib.h>
#include <cairo.h>

#include "barkit/gowl-bar-icon.h"
#include "tray/gowl-tray.h"

/* Build an ARGB32 surface from a callback over (x, y). */
typedef void (*PixelFn)(gint x, gint y, guint8 *a, guint8 *r, guint8 *g,
                        guint8 *b);

static cairo_surface_t *
make_surface(gint w, gint h, PixelFn fn)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
	                                                w, h);
	guint8 *data = cairo_image_surface_get_data(s);
	gint stride = cairo_image_surface_get_stride(s);
	gint x, y;

	for (y = 0; y < h; y++) {
		guint32 *row = (guint32 *)(data + (gsize)y * stride);

		for (x = 0; x < w; x++) {
			guint8 a = 255, r = 0, g = 0, b = 0;

			fn(x, y, &a, &r, &g, &b);
			/* Premultiplied, as the format requires. */
			row[x] = ((guint32)a << 24)
			       | ((guint32)((r * a + 127) / 255) << 16)
			       | ((guint32)((g * a + 127) / 255) <<  8)
			       |  (guint32)((b * a + 127) / 255);
		}
	}
	cairo_surface_mark_dirty(s);
	return s;
}

/* Deskflow: a black glyph, antialiased, on transparency.  Measured
 * from the real item -- every opaque pixel was rgb(0,0,0). */
static void
px_black_glyph(gint x, gint y, guint8 *a, guint8 *r, guint8 *g, guint8 *b)
{
	*r = *g = *b = 0;
	if (x < 4 || y < 4)
		*a = 0;                       /* transparent margin */
	else if (x == 4 || y == 4)
		*a = 90;                      /* antialiased edge */
	else
		*a = 255;
}

/*
 * The same glyph in white, which is just as invisible on a light bar.
 * The antialiased edge matters here: premultiplied, white at alpha 90
 * is stored as 90, so a reader that does not undo the premultiplication
 * sees a second tone and calls a plain white glyph a picture.
 */
static void
px_white_glyph(gint x, gint y, guint8 *a, guint8 *r, guint8 *g, guint8 *b)
{
	*r = *g = *b = 255;
	if (x < 4 || y < 4)
		*a = 0;
	else if (x == 4 || y == 4)
		*a = 90;
	else
		*a = 255;
}

/* Proton Mail Bridge: purple, measured from the real item. */
static void
px_purple(gint x, gint y, guint8 *a, guint8 *r, guint8 *g, guint8 *b)
{
	(void)x; (void)y;
	*a = 255; *r = 90; *g = 80; *b = 200;
}

/* One flat colour, but a colour: a red attention dot.  Not a mask --
 * the red is the whole message. */
static void
px_flat_red(gint x, gint y, guint8 *a, guint8 *r, guint8 *g, guint8 *b)
{
	(void)x; (void)y;
	*a = 255; *r = 220; *g = 20; *b = 20;
}

/* Grey, but several greys: a greyscale picture, which shading flat
 * would erase. */
static void
px_greyscale(gint x, gint y, guint8 *a, guint8 *r, guint8 *g, guint8 *b)
{
	(void)y;
	*a = 255;
	*r = *g = *b = (guint8)(x * 8);
}

/*
 * A dark glyph with a fringe so faint it carries no information: alpha
 * 2, which is what scaling a shape leaves around its edge.  Its stored
 * colour rounds to 1, and un-premultiplying that reports a tone of 127
 * --- brighter than anything actually drawn.  A reader that trusts it
 * calls a plainly dark glyph a mid-grey picture.
 */
static void
px_faint_fringe(gint x, gint y, guint8 *a, guint8 *r, guint8 *g, guint8 *b)
{
	gboolean body = (x >= 4 && x < 12 && y >= 4 && y < 12);
	gboolean ring = !body && (x >= 3 && x < 13 && y >= 3 && y < 13);

	if (body) {
		*a = 255; *r = *g = *b = 64;
	} else if (ring) {
		*a = 2; *r = *g = *b = 128;
	} else {
		*a = 0; *r = *g = *b = 0;
	}
}

/*
 * A dark navy glyph.  Confined to the dark end exactly as a mask is,
 * so only its chroma separates the two --- and an application that
 * chose a dark colour deliberately must keep it.
 */
static void
px_dark_navy(gint x, gint y, guint8 *a, guint8 *r, guint8 *g, guint8 *b)
{
	(void)x; (void)y;
	*a = 255; *r = 8; *g = 8; *b = 90;
}

/* The same trap at the light end: pale, but unmistakably yellow. */
static void
px_pale_yellow(gint x, gint y, guint8 *a, guint8 *r, guint8 *g, guint8 *b)
{
	(void)x; (void)y;
	*a = 255; *r = 255; *g = 240; *b = 170;
}

/* Flat mid-grey: achromatic, but nowhere near either end. */
static void
px_mid_grey(gint x, gint y, guint8 *a, guint8 *r, guint8 *g, guint8 *b)
{
	(void)x; (void)y;
	*a = 255; *r = *g = *b = 128;
}

static void
px_transparent(gint x, gint y, guint8 *a, guint8 *r, guint8 *g, guint8 *b)
{
	(void)x; (void)y;
	*a = 0; *r = *g = *b = 0;
}

/* ------------------------------------------------------------------ *
 * What is a mask
 * ------------------------------------------------------------------ */

static void
test_black_glyph_is_a_mask(void)
{
	cairo_surface_t *s = make_surface(32, 32, px_black_glyph);

	/* The reported bug, in one assertion. */
	g_assert_true(gowl_bar_icon_is_mask(s));
	cairo_surface_destroy(s);
}

static void
test_white_glyph_is_a_mask(void)
{
	cairo_surface_t *s = make_surface(32, 32, px_white_glyph);

	/* The same problem on a light bar, and the same answer. */
	g_assert_true(gowl_bar_icon_is_mask(s));
	cairo_surface_destroy(s);
}

static void
test_a_coloured_icon_is_not(void)
{
	cairo_surface_t *s = make_surface(22, 22, px_purple);

	/* Proton's icon.  It was never the problem and must not be
	 * repainted. */
	g_assert_false(gowl_bar_icon_is_mask(s));
	cairo_surface_destroy(s);
}

static void
test_one_flat_colour_is_not(void)
{
	cairo_surface_t *s = make_surface(16, 16, px_flat_red);

	/* Flat, like a mask, but red means something. */
	g_assert_false(gowl_bar_icon_is_mask(s));
	cairo_surface_destroy(s);
}

static void
test_a_faint_fringe_does_not_decide_it(void)
{
	cairo_surface_t *s = make_surface(16, 16, px_faint_fringe);

	/*
	 * The glyph is dark and is a mask.  The only pixels saying
	 * otherwise are an alpha-2 fringe whose colour is a rounding
	 * artefact, and reading those is how a mask gets mistaken for a
	 * picture and left black on a black bar.
	 */
	g_assert_true(gowl_bar_icon_is_mask(s));
	cairo_surface_destroy(s);
}

static void
test_a_dark_coloured_glyph_is_not(void)
{
	cairo_surface_t *s = make_surface(16, 16, px_dark_navy);

	/*
	 * As dark as Deskflow's glyph and just as confined, so the tone
	 * test alone would repaint it.  It has a colour of its own and
	 * the application meant it.
	 */
	g_assert_false(gowl_bar_icon_is_mask(s));
	cairo_surface_destroy(s);
}

static void
test_a_light_coloured_glyph_is_not(void)
{
	cairo_surface_t *s = make_surface(16, 16, px_pale_yellow);

	/* The same trap at the other end. */
	g_assert_false(gowl_bar_icon_is_mask(s));
	cairo_surface_destroy(s);
}

static void
test_mid_grey_is_not(void)
{
	cairo_surface_t *s = make_surface(16, 16, px_mid_grey);

	/*
	 * Achromatic and perfectly flat --- a mask under the old rule ---
	 * but it sits in the middle of the range, where an icon is
	 * equally visible on a dark bar and a light one and nobody is
	 * waiting to be told what colour to be.
	 */
	g_assert_false(gowl_bar_icon_is_mask(s));
	cairo_surface_destroy(s);
}

static void
test_greyscale_is_not(void)
{
	cairo_surface_t *s = make_surface(32, 32, px_greyscale);

	/* Achromatic, like a mask, but the shading IS the picture. */
	g_assert_false(gowl_bar_icon_is_mask(s));
	cairo_surface_destroy(s);
}

static void
test_nothing_visible_is_not(void)
{
	cairo_surface_t *s = make_surface(16, 16, px_transparent);

	/* An empty image is not a mask of anything, and tinting it would
	 * still draw nothing. */
	g_assert_false(gowl_bar_icon_is_mask(s));
	cairo_surface_destroy(s);
}

static void
test_null_is_not(void)
{
	g_assert_false(gowl_bar_icon_is_mask(NULL));
}

/* ------------------------------------------------------------------ *
 * The tint
 * ------------------------------------------------------------------ */

static void
test_tint_keeps_the_shape(void)
{
	cairo_surface_t *s = make_surface(32, 32, px_black_glyph);
	cairo_surface_t *t;
	const guint8 *sd, *td;
	gint stride, x, y;

	t = gowl_bar_icon_tint(s, 1.0, 1.0, 1.0);
	g_assert_nonnull(t);

	sd = cairo_image_surface_get_data(s);
	td = cairo_image_surface_get_data(t);
	stride = cairo_image_surface_get_stride(s);
	g_assert_cmpint(stride, ==, cairo_image_surface_get_stride(t));

	for (y = 0; y < 32; y++) {
		const guint32 *srow = (const guint32 *)(sd + (gsize)y * stride);
		const guint32 *trow = (const guint32 *)(td + (gsize)y * stride);

		for (x = 0; x < 32; x++) {
			/* Alpha is the glyph; it must survive untouched. */
			g_assert_cmpuint(trow[x] >> 24, ==, srow[x] >> 24);
		}
	}
	cairo_surface_destroy(t);
	cairo_surface_destroy(s);
}

static void
test_tint_paints_the_colour(void)
{
	cairo_surface_t *s = make_surface(8, 8, px_black_glyph);
	cairo_surface_t *t = gowl_bar_icon_tint(s, 1.0, 0.0, 0.0);
	const guint8 *td;
	gint stride;
	guint32 px;

	g_assert_nonnull(t);
	td = cairo_image_surface_get_data(t);
	stride = cairo_image_surface_get_stride(t);

	/* (6,6) is inside the glyph and fully opaque. */
	px = ((const guint32 *)(td + (gsize)6 * stride))[6];
	g_assert_cmpuint((px >> 24) & 0xff, ==, 255);
	g_assert_cmpuint((px >> 16) & 0xff, ==, 255);   /* red */
	g_assert_cmpuint((px >>  8) & 0xff, ==, 0);
	g_assert_cmpuint( px        & 0xff, ==, 0);

	cairo_surface_destroy(t);
	cairo_surface_destroy(s);
}

static void
test_tint_stays_premultiplied(void)
{
	cairo_surface_t *s = make_surface(8, 8, px_black_glyph);
	cairo_surface_t *t = gowl_bar_icon_tint(s, 1.0, 1.0, 1.0);
	const guint8 *td;
	gint stride;
	guint32 px;
	guint a, r;

	g_assert_nonnull(t);
	td = cairo_image_surface_get_data(t);
	stride = cairo_image_surface_get_stride(t);

	/* (4,5) is on the antialiased edge: alpha 90 of 255.  White at
	 * that alpha is 90, not 255 -- cairo renders un-premultiplied
	 * data as a bright halo rather than rejecting it. */
	px = ((const guint32 *)(td + (gsize)5 * stride))[4];
	a = (px >> 24) & 0xff;
	r = (px >> 16) & 0xff;
	g_assert_cmpuint(a, ==, 90);
	g_assert_cmpuint(r, <=, a);
	cairo_surface_destroy(t);
	cairo_surface_destroy(s);
}

static void
test_tint_does_not_touch_the_original(void)
{
	cairo_surface_t *s = make_surface(8, 8, px_black_glyph);
	cairo_surface_t *t;
	const guint8 *sd;
	gint stride;
	guint32 before, after;

	sd = cairo_image_surface_get_data(s);
	stride = cairo_image_surface_get_stride(s);
	before = ((const guint32 *)(sd + (gsize)6 * stride))[6];

	/* The lookup path hands back a surface the shared name cache
	 * owns; tinting it in place would repaint every other widget's
	 * copy of that icon too. */
	t = gowl_bar_icon_tint(s, 1.0, 0.0, 0.0);
	g_assert_nonnull(t);
	after = ((const guint32 *)(sd + (gsize)6 * stride))[6];
	g_assert_cmpuint(before, ==, after);

	cairo_surface_destroy(t);
	cairo_surface_destroy(s);
}

static void
test_tint_null(void)
{
	g_assert_null(gowl_bar_icon_tint(NULL, 1.0, 1.0, 1.0));
}

/* ------------------------------------------------------------------ *
 * Through the pipeline the bar actually uses
 * ------------------------------------------------------------------ */

/*
 * An application's pixmap as it arrives: straight (not premultiplied)
 * ARGB, big-endian, exactly what the StatusNotifierItem property hands
 * over.
 *
 * The shape is in the alpha channel.  The colour channels run from 0
 * to @tone across the glyph rather than sitting flat at it, because
 * that is what the real icons do --- Deskflow's three channels each
 * hold seventeen distinct values between 0 and 64 --- and a flat
 * fixture would be a kinder image than any application sends.  It was
 * a rule that could not survive that variation which left the icon
 * invisible.
 *
 * The caller owns the result.
 */
static guint8 *
sni_pixmap(gint w, gint h, guint8 tone, gboolean colourful)
{
	guint8 *p = g_malloc0((gsize)w * h * 4);
	gint x, y;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			guint8 *px = p + ((gsize)y * w + x) * 4;
			gboolean inside = (x > w / 8) && (y > h / 8)
			                  && (x < w - w / 8) && (y < h - h / 8);
			guint8 t = (guint8)((gint)tone * x / (w - 1));

			/* A glyph with an antialiased edge, which is where
			 * the tones spread further once it is scaled. */
			px[0] = inside ? 255 : (x == w / 8 || y == h / 8)
			                       ? 100 : 0;
			px[1] = t;
			px[2] = colourful ? (guint8)(t / 3) : t;
			px[3] = colourful ? 255 : t;
		}
	}
	return p;
}

/* Premultiply and scale, exactly as bar_tray_icon_for() does. */
static cairo_surface_t *
as_the_bar_draws_it(gint w, gint h, guint8 *pixmap, gint size)
{
	GowlTrayItem item;
	cairo_surface_t *raw, *scaled;

	memset(&item, 0, sizeof(item));
	item.pixmap        = pixmap;
	item.pixmap_width  = w;
	item.pixmap_height = h;

	raw = gowl_tray_item_argb32(&item);
	g_assert_nonnull(raw);
	scaled = gowl_bar_icon_scale(raw, size);
	cairo_surface_destroy(raw);
	return scaled;
}

static void
test_deskflow_through_the_pipeline(void)
{
	g_autofree guint8 *pixmap = NULL;
	cairo_surface_t *drawn;

	/*
	 * Deskflow's icon, to the numbers measured off the running
	 * application: a 64x64 glyph, shape entirely in the alpha, and
	 * colour channels that never leave the dark end but do not sit
	 * still inside it.  Scaled to a bar icon the real one spans
	 * fourteen shades --- two more than the twelve the first rule
	 * allowed, which is the whole of why it stayed invisible.
	 */
	pixmap = sni_pixmap(64, 64, 64, FALSE);
	drawn = as_the_bar_draws_it(64, 64, pixmap, 20);

	g_assert_true(gowl_bar_icon_is_mask(drawn));
	cairo_surface_destroy(drawn);
}

static void
test_bridge_through_the_pipeline(void)
{
	g_autofree guint8 *pixmap = NULL;
	cairo_surface_t *drawn;

	/* Proton Mail Bridge: a coloured logo, through the same steps.
	 * It was visible all along and must be left alone. */
	pixmap = sni_pixmap(22, 22, 200, TRUE);
	drawn = as_the_bar_draws_it(22, 22, pixmap, 20);

	g_assert_false(gowl_bar_icon_is_mask(drawn));
	cairo_surface_destroy(drawn);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-icon/mask/black-glyph-is-a-mask",
	                test_black_glyph_is_a_mask);
	g_test_add_func("/bar-icon/mask/white-glyph-is-a-mask",
	                test_white_glyph_is_a_mask);
	g_test_add_func("/bar-icon/mask/coloured-icon-is-not",
	                test_a_coloured_icon_is_not);
	g_test_add_func("/bar-icon/mask/one-flat-colour-is-not",
	                test_one_flat_colour_is_not);
	g_test_add_func("/bar-icon/mask/faint-fringe-does-not-decide-it",
	                test_a_faint_fringe_does_not_decide_it);
	g_test_add_func("/bar-icon/mask/dark-coloured-glyph-is-not",
	                test_a_dark_coloured_glyph_is_not);
	g_test_add_func("/bar-icon/mask/light-coloured-glyph-is-not",
	                test_a_light_coloured_glyph_is_not);
	g_test_add_func("/bar-icon/mask/mid-grey-is-not",
	                test_mid_grey_is_not);
	g_test_add_func("/bar-icon/mask/greyscale-is-not",
	                test_greyscale_is_not);
	g_test_add_func("/bar-icon/mask/deskflow-through-the-pipeline",
	                test_deskflow_through_the_pipeline);
	g_test_add_func("/bar-icon/mask/bridge-through-the-pipeline",
	                test_bridge_through_the_pipeline);
	g_test_add_func("/bar-icon/mask/nothing-visible-is-not",
	                test_nothing_visible_is_not);
	g_test_add_func("/bar-icon/mask/null-is-not", test_null_is_not);

	g_test_add_func("/bar-icon/tint/keeps-the-shape",
	                test_tint_keeps_the_shape);
	g_test_add_func("/bar-icon/tint/paints-the-colour",
	                test_tint_paints_the_colour);
	g_test_add_func("/bar-icon/tint/stays-premultiplied",
	                test_tint_stays_premultiplied);
	g_test_add_func("/bar-icon/tint/does-not-touch-the-original",
	                test_tint_does_not_touch_the_original);
	g_test_add_func("/bar-icon/tint/null", test_tint_null);

	return g_test_run();
}
