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

#ifndef GOWL_FX_TRANSITION_MODULE_H
#define GOWL_FX_TRANSITION_MODULE_H

/*
 * The whole GObject side of a transition module, from one line:
 *
 *     static const GowlFxTransitionKind slide_kind = { ... };
 *     GOWL_FX_DEFINE_TRANSITION_MODULE(GowlModuleSlide, gowl_module_slide,
 *                                      &slide_kind, "0.1.0",
 *                                      "Slides the desktop sideways ...")
 *
 * defines the module type, implements GowlSceneEffect by forwarding each
 * hook to a GowlFxTransitionHost, registers the transition at startup (and
 * again lazily from any hook, for a module loaded later), withdraws it on
 * deactivate, frees the GL at shutdown, and exports
 * gowl_module_register().  What is left in the module is its kind and its
 * draw function -- which is the part that differs.
 *
 * Every transition sorts at GOWL_FX_TRANSITION_PRIORITY, ahead of the
 * animation module, for the cube's reason: the one consumable event a
 * transition answers is the reveal it must swallow, and it can only do
 * that if it is asked before the animation module starts a fade.  They
 * claim NOTHING else (tests/test-transition-guard.sh), so being ahead
 * costs the modules behind them nothing.
 */

#include <glib-object.h>
#include <gmodule.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-scene-effect.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "fx/gowl-fx-transition.h"

#define GOWL_FX_TRANSITION_PRIORITY (-10)

#define GOWL_FX_DEFINE_TRANSITION_MODULE(TN, t_n, KIND, VERSION, DESC)        \
                                                                               \
typedef struct {                                                               \
	GowlModule            parent_instance;                                 \
	GowlFxTransitionHost *host;                                            \
} TN;                                                                          \
                                                                               \
typedef struct {                                                               \
	GowlModuleClass parent_class;                                          \
} TN##Class;                                                                   \
                                                                               \
static void t_n##_effect_init   (GowlSceneEffectInterface *iface);             \
static void t_n##_startup_init  (GowlStartupHandlerInterface *iface);          \
static void t_n##_shutdown_init (GowlShutdownHandlerInterface *iface);         \
GType t_n##_get_type (void);                                                   \
                                                                               \
G_DEFINE_TYPE_WITH_CODE(TN, t_n, GOWL_TYPE_MODULE,                             \
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, t_n##_effect_init)       \
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, t_n##_startup_init)   \
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, t_n##_shutdown_init))\
                                                                               \
static gboolean                                                                \
t_n##_frame(GowlSceneEffect *effect, GowlCompositor *self, GowlMonitor *m,     \
            gint64 now)                                                        \
{                                                                              \
	return gowl_fx_transition_host_frame(((TN *)effect)->host, self, m,    \
	                                     now);                             \
}                                                                              \
                                                                               \
static gboolean                                                                \
t_n##_client_event(GowlSceneEffect *effect, GowlCompositor *self,              \
                   GowlClient *c, GowlSceneEffectEvent event,                  \
                   const struct wlr_box *previous, gboolean interactive)       \
{                                                                              \
	(void)previous;                                                        \
	(void)interactive;                                                     \
	return gowl_fx_transition_host_client_event(((TN *)effect)->host,      \
	                                            self, c, event);           \
}                                                                              \
                                                                               \
static void                                                                    \
t_n##_monitor_removed(GowlSceneEffect *effect, GowlCompositor *self,           \
                      GowlMonitor *m)                                          \
{                                                                              \
	gowl_fx_transition_host_monitor_removed(((TN *)effect)->host, self, m);\
}                                                                              \
                                                                               \
static void                                                                    \
t_n##_finish(GowlSceneEffect *effect, GowlCompositor *self)                    \
{                                                                              \
	(void)self;                                                            \
	gowl_fx_transition_host_finish(((TN *)effect)->host);                  \
}                                                                              \
                                                                               \
static void                                                                    \
t_n##_effect_init(GowlSceneEffectInterface *iface)                             \
{                                                                              \
	iface->client_event    = t_n##_client_event;                           \
	iface->frame           = t_n##_frame;                                  \
	iface->monitor_removed = t_n##_monitor_removed;                        \
	iface->finish          = t_n##_finish;                                 \
}                                                                              \
                                                                               \
/* At startup the compositor exists and no frame has drawn yet: the       */ \
/* earliest a transition can be offered to Super+: and `tag-transition'. */ \
static void                                                                    \
t_n##_startup(GowlStartupHandler *handler, gpointer compositor)                \
{                                                                              \
	gowl_fx_transition_host_attach(((TN *)handler)->host,                  \
	                               (GowlCompositor *)compositor);          \
}                                                                              \
                                                                               \
static void                                                                    \
t_n##_startup_init(GowlStartupHandlerInterface *iface)                         \
{                                                                              \
	iface->on_startup = t_n##_startup;                                     \
}                                                                              \
                                                                               \
static void                                                                    \
t_n##_shutdown(GowlShutdownHandler *handler, gpointer compositor)              \
{                                                                              \
	(void)compositor;                                                      \
	gowl_fx_transition_host_finish(((TN *)handler)->host);                 \
}                                                                              \
                                                                               \
static void                                                                    \
t_n##_shutdown_init(GowlShutdownHandlerInterface *iface)                       \
{                                                                              \
	iface->on_shutdown = t_n##_shutdown;                                   \
}                                                                              \
                                                                               \
static gboolean                                                                \
t_n##_activate(GowlModule *base)                                               \
{                                                                              \
	gowl_module_set_priority(base, GOWL_FX_TRANSITION_PRIORITY);           \
	return TRUE;                                                           \
}                                                                              \
                                                                               \
static void                                                                    \
t_n##_deactivate(GowlModule *base)                                             \
{                                                                              \
	gowl_fx_transition_host_detach(((TN *)base)->host);                    \
}                                                                              \
                                                                               \
static const gchar *                                                           \
t_n##_name(GowlModule *base)                                                   \
{                                                                              \
	(void)base;                                                            \
	return (KIND)->name;                                                   \
}                                                                              \
                                                                               \
static const gchar *                                                           \
t_n##_version(GowlModule *base)                                                \
{                                                                              \
	(void)base;                                                            \
	return VERSION;                                                        \
}                                                                              \
                                                                               \
static const gchar *                                                           \
t_n##_description(GowlModule *base)                                            \
{                                                                              \
	(void)base;                                                            \
	return DESC;                                                           \
}                                                                              \
                                                                               \
static void                                                                    \
t_n##_finalize(GObject *object)                                                \
{                                                                              \
	g_clear_pointer(&((TN *)object)->host, gowl_fx_transition_host_free);  \
	G_OBJECT_CLASS(t_n##_parent_class)->finalize(object);                  \
}                                                                              \
                                                                               \
static void                                                                    \
t_n##_class_init(TN##Class *klass)                                             \
{                                                                              \
	GowlModuleClass *mod = GOWL_MODULE_CLASS(klass);                       \
                                                                               \
	mod->activate        = t_n##_activate;                                 \
	mod->deactivate      = t_n##_deactivate;                               \
	mod->get_name        = t_n##_name;                                     \
	mod->get_description = t_n##_description;                              \
	mod->get_version     = t_n##_version;                                  \
	G_OBJECT_CLASS(klass)->finalize = t_n##_finalize;                      \
}                                                                              \
                                                                               \
static void                                                                    \
t_n##_init(TN *self)                                                           \
{                                                                              \
	self->host = gowl_fx_transition_host_new(KIND);                        \
	gowl_module_set_priority(GOWL_MODULE(self), GOWL_FX_TRANSITION_PRIORITY);\
}                                                                              \
                                                                               \
G_MODULE_EXPORT GType                                                          \
gowl_module_register(void)                                                     \
{                                                                              \
	return t_n##_get_type();                                               \
}                                                                              \
                                                                               \
/* The kind, for tests/test-transition-render.c, which opens every       */ \
/* module and holds each draw function to the seam rule on a real GPU.   */ \
G_MODULE_EXPORT const GowlFxTransitionKind *gowl_transition_kind (void);       \
G_MODULE_EXPORT const GowlFxTransitionKind *                                   \
gowl_transition_kind(void)                                                     \
{                                                                              \
	return (KIND);                                                         \
}

#endif /* GOWL_FX_TRANSITION_MODULE_H */
