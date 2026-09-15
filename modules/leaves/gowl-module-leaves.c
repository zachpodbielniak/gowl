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
 * Falling leaves: autumn against the window.
 *
 * The drawing is in fx/gowl-fx-leaves.c -- the odd one of the family,
 * because a leaf is an OPAQUE OBJECT that covers the wallpaper rather
 * than a lens that bends it.  The host half is unchanged.
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
 *   - A FRAME RATE.  `leaves-fps' (30 by default) is a throttle, not a
 *     target: the pane is redrawn no more often than that, however fast
 *     the output runs.  Leaves read correctly at thirty and
 *     better than the rain does: a leaf takes several seconds to
 *     come down, so a frame is a small part of its journey.
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
 * pass per eligible window per tick, at 1/`leaves-scale' of the
 * window's size (half by default, which is a quarter of the pixels).
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-leaves"

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
#define GOWL_LEAVES_MIN_TRANSPARENCY (0.985)

#define GOWL_TYPE_MODULE_LEAVES (gowl_module_leaves_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleLeaves, gowl_module_leaves,
                     GOWL, MODULE_LEAVES, GowlModule)

/**
 * GowlLeavesStyle:
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
	gdouble leaf;
	gdouble cell;
	gdouble stuck;
	gdouble column;
	gdouble falling;
	gdouble flutter;
	gdouble tumble;
	gdouble wind;
	gdouble gust;
	gdouble gustiness;
	gdouble curl;
	gdouble veins;
	gdouble translucency;
	gdouble gloss;
	gdouble shine;
	gdouble shadow;
	gdouble fog;
	gdouble clarity;
	gdouble speed;
	gdouble tenure;
	gdouble opacity;
	gdouble brightness;
	gdouble light;
	gdouble warm[3];
	gdouble gold[3];
	gdouble dry[3];
	gint    fps;
	gint    scale;
	gint    frost;
	gint    frost_passes;
} GowlLeavesStyle;

/**
 * GowlLeavesSource:
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
} GowlLeavesSource;

#define GOWL_LEAVES_DATA_KEY "gowl-leaves-node"

typedef struct {
	struct wlr_scene_buffer *node;
	struct wl_listener       node_destroy;
	struct wlr_swapchain    *swapchain;
	GowlBackdropPlan         plan;
	gboolean                 have;
	guint64                  serial;      /* which wallpaper capture */
	guint64                  generation;  /* which settings */
	gdouble                  radius;
} GowlLeavesNodes;

struct _GowlModuleLeaves {
	GowlModule       parent_instance;
	GWeakRef         compositor;
	GowlFxGl        *gl;
	gboolean         gl_tried;
	GList           *sources;      /* GowlLeavesSource* */
	gboolean         capturing;
	guint64          serial;
	GowlLeavesStyle style;
	gboolean         style_known;
	guint64          generation;
	/* One clock for the whole desktop.  Two windows side by side are two
	 * panes in the same weather, and giving each its own makes them
	 * visibly disagree at the seam. */
	GowlFxLeafClock  clock;
	gint64           last_tick_us;
};

static void leaves_effect_init(GowlSceneEffectInterface *iface);
static void leaves_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleLeaves, gowl_module_leaves,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, leaves_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, leaves_shutdown_init))

/* ── Plumbing ────────────────────────────────────────────────────── */

static void
leaves_on_node_destroy(struct wl_listener *listener, void *data)
{
	GowlLeavesNodes *nodes;

	(void)data;
	nodes = wl_container_of(listener, nodes, node_destroy);
	wl_list_remove(&listener->link);
	nodes->node = NULL;
}

static void
leaves_set_node(GowlLeavesNodes *nodes, struct wlr_scene_buffer *buf)
{
	nodes->node = buf;
	if (buf == NULL)
		return;
	nodes->node_destroy.notify = leaves_on_node_destroy;
	wl_signal_add(&buf->node.events.destroy, &nodes->node_destroy);
}

static void
leaves_forget_node(GowlLeavesNodes *nodes)
{
	if (nodes == NULL || nodes->node == NULL)
		return;
	wl_list_remove(&nodes->node_destroy.link);
	nodes->node = NULL;
}

static void
leaves_nodes_free(gpointer data)
{
	GowlLeavesNodes *nodes = data;

	if (nodes->node != NULL)
		wlr_scene_node_destroy(&nodes->node->node);
	g_clear_pointer(&nodes->swapchain, wlr_swapchain_destroy);
	g_free(nodes);
}

static GowlLeavesNodes *
leaves_nodes(GowlClient *c, gboolean create)
{
	GowlLeavesNodes *nodes = g_object_get_data(G_OBJECT(c),
	                                              GOWL_LEAVES_DATA_KEY);

	if (nodes == NULL && create) {
		nodes = g_new0(GowlLeavesNodes, 1);
		g_object_set_data_full(G_OBJECT(c), GOWL_LEAVES_DATA_KEY, nodes,
		                       leaves_nodes_free);
	}
	return nodes;
}

static void
leaves_clear_nodes(GowlClient *c)
{
	if (g_object_get_data(G_OBJECT(c), GOWL_LEAVES_DATA_KEY) != NULL)
		g_object_set_data(G_OBJECT(c), GOWL_LEAVES_DATA_KEY, NULL);
}

/* Take the node down and give the buffers back.  The swapchain goes with
 * it: it is several buffers the size of the window, and the reason this
 * is called is usually that the effect has been switched off. */
static void
leaves_drop_node(GowlLeavesNodes *nodes)
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
leaves_ensure_gl(GowlModuleLeaves *mod, GowlCompositor *self)
{
	if (mod->gl != NULL || mod->gl_tried)
		return;
	if (self == NULL || self->renderer == NULL)
		return;

	mod->gl_tried = TRUE;
	mod->gl = gowl_fx_gl_new(self->renderer);
}

static void
leaves_source_free(GowlModuleLeaves *mod, GowlLeavesSource *src)
{
	if (src == NULL)
		return;
	gowl_fx_texture_drop(mod->gl, &src->sharp);
	gowl_fx_texture_drop(mod->gl, &src->soft);
	g_free(src);
}

static void
leaves_drop_sources(GowlModuleLeaves *mod)
{
	GList *l;

	for (l = mod->sources; l != NULL; l = l->next)
		leaves_source_free(mod, l->data);
	g_list_free(mod->sources);
	mod->sources = NULL;
}

static GowlLeavesSource *
leaves_source_for(GowlModuleLeaves *mod, GowlMonitor *m)
{
	GList *l;

	for (l = mod->sources; l != NULL; l = l->next) {
		GowlLeavesSource *src = l->data;

		if (src->monitor == m)
			return src;
	}
	return NULL;
}

/* ── The look, out of the config ─────────────────────────────────── */

static gboolean
leaves_read_style(GowlModuleLeaves *mod, GowlConfig *config)
{
	GowlLeavesStyle  style;
	GowlColor          *col;
	const gchar        *spec;
	gdouble             k;
	gint                i;
	static const gfloat fallback[3][3] = {
		{ 0.86f, 0.22f, 0.13f },
		{ 0.95f, 0.62f, 0.14f },
		{ 0.64f, 0.39f, 0.17f }
	};

	memset(&style, 0, sizeof(style));

	/*
	 * One knob over the whole preset.  It scales the four that together
	 * mean "how far into autumn is this" -- how many leaves are falling,
	 * how many are stuck, how fast they come down and how hard the wind
	 * blows -- and nothing else.
	 *
	 * Deliberately NOT the leaf size or the spacing.  Bigger leaves are
	 * not a heavier fall, they are a closer tree; and raising the size
	 * here would silently do nothing anyway, because the shader caps the
	 * blade against the column so it cannot be sliced off at the column
	 * edge.  An intensity knob that is quietly ignored above 1.2 is worse
	 * than one that does not touch the field at all.
	 */
	k = gowl_config_get_leaves_intensity(config);

	style.falling   = MIN(1.0, gowl_config_get_leaves_falling(config) * k);
	style.stuck     = MIN(1.0, gowl_config_get_leaves_stuck(config) * k);
	style.speed     = gowl_config_get_leaves_speed(config) * k;
	style.gustiness = MIN(2.0, gowl_config_get_leaves_gustiness(config) * k);

	style.leaf         = gowl_config_get_leaves_leaf(config);
	style.cell         = gowl_config_get_leaves_cell(config);
	style.column       = gowl_config_get_leaves_column(config);
	style.flutter      = gowl_config_get_leaves_flutter(config);
	style.tumble       = gowl_config_get_leaves_tumble(config);
	style.wind         = gowl_config_get_leaves_wind(config);
	style.gust         = gowl_config_get_leaves_gust(config);
	style.curl         = gowl_config_get_leaves_curl(config);
	style.veins        = gowl_config_get_leaves_veins(config);
	style.translucency = gowl_config_get_leaves_translucency(config);
	style.gloss        = gowl_config_get_leaves_gloss(config);
	style.shine        = gowl_config_get_leaves_shine(config);
	style.shadow       = gowl_config_get_leaves_shadow(config);
	style.fog          = gowl_config_get_leaves_fog(config);
	style.tenure       = gowl_config_get_leaves_tenure(config);
	style.opacity      = gowl_config_get_leaves_opacity(config);
	style.brightness   = gowl_config_get_leaves_brightness(config);
	style.light        = gowl_config_get_leaves_light(config);
	style.fps          = gowl_config_get_leaves_fps(config);
	style.scale        = gowl_config_get_leaves_scale(config);
	style.frost        = gowl_config_get_leaves_frost(config);
	style.frost_passes = gowl_config_get_leaves_frost_passes(config);
	/* The blade holds a film of water under it; that is the only thing
	 * here that lifts the pane's haze, so it is not a config key of its
	 * own -- there is nothing else for a `leaves-clarity' to mean. */
	style.clarity      = 0.70;

	/*
	 * The three points of the autumn ramp, each a colour spec the palette
	 * has already resolved.  A named colour that will not parse falls
	 * back to the tuned value rather than to white: three white tints
	 * would leave every leaf a grey silhouette, which looks like a
	 * rendering bug rather than like a typo in a config.
	 */
	for (i = 0; i < 3; i++) {
		gdouble *dst = i == 0 ? style.warm : (i == 1 ? style.gold : style.dry);

		spec = i == 0 ? gowl_config_get_leaves_warm(config)
		     : i == 1 ? gowl_config_get_leaves_gold(config)
		              : gowl_config_get_leaves_dry(config);
		col = spec != NULL ? gowl_color_new_from_hex(spec) : NULL;
		if (col != NULL) {
			dst[0] = col->r;
			dst[1] = col->g;
			dst[2] = col->b;
			gowl_color_free(col);
		} else {
			dst[0] = fallback[i][0];
			dst[1] = fallback[i][1];
			dst[2] = fallback[i][2];
		}
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
leaves_build_source(GowlModuleLeaves *mod, GowlCompositor *self,
                   GowlMonitor *m, GowlLeavesSource *src)
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

static GowlLeavesSource *
leaves_ensure_source(GowlModuleLeaves *mod, GowlCompositor *self,
                    GowlMonitor *m)
{
	GowlLeavesSource *src = leaves_source_for(mod, m);
	gboolean              stale;

	if (m->wlr_output == NULL)
		return NULL;

	if (src == NULL) {
		src = g_new0(GowlLeavesSource, 1);
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

	if (stale && !leaves_build_source(mod, self, m, src))
		return NULL;
	return src->sharp.tex != 0 && src->soft.tex != 0 ? src : NULL;
}

/* ── Per-client rendering ────────────────────────────────────────── */

static gboolean
leaves_client_eligible(GowlClient *c)
{
	return c != NULL && c->scene != NULL && !c->isfullscreen
	       && !gowl_fx_client_is_pinned(c);
}

/* Whether this window should have the effect behind it at all. */
static gboolean
leaves_client_wants(GowlCompositor *self, GowlClient *c)
{
	return leaves_client_eligible(c)
	       && gowl_config_get_backdrop_style(self->config) == GOWL_BACKDROP_LEAVES
	       && !(c->rule_flags & GOWL_CLIENT_RULE_NO_BLUR)
	       && c->alpha < GOWL_LEAVES_MIN_TRANSPARENCY
	       && c->mon != NULL;
}

static struct wlr_box
leaves_drawn_frame(GowlClient *c)
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
leaves_corner_radius(GowlCompositor *self, const struct wlr_box *frame,
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
leaves_acquire_buffer(GowlCompositor *self, GowlLeavesNodes *nodes,
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
leaves_fill_params(const GowlLeavesStyle *style,
                  const GowlBackdropPlan *plan,
                  gdouble radius, guint seed, GowlFxLeafParams *out)
{
	gdouble scale = (plan->scale_x + plan->scale_y) * 0.5;
	gdouble rad   = style->light * G_PI / 180.0;
	gdouble lx, ly;

	gowl_fx_leaf_params_init(out);
	out->width      = plan->buf_width;
	out->height     = plan->buf_height;
	out->radius     = (gfloat)(radius * scale);
	/*
	 * Lengths scale with the render; fractions, counts and weights do
	 * not.
	 *
	 * `flutter' is in the second group and it is the easy one to get
	 * wrong: it is already a fraction of a COLUMN, so scaling it as well
	 * would halve every swing at `leaves-scale' 2 -- which is always --
	 * and the shader's three-column reach is worked out against the
	 * unscaled number.  `tumble' is turns per fall, which is a count.
	 */
	out->leaf      = (gfloat)(style->leaf * scale);
	out->cell      = (gfloat)(style->cell * scale);
	out->column    = (gfloat)(style->column * scale);
	out->wind      = (gfloat)(style->wind * scale);
	out->gust_push = (gfloat)(style->gust * scale);
	out->flutter   = (gfloat)style->flutter;
	out->tumble    = (gfloat)style->tumble;
	out->stuck     = (gfloat)style->stuck;
	out->falling   = (gfloat)style->falling;
	out->curl      = (gfloat)style->curl;
	out->veins     = (gfloat)style->veins;
	out->translucency = (gfloat)style->translucency;
	out->gloss     = (gfloat)style->gloss;
	out->shine     = (gfloat)style->shine;
	out->shadow    = (gfloat)style->shadow;
	out->fog       = (gfloat)style->fog;
	out->clarity   = (gfloat)style->clarity;
	out->brightness = (gfloat)style->brightness;
	out->alpha     = (gfloat)style->opacity;
	out->tint_warm[0] = (gfloat)style->warm[0];
	out->tint_warm[1] = (gfloat)style->warm[1];
	out->tint_warm[2] = (gfloat)style->warm[2];
	out->tint_gold[0] = (gfloat)style->gold[0];
	out->tint_gold[1] = (gfloat)style->gold[1];
	out->tint_gold[2] = (gfloat)style->gold[2];
	out->tint_dry[0]  = (gfloat)style->dry[0];
	out->tint_dry[1]  = (gfloat)style->dry[1];
	out->tint_dry[2]  = (gfloat)style->dry[2];


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
	/* Rendered at `leaves-scale' of the window by default, so this is
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
leaves_update_client(GowlModuleLeaves *mod, GowlCompositor *self,
                    GowlClient *c, gboolean redraw)
{
	GowlLeavesNodes  *nodes;
	GowlLeavesSource *src;
	GowlBackdropPlan      plan;
	struct wlr_box        frame;
	gdouble               radius;

	leaves_ensure_gl(mod, self);
	if (mod->gl == NULL || self->config == NULL || self->locked)
		return FALSE;

	leaves_read_style(mod, self->config);

	if (!leaves_client_eligible(c)) {
		leaves_clear_nodes(c);
		return FALSE;
	}

	if (!leaves_client_wants(self, c)) {
		nodes = leaves_nodes(c, FALSE);
		if (nodes != NULL)
			leaves_drop_node(nodes);
		return FALSE;
	}

	src = leaves_ensure_source(mod, self, c->mon);
	if (src == NULL)
		return TRUE;   /* wanted, just not drawable this instant */

	frame = leaves_drawn_frame(c);
	if (!gowl_backdrop_plan(&frame, &c->mon->m, src->width, src->height,
	                        CLAMP(mod->style.scale, 1, 4), &plan)) {
		nodes = leaves_nodes(c, FALSE);
		if (nodes != NULL && nodes->node != NULL)
			wlr_scene_node_set_enabled(&nodes->node->node, FALSE);
		return FALSE;
	}

	nodes  = leaves_nodes(c, TRUE);
	radius = leaves_corner_radius(self, &frame, c->bw);

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
		GowlFxLeafParams  params;
		gfloat             clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		gboolean           ok;

		buf = leaves_acquire_buffer(self, nodes, plan.buf_width,
		                           plan.buf_height);
		if (buf == NULL)
			return TRUE;

		pass = gowl_fx_pass_begin(mod->gl, buf);
		if (pass == NULL) {
			wlr_buffer_unlock(buf);
			return TRUE;
		}
		leaves_fill_params(&mod->style, &plan, radius,
		                  gowl_client_get_id(c), &params);
		gowl_fx_pass_clear(pass, clear);
		ok = gowl_fx_pass_leaves(pass, &src->soft, &src->sharp, &params,
		                 &mod->clock);
		gowl_fx_pass_end(pass);

		if (!ok) {
			wlr_buffer_unlock(buf);
			/* The shader would not build.  Nothing will come of trying
			 * again, so let the output go back to sleep. */
			return FALSE;
		}

		if (nodes->node == NULL)
			leaves_set_node(nodes, wlr_scene_buffer_create(c->scene, buf));
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
leaves_frame(GowlSceneEffect *effect, GowlCompositor *self, GowlMonitor *m,
            gint64 now_us)
{
	GowlModuleLeaves  *mod = GOWL_MODULE_LEAVES(effect);
	GowlLeavesSource  *src;
	GList                 *l;
	gboolean               redraw, live = FALSE;

	g_weak_ref_set(&mod->compositor, self);

	if (self == NULL || self->config == NULL || self->locked || mod->capturing)
		return FALSE;
	if (gowl_config_get_backdrop_style(self->config) != GOWL_BACKDROP_LEAVES)
		return FALSE;

	leaves_ensure_gl(mod, self);
	if (mod->gl == NULL)
		return FALSE;
	leaves_read_style(mod, self->config);

	/*
	 * The clock advances against the WALL, not once per output: with two
	 * monitors this hook runs twice a frame, and advancing it each time
	 * would run the effect at double speed on a two-screen desk.
	 * Tracking the last instant instead makes the second call's step
	 * nearly zero, which is correct -- no time has passed.
	 */
	if (mod->last_tick_us != 0 && now_us > mod->last_tick_us) {
		gowl_fx_leaf_advance(&mod->clock,
		               (gdouble)(now_us - mod->last_tick_us) / 1e6,
		               mod->style.speed, mod->style.tenure,
		               mod->style.gustiness);
	}
	if (now_us > mod->last_tick_us)
		mod->last_tick_us = now_us;

	/* Throttled per output: two screens at different refresh rates should
	 * each be held against their own clock. */
	src = leaves_source_for(mod, m);
	redraw = TRUE;
	if (src != NULL && mod->style.fps > 0 && now_us < src->next_draw_us)
		redraw = FALSE;

	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if (c->mon != m)
			continue;
		if (leaves_update_client(mod, self, c, redraw))
			live = TRUE;
	}

	/* Set AFTER the loop: leaves_update_client() may have built the source
	 * on its first pass, and the throttle belongs to the source. */
	if (redraw) {
		src = leaves_source_for(mod, m);
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
leaves_client_event(GowlSceneEffect *effect, GowlCompositor *self,
                   GowlClient *c, GowlSceneEffectEvent event,
                   const struct wlr_box *previous, gboolean interactive)
{
	GowlModuleLeaves *mod = GOWL_MODULE_LEAVES(effect);

	g_weak_ref_set(&mod->compositor, self);

	if (c == NULL || mod->capturing)
		return FALSE;

	switch (event) {
	case GOWL_SCENE_EFFECT_UNMAP:
		if (g_object_get_data(G_OBJECT(c), GOWL_LEAVES_DATA_KEY) != NULL) {
			leaves_forget_node(leaves_nodes(c, FALSE));
			leaves_clear_nodes(c);
		}
		break;
	case GOWL_SCENE_EFFECT_DESTROY:
		leaves_clear_nodes(c);
		break;
	case GOWL_SCENE_EFFECT_REVEAL:
	case GOWL_SCENE_EFFECT_RELEASE:
		leaves_update_client(mod, self, c, TRUE);
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
leaves_client_placed(GowlSceneEffect *effect, GowlCompositor *self,
                    GowlClient *c, gboolean settled)
{
	GowlModuleLeaves *mod = GOWL_MODULE_LEAVES(effect);

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
	leaves_update_client(mod, self, c, TRUE);
}

static void
leaves_alpha_changed(GowlSceneEffect *effect, GowlClient *c, gfloat alpha)
{
	GowlModuleLeaves *mod = GOWL_MODULE_LEAVES(effect);
	GowlCompositor       *self = c != NULL ? c->compositor : NULL;

	if (self != NULL)
		leaves_update_client(mod, self, c, TRUE);
}

static void
leaves_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                      GowlMonitor *m)
{
	GowlModuleLeaves  *mod = GOWL_MODULE_LEAVES(effect);
	GowlLeavesSource  *src = leaves_source_for(mod, m);

	if (src != NULL) {
		mod->sources = g_list_remove(mod->sources, src);
		leaves_source_free(mod, src);
	}
}

static void
leaves_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	GowlModuleLeaves *mod = GOWL_MODULE_LEAVES(effect);
	GList *l;

	if (self != NULL) {
		for (l = self->clients; l != NULL; l = l->next)
			leaves_clear_nodes(l->data);
	}
	leaves_drop_sources(mod);
	g_clear_pointer(&mod->gl, gowl_fx_gl_free);
	mod->gl_tried = FALSE;
	/* The clock is wall time, and the wall kept going while the renderer
	 * was away.  Forget where it was rather than hand the next advance a
	 * gap of however long the GPU reset took. */
	mod->last_tick_us = 0;
	g_weak_ref_set(&mod->compositor, NULL);
}

static void
leaves_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_event    = leaves_client_event;
	iface->alpha_changed   = leaves_alpha_changed;
	iface->monitor_removed = leaves_monitor_removed;
	iface->finish          = leaves_finish;
	iface->client_placed   = leaves_client_placed;
	iface->frame           = leaves_frame;
}

static void
leaves_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	leaves_finish(GOWL_SCENE_EFFECT(handler), compositor);
}

static void
leaves_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = leaves_shutdown;
}

/* ── Module ──────────────────────────────────────────────────────── */

/* The same priority as the blur, the glass, the water and the rain, and
 * after the animation module for the hooks all of them hear. */
#define GOWL_LEAVES_PRIORITY (10)

static gboolean
leaves_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_LEAVES_PRIORITY);
	return TRUE;
}

static void
leaves_deactivate(GowlModule *base)
{
	GowlModuleLeaves *mod = GOWL_MODULE_LEAVES(base);
	GowlCompositor       *self = g_weak_ref_get(&mod->compositor);

	if (self != NULL) {
		leaves_finish(GOWL_SCENE_EFFECT(mod), self);
		g_object_unref(self);
	} else {
		leaves_drop_sources(mod);
		g_clear_pointer(&mod->gl, gowl_fx_gl_free);
		mod->gl_tried = FALSE;
	}
}

static const gchar *leaves_name(GowlModule *m)    { return "leaves"; }
static const gchar *leaves_version(GowlModule *m) { return "0.1.0"; }
static const gchar *leaves_description(GowlModule *m)
{
	return "Autumn leaves falling past the window and collecting on it";
}

static void
leaves_finalize(GObject *object)
{
	GowlModuleLeaves *mod = GOWL_MODULE_LEAVES(object);

	leaves_deactivate(GOWL_MODULE(object));
	g_weak_ref_clear(&mod->compositor);
	G_OBJECT_CLASS(gowl_module_leaves_parent_class)->finalize(object);
}

static void
gowl_module_leaves_class_init(GowlModuleLeavesClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = leaves_activate;
	mod->deactivate      = leaves_deactivate;
	mod->get_name        = leaves_name;
	mod->get_description = leaves_description;
	mod->get_version     = leaves_version;
	G_OBJECT_CLASS(klass)->finalize = leaves_finalize;
}

static void
gowl_module_leaves_init(GowlModuleLeaves *mod)
{
	g_weak_ref_init(&mod->compositor, NULL);
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_LEAVES_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_LEAVES;
}
