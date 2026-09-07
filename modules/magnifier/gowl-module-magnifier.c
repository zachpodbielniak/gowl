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
 * Screen zoom.  Hold the modifier, turn the wheel, and the whole output
 * magnifies around the pointer.
 *
 * THE ONE THING TO UNDERSTAND HERE is that this effect draws on the
 * screen it is photographing.  Every frame it captures the output and
 * puts the result back on that same output, so it has to take its own
 * sheet out of the scene before each capture -- otherwise it photographs
 * its own previous frame, and the picture recedes into itself the way a
 * camera pointed at its own monitor does.  gowl_fx_sheet_set_visible()
 * exists for this.
 *
 * It is also the only effect that captures EVERY frame rather than once
 * at the start.  That is inherent: the thing being magnified is live.  It
 * costs one extra scene render per frame, and only while zoomed in --
 * at 1.0 the module puts the sheet away entirely and costs nothing.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-magnifier"

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "config/gowl-config.h"
#include "fx/gowl-fx.h"
#include "interfaces/gowl-scene-effect.h"
#include "interfaces/gowl-mouse-handler.h"
#include "interfaces/gowl-keybind-handler.h"
#include "interfaces/gowl-shutdown-handler.h"

#include <math.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>

#include <gmodule.h>

/* Below this the magnification is not worth a capture per frame, and the
 * module puts everything away. */
#define GOWL_MAGNIFIER_IDLE_ZOOM 1.002

#define GOWL_TYPE_MODULE_MAGNIFIER (gowl_module_magnifier_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleMagnifier, gowl_module_magnifier,
                     GOWL, MODULE_MAGNIFIER, GowlModule)

/**
 * GowlMagnifierState:
 *
 * One output's zoom.  Per-monitor because the pointer is only ever on
 * one of them, and magnifying the screen the user is not looking at
 * would be both surprising and a capture per frame for nothing.
 */
typedef struct {
	GowlMonitor  *monitor;      /* unowned */
	GowlFxSheet  *sheet;
	GowlFxTexture frame;

	/* Where the zoom is, and where it is heading.  Two values rather than
	 * one so a wheel click is a smooth move instead of a jump. */
	gdouble       zoom;
	gdouble       target_zoom;
	gdouble       cx, cy;             /* centre, in output pixels */
	gdouble       target_cx, target_cy;
	gboolean      filter_smooth;      /* what the texture is set to now */
	gint64        last_us;
} GowlMagnifierState;

struct _GowlModuleMagnifier {
	GowlModule  parent_instance;
	GWeakRef    compositor;
	GowlFxGl   *gl;
	gboolean    gl_tried;
	GList      *states;      /* GowlMagnifierState* */
	gboolean    capturing;
};

static void magnifier_effect_init(GowlSceneEffectInterface *iface);
static void magnifier_mouse_init(GowlMouseHandlerInterface *iface);
static void magnifier_key_init(GowlKeybindHandlerInterface *iface);
static void magnifier_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleMagnifier, gowl_module_magnifier,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, magnifier_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_MOUSE_HANDLER, magnifier_mouse_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_KEYBIND_HANDLER, magnifier_key_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, magnifier_shutdown_init))

/* ── Plumbing ────────────────────────────────────────────────────── */

static GowlCompositor *
magnifier_compositor(GowlModuleMagnifier *mod)
{
	GowlCompositor *self = g_weak_ref_get(&mod->compositor);

	/* Borrowed for the length of the call; the compositor outlives every
	 * module, and returning a ref would make every call site remember to
	 * drop it. */
	if (self != NULL)
		g_object_unref(self);
	return self;
}

static void
magnifier_bind(GowlModuleMagnifier *mod, GowlCompositor *self)
{
	GowlCompositor *held = g_weak_ref_get(&mod->compositor);

	if (held != self)
		g_weak_ref_set(&mod->compositor, self);
	g_clear_object(&held);
}

static void
magnifier_ensure_gl(GowlModuleMagnifier *mod, GowlCompositor *self)
{
	if (mod->gl != NULL || mod->gl_tried)
		return;
	if (self == NULL || self->renderer == NULL)
		return;

	mod->gl_tried = TRUE;
	mod->gl = gowl_fx_gl_new(self->renderer);
}

static gboolean
magnifier_enabled(GowlModuleMagnifier *mod, GowlCompositor *self)
{
	return self != NULL
	       && self->config != NULL
	       && !self->locked
	       && gowl_config_get_magnifier(self->config)
	       && gowl_config_get_magnifier_max(self->config) > 1.0
	       && mod->gl != NULL;
}

static GowlMagnifierState *
magnifier_state_for(GowlModuleMagnifier *mod, GowlMonitor *m, gboolean create)
{
	GList *l;
	GowlMagnifierState *state;

	for (l = mod->states; l != NULL; l = l->next) {
		state = l->data;
		if (state->monitor == m)
			return state;
	}
	if (!create || m == NULL)
		return NULL;

	state = g_new0(GowlMagnifierState, 1);
	state->monitor     = m;
	state->zoom        = 1.0;
	state->target_zoom = 1.0;
	state->cx = state->target_cx = (gdouble)m->m.width * 0.5;
	state->cy = state->target_cy = (gdouble)m->m.height * 0.5;
	state->filter_smooth = TRUE;
	mod->states = g_list_prepend(mod->states, state);
	return state;
}

static void
magnifier_state_free(GowlModuleMagnifier *mod, GowlMagnifierState *state)
{
	if (state == NULL)
		return;
	g_clear_pointer(&state->sheet, gowl_fx_sheet_free);
	gowl_fx_texture_drop(mod->gl, &state->frame);
	g_free(state);
}

static void
magnifier_state_end(GowlModuleMagnifier *mod, GowlMagnifierState *state)
{
	mod->states = g_list_remove(mod->states, state);
	magnifier_state_free(mod, state);
}

static void
magnifier_end_all(GowlModuleMagnifier *mod)
{
	while (mod->states != NULL)
		magnifier_state_end(mod, mod->states->data);
}

/* ── Input ───────────────────────────────────────────────────────── */

/*
 * Which modifier arms the wheel.
 *
 * A scroll this module claims never reaches the focused client, so the
 * answer has to be a combination applications do not use.  An
 * unrecognised name falls back to Super rather than to "no modifier",
 * because the latter would silently make the mouse wheel stop working
 * everywhere.
 */
static guint32
magnifier_modifier_mask(GowlCompositor *self)
{
	const gchar *name = self->config != NULL
		? gowl_config_get_magnifier_modifier(self->config) : NULL;

	if (name == NULL)
		return WLR_MODIFIER_LOGO;
	if (g_ascii_strcasecmp(name, "alt") == 0
	    || g_ascii_strcasecmp(name, "mod1") == 0)
		return WLR_MODIFIER_ALT;
	if (g_ascii_strcasecmp(name, "ctrl") == 0
	    || g_ascii_strcasecmp(name, "control") == 0)
		return WLR_MODIFIER_CTRL;
	if (g_ascii_strcasecmp(name, "shift") == 0)
		return WLR_MODIFIER_SHIFT;
	if (g_ascii_strcasecmp(name, "super") != 0
	    && g_ascii_strcasecmp(name, "logo") != 0)
		g_warning("magnifier: unknown magnifier-modifier '%s'; using Super",
		          name);
	return WLR_MODIFIER_LOGO;
}

/* Keep the magnified window inside the output: panning past the edge
 * would show nothing, which reads as the magnifier being broken. */
static void
magnifier_clamp_centre(GowlMagnifierState *state, gdouble zoom)
{
	gdouble half_w = (gdouble)state->monitor->m.width * 0.5 / MAX(1.0, zoom);
	gdouble half_h = (gdouble)state->monitor->m.height * 0.5 / MAX(1.0, zoom);

	state->target_cx = CLAMP(state->target_cx, half_w,
	                         (gdouble)state->monitor->m.width - half_w);
	state->target_cy = CLAMP(state->target_cy, half_h,
	                         (gdouble)state->monitor->m.height - half_h);
}

static gboolean
magnifier_zoom_by(GowlModuleMagnifier *mod, GowlCompositor *self,
                   gdouble factor, gboolean absolute)
{
	GowlMagnifierState *state;
	gdouble max_zoom;

	if (!magnifier_enabled(mod, self) || self->selmon == NULL)
		return FALSE;

	max_zoom = gowl_config_get_magnifier_max(self->config);
	state = magnifier_state_for(mod, self->selmon, TRUE);

	state->target_zoom = absolute
		? CLAMP(factor, 1.0, max_zoom)
		: CLAMP(state->target_zoom * factor, 1.0, max_zoom);

	/*
	 * Zoom towards the pointer, the way a map does.  Anchoring on the
	 * middle of the screen instead would send whatever the user is
	 * pointing at sliding off the edge as they zoom in, which is the
	 * difference between a magnifier and a nuisance.
	 */
	if (self->wlr_cursor != NULL) {
		state->target_cx = self->wlr_cursor->x - (gdouble)state->monitor->m.x;
		state->target_cy = self->wlr_cursor->y - (gdouble)state->monitor->m.y;
	}
	magnifier_clamp_centre(state, state->target_zoom);

	if (state->monitor->wlr_output != NULL)
		wlr_output_schedule_frame(state->monitor->wlr_output);
	return TRUE;
}

static gboolean
magnifier_handle_axis(GowlMouseHandler *handler, guint axis, gdouble delta,
                       gint discrete, guint modifiers)
{
	GowlModuleMagnifier *mod = GOWL_MODULE_MAGNIFIER(handler);
	GowlCompositor *self = magnifier_compositor(mod);
	gdouble step, factor;

	if (self == NULL || axis != 0 || delta == 0.0)
		return FALSE;
	magnifier_ensure_gl(mod, self);
	if (!magnifier_enabled(mod, self))
		return FALSE;
	if ((modifiers & magnifier_modifier_mask(self)) == 0)
		return FALSE;

	step = gowl_config_get_magnifier_step(self->config);
	/* Scrolling up magnifies.  delta is positive downwards. */
	factor = delta < 0.0 ? step : 1.0 / step;
	return magnifier_zoom_by(mod, self, factor, FALSE);
}

/*
 * Panning while zoomed.
 *
 * Deliberately NOT claimed: the pointer still belongs to whatever is
 * under it, and a magnifier that stopped windows receiving motion would
 * make the desktop unusable at exactly the moment somebody needs it
 * magnified.
 */
static gboolean
magnifier_handle_motion(GowlMouseHandler *handler, gdouble x, gdouble y)
{
	GowlModuleMagnifier *mod = GOWL_MODULE_MAGNIFIER(handler);
	GowlCompositor *self = magnifier_compositor(mod);
	GowlMagnifierState *state;

	if (self == NULL || self->config == NULL
	    || !gowl_config_get_magnifier_follow_cursor(self->config))
		return FALSE;

	/* The zoomed output the pointer is actually over, if any. */
	state = NULL;
	{
		GList *l;

		for (l = mod->states; l != NULL && state == NULL; l = l->next) {
			GowlMagnifierState *s = l->data;

			if (s->target_zoom <= GOWL_MAGNIFIER_IDLE_ZOOM)
				continue;
			if (x < s->monitor->m.x || y < s->monitor->m.y
			    || x >= s->monitor->m.x + s->monitor->m.width
			    || y >= s->monitor->m.y + s->monitor->m.height)
				continue;
			state = s;
		}
	}
	if (state == NULL)
		return FALSE;

	state->target_cx = x - (gdouble)state->monitor->m.x;
	state->target_cy = y - (gdouble)state->monitor->m.y;
	magnifier_clamp_centre(state, state->target_zoom);

	if (state->monitor->wlr_output != NULL)
		wlr_output_schedule_frame(state->monitor->wlr_output);
	return FALSE;
}

/*
 * Keyboard fallbacks.
 *
 * gowl offers modules a key only after the user's own keybinds have
 * declined it, so these never shadow a configured bind -- they are there
 * so the magnifier is reachable on a machine with no touchpad and no
 * wheel, and on a keyboard where the user has not bound anything.
 */
static gboolean
magnifier_handle_key(GowlKeybindHandler *handler, guint modifiers,
                      guint keysym, gboolean pressed)
{
	GowlModuleMagnifier *mod = GOWL_MODULE_MAGNIFIER(handler);
	GowlCompositor *self = magnifier_compositor(mod);
	gdouble step;

	if (self == NULL || !pressed)
		return FALSE;
	magnifier_ensure_gl(mod, self);
	if (!magnifier_enabled(mod, self))
		return FALSE;
	if ((modifiers & magnifier_modifier_mask(self)) == 0)
		return FALSE;

	step = gowl_config_get_magnifier_step(self->config);

	switch (keysym) {
	case XKB_KEY_plus:
	case XKB_KEY_equal:
	case XKB_KEY_KP_Add:
		return magnifier_zoom_by(mod, self, step, FALSE);
	case XKB_KEY_minus:
	case XKB_KEY_KP_Subtract:
		return magnifier_zoom_by(mod, self, 1.0 / step, FALSE);
	case XKB_KEY_0:
	case XKB_KEY_KP_0:
		return magnifier_zoom_by(mod, self, 1.0, TRUE);
	default:
		break;
	}
	return FALSE;
}

/* ── Frame ───────────────────────────────────────────────────────── */

/*
 * Exponential approach to the target, framerate-independent.
 *
 * A fixed step per frame would move at different speeds on a 60 Hz and a
 * 144 Hz screen; deriving the factor from the elapsed time and a time
 * constant makes the motion the same on both, and it degrades gracefully
 * when a frame is late instead of overshooting.
 */
static gdouble
approach(gdouble current, gdouble target, gdouble dt, gint smoothing_ms)
{
	gdouble k;

	if (smoothing_ms <= 0 || dt <= 0.0)
		return target;
	k = 1.0 - exp(-dt / (MAX(1, smoothing_ms) / 1000.0 / 3.0));
	return current + (target - current) * CLAMP(k, 0.0, 1.0);
}

static gboolean
magnifier_draw(GowlModuleMagnifier *mod, GowlCompositor *self,
                GowlMagnifierState *state)
{
	struct wlr_buffer *buffer;
	GowlFxPass        *pass;
	GowlFxQuad         quad;
	gfloat             uv[8];
	gdouble            half_u, half_v, u0, u1, v0, v1;
	gboolean           smooth;

	/*
	 * Take the sheet out of the scene for the capture.  Without this the
	 * capture contains the previous magnified frame and the picture eats
	 * itself.
	 */
	gowl_fx_sheet_set_visible(state->sheet, FALSE);
	mod->capturing = TRUE;
	if (!gowl_fx_capture(mod->gl, self, state->monitor, &state->frame, 1)) {
		mod->capturing = FALSE;
		gowl_fx_sheet_set_visible(state->sheet, TRUE);
		return FALSE;
	}
	mod->capturing = FALSE;
	gowl_fx_sheet_set_visible(state->sheet, TRUE);

	smooth = gowl_config_get_magnifier_smooth(self->config);
	if (smooth != state->filter_smooth) {
		gowl_fx_texture_set_filter(mod->gl, &state->frame, smooth);
		state->filter_smooth = smooth;
	}

	buffer = gowl_fx_sheet_acquire(state->sheet);
	if (buffer == NULL)
		return FALSE;

	/*
	 * The visible window is 1/zoom of the output, centred on the pointer.
	 * Expressed in texture coordinates because that is all a magnifier
	 * is: the same quad, sampling a smaller part of the same picture.
	 */
	half_u = 0.5 / MAX(1.0, state->zoom);
	half_v = 0.5 / MAX(1.0, state->zoom);
	u0 = state->cx / MAX(1.0, (gdouble)state->monitor->m.width) - half_u;
	u1 = state->cx / MAX(1.0, (gdouble)state->monitor->m.width) + half_u;
	v0 = state->cy / MAX(1.0, (gdouble)state->monitor->m.height) - half_v;
	v1 = state->cy / MAX(1.0, (gdouble)state->monitor->m.height) + half_v;

	uv[0] = (gfloat)u0; uv[1] = (gfloat)v0;   /* top-left */
	uv[2] = (gfloat)u0; uv[3] = (gfloat)v1;   /* bottom-left */
	uv[4] = (gfloat)u1; uv[5] = (gfloat)v0;   /* top-right */
	uv[6] = (gfloat)u1; uv[7] = (gfloat)v1;   /* bottom-right */

	pass = gowl_fx_pass_begin(mod->gl, buffer);
	if (pass == NULL) {
		wlr_buffer_unlock(buffer);
		return FALSE;
	}

	gowl_fx_quad_init(&quad);
	quad.texture = state->frame.tex;
	quad.uv = uv;
	gowl_fx_pass_quad(pass, &quad);
	gowl_fx_pass_end(pass);

	gowl_fx_sheet_present(state->sheet, buffer);
	wlr_buffer_unlock(buffer);
	return TRUE;
}

static gboolean
magnifier_frame(GowlSceneEffect *effect, GowlCompositor *self, GowlMonitor *m,
                 gint64 now)
{
	GowlModuleMagnifier *mod = GOWL_MODULE_MAGNIFIER(effect);
	GowlMagnifierState  *state;
	gdouble dt;
	gint    smoothing;

	magnifier_bind(mod, self);

	if (mod->capturing || m == NULL)
		return FALSE;

	state = magnifier_state_for(mod, m, FALSE);
	if (state == NULL)
		return FALSE;

	magnifier_ensure_gl(mod, self);
	if (!magnifier_enabled(mod, self)) {
		magnifier_state_end(mod, state);
		return FALSE;
	}

	dt = state->last_us > 0
		? (gdouble)(now - state->last_us) / 1000000.0 : 0.016;
	state->last_us = now;
	dt = CLAMP(dt, 0.0, 0.25);

	smoothing = gowl_config_get_magnifier_smoothing(self->config);
	state->zoom = approach(state->zoom, state->target_zoom, dt, smoothing);
	state->cx   = approach(state->cx, state->target_cx, dt, smoothing);
	state->cy   = approach(state->cy, state->target_cy, dt, smoothing);

	/*
	 * Back to 1:1 and staying there: put everything away.  The sheet, the
	 * capture texture and the per-frame scene render all go, so an
	 * unzoomed session costs exactly nothing.
	 */
	if (state->target_zoom <= GOWL_MAGNIFIER_IDLE_ZOOM
	    && state->zoom <= GOWL_MAGNIFIER_IDLE_ZOOM) {
		magnifier_state_end(mod, state);
		return TRUE;
	}

	if (state->sheet == NULL) {
		/* Above the bar: a magnifier that magnified everything except
		 * the panel would be worse than useless to somebody who needs
		 * it to read the panel. */
		state->sheet = gowl_fx_sheet_new(self, m, GOWL_FX_SHEET_ABOVE_TOP);
		if (state->sheet == NULL) {
			magnifier_state_end(mod, state);
			return FALSE;
		}
	}

	if (!magnifier_draw(mod, self, state)) {
		magnifier_state_end(mod, state);
		return TRUE;
	}

	/* Keep asking for frames: the magnified content is live, so a still
	 * screen still has to be re-captured when a window under it repaints
	 * -- and while the zoom is easing there is motion of our own. */
	return TRUE;
}

static void
magnifier_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                           GowlMonitor *m)
{
	GowlModuleMagnifier *mod = GOWL_MODULE_MAGNIFIER(effect);
	GowlMagnifierState  *state = magnifier_state_for(mod, m, FALSE);

	if (state != NULL)
		magnifier_state_end(mod, state);
}

static void
magnifier_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	GowlModuleMagnifier *mod = GOWL_MODULE_MAGNIFIER(effect);

	magnifier_end_all(mod);
	g_clear_pointer(&mod->gl, gowl_fx_gl_free);
	mod->gl_tried = FALSE;
	g_weak_ref_set(&mod->compositor, NULL);
}

static void
magnifier_effect_init(GowlSceneEffectInterface *iface)
{
	iface->frame           = magnifier_frame;
	iface->monitor_removed = magnifier_monitor_removed;
	iface->finish          = magnifier_finish;
}

static void
magnifier_mouse_init(GowlMouseHandlerInterface *iface)
{
	iface->handle_axis   = magnifier_handle_axis;
	iface->handle_motion = magnifier_handle_motion;
}

static void
magnifier_key_init(GowlKeybindHandlerInterface *iface)
{
	iface->handle_key = magnifier_handle_key;
}

static void
magnifier_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	magnifier_finish(GOWL_SCENE_EFFECT(handler), compositor);
}

static void
magnifier_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = magnifier_shutdown;
}

/* ── Module ──────────────────────────────────────────────────────── */

/*
 * Last among the effect modules.
 *
 * The magnifier's sheet sits above everything, including the other
 * effects' sheets, because it magnifies whatever they drew -- a cube
 * rotation seen through the magnifier is still magnified.  Its frame hook
 * therefore has to run AFTER theirs have drawn, which a high priority
 * number gets: gowl-effects.c ticks providers in priority order.
 */
#define GOWL_MAGNIFIER_PRIORITY (100)

static gboolean
magnifier_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_MAGNIFIER_PRIORITY);
	return TRUE;
}

static void
magnifier_deactivate(GowlModule *base)
{
	GowlModuleMagnifier *mod = GOWL_MODULE_MAGNIFIER(base);

	magnifier_end_all(mod);
	g_clear_pointer(&mod->gl, gowl_fx_gl_free);
	mod->gl_tried = FALSE;
}

static const gchar *magnifier_name(GowlModule *m)    { return "magnifier"; }
static const gchar *magnifier_version(GowlModule *m) { return "0.1.0"; }
static const gchar *magnifier_description(GowlModule *m)
{
	return "Zooms the whole output around the pointer";
}

static void
magnifier_finalize(GObject *object)
{
	GowlModuleMagnifier *mod = GOWL_MODULE_MAGNIFIER(object);

	magnifier_deactivate(GOWL_MODULE(object));
	g_weak_ref_clear(&mod->compositor);
	G_OBJECT_CLASS(gowl_module_magnifier_parent_class)->finalize(object);
}

static void
gowl_module_magnifier_class_init(GowlModuleMagnifierClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = magnifier_activate;
	mod->deactivate      = magnifier_deactivate;
	mod->get_name        = magnifier_name;
	mod->get_description = magnifier_description;
	mod->get_version     = magnifier_version;
	G_OBJECT_CLASS(klass)->finalize = magnifier_finalize;
}

static void
gowl_module_magnifier_init(GowlModuleMagnifier *mod)
{
	g_weak_ref_init(&mod->compositor, NULL);
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_MAGNIFIER_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_MAGNIFIER;
}
