/*
 * gowlbar-config-compiler.h - C configuration compiler for gowlbar
 *
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
 *
 * Compiles a user-written C configuration file into a shared object
 * and loads it at runtime.  Uses the crispy library for compilation
 * and content-hash caching (SHA256).  The config source may define
 * CRISPY_PARAMS to pass extra compiler flags.  The compiled .so
 * must export a `gowlbar_config_init` symbol.
 *
 * Search path for bar.c:
 *  1. --c-config PATH (explicit override)
 *  2. $XDG_CONFIG_HOME/gowl/bar.c (~/.config/gowl/bar.c)
 *  3. SYSCONFDIR/gowl/bar.c (/etc/gowl/bar.c)
 *  4. DATADIR/gowl/bar.c (/usr/share/gowl/bar.c)
 *  5. ./data/bar.c (development fallback)
 */

#ifndef GOWLBAR_CONFIG_COMPILER_H
#define GOWLBAR_CONFIG_COMPILER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWLBAR_TYPE_CONFIG_COMPILER (gowlbar_config_compiler_get_type())

G_DECLARE_FINAL_TYPE(GowlbarConfigCompiler, gowlbar_config_compiler,
                     GOWLBAR, CONFIG_COMPILER, GObject)

/**
 * gowlbar_config_compiler_new:
 * @error: (nullable): return location for a #GError
 *
 * Creates a new #GowlbarConfigCompiler backed by the crispy library.
 * Probes gcc for its version and caches pkg-config output.
 * Uses SHA256 content-hash caching in $XDG_CACHE_HOME/gowl.
 *
 * Returns: (transfer full) (nullable): a new #GowlbarConfigCompiler,
 *          or %NULL if gcc is not found
 */
GowlbarConfigCompiler *
gowlbar_config_compiler_new(GError **error);

/**
 * gowlbar_config_compiler_find_config:
 * @self: a #GowlbarConfigCompiler
 *
 * Searches standard paths for a bar C config file.
 *
 * Search order:
 *  1. $XDG_CONFIG_HOME/gowl/bar.c
 *  2. SYSCONFDIR/gowl/bar.c
 *  3. DATADIR/gowl/bar.c
 *  4. ./data/bar.c (development fallback)
 *
 * Returns: (transfer full) (nullable): path to the bar.c,
 *          or %NULL if none found.  Free with g_free().
 */
gchar *
gowlbar_config_compiler_find_config(GowlbarConfigCompiler *self);

/**
 * gowlbar_config_compiler_compile:
 * @self: a #GowlbarConfigCompiler
 * @source_path: path to the C configuration source file
 * @force: if %TRUE, bypass cache and force recompilation
 * @error: (nullable): return location for a #GError
 *
 * Reads the source file, scans for an optional CRISPY_PARAMS
 * define, computes a SHA256 content hash, and compiles to a
 * shared object if no valid cached artifact exists (or if
 * @force is %TRUE).
 *
 * Returns: (transfer full) (nullable): path to the compiled .so,
 *          or %NULL on error.  Free with g_free().
 */
gchar *
gowlbar_config_compiler_compile(
    GowlbarConfigCompiler  *self,
    const gchar            *source_path,
    gboolean                force,
    GError                **error
);

/**
 * gowlbar_config_compiler_load_and_apply:
 * @self: a #GowlbarConfigCompiler
 * @so_path: path to the compiled shared object
 * @error: (nullable): return location for a #GError
 *
 * Opens the .so, looks up `gowlbar_config_init`, and calls it.
 * The init function must have signature:
 *   G_MODULE_EXPORT gboolean gowlbar_config_init(void);
 *
 * Returns: %TRUE if loaded and applied successfully
 */
gboolean
gowlbar_config_compiler_load_and_apply(
    GowlbarConfigCompiler  *self,
    const gchar            *so_path,
    GError                **error
);

G_END_DECLS

#endif /* GOWLBAR_CONFIG_COMPILER_H */
