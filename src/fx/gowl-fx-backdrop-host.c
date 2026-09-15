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
 * gowl-fx-backdrop-host.c -- the half of an animated backdrop that is
 * not optics.  See the header for why it exists.
 *
 * THE THREE THINGS AN ANIMATED BACKDROP NEEDS THAT A SETTLED ONE DOES
 * NOT, all of them here:
 *
 *   A CLOCK, kept in doubles and handed to the shader already wrapped.
 *   ONE clock for the whole desktop, not one per window: two windows
 *   side by side are two panes in the same weather, and giving each its
 *   own makes them visibly disagree at the seam.  It advances against
 *   the WALL rather than once per output -- with two monitors the frame
 *   hook runs twice a frame, and advancing per call runs the effect at
 *   double speed on a two-screen desk.
 *
 *   A FRAME RATE, which is a throttle and not a target, kept per
 *   OUTPUT so two screens at different refresh rates are each held
 *   against their own clock.
 *
 *   AN HONEST ADMISSION ABOUT POWER.  Returning TRUE from the frame hook
 *   is what keeps frames coming, and that holds the output awake.  It
 *   stops the moment there is nothing to draw -- no translucent window
 *   here, the session locked, the style changed, the module shut down --
 *   but while there IS something, a screen showing this is a screen
 *   that is rendering.  There is no version of an animated effect that
 *   is not.
 *
 * WHAT IT COSTS.  Per output, once per tag switch: one capture of the
 * desktop with the windows hidden, and one blur of it.  Then one
 * fragment pass per eligible window per tick, at 1/scale of the
 * window's size.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-fx"

#include "gowl-fx-backdrop-host.h"

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-client.h"
#include "core/gowl-effects.h"
#include "config/gowl-config.h"
#include "module/gowl-module-manager.h"
#include "interfaces/gowl-scene-effect.h"
#include "interfaces/gowl-client-decorator.h"

#include <drm_fourcc.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#include <string.h>

/* Above this a window is opaque enough that nothing behind it could be
 * seen.  The same threshold every other backdrop uses. */
#define GOWL_BACKDROP_HOST_MIN_TRANSPARENCY (0.985)

/* One output's wallpaper, sharp and blurred, as textures a shader reads. */
typedef struct {
	GowlMonitor   *monitor;   /* unowned */
	GowlFxTexture  sharp;
	GowlFxTexture  soft;
	guint32        tags;
	gint           width, height;
	guint64        serial;
	gint           frost;
	gint           frost_passes;
	gint64         next_draw_us;
} GowlBackdropHostSource;

/* One window's node, hung off the client so it lives and dies with it. */
typedef struct {
	struct wlr_scene_buffer *node;
	struct wl_listener       node_destroy;
	struct wlr_swapchain    *swapchain;
	GowlBackdropPlan         plan;
	gboolean                 have;
	guint64                  serial;      /* which wallpaper capture */
	guint64                  generation;  /* which settings */
	gdouble                  radius;
} GowlBackdropHostNodes;

struct _GowlBackdropHost {
	const GowlBackdropHostVTable *vt;
	gchar        *data_key;
	GowlFxGl     *gl;
	gboolean      gl_tried;
	GList        *sources;      /* GowlBackdropHostSource* */
	gboolean      capturing;
	guint64       serial;
	gpointer      style;        /* vt->style_size bytes */
	gboolean      style_known;
	guint64       generation;
	gpointer      clock;        /* vt->clock_size bytes */
	gint64        last_tick_us;
};

/* ── Plumbing ────────────────────────────────────────────────────── */

static const GowlBackdropHostCommon *
host_common(const GowlBackdropHost *host)
{
	/* The contract in the header: a style struct begins with one. */
	return (const GowlBackdropHostCommon *)host->style;
}

static void
host_on_node_destroy(struct wl_listener *listener, void *data)
{
	GowlBackdropHostNodes *nodes;

	(void)data;
	nodes = wl_container_of(listener, nodes, node_destroy);
	wl_list_remove(&listener->link);
	nodes->node = NULL;
}

static void
host_set_node(GowlBackdropHostNodes *nodes, struct wlr_scene_buffer *buf)
{
	nodes->node = buf;
	if (buf == NULL)
		return;
	nodes->node_destroy.notify = host_on_node_destroy;
	wl_signal_add(&buf->node.events.destroy, &nodes->node_destroy);
}

static void
host_forget_node(GowlBackdropHostNodes *nodes)
{
	if (nodes == NULL || nodes->node == NULL)
		return;
	wl_list_remove(&nodes->node_destroy.link);
	nodes->node = NULL;
}

static void
host_nodes_free(gpointer data)
{
	GowlBackdropHostNodes *nodes = data;

	if (nodes->node != NULL)
		wlr_scene_node_destroy(&nodes->node->node);
	g_clear_pointer(&nodes->swapchain, wlr_swapchain_destroy);
	g_free(nodes);
}

static GowlBackdropHostNodes *
host_nodes(GowlBackdropHost *host, GowlClient *c, gboolean create)
{
	GowlBackdropHostNodes *nodes = g_object_get_data(G_OBJECT(c),
	                                                 host->data_key);

	if (nodes == NULL && create) {
		nodes = g_new0(GowlBackdropHostNodes, 1);
		g_object_set_data_full(G_OBJECT(c), host->data_key, nodes,
		                       host_nodes_free);
	}
	return nodes;
}

static void
host_clear_nodes(GowlBackdropHost *host, GowlClient *c)
{
	if (g_object_get_data(G_OBJECT(c), host->data_key) != NULL)
		g_object_set_data(G_OBJECT(c), host->data_key, NULL);
}

/* Take the node down and give the buffers back.  The swapchain goes with
 * it: it is several buffers the size of the window, and the reason this
 * is called is usually that the effect has been switched off. */
static void
host_drop_node(GowlBackdropHostNodes *nodes)
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
host_ensure_gl(GowlBackdropHost *host, GowlCompositor *self)
{
	if (host->gl != NULL || host->gl_tried)
		return;
	if (self == NULL || self->renderer == NULL)
		return;

	host->gl_tried = TRUE;
	host->gl = gowl_fx_gl_new(self->renderer);
}

static void
host_source_free(GowlBackdropHost *host, GowlBackdropHostSource *src)
{
	if (src == NULL)
		return;
	gowl_fx_texture_drop(host->gl, &src->sharp);
	gowl_fx_texture_drop(host->gl, &src->soft);
	g_free(src);
}

static void
host_drop_sources(GowlBackdropHost *host)
{
	GList *l;

	for (l = host->sources; l != NULL; l = l->next)
		host_source_free(host, l->data);
	g_list_free(host->sources);
	host->sources = NULL;
}

static GowlBackdropHostSource *
host_source_for(GowlBackdropHost *host, GowlMonitor *m)
{
	GList *l;

	for (l = host->sources; l != NULL; l = l->next) {
		GowlBackdropHostSource *src = l->data;

		if (src->monitor == m)
			return src;
	}
	return NULL;
}

/* ── The look, out of the config ─────────────────────────────────── */

static gboolean
host_read_style(GowlBackdropHost *host, GowlConfig *config)
{
	g_autofree gpointer fresh = g_malloc0(host->vt->style_size);

	host->vt->read_style(fresh, config);

	if (host->style_known
	    && memcmp(fresh, host->style, host->vt->style_size) == 0)
		return FALSE;

	memcpy(host->style, fresh, host->vt->style_size);
	host->style_known = TRUE;
	host->generation++;
	return TRUE;
}

/* ── The wallpaper it draws over ─────────────────────────────────── */

static gboolean
host_build_source(GowlBackdropHost *host, GowlCompositor *self,
                  GowlMonitor *m, GowlBackdropHostSource *src)
{
	const GowlBackdropHostCommon *cm = host_common(host);
	GowlFxVis *vis;
	gboolean   ok;
	gint       i;

	host->capturing = TRUE;
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

	ok = gowl_fx_capture(host->gl, self, m, &src->sharp, 1);

	gowl_fx_vis_restore(vis);
	host->capturing = FALSE;

	if (!ok)
		return FALSE;

	if (!gowl_fx_texture_blur(host->gl, &src->soft, &src->sharp,
	                          CLAMP(cm->frost, 1, 8),
	                          CLAMP(cm->frost_passes, 1, 6)))
		return FALSE;

	src->tags         = m->tagset[m->seltags];
	src->width        = src->sharp.width;
	src->height       = src->sharp.height;
	src->frost        = cm->frost;
	src->frost_passes = cm->frost_passes;
	src->serial       = ++host->serial;
	return TRUE;
}

static GowlBackdropHostSource *
host_ensure_source(GowlBackdropHost *host, GowlCompositor *self,
                   GowlMonitor *m)
{
	const GowlBackdropHostCommon *cm = host_common(host);
	GowlBackdropHostSource *src = host_source_for(host, m);
	gboolean stale;

	if (m->wlr_output == NULL)
		return NULL;

	if (src == NULL) {
		src = g_new0(GowlBackdropHostSource, 1);
		src->monitor = m;
		host->sources = g_list_prepend(host->sources, src);
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
	        || src->frost != cm->frost
	        || src->frost_passes != cm->frost_passes;

	if (stale && !host_build_source(host, self, m, src))
		return NULL;
	return src->sharp.tex != 0 && src->soft.tex != 0 ? src : NULL;
}

/* ── Per-client rendering ────────────────────────────────────────── */

static gboolean
host_client_eligible(GowlClient *c)
{
	return c != NULL && c->scene != NULL && !c->isfullscreen
	       && !gowl_fx_client_is_pinned(c);
}

static gboolean
host_client_wants(GowlBackdropHost *host, GowlCompositor *self, GowlClient *c)
{
	return host_client_eligible(c)
	       && gowl_config_get_backdrop_style(self->config) == host->vt->style
	       && !(c->rule_flags & GOWL_CLIENT_RULE_NO_BLUR)
	       && c->alpha < GOWL_BACKDROP_HOST_MIN_TRANSPARENCY
	       && c->mon != NULL;
}

static struct wlr_box
host_drawn_frame(GowlClient *c)
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
 * border arithmetic it does.  Taking the decorator's number raw leaves a
 * transparent nick inside each corner on a bordered window.
 */
static gdouble
host_corner_radius(GowlCompositor *self, const struct wlr_box *frame,
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
host_acquire_buffer(GowlCompositor *self, GowlBackdropHostNodes *nodes,
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

/*
 * Draw one window's backdrop.
 *
 * Returns TRUE when there is something on this window, whether or not
 * this call managed to redraw it: the caller uses that to decide whether
 * the output still needs frames, and a render that failed is a reason to
 * try again next tick rather than to stop.
 */
static gboolean
host_update_client(GowlBackdropHost *host, GowlCompositor *self,
                   GowlClient *c, gboolean redraw)
{
	const GowlBackdropHostCommon *cm;
	GowlBackdropHostNodes  *nodes;
	GowlBackdropHostSource *src;
	GowlBackdropPlan        plan;
	struct wlr_box          frame;
	gdouble                 radius;

	host_ensure_gl(host, self);
	if (host->gl == NULL || self->config == NULL || self->locked)
		return FALSE;

	host_read_style(host, self->config);
	cm = host_common(host);

	if (!host_client_eligible(c)) {
		host_clear_nodes(host, c);
		return FALSE;
	}

	if (!host_client_wants(host, self, c)) {
		nodes = host_nodes(host, c, FALSE);
		if (nodes != NULL)
			host_drop_node(nodes);
		return FALSE;
	}

	src = host_ensure_source(host, self, c->mon);
	if (src == NULL)
		return TRUE;   /* wanted, just not drawable this instant */

	frame = host_drawn_frame(c);
	if (!gowl_backdrop_plan(&frame, &c->mon->m, src->width, src->height,
	                        CLAMP(cm->scale, 1, 4), &plan)) {
		nodes = host_nodes(host, c, FALSE);
		if (nodes != NULL && nodes->node != NULL)
			wlr_scene_node_set_enabled(&nodes->node->node, FALSE);
		return FALSE;
	}

	nodes  = host_nodes(host, c, TRUE);
	radius = host_corner_radius(self, &frame, c->bw);

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
	                                  nodes->generation, host->generation)) {
		struct wlr_buffer *buf;
		GowlFxPass        *pass;
		gfloat             clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		gboolean           ok;

		buf = host_acquire_buffer(self, nodes, plan.buf_width,
		                          plan.buf_height);
		if (buf == NULL)
			return TRUE;

		pass = gowl_fx_pass_begin(host->gl, buf);
		if (pass == NULL) {
			wlr_buffer_unlock(buf);
			return TRUE;
		}
		gowl_fx_pass_clear(pass, clear);
		ok = host->vt->draw(pass, &src->soft, &src->sharp,
		                    host->style, host->clock, &plan, radius,
		                    gowl_client_get_id(c));
		gowl_fx_pass_end(pass);

		if (!ok) {
			wlr_buffer_unlock(buf);
			/* The shader would not build.  Nothing will come of trying
			 * again, so let the output go back to sleep. */
			return FALSE;
		}

		if (nodes->node == NULL)
			host_set_node(nodes, wlr_scene_buffer_create(c->scene, buf));
		else
			wlr_scene_buffer_set_buffer(nodes->node, buf);
		wlr_buffer_unlock(buf);

		if (nodes->node == NULL)
			return TRUE;

		nodes->plan       = plan;
		nodes->have       = TRUE;
		nodes->serial     = src->serial;
		nodes->generation = host->generation;
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

/* ── The hooks a module forwards ─────────────────────────────────── */

GowlBackdropHost *
gowl_backdrop_host_new(const GowlBackdropHostVTable *vt)
{
	GowlBackdropHost *host;

	g_return_val_if_fail(vt != NULL, NULL);
	g_return_val_if_fail(vt->name != NULL, NULL);
	g_return_val_if_fail(vt->style_size >= sizeof(GowlBackdropHostCommon),
	                     NULL);
	g_return_val_if_fail(vt->read_style != NULL && vt->advance != NULL
	                     && vt->draw != NULL, NULL);

	host = g_new0(GowlBackdropHost, 1);
	host->vt = vt;
	host->data_key = g_strdup_printf("gowl-%s-node", vt->name);
	host->style = g_malloc0(vt->style_size);
	host->clock = vt->clock_size > 0 ? g_malloc0(vt->clock_size) : NULL;
	return host;
}

void
gowl_backdrop_host_free(GowlBackdropHost *host)
{
	if (host == NULL)
		return;
	host_drop_sources(host);
	g_clear_pointer(&host->gl, gowl_fx_gl_free);
	g_free(host->style);
	g_free(host->clock);
	g_free(host->data_key);
	g_free(host);
}

gboolean
gowl_backdrop_host_frame(GowlBackdropHost *host, GowlCompositor *self,
                         GowlMonitor *m, gint64 now_us)
{
	GowlBackdropHostSource *src;
	GList    *l;
	gboolean  redraw, live = FALSE;
	gint      fps;

	if (host == NULL || self == NULL || self->config == NULL)
		return FALSE;
	if (self->locked || host->capturing)
		return FALSE;
	if (gowl_config_get_backdrop_style(self->config) != host->vt->style)
		return FALSE;

	host_ensure_gl(host, self);
	if (host->gl == NULL)
		return FALSE;
	host_read_style(host, self->config);
	fps = host_common(host)->fps;

	/* The clock advances against the WALL, not once per output: see the
	 * note at the top. */
	if (host->last_tick_us != 0 && now_us > host->last_tick_us) {
		host->vt->advance(host->clock, host->style,
		                  (gdouble)(now_us - host->last_tick_us) / 1e6);
	}
	if (now_us > host->last_tick_us)
		host->last_tick_us = now_us;

	src = host_source_for(host, m);
	redraw = TRUE;
	if (src != NULL && fps > 0 && now_us < src->next_draw_us)
		redraw = FALSE;

	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if (c->mon != m)
			continue;
		if (host_update_client(host, self, c, redraw))
			live = TRUE;
	}

	/* Set AFTER the loop: host_update_client() may have built the source
	 * on its first pass, and the throttle belongs to the source. */
	if (redraw) {
		src = host_source_for(host, m);
		if (src != NULL) {
			src->next_draw_us = fps > 0
				? now_us + G_USEC_PER_SEC / fps
				: now_us;
		}
	}
	return live;
}

gboolean
gowl_backdrop_host_client_event(GowlBackdropHost *host, GowlCompositor *self,
                                GowlClient *c, GowlSceneEffectEvent event)
{
	if (host == NULL || c == NULL || host->capturing)
		return FALSE;

	switch (event) {
	case GOWL_SCENE_EFFECT_UNMAP:
		if (g_object_get_data(G_OBJECT(c), host->data_key) != NULL) {
			host_forget_node(host_nodes(host, c, FALSE));
			host_clear_nodes(host, c);
		}
		break;
	case GOWL_SCENE_EFFECT_DESTROY:
		host_clear_nodes(host, c);
		break;
	case GOWL_SCENE_EFFECT_REVEAL:
	case GOWL_SCENE_EFFECT_RELEASE:
		if (self != NULL)
			host_update_client(host, self, c, TRUE);
		break;
	default:
		break;
	}
	return FALSE;
}

void
gowl_backdrop_host_client_placed(GowlBackdropHost *host, GowlCompositor *self,
                                 GowlClient *c)
{
	if (host == NULL || self == NULL || c == NULL || host->capturing)
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
	 * by the throttle.
	 */
	if (gowl_compositor_client_is_grabbed(self, c))
		return;
	host_update_client(host, self, c, TRUE);
}

void
gowl_backdrop_host_alpha_changed(GowlBackdropHost *host, GowlClient *c)
{
	GowlCompositor *self = c != NULL ? c->compositor : NULL;

	if (host != NULL && self != NULL)
		host_update_client(host, self, c, TRUE);
}

void
gowl_backdrop_host_monitor_removed(GowlBackdropHost *host,
                                   GowlCompositor *self, GowlMonitor *m)
{
	GowlBackdropHostSource *src;

	if (host == NULL)
		return;
	src = host_source_for(host, m);
	if (src != NULL) {
		host->sources = g_list_remove(host->sources, src);
		host_source_free(host, src);
	}
}

void
gowl_backdrop_host_finish(GowlBackdropHost *host, GowlCompositor *self)
{
	GList *l;

	if (host == NULL)
		return;

	if (self != NULL) {
		for (l = self->clients; l != NULL; l = l->next)
			host_clear_nodes(host, l->data);
	}
	host_drop_sources(host);
	g_clear_pointer(&host->gl, gowl_fx_gl_free);
	host->gl_tried = FALSE;
	/* The clock is wall time, and the wall kept going while the renderer
	 * was away.  Forget where it was rather than hand the next advance a
	 * gap of however long the GPU reset took. */
	host->last_tick_us = 0;
	if (host->clock != NULL)
		memset(host->clock, 0, host->vt->clock_size);
}
