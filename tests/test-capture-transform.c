/* test-capture-transform.c -- a capture of a rotated output is upright
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * gowl_fx_capture() renders the scene into the OUTPUT'S OWN buffer, and
 * wlroots has already applied the output transform there: a 160x96
 * panel turned a quarter renders 160x96 pixels with the desktop lying on
 * its side in them.  Everything that reads a capture -- the backdrop
 * plans, the blur's crop, the magnifier, every sheet -- works in layout
 * coordinates and hands its result back to the scene, which turns it
 * AGAIN on the way out.
 *
 * So the capture has to be stored upright, or every translucent window
 * on a rotated screen shows the wallpaper 90 degrees off and stretched
 * while the wallpaper around it is the right way up.  That is invisible
 * in source and only obvious on a rotated panel, which is exactly the
 * screen nobody reviewing has.
 *
 * Checked here against REAL wlr_scene and a real headless output, for
 * all eight transforms: three coloured squares in three corners of the
 * LOGICAL screen must come back in the same three corners of the stored
 * texture, at the logical size.  Three, not two, because a transpose
 * keeps the top-left and bottom-right corners where they were.
 *
 * And the cache key every backdrop keeps its capture under must notice a
 * half turn (which keeps the size) and a new wallpaper (which nothing
 * but the wallpaper provider knows about).
 *
 * Needs a render node; skips without one.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include <drm_fourcc.h>
#include <wayland-server-core.h>

#include <wlr/render/allocator.h>
#include <wlr/render/gles2.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "config/gowl-config.h"
#include "module/gowl-module-manager.h"
#include "fx/gowl-fx.h"

/* Not square, and not a multiple of each other, so a swapped axis or a
 * stretch cannot land a square in the right corner by accident. */
#define RIG_SIZE "160x96"
#define RIG_W    (160)
#define RIG_H    (96)
#define MARK     (24)

typedef struct {
	gchar             *parent;
	gchar             *runtime;
	GowlModuleManager *modules;
	GowlConfig        *config;
	GowlCompositor    *compositor;
	GowlFxGl          *gl;
	gboolean           started;
} Rig;

static gboolean
rig_up(Rig *r)
{
	const gchar *parent;
	GError      *error = NULL;

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	/* Nothing may reach the session this runs in. */
	r->parent = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	parent = r->parent;
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-capture-xform-XXXXXX",
	                              NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "gles2", TRUE);
	g_setenv("GOWL_OUTPUT_SIZE", RIG_SIZE, TRUE);

	r->modules = gowl_module_manager_new();
	r->config = gowl_config_new();
	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_clear_error(&error);
		return FALSE;
	}
	r->started = TRUE;
	if (!wlr_renderer_is_gles2(r->compositor->renderer)
	    || !gowl_fx_gl_supported(r->compositor->renderer))
		return FALSE;
	g_assert_nonnull(r->compositor->selmon);

	r->gl = gowl_fx_gl_new(r->compositor->renderer);
	return r->gl != NULL;
}

static void
rig_down(Rig *r)
{
	/* The context borrows the renderer: it goes first. */
	if (r->gl != NULL)
		gowl_fx_gl_free(r->gl);
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	if (r->started)
		g_assert_cmpint(g_rmdir(r->runtime), ==, 0);
	else
		g_rmdir(r->runtime);
	g_free(r->runtime);
	if (r->parent != NULL)
		g_setenv("XDG_RUNTIME_DIR", r->parent, TRUE);
	else
		g_unsetenv("XDG_RUNTIME_DIR");
	g_free(r->parent);
	g_unsetenv("GOWL_OUTPUT_SIZE");
}

/*
 * A stored texture, drawn back out through the effect layer's own quad
 * into a buffer of the same size and read.  The quad's orientation is
 * pinned by test-fx-render (row 0 of the texture lands on row 0 of the
 * buffer), so what comes out is the texture as stored.
 */
static guint8 *
read_texture(Rig *r, const GowlFxTexture *tex)
{
	struct wlr_drm_format format;
	uint64_t              modifier = DRM_FORMAT_MOD_INVALID;
	struct wlr_swapchain *swapchain;
	struct wlr_buffer    *buffer;
	struct wlr_texture   *texture;
	struct wlr_texture_read_pixels_options opts;
	GowlFxPass *pass;
	GowlFxQuad  quad;
	gfloat      clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	guint8     *out = NULL;

	memset(&format, 0, sizeof(format));
	format.format    = DRM_FORMAT_XRGB8888;
	format.len       = 1;
	format.capacity  = 1;
	format.modifiers = &modifier;

	swapchain = wlr_swapchain_create(r->compositor->allocator,
	                                 tex->width, tex->height, &format);
	g_assert_nonnull(swapchain);
	buffer = wlr_swapchain_acquire(swapchain);
	g_assert_nonnull(buffer);

	pass = gowl_fx_pass_begin(r->gl, buffer);
	g_assert_nonnull(pass);
	gowl_fx_pass_clear(pass, clear);
	gowl_fx_quad_init(&quad);
	quad.texture = tex->tex;
	gowl_fx_pass_quad(pass, &quad);
	gowl_fx_pass_end(pass);

	texture = wlr_texture_from_buffer(r->compositor->renderer, buffer);
	g_assert_nonnull(texture);
	out = g_malloc0((gsize)tex->width * (gsize)tex->height * 4);
	memset(&opts, 0, sizeof(opts));
	opts.data   = out;
	opts.format = DRM_FORMAT_ARGB8888;
	opts.stride = (uint32_t)tex->width * 4;
	g_assert_true(wlr_texture_read_pixels(texture, &opts));

	wlr_texture_destroy(texture);
	wlr_buffer_unlock(buffer);
	wlr_swapchain_destroy(swapchain);
	return out;
}

/* Which of the four marker colours a pixel is, as a letter: r g b, or
 * k for the black left between them. */
static gchar
colour_at(const guint8 *pixels, gint width, gint x, gint y)
{
	const guint8 *p = pixels + ((gsize)y * (gsize)width + (gsize)x) * 4;
	gint b = p[0], g = p[1], r = p[2];

	if (r > 160 && g < 80 && b < 80)
		return 'r';
	if (g > 160 && r < 80 && b < 80)
		return 'g';
	if (b > 160 && r < 80 && g < 80)
		return 'b';
	if (r < 60 && g < 60 && b < 60)
		return 'k';
	return '?';
}

static struct wlr_scene_rect *
mark(Rig *r, gint x, gint y, gint w, gint h, gfloat red, gfloat green,
     gfloat blue)
{
	gfloat colour[4] = { red, green, blue, 1.0f };
	struct wlr_scene_rect *rect;

	rect = wlr_scene_rect_create(
		r->compositor->layers[GOWL_SCENE_LAYER_BG], w, h, colour);
	g_assert_nonnull(rect);
	wlr_scene_node_set_position(&rect->node, x, y);
	return rect;
}

/* ── The capture comes back upright for every transform ──────────── */

static void
test_capture_is_upright(void)
{
	Rig    r;
	gint   t;

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no GLES2 renderer to draw with here");
		return;
	}

	for (t = 0; t < 8; t++) {
		GowlMonitor           *m = r.compositor->selmon;
		struct wlr_scene_rect *ground, *tl, *tr, *bl;
		GowlFxTexture          tex;
		guint8                *pixels;
		gint                   lw, lh, cw, ch;

		g_assert_true(gowl_monitor_set_transform(m, t));
		g_assert_cmpint(m->wlr_output->transform, ==, t);

		/* Logical: the mode with the axes swapped on a quarter turn. */
		lw = m->m.width;
		lh = m->m.height;
		g_assert_cmpint(lw, ==, (t & 1) ? RIG_H : RIG_W);
		g_assert_cmpint(lh, ==, (t & 1) ? RIG_W : RIG_H);
		gowl_fx_capture_size(m, 1, &cw, &ch);
		g_assert_cmpint(cw, ==, lw);
		g_assert_cmpint(ch, ==, lh);

		/* Black ground, then red top-left, green top-right and blue
		 * bottom-left of the screen as the user sees it. */
		ground = mark(&r, m->m.x, m->m.y, lw, lh, 0.0f, 0.0f, 0.0f);
		tl = mark(&r, m->m.x, m->m.y, MARK, MARK, 1.0f, 0.0f, 0.0f);
		tr = mark(&r, m->m.x + lw - MARK, m->m.y, MARK, MARK,
		          0.0f, 1.0f, 0.0f);
		bl = mark(&r, m->m.x, m->m.y + lh - MARK, MARK, MARK,
		          0.0f, 0.0f, 1.0f);

		memset(&tex, 0, sizeof(tex));
		g_assert_true(gowl_fx_capture(r.gl, r.compositor, m, &tex, 1));
		g_assert_cmpint(tex.width, ==, lw);
		g_assert_cmpint(tex.height, ==, lh);

		pixels = read_texture(&r, &tex);
		if (colour_at(pixels, lw, MARK / 2, MARK / 2) != 'r'
		    || colour_at(pixels, lw, lw - MARK / 2, MARK / 2) != 'g'
		    || colour_at(pixels, lw, MARK / 2, lh - MARK / 2) != 'b'
		    || colour_at(pixels, lw, lw - MARK / 2, lh - MARK / 2) != 'k'
		    || colour_at(pixels, lw, lw / 2, lh / 2) != 'k') {
			g_error("transform %d: corners TL=%c TR=%c BL=%c BR=%c "
			        "centre=%c, want r g b k k -- the capture is "
			        "not upright", t,
			        colour_at(pixels, lw, MARK / 2, MARK / 2),
			        colour_at(pixels, lw, lw - MARK / 2, MARK / 2),
			        colour_at(pixels, lw, MARK / 2, lh - MARK / 2),
			        colour_at(pixels, lw, lw - MARK / 2,
			                  lh - MARK / 2),
			        colour_at(pixels, lw, lw / 2, lh / 2));
		}

		g_free(pixels);
		gowl_fx_texture_drop(r.gl, &tex);
		wlr_scene_node_destroy(&bl->node);
		wlr_scene_node_destroy(&tr->node);
		wlr_scene_node_destroy(&tl->node);
		wlr_scene_node_destroy(&ground->node);
	}

	rig_down(&r);
}

/* ── The cache key notices what the size alone cannot ───────────── */

static void
test_capture_key_sees_rotation_and_wallpaper(void)
{
	Rig              r;
	GowlMonitor     *m;
	GowlFxCaptureKey key;

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no GLES2 renderer to draw with here");
		return;
	}
	m = r.compositor->selmon;

	/* A key never taken matches nothing: an empty cache is stale. */
	memset(&key, 0, sizeof(key));
	g_assert_false(gowl_fx_capture_key_matches(&key, m));

	g_assert_true(gowl_monitor_set_transform(m, 1));
	gowl_fx_capture_key_take(&key, m);
	g_assert_true(gowl_fx_capture_key_matches(&key, m));

	/*
	 * A HALF turn keeps the size -- which is all the backdrops used to
	 * key on, and why turning a panel from 90 to 270 left the picture
	 * taken at 90 behind every window.
	 */
	g_assert_true(gowl_monitor_set_transform(m, 3));
	g_assert_cmpint(m->wlr_output->width, ==, RIG_W);
	g_assert_false(gowl_fx_capture_key_matches(&key, m));

	gowl_fx_capture_key_take(&key, m);
	g_assert_true(gowl_fx_capture_key_matches(&key, m));

	/* A new wallpaper changes nothing the output can see; only the
	 * provider saying so does. */
	gowl_compositor_backdrop_changed(r.compositor, m);
	g_assert_false(gowl_fx_capture_key_matches(&key, m));

	/* ...and NULL means every output. */
	gowl_fx_capture_key_take(&key, m);
	gowl_compositor_backdrop_changed(r.compositor, NULL);
	g_assert_false(gowl_fx_capture_key_matches(&key, m));

	rig_down(&r);
}

/* ── A sheet is upright too, and follows the panel ──────────────── */

static void
test_sheet_follows_rotation(void)
{
	Rig          r;
	GowlMonitor *m;
	GowlFxSheet *sheet;
	struct wlr_buffer *buffer;
	gint         w, h;

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no GLES2 renderer to draw with here");
		return;
	}
	m = r.compositor->selmon;

	g_assert_true(gowl_monitor_set_transform(m, 1));
	sheet = gowl_fx_sheet_new(r.compositor, m, GOWL_FX_SHEET_ABOVE_TOP);
	g_assert_nonnull(sheet);
	gowl_fx_sheet_get_size(sheet, &w, &h);
	g_assert_cmpint(w, ==, RIG_H);
	g_assert_cmpint(h, ==, RIG_W);

	/*
	 * The tube and the magnifier keep a sheet up for as long as they are
	 * on, so the panel can be turned under one.  The next buffer must be
	 * the new shape, or the effect is squeezed into the other axis.
	 */
	g_assert_true(gowl_monitor_set_transform(m, 0));
	gowl_fx_sheet_get_size(sheet, &w, &h);
	g_assert_cmpint(w, ==, RIG_W);
	g_assert_cmpint(h, ==, RIG_H);
	buffer = gowl_fx_sheet_acquire(sheet);
	g_assert_nonnull(buffer);
	g_assert_cmpint(buffer->width, ==, RIG_W);
	g_assert_cmpint(buffer->height, ==, RIG_H);
	wlr_buffer_unlock(buffer);

	gowl_fx_sheet_free(sheet);
	rig_down(&r);
}

/* ── The corner map itself ──────────────────────────────────────── */

static void
test_upright_uv_normal_is_identity(void)
{
	static const gfloat identity[8] = { 0.0f, 0.0f, 1.0f, 0.0f,
	                                    0.0f, 1.0f, 1.0f, 1.0f };
	gfloat uv[8];
	gint   i, t;

	gowl_fx_upright_uv(0, uv);
	for (i = 0; i < 8; i++)
		g_assert_cmpfloat(uv[i], ==, identity[i]);

	/* Every transform maps the unit square's corners onto its corners,
	 * each exactly once. */
	for (t = 0; t < 8; t++) {
		guint seen = 0;

		gowl_fx_upright_uv(t, uv);
		for (i = 0; i < 4; i++) {
			g_assert_true(uv[i * 2] == 0.0f || uv[i * 2] == 1.0f);
			g_assert_true(uv[i * 2 + 1] == 0.0f
			              || uv[i * 2 + 1] == 1.0f);
			seen |= 1u << ((gint)uv[i * 2] + 2 * (gint)uv[i * 2 + 1]);
		}
		g_assert_cmpuint(seen, ==, 0xfu);
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/capture-transform/uv-corners",
	                test_upright_uv_normal_is_identity);
	g_test_add_func("/capture-transform/upright-for-every-transform",
	                test_capture_is_upright);
	g_test_add_func("/capture-transform/key-sees-rotation-and-wallpaper",
	                test_capture_key_sees_rotation_and_wallpaper);
	g_test_add_func("/capture-transform/sheet-follows-rotation",
	                test_sheet_follows_rotation);

	return g_test_run();
}
