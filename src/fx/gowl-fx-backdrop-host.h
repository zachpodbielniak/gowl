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

#ifndef GOWL_FX_BACKDROP_HOST_H
#define GOWL_FX_BACKDROP_HOST_H

#include <glib-object.h>

#include "gowl-fx.h"
#include "gowl-types.h"
#include "gowl-enums.h"
#include "interfaces/gowl-scene-effect.h"
#include "util/gowl-backdrop-plan.h"

G_BEGIN_DECLS

/*
 * The half of an animated backdrop that is not optics.
 *
 * WHY THIS EXISTS.  modules/liquidwater, liquidrain, fizz, leaves and
 * snow are five copies of the same eight hundred lines: a clock, a
 * per-output frame throttle, a wallpaper capture taken with the windows
 * hidden, a per-client swapchain and scene buffer, a weak reference to
 * the compositor, and a teardown that has to survive the renderer going
 * away underneath it.  None of that changes with what is being drawn,
 * and the copies have already had to be fixed in lockstep more than once
 * -- the interactive-drag rate fix went into all five by hand.
 *
 * Four more effects would have been four more copies.  Instead they
 * share this, and each module is a vtable, a settings struct and a
 * shader.  The five older ones are NOT ported: they work, they are
 * tested, and rewriting working code to prove a point is how a
 * refactor becomes a regression.  They can move one at a time.
 *
 * WHAT A HOST DOES NOT DECIDE.  It does not know what the effect looks
 * like, what its config keys are called, or what its clock means.  It
 * knows when to redraw, what to draw onto, and when to stop.
 */

/**
 * GowlBackdropHostCommon:
 * @fps: redraw no more often than this, per output; 0 is every frame the
 *   output offers
 * @scale: render at 1/@scale of the window, 1 to 4
 * @frost: how much the wallpaper behind is blurred before the shader
 *   sees it, 1 to 8
 * @frost_passes: how many box passes of that blur, 1 to 6
 *
 * The four settings every animated backdrop has and the host reads.
 *
 * MUST BE THE FIRST MEMBER of whatever struct an effect keeps its own
 * settings in, so the host can find them from a void pointer.  That is a
 * blunt contract and a deliberate one: the alternative is four accessor
 * callbacks per effect to fetch four integers.
 */
typedef struct {
	gint fps;
	gint scale;
	gint frost;
	gint frost_passes;
} GowlBackdropHostCommon;

/**
 * GowlBackdropHostVTable:
 * @name: the effect's short name, as in the module and the config keys.
 *   Used for the per-client data key and in warnings
 * @style: the #GowlBackdropStyle this effect draws for.  The host draws
 *   nothing while `window-backdrop' is anything else
 * @style_size: sizeof the effect's settings struct, whose first member
 *   is a #GowlBackdropHostCommon
 * @clock_size: sizeof the effect's clock struct.  Zeroed at the start
 *   and after a renderer loss, which every clock here reads as "just
 *   started"
 * @read_style: (scope call): fill the settings struct from the config.
 *   Called every tick; the host compares the result with memcmp() and
 *   only re-captures when something it cares about moved
 * @advance: (scope call): move the clock on by @dt seconds
 * @draw: (scope call): draw one window's backdrop into the pass
 *
 * Everything a host needs to know about one effect.
 */
typedef struct {
	const gchar       *name;
	GowlBackdropStyle  style;
	gsize              style_size;
	gsize              clock_size;

	void     (*read_style) (gpointer                style,
	                        GowlConfig             *config);
	void     (*advance)    (gpointer                clock,
	                        gconstpointer           style,
	                        gdouble                 dt);
	gboolean (*draw)       (GowlFxPass             *pass,
	                        const GowlFxTexture    *soft,
	                        const GowlFxTexture    *sharp,
	                        gconstpointer           style,
	                        gconstpointer           clock,
	                        const GowlBackdropPlan *plan,
	                        gdouble                 radius,
	                        guint                   seed);
} GowlBackdropHostVTable;

typedef struct _GowlBackdropHost GowlBackdropHost;

/**
 * gowl_backdrop_host_new:
 * @vt: the effect.  Must outlive the host; a static is what every
 *   caller uses
 *
 * Returns: (transfer full): a host that has drawn nothing yet
 */
GowlBackdropHost *gowl_backdrop_host_new  (const GowlBackdropHostVTable *vt);

/**
 * gowl_backdrop_host_free:
 * @host: (nullable) (transfer full): the host
 *
 * Releases the host.  Call gowl_backdrop_host_finish() first if the
 * compositor is still alive: this cannot take scene nodes down without
 * it.
 */
void              gowl_backdrop_host_free (GowlBackdropHost *host);

/**
 * gowl_backdrop_host_frame:
 * @host: the host
 * @self: the compositor
 * @m: the output this frame is for
 * @now_us: the frame's instant
 *
 * The clock and the redraw, once per output per frame.
 *
 * Returns: %TRUE while there is something on this output to keep
 *   drawing, which is what asks for another frame.  An output with no
 *   translucent window on it goes back to sleep, and so does a locked
 *   session and a style that is no longer this one.
 */
gboolean gowl_backdrop_host_frame (GowlBackdropHost *host,
                                   GowlCompositor   *self,
                                   GowlMonitor      *m,
                                   gint64            now_us);

/**
 * gowl_backdrop_host_client_event:
 * @host: the host
 * @self: the compositor
 * @c: (nullable): the client
 * @event: what happened to it
 *
 * Returns: %FALSE always --- this claims nothing.
 */
gboolean gowl_backdrop_host_client_event (GowlBackdropHost     *host,
                                          GowlCompositor       *self,
                                          GowlClient           *c,
                                          GowlSceneEffectEvent  event);

/**
 * gowl_backdrop_host_client_placed:
 * @host: the host
 * @self: the compositor
 * @c: (nullable): the client that was just placed
 *
 * Draws the window's backdrop, which is also the BOOTSTRAP: an output
 * with nothing changing on it has stopped scheduling frames, so nothing
 * would start the clock.  Skipped while the window is being dragged --
 * see gowl_compositor_client_is_grabbed().
 */
void gowl_backdrop_host_client_placed (GowlBackdropHost *host,
                                       GowlCompositor   *self,
                                       GowlClient       *c);

/**
 * gowl_backdrop_host_alpha_changed:
 * @host: the host
 * @c: (nullable): the client whose opacity moved
 */
void gowl_backdrop_host_alpha_changed (GowlBackdropHost *host,
                                       GowlClient       *c);

/**
 * gowl_backdrop_host_monitor_removed:
 * @host: the host
 * @self: the compositor
 * @m: the output that went away
 */
void gowl_backdrop_host_monitor_removed (GowlBackdropHost *host,
                                         GowlCompositor   *self,
                                         GowlMonitor      *m);

/**
 * gowl_backdrop_host_finish:
 * @host: the host
 * @self: (nullable): the compositor, if it is still there
 *
 * Takes every node down, drops the captured wallpapers and the GL
 * context, and forgets where the clock was --- the wall kept going while
 * the renderer was away, and the next advance must not be handed the
 * gap.  Safe to call more than once, and safe with @self %NULL, which is
 * what a module deactivated after its compositor sees.
 */
void gowl_backdrop_host_finish (GowlBackdropHost *host,
                                GowlCompositor   *self);

G_END_DECLS

#endif /* GOWL_FX_BACKDROP_HOST_H */
