/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * gowl-fx-private.h -- shared between the fx translation units only.
 * Not installed, not for modules: a module gets GowlFxGl as an opaque
 * pointer, which is what keeps GL out of five module files.
 */

#ifndef GOWL_FX_PRIVATE_H
#define GOWL_FX_PRIVATE_H

#include "gowl-fx.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>

G_BEGIN_DECLS

typedef struct {
	EGLDisplay display;
	EGLContext context;
	EGLSurface draw;
	EGLSurface read;
	gboolean   restored;
} GowlFxEglSave;

typedef struct {
	GLuint program;
	GLint  u_tex;
	GLint  u_step;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxSamplerProg;

typedef struct {
	GLuint program;
	GLint  u_color;
	GLint  u_alpha;
	GLint  u_aspect;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxBackdropProg;

typedef struct {
	GLuint program;
	GLint  u_mvp;
	GLint  u_tex;
	GLint  u_tint;
	GLint  u_base;
	GLint  u_texamt;
	GLint  u_blur;
	GLint  u_edge;
	GLint  u_edge_width;
	GLint  u_spec;
	GLint  u_alpha;
	GLint  u_fade;
	GLint  u_corner;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxQuadProg;

/*
 * The liquid-glass program (gowl-fx-glass.c).
 *
 * Kept out of gowl_fx_gl_new() and built on first use: a driver that
 * cannot compile it must cost the desktop the glass alone, not the cube,
 * the overview, the switcher, the magnifier and the blur along with it.
 */
typedef struct {
	GLuint program;
	GLint  u_soft;
	GLint  u_sharp;
	GLint  u_src_origin;
	GLint  u_src_size;
	GLint  u_src_scale;
	GLint  u_size;
	GLint  u_radius;
	GLint  u_bevel;
	GLint  u_thickness;
	GLint  u_slope;
	GLint  u_maxd;
	GLint  u_shape;
	GLint  u_dispersion;
	GLint  u_rim;
	GLint  u_shade;
	GLint  u_edge_w;
	GLint  u_light;
	GLint  u_sat;
	GLint  u_clarity;
	GLint  u_centre_clarity;
	GLint  u_lens;
	GLint  u_sheen;
	GLint  u_tint;
	GLint  u_brightness;
	GLint  u_alpha;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxGlassProg;

/*
 * The liquid-water program (gowl-fx-water.c).
 *
 * Built on first use like the glass, and for the same reason.
 */
typedef struct {
	GLuint program;
	GLint  u_soft;
	GLint  u_sharp;
	GLint  u_src_origin;
	GLint  u_src_size;
	GLint  u_src_scale;
	GLint  u_size;
	GLint  u_radius;
	GLint  u_phase;
	GLint  u_drop_t;
	GLint  u_amp;
	GLint  u_wavelength;
	GLint  u_choppy;
	GLint  u_depth;
	GLint  u_drops;
	GLint  u_drop_amp;
	GLint  u_shore;
	GLint  u_dispersion;
	GLint  u_specular;
	GLint  u_shine;
	GLint  u_fresnel;
	GLint  u_reflect;
	GLint  u_caustics;
	GLint  u_foam;
	GLint  u_meniscus;
	GLint  u_light;
	GLint  u_tint;
	GLint  u_absorb;
	GLint  u_clarity;
	GLint  u_brightness;
	GLint  u_alpha;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxWaterProg;

/*
 * The liquid-rain program (gowl-fx-rain.c).
 *
 * Built on first use like the glass and the water, and for the same
 * reason.
 */
typedef struct {
	GLuint program;
	GLint  u_soft;
	GLint  u_sharp;
	GLint  u_src_origin;
	GLint  u_src_size;
	GLint  u_src_scale;
	GLint  u_size;
	GLint  u_radius;
	GLint  u_life;
	GLint  u_run;
	GLint  u_cell;
	GLint  u_density;
	GLint  u_bulge;
	GLint  u_depth;
	GLint  u_dispersion;
	GLint  u_runs;
	GLint  u_run_width;
	GLint  u_run_len;
	GLint  u_beads;
	GLint  u_fog;
	GLint  u_clarity;
	GLint  u_specular;
	GLint  u_shine;
	GLint  u_rim;
	GLint  u_impact;
	GLint  u_light;
	GLint  u_tint;
	GLint  u_absorb;
	GLint  u_brightness;
	GLint  u_alpha;
	GLint  u_seed;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxRainProg;

struct _GowlFxGl {
	struct wlr_renderer *renderer;   /* borrowed */
	EGLDisplay           display;
	EGLContext           context;

	GowlFxQuadProg       quad;
	GowlFxBackdropProg   backdrop;
	GowlFxSamplerProg    copy_2d;
	GowlFxSamplerProg    copy_ext;   /* program 0 without the extension */
	GowlFxSamplerProg    blur;
	GowlFxGlassProg      glass;      /* built on first use, see above */
	gboolean             glass_tried;
	GowlFxWaterProg      water;      /* likewise */
	gboolean             water_tried;
	GowlFxRainProg       rain;       /* likewise */
	gboolean             rain_tried;

	GLuint               scratch_fbo;
	GowlFxTexture        scratch_a;
	GowlFxTexture        scratch_b;
};

struct _GowlFxPass {
	GowlFxGl      *gl;
	GowlFxEglSave  save;
	gint           width;
	gint           height;
};

gboolean gowl_fx_egl_enter (GowlFxGl *self, GowlFxEglSave *save);
void     gowl_fx_egl_leave (GowlFxEglSave *save);
GLuint   gowl_fx_link_program (const gchar *vert_src, const gchar *frag_src);
void     gowl_fx_draw_screen_quad (GLint a_pos, GLint a_uv);
gboolean gowl_fx_texture_alloc (GowlFxTexture *tex, gint width, gint height);

G_END_DECLS

/*
 * Every GowlFxSheet currently parked in a scene.
 *
 * A sheet is a direct child of scene->tree -- a SIBLING of the layer
 * trees, not inside one -- so gowl_fx_vis_hide_layer() cannot reach it.
 * Anything capturing the output has to be able to, because a sheet is an
 * opaque monitor-sized picture of the desktop WITH its windows in it.
 * Compositor thread only; no locking.
 */
GList *gowl_fx_sheet_live (void);

/* The sheet's scene tree, so a capture can switch it off. */
struct wlr_scene_tree *gowl_fx_sheet_tree (GowlFxSheet *sheet);

#endif /* GOWL_FX_PRIVATE_H */