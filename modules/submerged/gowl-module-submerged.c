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
 * Submerged: the wallpaper seen from under water.
 *
 * The optics are in fx/gowl-fx-submerged.c -- wavelength absorption over
 * a distance, light scattered back in, and a caustic net from the
 * surface above.  The host half is shared; see the note in
 * modules/submerged.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-submerged"

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

#define GOWL_TYPE_MODULE_SUBMERGED (gowl_module_submerged_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleSubmerged, gowl_module_submerged,
                     GOWL, MODULE_SUBMERGED, GowlModule)

/*
 * The look, out of the config, in LOGICAL pixels.
 *
 * The host compares it with memcmp(), so every field has to be part of
 * what "the look" means -- and the common block has to come FIRST,
 * which is the contract in gowl-fx-backdrop-host.h.
 */
typedef struct {
	GowlBackdropHostCommon common;

	gdouble depth;
	gdouble extinction[3];
	gdouble murk;
	gdouble caustics;
	gdouble caustic_scale;
	gdouble shafts;
	gdouble shaft_lean;
	gdouble motes;
	gdouble mote_size;
	gdouble surface;
	gdouble sway;
	gdouble fog;
	gdouble speed;
	gdouble drift;
	gdouble clarity;
	gdouble opacity;
	gdouble brightness;
	gdouble water[3];
} GowlSubmergedStyle;

struct _GowlModuleSubmerged {
	GowlModule       parent_instance;
	GWeakRef         compositor;
	GowlBackdropHost *host;
};

static void submerged_effect_init(GowlSceneEffectInterface *iface);
static void submerged_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleSubmerged, gowl_module_submerged,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, submerged_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, submerged_shutdown_init))

/* ── The look ────────────────────────────────────────────────────── */

static void
submerged_read_style(gpointer style, GowlConfig *config)
{
	GowlSubmergedStyle *s = style;
	GowlColor          *tint;
	const gchar        *spec;
	gdouble            k;

	memset(s, 0, sizeof(*s));

	/*
	 * One knob over the whole preset: how bright the caustic net is,
	 * how much is drifting in the water, and how fast the surface
	 * overhead moves.
	 *
	 * NOT the depth.  Depth is the colour of the effect -- it is what
	 * the absorption is raised over -- and scaling it with an
	 * "intensity" would turn a brightness knob into a hue knob.
	 */
	k = gowl_config_get_submerged_intensity(config);

	s->caustics = MIN(3.0, gowl_config_get_submerged_caustics(config) * k);
	s->motes    = MIN(1.0, gowl_config_get_submerged_motes(config) * k);
	s->speed    = MIN(5.0, gowl_config_get_submerged_speed(config) * k);

	s->depth          = gowl_config_get_submerged_depth(config);
	s->extinction[0]  = gowl_config_get_submerged_extinction_r(config);
	s->extinction[1]  = gowl_config_get_submerged_extinction_g(config);
	s->extinction[2]  = gowl_config_get_submerged_extinction_b(config);
	s->murk           = gowl_config_get_submerged_murk(config);
	s->caustic_scale  = gowl_config_get_submerged_caustic_scale(config);
	s->shafts         = gowl_config_get_submerged_shafts(config);
	s->shaft_lean     = gowl_config_get_submerged_shaft_lean(config);
	s->mote_size      = gowl_config_get_submerged_mote_size(config);
	s->surface        = gowl_config_get_submerged_surface(config);
	s->sway           = gowl_config_get_submerged_sway(config);
	s->fog            = gowl_config_get_submerged_fog(config);
	s->drift          = gowl_config_get_submerged_drift(config);
	s->clarity        = gowl_config_get_submerged_clarity(config);
	s->opacity        = gowl_config_get_submerged_opacity(config);
	s->brightness     = gowl_config_get_submerged_brightness(config);

	s->common.fps          = gowl_config_get_submerged_fps(config);
	s->common.scale        = gowl_config_get_submerged_scale(config);
	s->common.frost        = gowl_config_get_submerged_frost(config);
	s->common.frost_passes = gowl_config_get_submerged_frost_passes(config);

	s->water[0] = s->water[1] = s->water[2] = 1.0;
	spec = gowl_config_get_submerged_water(config);
	tint = spec != NULL ? gowl_color_new_from_hex(spec) : NULL;
	if (tint != NULL) {
		s->water[0] = tint->r;
		s->water[1] = tint->g;
		s->water[2] = tint->b;
		gowl_color_free(tint);
	}
}

static void
submerged_advance(gpointer clock, gconstpointer style, gdouble dt)
{
	const GowlSubmergedStyle *s = style;

	gowl_fx_submerged_advance(clock, dt, s->speed, s->drift);
}

static gboolean
submerged_draw(GowlFxPass *pass, const GowlFxTexture *soft,
               const GowlFxTexture *sharp, gconstpointer style,
               gconstpointer clock, const GowlBackdropPlan *plan,
               gdouble radius, guint seed)
{
	const GowlSubmergedStyle *s = style;
	GowlFxSubmergedParams    p;
	gdouble               scale = (plan->scale_x + plan->scale_y) * 0.5;

	gowl_fx_submerged_params_init(&p);
	p.width  = plan->buf_width;
	p.height = plan->buf_height;
	p.radius = (gfloat)(radius * scale);

	/*
	 * Lengths scale with the render and nothing else does.
	 *
	 * `mote_size' and `sway' are pixels.  `depth' is METRES of water
	 * and `extinction' is per metre -- scaling either would make
	 * `submerged-scale' change how blue the window is, which is the
	 * setting doing something it has no business doing.
	 */
	p.mote_size     = (gfloat)(s->mote_size * scale);
	p.sway          = (gfloat)(s->sway * scale);
	p.depth         = (gfloat)s->depth;
	p.extinction[0] = (gfloat)s->extinction[0];
	p.extinction[1] = (gfloat)s->extinction[1];
	p.extinction[2] = (gfloat)s->extinction[2];
	p.murk          = (gfloat)s->murk;
	p.caustics      = (gfloat)s->caustics;
	p.caustic_scale = (gfloat)s->caustic_scale;
	p.shafts        = (gfloat)s->shafts;
	p.shaft_lean    = (gfloat)s->shaft_lean;
	p.motes         = (gfloat)s->motes;
	p.surface       = (gfloat)s->surface;
	p.fog           = (gfloat)s->fog;
	p.clarity       = (gfloat)s->clarity;
	p.brightness    = (gfloat)s->brightness;
	p.alpha         = (gfloat)s->opacity;
	p.water[0]      = (gfloat)s->water[0];
	p.water[1]      = (gfloat)s->water[1];
	p.water[2]      = (gfloat)s->water[2];

	p.src_origin[0] = (gfloat)plan->origin_x;
	p.src_origin[1] = (gfloat)plan->origin_y;
	p.src_scale     = (gfloat)plan->src_scale;
	/* Which film this window shows.  Anchored to the WINDOW so it stays
	 * put when the window moves, and per-client so two windows side by
	 * side are not the same film twice. */
	p.seed = (gfloat)(seed % 9973u) * 0.0016f;

	return gowl_fx_pass_submerged(pass, soft, sharp, &p, clock);
}

static const GowlBackdropHostVTable submerged_vtable = {
	"submerged",
	GOWL_BACKDROP_SUBMERGED,
	sizeof(GowlSubmergedStyle),
	sizeof(GowlFxSubmergedClock),
	submerged_read_style,
	submerged_advance,
	submerged_draw
};

/* ── Hooks, forwarded ────────────────────────────────────────────── */

static gboolean
submerged_frame(GowlSceneEffect *effect, GowlCompositor *self, GowlMonitor *m,
               gint64 now_us)
{
	GowlModuleSubmerged *mod = GOWL_MODULE_SUBMERGED(effect);

	g_weak_ref_set(&mod->compositor, self);
	return gowl_backdrop_host_frame(mod->host, self, m, now_us);
}

static gboolean
submerged_client_event(GowlSceneEffect *effect, GowlCompositor *self,
                      GowlClient *c, GowlSceneEffectEvent event,
                      const struct wlr_box *previous, gboolean interactive)
{
	GowlModuleSubmerged *mod = GOWL_MODULE_SUBMERGED(effect);

	g_weak_ref_set(&mod->compositor, self);
	return gowl_backdrop_host_client_event(mod->host, self, c, event);
}

static void
submerged_client_placed(GowlSceneEffect *effect, GowlCompositor *self,
                       GowlClient *c, gboolean settled)
{
	GowlModuleSubmerged *mod = GOWL_MODULE_SUBMERGED(effect);

	g_weak_ref_set(&mod->compositor, self);
	gowl_backdrop_host_client_placed(mod->host, self, c);
}

static void
submerged_alpha_changed(GowlSceneEffect *effect, GowlClient *c, gfloat alpha)
{
	GowlModuleSubmerged *mod = GOWL_MODULE_SUBMERGED(effect);

	gowl_backdrop_host_alpha_changed(mod->host, c);
}

static void
submerged_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                         GowlMonitor *m)
{
	GowlModuleSubmerged *mod = GOWL_MODULE_SUBMERGED(effect);

	gowl_backdrop_host_monitor_removed(mod->host, self, m);
}

static void
submerged_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	GowlModuleSubmerged *mod = GOWL_MODULE_SUBMERGED(effect);

	gowl_backdrop_host_finish(mod->host, self);
	g_weak_ref_set(&mod->compositor, NULL);
}

static void
submerged_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_event    = submerged_client_event;
	iface->alpha_changed   = submerged_alpha_changed;
	iface->monitor_removed = submerged_monitor_removed;
	iface->finish          = submerged_finish;
	iface->client_placed   = submerged_client_placed;
	iface->frame           = submerged_frame;
}

static void
submerged_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	submerged_finish(GOWL_SCENE_EFFECT(handler), compositor);
}

static void
submerged_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = submerged_shutdown;
}

/* ── Module ──────────────────────────────────────────────────────── */

/* The same priority as every other backdrop, and after the animation
 * module for the hooks all of them hear. */
#define GOWL_SUBMERGED_PRIORITY (10)

static gboolean
submerged_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_SUBMERGED_PRIORITY);
	return TRUE;
}

static void
submerged_deactivate(GowlModule *base)
{
	GowlModuleSubmerged *mod = GOWL_MODULE_SUBMERGED(base);
	GowlCompositor     *self = g_weak_ref_get(&mod->compositor);

	/* The compositor may already be gone -- a module outlives it when
	 * the manager is released last -- and the host copes with NULL. */
	gowl_backdrop_host_finish(mod->host, self);
	if (self != NULL)
		g_object_unref(self);
}

static const gchar *submerged_name(GowlModule *m)    { return "submerged"; }
static const gchar *submerged_version(GowlModule *m) { return "0.1.0"; }
static const gchar *submerged_description(GowlModule *m)
{
	return "Shows the wallpaper as though the window were under water";
}

static void
submerged_finalize(GObject *object)
{
	GowlModuleSubmerged *mod = GOWL_MODULE_SUBMERGED(object);

	submerged_deactivate(GOWL_MODULE(object));
	g_clear_pointer(&mod->host, gowl_backdrop_host_free);
	g_weak_ref_clear(&mod->compositor);
	G_OBJECT_CLASS(gowl_module_submerged_parent_class)->finalize(object);
}

static void
gowl_module_submerged_class_init(GowlModuleSubmergedClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = submerged_activate;
	mod->deactivate      = submerged_deactivate;
	mod->get_name        = submerged_name;
	mod->get_description = submerged_description;
	mod->get_version     = submerged_version;
	G_OBJECT_CLASS(klass)->finalize = submerged_finalize;
}

static void
gowl_module_submerged_init(GowlModuleSubmerged *mod)
{
	g_weak_ref_init(&mod->compositor, NULL);
	mod->host = gowl_backdrop_host_new(&submerged_vtable);
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_SUBMERGED_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_SUBMERGED;
}
