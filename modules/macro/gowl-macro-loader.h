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
 * gowl-macro-loader.h - Finding, compiling and opening macro files.
 *
 * A bare name is looked up through the macro search path, first hit
 * wins, trying NAME, NAME.c and NAME.so in each directory; a name with a
 * `/' in it is a path and is used as-is.  A `.c' file is compiled with
 * crispy into a cache keyed by the source's content, so editing a macro
 * and running it again runs the new code.  Every open happens under the
 * fault guard: a macro's constructors are its code too.
 */

#ifndef GOWL_MACRO_LOADER_H
#define GOWL_MACRO_LOADER_H

#include <glib.h>
#include <gmodule.h>

#include "macro/gowl-macro.h"

G_BEGIN_DECLS

#define GOWL_MACRO_LOADER_ERROR (gowl_macro_loader_error_quark())

typedef enum {
	GOWL_MACRO_LOADER_ERROR_NOT_FOUND,
	GOWL_MACRO_LOADER_ERROR_COMPILE,
	GOWL_MACRO_LOADER_ERROR_LOAD,
	GOWL_MACRO_LOADER_ERROR_ABI,
	GOWL_MACRO_LOADER_ERROR_FAULT
} GowlMacroLoaderError;

GQuark gowl_macro_loader_error_quark (void);

/* What a macro file exports */
typedef gboolean (*GowlMacroScriptRun) (GowlMacroContext *ctx);

typedef struct {
	gchar              *name;       /* normalized: basename minus .c/.so */
	gchar              *path;       /* the source or .so it came from */
	gchar              *so_path;    /* what was opened */
	gchar              *hash;       /* content hash of the source */
	GowlMacroScriptRun  run;
	gchar              *description;
	gboolean            threaded;
	guint               timeout_ms; /* 0 = none, when has_timeout */
	gboolean            has_timeout;/* FALSE = the module's default */
} GowlMacroScript;

typedef struct _GowlMacroLoader GowlMacroLoader;

GowlMacroLoader       *gowl_macro_loader_new             (const gchar      *cache_dir);
void                   gowl_macro_loader_free            (GowlMacroLoader  *self);
void                   gowl_macro_loader_set_dirs        (GowlMacroLoader  *self,
                                                          const gchar * const *dirs);
const gchar * const   *gowl_macro_loader_get_dirs        (GowlMacroLoader  *self);
GStrv                  gowl_macro_loader_default_dirs    (const gchar      *configured);
gchar                 *gowl_macro_loader_normalize_name  (const gchar      *spec);
gchar                 *gowl_macro_loader_resolve         (GowlMacroLoader  *self,
                                                          const gchar      *spec,
                                                          GError          **error);
GPtrArray             *gowl_macro_loader_discover        (GowlMacroLoader  *self);
const GowlMacroScript *gowl_macro_loader_load            (GowlMacroLoader  *self,
                                                          const gchar      *spec,
                                                          GError          **error);
gboolean               gowl_macro_loader_compile_only    (GowlMacroLoader  *self,
                                                          const gchar      *spec,
                                                          GError          **error);
void                   gowl_macro_loader_forget          (GowlMacroLoader  *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlMacroLoader, gowl_macro_loader_free)

G_END_DECLS

#endif /* GOWL_MACRO_LOADER_H */
