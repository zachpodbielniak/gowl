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
	GLint  u_flash;
	GLint  u_bolt;
	GLint  u_light;
	GLint  u_tint;
	GLint  u_absorb;
	GLint  u_brightness;
	GLint  u_alpha;
	GLint  u_seed;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxRainProg;

/*
 * The carbonation program (gowl-fx-fizz.c).
 *
 * Built on first use like the glass, the water and the rain, and for the
 * same reason.
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
	GLint  u_cling_t;
	GLint  u_rise;
	GLint  u_cell;
	GLint  u_bubble;
	GLint  u_growth;
	GLint  u_sites;
	GLint  u_site_width;
	GLint  u_spacing;
	GLint  u_stray;
	GLint  u_cling;
	GLint  u_wobble;
	GLint  u_foam;
	GLint  u_foam_depth;
	GLint  u_depth;
	GLint  u_dispersion;
	GLint  u_mirror;
	GLint  u_fog;
	GLint  u_clarity;
	GLint  u_specular;
	GLint  u_shine;
	GLint  u_rim;
	GLint  u_light;
	GLint  u_tint;
	GLint  u_absorb;
	GLint  u_brightness;
	GLint  u_alpha;
	GLint  u_seed;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxFizzProg;

/*
 * The falling-leaves program (gowl-fx-leaves.c).
 *
 * Built on first use like the rest.  The only one here whose subject is
 * OPAQUE: a leaf covers the wallpaper rather than bending it, which is
 * why it carries three tints and no refraction depth.
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
	GLint  u_stick_t;
	GLint  u_fall;
	GLint  u_gust;
	GLint  u_sway;
	GLint  u_leaf;
	GLint  u_cell;
	GLint  u_stuck;
	GLint  u_column;
	GLint  u_falling;
	GLint  u_flutter;
	GLint  u_tumble;
	GLint  u_wind;
	GLint  u_gust_push;
	GLint  u_curl;
	GLint  u_veins;
	GLint  u_translucency;
	GLint  u_gloss;
	GLint  u_shadow;
	GLint  u_fog;
	GLint  u_clarity;
	GLint  u_shine;
	GLint  u_tint_warm;
	GLint  u_tint_gold;
	GLint  u_tint_dry;
	GLint  u_light;
	GLint  u_brightness;
	GLint  u_alpha;
	GLint  u_seed;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxLeafProg;

/*
 * The snow program (gowl-fx-snow.c).
 *
 * Built on first use like the rest.  The largest of them: a settled
 * flake is a crystal, a bead and a run in one lifecycle, so it carries
 * both the scattering terms and the whole of the rain's refraction.
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
	GLint  u_settle_t;
	GLint  u_fall;
	GLint  u_run;
	GLint  u_frost_t;
	GLint  u_flake;
	GLint  u_cell;
	GLint  u_settled;
	GLint  u_column;
	GLint  u_falling;
	GLint  u_arms;
	GLint  u_drift;
	GLint  u_flutter;
	GLint  u_spin;
	GLint  u_melt;
	GLint  u_shrink;
	GLint  u_bulge;
	GLint  u_depth;
	GLint  u_dispersion;
	GLint  u_runs;
	GLint  u_run_width;
	GLint  u_run_len;
	GLint  u_beads;
	GLint  u_frost;
	GLint  u_frost_scale;
	GLint  u_sparkle;
	GLint  u_fog;
	GLint  u_clarity;
	GLint  u_glow;
	GLint  u_specular;
	GLint  u_shine;
	GLint  u_rim;
	GLint  u_light;
	GLint  u_tint;
	GLint  u_absorb;
	GLint  u_brightness;
	GLint  u_alpha;
	GLint  u_seed;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxSnowProg;

/*
 * The soap-film program (gowl-fx-soap.c).
 *
 * Built on first use like the rest.  The only one whose colour comes
 * from INTERFERENCE rather than from a tint or a texture, which is why
 * it carries an index of refraction and a gain and no palette at all.
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
	GLint  u_swirl_t;
	GLint  u_thickness;
	GLint  u_thin;
	GLint  u_drain;
	GLint  u_turbulence;
	GLint  u_swirl;
	GLint  u_index;
	GLint  u_gain;
	GLint  u_sheen;
	GLint  u_wedge;
	GLint  u_dispersion;
	GLint  u_pop;
	GLint  u_meniscus;
	GLint  u_fog;
	GLint  u_clarity;
	GLint  u_light;
	GLint  u_tint;
	GLint  u_brightness;
	GLint  u_alpha;
	GLint  u_seed;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxSoapProg;

/*
 * The embers program (gowl-fx-embers.c).
 *
 * Built on first use like the rest.  The only EMISSIVE one: its subject
 * adds light rather than bending it, which is why it carries a
 * temperature and no depth.
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
	GLint  u_rise;
	GLint  u_haze_t;
	GLint  u_column;
	GLint  u_density;
	GLint  u_ember;
	GLint  u_spacing;
	GLint  u_sway;
	GLint  u_drag;
	GLint  u_temperature;
	GLint  u_cool;
	GLint  u_flicker;
	GLint  u_ash;
	GLint  u_glow;
	GLint  u_hearth;
	GLint  u_haze;
	GLint  u_haze_scale;
	GLint  u_fog;
	GLint  u_clarity;
	GLint  u_tint;
	GLint  u_brightness;
	GLint  u_alpha;
	GLint  u_seed;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxEmbersProg;

/*
 * The submerged program (gowl-fx-submerged.c).
 *
 * Built on first use like the rest.  The only one whose colour is an
 * EXTINCTION rather than a tint: three per-metre absorption coefficients
 * and an exponential, which is where all of the blue comes from.
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
	GLint  u_caustic_t;
	GLint  u_drift;
	GLint  u_surf;
	GLint  u_depth;
	GLint  u_extinction;
	GLint  u_murk;
	GLint  u_water;
	GLint  u_caustics;
	GLint  u_caustic_scale;
	GLint  u_shafts;
	GLint  u_shaft_lean;
	GLint  u_motes;
	GLint  u_mote_size;
	GLint  u_surface;
	GLint  u_sway;
	GLint  u_fog;
	GLint  u_clarity;
	GLint  u_brightness;
	GLint  u_alpha;
	GLint  u_seed;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxSubmergedProg;

/*
 * The dew-on-a-web program (gowl-fx-dew.c).
 *
 * Built on first use like the rest.  Its optics are the rain's -- a
 * drop is a drop -- and everything else about it is WHERE the drops
 * are, which is why it carries a hub, a pitch and a spacing and no
 * density at all.
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
	GLint  u_sway_t;
	GLint  u_shimmer;
	GLint  u_radials;
	GLint  u_pitch;
	GLint  u_thread;
	GLint  u_drop;
	GLint  u_spacing;
	GLint  u_sag;
	GLint  u_depth;
	GLint  u_bulge;
	GLint  u_dispersion;
	GLint  u_silk;
	GLint  u_glint;
	GLint  u_shine;
	GLint  u_rim;
	GLint  u_sway;
	GLint  u_hub;
	GLint  u_fog;
	GLint  u_clarity;
	GLint  u_light;
	GLint  u_tint;
	GLint  u_absorb;
	GLint  u_brightness;
	GLint  u_alpha;
	GLint  u_seed;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxDewProg;

/*
 * The bokeh kernel (gowl-fx-bokeh.c).
 *
 * The blur module's other way of softening one output-sized picture of
 * the wallpaper, and the only shader here that is not drawn per window.
 */
typedef struct {
	GLuint program;
	GLint  u_tex;
	GLint  u_texel;
	GLint  u_radius;
	GLint  u_blades;
	GLint  u_rot;
	GLint  u_highlight;
	GLint  u_threshold;
	GLint  u_edge;
	GLint  u_taps;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxBokehProg;

/*
 * The PQ output encode (gowl-fx-pq.c).
 *
 * Built on first use like the rest, and its failure survivable: an HDR
 * output then shows what it showed before this existed, which is wrong
 * but is not a dead session.
 */
typedef struct {
	GLuint program;
	GLint  u_src;
	GLint  u_white;
	GLint  u_peak;
	GLint  a_pos;
	GLint  a_uv;
} GowlFxPqProg;

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
	GowlFxFizzProg       fizz;       /* likewise */
	gboolean             fizz_tried;
	GowlFxLeafProg       leaf;       /* likewise */
	gboolean             leaf_tried;
	GowlFxSnowProg       snow;       /* likewise */
	gboolean             snow_tried;
	GowlFxSoapProg       soap;       /* likewise */
	gboolean             soap_tried;
	GowlFxEmbersProg     embers;     /* likewise */
	gboolean             embers_tried;
	GowlFxSubmergedProg  submerged;  /* likewise */
	gboolean             submerged_tried;
	GowlFxDewProg        dew;        /* likewise */
	gboolean             dew_tried;
	GowlFxBokehProg      bokeh;      /* likewise */
	gboolean             bokeh_tried;
	GowlFxPqProg         pq;         /* likewise */
	gboolean             pq_tried;

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