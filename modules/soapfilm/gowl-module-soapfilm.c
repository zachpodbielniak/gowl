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
 * The soap film: the wallpaper through a draining, iridescent membrane.
 *
 * The optics are in fx/gowl-fx-soap.c -- thin-film interference, which
 * is the one thing in this directory that makes colour rather than
 * moving it.
 *
 * EVERYTHING THAT IS NOT OPTICS IS IN THE SHARED HOST.  The five older
 * animated backdrops each carry their own copy of the clock, the
 * per-output frame throttle, the wallpaper capture, the per-client
 * swapchain and the teardown -- eight hundred lines, five times, and
 * the interactive-drag fix had to be applied to all five by hand.  This
 * module and the three beside it use fx/gowl-fx-backdrop-host.c
 * instead, so what is left here is what is actually about soap: a
 * settings struct, a config read, and the call that draws.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-soapfilm"

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

#define GOWL_TYPE_MODULE_SOAPFILM (gowl_module_soapfilm_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleSoapfilm, gowl_module_soapfilm,
                     GOWL, MODULE_SOAPFILM, GowlModule)

/*
 * The look, out of the config, in LOGICAL pixels.
 *
 * The host compares it with memcmp(), so every field has to be part of
 * what "the look" means -- and the common block has to come FIRST,
 * which is the contract in gowl-fx-backdrop-host.h.
 */
typedef struct {
	GowlBackdropHostCommon common;

	gdouble thickness;
	gdouble thin;
	gdouble drain;
	gdouble turbulence;
	gdouble swirl;
	gdouble index;
	gdouble gain;
	gdouble sheen;
	gdouble wedge;
	gdouble pop;
	gdouble meniscus;
	gdouble fog;
	gdouble speed;
	gdouble life;
	gdouble clarity;
	gdouble opacity;
	gdouble brightness;
	gdouble light;
	gdouble tint[3];
} GowlSoapStyle;

struct _GowlModuleSoapfilm {
	GowlModule       parent_instance;
	GWeakRef         compositor;
	GowlBackdropHost *host;
};

static void soapfilm_effect_init(GowlSceneEffectInterface *iface);
static void soapfilm_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleSoapfilm, gowl_module_soapfilm,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, soapfilm_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, soapfilm_shutdown_init))

/* ── The look ────────────────────────────────────────────────────── */

static void
soap_read_style(gpointer style, GowlConfig *config)
{
	GowlSoapStyle *s = style;
	GowlColor     *tint;
	const gchar   *spec;
	gdouble       k;

	memset(s, 0, sizeof(*s));

	/*
	 * One knob over the whole preset, and it scales what "how much is
	 * happening" means for a film: how hard the thin patches churn, and
	 * how fast they rise.  Deliberately NOT the thickness -- a busier
	 * film is not a thicker one, and turning the thickness up past the
	 * point where the bands are a pixel wide gives aliasing, not
	 * intensity.
	 */
	k = gowl_config_get_soap_intensity(config);

	s->turbulence = MIN(2.0, gowl_config_get_soap_turbulence(config) * k);
	s->speed      = MIN(5.0, gowl_config_get_soap_speed(config) * k);

	s->thickness  = gowl_config_get_soap_thickness(config);
	s->thin       = gowl_config_get_soap_thin(config);
	s->drain      = gowl_config_get_soap_drain(config);
	s->swirl      = gowl_config_get_soap_swirl(config);
	s->index      = gowl_config_get_soap_index(config);
	s->gain       = gowl_config_get_soap_gain(config);
	s->sheen      = gowl_config_get_soap_sheen(config);
	s->wedge      = gowl_config_get_soap_wedge(config);
	s->pop        = gowl_config_get_soap_pop(config);
	s->meniscus   = gowl_config_get_soap_meniscus(config);
	s->fog        = gowl_config_get_soap_fog(config);
	s->life       = gowl_config_get_soap_life(config);
	s->clarity    = gowl_config_get_soap_clarity(config);
	s->opacity    = gowl_config_get_soap_opacity(config);
	s->brightness = gowl_config_get_soap_brightness(config);
	s->light      = gowl_config_get_soap_light(config);

	s->common.fps          = gowl_config_get_soap_fps(config);
	s->common.scale        = gowl_config_get_soap_scale(config);
	s->common.frost        = gowl_config_get_soap_frost(config);
	s->common.frost_passes = gowl_config_get_soap_frost_passes(config);

	s->tint[0] = s->tint[1] = s->tint[2] = 1.0;
	spec = gowl_config_get_soap_tint(config);
	tint = spec != NULL ? gowl_color_new_from_hex(spec) : NULL;
	if (tint != NULL) {
		s->tint[0] = tint->r;
		s->tint[1] = tint->g;
		s->tint[2] = tint->b;
		gowl_color_free(tint);
	}
}

static void
soap_advance(gpointer clock, gconstpointer style, gdouble dt)
{
	const GowlSoapStyle *s = style;

	gowl_fx_soap_advance(clock, dt, s->life, s->speed);
}

static gboolean
soap_draw(GowlFxPass *pass, const GowlFxTexture *soft,
          const GowlFxTexture *sharp, gconstpointer style,
          gconstpointer clock, const GowlBackdropPlan *plan,
          gdouble radius, guint seed)
{
	const GowlSoapStyle *s = style;
	GowlFxSoapParams    p;
	gdouble              scale = (plan->scale_x + plan->scale_y) * 0.5;
	gdouble              rad = s->light * G_PI / 180.0;
	gdouble              lx, ly;

	gowl_fx_soap_params_init(&p);
	p.width  = plan->buf_width;
	p.height = plan->buf_height;
	p.radius = (gfloat)(radius * scale);

	/*
	 * Lengths scale with the render; everything else does not.
	 *
	 * `wedge' is a length -- pixels of displacement -- and so is the
	 * corner radius.  `thickness' is in NANOMETRES of film and has
	 * nothing to do with how many screen pixels it is drawn over, so
	 * scaling it would change the colour of the effect every time
	 * somebody changed `soap-scale'.  `swirl' is cells across the pane,
	 * which is already a ratio.
	 */
	p.wedge      = (gfloat)(s->wedge * scale);
	p.thickness  = (gfloat)s->thickness;
	p.thin       = (gfloat)s->thin;
	p.drain      = (gfloat)s->drain;
	p.turbulence = (gfloat)s->turbulence;
	p.swirl      = (gfloat)s->swirl;
	p.index      = (gfloat)s->index;
	p.gain       = (gfloat)s->gain;
	p.sheen      = (gfloat)s->sheen;
	p.pop        = (gfloat)s->pop;
	p.meniscus   = (gfloat)s->meniscus;
	p.fog        = (gfloat)s->fog;
	p.clarity    = (gfloat)s->clarity;
	p.brightness = (gfloat)s->brightness;
	p.alpha      = (gfloat)s->opacity;
	p.dispersion = (gfloat)(0.7 * scale);
	p.tint[0]    = (gfloat)s->tint[0];
	p.tint[1]    = (gfloat)s->tint[1];
	p.tint[2]    = (gfloat)s->tint[2];

	/* 0 degrees is straight above, positive turns clockwise, and the
	 * light sits well off the pane. */
	lx = sin(rad) * 0.7;
	ly = -cos(rad) * 0.7;
	p.light[0] = (gfloat)lx;
	p.light[1] = (gfloat)ly;
	p.light[2] = (gfloat)sqrt(MAX(0.0, 1.0 - lx * lx - ly * ly));

	p.src_origin[0] = (gfloat)plan->origin_x;
	p.src_origin[1] = (gfloat)plan->origin_y;
	p.src_scale     = (gfloat)plan->src_scale;
	/* Which film this window shows.  Anchored to the WINDOW so it stays
	 * put when the window moves, and per-client so two windows side by
	 * side are not the same film twice. */
	p.seed = (gfloat)(seed % 9973u) * 0.0016f;

	return gowl_fx_pass_soap(pass, soft, sharp, &p, clock);
}

static const GowlBackdropHostVTable soap_vtable = {
	"soap",
	GOWL_BACKDROP_SOAP,
	sizeof(GowlSoapStyle),
	sizeof(GowlFxSoapClock),
	soap_read_style,
	soap_advance,
	soap_draw
};

/* ── Hooks, forwarded ────────────────────────────────────────────── */

static gboolean
soapfilm_frame(GowlSceneEffect *effect, GowlCompositor *self, GowlMonitor *m,
               gint64 now_us)
{
	GowlModuleSoapfilm *mod = GOWL_MODULE_SOAPFILM(effect);

	g_weak_ref_set(&mod->compositor, self);
	return gowl_backdrop_host_frame(mod->host, self, m, now_us);
}

static gboolean
soapfilm_client_event(GowlSceneEffect *effect, GowlCompositor *self,
                      GowlClient *c, GowlSceneEffectEvent event,
                      const struct wlr_box *previous, gboolean interactive)
{
	GowlModuleSoapfilm *mod = GOWL_MODULE_SOAPFILM(effect);

	g_weak_ref_set(&mod->compositor, self);
	return gowl_backdrop_host_client_event(mod->host, self, c, event);
}

static void
soapfilm_client_placed(GowlSceneEffect *effect, GowlCompositor *self,
                       GowlClient *c, gboolean settled)
{
	GowlModuleSoapfilm *mod = GOWL_MODULE_SOAPFILM(effect);

	g_weak_ref_set(&mod->compositor, self);
	gowl_backdrop_host_client_placed(mod->host, self, c);
}

static void
soapfilm_alpha_changed(GowlSceneEffect *effect, GowlClient *c, gfloat alpha)
{
	GowlModuleSoapfilm *mod = GOWL_MODULE_SOAPFILM(effect);

	gowl_backdrop_host_alpha_changed(mod->host, c);
}

static void
soapfilm_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                         GowlMonitor *m)
{
	GowlModuleSoapfilm *mod = GOWL_MODULE_SOAPFILM(effect);

	gowl_backdrop_host_monitor_removed(mod->host, self, m);
}

static void
soapfilm_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	GowlModuleSoapfilm *mod = GOWL_MODULE_SOAPFILM(effect);

	gowl_backdrop_host_finish(mod->host, self);
	g_weak_ref_set(&mod->compositor, NULL);
}

static void
soapfilm_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_event    = soapfilm_client_event;
	iface->alpha_changed   = soapfilm_alpha_changed;
	iface->monitor_removed = soapfilm_monitor_removed;
	iface->finish          = soapfilm_finish;
	iface->client_placed   = soapfilm_client_placed;
	iface->frame           = soapfilm_frame;
}

static void
soapfilm_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	soapfilm_finish(GOWL_SCENE_EFFECT(handler), compositor);
}

static void
soapfilm_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = soapfilm_shutdown;
}

/* ── Module ──────────────────────────────────────────────────────── */

/* The same priority as every other backdrop, and after the animation
 * module for the hooks all of them hear. */
#define GOWL_SOAPFILM_PRIORITY (10)

static gboolean
soapfilm_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_SOAPFILM_PRIORITY);
	return TRUE;
}

static void
soapfilm_deactivate(GowlModule *base)
{
	GowlModuleSoapfilm *mod = GOWL_MODULE_SOAPFILM(base);
	GowlCompositor     *self = g_weak_ref_get(&mod->compositor);

	/* The compositor may already be gone -- a module outlives it when
	 * the manager is released last -- and the host copes with NULL. */
	gowl_backdrop_host_finish(mod->host, self);
	if (self != NULL)
		g_object_unref(self);
}

static const gchar *soapfilm_name(GowlModule *m)    { return "soapfilm"; }
static const gchar *soapfilm_version(GowlModule *m) { return "0.1.0"; }
static const gchar *soapfilm_description(GowlModule *m)
{
	return "Shows the wallpaper through an iridescent soap film";
}

static void
soapfilm_finalize(GObject *object)
{
	GowlModuleSoapfilm *mod = GOWL_MODULE_SOAPFILM(object);

	soapfilm_deactivate(GOWL_MODULE(object));
	g_clear_pointer(&mod->host, gowl_backdrop_host_free);
	g_weak_ref_clear(&mod->compositor);
	G_OBJECT_CLASS(gowl_module_soapfilm_parent_class)->finalize(object);
}

static void
gowl_module_soapfilm_class_init(GowlModuleSoapfilmClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = soapfilm_activate;
	mod->deactivate      = soapfilm_deactivate;
	mod->get_name        = soapfilm_name;
	mod->get_description = soapfilm_description;
	mod->get_version     = soapfilm_version;
	G_OBJECT_CLASS(klass)->finalize = soapfilm_finalize;
}

static void
gowl_module_soapfilm_init(GowlModuleSoapfilm *mod)
{
	g_weak_ref_init(&mod->compositor, NULL);
	mod->host = gowl_backdrop_host_new(&soap_vtable);
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_SOAPFILM_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_SOAPFILM;
}
