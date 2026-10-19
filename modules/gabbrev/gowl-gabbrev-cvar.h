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
 * gowl-gabbrev-cvar.h - `type: c' variables written in the YAML.
 *
 * A `c' variable is C, compiled with crispy the first time it is used
 * and cached by the hash of its source, like a macro file:
 *
 *   params: {code: "return g_strdup_printf(\"%d\", 6 * 7);"}
 *       a function BODY; it gets `GowlGabbrevRequest *req' and the
 *       shorthands PARAM("key") and VAR("name"), and returns a new
 *       string (or NULL for an error)
 *   params: {source: "...a whole file..."}
 *       a translation unit exporting
 *       `gchar *gowl_gabbrev_var(GowlGabbrevRequest *req)'
 *   params: {file: "~/snips/weather.c"}
 *       the same, from a file
 *   params: {func: "name"}
 *       a function registered with gowl_gabbrev_register_func()
 *       (no compiler involved; see the module)
 *
 * BUILDING is the slow half (gcc) and runs on a worker thread; OPENING
 * and CALLING run on the compositor thread, under the fault guard.
 */

#ifndef GOWL_GABBREV_CVAR_H
#define GOWL_GABBREV_CVAR_H

#include <glib.h>

#include "gabbrev/gowl-gabbrev.h"

G_BEGIN_DECLS

#define GABBREV_CVAR_SYMBOL "gowl_gabbrev_var"

typedef gchar *(*GabbrevCvarFunc) (GowlGabbrevRequest *request);

/* What to build: exactly one of the three is set. */
typedef struct {
	gchar *code;
	gchar *source;
	gchar *file;
	gchar *cflags;      /* params.cflags, shell-expanded like CRISPY_PARAMS */
} GabbrevCvarSpec;

void      gabbrev_cvar_spec_clear   (GabbrevCvarSpec        *spec);
gboolean  gabbrev_cvar_spec_from_params (GabbrevCvarSpec    *spec,
                                     GHashTable             *params,
                                     GError                **error);
gchar    *gabbrev_cvar_spec_key     (const GabbrevCvarSpec  *spec);
gchar    *gabbrev_cvar_full_source  (const GabbrevCvarSpec  *spec,
                                     GError                **error);

/* Worker thread: compile (or find the cached build); the .so's path */
gchar    *gabbrev_cvar_build        (const GabbrevCvarSpec  *spec,
                                     const gchar            *cache_dir,
                                     GError                **error);

/* Compositor thread: open a build under the fault guard */
GabbrevCvarFunc gabbrev_cvar_open   (const gchar            *so_path,
                                     GError                **error);

/* Compositor thread: call one under the fault guard and @timeout_ms */
gchar    *gabbrev_cvar_call         (GabbrevCvarFunc         func,
                                     GowlGabbrevRequest     *request,
                                     guint                   timeout_ms,
                                     GError                **error);
gchar    *gabbrev_cvar_call_registered (const gchar         *name,
                                     GowlGabbrevRequest     *request,
                                     guint                   timeout_ms,
                                     GError                **error);

G_END_DECLS

#endif /* GOWL_GABBREV_CVAR_H */
