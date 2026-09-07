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
 * The desktop cube: a tag switch turns the desktop instead of cutting to
 * it, one face-step per tag crossed, so going from tag 1 to tag 4 turns
 * three times and shows tags 2 and 3 on the way past.
 *
 * How the pieces divide up:
 *
 *   gowl-cube-plan.c  decides WHAT turns and for how long.  No wlroots.
 *   gowl-cube-gl.c    draws it.  No compositor.
 *   this file         connects the two to gowl, and owns every side
 *                     effect: capturing each tag's desktop, putting an
 *                     opaque sheet over the monitor for the duration,
 *                     and putting the scene back exactly as it was.
 *
 * IT COEXISTS WITH THE ANIMATION MODULE, and that is the reason for the
 * delegation running through every hook below.  gowl allows one scene
 * presentation owner so two modules cannot fight over the same node.
 * The cube owns the OUTPUT, for about half a second at a time; the
 * animation module owns per-client geometry, always.  So the cube takes
 * the higher priority, answers what it must, and passes everything else
 * down to whichever provider it outranks --- which means with both
 * loaded, windows still pop, jiggle and settle exactly as before, and
 * with the cube alone nothing is missing either.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-cube"

#include "gowl-cube-plan.h"
#include "gowl-cube-gl.h"

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-effects.h"
#include "config/gowl-config.h"
#include "boxed/gowl-color.h"
#include "util/gowl-easing.h"
#include "interfaces/gowl-scene-effect.h"
#include "interfaces/gowl-shutdown-handler.h"

#include <math.h>
#include <drm_fourcc.h>

#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

#include <gmodule.h>

/* Intermediate desktops are only ever seen mid-spin, at an angle, under
 * motion blur.  Storing them at half size is invisible and is what keeps
 * an eight-step rotation on a 4K screen from costing a third of a
 * gigabyte of texture memory. */
#define GOWL_CUBE_INTERMEDIATE_DIVISOR 2

/* Angular speed, in radians per second, that counts as "full blur". */
#define GOWL_CUBE_BLUR_REFERENCE 6.0

#define GOWL_TYPE_MODULE_CUBE (gowl_module_cube_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleCube, gowl_module_cube, GOWL, MODULE_CUBE, GowlModule)

/**
 * GowlCubeRun:
 *
 * One rotation in flight, on one output.  Multi-monitor setups can have
 * several: tags are per-monitor in gowl, so two screens can be turning
 * different journeys at once and neither may touch the other's scene.
 */
typedef struct {
	GowlMonitor            *monitor;      /* unowned; runs end with the output */
	GowlCubePlan            plan;

	/* Stored desktops, indexed by slot.  Slots outside 0..steps have no
	 * desktop and draw as blank sides. */
	GowlCubeFace            face[GOWL_CUBE_MAX_STEPS + 1];

	struct wlr_scene_tree  *tree;
	struct wlr_scene_buffer *sheet;
	struct wlr_swapchain   *swapchain;

	/* Clients we hid for the duration, so they can be put back exactly.
	 * Only ever the fullscreen layer: everything below the sheet is
	 * covered by it, and everything above (the bar, notifications) is
	 * meant to stay. */
	GPtrArray              *hidden;
	gboolean                hid_fullscreen_bg;
} GowlCubeRun;

struct _GowlModuleCube {
	GowlModule  parent_instance;
	GWeakRef    compositor;
	GowlCubeGl *gl;
	gboolean    gl_tried;
	GList      *runs;        /* GowlCubeRun* */

	/* Set while a capture is rendering the scene.  Every hook checks it:
	 * a capture toggles scene visibility, and a reveal or a frame that
	 * ran in the middle of that would see a scene that is deliberately
	 * lying about which tag is on screen. */
	gboolean    capturing;
};

static void cube_effect_init(GowlSceneEffectInterface *iface);
static void cube_shutdown_init(GowlShutdownHandlerInterface *iface);
G_DEFINE_TYPE_WITH_CODE(GowlModuleCube, gowl_module_cube, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, cube_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, cube_shutdown_init))

/* ── Delegation ──────────────────────────────────────────────────── */

/*
 * The provider the cube outranks, if any.
 *
 * Everything the cube does not itself answer goes here.  Without this the
 * mere presence of the cube would silently switch window animations off,
 * which is the opposite of what loading a second effect should do.
 */
static GowlSceneEffect *
cube_next(GowlSceneEffect *effect, GowlCompositor *self)
{
	if (self == NULL || self->module_mgr == NULL)
		return NULL;
	return (GowlSceneEffect *)gowl_module_manager_get_scene_effect_after(
		self->module_mgr, effect);
}

static void
cube_bind(GowlModuleCube *mod, GowlCompositor *self)
{
	GowlCompositor *held = g_weak_ref_get(&mod->compositor);

	if (held == self) {
		g_clear_object(&held);
		return;
	}
	g_clear_object(&held);
	g_weak_ref_set(&mod->compositor, self);
}

/* ── Configuration ───────────────────────────────────────────────── */

/*
 * Build the GL side on first use, once.
 *
 * Not at activate(): a module can be loaded before the compositor has a
 * renderer, and the answer to "is this renderer one whose context we can
 * borrow" is only available afterwards.  @gl_tried keeps a `no' from
 * being re-asked sixty times a second for the rest of the session.
 */
static void
cube_ensure_gl(GowlModuleCube *mod, GowlCompositor *self)
{
	if (mod->gl != NULL || mod->gl_tried)
		return;
	if (self == NULL || self->renderer == NULL)
		return;

	mod->gl_tried = TRUE;
	mod->gl = gowl_cube_gl_new(self->renderer);
}

static gboolean
cube_enabled(GowlModuleCube *mod, GowlCompositor *self)
{
	return self != NULL
	       && self->config != NULL
	       && !self->locked
	       && gowl_config_get_cube(self->config)
	       && gowl_config_get_cube_duration(self->config) > 0
	       && mod->gl != NULL;
}

static void
cube_backdrop_rgb(GowlCompositor *self, gfloat *out)
{
	const gchar *spec = self->config != NULL
		? gowl_config_get_cube_backdrop_color(self->config) : NULL;
	GowlColor *color = spec != NULL ? gowl_color_new_from_hex(spec) : NULL;

	/* A colour that does not parse is a typo in a config file, not a
	 * reason to abandon the rotation. */
	if (color == NULL) {
		out[0] = 0.04f; out[1] = 0.043f; out[2] = 0.063f;
		return;
	}
	out[0] = (gfloat)color->r;
	out[1] = (gfloat)color->g;
	out[2] = (gfloat)color->b;
	gowl_color_free(color);
}

/* ── Per-monitor tag memory ──────────────────────────────────────── */

/*
 * The last tag set the cube knows about, kept on the monitor itself so it
 * lives and dies with the output and needs no bookkeeping when one is
 * unplugged.  Tag changes are noticed by comparing against this on each
 * frame rather than by listening for a signal: by the time a signal would
 * arrive the scene has already been rearranged, and the cube has to
 * reconstruct every tag's desktop from scratch anyway.
 */
#define GOWL_CUBE_TAG_KEY "gowl-cube-last-tags"

static guint32
cube_remembered_tags(GowlMonitor *m)
{
	return (guint32)GPOINTER_TO_UINT(
		g_object_get_data(G_OBJECT(m), GOWL_CUBE_TAG_KEY));
}

static void
cube_remember_tags(GowlMonitor *m, guint32 tags)
{
	/* Zero means "nothing recorded", so a monitor that genuinely shows no
	 * tags simply does not arm the cube --- which is right: there is no
	 * desktop to turn away from. */
	g_object_set_data(G_OBJECT(m), GOWL_CUBE_TAG_KEY,
	                  GUINT_TO_POINTER(tags));
}

static GowlCubeRun *
cube_run_for(GowlModuleCube *mod, GowlMonitor *m)
{
	GList *l;

	for (l = mod->runs; l != NULL; l = l->next) {
		GowlCubeRun *run = l->data;

		if (run->monitor == m)
			return run;
	}
	return NULL;
}

/* ── Scene capture ───────────────────────────────────────────────── */

typedef struct {
	GowlClient *client;
	gboolean    enabled;
} GowlCubeVisSave;

typedef struct {
	struct wlr_scene_node *node;
	gboolean               enabled;
} GowlCubeNodeSave;

/*
 * TRUE for a client the embedder pins over its own surface rather than
 * placing by tag: the in-buffer views of `emacs --gowl', and module
 * overlays such as a dropdown terminal.
 *
 * They are the awkward case for the cube.  gowl parks them in the overlay
 * layer, which is above the sheet, so left alone they would hang
 * motionless in mid-air while the desktop turned underneath them.  They
 * are also not on a tag in any meaningful sense --- nothing about them
 * changes when the tags do --- so there is no ONE side they belong to.
 * Putting them on every side is therefore both the honest answer and the
 * one without a floating artefact: they stay where the embedder put them,
 * and they turn with the desktop.
 */
static gboolean
cube_is_pinned(GowlClient *c)
{
	return c != NULL && (c->isembedded || c->isoverlay);
}

/*
 * Show exactly the clients on @tags and nothing else, remembering what was
 * showing before.
 *
 * Embedded and overlay clients are skipped, not hidden: they are managed
 * by the embedder (cmacs, under `emacs --gowl'), which does not route
 * their visibility through tags, so guessing here would make them blink.
 * They therefore appear on every face, which is also what they do on
 * every tag.
 */
static GArray *
cube_show_tags(GowlCompositor *self, GowlMonitor *m, guint32 tags)
{
	GArray *saved = g_array_new(FALSE, FALSE, sizeof(GowlCubeVisSave));
	GList  *l;

	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient      *c = l->data;
		GowlCubeVisSave  entry;

		if (c->mon != m || c->scene == NULL || c->isembedded || c->isoverlay)
			continue;

		entry.client  = c;
		entry.enabled = c->scene->node.enabled;
		g_array_append_val(saved, entry);

		wlr_scene_node_set_enabled(&c->scene->node,
		                           (c->tags & tags) != 0);
	}
	return saved;
}

static void
cube_restore_tags(GArray *saved)
{
	guint i;

	if (saved == NULL)
		return;
	for (i = 0; i < saved->len; i++) {
		GowlCubeVisSave *entry =
			&g_array_index(saved, GowlCubeVisSave, i);

		if (entry->client->scene != NULL)
			wlr_scene_node_set_enabled(&entry->client->scene->node,
			                           entry->enabled);
	}
	g_array_free(saved, TRUE);
}

/*
 * Render the scene as it would look on @tags into a texture the cube owns.
 *
 * The layers above the sheet are switched off for the duration: the bar
 * and any notifications stay live ON TOP of the rotation, so baking them
 * into every face as well would show four bars turning past.  The
 * recording indicator is switched off for the same reason and put back
 * immediately --- it is a safety light and must never be off for a frame
 * that is actually presented, which this one is not.
 */
static gboolean
cube_capture_face(GowlModuleCube *mod, GowlCompositor *self, GowlMonitor *m,
                   guint32 tags, GowlCubeFace *face, gint divisor)
{
	struct wlr_output_state  state;
	struct wlr_texture      *texture;
	struct wlr_scene_node   *node;
	GArray                  *saved;
	GArray                  *overlay_saved;
	gboolean                 top_on, block_on;
	gboolean                 rec_on[4];
	gboolean                 ok = FALSE;
	gint                     i;

	if (m->scene_output == NULL || m->wlr_output == NULL)
		return FALSE;

	mod->capturing = TRUE;

	top_on     = self->layers[GOWL_SCENE_LAYER_TOP]->node.enabled;
	block_on   = self->layers[GOWL_SCENE_LAYER_BLOCK]->node.enabled;
	wlr_scene_node_set_enabled(&self->layers[GOWL_SCENE_LAYER_TOP]->node, FALSE);
	wlr_scene_node_set_enabled(&self->layers[GOWL_SCENE_LAYER_BLOCK]->node, FALSE);

	/*
	 * The overlay layer is switched off a child at a time rather than
	 * wholesale, because it holds two unrelated kinds of thing: overlay
	 * layer-shell surfaces, which stay live above the rotation and so
	 * must not be baked into every side, and embedder-pinned clients,
	 * which must.
	 */
	overlay_saved = g_array_new(FALSE, FALSE, sizeof(GowlCubeNodeSave));
	wl_list_for_each(node,
	                 &self->layers[GOWL_SCENE_LAYER_OVERLAY]->children, link) {
		GowlCubeNodeSave entry;
		GList *l;
		gboolean pinned = FALSE;

		for (l = self->clients; l != NULL && !pinned; l = l->next) {
			GowlClient *c = l->data;

			if (cube_is_pinned(c) && c->scene != NULL
			    && &c->scene->node == node)
				pinned = TRUE;
		}
		if (pinned)
			continue;

		entry.node    = node;
		entry.enabled = node->enabled;
		g_array_append_val(overlay_saved, entry);
		wlr_scene_node_set_enabled(node, FALSE);
	}

	for (i = 0; i < 4; i++) {
		rec_on[i] = self->rec_indicator[i] != NULL
			&& self->rec_indicator[i]->node.enabled;
		if (self->rec_indicator[i] != NULL)
			wlr_scene_node_set_enabled(&self->rec_indicator[i]->node, FALSE);
	}

	saved = cube_show_tags(self, m, tags);

	wlr_output_state_init(&state);

	/* The scene skips a render when nothing changed, and between two
	 * captures nothing has from its point of view. */
	wlr_damage_ring_add_whole(&m->scene_output->damage_ring);

	if (wlr_scene_output_build_state(m->scene_output, &state, NULL)
	    && (state.committed & WLR_OUTPUT_STATE_BUFFER) != 0
	    && state.buffer != NULL) {
		texture = wlr_texture_from_buffer(self->renderer, state.buffer);
		if (texture != NULL) {
			ok = gowl_cube_gl_store_face(mod->gl, face, texture,
			                             MAX(1, state.buffer->width / divisor),
			                             MAX(1, state.buffer->height / divisor));
			wlr_texture_destroy(texture);
		}
	}
	wlr_output_state_finish(&state);

	cube_restore_tags(saved);
	for (i = 0; i < (gint)overlay_saved->len; i++) {
		GowlCubeNodeSave *entry =
			&g_array_index(overlay_saved, GowlCubeNodeSave, i);

		wlr_scene_node_set_enabled(entry->node, entry->enabled);
	}
	g_array_free(overlay_saved, TRUE);
	wlr_scene_node_set_enabled(&self->layers[GOWL_SCENE_LAYER_TOP]->node, top_on);
	wlr_scene_node_set_enabled(&self->layers[GOWL_SCENE_LAYER_BLOCK]->node, block_on);
	for (i = 0; i < 4; i++) {
		if (self->rec_indicator[i] != NULL)
			wlr_scene_node_set_enabled(&self->rec_indicator[i]->node,
			                           rec_on[i]);
	}

	/* The real frame that follows must draw everything again, because the
	 * captures consumed the damage that was going to produce it. */
	wlr_damage_ring_add_whole(&m->scene_output->damage_ring);

	mod->capturing = FALSE;
	return ok;
}

/* ── Run lifecycle ───────────────────────────────────────────────── */

static void
cube_run_free(GowlModuleCube *mod, GowlCubeRun *run)
{
	gint i;

	if (run == NULL)
		return;

	/* Put back the fullscreen layer before the sheet goes, so no frame
	 * can catch the output with neither on it. */
	if (run->hidden != NULL) {
		guint n;

		for (n = 0; n < run->hidden->len; n++) {
			GowlClient *c = g_ptr_array_index(run->hidden, n);

			/* The reference taken when it was hidden is what makes
			 * this safe: a window closed during the rotation has had
			 * its scene tree destroyed and c->scene set to NULL, and
			 * only a live GObject can be asked about that. */
			if (c->scene != NULL)
				wlr_scene_node_set_enabled(&c->scene->node, TRUE);
		}
		g_ptr_array_free(run->hidden, TRUE);
		run->hidden = NULL;
	}
	if (run->hid_fullscreen_bg && run->monitor != NULL
	    && run->monitor->fullscreen_bg != NULL)
		wlr_scene_node_set_enabled(&run->monitor->fullscreen_bg->node, TRUE);

	if (run->tree != NULL)
		wlr_scene_node_destroy(&run->tree->node);
	run->tree  = NULL;
	run->sheet = NULL;

	g_clear_pointer(&run->swapchain, wlr_swapchain_destroy);

	for (i = 0; i <= GOWL_CUBE_MAX_STEPS; i++)
		gowl_cube_gl_drop_face(mod->gl, &run->face[i]);

	g_free(run);
}

static void
cube_run_end(GowlModuleCube *mod, GowlCubeRun *run)
{
	GowlMonitor *m = run != NULL ? run->monitor : NULL;

	mod->runs = g_list_remove(mod->runs, run);
	cube_run_free(mod, run);

	/* The sheet just vanished from the scene; without whole damage the
	 * output can decide the frame it already drew is still valid and
	 * leave the last frame of the rotation frozen on screen. */
	if (m != NULL && m->scene_output != NULL)
		wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
}

static void
cube_end_all(GowlModuleCube *mod)
{
	while (mod->runs != NULL)
		cube_run_end(mod, mod->runs->data);
}

static gboolean
cube_swapchain_init(GowlCubeRun *run, GowlCompositor *self, GowlMonitor *m)
{
	struct wlr_drm_format format;
	uint64_t              modifier = DRM_FORMAT_MOD_INVALID;

	memset(&format, 0, sizeof(format));
	format.format    = m->wlr_output->render_format;
	format.len       = 1;
	format.capacity  = 1;
	format.modifiers = &modifier;

	run->swapchain = wlr_swapchain_create(self->allocator,
	                                      m->wlr_output->width,
	                                      m->wlr_output->height,
	                                      &format);
	if (run->swapchain == NULL) {
		/* Some drivers refuse the output's render format for an
		 * off-screen buffer.  Plain opaque 8888 is the format every GBM
		 * allocator in existence can produce. */
		format.format = DRM_FORMAT_XRGB8888;
		run->swapchain = wlr_swapchain_create(self->allocator,
		                                      m->wlr_output->width,
		                                      m->wlr_output->height,
		                                      &format);
	}
	return run->swapchain != NULL;
}

/*
 * Take over the output: an opaque sheet just under the bar, and the
 * fullscreen layer out of the way.
 *
 * The sheet does not need the layers below it switched off --- it is
 * opaque and covers the whole monitor, so it hides them by being there,
 * and on a multi-monitor setup switching a shared layer off would blank
 * the OTHER screen.  The fullscreen layer is the exception because gowl
 * stacks it above the bar, so it would otherwise show through.
 */
static gboolean
cube_take_output(GowlCubeRun *run, GowlCompositor *self, GowlMonitor *m)
{
	pixman_region32_t opaque;
	GList *l;

	run->tree = wlr_scene_tree_create(&self->scene->tree);
	if (run->tree == NULL)
		return FALSE;

	wlr_scene_node_place_above(&run->tree->node,
	                           &self->layers[GOWL_SCENE_LAYER_FLOAT]->node);
	wlr_scene_node_set_position(&run->tree->node, m->m.x, m->m.y);

	run->sheet = wlr_scene_buffer_create(run->tree, NULL);
	if (run->sheet == NULL) {
		wlr_scene_node_destroy(&run->tree->node);
		run->tree = NULL;
		return FALSE;
	}

	wlr_scene_buffer_set_dest_size(run->sheet, m->m.width, m->m.height);

	pixman_region32_init_rect(&opaque, 0, 0, m->m.width, m->m.height);
	wlr_scene_buffer_set_opaque_region(run->sheet, &opaque);
	pixman_region32_fini(&opaque);

	/*
	 * Two kinds of client sit ABOVE the sheet in gowl's layer order and
	 * so have to be taken down by hand for the duration: fullscreen
	 * clients, and the embedder-pinned ones.  Both are already on the
	 * sides of the solid, captured a moment ago, so hiding the live
	 * copies is what stops them hanging over the rotation.
	 */
	run->hidden = g_ptr_array_new_with_free_func(g_object_unref);
	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if (c->mon != m || c->scene == NULL || !c->scene->node.enabled)
			continue;
		if (c->scene->node.parent != self->layers[GOWL_SCENE_LAYER_FS]
		    && !cube_is_pinned(c))
			continue;
		wlr_scene_node_set_enabled(&c->scene->node, FALSE);
		g_ptr_array_add(run->hidden, g_object_ref(c));
	}
	if (m->fullscreen_bg != NULL && m->fullscreen_bg->node.enabled) {
		wlr_scene_node_set_enabled(&m->fullscreen_bg->node, FALSE);
		run->hid_fullscreen_bg = TRUE;
	}
	return TRUE;
}

/*
 * Begin a rotation on @m from @from_tags to @to_tags, or decline.
 *
 * Everything that can refuse does so before anything is touched, so a
 * declined rotation costs the user nothing but an instant tag switch,
 * which is what they had before this module existed.
 */
static gboolean
cube_run_start(GowlModuleCube *mod, GowlCompositor *self, GowlMonitor *m,
                guint32 from_tags, guint32 to_tags, gint64 now_us)
{
	GowlCubeRun *run;
	gint         j;

	if (m->wlr_output == NULL || m->scene_output == NULL
	    || m->m.width <= 0 || m->m.height <= 0)
		return FALSE;

	/*
	 * A rotated output renders into a buffer whose axes are not the
	 * screen's, and the cube's projection assumes they are.  Rather than
	 * present a sideways desktop, sit the rotation out.
	 */
	if (m->wlr_output->transform != WL_OUTPUT_TRANSFORM_NORMAL)
		return FALSE;

	run = g_new0(GowlCubeRun, 1);
	run->monitor = m;

	if (!gowl_cube_plan_init(&run->plan,
	                         gowl_cube_tag_index(from_tags),
	                         gowl_cube_tag_index(to_tags),
	                         gowl_config_get_cube_faces(self->config),
	                         gowl_config_get_cube_duration(self->config),
	                         gowl_config_get_cube_step_duration(self->config),
	                         now_us)) {
		g_free(run);
		return FALSE;
	}

	if (!cube_swapchain_init(run, self, m)) {
		g_warning("cube: no off-screen buffers for %s; leaving tag "
		          "switches instant on that output",
		          m->wlr_output->name != NULL ? m->wlr_output->name : "?");
		g_free(run);
		return FALSE;
	}

	/*
	 * Capture every desktop in the journey up front.  It is the whole
	 * cost of the effect and it lands in one frame at the start, which is
	 * the right place for it: a capture taken later, while the solid is
	 * already turning, would have to rearrange the scene mid-rotation.
	 */
	for (j = 0; j <= run->plan.steps; j++) {
		gboolean endpoint = (j == 0 || j == run->plan.steps);

		if (!cube_capture_face(mod, self, m,
		                       1u << run->plan.tag[j], &run->face[j],
		                       endpoint ? 1 : GOWL_CUBE_INTERMEDIATE_DIVISOR)) {
			/* The two ends are what make the effect seamless; without
			 * one of them the rotation would visibly cut. */
			if (endpoint) {
				cube_run_free(mod, run);
				return FALSE;
			}
		}
	}

	if (!cube_take_output(run, self, m)) {
		cube_run_free(mod, run);
		return FALSE;
	}

	mod->runs = g_list_prepend(mod->runs, run);
	return TRUE;
}

/* ── Frame ───────────────────────────────────────────────────────── */

static gdouble
cube_eased(GowlCompositor *self, const GowlCubePlan *plan, gint64 at_us)
{
	const gchar *curve = self->config != NULL
		? gowl_config_get_cube_curve(self->config) : NULL;

	return gowl_easing_eval(curve, gowl_cube_plan_progress(plan, at_us));
}

static gboolean
cube_draw(GowlModuleCube *mod, GowlCompositor *self, GowlCubeRun *run,
           gint64 now_us)
{
	GowlCubeFrame      frame;
	GowlCubeFace       window[4];
	struct wlr_buffer *buffer;
	GowlMonitor       *m = run->monitor;
	gdouble            linear, eased, eased_prev, omega;
	gint               first, last, j;

	memset(&frame, 0, sizeof(frame));
	memset(window, 0, sizeof(window));

	linear = gowl_cube_plan_progress(&run->plan, now_us);
	eased  = cube_eased(self, &run->plan, now_us);

	/* Angular speed by difference rather than by differentiating the
	 * curve: the curves are Béziers solved numerically, so a difference
	 * over a fixed short interval is both simpler and exactly as good for
	 * something that only scales a blur. */
	eased_prev = cube_eased(self, &run->plan, now_us - 8000);
	omega = fabs(eased - eased_prev) * (gdouble)run->plan.steps
	        * gowl_cube_plan_face_angle(&run->plan) / 0.008;

	gowl_cube_plan_slot_window(&run->plan, eased, &first, &last);
	/* The planner already caps the span at four and at the face count;
	 * this is the belt to that braces, because overrunning `window'
	 * would read past the end of a stack array. */
	if (last - first >= (gint)G_N_ELEMENTS(window))
		last = first + (gint)G_N_ELEMENTS(window) - 1;

	for (j = first; j <= last; j++) {
		if (j >= 0 && j <= run->plan.steps)
			window[j - first] = run->face[j];
	}

	frame.faces       = run->plan.faces;
	frame.dir         = run->plan.dir;
	frame.rotation    = gowl_cube_plan_rotation(&run->plan, eased);
	frame.face_angle  = gowl_cube_plan_face_angle(&run->plan);
	frame.bump        = gowl_cube_plan_bump(linear);
	frame.speed       = CLAMP(omega / GOWL_CUBE_BLUR_REFERENCE, 0.0, 1.0);
	frame.zoom        = gowl_config_get_cube_zoom(self->config);
	frame.pitch_deg   = gowl_config_get_cube_pitch(self->config);
	frame.shading     = gowl_config_get_cube_shading(self->config);
	frame.reflection  = gowl_config_get_cube_reflection(self->config);
	frame.motion_blur = gowl_config_get_cube_motion_blur(self->config);
	frame.caps        = gowl_config_get_cube_caps(self->config);
	frame.first_slot  = first;
	frame.last_slot   = last;
	frame.slot        = window;
	cube_backdrop_rgb(self, frame.backdrop);

	buffer = wlr_swapchain_acquire(run->swapchain);
	if (buffer == NULL)
		return FALSE;

	if (!gowl_cube_gl_render(mod->gl, buffer, &frame)) {
		wlr_buffer_unlock(buffer);
		return FALSE;
	}

	wlr_scene_buffer_set_buffer(run->sheet, buffer);
	wlr_buffer_unlock(buffer);

	/* Keep the sheet over the monitor even if the output was reshaped
	 * under us; a stale size would letterbox the rotation. */
	wlr_scene_node_set_position(&run->tree->node, m->m.x, m->m.y);
	wlr_scene_buffer_set_dest_size(run->sheet, m->m.width, m->m.height);
	return TRUE;
}

/*
 * Notice a tag change and, if it is one worth turning for, start.
 *
 * Called from the frame hook rather than from the monitor's tag-changed
 * signal.  The signal fires before the scene has been rearranged, but the
 * cube needs every tag's desktop, not just the old one, and builds them
 * all by hand anyway --- so there is nothing the earlier callback would
 * buy, and polling here also catches outputs that appeared after the
 * module loaded, which a per-monitor signal connection would miss.
 */
static void
cube_poll_tags(GowlModuleCube *mod, GowlCompositor *self, GowlMonitor *m,
                gint64 now_us)
{
	guint32 previous = cube_remembered_tags(m);
	guint32 current  = m->tagset[m->seltags];

	if (current == previous)
		return;

	cube_remember_tags(m, current);

	if (previous == 0 || !cube_enabled(mod, self))
		return;
	if (!gowl_config_get_cube_all_monitors(self->config) && self->selmon != m)
		return;

	/*
	 * A tag change arriving mid-rotation ends the one in flight and
	 * starts a fresh one from where the old journey was headed.  Bending
	 * the path instead would mean re-capturing every desktop on the new
	 * route while the old textures are still on screen; a switch inside
	 * half a second is rare enough that the honest cut is the better
	 * trade.
	 */
	if (cube_run_for(mod, m) != NULL)
		cube_run_end(mod, cube_run_for(mod, m));

	cube_run_start(mod, self, m, previous, current, now_us);
}

static gboolean
cube_frame(GowlSceneEffect *effect, GowlCompositor *self, GowlMonitor *m,
            gint64 now)
{
	GowlModuleCube  *mod = GOWL_MODULE_CUBE(effect);
	GowlSceneEffect *next;
	GowlCubeRun     *run;
	gboolean         next_live = FALSE;

	cube_bind(mod, self);

	next = cube_next(effect, self);
	if (next != NULL && GOWL_SCENE_EFFECT_GET_IFACE(next)->frame != NULL)
		next_live = GOWL_SCENE_EFFECT_GET_IFACE(next)->frame(next, self, m, now);

	if (mod->capturing || m == NULL)
		return next_live;

	cube_ensure_gl(mod, self);
	cube_poll_tags(mod, self, m, now);

	run = cube_run_for(mod, m);
	if (run == NULL)
		return next_live;

	if (gowl_cube_plan_progress(&run->plan, now) >= 1.0) {
		cube_run_end(mod, run);
		return TRUE;
	}

	if (!cube_draw(mod, self, run, now)) {
		/* A frame that cannot be drawn is the end of the rotation, not a
		 * reason to leave a stale sheet over the user's desktop. */
		cube_run_end(mod, run);
		return TRUE;
	}
	return TRUE;
}

/* ── Delegated hooks ─────────────────────────────────────────────── */

/*
 * TRUE when this monitor is one frame away from starting a rotation.
 *
 * gowl reveals newly visible windows with a fade, and that fade is
 * started by the same arrange() call that changed the tags --- before the
 * cube has seen anything.  Left alone it would run underneath the
 * rotation and finish long before it, so the windows the cube already
 * showed at full strength would be there; but a slower fade, or a shorter
 * rotation, would end with the desktop fading UP after it had landed.
 * Swallowing the reveal is both cheaper and unconditionally right: the
 * cube's first captured face already shows them.
 */
static gboolean
cube_pending(GowlModuleCube *mod, GowlCompositor *self, GowlMonitor *m)
{
	guint32 previous;

	if (m == NULL || !cube_enabled(mod, self) || mod->capturing)
		return FALSE;
	if (cube_run_for(mod, m) != NULL)
		return TRUE;

	previous = cube_remembered_tags(m);
	if (previous == 0 || previous == m->tagset[m->seltags])
		return FALSE;
	if (!gowl_config_get_cube_all_monitors(self->config) && self->selmon != m)
		return FALSE;

	return gowl_cube_tag_index(previous)
	       != gowl_cube_tag_index(m->tagset[m->seltags]);
}

static gboolean
cube_client_event(GowlSceneEffect *effect, GowlCompositor *self, GowlClient *c,
                   GowlSceneEffectEvent event, const struct wlr_box *previous,
                   gboolean interactive)
{
	GowlModuleCube  *mod = GOWL_MODULE_CUBE(effect);
	GowlSceneEffect *next;

	cube_bind(mod, self);

	if (event == GOWL_SCENE_EFFECT_REVEAL && c != NULL
	    && cube_pending(mod, self, c->mon))
		return TRUE;

	next = cube_next(effect, self);
	return next != NULL
	       && GOWL_SCENE_EFFECT_GET_IFACE(next)->client_event != NULL
	       && GOWL_SCENE_EFFECT_GET_IFACE(next)->client_event(
			next, self, c, event, previous, interactive);
}

static gboolean
cube_get_geometry(GowlSceneEffect *effect, GowlClient *c, struct wlr_box *box)
{
	GowlSceneEffect *next = c != NULL ? cube_next(effect, c->compositor) : NULL;

	return next != NULL
	       && GOWL_SCENE_EFFECT_GET_IFACE(next)->get_geometry != NULL
	       && GOWL_SCENE_EFFECT_GET_IFACE(next)->get_geometry(next, c, box);
}

static void
cube_alpha_changed(GowlSceneEffect *effect, GowlClient *c, gfloat alpha)
{
	GowlSceneEffect *next = c != NULL ? cube_next(effect, c->compositor) : NULL;

	if (next != NULL && GOWL_SCENE_EFFECT_GET_IFACE(next)->alpha_changed != NULL)
		GOWL_SCENE_EFFECT_GET_IFACE(next)->alpha_changed(next, c, alpha);
}

static struct wlr_surface *
cube_surface_at(GowlSceneEffect *effect, GowlClient *c, gdouble x, gdouble y,
                 gdouble *sx, gdouble *sy)
{
	GowlSceneEffect *next = c != NULL ? cube_next(effect, c->compositor) : NULL;

	return next != NULL
	       && GOWL_SCENE_EFFECT_GET_IFACE(next)->surface_at != NULL
	           ? GOWL_SCENE_EFFECT_GET_IFACE(next)->surface_at(next, c, x, y,
	                                                            sx, sy)
	           : NULL;
}

static void
cube_frame_done(GowlSceneEffect *effect, GowlCompositor *self, GowlMonitor *m,
                 const struct timespec *now)
{
	GowlSceneEffect *next = cube_next(effect, self);

	if (next != NULL && GOWL_SCENE_EFFECT_GET_IFACE(next)->frame_done != NULL)
		GOWL_SCENE_EFFECT_GET_IFACE(next)->frame_done(next, self, m, now);
}

static void
cube_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                      GowlMonitor *m)
{
	GowlModuleCube  *mod = GOWL_MODULE_CUBE(effect);
	GowlCubeRun     *run = cube_run_for(mod, m);
	GowlSceneEffect *next;

	/* The run holds a scene tree parented under a scene that is about to
	 * lose this output, and textures it must free while the renderer is
	 * still alive. */
	if (run != NULL)
		cube_run_end(mod, run);

	next = cube_next(effect, self);
	if (next != NULL && GOWL_SCENE_EFFECT_GET_IFACE(next)->monitor_removed != NULL)
		GOWL_SCENE_EFFECT_GET_IFACE(next)->monitor_removed(next, self, m);
}

/*
 * Everything GL must go before the renderer does, and `finish' is the one
 * hook gowl guarantees runs while the scene and renderer are still there.
 */
static void
cube_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	GowlModuleCube  *mod = GOWL_MODULE_CUBE(effect);
	GowlSceneEffect *next = cube_next(effect, self);

	cube_end_all(mod);
	g_clear_pointer(&mod->gl, gowl_cube_gl_free);
	mod->gl_tried = FALSE;

	if (next != NULL && GOWL_SCENE_EFFECT_GET_IFACE(next)->finish != NULL)
		GOWL_SCENE_EFFECT_GET_IFACE(next)->finish(next, self);

	g_weak_ref_set(&mod->compositor, NULL);
}

static void
cube_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_event    = cube_client_event;
	iface->get_geometry    = cube_get_geometry;
	iface->alpha_changed   = cube_alpha_changed;
	iface->surface_at      = cube_surface_at;
	iface->frame           = cube_frame;
	iface->frame_done      = cube_frame_done;
	iface->monitor_removed = cube_monitor_removed;
	iface->finish          = cube_finish;
}

static void
cube_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	cube_finish(GOWL_SCENE_EFFECT(handler), compositor);
}

static void
cube_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = cube_shutdown;
}

/* ── Module ──────────────────────────────────────────────────────── */

/*
 * Ahead of the animation module, which sits at the default zero.
 *
 * The order matters in one direction only: the cube must be asked first
 * so it can decline, because the provider that answers first is the one
 * that owns the frame.  Everything it declines it hands straight down.
 */
#define GOWL_CUBE_PRIORITY (-10)

static gboolean
cube_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_CUBE_PRIORITY);
	return TRUE;
}

static void
cube_deactivate(GowlModule *base)
{
	GowlModuleCube *mod = GOWL_MODULE_CUBE(base);

	cube_end_all(mod);
	g_clear_pointer(&mod->gl, gowl_cube_gl_free);
	mod->gl_tried = FALSE;
}

static const gchar *cube_name(GowlModule *m)        { return "cube"; }
static const gchar *cube_version(GowlModule *m)     { return "0.1.0"; }
static const gchar *cube_description(GowlModule *m)
{
	return "Rotates the desktop as a solid when the tag changes";
}

static void
cube_finalize(GObject *object)
{
	GowlModuleCube *mod = GOWL_MODULE_CUBE(object);

	cube_deactivate(GOWL_MODULE(object));
	g_weak_ref_clear(&mod->compositor);
	G_OBJECT_CLASS(gowl_module_cube_parent_class)->finalize(object);
}

static void
gowl_module_cube_class_init(GowlModuleCubeClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = cube_activate;
	mod->deactivate      = cube_deactivate;
	mod->get_name        = cube_name;
	mod->get_description = cube_description;
	mod->get_version     = cube_version;
	G_OBJECT_CLASS(klass)->finalize = cube_finalize;
}

static void
gowl_module_cube_init(GowlModuleCube *mod)
{
	g_weak_ref_init(&mod->compositor, NULL);
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_CUBE_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_CUBE;
}
