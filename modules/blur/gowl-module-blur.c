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
 * Frosted glass and drop shadows.
 *
 * Neither of these takes over the output the way the cube or the overview
 * do.  They add two nodes per window INSIDE that window's own scene tree
 * -- a blurred backdrop and a shadow, both below everything else the
 * client owns -- and otherwise leave the compositor alone.  That is why
 * this module has no sheet and no frame-by-frame drawing: once the nodes
 * are in place wlroots composites them, and the module only wakes up when
 * a window moves, resizes, or changes opacity.
 *
 * WHAT THE BLUR ACTUALLY BLURS, AND WHY IT MATTERS.
 *
 * It blurs the WALLPAPER, once, and shows a crop of that behind each
 * translucent window.  It does not blur other windows.  The honest
 * alternative -- capturing what is behind each translucent window
 * individually -- costs one full scene render per such window per frame,
 * which is three or four extra renders at sixty hertz for a look.  The
 * wallpaper approach costs one capture and one blur for as long as the
 * wallpaper stays put, and is indistinguishable in the common case of a
 * translucent terminal over the desktop.  Where it differs is a
 * translucent window over another window: you see blurred wallpaper
 * rather than a blurred version of the window below.  Documented, not
 * hidden.
 *
 * The shadows are analytic (gowl-blur-shadow.c): a blurred rectangle
 * evaluated in closed form rather than drawn and blurred, so they cost
 * one CPU pass per resize and nothing per frame.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-blur"

#include "gowl-blur-shadow.h"

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-client.h"
#include "core/gowl-frame-sink.h"
#include "config/gowl-config.h"
#include "boxed/gowl-color.h"
#include "fx/gowl-fx.h"
#include "interfaces/gowl-scene-effect.h"
#include "interfaces/gowl-shutdown-handler.h"

#include <math.h>
#include <string.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#include <gmodule.h>

/* Below this a window is opaque enough that a blurred backdrop would
 * never show through it. */
#define GOWL_BLUR_MIN_TRANSPARENCY 0.985

#define GOWL_TYPE_MODULE_BLUR (gowl_module_blur_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleBlur, gowl_module_blur, GOWL, MODULE_BLUR,
                     GowlModule)

/**
 * GowlBlurBackdrop:
 *
 * One output's blurred wallpaper, as a buffer the scene can crop from.
 *
 * A buffer rather than a texture because the consumers are scene nodes,
 * not a shader: each translucent window shows a different rectangle of
 * the same image, which is exactly what wlr_scene_buffer's source box is
 * for.
 */
typedef struct {
	GowlMonitor       *monitor;      /* unowned */
	struct wlr_buffer *buffer;       /* locked */
	guint32            tags;         /* what was showing when it was made */
	gint               width, height;
} GowlBlurBackdrop;

struct _GowlModuleBlur {
	GowlModule  parent_instance;
	GWeakRef    compositor;
	GowlFxGl   *gl;
	gboolean    gl_tried;
	GList      *backdrops;   /* GowlBlurBackdrop* */
	gboolean    capturing;
};

/* Per-client decoration, hung off the client so it lives and dies with
 * it and needs no separate bookkeeping. */
#define GOWL_BLUR_DATA_KEY "gowl-blur-nodes"

typedef struct {
	struct wlr_scene_buffer *shadow;
	struct wlr_scene_buffer *backdrop;
	/* What the current shadow was drawn for, so it is only redrawn when
	 * something it depends on has actually changed. */
	gint    shadow_w, shadow_h, shadow_radius;
	gdouble shadow_opacity;
} GowlBlurNodes;

static void blur_effect_init(GowlSceneEffectInterface *iface);
static void blur_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleBlur, gowl_module_blur, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, blur_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, blur_shutdown_init))

/* ── Plumbing ────────────────────────────────────────────────────── */

static void
blur_nodes_free(gpointer data)
{
	GowlBlurNodes *nodes = data;

	/*
	 * The nodes are children of the client's own scene tree, so by the
	 * time the client is destroyed wlroots has already taken them with
	 * it.  This only runs while the client is alive -- when the module is
	 * disabled -- so the nodes still exist and must go by hand.
	 */
	if (nodes->shadow != NULL)
		wlr_scene_node_destroy(&nodes->shadow->node);
	if (nodes->backdrop != NULL)
		wlr_scene_node_destroy(&nodes->backdrop->node);
	g_free(nodes);
}

static GowlBlurNodes *
blur_nodes(GowlClient *c, gboolean create)
{
	GowlBlurNodes *nodes = g_object_get_data(G_OBJECT(c), GOWL_BLUR_DATA_KEY);

	if (nodes == NULL && create) {
		nodes = g_new0(GowlBlurNodes, 1);
		g_object_set_data_full(G_OBJECT(c), GOWL_BLUR_DATA_KEY, nodes,
		                       blur_nodes_free);
	}
	return nodes;
}

static void
blur_clear_nodes(GowlClient *c)
{
	if (g_object_get_data(G_OBJECT(c), GOWL_BLUR_DATA_KEY) != NULL)
		g_object_set_data(G_OBJECT(c), GOWL_BLUR_DATA_KEY, NULL);
}

static void
blur_ensure_gl(GowlModuleBlur *mod, GowlCompositor *self)
{
	if (mod->gl != NULL || mod->gl_tried)
		return;
	if (self == NULL || self->renderer == NULL)
		return;

	mod->gl_tried = TRUE;
	mod->gl = gowl_fx_gl_new(self->renderer);
}

static void
blur_backdrop_free(GowlBlurBackdrop *bd)
{
	if (bd == NULL)
		return;
	if (bd->buffer != NULL)
		wlr_buffer_unlock(bd->buffer);
	g_free(bd);
}

static void
blur_drop_backdrops(GowlModuleBlur *mod)
{
	g_list_free_full(mod->backdrops, (GDestroyNotify)blur_backdrop_free);
	mod->backdrops = NULL;
}

static GowlBlurBackdrop *
blur_backdrop_for(GowlModuleBlur *mod, GowlMonitor *m)
{
	GList *l;

	for (l = mod->backdrops; l != NULL; l = l->next) {
		GowlBlurBackdrop *bd = l->data;

		if (bd->monitor == m)
			return bd;
	}
	return NULL;
}

/* ── The blurred wallpaper ───────────────────────────────────────── */

/*
 * Capture the desktop with every window hidden, blur it, and keep the
 * result as a buffer.
 *
 * Keyed on the tag set as well as the output, because per-tag wallpapers
 * mean the answer changes when the tags do -- and a blurred backdrop
 * showing the previous tag's wallpaper is exactly the sort of thing
 * nobody notices in review and everybody notices on screen.
 */
static gboolean
blur_build_backdrop(GowlModuleBlur *mod, GowlCompositor *self, GowlMonitor *m,
                     GowlBlurBackdrop *bd)
{
	GowlFxVis         *vis;
	GowlFxTexture      raw;
	GowlFxTexture      soft;
	struct wlr_buffer *out = NULL;
	GowlFxPass        *pass;
	GowlFxQuad         quad;
	gboolean           ok = FALSE;
	gint               i;

	memset(&raw, 0, sizeof(raw));
	memset(&soft, 0, sizeof(soft));

	mod->capturing = TRUE;
	vis = gowl_fx_vis_begin();

	/* Everything except the background layers.  What is left is the
	 * wallpaper, which is what gets blurred. */
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_TILE, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_FLOAT, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_TOP, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_FS, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_OVERLAY, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_BLOCK, FALSE);
	for (i = 0; i < 4; i++) {
		if (self->rec_indicator[i] != NULL)
			gowl_fx_vis_set(vis, &self->rec_indicator[i]->node, FALSE);
	}
	if (m->fullscreen_bg != NULL)
		gowl_fx_vis_set(vis, &m->fullscreen_bg->node, FALSE);

	if (gowl_fx_capture(mod->gl, self, m, &raw, 1)
	    && gowl_fx_capture_to_buffer(mod->gl, self, m, &out)) {
		ok = TRUE;
	}

	gowl_fx_vis_restore(vis);
	mod->capturing = FALSE;

	if (!ok) {
		gowl_fx_texture_drop(mod->gl, &raw);
		if (out != NULL)
			wlr_buffer_unlock(out);
		return FALSE;
	}

	ok = gowl_fx_texture_blur(mod->gl, &soft, &raw,
	                          gowl_config_get_blur_downscale(self->config),
	                          gowl_config_get_blur_passes(self->config));

	/* Draw the blurred texture back into the buffer the scene will
	 * sample, at the output's full size. */
	if (ok) {
		pass = gowl_fx_pass_begin(mod->gl, out);
		if (pass != NULL) {
			gdouble bright = gowl_config_get_blur_brightness(self->config);
			gfloat  clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

			gowl_fx_pass_clear(pass, clear);
			gowl_fx_quad_init(&quad);
			quad.texture = soft.tex;
			quad.tint[0] = (gfloat)bright;
			quad.tint[1] = (gfloat)bright;
			quad.tint[2] = (gfloat)bright;
			gowl_fx_pass_quad(pass, &quad);
			gowl_fx_pass_end(pass);
		} else {
			ok = FALSE;
		}
	}

	gowl_fx_texture_drop(mod->gl, &raw);
	gowl_fx_texture_drop(mod->gl, &soft);

	if (!ok) {
		wlr_buffer_unlock(out);
		return FALSE;
	}

	if (bd->buffer != NULL)
		wlr_buffer_unlock(bd->buffer);
	bd->buffer = out;
	bd->tags   = m->tagset[m->seltags];
	bd->width  = out->width;
	bd->height = out->height;
	return TRUE;
}

static GowlBlurBackdrop *
blur_ensure_backdrop(GowlModuleBlur *mod, GowlCompositor *self, GowlMonitor *m)
{
	GowlBlurBackdrop *bd = blur_backdrop_for(mod, m);
	gboolean stale;

	if (bd == NULL) {
		bd = g_new0(GowlBlurBackdrop, 1);
		bd->monitor = m;
		mod->backdrops = g_list_prepend(mod->backdrops, bd);
	}

	stale = bd->buffer == NULL
	        || bd->tags != m->tagset[m->seltags]
	        || bd->width != m->wlr_output->width
	        || bd->height != m->wlr_output->height;

	if (stale && !blur_build_backdrop(mod, self, m, bd))
		return NULL;
	return bd->buffer != NULL ? bd : NULL;
}

/* ── Per-client decoration ───────────────────────────────────────── */

static gboolean
blur_client_eligible(GowlClient *c)
{
	/* Fullscreen windows cover the wallpaper entirely, so neither
	 * decoration could be seen; embedder-pinned ones are placed by
	 * somebody else and must not gain nodes they did not ask for. */
	return c != NULL && c->scene != NULL && !c->isfullscreen
	       && !gowl_fx_client_is_pinned(c);
}

static void
blur_apply_shadow(GowlModuleBlur *mod, GowlCompositor *self, GowlClient *c,
                   GowlBlurNodes *nodes)
{
	gint    radius = gowl_config_get_shadow_radius(self->config);
	gdouble opacity = gowl_config_get_shadow_opacity(self->config);
	gint    pad, width, height;
	gdouble rgb[3] = { 0.0, 0.0, 0.0 };

	if (!gowl_config_get_shadow(self->config) || radius <= 0
	    || opacity <= 0.0 || c->geom.width <= 0 || c->geom.height <= 0) {
		if (nodes->shadow != NULL) {
			wlr_scene_node_destroy(&nodes->shadow->node);
			nodes->shadow = NULL;
		}
		return;
	}

	{
		const gchar *spec = gowl_config_get_shadow_color(self->config);
		GowlColor *color = spec != NULL ? gowl_color_new_from_hex(spec) : NULL;

		if (color != NULL) {
			rgb[0] = color->r;
			rgb[1] = color->g;
			rgb[2] = color->b;
			gowl_color_free(color);
		}
	}

	pad    = radius * 2;
	width  = c->geom.width + pad * 2;
	height = c->geom.height + pad * 2;

	/* Only redraw when something the image depends on has changed.  A
	 * window being dragged changes position every frame and size never,
	 * and re-rendering the shadow for each of those would be the one
	 * expensive thing in an otherwise free effect. */
	if (nodes->shadow != NULL
	    && nodes->shadow_w == c->geom.width
	    && nodes->shadow_h == c->geom.height
	    && nodes->shadow_radius == radius
	    && nodes->shadow_opacity == opacity) {
		return;
	}

	if (nodes->shadow != NULL) {
		wlr_scene_node_destroy(&nodes->shadow->node);
		nodes->shadow = NULL;
	}

	{
		guint8 *pixels = gowl_blur_shadow_render(
			width, height, (gdouble)pad, (gdouble)pad,
			(gdouble)c->geom.width, (gdouble)c->geom.height,
			(gdouble)radius, (gdouble)MAX(0, (gint)c->bw) + 6.0,
			opacity, rgb);
		struct wlr_buffer *buffer;

		if (pixels == NULL)
			return;
		buffer = gowl_raw_buffer_create(pixels, width, height, width * 4);
		g_free(pixels);
		if (buffer == NULL)
			return;

		nodes->shadow = wlr_scene_buffer_create(c->scene, buffer);
		wlr_buffer_drop(buffer);
		if (nodes->shadow == NULL)
			return;
	}

	/* Under everything the client owns, offset so the light appears to
	 * come from above. */
	wlr_scene_node_lower_to_bottom(&nodes->shadow->node);
	wlr_scene_node_set_position(&nodes->shadow->node,
	                            -pad + gowl_config_get_shadow_offset_x(self->config),
	                            -pad + gowl_config_get_shadow_offset_y(self->config));
	nodes->shadow_w = c->geom.width;
	nodes->shadow_h = c->geom.height;
	nodes->shadow_radius = radius;
	nodes->shadow_opacity = opacity;
}

static void
blur_apply_backdrop(GowlModuleBlur *mod, GowlCompositor *self, GowlClient *c,
                     GowlBlurNodes *nodes)
{
	GowlBlurBackdrop *bd;
	struct wlr_fbox   src;
	gdouble           scale;

	if (!gowl_config_get_blur(self->config)
	    || c->alpha >= GOWL_BLUR_MIN_TRANSPARENCY
	    || c->mon == NULL) {
		if (nodes->backdrop != NULL) {
			wlr_scene_node_destroy(&nodes->backdrop->node);
			nodes->backdrop = NULL;
		}
		return;
	}

	bd = blur_ensure_backdrop(mod, self, c->mon);
	if (bd == NULL)
		return;

	if (nodes->backdrop == NULL) {
		nodes->backdrop = wlr_scene_buffer_create(c->scene, bd->buffer);
		if (nodes->backdrop == NULL)
			return;
	} else {
		wlr_scene_buffer_set_buffer(nodes->backdrop, bd->buffer);
	}

	/*
	 * The window shows the part of the blurred wallpaper it is standing
	 * on.  The source box is in BUFFER pixels while the geometry is
	 * logical, so it is scaled by the output's ratio -- on a HiDPI screen
	 * the two differ by a factor of two and the crop would otherwise be
	 * from the top-left quarter of the wallpaper.
	 */
	scale = c->mon->m.width > 0
		? (gdouble)bd->width / (gdouble)c->mon->m.width : 1.0;

	src.x      = ((gdouble)c->geom.x - (gdouble)c->mon->m.x) * scale;
	src.y      = ((gdouble)c->geom.y - (gdouble)c->mon->m.y) * scale;
	src.width  = (gdouble)c->geom.width * scale;
	src.height = (gdouble)c->geom.height * scale;

	wlr_scene_buffer_set_source_box(nodes->backdrop, &src);
	wlr_scene_buffer_set_dest_size(nodes->backdrop,
	                               c->geom.width, c->geom.height);
	wlr_scene_node_lower_to_bottom(&nodes->backdrop->node);
	/* Above the shadow, which is outside the window's rectangle anyway. */
	if (nodes->shadow != NULL)
		wlr_scene_node_lower_to_bottom(&nodes->shadow->node);
	wlr_scene_node_set_position(&nodes->backdrop->node, 0, 0);
}

static void
blur_update_client(GowlModuleBlur *mod, GowlCompositor *self, GowlClient *c)
{
	GowlBlurNodes *nodes;

	blur_ensure_gl(mod, self);
	if (mod->gl == NULL || self->config == NULL || self->locked)
		return;

	if (!blur_client_eligible(c)) {
		blur_clear_nodes(c);
		return;
	}

	nodes = blur_nodes(c, TRUE);
	blur_apply_shadow(mod, self, c, nodes);
	blur_apply_backdrop(mod, self, c, nodes);
}

/* ── Hooks ───────────────────────────────────────────────────────── */

/*
 * Never claims an event.  The decorations follow the window rather than
 * placing it, so every hook here returns FALSE and the animation module
 * goes on owning geometry exactly as it did before.
 */
static gboolean
blur_client_event(GowlSceneEffect *effect, GowlCompositor *self, GowlClient *c,
                   GowlSceneEffectEvent event, const struct wlr_box *previous,
                   gboolean interactive)
{
	GowlModuleBlur *mod = GOWL_MODULE_BLUR(effect);

	g_weak_ref_set(&mod->compositor, self);

	if (c == NULL || mod->capturing)
		return FALSE;

	switch (event) {
	case GOWL_SCENE_EFFECT_UNMAP:
	case GOWL_SCENE_EFFECT_DESTROY:
		/* The nodes are children of a scene tree that is about to go, so
		 * forget them rather than free them: touching them afterwards
		 * would be a use-after-free. */
		if (g_object_get_data(G_OBJECT(c), GOWL_BLUR_DATA_KEY) != NULL) {
			GowlBlurNodes *nodes = blur_nodes(c, FALSE);

			nodes->shadow = NULL;
			nodes->backdrop = NULL;
			blur_clear_nodes(c);
		}
		break;
	case GOWL_SCENE_EFFECT_GEOMETRY:
	case GOWL_SCENE_EFFECT_REVEAL:
	case GOWL_SCENE_EFFECT_RELEASE:
		blur_update_client(mod, self, c);
		break;
	default:
		break;
	}
	return FALSE;
}

static void
blur_alpha_changed(GowlSceneEffect *effect, GowlClient *c, gfloat alpha)
{
	GowlModuleBlur *mod = GOWL_MODULE_BLUR(effect);
	GowlCompositor *self = c != NULL ? c->compositor : NULL;

	/* A window becoming translucent is exactly when it needs a blurred
	 * backdrop, and becoming opaque is when it should lose one. */
	if (self != NULL)
		blur_update_client(mod, self, c);
}

static void
blur_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                      GowlMonitor *m)
{
	GowlModuleBlur   *mod = GOWL_MODULE_BLUR(effect);
	GowlBlurBackdrop *bd = blur_backdrop_for(mod, m);

	if (bd != NULL) {
		mod->backdrops = g_list_remove(mod->backdrops, bd);
		blur_backdrop_free(bd);
	}
}

static void
blur_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	GowlModuleBlur *mod = GOWL_MODULE_BLUR(effect);
	GList *l;

	/* Drop the nodes while the scene is still alive, then the buffers,
	 * then GL -- the reverse of how they were made. */
	if (self != NULL) {
		for (l = self->clients; l != NULL; l = l->next)
			blur_clear_nodes(l->data);
	}
	blur_drop_backdrops(mod);
	g_clear_pointer(&mod->gl, gowl_fx_gl_free);
	mod->gl_tried = FALSE;
	g_weak_ref_set(&mod->compositor, NULL);
}

static void
blur_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_event    = blur_client_event;
	iface->alpha_changed   = blur_alpha_changed;
	iface->monitor_removed = blur_monitor_removed;
	iface->finish          = blur_finish;
}

static void
blur_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	blur_finish(GOWL_SCENE_EFFECT(handler), compositor);
}

static void
blur_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = blur_shutdown;
}

/* ── Module ──────────────────────────────────────────────────────── */

/*
 * After the animation module.
 *
 * The decorations follow the window's FINAL geometry, and the animation
 * module is what decides that.  Running first would decorate a rectangle
 * the window is about to leave.  It claims nothing either way, so the
 * order costs nobody anything.
 */
#define GOWL_BLUR_PRIORITY (10)

static gboolean
blur_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_BLUR_PRIORITY);
	return TRUE;
}

static void
blur_deactivate(GowlModule *base)
{
	GowlModuleBlur *mod = GOWL_MODULE_BLUR(base);
	GowlCompositor *self = g_weak_ref_get(&mod->compositor);

	if (self != NULL) {
		blur_finish(GOWL_SCENE_EFFECT(mod), self);
		g_object_unref(self);
	} else {
		blur_drop_backdrops(mod);
		g_clear_pointer(&mod->gl, gowl_fx_gl_free);
		mod->gl_tried = FALSE;
	}
}

static const gchar *blur_name(GowlModule *m)    { return "blur"; }
static const gchar *blur_version(GowlModule *m) { return "0.1.0"; }
static const gchar *blur_description(GowlModule *m)
{
	return "Blurs the desktop behind translucent windows and adds shadows";
}

static void
blur_finalize(GObject *object)
{
	GowlModuleBlur *mod = GOWL_MODULE_BLUR(object);

	blur_deactivate(GOWL_MODULE(object));
	g_weak_ref_clear(&mod->compositor);
	G_OBJECT_CLASS(gowl_module_blur_parent_class)->finalize(object);
}

static void
gowl_module_blur_class_init(GowlModuleBlurClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = blur_activate;
	mod->deactivate      = blur_deactivate;
	mod->get_name        = blur_name;
	mod->get_description = blur_description;
	mod->get_version     = blur_version;
	G_OBJECT_CLASS(klass)->finalize = blur_finalize;
}

static void
gowl_module_blur_init(GowlModuleBlur *mod)
{
	g_weak_ref_init(&mod->compositor, NULL);
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_BLUR_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_BLUR;
}
