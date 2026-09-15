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
 * Embers: a fire below the window, and the heat above it.
 *
 * The optics are in fx/gowl-fx-embers.c -- sparks cooling along the
 * Planckian locus and dimming as the fourth power of their temperature,
 * seen through a turbulent index field.  The host half is shared; see
 * the note in modules/embers.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-embers"

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "config/gowl-config.h"
#include "boxed/gowl-color.h"
#include "fx/gowl-fx.h"
#include "fx/gowl-fx-backdrop-host.h"
#include "interfaces/gowl-scene-effect.h"
#include "interfaces/gowl-shutdown-handler.h"

#include <math.h>
#include <string.h>
#include <gmodule.h>

#define GOWL_TYPE_MODULE_EMBERS (gowl_module_embers_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleEmbers, gowl_module_embers,
                     GOWL, MODULE_EMBERS, GowlModule)

/*
 * The look, out of the config, in LOGICAL pixels.
 *
 * The host compares it with memcmp(), so every field has to be part of
 * what "the look" means -- and the common block has to come FIRST,
 * which is the contract in gowl-fx-backdrop-host.h.
 */
typedef struct {
	GowlBackdropHostCommon common;

	gdouble column;
	gdouble density;
	gdouble ember;
	gdouble spacing;
	gdouble sway;
	gdouble drag;
	gdouble temperature;
	gdouble cool;
	gdouble flicker;
	gdouble ash;
	gdouble glow;
	gdouble hearth;
	gdouble haze;
	gdouble haze_scale;
	gdouble fog;
	gdouble speed;
	gdouble clarity;
	gdouble opacity;
	gdouble brightness;
	gdouble tint[3];
} GowlEmbersStyle;

struct _GowlModuleEmbers {
	GowlModule       parent_instance;
	GWeakRef         compositor;
	GowlBackdropHost *host;
};

static void embers_effect_init(GowlSceneEffectInterface *iface);
static void embers_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleEmbers, gowl_module_embers,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, embers_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, embers_shutdown_init))

/* ── The look ────────────────────────────────────────────────────── */

static void
embers_read_style(gpointer style, GowlConfig *config)
{
	GowlEmbersStyle *s = style;
	GowlColor       *tint;
	const gchar     *spec;
	gdouble         k;

	memset(s, 0, sizeof(*s));

	/*
	 * One knob over the whole preset: how many sparks there are, how
	 * closely a column lets them go, and how fast they rise.
	 *
	 * NOT the temperature, which is the colour and not the amount, and
	 * not the sizes -- a busier fire is not a fire with bigger sparks
	 * in a smaller window.
	 */
	k = gowl_config_get_embers_intensity(config);

	s->density = MIN(1.0, gowl_config_get_embers_density(config) * k);
	s->spacing = MIN(1.0, gowl_config_get_embers_spacing(config) * k);
	s->speed   = MIN(5.0, gowl_config_get_embers_speed(config) * k);

	s->column      = gowl_config_get_embers_column(config);
	s->ember       = gowl_config_get_embers_ember(config);
	s->sway        = gowl_config_get_embers_sway(config);
	s->drag        = gowl_config_get_embers_drag(config);
	s->temperature = gowl_config_get_embers_temperature(config);
	s->cool        = gowl_config_get_embers_cool(config);
	s->flicker     = gowl_config_get_embers_flicker(config);
	s->ash         = gowl_config_get_embers_ash(config);
	s->glow        = gowl_config_get_embers_glow(config);
	s->hearth      = gowl_config_get_embers_hearth(config);
	s->haze        = gowl_config_get_embers_haze(config);
	s->haze_scale  = gowl_config_get_embers_haze_scale(config);
	s->fog         = gowl_config_get_embers_fog(config);
	s->clarity     = gowl_config_get_embers_clarity(config);
	s->opacity     = gowl_config_get_embers_opacity(config);
	s->brightness  = gowl_config_get_embers_brightness(config);

	s->common.fps          = gowl_config_get_embers_fps(config);
	s->common.scale        = gowl_config_get_embers_scale(config);
	s->common.frost        = gowl_config_get_embers_frost(config);
	s->common.frost_passes = gowl_config_get_embers_frost_passes(config);

	s->tint[0] = s->tint[1] = s->tint[2] = 1.0;
	spec = gowl_config_get_embers_tint(config);
	tint = spec != NULL ? gowl_color_new_from_hex(spec) : NULL;
	if (tint != NULL) {
		s->tint[0] = tint->r;
		s->tint[1] = tint->g;
		s->tint[2] = tint->b;
		gowl_color_free(tint);
	}
}

static void
embers_advance(gpointer clock, gconstpointer style, gdouble dt)
{
	const GowlEmbersStyle *s = style;

	gowl_fx_embers_advance(clock, dt, s->speed, s->speed);
}

static gboolean
embers_draw(GowlFxPass *pass, const GowlFxTexture *soft,
            const GowlFxTexture *sharp, gconstpointer style,
            gconstpointer clock, const GowlBackdropPlan *plan,
            gdouble radius, guint seed)
{
	const GowlEmbersStyle *s = style;
	GowlFxEmbersParams    p;
	gdouble            scale = (plan->scale_x + plan->scale_y) * 0.5;

	gowl_fx_embers_params_init(&p);
	p.width  = plan->buf_width;
	p.height = plan->buf_height;
	p.radius = (gfloat)(radius * scale);

	/*
	 * Lengths scale with the render and nothing else does.
	 *
	 * `column', `ember', `sway' and `haze' are all pixels.
	 * `temperature' is kelvin, `drag' and `cool' are rates over a
	 * unit rise, and `haze_scale' is cells across the pane -- all of
	 * them already ratios, and scaling any of them would make
	 * `embers-scale' change what the fire IS rather than how finely it
	 * is drawn.
	 */
	p.column      = (gfloat)(s->column * scale);
	p.ember       = (gfloat)(s->ember * scale);
	p.sway        = (gfloat)(s->sway * scale);
	p.haze        = (gfloat)(s->haze * scale);
	p.density     = (gfloat)s->density;
	p.spacing     = (gfloat)s->spacing;
	p.drag        = (gfloat)s->drag;
	p.temperature = (gfloat)s->temperature;
	p.cool        = (gfloat)s->cool;
	p.flicker     = (gfloat)s->flicker;
	p.ash         = (gfloat)s->ash;
	p.glow        = (gfloat)s->glow;
	p.hearth      = (gfloat)s->hearth;
	p.haze_scale  = (gfloat)s->haze_scale;
	p.fog         = (gfloat)s->fog;
	p.clarity     = (gfloat)s->clarity;
	p.brightness  = (gfloat)s->brightness;
	p.alpha       = (gfloat)s->opacity;
	p.tint[0]     = (gfloat)s->tint[0];
	p.tint[1]     = (gfloat)s->tint[1];
	p.tint[2]     = (gfloat)s->tint[2];

	p.src_origin[0] = (gfloat)plan->origin_x;
	p.src_origin[1] = (gfloat)plan->origin_y;
	p.src_scale     = (gfloat)plan->src_scale;
	/* Which film this window shows.  Anchored to the WINDOW so it stays
	 * put when the window moves, and per-client so two windows side by
	 * side are not the same film twice. */
	p.seed = (gfloat)(seed % 9973u) * 0.0016f;

	return gowl_fx_pass_embers(pass, soft, sharp, &p, clock);
}

static const GowlBackdropHostVTable embers_vtable = {
	"embers",
	GOWL_BACKDROP_EMBERS,
	sizeof(GowlEmbersStyle),
	sizeof(GowlFxEmbersClock),
	embers_read_style,
	embers_advance,
	embers_draw
};

/* ── Hooks, forwarded ────────────────────────────────────────────── */

static gboolean
embers_frame(GowlSceneEffect *effect, GowlCompositor *self, GowlMonitor *m,
               gint64 now_us)
{
	GowlModuleEmbers *mod = GOWL_MODULE_EMBERS(effect);

	g_weak_ref_set(&mod->compositor, self);
	return gowl_backdrop_host_frame(mod->host, self, m, now_us);
}

static gboolean
embers_client_event(GowlSceneEffect *effect, GowlCompositor *self,
                      GowlClient *c, GowlSceneEffectEvent event,
                      const struct wlr_box *previous, gboolean interactive)
{
	GowlModuleEmbers *mod = GOWL_MODULE_EMBERS(effect);

	g_weak_ref_set(&mod->compositor, self);
	return gowl_backdrop_host_client_event(mod->host, self, c, event);
}

static void
embers_client_placed(GowlSceneEffect *effect, GowlCompositor *self,
                       GowlClient *c, gboolean settled)
{
	GowlModuleEmbers *mod = GOWL_MODULE_EMBERS(effect);

	g_weak_ref_set(&mod->compositor, self);
	gowl_backdrop_host_client_placed(mod->host, self, c);
}

static void
embers_alpha_changed(GowlSceneEffect *effect, GowlClient *c, gfloat alpha)
{
	GowlModuleEmbers *mod = GOWL_MODULE_EMBERS(effect);

	gowl_backdrop_host_alpha_changed(mod->host, c);
}

static void
embers_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                         GowlMonitor *m)
{
	GowlModuleEmbers *mod = GOWL_MODULE_EMBERS(effect);

	gowl_backdrop_host_monitor_removed(mod->host, self, m);
}

static void
embers_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	GowlModuleEmbers *mod = GOWL_MODULE_EMBERS(effect);

	gowl_backdrop_host_finish(mod->host, self);
	g_weak_ref_set(&mod->compositor, NULL);
}

static void
embers_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_event    = embers_client_event;
	iface->alpha_changed   = embers_alpha_changed;
	iface->monitor_removed = embers_monitor_removed;
	iface->finish          = embers_finish;
	iface->client_placed   = embers_client_placed;
	iface->frame           = embers_frame;
}

static void
embers_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	embers_finish(GOWL_SCENE_EFFECT(handler), compositor);
}

static void
embers_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = embers_shutdown;
}

/* ── Module ──────────────────────────────────────────────────────── */

/* The same priority as every other backdrop, and after the animation
 * module for the hooks all of them hear. */
#define GOWL_EMBERS_PRIORITY (10)

static gboolean
embers_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_EMBERS_PRIORITY);
	return TRUE;
}

static void
embers_deactivate(GowlModule *base)
{
	GowlModuleEmbers *mod = GOWL_MODULE_EMBERS(base);
	GowlCompositor     *self = g_weak_ref_get(&mod->compositor);

	/* The compositor may already be gone -- a module outlives it when
	 * the manager is released last -- and the host copes with NULL. */
	gowl_backdrop_host_finish(mod->host, self);
	if (self != NULL)
		g_object_unref(self);
}

static const gchar *embers_name(GowlModule *m)    { return "embers"; }
static const gchar *embers_version(GowlModule *m) { return "0.1.0"; }
static const gchar *embers_description(GowlModule *m)
{
	return "Shows sparks rising from a fire below the window";
}

static void
embers_finalize(GObject *object)
{
	GowlModuleEmbers *mod = GOWL_MODULE_EMBERS(object);

	embers_deactivate(GOWL_MODULE(object));
	g_clear_pointer(&mod->host, gowl_backdrop_host_free);
	g_weak_ref_clear(&mod->compositor);
	G_OBJECT_CLASS(gowl_module_embers_parent_class)->finalize(object);
}

static void
gowl_module_embers_class_init(GowlModuleEmbersClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = embers_activate;
	mod->deactivate      = embers_deactivate;
	mod->get_name        = embers_name;
	mod->get_description = embers_description;
	mod->get_version     = embers_version;
	G_OBJECT_CLASS(klass)->finalize = embers_finalize;
}

static void
gowl_module_embers_init(GowlModuleEmbers *mod)
{
	g_weak_ref_init(&mod->compositor, NULL);
	mod->host = gowl_backdrop_host_new(&embers_vtable);
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_EMBERS_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_EMBERS;
}
