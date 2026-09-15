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
 * Dew: an orb web across the window, strung with water.
 *
 * The optics are the rain's -- a drop is a ball lens -- and the effect
 * is WHERE the drops are; see fx/gowl-fx-dew.c.  The host half is
 * shared; see the note in modules/dew.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-dew"

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

#define GOWL_TYPE_MODULE_DEW (gowl_module_dew_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleDew, gowl_module_dew,
                     GOWL, MODULE_DEW, GowlModule)

/*
 * The look, out of the config, in LOGICAL pixels.
 *
 * The host compares it with memcmp(), so every field has to be part of
 * what "the look" means -- and the common block has to come FIRST,
 * which is the contract in gowl-fx-backdrop-host.h.
 */
typedef struct {
	GowlBackdropHostCommon common;

	gdouble radials;
	gdouble pitch;
	gdouble thread;
	gdouble drop;
	gdouble spacing;
	gdouble sag;
	gdouble depth;
	gdouble bulge;
	gdouble silk;
	gdouble glint;
	gdouble shine;
	gdouble rim;
	gdouble sway;
	gdouble fog;
	gdouble speed;
	gdouble clarity;
	gdouble opacity;
	gdouble brightness;
	gdouble light;
	gdouble tint[3];
} GowlDewStyle;

struct _GowlModuleDew {
	GowlModule       parent_instance;
	GWeakRef         compositor;
	GowlBackdropHost *host;
};

static void dew_effect_init(GowlSceneEffectInterface *iface);
static void dew_shutdown_init(GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleDew, gowl_module_dew,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, dew_effect_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, dew_shutdown_init))

/* ── The look ────────────────────────────────────────────────────── */

static void
dew_read_style(gpointer style, GowlConfig *config)
{
	GowlDewStyle *s = style;
	GowlColor    *tint;
	const gchar  *spec;
	gdouble      k;

	memset(s, 0, sizeof(*s));

	/*
	 * One knob over the whole preset, and for a web it is the WIND:
	 * how far it breathes and how fast.  There is nothing else a web
	 * does more or less of -- the number of drops is the structure, and
	 * scaling that would be a different web rather than a livelier one.
	 */
	k = gowl_config_get_dew_intensity(config);

	s->sway  = MIN(40.0, gowl_config_get_dew_sway(config) * k);
	s->speed = MIN(5.0, gowl_config_get_dew_speed(config) * k);

	s->radials    = gowl_config_get_dew_radials(config);
	s->pitch      = gowl_config_get_dew_pitch(config);
	s->thread     = gowl_config_get_dew_thread(config);
	s->drop       = gowl_config_get_dew_drop(config);
	s->spacing    = gowl_config_get_dew_spacing(config);
	s->sag        = gowl_config_get_dew_sag(config);
	s->depth      = gowl_config_get_dew_depth(config);
	s->bulge      = gowl_config_get_dew_bulge(config);
	s->silk       = gowl_config_get_dew_silk(config);
	s->glint      = gowl_config_get_dew_glint(config);
	s->shine      = gowl_config_get_dew_shine(config);
	s->rim        = gowl_config_get_dew_rim(config);
	s->fog        = gowl_config_get_dew_fog(config);
	s->clarity    = gowl_config_get_dew_clarity(config);
	s->opacity    = gowl_config_get_dew_opacity(config);
	s->brightness = gowl_config_get_dew_brightness(config);
	s->light      = gowl_config_get_dew_light(config);

	s->common.fps          = gowl_config_get_dew_fps(config);
	s->common.scale        = gowl_config_get_dew_scale(config);
	s->common.frost        = gowl_config_get_dew_frost(config);
	s->common.frost_passes = gowl_config_get_dew_frost_passes(config);

	s->tint[0] = s->tint[1] = s->tint[2] = 1.0;
	spec = gowl_config_get_dew_tint(config);
	tint = spec != NULL ? gowl_color_new_from_hex(spec) : NULL;
	if (tint != NULL) {
		s->tint[0] = tint->r;
		s->tint[1] = tint->g;
		s->tint[2] = tint->b;
		gowl_color_free(tint);
	}
}

static void
dew_advance(gpointer clock, gconstpointer style, gdouble dt)
{
	const GowlDewStyle *s = style;

	gowl_fx_dew_advance(clock, dt, s->speed);
}

static gboolean
dew_draw(GowlFxPass *pass, const GowlFxTexture *soft,
         const GowlFxTexture *sharp, gconstpointer style,
         gconstpointer clock, const GowlBackdropPlan *plan,
         gdouble radius, guint seed)
{
	const GowlDewStyle *s = style;
	GowlFxDewParams    p;
	gdouble         scale = (plan->scale_x + plan->scale_y) * 0.5;
	gdouble              rad = s->light * G_PI / 180.0;
	gdouble              lx, ly;

	gowl_fx_dew_params_init(&p);
	p.width  = plan->buf_width;
	p.height = plan->buf_height;
	p.radius = (gfloat)(radius * scale);

	/*
	 * Lengths scale with the render and nothing else does.
	 *
	 * `pitch', `thread', `drop', `spacing', `sag' and `sway' are all
	 * pixels, and they have to scale TOGETHER: the shader caps the drop
	 * against the spacing and the pitch, so scaling one without the
	 * others would silently change the web rather than its resolution.
	 * `radials' is a count, `depth' is in a drop's own radii and
	 * `shine' is an exponent.
	 */
	p.pitch      = (gfloat)(s->pitch * scale);
	p.thread     = (gfloat)(s->thread * scale);
	p.drop       = (gfloat)(s->drop * scale);
	p.spacing    = (gfloat)(s->spacing * scale);
	p.sag        = (gfloat)(s->sag * scale);
	p.sway       = (gfloat)(s->sway * scale);
	p.radials    = (gfloat)s->radials;
	p.depth      = (gfloat)s->depth;
	p.bulge      = (gfloat)s->bulge;
	p.silk       = (gfloat)s->silk;
	p.glint      = (gfloat)s->glint;
	p.shine      = (gfloat)s->shine;
	p.rim        = (gfloat)s->rim;
	p.fog        = (gfloat)s->fog;
	p.clarity    = (gfloat)s->clarity;
	p.brightness = (gfloat)s->brightness;
	p.alpha      = (gfloat)s->opacity;
	p.dispersion = (gfloat)(0.6 * scale);
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

	return gowl_fx_pass_dew(pass, soft, sharp, &p, clock);
}

static const GowlBackdropHostVTable dew_vtable = {
	"dew",
	GOWL_BACKDROP_DEW,
	sizeof(GowlDewStyle),
	sizeof(GowlFxDewClock),
	dew_read_style,
	dew_advance,
	dew_draw
};

/* ── Hooks, forwarded ────────────────────────────────────────────── */

static gboolean
dew_frame(GowlSceneEffect *effect, GowlCompositor *self, GowlMonitor *m,
               gint64 now_us)
{
	GowlModuleDew *mod = GOWL_MODULE_DEW(effect);

	g_weak_ref_set(&mod->compositor, self);
	return gowl_backdrop_host_frame(mod->host, self, m, now_us);
}

static gboolean
dew_client_event(GowlSceneEffect *effect, GowlCompositor *self,
                      GowlClient *c, GowlSceneEffectEvent event,
                      const struct wlr_box *previous, gboolean interactive)
{
	GowlModuleDew *mod = GOWL_MODULE_DEW(effect);

	g_weak_ref_set(&mod->compositor, self);
	return gowl_backdrop_host_client_event(mod->host, self, c, event);
}

static void
dew_client_placed(GowlSceneEffect *effect, GowlCompositor *self,
                       GowlClient *c, gboolean settled)
{
	GowlModuleDew *mod = GOWL_MODULE_DEW(effect);

	g_weak_ref_set(&mod->compositor, self);
	gowl_backdrop_host_client_placed(mod->host, self, c);
}

static void
dew_alpha_changed(GowlSceneEffect *effect, GowlClient *c, gfloat alpha)
{
	GowlModuleDew *mod = GOWL_MODULE_DEW(effect);

	gowl_backdrop_host_alpha_changed(mod->host, c);
}

static void
dew_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,
                         GowlMonitor *m)
{
	GowlModuleDew *mod = GOWL_MODULE_DEW(effect);

	gowl_backdrop_host_monitor_removed(mod->host, self, m);
}

static void
dew_finish(GowlSceneEffect *effect, GowlCompositor *self)
{
	GowlModuleDew *mod = GOWL_MODULE_DEW(effect);

	gowl_backdrop_host_finish(mod->host, self);
	g_weak_ref_set(&mod->compositor, NULL);
}

static void
dew_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_event    = dew_client_event;
	iface->alpha_changed   = dew_alpha_changed;
	iface->monitor_removed = dew_monitor_removed;
	iface->finish          = dew_finish;
	iface->client_placed   = dew_client_placed;
	iface->frame           = dew_frame;
}

static void
dew_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	dew_finish(GOWL_SCENE_EFFECT(handler), compositor);
}

static void
dew_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = dew_shutdown;
}

/* ── Module ──────────────────────────────────────────────────────── */

/* The same priority as every other backdrop, and after the animation
 * module for the hooks all of them hear. */
#define GOWL_DEW_PRIORITY (10)

static gboolean
dew_activate(GowlModule *base)
{
	gowl_module_set_priority(base, GOWL_DEW_PRIORITY);
	return TRUE;
}

static void
dew_deactivate(GowlModule *base)
{
	GowlModuleDew *mod = GOWL_MODULE_DEW(base);
	GowlCompositor     *self = g_weak_ref_get(&mod->compositor);

	/* The compositor may already be gone -- a module outlives it when
	 * the manager is released last -- and the host copes with NULL. */
	gowl_backdrop_host_finish(mod->host, self);
	if (self != NULL)
		g_object_unref(self);
}

static const gchar *dew_name(GowlModule *m)    { return "dew"; }
static const gchar *dew_version(GowlModule *m) { return "0.1.0"; }
static const gchar *dew_description(GowlModule *m)
{
	return "Draws an orb web across the window, strung with dew";
}

static void
dew_finalize(GObject *object)
{
	GowlModuleDew *mod = GOWL_MODULE_DEW(object);

	dew_deactivate(GOWL_MODULE(object));
	g_clear_pointer(&mod->host, gowl_backdrop_host_free);
	g_weak_ref_clear(&mod->compositor);
	G_OBJECT_CLASS(gowl_module_dew_parent_class)->finalize(object);
}

static void
gowl_module_dew_class_init(GowlModuleDewClass *klass)
{
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);

	mod->activate        = dew_activate;
	mod->deactivate      = dew_deactivate;
	mod->get_name        = dew_name;
	mod->get_description = dew_description;
	mod->get_version     = dew_version;
	G_OBJECT_CLASS(klass)->finalize = dew_finalize;
}

static void
gowl_module_dew_init(GowlModuleDew *mod)
{
	g_weak_ref_init(&mod->compositor, NULL);
	mod->host = gowl_backdrop_host_new(&dew_vtable);
	gowl_module_set_priority(GOWL_MODULE(mod), GOWL_DEW_PRIORITY);
}

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_DEW;
}
