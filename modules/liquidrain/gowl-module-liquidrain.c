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
 * Liquid rain: the wallpaper through a window somebody left out in it.
 *
 * The optics are in fx/gowl-fx-rain.c -- a field of spherical caps, each
 * refracting at n = 1.333, over a pane that is frosted everywhere they
 * are not.  This file is the part that is not optics, and it is the same
 * shape as the liquid water next door because the two share the one
 * property nothing else here has: THEY MOVE.
 *
 * THE OTHER BACKDROPS SETTLE AND THESE TWO NEVER DO.  The blur draws a
 * crop of one shared picture; the glass draws a window's own picture and
 * redraws it when the window moves.  Both are, between events, correct
 * and free.  Rain is stale the moment it is drawn, which forces three
 * things the blur and the glass never needed:
 *
 *   - A CLOCK.  Kept in the module, in doubles, and handed to the shader
 *     as fractions already wrapped into [0, 1).  gowl-fx.h's
 *     GowlFxRainClock says why fractions and not seconds; the short
 *     version is that a seconds-since-start float loses its mantissa and
 *     the drops visibly step.  It is one sky for the whole desktop, not
 *     one per window: two windows side by side are two panes in the same
 *     weather, and giving each its own clock makes them disagree at the
 *     seam.
 *
 *   - A FRAME RATE.  `rain-fps' (30 by default) is a throttle, not a
 *     target: the pane is redrawn no more often than that, however fast
 *     the output runs.  Rain reads correctly at thirty -- a drop crosses
 *     a screen in seconds, not in frames -- and it halves the cost.
 *
 *   - AN HONEST ADMISSION ABOUT POWER.  Returning TRUE from the frame
 *     hook is what keeps frames coming, and this module returns TRUE
 *     while any window it would draw is on the output.  That holds the
 *     output awake.  It stops the moment there is nothing to draw -- no
 *     translucent window on this screen, the session locked, the style
 *     changed, the module shut down -- but while there IS something, a
 *     screen showing rain is a screen that is rendering.  There is no
 *     version of an animated effect that is not.
 *
 * WHAT IT COSTS.  Per output, once per tag switch: one capture of the
 * desktop with the windows hidden, and one blur of it, exactly as the
 * blur, the glass and the water pay.  Then one fragment pass per
 * eligible window per tick, at 1/`rain-scale' of the window's size (half
 * by default, which is a quarter of the pixels).  The shader is dearer
 * per pixel than the water's -- it asks nine cells of two turned
 * lattices and three columns, where the water asks five samples of one
 * height field -- so the halving matters more here.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-liquidrain"

#include "util/gowl-backdrop-plan.h"

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-client.h"
#include "core/gowl-effects.h"
#include "config/gowl-config.h"
#include "boxed/gowl-color.h"
#include "module/gowl-module-manager.h"
#include "fx/gowl-fx.h"

#include <drm_fourcc.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include "interfaces/gowl-scene-effect.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "interfaces/gowl-client-decorator.h"

#include <math.h>
#include <string.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#include <gmodule.h>

/* Above this a window is opaque enough that no rain behind it could be
 * seen.  The same threshold the blur, the glass and the water use. */
#define GOWL_RAIN_MIN_TRANSPARENCY (0.985)

#define GOWL_TYPE_MODULE_LIQUID_RAIN (gowl_module_liquid_rain_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleLiquidRain, gowl_module_liquid_rain,
                     GOWL, MODULE_LIQUID_RAIN, GowlModule)

/**
 * GowlRainStyle:
 *
 * The look, read out of the config, in LOGICAL pixels.
 *
 * Compared with memcmp() rather than field by field, for the reason the
 * glass module gives: `reload_config' can change any of it, and a
 * comparison that forgets one field leaves windows showing the old look.
 * It matters less here than there -- this redraws every tick anyway --
 * but the frost textures are NOT redrawn every tick, and they depend on
 * two of these.
 */
typedef struct {
	gdouble cell;
	gdouble density;
	gdouble bulge;
	gdouble depth;
	gdouble runs;
	gdouble run_width;
	gdouble run_length;
	gdouble beads;
	gdouble fog;
	gdouble specular;
	gdouble shine;
	gdouble rim;
	gdouble impact;
	gdouble absorption;
	gdouble life;
	gdouble speed;
	gdouble clarity;
	gdouble opacity;
	gdouble brightness;
	gdouble light;
	gdouble tint[3];
	gint    fps;
	gint    scale;
	gint    frost;
	gint    frost_passes;
} GowlRainStyle;

/**
 * GowlRainSource:
 *
 * One output's wallpaper, sharp and frosted, as textures a shader reads.
 */
typedef struct {
	GowlMonitor   *monitor;   /* unowned */
	GowlFxTexture  sharp;
	GowlFxTexture  soft;
	guint32        tags;
	gint           width, height;
	guint64        serial;
	gint           frost;
	gint           frost_passes;
	/* When this output may next redraw its rain.  Per output, because
	 * two screens at different refresh rates should each be throttled
	 * against their own clock rather than against whichever ticked
	 * last. */
	gint64         next_draw_us;
} GowlRainSource;

#define GOWL_RAIN_DATA_KEY "gowl-liquidrain-node"

typedef struct {
	struct wlr_scene_buffer *node;
	struct wl_listener       node_destroy;
	struct wlr_swapchain    *swapchain;
	GowlBackdropPlan         plan;
	gboolean                 have;
	guint64                  serial;      /* which wallpaper capture */
	guint64                  generation;  /* which settings */
	gdouble                  radius;
} GowlRainNodes;

struct _GowlModuleLiquidRain {
	GowlModule       parent_instance;
	GWeakRef         compositor;
	GowlFxGl        *gl;
	gboolean         gl_tried;
	GList           *sources;      /* GowlRainSource* */
	gboolean         capturing;
	guint64          serial;
	GowlRainStyle    style;
	gboolean         style_known;
	guint64          generation;
	/* One sky for the whole desktop.  Two windows side by side are two
	 * panes in the same weather, and giving each its own clock makes them
	 * visibly disagree at the seam. */
	GowlFxRainClock  clock;
	gint64           last_tick_us;
};

static void rain_effect_init(GowlSceneEffectInterface *iface);
static void rain_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleLiquidRain, gowl_module_liquid_rain,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, rain_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, rain_shutdown_init))

/* ── Plumbing ────────────────────────────────────────────────────── */

static void
rain_on_node_destroy(struct wl_listener *listener, void *data)
{
	GowlRainNodes *nodes;

	(void)data;
	nodes = wl_container_of(listener, nodes, node_destroy);
	wl_list_remove(&listener->link);
	nodes->node = NULL;
}

static void
rain_set_node(GowlRainNodes *nodes, struct wlr_scene_buffer *buf)
{
	nodes->node = buf;
	if (buf == NULL)
		return;
	nodes->node_destroy.notify = rain_on_node_destroy;
	wl_signal_add(&buf->node.events.destroy, &nodes->node_destroy);
}

static void
rain_forget_node(GowlRainNodes *nodes)
{
	if (nodes == NULL || nodes->node == NULL)
		return;
	wl_list_remove(&nodes->node_destroy.link);
	nodes->node = NULL;
}

static void
rain_nodes_free(gpointer data)
{
	GowlRainNodes *nodes = data;

	if (nodes->node != NULL)
		wlr_scene_node_destroy(&nodes->node->node);
	g_clear_pointer(&nodes->swapchain, wlr_swapchain_destroy);
	g_free(nodes);
}

static GowlRainNodes *
rain_nodes(GowlClient *c, gboolean create)
{
	GowlRainNodes *nodes = g_object_get_data(G_OBJECT(c),
	                                          GOWL_RAIN_DATA_KEY);

	if (nodes == NULL && create) {
		nodes = g_new0(GowlRainNodes, 1);
		g_object_set_data_full(G_OBJECT(c), GOWL_RAIN_DATA_KEY, nodes,
		                       rain_nodes_free);
	}
	return nodes;
}

static void
rain_clear_nodes(GowlClient *c)
{
	if (g_object_get_data(G_OBJECT(c), GOWL_RAIN_DATA_KEY) != NULL)
		g_object_set_data(G_OBJECT(c), GOWL_RAIN_DATA_KEY, NULL);
}

/* Take the node down and give the buffers back.  The swapchain goes with
 * it: it is several buffers the size of the window, and the reason this
 * is called is usually that the rain has been switched off. */
static void
rain_drop_node(GowlRainNodes *nodes)
{
	if (nodes->node != NULL) {
		wlr_scene_node_destroy(&nodes->node->node);
		nodes->node = NULL;
	}
	g_clear_pointer(&nodes->swapchain, wlr_swapchain_destroy);
	nodes->have = FALSE;
	memset(&nodes->plan, 0, sizeof(nodes->plan));
}

static void
rain_ensure_gl(GowlModuleLiquidRain *mod, GowlCompositor *self)
{
	if (mod->gl != NULL || mod->gl_tried)
		return;
	if (self == NULL || self->renderer == NULL)
		return;

	mod->gl_tried = TRUE;
	mod->gl = gowl_fx_gl_new(self->renderer);
}

static void
rain_source_free(GowlModuleLiquidRain *mod, GowlRainSource *src)
{
	if (src == NULL)
		return;
	gowl_fx_texture_drop(mod->gl, &src->sharp);
	gowl_fx_texture_drop(mod->gl, &src->soft);
	g_free(src);
}

static void
rain_drop_sources(GowlModuleLiquidRain *mod)
{
	GList *l;

	for (l = mod->sources; l != NULL; l = l->next)
		rain_source_free(mod, l->data);
	g_list_free(mod->sources);
	mod->sources = NULL;
}

static GowlRainSource *
rain_source_for(GowlModuleLiquidRain *mod, GowlMonitor *m)
{
	GList *l;

	for (l = mod->sources; l != NULL; l = l->next) {
		GowlRainSource *src = l->data;

		if (src->monitor == m)
			return src;
	}
	return NULL;
}

/* ── The look, out of the config ─────────────────────────────────── */

static gboolean
rain_read_style(GowlModuleLiquidRain *mod, GowlConfig *config)
{
	GowlRainStyle  style;
	GowlColor     *tint;
	const gchar   *spec;
	gdouble        k;

	memset(&style, 0, sizeof(style));

	/*
	 * One knob over the whole preset.  It scales the three that together
	 * mean "how hard is it raining" -- how many drops there are, how many
	 * of them are running, and how fast those fall -- and nothing else.
	 *
	 * Deliberately NOT the cell size.  Scaling that with them would not
	 * give heavier rain; it would give the same rain on a smaller window,
	 * which is the mistake the water's intensity knob was careful to
	 * avoid with its wavelength.  A storm has more and faster drops than
	 * a drizzle, not bigger ones by much.
	 */
	k = gowl_config_get_rain_intensity(config);

	style.cell       = gowl_config_get_rain_cell(config);
	style.density    = MIN(0.9, gowl_config_get_rain_density(config) * k);
	style.runs       = MIN(1.0, gowl_config_get_rain_runs(config) * k);
	style.speed      = gowl_config_get_rain_speed(config) * k;

	style.bulge      = gowl_config_get_rain_bulge(config);
	style.depth      = gowl_config_get_rain_depth(config);
	style.run_width  = gowl_config_get_rain_run_width(config);
	style.run_length = gowl_config_get_rain_run_length(config);
	style.beads      = gowl_config_get_rain_beads(config);
	style.fog        = gowl_config_get_rain_fog(config);
	style.specular   = gowl_config_get_rain_specular(config);
	style.shine      = gowl_config_get_rain_shine(config);
	style.rim        = gowl_config_get_rain_rim(config);
	style.impact     = gowl_config_get_rain_impact(config);
	style.absorption = gowl_config_get_rain_absorption(config);
	style.life       = gowl_config_get_rain_life(config);
	style.clarity    = gowl_config_get_rain_clarity(config);
	style.opacity    = gowl_config_get_rain_opacity(config);
	style.brightness = gowl_config_get_rain_brightness(config);
	style.light      = gowl_config_get_rain_light(config);
	style.fps        = gowl_config_get_rain_fps(config);
	style.scale      = gowl_config_get_rain_scale(config);
	style.frost      = gowl_config_get_rain_frost(config);
	style.frost_passes = gowl_config_get_rain_frost_passes(config);

	style.tint[0] = style.tint[1] = style.tint[2] = 1.0;
	spec = gowl_config_get_rain_tint(config);
	tint = spec != NULL ? gowl_color_new_from_hex(spec) : NULL;
	if (tint != NULL) {
		style.tint[0] = tint->r;
		style.tint[1] = tint->g;
		style.tint[2] = tint->b;
		gowl_color_free(tint);
	}

	if (mod->style_known && memcmp(&style, &mod->style, sizeof(style)) == 0)
		return FALSE;

	mod->style = style;
	mod->style_known = TRUE;
	mod->generation++;
	return TRUE;
}

/* ── The wallpaper it bends ──────────────────────────────────────── */

static gboolean
rain_build_source(GowlModuleLiquidRain *mod, GowlCompositor *self,
                   GowlMonitor *m, GowlRainSource *src)
{
	GowlFxVis *vis;
	gboolean   ok;
	gint       i;

	mod->capturing = TRUE;
	vis = gowl_fx_vis_begin();

	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_TILE, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_FLOAT, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_TOP, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_FS, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_OVERLAY, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_BLOCK, FALSE);
	/* And the effect sheets, which the layer sweep cannot reach: one
	 * holds an opaque picture of the desktop WITH its windows. */
	gowl_fx_vis_hide_sheets(vis);
	for (i = 0; i < 4; i++) {
		if (self->rec_indicator[i] != NULL)
			gowl_fx_vis_set(vis, &self->rec_indicator[i]->node, FALSE);
	}
	if (m->fullscreen_bg != NULL)
		gowl_fx_vis_set(vis, &m->fullscreen_bg->node, FALSE);

	ok = gowl_fx_capture(mod->gl, self, m, &src->sharp, 1);

	gowl_fx_vis_restore(vis);
	mod->capturing = FALSE;

	if (!ok)
		return FALSE;

	if (!gowl_fx_texture_blur(mod->gl, &src->soft, &src->sharp,
	                          CLAMP(mod->style.frost, 1, 8),
	                          CLAMP(mod->style.frost_passes, 1, 6)))
		return FALSE;

	src->tags         = m->tagset[m->seltags];
	src->width        = src->sharp.width;
	src->height       = src->sharp.height;
	src->frost        = mod->style.frost;
	src->frost_passes = mod->style.frost_passes;
	src->serial       = ++mod->serial;
	return TRUE;
}

static GowlRainSource *
rain_ensure_source(GowlModuleLiquidRain *mod, GowlCompositor *self,
                    GowlMonitor *m)
{
	GowlRainSource *src = rain_source_for(mod, m);
	gboolean         stale;

	if (m->wlr_output == NULL)
		return NULL;

	if (src == NULL) {
		src = g_new0(GowlRainSource, 1);
		src->monitor = m;
		mod->sources = g_list_prepend(mod->sources, src);
	}

	/*
	 * The wallpaper, unlike the surface over it, does NOT change every
	 * tick.  Rebuilding it per frame would be a full scene capture and a
	 * blur at thirty hertz, which is the difference between an effect
	 * that costs a millisecond and one that costs the machine.
	 */
	stale = src->sharp.tex == 0
	        || src->soft.tex == 0
	        || src->tags != m->tagset[m->seltags]
	        || src->width != m->wlr_output->width
	        || src->height != m->wlr_output->height
	        || src->frost != mod->style.frost
	        || src->frost_passes != mod->style.frost_passes;

	if (stale && !rain_build_source(mod, self, m, src))
		return NULL;
	return src->sharp.tex != 0 && src->soft.tex != 0 ? src : NULL;
}

/* ── Per-client rendering ────────────────────────────────────────── */

static gboolean
rain_client_eligible(GowlClient *c)
{
	return c != NULL && c->scene != NULL && !c->isfullscreen
	       && !gowl_fx_client_is_pinned(c);
}

/* Whether this window should have rain behind it at all. */
static gboolean
rain_client_wants(GowlCompositor *self, GowlClient *c)
{
	return rain_client_eligible(c)
	       && gowl_config_get_backdrop_style(self->config) == GOWL_BACKDROP_RAIN
	       && !(c->rule_flags & GOWL_CLIENT_RULE_NO_BLUR)
	       && c->alpha < GOWL_RAIN_MIN_TRANSPARENCY
	       && c->mon != NULL;
}

static struct wlr_box
rain_drawn_frame(GowlClient *c)
{
	if (c->frame.width > 0 && c->frame.height > 0)
		return c->frame;
	return c->geom;
}

/*
 * The rounded rect the window is actually drawn as, so the backdrop ends
 * exactly where the window does.
 *
 * Asks the decorator for its radius and then applies the SAME clamp and
 * border arithmetic it does (gowl_backdrop_corner_radius).  Taking the
 * decorator's number raw leaves a transparent nick inside each corner on
 * a bordered window.  No decorator means square corners.
 */
static gdouble
rain_corner_radius(GowlCompositor *self, const struct wlr_box *frame,
                     guint border_width)
{
	gpointer dec;

	if (self->module_mgr == NULL)
		return 0.0;
	dec = gowl_module_manager_get_decorator(self->module_mgr);
	if (dec == NULL)
		return 0.0;
	return gowl_backdrop_corner_radius(
		gowl_client_decorator_get_corner_radius((GowlClientDecorator *)dec),
		(gint)border_width, frame->width, frame->height);
}

static struct wlr_buffer *
rain_acquire_buffer(GowlCompositor *self, GowlRainNodes *nodes,
                     gint width, gint height)
{
	if (self->allocator == NULL || width <= 0 || height <= 0)
		return NULL;

	if (nodes->swapchain != NULL
	    && (nodes->plan.buf_width != width
	        || nodes->plan.buf_height != height))
		g_clear_pointer(&nodes->swapchain, wlr_swapchain_destroy);

	if (nodes->swapchain == NULL) {
		struct wlr_drm_format format;
		uint64_t              modifier = DRM_FORMAT_MOD_INVALID;

		memset(&format, 0, sizeof(format));
		format.format    = DRM_FORMAT_ARGB8888;
		format.len       = 1;
		format.capacity  = 1;
		format.modifiers = &modifier;

		nodes->swapchain = wlr_swapchain_create(self->allocator,
		                                        width, height, &format);
		if (nodes->swapchain == NULL)
			return NULL;
	}

	return wlr_swapchain_acquire(nodes->swapchain);
}

static void
rain_fill_params(const GowlRainStyle *style, const GowlBackdropPlan *plan,
                 gdouble radius, GowlFxRainParams *out)
{
	gdouble scale = (plan->scale_x + plan->scale_y) * 0.5;
	gdouble rad   = style->light * G_PI / 180.0;
	gdouble lx, ly;

	gowl_fx_rain_params_init(out);
	out->width      = plan->buf_width;
	out->height     = plan->buf_height;
	out->radius     = (gfloat)(radius * scale);
	/*
	 * Lengths scale with the render; weights and exponents do not.
	 *
	 * `cell' has to be in that first group and it is easy to miss why:
	 * the drop size is a fraction of it, so leaving it in logical pixels
	 * while the buffer is half size would halve every drop the moment the
	 * effect was rendered at `rain-scale' 2 -- which is always.
	 */
	out->cell       = (gfloat)(style->cell * scale);
	out->run_width  = (gfloat)(style->run_width * scale);
	out->run_len    = (gfloat)(style->run_length * scale);
	/* NOT scaled: `depth' is measured in a drop's own radii, so it is
	 * already a ratio, and multiplying it by the render scale would make
	 * a half-size render refract half as much. */
	out->depth      = (gfloat)style->depth;
	out->density    = (gfloat)style->density;
	out->bulge      = (gfloat)style->bulge;
	out->runs       = (gfloat)style->runs;
	out->beads      = (gfloat)style->beads;
	out->fog        = (gfloat)style->fog;
	out->specular   = (gfloat)style->specular;
	out->shine      = (gfloat)style->shine;
	out->rim        = (gfloat)style->rim;
	out->impact     = (gfloat)style->impact;
	out->absorption = (gfloat)style->absorption;
	out->clarity    = (gfloat)style->clarity;
	out->brightness = (gfloat)style->brightness;
	out->alpha      = (gfloat)style->opacity;
	out->dispersion = (gfloat)(0.7 * scale);
	out->tint[0]    = (gfloat)style->tint[0];
	out->tint[1]    = (gfloat)style->tint[1];
	out->tint[2]    = (gfloat)style->tint[2];

	/* 0 degrees is straight above, positive turns clockwise, and the
	 * light sits WELL off the pane: one near its plane puts the glint on
	 * the rim of every drop at once, which reads as an outline. */
	lx = sin(rad) * 0.7;
	ly = -cos(rad) * 0.7;
	out->light[0] = (gfloat)lx;
	out->light[1] = (gfloat)ly;
	out->light[2] = (gfloat)sqrt(MAX(0.0, 1.0 - lx * lx - ly * ly));

	out->src_origin[0] = (gfloat)plan->origin_x;
	out->src_origin[1] = (gfloat)plan->origin_y;
	/* The rain renders at `rain-scale' of the window by default, so this
	 * is nearly always 2 -- and without it the drops would magnify a
	 * quarter of the wallpaper rather than the piece behind them. */
	out->src_scale = (gfloat)plan->src_scale;
}

/*
 * Draw one window's rain.
 *
 * Returns TRUE when there is rain on this window, whether or not this
 * call managed to redraw it: the caller uses that to decide whether the
 * output still needs frames, and a render that failed is a reason to try
 * again next tick rather than to stop.
 */
static gboolean
rain_update_client(GowlModuleLiquidRain *mod, GowlCompositor *self,
                    GowlClient *c, gboolean redraw)
{
	GowlRainNodes    *nodes;
	GowlRainSource   *src;
	GowlBackdropPlan   plan;
	struct wlr_box     frame;
	gdouble            radius;

	rain_ensure_gl(mod, self);
	if (mod->gl == NULL || self->config == NULL || self->locked)
		return FALSE;

	rain_read_style(mod, self->config);

	if (!rain_client_eligible(c)) {
		rain_clear_nodes(c);
		return FALSE;
	}

	if (!rain_client_wants(self, c)) {
		nodes = rain_nodes(c, FALSE);
		if (nodes != NULL)
			rain_drop_node(nodes);
		return FALSE;
	}

	src = rain_ensure_source(mod, self, c->mon);
	if (src == NULL)
		return TRUE;   /* wanted, just not drawable this instant */

	frame = rain_drawn_frame(c);
	if (!gowl_backdrop_plan(&frame, &c->mon->m, src->width, src->height,
	                        CLAMP(mod->style.scale, 1, 4), &plan)) {
		nodes = rain_nodes(c, FALSE);
		if (nodes != NULL && nodes->node != NULL)
			wlr_scene_node_set_enabled(&nodes->node->node, FALSE);
		return FALSE;
	}

	nodes  = rain_nodes(c, TRUE);
	radius = rain_corner_radius(self, &frame, c->bw);

	/*
	 * There is no staleness question about the DROPS -- the clock has
	 * moved, so they are stale -- only about whether this tick is one of the
	 * ones that redraws.  Everything else that would force a redraw (a
	 * resize, a new capture, a settings change) is folded in here so a
	 * throttled tick still catches them.
	 */
	if (redraw
	    || nodes->radius != radius
	    || gowl_backdrop_render_stale(nodes->have && nodes->node != NULL,
	                                  &nodes->plan, &plan,
	                                  nodes->serial, src->serial,
	                                  nodes->generation, mod->generation)) {
		struct wlr_buffer *buf;
		GowlFxPass        *pass;
		GowlFxRainParams  params;
		gfloat             clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		gboolean           ok;

		buf = rain_acquire_buffer(self, nodes, plan.buf_width,
		                           plan.buf_height);
		if (buf == NULL)
			return TRUE;

		pass = gowl_fx_pass_begin(mod->gl, buf);
		if (pass == NULL) {
			wlr_buffer_unlock(buf);
			return TRUE;
		}
		rain_fill_params(&mod->style, &plan, radius, &params);
		gowl_fx_pass_clear(pass, clear);
		ok = gowl_fx_pass_rain(pass, &src->soft, &src->sharp, &params,
		                        &mod->clock);
		gowl_fx_pass_end(pass);

		if (!ok) {
			wlr_buffer_unlock(buf);
			/* The shader would not build.  Nothing will come of trying
			 * again, so let the output go back to sleep. */
			return FALSE;
		}

		if (nodes->node == NULL)
			rain_set_node(nodes, wlr_scene_buffer_create(c->scene, buf));
		else
			wlr_scene_buffer_set_buffer(nodes->node, buf);
		wlr_buffer_unlock(buf);

		if (nodes->node == NULL)
			return TRUE;

		nodes->plan       = plan;
		nodes->have       = TRUE;
		nodes->serial     = src->serial;
		nodes->generation = mod->generation;
		nodes->radius     = radius;
	}

	if (nodes->node == NULL)
		return TRUE;

	wlr_scene_node_set_enabled(&nodes->node->node, TRUE);
	wlr_scene_buffer_set_source_box(nodes->node, &plan.src);
	wlr_scene_buffer_set_dest_size(nodes->node, plan.vis.width,
	                               plan.vis.height);
	wlr_scene_node_set_position(&nodes->node->node,
	                            plan.vis.x - frame.x, plan.vis.y - frame.y);
	wlr_scene_node_lower_to_bottom(&nodes->node->node);
	return TRUE;
}

/* ── Hooks ───────────────────────────────────────────────────────── */

/*
 * The clock and the redraw.
 *
 * Returning TRUE is what asks for another frame on this output, and it is
 * the only reason the rain keeps moving -- an output with nothing else
 * changing on it stops redrawing, which would freeze the surface.  So it
 * is returned exactly while there is rain to draw, and not one tick
 * longer: a screen with no translucent window on it goes back to sleep.
 */
static gboolean
rain_frame(GowlSceneEffect *effect, GowlCompositor *self, GowlMonitor *m,
            gint64 now_us)
{
	GowlModuleLiquidRain *mod = GOWL_MODULE_LIQUID_RAIN(effect);
	GowlRainSource       *src;
	GList                 *l;
	gboolean               redraw, live = FALSE;

	g_weak_ref_set(&mod->compositor, self);

	if (self == NULL || self->config == NULL || self->locked || mod->capturing)
		return FALSE;
	if (gowl_config_get_backdrop_style(self->config) != GOWL_BACKDROP_RAIN)
		return FALSE;

	rain_ensure_gl(mod, self);
	if (mod->gl == NULL)
		return FALSE;
	rain_read_style(mod, self->config);

	/*
	 * The clock advances against the WALL, not once per output: with two
	 * monitors this hook runs twice a frame, and advancing it each time
	 * would run the rain at double speed on a two-screen desk.  Tracking
	 * the last instant instead makes the second call's step nearly zero,
	 * which is correct -- no time has passed.
	 */
	if (mod->last_tick_us != 0 && now_us > mod->last_tick_us) {
		gowl_fx_rain_advance(&mod->clock,
		                     (gdouble)(now_us - mod->last_tick_us) / 1e6,
		                     mod->style.speed, mod->style.life);
	}
	if (now_us > mod->last_tick_us)
		mod->last_tick_us = now_us;

	/* Throttled per output: two screens at different refresh rates should
	 * each be held against their own clock. */
	src = rain_source_for(mod, m);
	redraw = TRUE;
	if (src != NULL && mod->style.fps > 0 && now_us < src->next_draw_us)
		redraw = FALSE;

	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if (c->mon != m)
			continue;
		if (rain_update_client(mod, self, c, redraw))
			live = TRUE;
	}

	/* Set AFTER the loop: rain_update_client() may have built the source
	 * on its first pass, and the throttle belongs to the source. */
	if (redraw) {
		src = rain_source_for(mod, m);
		if (src != NULL) {
			src->next_draw_us = mod->style.fps > 0
				? now_us + G_USEC_PER_SEC / mod->style.fps
				: now_us;
		}
	}
	return live;
}

/*
 * Claims nothing, and has no use for GEOMETRY: the animation module claims
 * that for every tile.  Where a window is drawn arrives through
 * client_placed, which every provider gets.
 */
static gboolean
rain_client_event(GowlSceneEffect *effect, GowlCompositor *self,
                   GowlClient *c, GowlSceneEffectEvent event,
                   const struct wlr_box *previous, gboolean interactive)
{
	GowlModuleLiquidRain *mod = GOWL_MODULE_LIQUID_RAIN(effect);

	g_weak_ref_set(&mod->compositor, self);

	if (c == NULL || mod->capturing)
		return FALSE;

	switch (event) {
	case GOWL_SCENE_EFFECT_UNMAP:
		if (g_object_get_data(G_OBJECT(c), GOWL_RAIN_DATA_KEY) != NULL) {
			rain_forget_node(rain_nodes(c, FALSE));
			rain_clear_nodes(c);
		}
		break;
	case GOWL_SCENE_EFFECT_DESTROY:
		rain_clear_nodes(c);
		break;
	case GOWL_SCENE_EFFECT_REVEAL:
	case GOWL_SCENE_EFFECT_RELEASE:
		rain_update_client(mod, self, c, TRUE);
		break;
	default:
		break;
	}
	return FALSE;
}

/*
 * Drawing here is not just for tidiness: it is the BOOTSTRAP.  An output
 * with nothing changing on it has stopped scheduling frames, so the frame
 * hook is not running and nothing would ever start it.  Drawing the rain
 * damages a scene node, which is what makes the output ask for a frame,
 * which is what gets the clock going.  Without this a window that became
 * translucent on a still screen would sit there with no rain until
 * something else happened to wake the output.
 */
static void
rain_client_placed(GowlSceneEffect *effect, GowlCompositor *self,
                    GowlClient *c, gboolean settled)
{
	GowlModuleLiquidRain *mod = GOWL_MODULE_LIQUID_RAIN(effect);

	g_weak_ref_set(&mod->compositor, self);

	if (c == NULL || mod->capturing)
		return;
	rain_update_client(mod, self, c, TRUE);
}

static void
rain_alpha_changed(GowlSceneEffect *effect, GowlClient *c, gfloat alpha)
{
	GowlModuleLiquidRain *mod = GOWL_MODULE_LIQUID_RAIN(effect);
	GowlCompositor        *self = c != NULL ? c->compositor : NULL;

	if (self != NULL)
		rain_update_client(mod, self, c, TRUE);
}

static void
rain_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                      GowlMonitor *m)
{
	GowlModuleLiquidRain *mod = GOWL_MODULE_LIQUID_RAIN(effect);
	GowlRainSource       *src = rain_source_for(mod, m);

	if (src != NULL) {
		mod->sources = g_list_remove(mod->sources, src);
		rain_source_free(mod, src);
	}
}

static void
rain_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	GowlModuleLiquidRain *mod = GOWL_MODULE_LIQUID_RAIN(effect);
	GList *l;

	if (self != NULL) {
		for (l = self->clients; l != NULL; l = l->next)
			rain_clear_nodes(l->data);
	}
	rain_drop_sources(mod);
	g_clear_pointer(&mod->gl, gowl_fx_gl_free);
	mod->gl_tried = FALSE;
	/* The clock is wall time, and the wall kept going while the renderer
	 * was away.  Forget where it was rather than hand the next advance a
	 * gap of however long the GPU reset took. */
	mod->last_tick_us = 0;
	g_weak_ref_set(&mod->compositor, NULL);
}

static void
rain_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_event    = rain_client_event;
	iface->alpha_changed   = rain_alpha_changed;
	iface->monitor_removed = rain_monitor_removed;
	iface->finish          = rain_finish;
	iface->client_placed   = rain_client_placed;
	iface->frame           = rain_frame;
}

static void
rain_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	rain_finish(GOWL_SCENE_EFFECT(handler), compositor);
}

static void
rain_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = rain_shutdown;
}

/* ── Module ──────────────────────────────────────────────────────── */

/* The same priority as the blur and the glass, and after the animation
 * module for the hooks all of them hear. */
#define GOWL_RAIN_PRIORITY (10)

static gboolean
rain_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_RAIN_PRIORITY);
	return TRUE;
}

static void
rain_deactivate(GowlModule *base)
{
	GowlModuleLiquidRain *mod = GOWL_MODULE_LIQUID_RAIN(base);
	GowlCompositor        *self = g_weak_ref_get(&mod->compositor);

	if (self != NULL) {
		rain_finish(GOWL_SCENE_EFFECT(mod), self);
		g_object_unref(self);
	} else {
		rain_drop_sources(mod);
		g_clear_pointer(&mod->gl, gowl_fx_gl_free);
		mod->gl_tried = FALSE;
	}
}

static const gchar *rain_name(GowlModule *m)    { return "liquidrain"; }
static const gchar *rain_version(GowlModule *m) { return "0.1.0"; }
static const gchar *rain_description(GowlModule *m)
{
	return "Shows the wallpaper through a rained-on window";
}

static void
rain_finalize(GObject *object)
{
	GowlModuleLiquidRain *mod = GOWL_MODULE_LIQUID_RAIN(object);

	rain_deactivate(GOWL_MODULE(object));
	g_weak_ref_clear(&mod->compositor);
	G_OBJECT_CLASS(gowl_module_liquid_rain_parent_class)->finalize(object);
}

static void
gowl_module_liquid_rain_class_init(GowlModuleLiquidRainClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = rain_activate;
	mod->deactivate      = rain_deactivate;
	mod->get_name        = rain_name;
	mod->get_description = rain_description;
	mod->get_version     = rain_version;
	G_OBJECT_CLASS(klass)->finalize = rain_finalize;
}

static void
gowl_module_liquid_rain_init(GowlModuleLiquidRain *mod)
{
	g_weak_ref_init(&mod->compositor, NULL);
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_RAIN_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_LIQUID_RAIN;
}
