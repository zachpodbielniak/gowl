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
 * gowl-macro.h - What a gowl macro can call.
 *
 * A macro is a C file the opt-in `macro' module compiles with crispy
 * and runs inside the compositor, on demand: from a keybind, gowl-msg,
 * D-Bus, a compositor event, a timer, the input remapper, or C code.
 * It exports one function:
 *
 *     #include <gowl/gowl.h>
 *
 *     G_MODULE_EXPORT gboolean
 *     gowl_macro_run(GowlMacroContext *ctx)
 *     {
 *         gowl_macro_key(ctx, NULL, "Super+9");
 *         gowl_macro_wait(ctx, 200);
 *         gowl_macro_text(ctx, gowl_macro_find_client(ctx, "app-id:foot"),
 *                         "make\n");
 *         return TRUE;
 *     }
 *
 * TWO MODES.  By default a macro runs on the compositor thread and the
 * input steps it asks for (key, text, button, wait, command, action,
 * focus) are QUEUED and played afterwards on timers -- so a wait never
 * freezes the desktop.  A file containing `#define GOWL_MACRO_THREADED 1'
 * runs on a worker thread instead: steps happen when called,
 * gowl_macro_wait() really sleeps, and anything else that touches the
 * compositor must go through gowl_macro_call_on_compositor().
 *
 * EVERY RUN IS GUARDED.  A crash (SIGSEGV, SIGBUS, SIGFPE, SIGILL,
 * SIGABRT) or running past the time budget (`timeout-ms', 2000 by
 * default) unwinds the macro, quarantines it, logs it and raises a
 * notification; the session keeps running.  A macro that knows it is
 * long says so first: gowl_macro_set_timeout(ctx, 10000), or
 * `#define GOWL_MACRO_TIMEOUT_MS 10000'.  See docs/macros.org.
 */

#ifndef GOWL_MACRO_H
#define GOWL_MACRO_H

#include <glib-object.h>
#include <gmodule.h>

#include "gowl-types.h"
#include "gowl-enums.h"

G_BEGIN_DECLS

/**
 * GOWL_MACRO_ABI:
 *
 * The macro ABI version.  A macro may export
 * `const guint gowl_macro_abi = GOWL_MACRO_ABI;' to be refused cleanly,
 * rather than misbehave, by a gowl it was not written for.
 */
#define GOWL_MACRO_ABI (1)

/**
 * GowlMacroTrigger:
 * @GOWL_MACRO_TRIGGER_API: called from C (or an embedder)
 * @GOWL_MACRO_TRIGGER_IPC: `macro-run' over IPC -- the socket, a
 *   keybind's `ipc-command', cmacs's `gowl-run-command', MCP
 * @GOWL_MACRO_TRIGGER_DBUS: the org.gowl.Macro1 D-Bus service
 * @GOWL_MACRO_TRIGGER_EVENT: a compositor event (`triggers:')
 * @GOWL_MACRO_TRIGGER_TIMER: an interval (`every N: ...')
 * @GOWL_MACRO_TRIGGER_REMAP: an input-remap `{macro: ...}' target
 *
 * What started a run.  The detail string says more: the event name,
 * the remap rule and input, the IPC command line.
 */
typedef enum {
	GOWL_MACRO_TRIGGER_API,
	GOWL_MACRO_TRIGGER_IPC,
	GOWL_MACRO_TRIGGER_DBUS,
	GOWL_MACRO_TRIGGER_EVENT,
	GOWL_MACRO_TRIGGER_TIMER,
	GOWL_MACRO_TRIGGER_REMAP
} GowlMacroTrigger;

#define GOWL_TYPE_MACRO_TRIGGER (gowl_macro_trigger_get_type())
GType gowl_macro_trigger_get_type (void) G_GNUC_CONST;

typedef struct _GowlMacroContext GowlMacroContext;

/**
 * GowlMacroFunc:
 * @ctx: the run
 * @user_data: the data given to gowl_macro_register_func()
 *
 * A macro written in C and registered by name, rather than a file.
 *
 * Returns: %TRUE on success
 */
typedef gboolean (*GowlMacroFunc) (GowlMacroContext *ctx,
                                   gpointer          user_data);

/**
 * GowlMacroCompositorFunc:
 * @compositor: the compositor
 * @user_data: passed through
 *
 * Code run on the compositor thread by gowl_macro_call_on_compositor().
 */
typedef void (*GowlMacroCompositorFunc) (GowlCompositor *compositor,
                                         gpointer        user_data);

/* --- the run --- */
GowlCompositor     *gowl_macro_get_compositor      (GowlMacroContext *ctx);
const gchar        *gowl_macro_get_name            (GowlMacroContext *ctx);
const gchar        *gowl_macro_get_path            (GowlMacroContext *ctx);
guint               gowl_macro_get_argc            (GowlMacroContext *ctx);
const gchar        *gowl_macro_get_arg             (GowlMacroContext *ctx,
                                                    guint             index);
const gchar * const *gowl_macro_get_argv           (GowlMacroContext *ctx);
GowlMacroTrigger    gowl_macro_get_trigger         (GowlMacroContext *ctx);
const gchar        *gowl_macro_get_trigger_detail  (GowlMacroContext *ctx);
gboolean            gowl_macro_is_threaded         (GowlMacroContext *ctx);

/* --- control --- */
gboolean            gowl_macro_set_timeout         (GowlMacroContext *ctx,
                                                    guint             timeout_ms);
gboolean            gowl_macro_is_cancelled        (GowlMacroContext *ctx);
gboolean            gowl_macro_sleep               (GowlMacroContext *ctx,
                                                    guint             ms);
void                gowl_macro_log                 (GowlMacroContext *ctx,
                                                    const gchar      *format,
                                                    ...) G_GNUC_PRINTF(2, 3);
void                gowl_macro_notify              (GowlMacroContext *ctx,
                                                    const gchar      *summary,
                                                    const gchar      *body);
void                gowl_macro_set_result          (GowlMacroContext *ctx,
                                                    const gchar      *text);
gboolean            gowl_macro_call_on_compositor  (GowlMacroContext        *ctx,
                                                    GowlMacroCompositorFunc  func,
                                                    gpointer                 user_data);

/* --- steps: queued (default) or immediate (threaded), in order --- */
gboolean            gowl_macro_key                 (GowlMacroContext *ctx,
                                                    GowlClient       *target,
                                                    const gchar      *combo);
gboolean            gowl_macro_key_code            (GowlMacroContext *ctx,
                                                    GowlClient       *target,
                                                    guint32           keycode,
                                                    guint32           modifiers);
gboolean            gowl_macro_text                (GowlMacroContext *ctx,
                                                    GowlClient       *target,
                                                    const gchar      *text);
gboolean            gowl_macro_button              (GowlMacroContext *ctx,
                                                    guint32           button);
gboolean            gowl_macro_wait                (GowlMacroContext *ctx,
                                                    guint             ms);
gboolean            gowl_macro_command             (GowlMacroContext *ctx,
                                                    const gchar      *line);
gboolean            gowl_macro_action              (GowlMacroContext *ctx,
                                                    GowlAction        action,
                                                    const gchar      *arg);
gboolean            gowl_macro_focus               (GowlMacroContext *ctx,
                                                    GowlClient       *client);

/* --- compositor helpers (compositor thread; see the threaded note) --- */
GowlClient         *gowl_macro_find_client         (GowlMacroContext *ctx,
                                                    const gchar      *pattern);
GList              *gowl_macro_list_clients        (GowlMacroContext *ctx,
                                                    gboolean          visible_only);
void                gowl_macro_sort_clients        (GowlMacroContext *ctx,
                                                    GList            *clients,
                                                    GCompareFunc      compare);
gchar              *gowl_macro_run_command         (GowlMacroContext *ctx,
                                                    const gchar      *line);
void                gowl_macro_move_client         (GowlMacroContext *ctx,
                                                    GowlClient       *client,
                                                    GowlMonitor      *monitor,
                                                    guint32           tags);

/* --- C macros and running by name --- */
void                gowl_macro_register_func       (const gchar      *name,
                                                    GowlMacroFunc     func,
                                                    gpointer          user_data,
                                                    GDestroyNotify    destroy);
gboolean            gowl_macro_unregister_func     (const gchar      *name);
gchar              *gowl_macro_run_by_name         (GowlCompositor   *compositor,
                                                    const gchar      *name,
                                                    const gchar * const *argv);

G_END_DECLS

#endif /* GOWL_MACRO_H */
