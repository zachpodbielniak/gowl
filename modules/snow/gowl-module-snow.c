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
 * Snow: the window in a snowfall, and what becomes of what lands.
 *
 * The drawing is in fx/gowl-fx-snow.c -- six-fold crystals that fall,
 * settle, round off as they melt, collapse into water beads a third
 * their size and run away down the pane, with frost creeping in from
 * the edges while it happens.
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
 *   - A FRAME RATE.  `snow-fps' (30 by default) is a throttle, not a
 *     target: the pane is redrawn no more often than that, however fast
 *     the output runs.  Snow reads correctly at thirty and is the
 *     most forgiving of the family: a flake drifts down over several
 *     seconds, and it is the DEAREST shader here, so the halving
 *     matters most.
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
 * pass per eligible window per tick, at 1/`snow-scale' of the
 * window's size (half by default, which is a quarter of the pixels).
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-snow"

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
#define GOWL_SNOW_MIN_TRANSPARENCY (0.985)

#define GOWL_TYPE_MODULE_SNOW (gowl_module_snow_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleSnow, gowl_module_snow,
                     GOWL, MODULE_SNOW, GowlModule)

/**
 * GowlSnowStyle:
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
	gdouble flake;
	gdouble cell;
	gdouble settled;
	gdouble column;
	gdouble falling;
	gdouble arms;
	gdouble drift;
	gdouble flutter;
	gdouble spin;
	gdouble melt;
	gdouble shrink;
	gdouble depth;
	gdouble runs;
	gdouble run_width;
	gdouble run_length;
	gdouble beads;
	gdouble ice;
	gdouble ice_rate;
	gdouble ice_scale;
	gdouble sparkle;
	gdouble fog;
	gdouble glow;
	gdouble specular;
	gdouble shine;
	gdouble rim;
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
} GowlSnowStyle;

/**
 * GowlSnowSource:
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
} GowlSnowSource;

#define GOWL_SNOW_DATA_KEY "gowl-snow-node"

typedef struct {
	struct wlr_scene_buffer *node;
	struct wl_listener       node_destroy;
	struct wlr_swapchain    *swapchain;
	GowlBackdropPlan         plan;
	gboolean                 have;
	guint64                  serial;      /* which wallpaper capture */
	guint64                  generation;  /* which settings */
	gdouble                  radius;
} GowlSnowNodes;

struct _GowlModuleSnow {
	GowlModule       parent_instance;
	GWeakRef         compositor;
	GowlFxGl        *gl;
	gboolean         gl_tried;
	GList           *sources;      /* GowlSnowSource* */
	gboolean         capturing;
	guint64          serial;
	GowlSnowStyle style;
	gboolean         style_known;
	guint64          generation;
	/* One clock for the whole desktop.  Two windows side by side are two
	 * panes in the same weather, and giving each its own makes them
	 * visibly disagree at the seam. */
	GowlFxSnowClock  clock;
	gint64           last_tick_us;
};

static void snow_effect_init(GowlSceneEffectInterface *iface);
static void snow_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleSnow, gowl_module_snow,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, snow_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, snow_shutdown_init))

/* ── Plumbing ────────────────────────────────────────────────────── */

static void
snow_on_node_destroy(struct wl_listener *listener, void *data)
{
	GowlSnowNodes *nodes;

	(void)data;
	nodes = wl_container_of(listener, nodes, node_destroy);
	wl_list_remove(&listener->link);
	nodes->node = NULL;
}

static void
snow_set_node(GowlSnowNodes *nodes, struct wlr_scene_buffer *buf)
{
	nodes->node = buf;
	if (buf == NULL)
		return;
	nodes->node_destroy.notify = snow_on_node_destroy;
	wl_signal_add(&buf->node.events.destroy, &nodes->node_destroy);
}

static void
snow_forget_node(GowlSnowNodes *nodes)
{
	if (nodes == NULL || nodes->node == NULL)
		return;
	wl_list_remove(&nodes->node_destroy.link);
	nodes->node = NULL;
}

static void
snow_nodes_free(gpointer data)
{
	GowlSnowNodes *nodes = data;

	if (nodes->node != NULL)
		wlr_scene_node_destroy(&nodes->node->node);
	g_clear_pointer(&nodes->swapchain, wlr_swapchain_destroy);
	g_free(nodes);
}

static GowlSnowNodes *
snow_nodes(GowlClient *c, gboolean create)
{
	GowlSnowNodes *nodes = g_object_get_data(G_OBJECT(c),
	                                              GOWL_SNOW_DATA_KEY);

	if (nodes == NULL && create) {
		nodes = g_new0(GowlSnowNodes, 1);
		g_object_set_data_full(G_OBJECT(c), GOWL_SNOW_DATA_KEY, nodes,
		                       snow_nodes_free);
	}
	return nodes;
}

static void
snow_clear_nodes(GowlClient *c)
{
	if (g_object_get_data(G_OBJECT(c), GOWL_SNOW_DATA_KEY) != NULL)
		g_object_set_data(G_OBJECT(c), GOWL_SNOW_DATA_KEY, NULL);
}

/* Take the node down and give the buffers back.  The swapchain goes with
 * it: it is several buffers the size of the window, and the reason this
 * is called is usually that the effect has been switched off. */
static void
snow_drop_node(GowlSnowNodes *nodes)
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
snow_ensure_gl(GowlModuleSnow *mod, GowlCompositor *self)
{
	if (mod->gl != NULL || mod->gl_tried)
		return;
	if (self == NULL || self->renderer == NULL)
		return;

	mod->gl_tried = TRUE;
	mod->gl = gowl_fx_gl_new(self->renderer);
}

static void
snow_source_free(GowlModuleSnow *mod, GowlSnowSource *src)
{
	if (src == NULL)
		return;
	gowl_fx_texture_drop(mod->gl, &src->sharp);
	gowl_fx_texture_drop(mod->gl, &src->soft);
	g_free(src);
}

static void
snow_drop_sources(GowlModuleSnow *mod)
{
	GList *l;

	for (l = mod->sources; l != NULL; l = l->next)
		snow_source_free(mod, l->data);
	g_list_free(mod->sources);
	mod->sources = NULL;
}

static GowlSnowSource *
snow_source_for(GowlModuleSnow *mod, GowlMonitor *m)
{
	GList *l;

	for (l = mod->sources; l != NULL; l = l->next) {
		GowlSnowSource *src = l->data;

		if (src->monitor == m)
			return src;
	}
	return NULL;
}

/* ── The look, out of the config ─────────────────────────────────── */

static gboolean
snow_read_style(GowlModuleSnow *mod, GowlConfig *config)
{
	GowlSnowStyle  style;
	GowlColor          *tint;
	const gchar        *spec;
	gdouble             k;

	memset(&style, 0, sizeof(style));

	/*
	 * One knob over the whole preset.  It scales the three that together
	 * mean "how hard is it snowing" -- how many flakes are falling, how
	 * many have settled, and how fast they come down -- and nothing else.
	 *
	 * Deliberately NOT the flake size.  Bigger crystals are not heavier
	 * snow, and raising the size here would do nothing anyway: the shader
	 * caps the flake against the column so it cannot be sliced off at the
	 * column edge, so an intensity that tried to move it would be
	 * silently ignored above about 1.2.
	 *
	 * And deliberately NOT the melt or the frost.  Those are the pane's
	 * TEMPERATURE, which is what the preset table varies alongside the
	 * fall rate; scaling them with the intensity would mean that turning
	 * the snow up also froze the window, which is a different setting
	 * wearing this one's clothes.
	 */
	k = gowl_config_get_snow_intensity(config);

	style.falling  = MIN(1.0, gowl_config_get_snow_falling(config) * k);
	style.settled  = MIN(1.0, gowl_config_get_snow_settled(config) * k);
	style.speed    = gowl_config_get_snow_speed(config) * k;

	style.flake      = gowl_config_get_snow_flake(config);
	style.cell       = gowl_config_get_snow_cell(config);
	style.column     = gowl_config_get_snow_column(config);
	style.arms       = gowl_config_get_snow_arms(config);
	style.drift      = gowl_config_get_snow_drift(config);
	style.flutter    = gowl_config_get_snow_flutter(config);
	style.spin       = gowl_config_get_snow_spin(config);
	style.melt       = gowl_config_get_snow_melt(config);
	style.shrink     = gowl_config_get_snow_shrink(config);
	style.depth      = gowl_config_get_snow_depth(config);
	style.runs       = gowl_config_get_snow_runs(config);
	style.run_width  = gowl_config_get_snow_run_width(config);
	style.run_length = gowl_config_get_snow_run_length(config);
	style.beads      = gowl_config_get_snow_beads(config);
	style.ice        = gowl_config_get_snow_ice(config);
	style.ice_rate   = gowl_config_get_snow_ice_rate(config);
	style.ice_scale  = gowl_config_get_snow_ice_scale(config);
	style.sparkle    = gowl_config_get_snow_sparkle(config);
	style.fog        = gowl_config_get_snow_fog(config);
	style.glow       = gowl_config_get_snow_glow(config);
	style.specular   = gowl_config_get_snow_specular(config);
	style.shine      = gowl_config_get_snow_shine(config);
	style.rim        = gowl_config_get_snow_rim(config);
	style.absorption = gowl_config_get_snow_absorption(config);
	style.life       = gowl_config_get_snow_life(config);
	style.clarity    = gowl_config_get_snow_clarity(config);
	style.opacity    = gowl_config_get_snow_opacity(config);
	style.brightness = gowl_config_get_snow_brightness(config);
	style.light      = gowl_config_get_snow_light(config);
	style.fps        = gowl_config_get_snow_fps(config);
	style.scale      = gowl_config_get_snow_scale(config);
	style.frost      = gowl_config_get_snow_frost(config);
	style.frost_passes = gowl_config_get_snow_frost_passes(config);

	style.tint[0] = style.tint[1] = style.tint[2] = 1.0;
	spec = gowl_config_get_snow_tint(config);
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
snow_build_source(GowlModuleSnow *mod, GowlCompositor *self,
                   GowlMonitor *m, GowlSnowSource *src)
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

static GowlSnowSource *
snow_ensure_source(GowlModuleSnow *mod, GowlCompositor *self,
                    GowlMonitor *m)
{
	GowlSnowSource *src = snow_source_for(mod, m);
	gboolean              stale;

	if (m->wlr_output == NULL)
		return NULL;

	if (src == NULL) {
		src = g_new0(GowlSnowSource, 1);
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

	if (stale && !snow_build_source(mod, self, m, src))
		return NULL;
	return src->sharp.tex != 0 && src->soft.tex != 0 ? src : NULL;
}

/* ── Per-client rendering ────────────────────────────────────────── */

static gboolean
snow_client_eligible(GowlClient *c)
{
	return c != NULL && c->scene != NULL && !c->isfullscreen
	       && !gowl_fx_client_is_pinned(c);
}

/* Whether this window should have the effect behind it at all. */
static gboolean
snow_client_wants(GowlCompositor *self, GowlClient *c)
{
	return snow_client_eligible(c)
	       && gowl_config_get_backdrop_style(self->config) == GOWL_BACKDROP_SNOW
	       && !(c->rule_flags & GOWL_CLIENT_RULE_NO_BLUR)
	       && c->alpha < GOWL_SNOW_MIN_TRANSPARENCY
	       && c->mon != NULL;
}

static struct wlr_box
snow_drawn_frame(GowlClient *c)
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
snow_corner_radius(GowlCompositor *self, const struct wlr_box *frame,
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
snow_acquire_buffer(GowlCompositor *self, GowlSnowNodes *nodes,
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
snow_fill_params(const GowlSnowStyle *style,
                  const GowlBackdropPlan *plan,
                  gdouble radius, guint seed, GowlFxSnowParams *out)
{
	gdouble scale = (plan->scale_x + plan->scale_y) * 0.5;
	gdouble rad   = style->light * G_PI / 180.0;
	gdouble lx, ly;

	gowl_fx_snow_params_init(out);
	out->width      = plan->buf_width;
	out->height     = plan->buf_height;
	out->radius     = (gfloat)(radius * scale);
	/*
	 * Lengths scale with the render; fractions, shares and exponents do
	 * not.
	 *
	 * `shrink' is the one worth naming: it is how big the water bead is
	 * as a SHARE of the crystal it came from, so it is already a ratio,
	 * and scaling it would make a half-size render melt to a different
	 * proportion than a full-size one -- two windows side by side in
	 * visible disagreement about how much of a snowflake is water.
	 * `flutter' is likewise a fraction of a column, and the shader's
	 * three-column reach is worked out against the unscaled number.
	 */
	out->flake      = (gfloat)(style->flake * scale);
	out->cell       = (gfloat)(style->cell * scale);
	out->column     = (gfloat)(style->column * scale);
	out->drift      = (gfloat)(style->drift * scale);
	out->run_width  = (gfloat)(style->run_width * scale);
	out->run_len    = (gfloat)(style->run_length * scale);
	out->frost_scale = (gfloat)(style->ice_scale * scale);
	out->flutter    = (gfloat)style->flutter;
	out->shrink     = (gfloat)style->shrink;
	out->settled    = (gfloat)style->settled;
	out->falling    = (gfloat)style->falling;
	out->arms       = (gfloat)style->arms;
	out->spin       = (gfloat)style->spin;
	out->melt       = (gfloat)style->melt;
	/* NOT scaled: `depth' is measured in a bead's own radii, so it is
	 * already a ratio, and multiplying it by the render scale would make
	 * a half-size render refract half as much. */
	out->depth      = (gfloat)style->depth;
	out->bulge      = 1.0f;
	out->runs       = (gfloat)style->runs;
	out->beads      = (gfloat)style->beads;
	out->frost      = (gfloat)style->ice;
	out->sparkle    = (gfloat)style->sparkle;
	out->fog        = (gfloat)style->fog;
	out->clarity    = (gfloat)style->clarity;
	out->glow       = (gfloat)style->glow;
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
	/* Rendered at `snow-scale' of the window by default, so this is
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
snow_update_client(GowlModuleSnow *mod, GowlCompositor *self,
                    GowlClient *c, gboolean redraw)
{
	GowlSnowNodes  *nodes;
	GowlSnowSource *src;
	GowlBackdropPlan      plan;
	struct wlr_box        frame;
	gdouble               radius;

	snow_ensure_gl(mod, self);
	if (mod->gl == NULL || self->config == NULL || self->locked)
		return FALSE;

	snow_read_style(mod, self->config);

	if (!snow_client_eligible(c)) {
		snow_clear_nodes(c);
		return FALSE;
	}

	if (!snow_client_wants(self, c)) {
		nodes = snow_nodes(c, FALSE);
		if (nodes != NULL)
			snow_drop_node(nodes);
		return FALSE;
	}

	src = snow_ensure_source(mod, self, c->mon);
	if (src == NULL)
		return TRUE;   /* wanted, just not drawable this instant */

	frame = snow_drawn_frame(c);
	if (!gowl_backdrop_plan(&frame, &c->mon->m, src->width, src->height,
	                        CLAMP(mod->style.scale, 1, 4), &plan)) {
		nodes = snow_nodes(c, FALSE);
		if (nodes != NULL && nodes->node != NULL)
			wlr_scene_node_set_enabled(&nodes->node->node, FALSE);
		return FALSE;
	}

	nodes  = snow_nodes(c, TRUE);
	radius = snow_corner_radius(self, &frame, c->bw);

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
		GowlFxSnowParams  params;
		gfloat             clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		gboolean           ok;

		buf = snow_acquire_buffer(self, nodes, plan.buf_width,
		                           plan.buf_height);
		if (buf == NULL)
			return TRUE;

		pass = gowl_fx_pass_begin(mod->gl, buf);
		if (pass == NULL) {
			wlr_buffer_unlock(buf);
			return TRUE;
		}
		snow_fill_params(&mod->style, &plan, radius,
		                  gowl_client_get_id(c), &params);
		gowl_fx_pass_clear(pass, clear);
		ok = gowl_fx_pass_snow(pass, &src->soft, &src->sharp, &params,
		                 &mod->clock);
		gowl_fx_pass_end(pass);

		if (!ok) {
			wlr_buffer_unlock(buf);
			/* The shader would not build.  Nothing will come of trying
			 * again, so let the output go back to sleep. */
			return FALSE;
		}

		if (nodes->node == NULL)
			snow_set_node(nodes, wlr_scene_buffer_create(c->scene, buf));
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
snow_frame(GowlSceneEffect *effect, GowlCompositor *self, GowlMonitor *m,
            gint64 now_us)
{
	GowlModuleSnow  *mod = GOWL_MODULE_SNOW(effect);
	GowlSnowSource  *src;
	GList                 *l;
	gboolean               redraw, live = FALSE;

	g_weak_ref_set(&mod->compositor, self);

	if (self == NULL || self->config == NULL || self->locked || mod->capturing)
		return FALSE;
	if (gowl_config_get_backdrop_style(self->config) != GOWL_BACKDROP_SNOW)
		return FALSE;

	snow_ensure_gl(mod, self);
	if (mod->gl == NULL)
		return FALSE;
	snow_read_style(mod, self->config);

	/*
	 * The clock advances against the WALL, not once per output: with two
	 * monitors this hook runs twice a frame, and advancing it each time
	 * would run the effect at double speed on a two-screen desk.
	 * Tracking the last instant instead makes the second call's step
	 * nearly zero, which is correct -- no time has passed.
	 */
	if (mod->last_tick_us != 0 && now_us > mod->last_tick_us) {
		gowl_fx_snow_advance(&mod->clock,
		               (gdouble)(now_us - mod->last_tick_us) / 1e6,
		               mod->style.speed, mod->style.life,
		               mod->style.ice_rate);
	}
	if (now_us > mod->last_tick_us)
		mod->last_tick_us = now_us;

	/* Throttled per output: two screens at different refresh rates should
	 * each be held against their own clock. */
	src = snow_source_for(mod, m);
	redraw = TRUE;
	if (src != NULL && mod->style.fps > 0 && now_us < src->next_draw_us)
		redraw = FALSE;

	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if (c->mon != m)
			continue;
		if (snow_update_client(mod, self, c, redraw))
			live = TRUE;
	}

	/* Set AFTER the loop: snow_update_client() may have built the source
	 * on its first pass, and the throttle belongs to the source. */
	if (redraw) {
		src = snow_source_for(mod, m);
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
snow_client_event(GowlSceneEffect *effect, GowlCompositor *self,
                   GowlClient *c, GowlSceneEffectEvent event,
                   const struct wlr_box *previous, gboolean interactive)
{
	GowlModuleSnow *mod = GOWL_MODULE_SNOW(effect);

	g_weak_ref_set(&mod->compositor, self);

	if (c == NULL || mod->capturing)
		return FALSE;

	switch (event) {
	case GOWL_SCENE_EFFECT_UNMAP:
		if (g_object_get_data(G_OBJECT(c), GOWL_SNOW_DATA_KEY) != NULL) {
			snow_forget_node(snow_nodes(c, FALSE));
			snow_clear_nodes(c);
		}
		break;
	case GOWL_SCENE_EFFECT_DESTROY:
		snow_clear_nodes(c);
		break;
	case GOWL_SCENE_EFFECT_REVEAL:
	case GOWL_SCENE_EFFECT_RELEASE:
		snow_update_client(mod, self, c, TRUE);
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
snow_client_placed(GowlSceneEffect *effect, GowlCompositor *self,
                    GowlClient *c, gboolean settled)
{
	GowlModuleSnow *mod = GOWL_MODULE_SNOW(effect);

	g_weak_ref_set(&mod->compositor, self);

	if (c == NULL || mod->capturing)
		return;
	/*
	 * NOT WHILE IT IS BEING DRAGGED.
	 *
	 * This hook runs once per POINTER MOTION EVENT of an interactive
	 * move or resize -- see gowl_compositor_client_is_grabbed() for why
	 * `settled' cannot notice that -- and a mouse reports several times
	 * faster than a screen refreshes.  Worse, a resize changes the
	 * buffer size on every one of them, and a changed size throws the
	 * swapchain away and allocates a new one.
	 *
	 * The frame hook redraws this window anyway, at the output's rate,
	 * and a moved window makes the plan stale so it will not be skipped
	 * by the throttle.  So the drag costs what it was always going to
	 * cost once, per frame, instead of once per input event.
	 */
	if (gowl_compositor_client_is_grabbed(self, c))
		return;
	snow_update_client(mod, self, c, TRUE);
}

static void
snow_alpha_changed(GowlSceneEffect *effect, GowlClient *c, gfloat alpha)
{
	GowlModuleSnow *mod = GOWL_MODULE_SNOW(effect);
	GowlCompositor       *self = c != NULL ? c->compositor : NULL;

	if (self != NULL)
		snow_update_client(mod, self, c, TRUE);
}

static void
snow_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                      GowlMonitor *m)
{
	GowlModuleSnow  *mod = GOWL_MODULE_SNOW(effect);
	GowlSnowSource  *src = snow_source_for(mod, m);

	if (src != NULL) {
		mod->sources = g_list_remove(mod->sources, src);
		snow_source_free(mod, src);
	}
}

static void
snow_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	GowlModuleSnow *mod = GOWL_MODULE_SNOW(effect);
	GList *l;

	if (self != NULL) {
		for (l = self->clients; l != NULL; l = l->next)
			snow_clear_nodes(l->data);
	}
	snow_drop_sources(mod);
	g_clear_pointer(&mod->gl, gowl_fx_gl_free);
	mod->gl_tried = FALSE;
	/* The clock is wall time, and the wall kept going while the renderer
	 * was away.  Forget where it was rather than hand the next advance a
	 * gap of however long the GPU reset took. */
	mod->last_tick_us = 0;
	g_weak_ref_set(&mod->compositor, NULL);
}

static void
snow_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_event    = snow_client_event;
	iface->alpha_changed   = snow_alpha_changed;
	iface->monitor_removed = snow_monitor_removed;
	iface->finish          = snow_finish;
	iface->client_placed   = snow_client_placed;
	iface->frame           = snow_frame;
}

static void
snow_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	snow_finish(GOWL_SCENE_EFFECT(handler), compositor);
}

static void
snow_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = snow_shutdown;
}

/* ── Module ──────────────────────────────────────────────────────── */

/* The same priority as the blur, the glass, the water and the rain, and
 * after the animation module for the hooks all of them hear. */
#define GOWL_SNOW_PRIORITY (10)

static gboolean
snow_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_SNOW_PRIORITY);
	return TRUE;
}

static void
snow_deactivate(GowlModule *base)
{
	GowlModuleSnow *mod = GOWL_MODULE_SNOW(base);
	GowlCompositor       *self = g_weak_ref_get(&mod->compositor);

	if (self != NULL) {
		snow_finish(GOWL_SCENE_EFFECT(mod), self);
		g_object_unref(self);
	} else {
		snow_drop_sources(mod);
		g_clear_pointer(&mod->gl, gowl_fx_gl_free);
		mod->gl_tried = FALSE;
	}
}

static const gchar *snow_name(GowlModule *m)    { return "snow"; }
static const gchar *snow_version(GowlModule *m) { return "0.1.0"; }
static const gchar *snow_description(GowlModule *m)
{
	return "Snow falling past the window, settling on it and melting off";
}

static void
snow_finalize(GObject *object)
{
	GowlModuleSnow *mod = GOWL_MODULE_SNOW(object);

	snow_deactivate(GOWL_MODULE(object));
	g_weak_ref_clear(&mod->compositor);
	G_OBJECT_CLASS(gowl_module_snow_parent_class)->finalize(object);
}

static void
gowl_module_snow_class_init(GowlModuleSnowClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = snow_activate;
	mod->deactivate      = snow_deactivate;
	mod->get_name        = snow_name;
	mod->get_description = snow_description;
	mod->get_version     = snow_version;
	G_OBJECT_CLASS(klass)->finalize = snow_finalize;
}

static void
gowl_module_snow_init(GowlModuleSnow *mod)
{
	g_weak_ref_init(&mod->compositor, NULL);
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_SNOW_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_SNOW;
}
