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
 * A window switcher that shows the windows.
 *
 * Cards in a row, receding and turning away from the middle, each one a
 * live capture of that window rather than an icon and a title -- because
 * "the one with the terminal in it" is how people actually recognise a
 * window, and three terminals with the same icon are indistinguishable
 * until you can see them.
 *
 * TWO ENTRY POINTS, on purpose.  Held-modifier alt-tab is the muscle
 * memory: tap to advance, let go to commit.  And every step is also a
 * command -- switcher-next, switcher-select, switcher-list -- so an
 * embedder can drive the same strip from somewhere else entirely.  Under
 * `emacs --gowl' that means the picker can be an ordinary completing-read
 * with the compositor's own 3D strip as its preview, which is not a thing
 * a compositor can usually offer.
 *
 * The order is the focus stack, so the first tap lands on the window you
 * were last in.  That is the whole reason alt-tab is worth having.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-switcher"

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-client.h"
#include "config/gowl-config.h"
#include "boxed/gowl-color.h"
#include "fx/gowl-fx.h"
#include "util/gowl-easing.h"
#include "interfaces/gowl-scene-effect.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-keybind-handler.h"
#include "interfaces/gowl-mouse-handler.h"
#include "interfaces/gowl-shutdown-handler.h"

#include <math.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>

#include <gmodule.h>

/* More than this and the cards are too small to tell apart anyway; it
 * also bounds the captures taken when the switcher opens. */
#define GOWL_SWITCHER_MAX_CARDS 24

/* Cards are shown at about half the screen width, so half resolution is
 * already more than is visible. */
#define GOWL_SWITCHER_CARD_DIVISOR 2

/* Vertical field of view for the strip. */
#define GOWL_SWITCHER_FOV_Y (40.0 * G_PI / 180.0)

#define GOWL_TYPE_MODULE_SWITCHER (gowl_module_switcher_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleSwitcher, gowl_module_switcher,
                     GOWL, MODULE_SWITCHER, GowlModule)

typedef struct {
	GowlClient   *client;     /* referenced, so a window closing mid-swipe
	                           * cannot leave a dangling card */
	GowlFxTexture tex;
	gdouble       aspect;     /* the window's own shape, w/h */
} GowlSwitcherCard;

typedef struct {
	GowlMonitor     *monitor;      /* unowned */
	GowlFxSheet     *sheet;
	GowlSwitcherCard card[GOWL_SWITCHER_MAX_CARDS];
	gint             count;

	gint             selected;
	gdouble          scroll;       /* eased position, in cards */
	gdouble          target_scroll;

	/* The modifier that was held when the switcher opened.  Letting go
	 * of it commits, which is what makes this alt-tab rather than a
	 * dialog. */
	guint            hold_mask;
	gboolean         closing;
	gint64           last_us;
} GowlSwitcherRun;

struct _GowlModuleSwitcher {
	GowlModule  parent_instance;
	GWeakRef    compositor;
	GowlFxGl   *gl;
	gboolean    gl_tried;
	GowlSwitcherRun *run;    /* at most one; the keyboard has one focus */
	gboolean    capturing;
};

static void switcher_effect_init(GowlSceneEffectInterface *iface);
static void switcher_ipc_init(GowlIpcHandlerInterface *iface);
static void switcher_key_init(GowlKeybindHandlerInterface *iface);
static void switcher_mouse_init(GowlMouseHandlerInterface *iface);
static void switcher_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleSwitcher, gowl_module_switcher,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, switcher_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER, switcher_ipc_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_KEYBIND_HANDLER, switcher_key_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_MOUSE_HANDLER, switcher_mouse_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, switcher_shutdown_init))

/* ── Plumbing ────────────────────────────────────────────────────── */

static GowlCompositor *
switcher_compositor(GowlModuleSwitcher *mod)
{
	GowlCompositor *self = g_weak_ref_get(&mod->compositor);

	if (self != NULL)
		g_object_unref(self);
	return self;
}

static void
switcher_bind(GowlModuleSwitcher *mod, GowlCompositor *self)
{
	GowlCompositor *held = g_weak_ref_get(&mod->compositor);

	if (held != self)
		g_weak_ref_set(&mod->compositor, self);
	g_clear_object(&held);
}

static void
switcher_ensure_gl(GowlModuleSwitcher *mod, GowlCompositor *self)
{
	if (mod->gl != NULL || mod->gl_tried)
		return;
	if (self == NULL || self->renderer == NULL)
		return;

	mod->gl_tried = TRUE;
	mod->gl = gowl_fx_gl_new(self->renderer);
}

static gboolean
switcher_enabled(GowlModuleSwitcher *mod, GowlCompositor *self)
{
	return self != NULL
	       && self->config != NULL
	       && !self->locked
	       && gowl_config_get_switcher(self->config)
	       && mod->gl != NULL;
}

static void
switcher_run_free(GowlModuleSwitcher *mod, GowlSwitcherRun *run)
{
	gint i;

	if (run == NULL)
		return;
	g_clear_pointer(&run->sheet, gowl_fx_sheet_free);
	for (i = 0; i < run->count; i++) {
		gowl_fx_texture_drop(mod->gl, &run->card[i].tex);
		g_clear_object(&run->card[i].client);
	}
	g_free(run);
}

static void
switcher_end(GowlModuleSwitcher *mod)
{
	GowlSwitcherRun *run = mod->run;

	mod->run = NULL;
	switcher_run_free(mod, run);
}

/* ── Candidates ──────────────────────────────────────────────────── */

/*
 * Windows worth switching to, most-recently-focused first.
 *
 * Focus order rather than stacking or creation order, because the first
 * tap of alt-tab has to land on the window you were in before this one --
 * that single behaviour is most of what the key is for.
 */
static gboolean
switcher_candidate(GowlCompositor *self, GowlMonitor *m, GowlClient *c,
                    gboolean all_tags)
{
	if (c == NULL || c->mon != m || c->scene == NULL)
		return FALSE;
	if (gowl_fx_client_is_pinned(c))
		return FALSE;
	if (!all_tags && (c->tags & m->tagset[m->seltags]) == 0)
		return FALSE;
	return TRUE;
}

static gboolean
switcher_capture_card(GowlModuleSwitcher *mod, GowlCompositor *self,
                       GowlMonitor *m, GowlSwitcherCard *card)
{
	GowlFxVis *vis;
	gboolean   ok;
	gint       i;

	mod->capturing = TRUE;
	vis = gowl_fx_vis_begin();

	/*
	 * One window on its own, on a plain background.  Everything else goes
	 * -- the wallpaper included -- because a card is a picture of a
	 * WINDOW, and a card showing the wallpaper behind it would be a
	 * picture of the desktop with a window on it.
	 */
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_BG, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_BOTTOM, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_TOP, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_BLOCK, FALSE);
	gowl_fx_vis_hide_layer(vis, self, GOWL_SCENE_LAYER_OVERLAY, FALSE);
	for (i = 0; i < 4; i++) {
		if (self->rec_indicator[i] != NULL)
			gowl_fx_vis_set(vis, &self->rec_indicator[i]->node, FALSE);
	}
	if (m->fullscreen_bg != NULL)
		gowl_fx_vis_set(vis, &m->fullscreen_bg->node, FALSE);
	gowl_fx_vis_show_only(vis, self, m, card->client);

	ok = gowl_fx_capture(mod->gl, self, m, &card->tex,
	                     GOWL_SWITCHER_CARD_DIVISOR);

	gowl_fx_vis_restore(vis);
	mod->capturing = FALSE;
	return ok;
}

static GowlSwitcherRun *
switcher_open(GowlModuleSwitcher *mod, GowlCompositor *self, guint hold_mask)
{
	GowlSwitcherRun *run;
	GowlMonitor     *m;
	GList           *l;
	gboolean         all_tags;
	gint             i;

	if (mod->run != NULL)
		return mod->run;

	m = self->selmon;
	if (m == NULL || m->wlr_output == NULL || m->scene_output == NULL
	    || m->m.width <= 0 || m->m.height <= 0)
		return NULL;
	if (m->wlr_output->transform != WL_OUTPUT_TRANSFORM_NORMAL)
		return NULL;

	all_tags = gowl_config_get_switcher_all_tags(self->config);

	run = g_new0(GowlSwitcherRun, 1);
	run->monitor   = m;
	run->hold_mask = hold_mask;

	for (l = self->fstack; l != NULL && run->count < GOWL_SWITCHER_MAX_CARDS;
	     l = l->next) {
		GowlClient *c = l->data;

		if (!switcher_candidate(self, m, c, all_tags))
			continue;
		run->card[run->count].client = g_object_ref(c);
		run->card[run->count].aspect = c->geom.height > 0
			? (gdouble)c->geom.width / (gdouble)c->geom.height
			: (gdouble)m->m.width / (gdouble)m->m.height;
		run->count++;
	}

	/* One window is not a choice, and none is not a switcher. */
	if (run->count < 2) {
		switcher_run_free(mod, run);
		return NULL;
	}

	for (i = 0; i < run->count; i++)
		switcher_capture_card(mod, self, m, &run->card[i]);

	run->sheet = gowl_fx_sheet_new(self, m, GOWL_FX_SHEET_ABOVE_TOP);
	if (run->sheet == NULL) {
		switcher_run_free(mod, run);
		return NULL;
	}

	/*
	 * Start on the card AFTER the focused one.  The focused window is
	 * card 0, and a switcher that opened on the window you are already in
	 * would need two taps to do the thing it exists for.
	 */
	run->selected      = 1;
	run->scroll        = 0.0;
	run->target_scroll = 1.0;

	mod->run = run;
	if (m->wlr_output != NULL)
		wlr_output_schedule_frame(m->wlr_output);
	return run;
}

static void
switcher_commit(GowlModuleSwitcher *mod, GowlCompositor *self, gboolean commit)
{
	GowlSwitcherRun *run = mod->run;
	GowlClient      *chosen;

	if (run == NULL)
		return;

	chosen = commit && run->selected >= 0 && run->selected < run->count
		? run->card[run->selected].client : NULL;

	/* The sheet must come down BEFORE the focus change: showing a
	 * window means revealing its tag, and the reveal has to land on a
	 * visible desktop rather than behind the switcher. */
	switcher_end(mod);

	if (chosen != NULL && GOWL_IS_CLIENT(chosen) && chosen->scene != NULL)
		gowl_compositor_show_client(self, chosen);
}

static void
switcher_step(GowlModuleSwitcher *mod, gint delta)
{
	GowlSwitcherRun *run = mod->run;

	if (run == NULL || run->count <= 0)
		return;

	/*
	 * Wrapping, unlike the overview's grid.  A row of windows in
	 * recency order has no meaningful ends -- going past the last one
	 * back to the first is how every alt-tab in existence behaves, and
	 * stopping dead would just feel broken.
	 */
	run->selected = (run->selected + delta) % run->count;
	if (run->selected < 0)
		run->selected += run->count;
	run->target_scroll = (gdouble)run->selected;

	/*
	 * Take the shorter way round the ring.  Without this, wrapping from
	 * the last card to the first scrolls the entire strip backwards past
	 * every card in between.
	 */
	while (run->target_scroll - run->scroll > (gdouble)run->count * 0.5)
		run->scroll += (gdouble)run->count;
	while (run->scroll - run->target_scroll > (gdouble)run->count * 0.5)
		run->scroll -= (gdouble)run->count;

	if (run->monitor->wlr_output != NULL)
		wlr_output_schedule_frame(run->monitor->wlr_output);
}

/* ── Drawing ─────────────────────────────────────────────────────── */

static void
switcher_backdrop_rgb(GowlCompositor *self, gfloat *out)
{
	const gchar *spec = self->config != NULL
		? gowl_config_get_switcher_backdrop_color(self->config) : NULL;
	GowlColor *color = spec != NULL ? gowl_color_new_from_hex(spec) : NULL;

	if (color == NULL) {
		out[0] = 0.055f; out[1] = 0.063f; out[2] = 0.094f;
		return;
	}
	out[0] = (gfloat)color->r;
	out[1] = (gfloat)color->g;
	out[2] = (gfloat)color->b;
	gowl_color_free(color);
}

/* One card's four corners, in the strip's world space. */
static void
switcher_card_quad(gfloat *pos, gdouble offset, gdouble half_w, gdouble half_h,
                    gdouble spacing, gdouble angle_rad, gdouble depth,
                    gboolean mirrored)
{
	gdouble turn = angle_rad * CLAMP(offset, -1.0, 1.0);
	gdouble cx = offset * spacing;
	gdouble cz = -fabs(CLAMP(offset, -1.5, 1.5)) * depth;
	gdouble ux = cos(turn), uz = -sin(turn);
	gdouble top    = mirrored ? -3.0 * half_h : half_h;
	gdouble bottom = -half_h;
	gint    i;

	pos[0]  = (gfloat)(cx - ux * half_w);
	pos[1]  = (gfloat)top;
	pos[2]  = (gfloat)(cz - uz * half_w);

	pos[3]  = (gfloat)(cx - ux * half_w);
	pos[4]  = (gfloat)bottom;
	pos[5]  = (gfloat)(cz - uz * half_w);

	pos[6]  = (gfloat)(cx + ux * half_w);
	pos[7]  = (gfloat)top;
	pos[8]  = (gfloat)(cz + uz * half_w);

	pos[9]  = (gfloat)(cx + ux * half_w);
	pos[10] = (gfloat)bottom;
	pos[11] = (gfloat)(cz + uz * half_w);

	for (i = 0; i < 12; i++) {
		if (!isfinite(pos[i]))
			pos[i] = 0.0f;
	}
}

static gboolean
switcher_draw(GowlModuleSwitcher *mod, GowlCompositor *self,
               GowlSwitcherRun *run)
{
	struct wlr_buffer *buffer;
	GowlFxPass *pass;
	gfloat proj[16], view[16], vp[16], clear[4], backdrop[3];
	gdouble card_w, card_h, spacing, angle, depth, dist, reflection;
	gint w, h, pass_index;

	gowl_fx_sheet_get_size(run->sheet, &w, &h);
	if (w <= 0 || h <= 0)
		return FALSE;

	card_w = (gdouble)w * gowl_config_get_switcher_scale(self->config) * 0.5;
	card_h = card_w * (gdouble)h / (gdouble)w;
	spacing = card_w * 2.0 * gowl_config_get_switcher_spacing(self->config);
	angle = gowl_config_get_switcher_angle(self->config) * G_PI / 180.0;
	depth = card_w * 0.9;
	reflection = gowl_config_get_switcher_reflection(self->config);

	/* Far enough back that the centre card sits comfortably inside the
	 * frame with its neighbours showing at the edges. */
	dist = card_h / tan(GOWL_SWITCHER_FOV_Y * 0.5) * 1.35 + depth;

	buffer = gowl_fx_sheet_acquire(run->sheet);
	if (buffer == NULL)
		return FALSE;

	pass = gowl_fx_pass_begin(mod->gl, buffer);
	if (pass == NULL) {
		wlr_buffer_unlock(buffer);
		return FALSE;
	}

	switcher_backdrop_rgb(self, backdrop);
	clear[0] = backdrop[0] * 0.4f;
	clear[1] = backdrop[1] * 0.4f;
	clear[2] = backdrop[2] * 0.4f;
	clear[3] = 1.0f;
	gowl_fx_pass_clear(pass, clear);
	gowl_fx_pass_backdrop(pass, backdrop, 1.0f);

	gowl_fx_mat4_perspective(proj, GOWL_SWITCHER_FOV_Y,
	                          (gdouble)w / (gdouble)h,
	                          MAX(1.0, dist * 0.1), dist + depth * 8.0);
	gowl_fx_mat4_view(view, dist, 0.0);
	gowl_fx_mat4_multiply(vp, proj, view);

	/*
	 * Reflections first, then cards: with no depth buffer the order IS
	 * the depth, and within each set the cards are drawn from the
	 * outside in so the nearer ones land on top of their neighbours.
	 */
	for (pass_index = 0; pass_index < 2; pass_index++) {
		gboolean mirrored = (pass_index == 0);
		gint     step;

		if (mirrored && reflection <= 0.0)
			continue;

		for (step = run->count - 1; step >= 0; step--) {
			/*
			 * Walk outwards-in: index by distance from the selection so
			 * the furthest card is drawn first and the selected one last.
			 */
			gint    sign = (step % 2 == 0) ? 1 : -1;
			gint    away = (step + 1) / 2;
			gint    idx = run->selected + sign * away;
			gdouble offset;
			gdouble fade;
			gfloat  pos[12];
			GowlFxQuad quad;

			idx = ((idx % run->count) + run->count) % run->count;
			offset = (gdouble)idx - run->scroll;

			/* Shortest way round the ring, so the strip does not tear
			 * where the wrap happens. */
			while (offset > (gdouble)run->count * 0.5)
				offset -= (gdouble)run->count;
			while (offset < -(gdouble)run->count * 0.5)
				offset += (gdouble)run->count;

			/* Beyond this the card is a sliver behind its neighbours. */
			if (fabs(offset) > 3.5)
				continue;

			switcher_card_quad(pos, offset, card_w, card_h, spacing,
			                    angle, depth, mirrored);

			/* Cards fade out towards the edges instead of stopping, so
			 * the strip has no visible end. */
			fade = CLAMP(1.0 - (fabs(offset) - 2.0) / 1.5, 0.0, 1.0);

			gowl_fx_quad_init(&quad);
			quad.mvp     = vp;
			quad.pos     = pos;
			quad.texture = run->card[idx].tex.tex;
			quad.base[0] = backdrop[0] * 2.0f;
			quad.base[1] = backdrop[1] * 2.0f;
			quad.base[2] = backdrop[2] * 2.0f;
			quad.corner  = 0.02f;
			{
				/* The off-centre cards dim as they turn away, which is
				 * what makes the middle one read as chosen without any
				 * chrome around it. */
				gdouble lit = 1.0 - 0.45 * CLAMP(fabs(offset), 0.0, 1.0);

				quad.tint[0] = (gfloat)lit;
				quad.tint[1] = (gfloat)lit;
				quad.tint[2] = (gfloat)lit;
			}
			quad.edge  = fabs(offset) < 0.5
				? (gfloat)(0.22 * (1.0 - fabs(offset) * 2.0)) : 0.0f;
			quad.fade  = mirrored ? 1.0f : 0.0f;
			quad.alpha = (gfloat)(fade * (mirrored ? reflection : 1.0));
			gowl_fx_pass_quad(pass, &quad);
		}
	}

	gowl_fx_pass_end(pass);
	gowl_fx_sheet_present(run->sheet, buffer);
	wlr_buffer_unlock(buffer);
	return TRUE;
}

static gboolean
switcher_frame(GowlSceneEffect *effect, GowlCompositor *self, GowlMonitor *m,
                gint64 now)
{
	GowlModuleSwitcher *mod = GOWL_MODULE_SWITCHER(effect);
	GowlSwitcherRun    *run = mod->run;
	gdouble dt, k;
	gint    duration;

	switcher_bind(mod, self);

	if (mod->capturing || run == NULL || run->monitor != m)
		return FALSE;

	if (!switcher_enabled(mod, self)) {
		switcher_end(mod);
		return TRUE;
	}

	dt = run->last_us > 0 ? (gdouble)(now - run->last_us) / 1000000.0 : 0.016;
	run->last_us = now;
	dt = CLAMP(dt, 0.0, 0.25);

	/* Framerate-independent approach, so the strip moves at the same
	 * speed on a 60 Hz and a 144 Hz screen. */
	duration = MAX(1, gowl_config_get_switcher_duration(self->config));
	k = 1.0 - exp(-dt / ((gdouble)duration / 1000.0 / 3.0));
	run->scroll += (run->target_scroll - run->scroll) * CLAMP(k, 0.0, 1.0);

	if (!switcher_draw(mod, self, run)) {
		switcher_end(mod);
		return TRUE;
	}
	return TRUE;
}

/* ── Input ───────────────────────────────────────────────────────── */

/* Which modifier keysyms correspond to a held mask, so a release of the
 * right one can commit. */
static gboolean
switcher_is_hold_key(guint keysym, guint mask)
{
	switch (keysym) {
	case XKB_KEY_Super_L: case XKB_KEY_Super_R:
		return (mask & WLR_MODIFIER_LOGO) != 0;
	case XKB_KEY_Alt_L: case XKB_KEY_Alt_R: case XKB_KEY_Meta_L:
	case XKB_KEY_Meta_R: case XKB_KEY_ISO_Level3_Shift:
		return (mask & WLR_MODIFIER_ALT) != 0;
	case XKB_KEY_Control_L: case XKB_KEY_Control_R:
		return (mask & WLR_MODIFIER_CTRL) != 0;
	default:
		return FALSE;
	}
}

static gboolean
switcher_handle_key(GowlKeybindHandler *handler, guint modifiers, guint keysym,
                     gboolean pressed)
{
	GowlModuleSwitcher *mod = GOWL_MODULE_SWITCHER(handler);
	GowlCompositor     *self = switcher_compositor(mod);
	GowlSwitcherRun    *run;

	if (self == NULL)
		return FALSE;
	run = mod->run;

	if (run == NULL) {
		/*
		 * Not open.  The only keys that matter are the ones that open
		 * it, and only as a FALLBACK: gowl offers modules a key after
		 * the user's own binds have declined it, so a configured
		 * Super+Tab still wins.
		 */
		if (!pressed || keysym != XKB_KEY_Tab
		    || (modifiers & (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT)) == 0)
			return FALSE;

		switcher_ensure_gl(mod, self);
		if (!switcher_enabled(mod, self))
			return FALSE;
		return switcher_open(mod, self,
		                     modifiers & (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT))
		       != NULL;
	}

	if (!pressed) {
		/* Letting go of the held modifier commits.  This is the whole
		 * reason modules see key releases at all. */
		if (run->hold_mask != 0 && switcher_is_hold_key(keysym, run->hold_mask)) {
			switcher_commit(mod, self, TRUE);
			return TRUE;
		}
		return FALSE;
	}

	switch (keysym) {
	case XKB_KEY_Escape:
		switcher_commit(mod, self, FALSE);
		return TRUE;
	case XKB_KEY_Return: case XKB_KEY_KP_Enter: case XKB_KEY_space:
		switcher_commit(mod, self, TRUE);
		return TRUE;
	case XKB_KEY_Tab:
	case XKB_KEY_Right: case XKB_KEY_l:
		switcher_step(mod, 1);
		return TRUE;
	case XKB_KEY_ISO_Left_Tab:
	case XKB_KEY_Left: case XKB_KEY_h:
		switcher_step(mod, -1);
		return TRUE;
	default:
		break;
	}
	/* Everything else is swallowed while the switcher is up: a keystroke
	 * meant for the picker must not also reach the window behind it. */
	return TRUE;
}

static gboolean
switcher_handle_button(GowlMouseHandler *handler, guint button, guint state,
                        guint modifiers)
{
	GowlModuleSwitcher *mod = GOWL_MODULE_SWITCHER(handler);
	GowlCompositor     *self = switcher_compositor(mod);

	if (self == NULL || mod->run == NULL || state == 0)
		return FALSE;

	/* Any click commits the card in the middle.  Picking a card by
	 * pointing at it would need the strip's inverse projection for very
	 * little: the strip is driven by the keyboard. */
	switcher_commit(mod, self, TRUE);
	return TRUE;
}

static gboolean
switcher_handle_axis(GowlMouseHandler *handler, guint axis, gdouble delta,
                      gint discrete, guint modifiers)
{
	GowlModuleSwitcher *mod = GOWL_MODULE_SWITCHER(handler);

	if (mod->run == NULL || delta == 0.0)
		return FALSE;

	switcher_step(mod, delta > 0.0 ? 1 : -1);
	return TRUE;
}

/* ── Commands ────────────────────────────────────────────────────── */

/*
 * The same strip, driven from anywhere.
 *
 * `switcher-list' is what makes an external picker possible: an embedder
 * reads the candidates, shows them in its own UI, and calls
 * `switcher-select' as the user moves through them, so the compositor's
 * 3D strip becomes the preview for a completion buffer.
 */
static gchar *
switcher_handle_command(GowlIpcHandler *handler, const gchar *command,
                         const gchar *args)
{
	GowlModuleSwitcher *mod = GOWL_MODULE_SWITCHER(handler);
	GowlCompositor     *self = switcher_compositor(mod);

	if (self == NULL || command == NULL)
		return NULL;

	if (g_strcmp0(command, "switcher") == 0
	    || g_strcmp0(command, "switcher-next") == 0
	    || g_strcmp0(command, "switcher-prev") == 0) {
		gint delta = g_strcmp0(command, "switcher-prev") == 0 ? -1 : 1;

		switcher_ensure_gl(mod, self);
		if (!switcher_enabled(mod, self))
			return g_strdup("ERROR switcher unavailable");

		if (mod->run == NULL) {
			/*
			 * Opened by command, but still alt-tab if a modifier is
			 * down.  A user who binds Super+Tab in their config takes
			 * the key away from this module's own fallback, and reading
			 * the held modifiers here is what stops that from silently
			 * costing them release-to-commit.  Nothing held means
			 * nothing to let go of, and the strip stays up until
			 * something commits or cancels it.
			 */
			struct wlr_keyboard *kbd = self->wlr_seat != NULL
				? wlr_seat_get_keyboard(self->wlr_seat) : NULL;
			guint held = kbd != NULL
				? (wlr_keyboard_get_modifiers(kbd)
				   & (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT)) : 0;

			if (switcher_open(mod, self, held) == NULL)
				return g_strdup("ERROR nothing to switch to");
			if (delta < 0)
				switcher_step(mod, -2);
			return g_strdup("OK switcher open");
		}
		switcher_step(mod, delta);
		return g_strdup("OK switcher moved");
	}

	if (g_strcmp0(command, "switcher-close") == 0) {
		if (mod->run == NULL)
			return g_strdup("OK switcher closed");
		switcher_commit(mod, self, g_strcmp0(args, "cancel") != 0);
		return g_strdup("OK switcher closed");
	}

	if (g_strcmp0(command, "switcher-select") == 0) {
		gint want;

		if (mod->run == NULL || args == NULL)
			return g_strdup("ERROR switcher not open");
		want = (gint)g_ascii_strtoll(args, NULL, 10);
		if (want < 0 || want >= mod->run->count)
			return g_strdup("ERROR no such card");
		switcher_step(mod, want - mod->run->selected);
		return g_strdup("OK switcher selected");
	}

	if (g_strcmp0(command, "switcher-list") == 0) {
		GString *out;
		gint i;

		if (mod->run == NULL)
			return g_strdup("ERROR switcher not open");
		out = g_string_new(NULL);
		for (i = 0; i < mod->run->count; i++) {
			GowlClient *c = mod->run->card[i].client;

			g_string_append_printf(out, "%d\\t%s\\t%s\\n", i,
			                       c->app_id != NULL ? c->app_id : "",
			                       c->title != NULL ? c->title : "");
		}
		return g_string_free(out, FALSE);
	}

	return NULL;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

/*
 * A window vanishing while the switcher is up.
 *
 * The card holds a reference, so the GObject survives -- but its scene
 * tree does not, and committing to a window that has unmapped would do
 * nothing at best.  Dropping the whole switcher is the honest response:
 * the list it is showing is no longer true.
 */
static gboolean
switcher_client_event(GowlSceneEffect *effect, GowlCompositor *self,
                       GowlClient *c, GowlSceneEffectEvent event,
                       const struct wlr_box *previous, gboolean interactive)
{
	GowlModuleSwitcher *mod = GOWL_MODULE_SWITCHER(effect);
	gint i;

	switcher_bind(mod, self);

	if (mod->run == NULL || c == NULL)
		return FALSE;
	if (event != GOWL_SCENE_EFFECT_UNMAP && event != GOWL_SCENE_EFFECT_DESTROY)
		return FALSE;

	for (i = 0; i < mod->run->count; i++) {
		if (mod->run->card[i].client == c) {
			switcher_end(mod);
			break;
		}
	}
	return FALSE;
}

static void
switcher_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                          GowlMonitor *m)
{
	GowlModuleSwitcher *mod = GOWL_MODULE_SWITCHER(effect);

	if (mod->run != NULL && mod->run->monitor == m)
		switcher_end(mod);
}

static void
switcher_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	GowlModuleSwitcher *mod = GOWL_MODULE_SWITCHER(effect);

	switcher_end(mod);
	g_clear_pointer(&mod->gl, gowl_fx_gl_free);
	mod->gl_tried = FALSE;
	g_weak_ref_set(&mod->compositor, NULL);
}

static void switcher_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_event    = switcher_client_event;
	iface->frame           = switcher_frame;
	iface->monitor_removed = switcher_monitor_removed;
	iface->finish          = switcher_finish;
}
static void switcher_ipc_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = switcher_handle_command;
}
static void switcher_key_init(GowlKeybindHandlerInterface *iface)
{
	iface->handle_key = switcher_handle_key;
}
static void switcher_mouse_init(GowlMouseHandlerInterface *iface)
{
	iface->handle_button = switcher_handle_button;
	iface->handle_axis   = switcher_handle_axis;
}
static void
switcher_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	switcher_finish(GOWL_SCENE_EFFECT(handler), compositor);
}
static void switcher_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = switcher_shutdown;
}

/* Between the overview and the animation module: it has to be asked
 * before window animations so it can swallow keys and clicks while it is
 * up, and after the effects that own a whole tag change. */
#define GOWL_SWITCHER_PRIORITY (-6)

static gboolean
switcher_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_SWITCHER_PRIORITY);
	return TRUE;
}

static void
switcher_deactivate(GowlModule *base)
{
	GowlModuleSwitcher *mod = GOWL_MODULE_SWITCHER(base);

	switcher_end(mod);
	g_clear_pointer(&mod->gl, gowl_fx_gl_free);
	mod->gl_tried = FALSE;
}

static const gchar *switcher_name(GowlModule *m)    { return "switcher"; }
static const gchar *switcher_version(GowlModule *m) { return "0.1.0"; }
static const gchar *switcher_description(GowlModule *m)
{
	return "Alt-tab as a 3D strip of live window previews";
}

static void
switcher_finalize(GObject *object)
{
	GowlModuleSwitcher *mod = GOWL_MODULE_SWITCHER(object);

	switcher_deactivate(GOWL_MODULE(object));
	g_weak_ref_clear(&mod->compositor);
	G_OBJECT_CLASS(gowl_module_switcher_parent_class)->finalize(object);
}

static void
gowl_module_switcher_class_init(GowlModuleSwitcherClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = switcher_activate;
	mod->deactivate      = switcher_deactivate;
	mod->get_name        = switcher_name;
	mod->get_description = switcher_description;
	mod->get_version     = switcher_version;
	G_OBJECT_CLASS(klass)->finalize = switcher_finalize;
}

static void
gowl_module_switcher_init(GowlModuleSwitcher *mod)
{
	g_weak_ref_init(&mod->compositor, NULL);
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_SWITCHER_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_SWITCHER;
}
