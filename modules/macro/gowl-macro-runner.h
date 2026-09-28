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
 * gowl-macro-runner.h - Running macros: guarded, timelined, threaded.
 *
 * A run is either:
 *   - TIMELINE: the macro body runs under the fault guard on the
 *     compositor thread and returns quickly; the steps it queued play
 *     afterwards on wl_event_loop timers (a wait never blocks).
 *   - THREADED: the body runs under the guard on a worker thread of its
 *     own (alternate stack, own watchdog); steps and compositor calls
 *     hop to the compositor thread through an eventfd queue and wait.
 *
 * Everything the runner reports back -- started, finished, faulted,
 * notify, log, journal -- happens on the compositor thread.
 */

#ifndef GOWL_MACRO_RUNNER_H
#define GOWL_MACRO_RUNNER_H

#include <glib.h>
#include <wayland-server-core.h>

#include "macro/gowl-macro.h"
#include "macro/gowl-macro-private.h"

G_BEGIN_DECLS

typedef struct _GowlMacroRunner GowlMacroRunner;

/* The macro body, however it was provided (a file, a C registration) */
typedef gboolean (*GowlMacroRunnerBody) (GowlMacroContext *ctx,
                                         gpointer          data);

typedef struct {
	void (*started)  (gpointer module, guint id, const gchar *name,
	                  gboolean threaded);
	void (*finished) (gpointer module, guint id, const gchar *name,
	                  const gchar *result, gboolean cancelled);
	void (*faulted)  (gpointer module, guint id, const gchar *name,
	                  gint signo);
	void (*notify)   (gpointer module, const gchar *name,
	                  const gchar *summary, const gchar *body);
	void (*log)      (gpointer module, const gchar *name,
	                  const gchar *message);
	void (*journal)  (gpointer module, const gchar *name, gboolean running);
} GowlMacroRunnerCallbacks;

GowlMacroRunner *gowl_macro_runner_new          (GowlCompositor                 *compositor,
                                                 struct wl_event_loop           *loop,
                                                 const GowlMacroRunnerCallbacks *callbacks,
                                                 gpointer                        module);
void             gowl_macro_runner_shutdown     (GowlMacroRunner  *self);
GowlMacroRunner *gowl_macro_runner_ref          (GowlMacroRunner  *self);
void             gowl_macro_runner_unref        (GowlMacroRunner  *self);

guint            gowl_macro_runner_start        (GowlMacroRunner     *self,
                                                 GowlMacroContext    *ctx,
                                                 GowlMacroRunnerBody  body,
                                                 gpointer             body_data,
                                                 guint                timeout_ms,
                                                 guint                max_steps,
                                                 gchar              **out_reply);
guint            gowl_macro_runner_stop         (GowlMacroRunner  *self,
                                                 const gchar      *which);
guint            gowl_macro_runner_n_active     (GowlMacroRunner  *self);
gboolean         gowl_macro_runner_is_running   (GowlMacroRunner  *self,
                                                 const gchar      *name);
gchar           *gowl_macro_runner_status_json  (GowlMacroRunner  *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlMacroRunner, gowl_macro_runner_unref)

G_END_DECLS

#endif /* GOWL_MACRO_RUNNER_H */
