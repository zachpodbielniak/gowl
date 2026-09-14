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
 * Carbonation: the wallpaper through a glass of something fizzy.
 *
 * The optics are in fx/gowl-fx-fizz.c -- trains of gas bubbles rising
 * from nucleation sites, each a DIVERGING lens with a silvered outer
 * quarter, which is the exact inverse of the rain's drops.
 *
 * THE SAME SHAPE AS THE LIQUID RAIN NEXT DOOR, and for the same reason:
 * everything here that is not optics is the host half of an ANIMATED
 * backdrop, and that problem does not change with what is being drawn.
 *
 * THE OTHER BACKDROPS SETTLE AND THE ANIMATED ONES NEVER DO.  The blur
 * draws a crop of one shared picture; the glass draws a window's own
 * picture and redraws it when the window moves.  Both are, between
 * events, correct and free.  This is stale the moment it is drawn, which
 * forces three things they never needed:
 *
 *   - A CLOCK.  Kept in the module, in doubles, and handed to the shader
 *     as cycle counts already wrapped.  gowl-fx.h says why cycles and
 *     not seconds; the short version is that a seconds-since-start float
 *     loses its mantissa and the motion visibly steps.  It is ONE clock
 *     for the whole desktop rather than one per window: two windows side
 *     by side are two panes in the same weather, and giving each its own
 *     makes them disagree at the seam.
 *
 *   - A FRAME RATE.  `fizz-fps' (30 by default) is a throttle, not a
 *     target: the pane is redrawn no more often than that, however fast
 *     the output runs.  Bubbles read correctly at thirty -- one
 *     crosses the glass in seconds, not in frames -- and it halves
 *     the cost.
 *
 *   - AN HONEST ADMISSION ABOUT POWER.  Returning TRUE from the frame
 *     hook is what keeps frames coming, and this module returns TRUE
 *     while any window it would draw is on the output.  That holds the
 *     output awake.  It stops the moment there is nothing to draw -- no
 *     translucent window on this screen, the session locked, the style
 *     changed, the module shut down -- but while there IS something, a
 *     screen showing this is a screen that is rendering.  There is no
 *     version of an animated effect that is not.
 *
 * WHAT IT COSTS.  Per output, once per tag switch: one capture of the
 * desktop with the windows hidden, and one blur of it, exactly as the
 * blur, the glass, the water and the rain all pay.  Then one fragment
 * pass per eligible window per tick, at 1/`fizz-scale' of the
 * window's size (half by default, which is a quarter of the pixels).
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fizz"

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

/* Above this a window is opaque enough that nothing behind it could be
 * seen.  The same threshold every other backdrop uses. */
#define GOWL_FIZZ_MIN_TRANSPARENCY (0.985)

#define GOWL_TYPE_MODULE_FIZZ (gowl_module_fizz_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleFizz, gowl_module_fizz,
                     GOWL, MODULE_FIZZ, GowlModule)

/**
 * GowlFizzStyle:
 *
 * The look, read out of the config, in LOGICAL pixels.
 *
 * Compared with memcmp() rather than field by field, for the reason the
 * glass module gives: `reload_config' can change any of it, and a
 * comparison that forgets one field leaves windows showing the old look.
 * It matters less here than there -- this redraws every tick anyway --
 * but the blurred source is NOT redrawn every tick, and it depends on
 * two of these.
 */
typedef struct {
	gdouble cell;
	gdouble bubble;
	gdouble growth;
	gdouble sites;
	gdouble site_width;
	gdouble spacing;
	gdouble stray;
	gdouble cling;
	gdouble wobble;
	gdouble foam;
	gdouble foam_depth;
	gdouble depth;
	gdouble mirror;
	gdouble fog;
	gdouble specular;
	gdouble shine;
	gdouble rim;
	gdouble absorption;
	gdouble cling_life;
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
} GowlFizzStyle;

/**
 * GowlFizzSource:
 *
 * One output's wallpaper, sharp and blurred, as textures a shader reads.
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
	/* When this output may next redraw.  Per output, because two screens
	 * at different refresh rates should each be throttled against their
	 * own clock rather than against whichever ticked last. */
	gint64         next_draw_us;
} GowlFizzSource;

#define GOWL_FIZZ_DATA_KEY "gowl-fizz-node"

typedef struct {
	struct wlr_scene_buffer *node;
	struct wl_listener       node_destroy;
	struct wlr_swapchain    *swapchain;
	GowlBackdropPlan         plan;
	gboolean                 have;
	guint64                  serial;      /* which wallpaper capture */
	guint64                  generation;  /* which settings */
	gdouble                  radius;
} GowlFizzNodes;

struct _GowlModuleFizz {
	GowlModule       parent_instance;
	GWeakRef         compositor;
	GowlFxGl        *gl;
	gboolean         gl_tried;
	GList           *sources;      /* GowlFizzSource* */
	gboolean         capturing;
	guint64          serial;
	GowlFizzStyle style;
	gboolean         style_known;
	guint64          generation;
	/* One clock for the whole desktop.  Two windows side by side are two
	 * panes in the same weather, and giving each its own makes them
	 * visibly disagree at the seam. */
	GowlFxFizzClock  clock;
	gint64           last_tick_us;
};

static void fizz_effect_init(GowlSceneEffectInterface *iface);
static void fizz_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleFizz, gowl_module_fizz,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, fizz_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, fizz_shutdown_init))

/* ── Plumbing ────────────────────────────────────────────────────── */

static void
fizz_on_node_destroy(struct wl_listener *listener, void *data)
{
	GowlFizzNodes *nodes;

	(void)data;
	nodes = wl_container_of(listener, nodes, node_destroy);
	wl_list_remove(&listener->link);
	nodes->node = NULL;
}

static void
fizz_set_node(GowlFizzNodes *nodes, struct wlr_scene_buffer *buf)
{
	nodes->node = buf;
	if (buf == NULL)
		return;
	nodes->node_destroy.notify = fizz_on_node_destroy;
	wl_signal_add(&buf->node.events.destroy, &nodes->node_destroy);
}

static void
fizz_forget_node(GowlFizzNodes *nodes)
{
	if (nodes == NULL || nodes->node == NULL)
		return;
	wl_list_remove(&nodes->node_destroy.link);
	nodes->node = NULL;
}

static void
fizz_nodes_free(gpointer data)
{
	GowlFizzNodes *nodes = data;

	if (nodes->node != NULL)
		wlr_scene_node_destroy(&nodes->node->node);
	g_clear_pointer(&nodes->swapchain, wlr_swapchain_destroy);
	g_free(nodes);
}

static GowlFizzNodes *
fizz_nodes(GowlClient *c, gboolean create)
{
	GowlFizzNodes *nodes = g_object_get_data(G_OBJECT(c),
	                                              GOWL_FIZZ_DATA_KEY);

	if (nodes == NULL && create) {
		nodes = g_new0(GowlFizzNodes, 1);
		g_object_set_data_full(G_OBJECT(c), GOWL_FIZZ_DATA_KEY, nodes,
		                       fizz_nodes_free);
	}
	return nodes;
}

static void
fizz_clear_nodes(GowlClient *c)
{
	if (g_object_get_data(G_OBJECT(c), GOWL_FIZZ_DATA_KEY) != NULL)
		g_object_set_data(G_OBJECT(c), GOWL_FIZZ_DATA_KEY, NULL);
}

/* Take the node down and give the buffers back.  The swapchain goes with
 * it: it is several buffers the size of the window, and the reason this
 * is called is usually that the effect has been switched off. */
static void
fizz_drop_node(GowlFizzNodes *nodes)
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
fizz_ensure_gl(GowlModuleFizz *mod, GowlCompositor *self)
{
	if (mod->gl != NULL || mod->gl_tried)
		return;
	if (self == NULL || self->renderer == NULL)
		return;

	mod->gl_tried = TRUE;
	mod->gl = gowl_fx_gl_new(self->renderer);
}

static void
fizz_source_free(GowlModuleFizz *mod, GowlFizzSource *src)
{
	if (src == NULL)
		return;
	gowl_fx_texture_drop(mod->gl, &src->sharp);
	gowl_fx_texture_drop(mod->gl, &src->soft);
	g_free(src);
}

static void
fizz_drop_sources(GowlModuleFizz *mod)
{
	GList *l;

	for (l = mod->sources; l != NULL; l = l->next)
		fizz_source_free(mod, l->data);
	g_list_free(mod->sources);
	mod->sources = NULL;
}

static GowlFizzSource *
fizz_source_for(GowlModuleFizz *mod, GowlMonitor *m)
{
	GList *l;

	for (l = mod->sources; l != NULL; l = l->next) {
		GowlFizzSource *src = l->data;

		if (src->monitor == m)
			return src;
	}
	return NULL;
}

/* ── The look, out of the config ─────────────────────────────────── */

static gboolean
fizz_read_style(GowlModuleFizz *mod, GowlConfig *config)
{
	GowlFizzStyle  style;
	GowlColor          *tint;
	const gchar        *spec;
	gdouble             k;

	memset(&style, 0, sizeof(style));

	/*
	 * One knob over the whole preset.  It scales the four that together
	 * mean "how carbonated is this" -- how many sites there are, how
	 * closely they emit, how many loose bubbles there are between them,
	 * and how fast they rise -- and nothing else.
	 *
	 * Deliberately NOT the bubble size or the column width.  Scaling
	 * those with them would not give a fizzier drink; it would give the
	 * same drink in a smaller glass, which is the mistake the water's
	 * intensity knob was careful to avoid with its wavelength.  Champagne
	 * has MORE and FINER bubbles than cola, and the preset table is where
	 * "finer" lives.
	 */
	k = gowl_config_get_fizz_intensity(config);

	style.sites      = MIN(1.0, gowl_config_get_fizz_sites(config) * k);
	style.spacing    = MIN(1.0, gowl_config_get_fizz_spacing(config) * k);
	style.stray      = MIN(1.0, gowl_config_get_fizz_stray(config) * k);
	style.speed      = gowl_config_get_fizz_speed(config) * k;

	style.cell       = gowl_config_get_fizz_cell(config);
	style.bubble     = gowl_config_get_fizz_bubble(config);
	style.growth     = gowl_config_get_fizz_growth(config);
	style.site_width = gowl_config_get_fizz_site_width(config);
	style.cling      = gowl_config_get_fizz_cling(config);
	style.wobble     = gowl_config_get_fizz_wobble(config);
	style.foam       = gowl_config_get_fizz_foam(config);
	style.foam_depth = gowl_config_get_fizz_foam_depth(config);
	style.depth      = gowl_config_get_fizz_depth(config);
	style.mirror     = gowl_config_get_fizz_mirror(config);
	style.fog        = gowl_config_get_fizz_fog(config);
	style.specular   = gowl_config_get_fizz_specular(config);
	style.shine      = gowl_config_get_fizz_shine(config);
	style.rim        = gowl_config_get_fizz_rim(config);
	style.absorption = gowl_config_get_fizz_absorption(config);
	style.cling_life = gowl_config_get_fizz_cling_life(config);
	style.clarity    = gowl_config_get_fizz_clarity(config);
	style.opacity    = gowl_config_get_fizz_opacity(config);
	style.brightness = gowl_config_get_fizz_brightness(config);
	style.light      = gowl_config_get_fizz_light(config);
	style.fps        = gowl_config_get_fizz_fps(config);
	style.scale      = gowl_config_get_fizz_scale(config);
	style.frost      = gowl_config_get_fizz_frost(config);
	style.frost_passes = gowl_config_get_fizz_frost_passes(config);

	style.tint[0] = style.tint[1] = style.tint[2] = 1.0;
	spec = gowl_config_get_fizz_tint(config);
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

/* ── The wallpaper it draws over ─────────────────────────────────── */

static gboolean
fizz_build_source(GowlModuleFizz *mod, GowlCompositor *self,
                   GowlMonitor *m, GowlFizzSource *src)
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

static GowlFizzSource *
fizz_ensure_source(GowlModuleFizz *mod, GowlCompositor *self,
                    GowlMonitor *m)
{
	GowlFizzSource *src = fizz_source_for(mod, m);
	gboolean              stale;

	if (m->wlr_output == NULL)
		return NULL;

	if (src == NULL) {
		src = g_new0(GowlFizzSource, 1);
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

	if (stale && !fizz_build_source(mod, self, m, src))
		return NULL;
	return src->sharp.tex != 0 && src->soft.tex != 0 ? src : NULL;
}

/* ── Per-client rendering ────────────────────────────────────────── */

static gboolean
fizz_client_eligible(GowlClient *c)
{
	return c != NULL && c->scene != NULL && !c->isfullscreen
	       && !gowl_fx_client_is_pinned(c);
}

/* Whether this window should have the effect behind it at all. */
static gboolean
fizz_client_wants(GowlCompositor *self, GowlClient *c)
{
	return fizz_client_eligible(c)
	       && gowl_config_get_backdrop_style(self->config) == GOWL_BACKDROP_FIZZ
	       && !(c->rule_flags & GOWL_CLIENT_RULE_NO_BLUR)
	       && c->alpha < GOWL_FIZZ_MIN_TRANSPARENCY
	       && c->mon != NULL;
}

static struct wlr_box
fizz_drawn_frame(GowlClient *c)
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
fizz_corner_radius(GowlCompositor *self, const struct wlr_box *frame,
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
fizz_acquire_buffer(GowlCompositor *self, GowlFizzNodes *nodes,
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
fizz_fill_params(const GowlFizzStyle *style,
                  const GowlBackdropPlan *plan,
                  gdouble radius, guint seed, GowlFxFizzParams *out)
{
	gdouble scale = (plan->scale_x + plan->scale_y) * 0.5;
	gdouble rad   = style->light * G_PI / 180.0;
	gdouble lx, ly;

	gowl_fx_fizz_params_init(out);
	out->width      = plan->buf_width;
	out->height     = plan->buf_height;
	out->radius     = (gfloat)(radius * scale);
	/*
	 * Lengths scale with the render; fractions, weights and exponents do
	 * not.
	 *
	 * `bubble' is in the second group and it is easy to get wrong: it is
	 * already a FRACTION of the column, so scaling it as well would
	 * shrink every bubble the moment the effect was rendered at
	 * `fizz-scale' 2 -- which is always.  `cell', `site_width',
	 * `foam_depth' and `wobble' ARE lengths and do scale.
	 */
	out->cell       = (gfloat)(style->cell * scale);
	out->site_width = (gfloat)(style->site_width * scale);
	out->foam_depth = (gfloat)(style->foam_depth * scale);
	out->wobble     = (gfloat)(style->wobble * scale);
	out->bubble     = (gfloat)style->bubble;
	/* NOT scaled: `depth' is measured in a bubble's own radii, so it is
	 * already a ratio, and multiplying it by the render scale would make
	 * a half-size render refract half as much. */
	out->depth      = (gfloat)style->depth;
	out->growth     = (gfloat)style->growth;
	out->sites      = (gfloat)style->sites;
	out->spacing    = (gfloat)style->spacing;
	out->stray      = (gfloat)style->stray;
	out->cling      = (gfloat)style->cling;
	out->foam       = (gfloat)style->foam;
	out->mirror     = (gfloat)style->mirror;
	out->fog        = (gfloat)style->fog;
	out->clarity    = (gfloat)style->clarity;
	out->specular   = (gfloat)style->specular;
	out->shine      = (gfloat)style->shine;
	out->rim        = (gfloat)style->rim;
	out->absorption = (gfloat)style->absorption;
	out->brightness = (gfloat)style->brightness;
	out->alpha      = (gfloat)style->opacity;
	out->dispersion = (gfloat)(0.6 * scale);
	out->tint[0]    = (gfloat)style->tint[0];
	out->tint[1]    = (gfloat)style->tint[1];
	out->tint[2]    = (gfloat)style->tint[2];


	/* 0 degrees is straight above, positive turns clockwise, and the
	 * light sits WELL off the pane: one near its plane puts the glint on
	 * the rim of everything at once, which reads as an outline. */
	lx = sin(rad) * 0.7;
	ly = -cos(rad) * 0.7;
	out->light[0] = (gfloat)lx;
	out->light[1] = (gfloat)ly;
	out->light[2] = (gfloat)sqrt(MAX(0.0, 1.0 - lx * lx - ly * ly));

	out->src_origin[0] = (gfloat)plan->origin_x;
	out->src_origin[1] = (gfloat)plan->origin_y;
	/* Rendered at `fizz-scale' of the window by default, so this is
	 * nearly always 2 -- and without it the effect would magnify a
	 * quarter of the wallpaper rather than the piece behind it. */
	out->src_scale = (gfloat)plan->src_scale;

	/*
	 * Which crop of the field this window shows.
	 *
	 * The pattern is anchored to the WINDOW rather than to the screen
	 * behind it, so it stays put when the window moves -- which is what
	 * something ON a pane does.  The cost of that is every window
	 * starting its field at its own corner, which means every window
	 * would show the SAME thing in the same places; two terminals side by
	 * side make it obvious in a second.
	 *
	 * The client's id is stable for its lifetime and never reused, so a
	 * window keeps its own weather for as long as it is open and gets
	 * different weather next time.
	 */
	out->seed = (gfloat)(seed % 9973u) * 0.0016f;
}

/*
 * Draw one window's backdrop.
 *
 * Returns TRUE when there is something on this window, whether or not
 * this call managed to redraw it: the caller uses that to decide whether
 * the output still needs frames, and a render that failed is a reason to
 * try again next tick rather than to stop.
 */
static gboolean
fizz_update_client(GowlModuleFizz *mod, GowlCompositor *self,
                    GowlClient *c, gboolean redraw)
{
	GowlFizzNodes  *nodes;
	GowlFizzSource *src;
	GowlBackdropPlan      plan;
	struct wlr_box        frame;
	gdouble               radius;

	fizz_ensure_gl(mod, self);
	if (mod->gl == NULL || self->config == NULL || self->locked)
		return FALSE;

	fizz_read_style(mod, self->config);

	if (!fizz_client_eligible(c)) {
		fizz_clear_nodes(c);
		return FALSE;
	}

	if (!fizz_client_wants(self, c)) {
		nodes = fizz_nodes(c, FALSE);
		if (nodes != NULL)
			fizz_drop_node(nodes);
		return FALSE;
	}

	src = fizz_ensure_source(mod, self, c->mon);
	if (src == NULL)
		return TRUE;   /* wanted, just not drawable this instant */

	frame = fizz_drawn_frame(c);
	if (!gowl_backdrop_plan(&frame, &c->mon->m, src->width, src->height,
	                        CLAMP(mod->style.scale, 1, 4), &plan)) {
		nodes = fizz_nodes(c, FALSE);
		if (nodes != NULL && nodes->node != NULL)
			wlr_scene_node_set_enabled(&nodes->node->node, FALSE);
		return FALSE;
	}

	nodes  = fizz_nodes(c, TRUE);
	radius = fizz_corner_radius(self, &frame, c->bw);

	/*
	 * There is no staleness question about the MOTION -- the clock has
	 * moved, so it is stale -- only about whether this tick is one of the
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
		GowlFxFizzParams  params;
		gfloat             clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		gboolean           ok;

		buf = fizz_acquire_buffer(self, nodes, plan.buf_width,
		                           plan.buf_height);
		if (buf == NULL)
			return TRUE;

		pass = gowl_fx_pass_begin(mod->gl, buf);
		if (pass == NULL) {
			wlr_buffer_unlock(buf);
			return TRUE;
		}
		fizz_fill_params(&mod->style, &plan, radius,
		                  gowl_client_get_id(c), &params);
		gowl_fx_pass_clear(pass, clear);
		ok = gowl_fx_pass_fizz(pass, &src->soft, &src->sharp, &params,
		                 &mod->clock);
		gowl_fx_pass_end(pass);

		if (!ok) {
			wlr_buffer_unlock(buf);
			/* The shader would not build.  Nothing will come of trying
			 * again, so let the output go back to sleep. */
			return FALSE;
		}

		if (nodes->node == NULL)
			fizz_set_node(nodes, wlr_scene_buffer_create(c->scene, buf));
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
 * the only reason the effect keeps moving -- an output with nothing else
 * changing on it stops redrawing, which would freeze the surface.  So it
 * is returned exactly while there is something to draw, and not one tick
 * longer: a screen with no translucent window on it goes back to sleep.
 */
static gboolean
fizz_frame(GowlSceneEffect *effect, GowlCompositor *self, GowlMonitor *m,
            gint64 now_us)
{
	GowlModuleFizz  *mod = GOWL_MODULE_FIZZ(effect);
	GowlFizzSource  *src;
	GList                 *l;
	gboolean               redraw, live = FALSE;

	g_weak_ref_set(&mod->compositor, self);

	if (self == NULL || self->config == NULL || self->locked || mod->capturing)
		return FALSE;
	if (gowl_config_get_backdrop_style(self->config) != GOWL_BACKDROP_FIZZ)
		return FALSE;

	fizz_ensure_gl(mod, self);
	if (mod->gl == NULL)
		return FALSE;
	fizz_read_style(mod, self->config);

	/*
	 * The clock advances against the WALL, not once per output: with two
	 * monitors this hook runs twice a frame, and advancing it each time
	 * would run the effect at double speed on a two-screen desk.
	 * Tracking the last instant instead makes the second call's step
	 * nearly zero, which is correct -- no time has passed.
	 */
	if (mod->last_tick_us != 0 && now_us > mod->last_tick_us) {
		gowl_fx_fizz_advance(&mod->clock,
		               (gdouble)(now_us - mod->last_tick_us) / 1e6,
		               mod->style.speed, mod->style.cling_life);
	}
	if (now_us > mod->last_tick_us)
		mod->last_tick_us = now_us;

	/* Throttled per output: two screens at different refresh rates should
	 * each be held against their own clock. */
	src = fizz_source_for(mod, m);
	redraw = TRUE;
	if (src != NULL && mod->style.fps > 0 && now_us < src->next_draw_us)
		redraw = FALSE;

	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if (c->mon != m)
			continue;
		if (fizz_update_client(mod, self, c, redraw))
			live = TRUE;
	}

	/* Set AFTER the loop: fizz_update_client() may have built the source
	 * on its first pass, and the throttle belongs to the source. */
	if (redraw) {
		src = fizz_source_for(mod, m);
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
fizz_client_event(GowlSceneEffect *effect, GowlCompositor *self,
                   GowlClient *c, GowlSceneEffectEvent event,
                   const struct wlr_box *previous, gboolean interactive)
{
	GowlModuleFizz *mod = GOWL_MODULE_FIZZ(effect);

	g_weak_ref_set(&mod->compositor, self);

	if (c == NULL || mod->capturing)
		return FALSE;

	switch (event) {
	case GOWL_SCENE_EFFECT_UNMAP:
		if (g_object_get_data(G_OBJECT(c), GOWL_FIZZ_DATA_KEY) != NULL) {
			fizz_forget_node(fizz_nodes(c, FALSE));
			fizz_clear_nodes(c);
		}
		break;
	case GOWL_SCENE_EFFECT_DESTROY:
		fizz_clear_nodes(c);
		break;
	case GOWL_SCENE_EFFECT_REVEAL:
	case GOWL_SCENE_EFFECT_RELEASE:
		fizz_update_client(mod, self, c, TRUE);
		break;
	default:
		break;
	}
	return FALSE;
}

/*
 * Drawing here is not just for tidiness: it is the BOOTSTRAP.  An output
 * with nothing changing on it has stopped scheduling frames, so the frame
 * hook is not running and nothing would ever start it.  Drawing damages a
 * scene node, which is what makes the output ask for a frame, which is
 * what gets the clock going.  Without this a window that became
 * translucent on a still screen would sit there with nothing behind it
 * until something else happened to wake the output.
 */
static void
fizz_client_placed(GowlSceneEffect *effect, GowlCompositor *self,
                    GowlClient *c, gboolean settled)
{
	GowlModuleFizz *mod = GOWL_MODULE_FIZZ(effect);

	g_weak_ref_set(&mod->compositor, self);

	if (c == NULL || mod->capturing)
		return;
	fizz_update_client(mod, self, c, TRUE);
}

static void
fizz_alpha_changed(GowlSceneEffect *effect, GowlClient *c, gfloat alpha)
{
	GowlModuleFizz *mod = GOWL_MODULE_FIZZ(effect);
	GowlCompositor       *self = c != NULL ? c->compositor : NULL;

	if (self != NULL)
		fizz_update_client(mod, self, c, TRUE);
}

static void
fizz_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                      GowlMonitor *m)
{
	GowlModuleFizz  *mod = GOWL_MODULE_FIZZ(effect);
	GowlFizzSource  *src = fizz_source_for(mod, m);

	if (src != NULL) {
		mod->sources = g_list_remove(mod->sources, src);
		fizz_source_free(mod, src);
	}
}

static void
fizz_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	GowlModuleFizz *mod = GOWL_MODULE_FIZZ(effect);
	GList *l;

	if (self != NULL) {
		for (l = self->clients; l != NULL; l = l->next)
			fizz_clear_nodes(l->data);
	}
	fizz_drop_sources(mod);
	g_clear_pointer(&mod->gl, gowl_fx_gl_free);
	mod->gl_tried = FALSE;
	/* The clock is wall time, and the wall kept going while the renderer
	 * was away.  Forget where it was rather than hand the next advance a
	 * gap of however long the GPU reset took. */
	mod->last_tick_us = 0;
	g_weak_ref_set(&mod->compositor, NULL);
}

static void
fizz_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_event    = fizz_client_event;
	iface->alpha_changed   = fizz_alpha_changed;
	iface->monitor_removed = fizz_monitor_removed;
	iface->finish          = fizz_finish;
	iface->client_placed   = fizz_client_placed;
	iface->frame           = fizz_frame;
}

static void
fizz_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	fizz_finish(GOWL_SCENE_EFFECT(handler), compositor);
}

static void
fizz_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = fizz_shutdown;
}

/* ── Module ──────────────────────────────────────────────────────── */

/* The same priority as the blur, the glass, the water and the rain, and
 * after the animation module for the hooks all of them hear. */
#define GOWL_FIZZ_PRIORITY (10)

static gboolean
fizz_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_FIZZ_PRIORITY);
	return TRUE;
}

static void
fizz_deactivate(GowlModule *base)
{
	GowlModuleFizz *mod = GOWL_MODULE_FIZZ(base);
	GowlCompositor       *self = g_weak_ref_get(&mod->compositor);

	if (self != NULL) {
		fizz_finish(GOWL_SCENE_EFFECT(mod), self);
		g_object_unref(self);
	} else {
		fizz_drop_sources(mod);
		g_clear_pointer(&mod->gl, gowl_fx_gl_free);
		mod->gl_tried = FALSE;
	}
}

static const gchar *fizz_name(GowlModule *m)    { return "fizz"; }
static const gchar *fizz_version(GowlModule *m) { return "0.1.0"; }
static const gchar *fizz_description(GowlModule *m)
{
	return "Shows the wallpaper through a glass of something carbonated";
}

static void
fizz_finalize(GObject *object)
{
	GowlModuleFizz *mod = GOWL_MODULE_FIZZ(object);

	fizz_deactivate(GOWL_MODULE(object));
	g_weak_ref_clear(&mod->compositor);
	G_OBJECT_CLASS(gowl_module_fizz_parent_class)->finalize(object);
}

static void
gowl_module_fizz_class_init(GowlModuleFizzClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = fizz_activate;
	mod->deactivate      = fizz_deactivate;
	mod->get_name        = fizz_name;
	mod->get_description = fizz_description;
	mod->get_version     = fizz_version;
	G_OBJECT_CLASS(klass)->finalize = fizz_finalize;
}

static void
gowl_module_fizz_init(GowlModuleFizz *mod)
{
	g_weak_ref_init(&mod->compositor, NULL);
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_FIZZ_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_FIZZ;
}
