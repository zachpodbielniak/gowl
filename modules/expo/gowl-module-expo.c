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
 * Expo: every tag at once, as a grid, and the camera pulling back out of
 * the one you are on to show you.
 *
 * It shares the cube's whole apparatus -- capture a tag's desktop into a
 * texture, park an opaque sheet over the output, draw textured quads --
 * and differs only in where the quads go.  That is why both are small:
 * all of it lives in the core effect layer (src/fx).
 *
 * TWO THINGS MAKE IT FEEL RIGHT rather than merely work.
 *
 * The grid moves as ONE object under a single transform, so opening reads
 * as a camera pulling back from the desktop rather than as nine pictures
 * flying into formation.  And at either end the transform is exact: the
 * tile you came from is precisely the screen, so the overview opens out
 * of the live desktop and closes back into the tile you picked with no
 * cut at either end.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-expo"

#include "gowl-expo-layout.h"

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "config/gowl-config.h"
#include "boxed/gowl-color.h"
#include "fx/gowl-fx.h"
#include "util/gowl-easing.h"
#include "interfaces/gowl-scene-effect.h"
#include "interfaces/gowl-gesture-handler.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-keybind-handler.h"
#include "interfaces/gowl-mouse-handler.h"
#include "interfaces/gowl-shutdown-handler.h"

#include <math.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>

#include <gmodule.h>

/* Tiles are shown at a third of the screen or less, so half resolution is
 * indistinguishable and costs a quarter of the memory.  The tag the
 * overview opens out of is the exception: it fills the screen at the
 * moment the effect starts, and any softness there would be a visible
 * blink. */
#define GOWL_EXPO_TILE_DIVISOR 2

/* Four fingers, so it cannot be confused with the cube's three. */
#define GOWL_EXPO_SWIPE_FINGERS 4
/* Travel, in touchpad pixels, for a full open. */
#define GOWL_EXPO_SWIPE_TRAVEL 260.0

#define GOWL_TYPE_MODULE_EXPO (gowl_module_expo_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleExpo, gowl_module_expo, GOWL, MODULE_EXPO,
                     GowlModule)

/**
 * GowlExpoRun:
 *
 * The overview on one output.  Per-monitor because tags are: two screens
 * showing different tags each have their own set to lay out.
 */
typedef struct {
	GowlMonitor   *monitor;      /* unowned */
	GowlFxSheet   *sheet;
	GowlExpoLayout layout;

	/* tile index -> tag index (0-based).  They differ as soon as
	 * expo-hide-empty drops a tag out of the grid. */
	gint           tag_of[GOWL_EXPO_MAX_TAGS];
	GowlFxTexture  tile[GOWL_EXPO_MAX_TAGS];

	gint           anchor;       /* the tile the zoom pivots on */
	gint           selected;
	gboolean       closing;

	gint64         start_us;
	gint64         dur_us;
	gdouble        from_progress;
	gdouble        to_progress;
	gdouble        progress;

	/* A four-finger swipe driving the open directly. */
	gboolean       scrubbing;
	gdouble        scrub_accum;
} GowlExpoRun;

struct _GowlModuleExpo {
	GowlModule  parent_instance;
	GWeakRef    compositor;
	GowlFxGl   *gl;
	gboolean    gl_tried;
	GList      *runs;
	gboolean    capturing;

	gboolean      gesture_active;
	GowlMonitor  *gesture_monitor;
};

static void expo_effect_init(GowlSceneEffectInterface *iface);
static void expo_gesture_init(GowlGestureHandlerInterface *iface);
static void expo_ipc_init(GowlIpcHandlerInterface *iface);
static void expo_key_init(GowlKeybindHandlerInterface *iface);
static void expo_mouse_init(GowlMouseHandlerInterface *iface);
static void expo_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleExpo, gowl_module_expo, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, expo_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_GESTURE_HANDLER, expo_gesture_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER, expo_ipc_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_KEYBIND_HANDLER, expo_key_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_MOUSE_HANDLER, expo_mouse_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, expo_shutdown_init))

/* ── Plumbing ────────────────────────────────────────────────────── */

static GowlCompositor *
expo_compositor(GowlModuleExpo *mod)
{
	GowlCompositor *self = g_weak_ref_get(&mod->compositor);

	if (self != NULL)
		g_object_unref(self);
	return self;
}

static void
expo_bind(GowlModuleExpo *mod, GowlCompositor *self)
{
	GowlCompositor *held = g_weak_ref_get(&mod->compositor);

	if (held != self)
		g_weak_ref_set(&mod->compositor, self);
	g_clear_object(&held);
}

static void
expo_ensure_gl(GowlModuleExpo *mod, GowlCompositor *self)
{
	if (mod->gl != NULL || mod->gl_tried)
		return;
	if (self == NULL || self->renderer == NULL)
		return;

	mod->gl_tried = TRUE;
	mod->gl = gowl_fx_gl_new(self->renderer);
}

static gboolean
expo_enabled(GowlModuleExpo *mod, GowlCompositor *self)
{
	return self != NULL
	       && self->config != NULL
	       && !self->locked
	       && gowl_config_get_expo(self->config)
	       && mod->gl != NULL;
}

static GowlExpoRun *
expo_run_for(GowlModuleExpo *mod, GowlMonitor *m)
{
	GList *l;

	for (l = mod->runs; l != NULL; l = l->next) {
		GowlExpoRun *run = l->data;

		if (run->monitor == m)
			return run;
	}
	return NULL;
}

static void
expo_run_free(GowlModuleExpo *mod, GowlExpoRun *run)
{
	gint i;

	if (run == NULL)
		return;
	g_clear_pointer(&run->sheet, gowl_fx_sheet_free);
	for (i = 0; i < GOWL_EXPO_MAX_TAGS; i++)
		gowl_fx_texture_drop(mod->gl, &run->tile[i]);
	g_free(run);
}

static void
expo_run_end(GowlModuleExpo *mod, GowlExpoRun *run)
{
	mod->runs = g_list_remove(mod->runs, run);
	expo_run_free(mod, run);
}

static void
expo_end_all(GowlModuleExpo *mod)
{
	while (mod->runs != NULL)
		expo_run_end(mod, mod->runs->data);
}

/* ── Capture ─────────────────────────────────────────────────────── */

static gboolean
expo_capture_tile(GowlModuleExpo *mod, GowlCompositor *self, GowlMonitor *m,
                   gint tag_index, GowlFxTexture *out, gint divisor)
{
	GowlFxVis *vis;
	gboolean   ok;
	gint       i;

	mod->capturing = TRUE;
	vis = gowl_fx_vis_begin();

	/* The bar and notifications stay live above the overview, so they
	 * must not be baked into every tile as well. */
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_TOP, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_BLOCK, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_OVERLAY, TRUE);
	for (i = 0; i < 4; i++) {
		if (self->rec_indicator[i] != NULL)
			gowl_fx_vis_set(vis, &self->rec_indicator[i]->node, FALSE);
	}
	gowl_fx_vis_show_tags(vis, self, m, 1u << tag_index);

	ok = gowl_fx_capture(mod->gl, self, m, out, divisor);

	gowl_fx_vis_restore(vis);
	mod->capturing = FALSE;
	return ok;
}

/* Whether any client lives on @tag_index for this output. */
static gboolean
expo_tag_has_clients(GowlCompositor *self, GowlMonitor *m, gint tag_index)
{
	GList *l;

	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = l->data;

		if (c->mon == m && !gowl_fx_client_is_pinned(c)
		    && (c->tags & (1u << tag_index)) != 0)
			return TRUE;
	}
	return FALSE;
}

/* ── Opening and closing ─────────────────────────────────────────── */

static gint
expo_current_tag(GowlMonitor *m)
{
	guint32 mask = m->tagset[m->seltags];
	gint    i;

	for (i = 0; i < GOWL_EXPO_MAX_TAGS; i++) {
		if ((mask & (1u << i)) != 0)
			return i;
	}
	return 0;
}

static void
expo_animate(GowlExpoRun *run, gdouble to, gint duration_ms, gint64 now_us)
{
	run->from_progress = run->progress;
	run->to_progress   = CLAMP(to, 0.0, 1.0);
	run->start_us      = now_us;
	/*
	 * Shorten the trip in proportion to how far there is to go, so
	 * closing from a half-open overview does not take as long as closing
	 * from a fully open one.  Floored, because an instant jump reads as
	 * a glitch.
	 */
	run->dur_us = (gint64)(MAX(0, duration_ms) * 1000.0
	                        * fabs(run->to_progress - run->from_progress));
	if (run->dur_us < 80000)
		run->dur_us = 80000;
	run->scrubbing = FALSE;
}

static GowlExpoRun *
expo_open(GowlModuleExpo *mod, GowlCompositor *self, GowlMonitor *m,
           gboolean scrubbing)
{
	GowlExpoRun *run;
	gint  wanted, current, count, i;
	gint  divisor;

	if (m == NULL || m->wlr_output == NULL || m->scene_output == NULL
	    || m->m.width <= 0 || m->m.height <= 0)
		return NULL;

	/* Same reason as the cube: the projection assumes screen-aligned
	 * axes, and a rotated output does not have them. */
	if (m->wlr_output->transform != WL_OUTPUT_TRANSFORM_NORMAL)
		return NULL;

	run = expo_run_for(mod, m);
	if (run != NULL)
		return run;

	wanted  = gowl_config_get_expo_tags(self->config);
	current = expo_current_tag(m);

	run = g_new0(GowlExpoRun, 1);
	run->monitor = m;

	/*
	 * Which tags become tiles.  The tag being viewed is ALWAYS one, even
	 * with expo-hide-empty on and nothing on it: the overview has to open
	 * out of somewhere, and that somewhere is where the user is.
	 */
	count = 0;
	for (i = 0; i < wanted && count < GOWL_EXPO_MAX_TAGS; i++) {
		if (gowl_config_get_expo_hide_empty(self->config)
		    && i != current && !expo_tag_has_clients(self, m, i))
			continue;
		run->tag_of[count] = i;
		if (i == current)
			run->anchor = count;
		count++;
	}
	if (count == 0) {
		g_free(run);
		return NULL;
	}

	if (!gowl_expo_layout_build(&run->layout, count,
	                            gowl_config_get_expo_columns(self->config),
	                            (gdouble)m->m.width, (gdouble)m->m.height,
	                            gowl_config_get_expo_gap(self->config))) {
		g_free(run);
		return NULL;
	}

	run->selected = run->anchor;
	for (i = 0; i < count; i++) {
		divisor = (i == run->anchor) ? 1 : GOWL_EXPO_TILE_DIVISOR;
		if (!expo_capture_tile(mod, self, m, run->tag_of[i],
		                       &run->tile[i], divisor)
		    && i == run->anchor) {
			/* Without the anchor at full size the overview cannot open
			 * seamlessly, which is the whole effect. */
			expo_run_free(mod, run);
			return NULL;
		}
	}

	run->sheet = gowl_fx_sheet_new(self, m, GOWL_FX_SHEET_NONE);
	if (run->sheet == NULL) {
		expo_run_free(mod, run);
		return NULL;
	}

	run->progress = 0.0;
	mod->runs = g_list_prepend(mod->runs, run);

	if (scrubbing) {
		run->scrubbing = TRUE;
	} else {
		expo_animate(run, 1.0,
		             gowl_config_get_expo_duration(self->config),
		             g_get_monotonic_time());
	}

	if (m->wlr_output != NULL)
		wlr_output_schedule_frame(m->wlr_output);
	return run;
}

/*
 * Close, zooming back into the tile that is selected rather than the one
 * we came from.
 *
 * Re-capturing the destination at full size first is not fussiness: it is
 * stored at half size like every other tile, and the last frame of the
 * close is that tile filling the entire screen.  The capture is also
 * fresher than the one taken on open, which is free accuracy.
 */
static void
expo_close(GowlModuleExpo *mod, GowlCompositor *self, GowlExpoRun *run,
            gboolean commit)
{
	gint tag;

	if (run == NULL || run->closing)
		return;

	if (!commit)
		run->selected = run->anchor;

	run->anchor  = CLAMP(run->selected, 0, run->layout.count - 1);
	run->closing = TRUE;
	tag = run->tag_of[run->anchor];

	expo_capture_tile(mod, self, run->monitor, tag, &run->tile[run->anchor], 1);

	if (commit && run->monitor->tagset[run->monitor->seltags] != (1u << tag)) {
		/*
		 * Switch the tag NOW rather than when the animation finishes.
		 * The sheet is covering the output for the whole close, so
		 * nothing of the change is visible until it lifts -- and doing
		 * it now means the desktop underneath is already correct when
		 * the last frame of the zoom matches it exactly.
		 */
		gowl_compositor_view_tags(self, run->monitor, 1u << tag);
	}

	expo_animate(run, 0.0, gowl_config_get_expo_duration(self->config),
	             g_get_monotonic_time());

	if (run->monitor->wlr_output != NULL)
		wlr_output_schedule_frame(run->monitor->wlr_output);
}

static gboolean
expo_toggle(GowlModuleExpo *mod, GowlCompositor *self, GowlMonitor *m)
{
	GowlExpoRun *run;

	expo_ensure_gl(mod, self);
	if (!expo_enabled(mod, self) || m == NULL)
		return FALSE;

	run = expo_run_for(mod, m);
	if (run != NULL) {
		expo_close(mod, self, run, TRUE);
		return TRUE;
	}
	return expo_open(mod, self, m, FALSE) != NULL;
}

/* ── Drawing ─────────────────────────────────────────────────────── */

static void
expo_backdrop_rgb(GowlCompositor *self, gfloat *out)
{
	const gchar *spec = self->config != NULL
		? gowl_config_get_expo_backdrop_color(self->config) : NULL;
	GowlColor *color = spec != NULL ? gowl_color_new_from_hex(spec) : NULL;

	if (color == NULL) {
		out[0] = 0.071f; out[1] = 0.078f; out[2] = 0.122f;
		return;
	}
	out[0] = (gfloat)color->r;
	out[1] = (gfloat)color->g;
	out[2] = (gfloat)color->b;
	gowl_color_free(color);
}

static gboolean
expo_draw(GowlModuleExpo *mod, GowlCompositor *self, GowlExpoRun *run)
{
	struct wlr_buffer *buffer;
	GowlFxPass *pass;
	GowlFxQuad  quad;
	gfloat      ortho[16], clear[4], backdrop[3];
	gdouble     scale, off_x, off_y;
	gdouble     dim, corner;
	gint        w, h, i;

	gowl_fx_sheet_get_size(run->sheet, &w, &h);
	if (w <= 0 || h <= 0)
		return FALSE;

	buffer = gowl_fx_sheet_acquire(run->sheet);
	if (buffer == NULL)
		return FALSE;

	pass = gowl_fx_pass_begin(mod->gl, buffer);
	if (pass == NULL) {
		wlr_buffer_unlock(buffer);
		return FALSE;
	}

	expo_backdrop_rgb(self, backdrop);
	clear[0] = backdrop[0] * 0.35f;
	clear[1] = backdrop[1] * 0.35f;
	clear[2] = backdrop[2] * 0.35f;
	clear[3] = 1.0f;
	gowl_fx_pass_clear(pass, clear);
	if (run->progress > 0.0)
		gowl_fx_pass_backdrop(pass, backdrop, (gfloat)run->progress);

	/*
	 * Everything below is in OUTPUT PIXELS, which is what the layout
	 * speaks.  The sheet's buffer may be larger on a scaled output, so
	 * the ortho is built from the logical size and the viewport does the
	 * rest.
	 */
	gowl_fx_mat4_ortho(ortho, (gdouble)run->monitor->m.width,
	                    (gdouble)run->monitor->m.height);
	gowl_expo_layout_transform(&run->layout, run->anchor, run->progress,
	                            (gdouble)run->monitor->m.width,
	                            (gdouble)run->monitor->m.height,
	                            &scale, &off_x, &off_y);

	/* Everything decorative fades in with the zoom, so the closed frame
	 * is the plain desktop and there is no cut at either end. */
	dim    = gowl_config_get_expo_dim(self->config) * run->progress;
	corner = gowl_config_get_expo_corner(self->config) * run->progress;

	for (i = 0; i < run->layout.count; i++) {
		gdouble x = run->layout.cell[i].x * scale + off_x;
		gdouble y = run->layout.cell[i].y * scale + off_y;
		gdouble cw = run->layout.cell[i].width * scale;
		gdouble ch = run->layout.cell[i].height * scale;
		gfloat  pos[12];
		gboolean chosen = (i == run->selected);
		gdouble  bright = chosen ? 1.0 : 1.0 - dim;

		/* Entirely off screen: skip rather than trust the rasteriser,
		 * because at the closed end the scale is large and the far tiles
		 * are a long way out. */
		if (x + cw < -cw || y + ch < -ch
		    || x > (gdouble)run->monitor->m.width + cw
		    || y > (gdouble)run->monitor->m.height + ch)
			continue;

		pos[0]  = (gfloat)x;        pos[1]  = (gfloat)y;        pos[2]  = 0.0f;
		pos[3]  = (gfloat)x;        pos[4]  = (gfloat)(y + ch); pos[5]  = 0.0f;
		pos[6]  = (gfloat)(x + cw); pos[7]  = (gfloat)y;        pos[8]  = 0.0f;
		pos[9]  = (gfloat)(x + cw); pos[10] = (gfloat)(y + ch); pos[11] = 0.0f;

		gowl_fx_quad_init(&quad);
		quad.mvp     = ortho;
		quad.pos     = pos;
		quad.texture = run->tile[i].tex;
		quad.tint[0] = (gfloat)bright;
		quad.tint[1] = (gfloat)bright;
		quad.tint[2] = (gfloat)bright;
		quad.base[0] = backdrop[0] * 1.6f;
		quad.base[1] = backdrop[1] * 1.6f;
		quad.base[2] = backdrop[2] * 1.6f;
		quad.corner  = (gfloat)corner;
		/* The selection is a lit border rather than a colour wash: it
		 * has to be obvious without changing what the thumbnail of the
		 * desktop actually looks like. */
		quad.edge       = chosen ? (gfloat)(0.35 * run->progress) : 0.0f;
		quad.edge_width = 0.012f;
		gowl_fx_pass_quad(pass, &quad);
	}

	gowl_fx_pass_end(pass);
	gowl_fx_sheet_present(run->sheet, buffer);
	wlr_buffer_unlock(buffer);
	return TRUE;
}

static gboolean
expo_frame(GowlSceneEffect *effect, GowlCompositor *self, GowlMonitor *m,
            gint64 now)
{
	GowlModuleExpo *mod = GOWL_MODULE_EXPO(effect);
	GowlExpoRun    *run;

	expo_bind(mod, self);

	if (mod->capturing || m == NULL)
		return FALSE;

	run = expo_run_for(mod, m);
	if (run == NULL)
		return FALSE;

	if (!expo_enabled(mod, self)) {
		expo_run_end(mod, run);
		return TRUE;
	}

	if (!run->scrubbing) {
		gdouble t = run->dur_us > 0
			? CLAMP((gdouble)(now - run->start_us) / (gdouble)run->dur_us,
			        0.0, 1.0)
			: 1.0;
		gdouble e = gowl_easing_eval(
			gowl_config_get_expo_curve(self->config), t);

		run->progress = run->from_progress
			+ (run->to_progress - run->from_progress) * e;

		if (t >= 1.0 && run->closing) {
			/* Done: the sheet lifts onto a desktop that already matches
			 * the last frame exactly. */
			expo_run_end(mod, run);
			return TRUE;
		}
	}

	if (!expo_draw(mod, self, run)) {
		expo_run_end(mod, run);
		return TRUE;
	}
	return TRUE;
}

/* ── Input ───────────────────────────────────────────────────────── */

/*
 * Expo claims a client event only while it is on screen, and only the
 * reveal: a tag change made from inside the overview would otherwise fade
 * windows in behind the sheet for nobody to see.
 */
static gboolean
expo_client_event(GowlSceneEffect *effect, GowlCompositor *self, GowlClient *c,
                   GowlSceneEffectEvent event, const struct wlr_box *previous,
                   gboolean interactive)
{
	GowlModuleExpo *mod = GOWL_MODULE_EXPO(effect);

	expo_bind(mod, self);
	return event == GOWL_SCENE_EFFECT_REVEAL && c != NULL
	       && expo_run_for(mod, c->mon) != NULL;
}

static GowlExpoRun *
expo_focused_run(GowlModuleExpo *mod, GowlCompositor **out_self)
{
	GowlCompositor *self = expo_compositor(mod);

	if (out_self != NULL)
		*out_self = self;
	if (self == NULL || self->selmon == NULL)
		return NULL;
	return expo_run_for(mod, self->selmon);
}

static gboolean
expo_handle_key(GowlKeybindHandler *handler, guint modifiers, guint keysym,
                 gboolean pressed)
{
	GowlModuleExpo *mod = GOWL_MODULE_EXPO(handler);
	GowlCompositor *self = NULL;
	GowlExpoRun    *run = expo_focused_run(mod, &self);

	if (!pressed || run == NULL || run->closing)
		return FALSE;

	switch (keysym) {
	case XKB_KEY_Escape:
		expo_close(mod, self, run, FALSE);
		return TRUE;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		expo_close(mod, self, run, TRUE);
		return TRUE;
	case XKB_KEY_Left:  case XKB_KEY_h:
		run->selected = gowl_expo_layout_step(&run->layout, run->selected, -1, 0);
		break;
	case XKB_KEY_Right: case XKB_KEY_l:
		run->selected = gowl_expo_layout_step(&run->layout, run->selected, 1, 0);
		break;
	case XKB_KEY_Up:    case XKB_KEY_k:
		run->selected = gowl_expo_layout_step(&run->layout, run->selected, 0, -1);
		break;
	case XKB_KEY_Down:  case XKB_KEY_j:
		run->selected = gowl_expo_layout_step(&run->layout, run->selected, 0, 1);
		break;
	default:
		/* A digit picks that tag directly and commits, which is the
		 * fastest path and the one muscle memory reaches for. */
		if (keysym >= XKB_KEY_1 && keysym <= XKB_KEY_9) {
			gint want = (gint)(keysym - XKB_KEY_1);
			gint i;

			for (i = 0; i < run->layout.count; i++) {
				if (run->tag_of[i] == want) {
					run->selected = i;
					expo_close(mod, self, run, TRUE);
					return TRUE;
				}
			}
			return TRUE;
		}
		/*
		 * Everything else is swallowed while the overview is up.  It is
		 * a modal sheet over the whole output, so a keystroke reaching
		 * the window behind it would go somewhere the user cannot see.
		 * Configured keybinds are consulted before modules, so the
		 * user's own binds -- Super+1, volume keys -- still work.
		 */
		return TRUE;
	}

	if (run->monitor->wlr_output != NULL)
		wlr_output_schedule_frame(run->monitor->wlr_output);
	return TRUE;
}

/* Which tile the pointer is over, or -1. */
static gint
expo_tile_at_pointer(GowlCompositor *self, GowlExpoRun *run)
{
	gdouble scale, off_x, off_y;

	if (self->wlr_cursor == NULL)
		return -1;

	gowl_expo_layout_transform(&run->layout, run->anchor, run->progress,
	                            (gdouble)run->monitor->m.width,
	                            (gdouble)run->monitor->m.height,
	                            &scale, &off_x, &off_y);
	return gowl_expo_layout_at(&run->layout, scale, off_x, off_y,
	                            self->wlr_cursor->x - (gdouble)run->monitor->m.x,
	                            self->wlr_cursor->y - (gdouble)run->monitor->m.y);
}

static gboolean
expo_handle_motion(GowlMouseHandler *handler, gdouble x, gdouble y)
{
	GowlModuleExpo *mod = GOWL_MODULE_EXPO(handler);
	GowlCompositor *self = NULL;
	GowlExpoRun    *run = expo_focused_run(mod, &self);
	gint tile;

	if (run == NULL || run->closing)
		return FALSE;

	tile = expo_tile_at_pointer(self, run);
	if (tile >= 0 && tile != run->selected) {
		run->selected = tile;
		if (run->monitor->wlr_output != NULL)
			wlr_output_schedule_frame(run->monitor->wlr_output);
	}
	/* Not claimed: the pointer still belongs to whatever is beneath, and
	 * the overview is about to go away anyway. */
	return FALSE;
}

static gboolean
expo_handle_button(GowlMouseHandler *handler, guint button, guint state,
                    guint modifiers)
{
	GowlModuleExpo *mod = GOWL_MODULE_EXPO(handler);
	GowlCompositor *self = NULL;
	GowlExpoRun    *run = expo_focused_run(mod, &self);
	gint tile;

	if (run == NULL || run->closing || state == 0)
		return FALSE;

	tile = expo_tile_at_pointer(self, run);
	if (tile >= 0) {
		run->selected = tile;
		expo_close(mod, self, run, TRUE);
	} else {
		/* Clicking the gap dismisses without changing tag, the way a
		 * click outside a dialog does. */
		expo_close(mod, self, run, FALSE);
	}
	/* Claimed: a click meant for the overview must not also reach the
	 * window whose thumbnail happens to be under the pointer. */
	return TRUE;
}

/* ── Gesture ─────────────────────────────────────────────────────── */

static gboolean
expo_gesture_begin(GowlGestureHandler *handler, gpointer compositor,
                    guint fingers)
{
	GowlModuleExpo *mod = GOWL_MODULE_EXPO(handler);
	GowlCompositor *self = compositor;

	if (fingers != GOWL_EXPO_SWIPE_FINGERS)
		return FALSE;

	expo_bind(mod, self);
	expo_ensure_gl(mod, self);
	if (!expo_enabled(mod, self) || self->selmon == NULL)
		return FALSE;

	mod->gesture_active  = TRUE;
	mod->gesture_monitor = self->selmon;
	return TRUE;
}

static gboolean
expo_gesture_update(GowlGestureHandler *handler, gpointer compositor,
                     gdouble dx, gdouble dy)
{
	GowlModuleExpo *mod = GOWL_MODULE_EXPO(handler);
	GowlCompositor *self = compositor;
	GowlExpoRun    *run;

	if (!mod->gesture_active || mod->gesture_monitor == NULL)
		return FALSE;

	run = expo_run_for(mod, mod->gesture_monitor);
	if (run == NULL) {
		/* Upwards opens.  Lazily, on real travel, so an abandoned
		 * four-finger gesture costs no captures. */
		if (dy > -6.0)
			return TRUE;
		run = expo_open(mod, self, mod->gesture_monitor, TRUE);
		if (run == NULL)
			return TRUE;
	}
	if (run->closing)
		return TRUE;

	run->scrubbing = TRUE;
	run->scrub_accum += -dy;
	run->progress = CLAMP(run->scrub_accum / GOWL_EXPO_SWIPE_TRAVEL, 0.0, 1.0);

	if (run->monitor->wlr_output != NULL)
		wlr_output_schedule_frame(run->monitor->wlr_output);
	return TRUE;
}

static gboolean
expo_gesture_end(GowlGestureHandler *handler, gpointer compositor,
                  gboolean cancelled)
{
	GowlModuleExpo *mod = GOWL_MODULE_EXPO(handler);
	GowlCompositor *self = compositor;
	GowlExpoRun    *run;
	gboolean claimed = mod->gesture_active;

	if (!claimed)
		return FALSE;

	mod->gesture_active = FALSE;
	run = mod->gesture_monitor != NULL
		? expo_run_for(mod, mod->gesture_monitor) : NULL;
	mod->gesture_monitor = NULL;

	if (run == NULL || run->closing)
		return claimed;

	run->scrubbing = FALSE;
	/* Past halfway it settles open; short of it, it goes back. */
	if (!cancelled && run->progress >= 0.5) {
		expo_animate(run, 1.0,
		             gowl_config_get_expo_duration(self->config),
		             g_get_monotonic_time());
	} else {
		expo_close(mod, self, run, FALSE);
	}

	if (run->monitor != NULL && run->monitor->wlr_output != NULL)
		wlr_output_schedule_frame(run->monitor->wlr_output);
	return claimed;
}

/* ── Commands ────────────────────────────────────────────────────── */

/*
 * The overview's entry points, reachable from a keybind
 * (`{ action: ipc_command, arg: "expo" }'), from the IPC socket, and from
 * an embedder.  A module .so cannot export a function for the compositor
 * to call, so it exports a name instead.
 */
static gchar *
expo_handle_command(GowlIpcHandler *handler, const gchar *command,
                     const gchar *args)
{
	GowlModuleExpo *mod = GOWL_MODULE_EXPO(handler);
	GowlCompositor *self = expo_compositor(mod);
	GowlExpoRun    *run;

	if (self == NULL || command == NULL)
		return NULL;

	if (g_strcmp0(command, "expo") == 0
	    || g_strcmp0(command, "expo-toggle") == 0)
		return g_strdup(expo_toggle(mod, self, self->selmon)
		                ? "OK expo toggled" : "ERROR expo unavailable");

	if (g_strcmp0(command, "expo-open") == 0) {
		expo_ensure_gl(mod, self);
		if (!expo_enabled(mod, self))
			return g_strdup("ERROR expo unavailable");
		return g_strdup(expo_open(mod, self, self->selmon, FALSE) != NULL
		                ? "OK expo open" : "ERROR expo unavailable");
	}

	if (g_strcmp0(command, "expo-close") == 0) {
		run = self->selmon != NULL ? expo_run_for(mod, self->selmon) : NULL;
		if (run == NULL)
			return g_strdup("OK expo closed");
		expo_close(mod, self, run, g_strcmp0(args, "cancel") != 0);
		return g_strdup("OK expo closing");
	}

	if (g_strcmp0(command, "expo-select") == 0) {
		gint tag, i;

		run = self->selmon != NULL ? expo_run_for(mod, self->selmon) : NULL;
		if (run == NULL || args == NULL)
			return g_strdup("ERROR expo not open");

		tag = (gint)g_ascii_strtoll(args, NULL, 10) - 1;
		for (i = 0; i < run->layout.count; i++) {
			if (run->tag_of[i] == tag) {
				run->selected = i;
				expo_close(mod, self, run, TRUE);
				return g_strdup("OK expo selected");
			}
		}
		return g_strdup("ERROR no such tag in the overview");
	}

	return NULL;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

static void
expo_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                      GowlMonitor *m)
{
	GowlModuleExpo *mod = GOWL_MODULE_EXPO(effect);
	GowlExpoRun    *run = expo_run_for(mod, m);

	if (run != NULL)
		expo_run_end(mod, run);
	if (mod->gesture_monitor == m) {
		mod->gesture_active = FALSE;
		mod->gesture_monitor = NULL;
	}
}

static void
expo_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	GowlModuleExpo *mod = GOWL_MODULE_EXPO(effect);

	expo_end_all(mod);
	g_clear_pointer(&mod->gl, gowl_fx_gl_free);
	mod->gl_tried = FALSE;
	mod->gesture_active = FALSE;
	mod->gesture_monitor = NULL;
	g_weak_ref_set(&mod->compositor, NULL);
}

static void expo_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_event    = expo_client_event;
	iface->frame           = expo_frame;
	iface->monitor_removed = expo_monitor_removed;
	iface->finish          = expo_finish;
}
static void expo_gesture_init(GowlGestureHandlerInterface *iface)
{
	iface->swipe_begin  = expo_gesture_begin;
	iface->swipe_update = expo_gesture_update;
	iface->swipe_end    = expo_gesture_end;
}
static void expo_ipc_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = expo_handle_command;
}
static void expo_key_init(GowlKeybindHandlerInterface *iface)
{
	iface->handle_key = expo_handle_key;
}
static void expo_mouse_init(GowlMouseHandlerInterface *iface)
{
	iface->handle_motion = expo_handle_motion;
	iface->handle_button = expo_handle_button;
}
static void
expo_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	expo_finish(GOWL_SCENE_EFFECT(handler), compositor);
}
static void expo_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = expo_shutdown;
}

/*
 * Ahead of the animation module for the same reason the cube is: it needs
 * to claim the reveal of windows on a tag it is animating to.  Behind the
 * cube, because a tag change made from the overview should be the
 * overview's zoom rather than the cube's rotation -- and the overview
 * claims that reveal first only because it is already on screen.
 */
#define GOWL_EXPO_PRIORITY (-8)

static gboolean
expo_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_EXPO_PRIORITY);
	return TRUE;
}

static void
expo_deactivate(GowlModule *base)
{
	GowlModuleExpo *mod = GOWL_MODULE_EXPO(base);

	expo_end_all(mod);
	g_clear_pointer(&mod->gl, gowl_fx_gl_free);
	mod->gl_tried = FALSE;
}

static const gchar *expo_name(GowlModule *m)    { return "expo"; }
static const gchar *expo_version(GowlModule *m) { return "0.1.0"; }
static const gchar *expo_description(GowlModule *m)
{
	return "Shows every tag at once as a grid of live thumbnails";
}

static void
expo_finalize(GObject *object)
{
	GowlModuleExpo *mod = GOWL_MODULE_EXPO(object);

	expo_deactivate(GOWL_MODULE(object));
	g_weak_ref_clear(&mod->compositor);
	G_OBJECT_CLASS(gowl_module_expo_parent_class)->finalize(object);
}

static void
gowl_module_expo_class_init(GowlModuleExpoClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = expo_activate;
	mod->deactivate      = expo_deactivate;
	mod->get_name        = expo_name;
	mod->get_description = expo_description;
	mod->get_version     = expo_version;
	G_OBJECT_CLASS(klass)->finalize = expo_finalize;
}

static void
gowl_module_expo_init(GowlModuleExpo *mod)
{
	g_weak_ref_init(&mod->compositor, NULL);
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_EXPO_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_EXPO;
}
