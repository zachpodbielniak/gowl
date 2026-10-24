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
 * gowl-hotkey-private.h - What the hotkey module needs from libgowl's
 * registries and nobody else does: making calls, reading the sources,
 * and the counter that says something changed.
 */

#ifndef GOWL_HOTKEY_PRIVATE_H
#define GOWL_HOTKEY_PRIVATE_H

#include "hotkey/gowl-hotkey.h"

G_BEGIN_DECLS

/* A call's facts.  Strings are copied; @focused is not referenced (a
   call lives for one dispatch on the compositor thread). */
GowlHotkeyCall *gowl_hotkey_call_new          (GowlCompositor       *compositor,
                                               gpointer              focused,
                                               const gchar          *id,
                                               const gchar          *trigger,
                                               const gchar          *detail,
                                               const gchar * const  *args);
gchar          *gowl_hotkey_call_steal_result (GowlHotkeyCall       *call);
void            gowl_hotkey_call_free         (GowlHotkeyCall       *call);

/* Runs the function registered as @name under the fault guard.  FALSE
   with @error for an unknown name or a fault; @signo (nullable) says
   which signal a fault was. */
gboolean        gowl_hotkey_invoke_func       (const gchar          *name,
                                               GowlHotkeyCall       *call,
                                               guint                 timeout_ms,
                                               GError              **error);
gboolean        gowl_hotkey_has_func          (const gchar          *name);

/* A named context: 1 true, 0 false, -1 when nothing of that name is
   registered or set.  A predicate that faults is 0, with @error set. */
gint            gowl_hotkey_eval_context      (const gchar          *name,
                                               GowlHotkeyCall       *call,
                                               guint                 timeout_ms,
                                               GError              **error);

/* The process-wide `hotkey:' sections, in the order they were added. */
typedef struct {
	gchar *id;
	gchar *yaml;
} GowlHotkeySource;

GPtrArray      *gowl_hotkey_dup_sources       (void);
void            gowl_hotkey_source_free       (GowlHotkeySource     *source);

/* Moves whenever a function, a context predicate or a source is added
   or removed -- not when a boolean context changes value, which is
   read live. */
guint           gowl_hotkey_get_generation    (void);

G_END_DECLS

#endif /* GOWL_HOTKEY_PRIVATE_H */
