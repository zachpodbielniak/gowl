/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Tag transitions on a real GPU: the drawing primitives, and every
 * transition module this tree built, held to the SEAM rule.
 *
 * The first and last frames of a tag switch are presented.  If at
 * progress 0 a transition draws anything but the desktop being left --
 * a pixel off, a little dimmer, a sliver of backdrop at an edge -- the
 * switch begins with a flash, and likewise at 1 on arrival.  So for each
 * transition this draws progress 0 and 1 over two desktops that are
 * unmistakable (each a different two-way gradient, so a picture that is
 * shifted, scaled, flipped or mirrored is caught, not just a wrong
 * colour), and compares a grid of pixels against the desktop that
 * should be there.  Halfway it must be NEITHER: a transition that does
 * nothing mid-flight passes the seams and is still broken.
 *
 * Modules are found by opening every .so in GOWL_TEST_MODULE_DIR and
 * asking for `gowl_transition_kind', which
 * GOWL_FX_DEFINE_TRANSITION_MODULE exports -- so a new transition is
 * checked here without this file knowing it exists.
 *
 * Set GOWL_TEST_TRANSITION_DUMP=DIR to also write each transition's
 * frames as PNGs, which is how to look at one without a session.
 *
 * Skips itself without a DRM render node.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gmodule.h>
#include <stdlib.h>
#include <string.h>
#include <cairo.h>

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
#include "fx/gowl-fx-transition.h"

#define TEST_W 320
#define TEST_H 180
/* Per channel, out of 255: room for filtering and 8-bit rounding, far
 * below the difference between the two desktops anywhere. */
#define SEAM_TOLERANCE 10

typedef struct {
	struct wl_event_loop *loop;
	struct wlr_backend   *backend;
	struct wlr_renderer  *renderer;
	struct wlr_allocator *allocator;
	struct wlr_swapchain *swapchain;
	GowlFxGl             *gl;
	GowlFxTexture         from;
	GowlFxTexture         to;
} Fixture;

/* ── Desktops ────────────────────────────────────────────────────── */

/* Desktop A: red runs left to right, blue top to bottom, green low.
 * Desktop B: the other way round, green high.  Nowhere alike. */
static void
desktop_pixel(gboolean b_side, gint x, gint y, guint8 *r, guint8 *g,
              guint8 *bl)
{
	guint8 u = (guint8)(30 + x * 195 / (TEST_W - 1));
	guint8 v = (guint8)(30 + y * 195 / (TEST_H - 1));

	if (!b_side) {
		*r = u;   *g = 20;  *bl = v;
	} else {
		*r = v;   *g = 235; *bl = u;
	}
}

static struct wlr_texture *
make_desktop(struct wlr_renderer *renderer, gboolean b_side)
{
	guint8 *pixels = g_malloc0((gsize)TEST_W * TEST_H * 4);
	struct wlr_texture *texture;
	gint x, y;

	for (y = 0; y < TEST_H; y++) {
		for (x = 0; x < TEST_W; x++) {
			guint8 *p = pixels + ((gsize)y * TEST_W + x) * 4;

			/* ARGB8888 little-endian: B, G, R, A in memory. */
			desktop_pixel(b_side, x, y, &p[2], &p[1], &p[0]);
			p[3] = 0xff;
		}
	}
	texture = wlr_texture_from_pixels(renderer, DRM_FORMAT_ARGB8888,
	                                  TEST_W * 4, TEST_W, TEST_H, pixels);
	g_free(pixels);
	return texture;
}

static gboolean
fixture_open(Fixture *f)
{
	struct wlr_drm_format format;
	uint64_t modifier = DRM_FORMAT_MOD_INVALID;
	struct wlr_texture *t;

	memset(f, 0, sizeof(*f));
	f->loop = wl_event_loop_create();
	f->backend = f->loop != NULL ? wlr_headless_backend_create(f->loop) : NULL;
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
	f->swapchain = wlr_swapchain_create(f->allocator, TEST_W, TEST_H,
	                                    &format);
	if (f->swapchain == NULL)
		return FALSE;
	f->gl = gowl_fx_gl_new(f->renderer);
	if (f->gl == NULL)
		return FALSE;

	t = make_desktop(f->renderer, FALSE);
	g_assert_true(gowl_fx_texture_store(f->gl, &f->from, t, TEST_W, TEST_H));
	wlr_texture_destroy(t);
	t = make_desktop(f->renderer, TRUE);
	g_assert_true(gowl_fx_texture_store(f->gl, &f->to, t, TEST_W, TEST_H));
	wlr_texture_destroy(t);
	return TRUE;
}

static void
fixture_close(Fixture *f)
{
	if (f->gl != NULL) {
		gowl_fx_texture_drop(f->gl, &f->from);
		gowl_fx_texture_drop(f->gl, &f->to);
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

/* ── Reading back ────────────────────────────────────────────────── */

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

/* How far @pixels is from the desktop at worst, over a grid that keeps
 * two pixels clear of the edges (sampling there is the filter's call). */
static gint
worst_error(const guint8 *pixels, gboolean b_side)
{
	gint worst = 0;
	gint x, y;

	for (y = 2; y < TEST_H - 2; y += 7) {
		for (x = 2; x < TEST_W - 2; x += 9) {
			const guint8 *p = pixels + ((gsize)y * TEST_W + x) * 4;
			guint8 r, g, b;

			desktop_pixel(b_side, x, y, &r, &g, &b);
			worst = MAX(worst, ABS((gint)p[2] - (gint)r));
			worst = MAX(worst, ABS((gint)p[1] - (gint)g));
			worst = MAX(worst, ABS((gint)p[0] - (gint)b));
		}
	}
	return worst;
}

static void
dump_png(const guint8 *pixels, const gchar *name, gdouble progress)
{
	const gchar *dir = g_getenv("GOWL_TEST_TRANSITION_DUMP");
	cairo_surface_t *s;
	g_autofree gchar *file = NULL;
	gint y;

	if (dir == NULL || *dir == '\0')
		return;
	g_mkdir_with_parents(dir, 0755);
	s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, TEST_W, TEST_H);
	cairo_surface_flush(s);
	for (y = 0; y < TEST_H; y++)
		memcpy(cairo_image_surface_get_data(s)
		       + (gsize)y * (gsize)cairo_image_surface_get_stride(s),
		       pixels + (gsize)y * TEST_W * 4, TEST_W * 4);
	cairo_surface_mark_dirty(s);
	file = g_strdup_printf("%s/%s-%03d.png", dir, name,
	                       (gint)(progress * 100.0 + 0.5));
	cairo_surface_write_to_png(s, file);
	cairo_surface_destroy(s);
}

/* ── Drawing one frame ───────────────────────────────────────────── */

typedef gboolean (*DrawFunc) (GowlFxPass *pass, gpointer data, gdouble p);

static guint8 *
render(Fixture *f, DrawFunc draw, gpointer data, gdouble p)
{
	struct wlr_buffer *buffer = wlr_swapchain_acquire(f->swapchain);
	GowlFxPass *pass;
	gfloat black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	guint8 *pixels;
	gboolean ok;

	g_assert_nonnull(buffer);
	pass = gowl_fx_pass_begin(f->gl, buffer);
	g_assert_nonnull(pass);
	/* Black first, so a transition that leaves part of the output
	 * undrawn at a seam shows it rather than inheriting the last frame. */
	gowl_fx_pass_clear(pass, black);
	ok = draw(pass, data, p);
	g_assert_true(gowl_fx_pass_end(pass));
	g_assert_true(ok);
	pixels = read_back(f->renderer, buffer);
	wlr_buffer_unlock(buffer);
	return pixels;
}

/*
 * The seam rule for one drawing: the desktop left at 0, the one arrived
 * at at 1, neither halfway.  Returns FALSE (and skips) when pixels
 * cannot be read back on this renderer.
 */
static gboolean
check_seams(Fixture *f, const gchar *name, DrawFunc draw, gpointer data)
{
	static const gdouble frames[] = { 0.15, 0.3, 0.5, 0.7, 0.85 };
	guint8 *px;
	gsize i;

	px = render(f, draw, data, 0.0);
	if (px == NULL)
		return FALSE;
	dump_png(px, name, 0.0);
	if (worst_error(px, FALSE) > SEAM_TOLERANCE)
		g_error("%s: progress 0 is not the desktop being left (worst "
		        "channel off by %d)", name, worst_error(px, FALSE));
	g_free(px);

	px = render(f, draw, data, 1.0);
	g_assert_nonnull(px);
	dump_png(px, name, 1.0);
	if (worst_error(px, TRUE) > SEAM_TOLERANCE)
		g_error("%s: progress 1 is not the desktop arrived at (worst "
		        "channel off by %d)", name, worst_error(px, TRUE));
	g_free(px);

	px = render(f, draw, data, 0.5);
	g_assert_nonnull(px);
	if (worst_error(px, FALSE) <= SEAM_TOLERANCE
	    || worst_error(px, TRUE) <= SEAM_TOLERANCE)
		g_error("%s: halfway is still one of the two desktops; the "
		        "transition does nothing", name);
	g_free(px);

	for (i = 0; i < G_N_ELEMENTS(frames); i++) {
		if (g_getenv("GOWL_TEST_TRANSITION_DUMP") == NULL)
			break;
		px = render(f, draw, data, frames[i]);
		dump_png(px, name, frames[i]);
		g_free(px);
	}
	return TRUE;
}

/* ── The primitives ──────────────────────────────────────────────── */

static gboolean
draw_fade_blend(GowlFxPass *pass, gpointer data, gdouble p)
{
	Fixture *f = data;
	GowlFxBlendParams params;

	gowl_fx_blend_params_init(&params);
	params.kind = GOWL_FX_BLEND_FADE;
	params.from = f->from.tex;
	params.to = f->to.tex;
	params.progress = (gfloat)p;
	return gowl_fx_pass_blend(pass, &params);
}

/* The flat desktop through an ortho MVP and the default rectangle must
 * land exactly where the screen-filling quad does. */
static gboolean
draw_ortho_desktops(GowlFxPass *pass, gpointer data, gdouble p)
{
	Fixture *f = data;
	gfloat mvp[16];

	gowl_fx_mat4_ortho(mvp, TEST_W, TEST_H);
	gowl_fx_transition_pass_desktop(pass, &f->from, mvp, NULL,
	                                (gfloat)(1.0 - p), 1.0f);
	if (p > 0.0)
		gowl_fx_transition_pass_desktop(pass, &f->to, mvp, NULL,
		                                (gfloat)p, 1.0f);
	return TRUE;
}

/* A mesh of two triangles covering the output, same orientation rule. */
static gboolean
draw_mesh_desktop(GowlFxPass *pass, gpointer data, gdouble p)
{
	Fixture *f = data;
	gfloat mvp[16];
	gfloat w = (gfloat)TEST_W, h = (gfloat)TEST_H;
	const gfloat v[6 * GOWL_FX_MESH_STRIDE] = {
		/* x  y  z   u  v   shade alpha */
		0, 0, 0,  0, 0,  1, 1,
		0, h, 0,  0, 1,  1, 1,
		w, 0, 0,  1, 0,  1, 1,
		w, 0, 0,  1, 0,  1, 1,
		0, h, 0,  0, 1,  1, 1,
		w, h, 0,  1, 1,  1, 1,
	};
	GowlFxMesh mesh;

	gowl_fx_mat4_ortho(mvp, TEST_W, TEST_H);
	memset(&mesh, 0, sizeof mesh);
	mesh.mvp = mvp;
	mesh.verts = v;
	mesh.n_verts = 6;
	mesh.texture = p < 0.5 ? f->from.tex : f->to.tex;
	mesh.tint[0] = mesh.tint[1] = mesh.tint[2] = 1.0f;
	mesh.alpha = 1.0f;
	/* Halfway, dim it so "neither desktop" holds for the check. */
	if (p == 0.5)
		mesh.tint[0] = mesh.tint[1] = mesh.tint[2] = 0.3f;
	return gowl_fx_pass_mesh(pass, &mesh);
}

static void
test_primitives(void)
{
	Fixture f;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 renderer available (no DRM render node?)");
		return;
	}
	g_assert_true(gowl_fx_blend_supported(GOWL_FX_BLEND_FADE));
	if (!check_seams(&f, "prim-blend-fade", draw_fade_blend, &f)) {
		fixture_close(&f);
		g_test_skip("cannot read pixels back from this renderer");
		return;
	}
	check_seams(&f, "prim-ortho-desktop", draw_ortho_desktops, &f);
	check_seams(&f, "prim-mesh", draw_mesh_desktop, &f);
	fixture_close(&f);
}

/* ── Every transition module ─────────────────────────────────────── */

typedef struct {
	Fixture                     *f;
	const GowlFxTransitionKind  *kind;
	GowlFxTransitionRun         *run;
} KindRun;

static gboolean
draw_kind(GowlFxPass *pass, gpointer data, gdouble p)
{
	KindRun *k = data;

	return k->kind->draw(pass, k->run, p, p);
}

static void
check_kind(Fixture *f, const GowlFxTransitionKind *kind, gint dir)
{
	GowlFxTransitionRun run;
	KindRun k;
	g_autofree gchar *name = g_strdup_printf("%s%s", kind->name,
	                                         dir > 0 ? "" : "-back");

	memset(&run, 0, sizeof run);
	run.from_tags  = dir > 0 ? 1 : 4;
	run.to_tags    = dir > 0 ? 4 : 1;
	run.from_index = dir > 0 ? 0 : 2;
	run.to_index   = dir > 0 ? 2 : 0;
	run.dir        = dir;
	/* Two steps, so a multi-step kind crosses an intermediate -- which
	 * it gets as "the nearer end", there being no capture of it here. */
	run.steps      = kind->multi_step ? 2 : 1;
	run.from       = f->from;
	run.to         = f->to;
	run.width      = TEST_W;
	run.height     = TEST_H;
	run.dur_us     = G_USEC_PER_SEC;
	run.center[0]  = 0.3f;
	run.center[1]  = 0.6f;
	run.seed       = 12345u;

	if (kind->setup != NULL)
		g_assert_true(kind->setup(&run));
	k.f = f;
	k.kind = kind;
	k.run = &run;
	check_seams(f, name, draw_kind, &k);
	if (kind->teardown != NULL)
		kind->teardown(&run);
	/* Not freed: they belong to the fixture. */
	memset(&run.from, 0, sizeof run.from);
	memset(&run.to, 0, sizeof run.to);
}

static void
test_every_transition_module(void)
{
	Fixture f;
	GDir *dir;
	const gchar *entry;
	gint tested = 0;

	if (!fixture_open(&f)) {
		fixture_close(&f);
		g_test_skip("no GLES2 renderer available (no DRM render node?)");
		return;
	}
	dir = g_dir_open(GOWL_TEST_MODULE_DIR, 0, NULL);
	if (dir == NULL) {
		fixture_close(&f);
		g_test_skip("no module directory; run `make modules' first");
		return;
	}
	while ((entry = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *path = NULL;
		GModule *mod;
		const GowlFxTransitionKind *(*get_kind)(void) = NULL;

		if (!g_str_has_suffix(entry, ".so"))
			continue;
		path = g_build_filename(GOWL_TEST_MODULE_DIR, entry, NULL);
		mod = g_module_open(path, G_MODULE_BIND_LOCAL | G_MODULE_BIND_LAZY);
		if (mod == NULL)
			continue;
		if (!g_module_symbol(mod, "gowl_transition_kind",
		                     (gpointer *)&get_kind) || get_kind == NULL) {
			g_module_close(mod);
			continue;
		}
		g_test_message("transition: %s", get_kind()->name);
		check_kind(&f, get_kind(), 1);
		check_kind(&f, get_kind(), -1);
		tested++;
		/* Kept open: closing would unmap code a GType still points at. */
		g_module_make_resident(mod);
	}
	g_dir_close(dir);
	g_test_message("%d transition module(s) checked", tested);
	fixture_close(&f);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/transition-render/primitives", test_primitives);
	g_test_add_func("/transition-render/every-transition-module",
	                test_every_transition_module);

	return g_test_run();
}
