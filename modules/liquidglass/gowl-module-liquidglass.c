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
 * Liquid glass: the wallpaper, refracted through the window.
 *
 * The blur module next door shows a translucent window a blurred crop of
 * the wallpaper.  This shows it the wallpaper as it would look through a
 * slab of glass the shape of the window -- flat in the middle, rounded
 * off over a bevel at the edge, so the rim bends what is behind the
 * middle out towards it, splits it into colours, darkens where it
 * magnifies, and catches a line of light.  fx/gowl-fx-glass.c has the
 * optics; this file has the bookkeeping.
 *
 * THE TWO ARE ALTERNATIVES, NOT LAYERS.  Both put one scene node in the
 * same place in the same window's tree, so both switched on means the
 * blur's is simply hidden behind the glass while still costing a capture
 * and a blur every tag switch.  `window-backdrop' picks one, both modules
 * read it, and Super+Shift+" cycles it.  Loading both is normal and is
 * what makes that key instant.
 *
 * WHAT IT COSTS, AND WHY IT IS SHAPED LIKE THIS.
 *
 * Per output, once per tag switch: one capture of the desktop with every
 * window hidden, and one blur of it.  Exactly what the blur module pays,
 * for exactly the same reason -- see its file for why the wallpaper
 * rather than each window's real backdrop.
 *
 * Per window, when it MOVES OR RESIZES: one fragment-shader pass over the
 * window.  That is the real difference between the two modules.  A blur
 * backdrop is a crop of one shared picture, so a window that moved
 * re-points its crop for nothing; glass bends the wallpaper behind THIS
 * window, so a window that moved is refracting somewhere else and has to
 * be drawn again.  Three things keep that affordable:
 *
 *   - A window that has come to rest is drawn at full device resolution.
 *     One that is still moving is drawn at HALF, and the scene stretches
 *     it.  Nobody has ever caught a lens at half resolution sliding past
 *     at sixty hertz, and it is a quarter of the pixels.
 *   - A move of less than one texture pixel is not a move
 *     (gowl_backdrop_render_stale()).  A drag reports fractional positions
 *     that round to the same pixel several frames running.
 *   - Each window has its own small swapchain, so a frame is never drawn
 *     into the buffer the scene is still reading from.
 *
 * A window whose glass cannot be rendered keeps the last picture rather
 * than blinking: a missed frame is invisible, a window that goes clear
 * for one frame is not.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-liquidglass"

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

/* Above this a window is opaque enough that no glass behind it could be
 * seen.  The same threshold the blur uses, and for the same reason. */
#define GOWL_GLASS_MIN_TRANSPARENCY (0.985)

/* How much the render is scaled down while a window is still moving. */
#define GOWL_GLASS_MOVING_DIVISOR (2)

#define GOWL_TYPE_MODULE_LIQUID_GLASS (gowl_module_liquid_glass_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleLiquidGlass, gowl_module_liquid_glass,
                     GOWL, MODULE_LIQUID_GLASS, GowlModule)

/**
 * GowlGlassStyle:
 *
 * The look, read out of the config, in LOGICAL pixels.
 *
 * Held as a plain struct and compared with memcmp() rather than tracked
 * field by field: `reload_config' can change any of it, every window's
 * render has to be redone when it does, and a comparison that forgets one
 * field leaves windows showing the old look until they next move --- a
 * bug that appears only after a reload and only on the windows nobody
 * touched.  Zeroed before it is filled so the padding compares equal.
 */
typedef struct {
	gdouble bevel;
	gdouble thickness;
	gdouble slope;
	gdouble dispersion;
	gdouble rim;
	gdouble shade;
	gdouble edge_width;
	gdouble saturation;
	gdouble clarity;
	gdouble centre_clarity;
	gdouble lens;
	gdouble sheen;
	gdouble light;
	gdouble brightness;
	gdouble opacity;
	gdouble tint[3];
	gint    shape;
	gint    frost;
	gint    frost_passes;
} GowlGlassStyle;

/**
 * GowlGlassSource:
 *
 * One output's wallpaper, sharp and frosted, as textures a shader reads.
 *
 * Textures rather than buffers --- the opposite of the blur's choice ---
 * because the consumer here IS a shader.  The blur hands its picture to
 * the scene, which crops it; this hands it to a fragment program, which
 * bends it.
 */
typedef struct {
	GowlMonitor   *monitor;   /* unowned */
	GowlFxTexture  sharp;
	GowlFxTexture  soft;
	guint32        tags;      /* what was showing when it was taken */
	gint           width, height;
	guint64        serial;    /* which capture this is */
	gint           frost;     /* what it was frosted with */
	gint           frost_passes;
} GowlGlassSource;

/* Per-client state, hung off the client so it lives and dies with it. */
#define GOWL_GLASS_DATA_KEY "gowl-liquidglass-node"

typedef struct {
	struct wlr_scene_buffer *node;
	/* On the node's destroy signal for as long as it lives, so a node
	 * that goes with the client's tree clears this pointer: a pointer
	 * that is set always names a live node. */
	struct wl_listener       node_destroy;
	/*
	 * Its OWN swapchain, for the reason the blur's backdrop has one and
	 * one more.  The blur's reason: a buffer of the output's swapchain
	 * held past the frame is a slot taken out of the rotation the output
	 * needs to present, tied to a pool the compositor keeps drawing the
	 * live desktop into -- and if that slot is ever handed back out, what
	 * you are holding stops being your picture and becomes a photograph
	 * of the desktop, windows and all.  The one more: this is redrawn
	 * while the scene may still be reading the last one, so there has to
	 * be more than one.
	 */
	struct wlr_swapchain    *swapchain;
	GowlBackdropPlan            plan;      /* what the buffer was drawn for */
	gboolean                 have;      /* whether @plan means anything */
	guint64                  serial;    /* which wallpaper capture */
	guint64                  generation;/* which settings */
	gdouble                  radius;    /* the corner radius it was drawn with */
	/*
	 * When this window was last drawn, for the drag throttle only.
	 *
	 * The animated backdrops next door get this for free: they have a
	 * frame hook, so they can simply do nothing on a client_placed for a
	 * window being dragged and let the per-output tick redraw it.  The
	 * glass has no clock and no frame hook -- it is event-driven,
	 * because between events it is correct -- so skipping the event
	 * would freeze it for the whole drag.  It has to keep drawing and
	 * count the time itself.
	 */
	gint64                   last_us;
} GowlGlassNodes;

struct _GowlModuleLiquidGlass {
	GowlModule      parent_instance;
	GWeakRef        compositor;
	GowlFxGl       *gl;
	gboolean        gl_tried;
	GList          *sources;      /* GowlGlassSource* */
	gboolean        capturing;
	guint64         serial;       /* the last capture taken */
	GowlGlassStyle  style;        /* what the config last said */
	gboolean        style_known;
	guint64         generation;   /* bumped when @style changes */
};

static void glass_effect_init(GowlSceneEffectInterface *iface);
static void glass_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleLiquidGlass, gowl_module_liquid_glass,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, glass_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, glass_shutdown_init))

/* ── Plumbing ────────────────────────────────────────────────────── */

static void
glass_on_node_destroy(struct wl_listener *listener, void *data)
{
	GowlGlassNodes *nodes;

	(void)data;
	nodes = wl_container_of(listener, nodes, node_destroy);
	wl_list_remove(&listener->link);
	nodes->node = NULL;
}

static void
glass_set_node(GowlGlassNodes *nodes, struct wlr_scene_buffer *buf)
{
	nodes->node = buf;
	if (buf == NULL)
		return;
	nodes->node_destroy.notify = glass_on_node_destroy;
	wl_signal_add(&buf->node.events.destroy, &nodes->node_destroy);
}

/* Stop following the node and leave it where it is, in a tree that is
 * about to be destroyed with it in it. */
static void
glass_forget_node(GowlGlassNodes *nodes)
{
	if (nodes == NULL || nodes->node == NULL)
		return;
	wl_list_remove(&nodes->node_destroy.link);
	nodes->node = NULL;
}

static void
glass_nodes_free(gpointer data)
{
	GowlGlassNodes *nodes = data;

	/* A node that went with the client's tree has already cleared its
	 * pointer through its destroy listener, so whatever is still set is
	 * alive and must go by hand.  The listener takes itself off as the
	 * node goes. */
	if (nodes->node != NULL)
		wlr_scene_node_destroy(&nodes->node->node);
	g_clear_pointer(&nodes->swapchain, wlr_swapchain_destroy);
	g_free(nodes);
}

static GowlGlassNodes *
glass_nodes(GowlClient *c, gboolean create)
{
	GowlGlassNodes *nodes = g_object_get_data(G_OBJECT(c),
	                                          GOWL_GLASS_DATA_KEY);

	if (nodes == NULL && create) {
		nodes = g_new0(GowlGlassNodes, 1);
		g_object_set_data_full(G_OBJECT(c), GOWL_GLASS_DATA_KEY, nodes,
		                       glass_nodes_free);
	}
	return nodes;
}

static void
glass_clear_nodes(GowlClient *c)
{
	if (g_object_get_data(G_OBJECT(c), GOWL_GLASS_DATA_KEY) != NULL)
		g_object_set_data(G_OBJECT(c), GOWL_GLASS_DATA_KEY, NULL);
}

static void
glass_ensure_gl(GowlModuleLiquidGlass *mod, GowlCompositor *self)
{
	if (mod->gl != NULL || mod->gl_tried)
		return;
	if (self == NULL || self->renderer == NULL)
		return;

	mod->gl_tried = TRUE;
	mod->gl = gowl_fx_gl_new(self->renderer);
}

static void
glass_source_free(GowlModuleLiquidGlass *mod, GowlGlassSource *src)
{
	if (src == NULL)
		return;
	gowl_fx_texture_drop(mod->gl, &src->sharp);
	gowl_fx_texture_drop(mod->gl, &src->soft);
	g_free(src);
}

static void
glass_drop_sources(GowlModuleLiquidGlass *mod)
{
	GList *l;

	for (l = mod->sources; l != NULL; l = l->next)
		glass_source_free(mod, l->data);
	g_list_free(mod->sources);
	mod->sources = NULL;
}

static GowlGlassSource *
glass_source_for(GowlModuleLiquidGlass *mod, GowlMonitor *m)
{
	GList *l;

	for (l = mod->sources; l != NULL; l = l->next) {
		GowlGlassSource *src = l->data;

		if (src->monitor == m)
			return src;
	}
	return NULL;
}

/* ── The look, out of the config ─────────────────────────────────── */

static gint
glass_shape_value(const gchar *name)
{
	if (g_strcmp0(name, "circle") == 0)
		return 0;
	if (g_strcmp0(name, "lip") == 0)
		return 2;
	return 1;   /* squircle */
}

/*
 * Read the whole look, and say whether it has changed since last time.
 *
 * Every window's render depends on all of it, so the answer is what
 * decides whether a reload redraws them.
 */
static gboolean
glass_read_style(GowlModuleLiquidGlass *mod, GowlConfig *config)
{
	GowlGlassStyle  style;
	GowlColor      *tint;
	const gchar    *spec;

	memset(&style, 0, sizeof(style));
	style.bevel        = gowl_config_get_glass_bevel(config);
	style.thickness    = gowl_config_get_glass_thickness(config);
	style.slope        = gowl_config_get_glass_slope(config);
	style.dispersion   = gowl_config_get_glass_dispersion(config);
	style.rim          = gowl_config_get_glass_rim(config);
	style.shade        = gowl_config_get_glass_shade(config);
	style.edge_width   = gowl_config_get_glass_edge_width(config);
	style.saturation   = gowl_config_get_glass_saturation(config);
	style.clarity      = gowl_config_get_glass_clarity(config);
	style.centre_clarity = gowl_config_get_glass_centre_clarity(config);
	style.lens         = gowl_config_get_glass_lens(config);
	style.sheen        = gowl_config_get_glass_sheen(config);
	style.light        = gowl_config_get_glass_light(config);
	style.brightness   = gowl_config_get_glass_brightness(config);
	style.opacity      = gowl_config_get_glass_opacity(config);
	style.shape        = glass_shape_value(
		gowl_config_get_glass_shape(config));
	style.frost        = gowl_config_get_glass_frost(config);
	style.frost_passes = gowl_config_get_glass_frost_passes(config);

	style.tint[0] = style.tint[1] = style.tint[2] = 1.0;
	spec = gowl_config_get_glass_tint(config);
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

/* ── The wallpaper it refracts ───────────────────────────────────── */

/*
 * Capture the desktop with every window hidden, and keep it twice: as it
 * is, and frosted.
 *
 * Keyed on the tag set as well as the output, because wallpapers are
 * per-tag -- and glass showing the tag you just left, through every
 * translucent window on the tag you arrived at, is exactly the sort of
 * thing nobody notices in review.
 */
static gboolean
glass_build_source(GowlModuleLiquidGlass *mod, GowlCompositor *self,
                   GowlMonitor *m, GowlGlassSource *src)
{
	GowlFxVis *vis;
	gboolean   ok;
	gint       i;

	mod->capturing = TRUE;
	vis = gowl_fx_vis_begin();

	/* Everything except the background layers.  What is left is the
	 * wallpaper, which is what the glass bends. */
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_TILE, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_FLOAT, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_TOP, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_FS, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_OVERLAY, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_BLOCK, FALSE);
	/* And the effect sheets, which the layer sweep cannot reach: they
	 * hang off scene->tree beside the layers, and one holds an opaque
	 * picture of the desktop WITH its windows. */
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
	/* A new capture, so every window's render is redone -- and only
	 * then, which is what the serial is for. */
	src->serial = ++mod->serial;
	return TRUE;
}

static GowlGlassSource *
glass_ensure_source(GowlModuleLiquidGlass *mod, GowlCompositor *self,
                    GowlMonitor *m)
{
	GowlGlassSource *src = glass_source_for(mod, m);
	gboolean         stale;

	if (m->wlr_output == NULL)
		return NULL;

	if (src == NULL) {
		src = g_new0(GowlGlassSource, 1);
		src->monitor = m;
		mod->sources = g_list_prepend(mod->sources, src);
	}

	stale = src->sharp.tex == 0
	        || src->soft.tex == 0
	        || src->tags != m->tagset[m->seltags]
	        || src->width != m->wlr_output->width
	        || src->height != m->wlr_output->height
	        || src->frost != mod->style.frost
	        || src->frost_passes != mod->style.frost_passes;

	if (stale && !glass_build_source(mod, self, m, src))
		return NULL;
	return src->sharp.tex != 0 && src->soft.tex != 0 ? src : NULL;
}

/* ── Per-client rendering ────────────────────────────────────────── */

static gboolean
glass_client_eligible(GowlClient *c)
{
	/* Fullscreen windows cover the wallpaper entirely, so no glass could
	 * be seen; embedder-pinned ones are placed by somebody else and must
	 * not gain a node they did not ask for. */
	return c != NULL && c->scene != NULL && !c->isfullscreen
	       && !gowl_fx_client_is_pinned(c);
}

/*
 * Work from the frame as DRAWN, not from c->geom.
 *
 * A layout that allows overflow (scrolling) leaves c->geom unclipped on
 * purpose, and a floating window dragged half off the monitor is never
 * clipped at all, so c->geom can describe a rectangle largely not on this
 * output.  While an animation moves the window, c->geom is where it is
 * going and c->frame where it is now, which is what the glass has to
 * match.  Before the window is first drawn there is no frame.
 */
static struct wlr_box
glass_drawn_frame(GowlClient *c)
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
glass_corner_radius(GowlCompositor *self, const struct wlr_box *frame,
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

/*
 * A buffer of this window's own to draw the glass into.
 *
 * ARGB, not XRGB: the corners are rounded and the whole thing carries the
 * configured opacity, so it has an alpha channel and the format has to
 * have somewhere to put it.  An allocator that refuses ARGB8888 -- which
 * would be unusual -- leaves the window without glass rather than with a
 * square opaque patch where its corners should be.
 */
static struct wlr_buffer *
glass_acquire_buffer(GowlCompositor *self, GowlGlassNodes *nodes,
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

/* Fill in the shader's parameters from the look, the plan and the
 * window.  Lengths are logical and the plan's scale makes them the
 * buffer's; see gowl-backdrop-plan.h on why that conversion lives in one
 * place. */
static void
glass_fill_params(const GowlGlassStyle *style, const GowlBackdropPlan *plan,
                  gdouble radius, GowlFxGlassParams *out)
{
	gdouble scale = (plan->scale_x + plan->scale_y) * 0.5;
	gdouble rad   = style->light * G_PI / 180.0;

	gowl_fx_glass_params_init(out);
	out->width      = plan->buf_width;
	out->height     = plan->buf_height;
	out->radius     = (gfloat)(radius * scale);
	out->bevel      = (gfloat)(style->bevel * scale);
	out->thickness  = (gfloat)(style->thickness * scale);
	out->slope      = (gfloat)style->slope;
	out->shape      = (gfloat)style->shape;
	out->dispersion = (gfloat)(style->dispersion * scale);
	out->rim        = (gfloat)style->rim;
	out->shade      = (gfloat)style->shade;
	out->edge_width = (gfloat)(style->edge_width * scale);
	out->saturation = (gfloat)style->saturation;
	out->clarity    = (gfloat)style->clarity;
	out->centre_clarity = (gfloat)style->centre_clarity;
	out->lens       = (gfloat)style->lens;
	out->sheen      = (gfloat)style->sheen;
	out->brightness = (gfloat)style->brightness;
	out->alpha      = (gfloat)style->opacity;
	out->tint[0]    = (gfloat)style->tint[0];
	out->tint[1]    = (gfloat)style->tint[1];
	out->tint[2]    = (gfloat)style->tint[2];
	/* 0 degrees is straight above and positive turns clockwise, with
	 * screen y pointing down. */
	out->light[0]   = (gfloat)sin(rad);
	out->light[1]   = (gfloat)-cos(rad);
	/* The wallpaper texture is NOT scaled down with the render, so the
	 * origin is in its own pixels and the plan already has it that way. */
	out->src_origin[0] = (gfloat)plan->origin_x;
	out->src_origin[1] = (gfloat)plan->origin_y;
	/* And how far one buffer pixel reaches into it.  A render scaled
	 * down while the window moves still covers the WHOLE window; without
	 * this it covered a quarter of it, magnified. */
	out->src_scale = (gfloat)plan->src_scale;
}

/*
 * Take the node down and give the buffers back.
 *
 * The swapchain goes with it, which is not tidiness: it is three buffers
 * the size of the window, and the reason this is called is usually that
 * the glass has been switched off -- by `window-backdrop', by a window
 * going opaque, by a window rule.  Keeping a maximised window's worth of
 * them (sixty megabytes on a 2880x1920 screen, for each such window) for
 * a look nobody asked for would be a leak with a different name.  The
 * next render allocates again.
 */
static void
glass_drop_node(GowlGlassNodes *nodes)
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
glass_update_client(GowlModuleLiquidGlass *mod, GowlCompositor *self,
                    GowlClient *c, gboolean settled)
{
	GowlGlassNodes  *nodes;
	GowlGlassSource *src;
	GowlBackdropPlan    plan;
	struct wlr_box   frame;
	gint             divisor;
	gboolean         moving;
	gdouble          radius;

	glass_ensure_gl(mod, self);
	if (mod->gl == NULL || self->config == NULL || self->locked)
		return;

	glass_read_style(mod, self->config);

	if (!glass_client_eligible(c)) {
		glass_clear_nodes(c);
		return;
	}

	/*
	 * Not our style, not our window, or nothing behind it: take the node
	 * down.  This is the path Super+Shift+" takes away from the glass,
	 * and it has to be complete -- a node left enabled behind a window
	 * that has since gone opaque is a rectangle of stale wallpaper.
	 */
	if (gowl_config_get_backdrop_style(self->config) != GOWL_BACKDROP_GLASS
	    || (c->rule_flags & GOWL_CLIENT_RULE_NO_BLUR)
	    || c->alpha >= GOWL_GLASS_MIN_TRANSPARENCY
	    || c->mon == NULL) {
		nodes = glass_nodes(c, FALSE);
		if (nodes != NULL)
			glass_drop_node(nodes);
		return;
	}

	src = glass_ensure_source(mod, self, c->mon);
	if (src == NULL)
		return;

	frame   = glass_drawn_frame(c);
	/*
	 * A DRAG COUNTS AS MOVING, and `settled' does not say so.
	 *
	 * settled is FALSE while a provider is animating the window and TRUE
	 * otherwise -- and during an interactive move or resize nothing is
	 * animating it, the pointer is moving it directly, so it has been
	 * TRUE throughout every drag since this divisor was written.  The
	 * one case the halved resolution was most obviously for was the one
	 * case it never applied to.
	 */
	moving  = !settled || gowl_compositor_client_is_grabbed(self, c);
	divisor = moving ? GOWL_GLASS_MOVING_DIVISOR : 1;
	if (!gowl_backdrop_plan(&frame, &c->mon->m, src->width, src->height,
	                     divisor, &plan)) {
		nodes = glass_nodes(c, FALSE);
		if (nodes != NULL && nodes->node != NULL)
			wlr_scene_node_set_enabled(&nodes->node->node, FALSE);
		return;
	}

	nodes  = glass_nodes(c, TRUE);
	radius = glass_corner_radius(self, &frame, c->bw);

	/*
	 * ONCE PER FRAME WHILE DRAGGING, NOT ONCE PER MOTION EVENT.
	 *
	 * This function runs from client_placed, which during a drag runs
	 * once per pointer motion -- and a mouse reports several times
	 * faster than a screen refreshes, so most of those renders are
	 * thrown away before anyone sees them.  The render is not even the
	 * expensive half: a resize changes the buffer size on every one of
	 * them, and a changed size throws the swapchain away and allocates
	 * a new one.
	 *
	 * Eight milliseconds is a little under two frames at 240 Hz, so the
	 * cap is above any refresh rate anybody has and below every mouse
	 * report rate -- which is the whole point.  It applies ONLY while
	 * the window is being dragged: everywhere else this is event-driven
	 * because between events it is correct, and a throttle there would
	 * be a backdrop that lags its window for no reason.
	 */
	if (moving && nodes->have && nodes->node != NULL) {
		gint64 now = g_get_monotonic_time();

		if (now - nodes->last_us < 8000) {
			/* Keep tracking the window; just do not draw again.  The
			 * source box stays the one the buffer was drawn with --
			 * the new plan's is in the new buffer's coordinates, and
			 * a source box past the end of the buffer wlroots has is
			 * an abort rather than a glitch. */
			wlr_scene_node_set_enabled(&nodes->node->node, TRUE);
			wlr_scene_buffer_set_source_box(nodes->node, &nodes->plan.src);
			wlr_scene_buffer_set_dest_size(nodes->node, plan.vis.width,
			                               plan.vis.height);
			wlr_scene_node_set_position(&nodes->node->node,
			                            plan.vis.x - frame.x,
			                            plan.vis.y - frame.y);
			wlr_scene_node_lower_to_bottom(&nodes->node->node);
			return;
		}
	}

	if (gowl_backdrop_render_stale(nodes->have && nodes->node != NULL,
	                            &nodes->plan, &plan,
	                            nodes->serial, src->serial,
	                            nodes->generation, mod->generation)
	    || nodes->radius != radius) {
		struct wlr_buffer *buf;
		GowlFxPass        *pass;
		GowlFxGlassParams  params;
		gfloat             clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		gboolean           ok;

		buf = glass_acquire_buffer(self, nodes, plan.buf_width,
		                           plan.buf_height);
		if (buf == NULL)
			return;

		pass = gowl_fx_pass_begin(mod->gl, buf);
		if (pass == NULL) {
			wlr_buffer_unlock(buf);
			return;
		}
		glass_fill_params(&mod->style, &plan, radius, &params);
		gowl_fx_pass_clear(pass, clear);
		ok = gowl_fx_pass_glass(pass, &src->soft, &src->sharp, &params);
		gowl_fx_pass_end(pass);

		if (!ok) {
			wlr_buffer_unlock(buf);
			return;
		}

		if (nodes->node == NULL)
			glass_set_node(nodes, wlr_scene_buffer_create(c->scene, buf));
		else
			wlr_scene_buffer_set_buffer(nodes->node, buf);
		wlr_buffer_unlock(buf);

		if (nodes->node == NULL)
			return;

		nodes->plan       = plan;
		nodes->have       = TRUE;
		nodes->serial     = src->serial;
		nodes->generation = mod->generation;
		nodes->radius     = radius;
		nodes->last_us    = g_get_monotonic_time();
	}

	if (nodes->node == NULL)
		return;

	wlr_scene_node_set_enabled(&nodes->node->node, TRUE);
	wlr_scene_buffer_set_source_box(nodes->node, &plan.src);
	wlr_scene_buffer_set_dest_size(nodes->node, plan.vis.width,
	                               plan.vis.height);
	/* The node is a child of c->scene, which sits at the frame origin. */
	wlr_scene_node_set_position(&nodes->node->node,
	                            plan.vis.x - frame.x, plan.vis.y - frame.y);
	/* Under everything the client draws, and under the blur module's
	 * shadow if it made one -- which is outside the window's rectangle
	 * anyway, so the order between them is only ever visible at the
	 * corners. */
	wlr_scene_node_lower_to_bottom(&nodes->node->node);
}

/* ── Hooks ───────────────────────────────────────────────────────── */

/*
 * Never claims an event, and has no use for GEOMETRY: that stops at the
 * first provider to claim it, which for every tile is the animation
 * module sorted ahead of this one.  Where a window is drawn arrives
 * through client_placed below, which every provider gets.
 */
static gboolean
glass_client_event(GowlSceneEffect *effect, GowlCompositor *self,
                   GowlClient *c, GowlSceneEffectEvent event,
                   const struct wlr_box *previous, gboolean interactive)
{
	GowlModuleLiquidGlass *mod = GOWL_MODULE_LIQUID_GLASS(effect);

	g_weak_ref_set(&mod->compositor, self);

	if (c == NULL || mod->capturing)
		return FALSE;

	switch (event) {
	case GOWL_SCENE_EFFECT_UNMAP:
		/* The tree the node hangs off is destroyed straight after this
		 * and takes it along: stop following it and leave it to that. */
		if (g_object_get_data(G_OBJECT(c), GOWL_GLASS_DATA_KEY) != NULL) {
			glass_forget_node(glass_nodes(c, FALSE));
			glass_clear_nodes(c);
		}
		break;
	case GOWL_SCENE_EFFECT_DESTROY:
		/*
		 * Two different things arrive as DESTROY.  A window that is
		 * gone: its tree went at unmap with the node in it, and the
		 * destroy listener has cleared the pointer.  And a window the
		 * compositor is taking over as an overlay, showing in a panel or
		 * giving back, whose tree STAYS: forgetting the node then would
		 * leave one at the old size hanging off the window, and another
		 * on every trip into the scratchpad and back.
		 */
		glass_clear_nodes(c);
		break;
	case GOWL_SCENE_EFFECT_REVEAL:
	case GOWL_SCENE_EFFECT_RELEASE:
		glass_update_client(mod, self, c, !gowl_effects_has_geometry(c));
		break;
	default:
		break;
	}
	return FALSE;
}

static void
glass_client_placed(GowlSceneEffect *effect, GowlCompositor *self,
                    GowlClient *c, gboolean settled)
{
	GowlModuleLiquidGlass *mod = GOWL_MODULE_LIQUID_GLASS(effect);

	g_weak_ref_set(&mod->compositor, self);

	if (c == NULL || mod->capturing)
		return;
	glass_update_client(mod, self, c, settled);
}

static void
glass_alpha_changed(GowlSceneEffect *effect, GowlClient *c, gfloat alpha)
{
	GowlModuleLiquidGlass *mod = GOWL_MODULE_LIQUID_GLASS(effect);
	GowlCompositor        *self = c != NULL ? c->compositor : NULL;

	/* A window becoming translucent is exactly when it needs glass, and
	 * becoming opaque is when it should lose it. */
	if (self != NULL)
		glass_update_client(mod, self, c, !gowl_effects_has_geometry(c));
}

static void
glass_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                      GowlMonitor *m)
{
	GowlModuleLiquidGlass *mod = GOWL_MODULE_LIQUID_GLASS(effect);
	GowlGlassSource       *src = glass_source_for(mod, m);

	if (src != NULL) {
		mod->sources = g_list_remove(mod->sources, src);
		glass_source_free(mod, src);
	}
}

static void
glass_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	GowlModuleLiquidGlass *mod = GOWL_MODULE_LIQUID_GLASS(effect);
	GList *l;

	/* Drop the nodes while the scene is still alive, then the textures,
	 * then GL -- the reverse of how they were made. */
	if (self != NULL) {
		for (l = self->clients; l != NULL; l = l->next)
			glass_clear_nodes(l->data);
	}
	glass_drop_sources(mod);
	g_clear_pointer(&mod->gl, gowl_fx_gl_free);
	mod->gl_tried = FALSE;
	g_weak_ref_set(&mod->compositor, NULL);
}

static void
glass_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_event    = glass_client_event;
	iface->alpha_changed   = glass_alpha_changed;
	iface->monitor_removed = glass_monitor_removed;
	iface->finish          = glass_finish;
	iface->client_placed   = glass_client_placed;
}

static void
glass_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	glass_finish(GOWL_SCENE_EFFECT(handler), compositor);
}

static void
glass_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = glass_shutdown;
}

/* ── Module ──────────────────────────────────────────────────────── */

/*
 * The same priority as the blur, and after the animation module for the
 * hooks both hear: by the time this reads the frame, the window is where
 * the animation put it.  Priority is NOT how the glass follows a window
 * and cannot be -- a CONSUMABLE hook stops at the first provider to claim
 * it, and the animation module claims GEOMETRY for every tile.  The glass
 * follows client_placed, which is BROADCAST.
 */
#define GOWL_GLASS_PRIORITY (10)

static gboolean
glass_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_GLASS_PRIORITY);
	return TRUE;
}

static void
glass_deactivate(GowlModule *base)
{
	GowlModuleLiquidGlass *mod = GOWL_MODULE_LIQUID_GLASS(base);
	GowlCompositor        *self = g_weak_ref_get(&mod->compositor);

	if (self != NULL) {
		glass_finish(GOWL_SCENE_EFFECT(mod), self);
		g_object_unref(self);
	} else {
		glass_drop_sources(mod);
		g_clear_pointer(&mod->gl, gowl_fx_gl_free);
		mod->gl_tried = FALSE;
	}
}

static const gchar *glass_name(GowlModule *m)    { return "liquidglass"; }
static const gchar *glass_version(GowlModule *m) { return "0.1.0"; }
static const gchar *glass_description(GowlModule *m)
{
	return "Refracts the wallpaper through translucent windows";
}

static void
glass_finalize(GObject *object)
{
	GowlModuleLiquidGlass *mod = GOWL_MODULE_LIQUID_GLASS(object);

	glass_deactivate(GOWL_MODULE(object));
	g_weak_ref_clear(&mod->compositor);
	G_OBJECT_CLASS(gowl_module_liquid_glass_parent_class)->finalize(object);
}

static void
gowl_module_liquid_glass_class_init(GowlModuleLiquidGlassClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = glass_activate;
	mod->deactivate      = glass_deactivate;
	mod->get_name        = glass_name;
	mod->get_description = glass_description;
	mod->get_version     = glass_version;
	G_OBJECT_CLASS(klass)->finalize = glass_finalize;
}

static void
gowl_module_liquid_glass_init(GowlModuleLiquidGlass *mod)
{
	g_weak_ref_init(&mod->compositor, NULL);
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_GLASS_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_LIQUID_GLASS;
}
