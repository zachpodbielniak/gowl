/*
 * gowl-config-compiler.h - C configuration compiler
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
 * must export a `gowl_config_init` symbol.
 *
 * Search path for config.c:
 *  1. --c-config PATH (explicit override)
 *  2. $XDG_CONFIG_HOME/gowl/config.c (~/.config/gowl/config.c)
 *  3. SYSCONFDIR/gowl/config.c (/etc/gowl/config.c)
 *  4. DATADIR/gowl/config.c (/usr/share/gowl/config.c)
 *  5. ./data/config.c (development fallback)
 */

#ifndef GOWL_CONFIG_COMPILER_H
#define GOWL_CONFIG_COMPILER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_CONFIG_COMPILER (gowl_config_compiler_get_type())

G_DECLARE_FINAL_TYPE(GowlConfigCompiler, gowl_config_compiler,
                     GOWL, CONFIG_COMPILER, GObject)

/**
 * gowl_config_compiler_new:
 * @error: (nullable): return location for a #GError
 *
 * Creates a new #GowlConfigCompiler backed by the crispy library.
 * Probes gcc for its version and caches pkg-config output for
 * the default GLib/GObject/GIO libraries.  Uses SHA256 content-
 * hash caching in $XDG_CACHE_HOME/gowl.
 *
 * Returns: (transfer full) (nullable): a new #GowlConfigCompiler,
 *          or %NULL if gcc is not found
 */
GowlConfigCompiler *
gowl_config_compiler_new(GError **error);

/**
 * gowl_config_compiler_find_config:
 * @self: a #GowlConfigCompiler
 *
 * Searches standard paths for a C config file.
 *
 * Search order:
 *  1. $XDG_CONFIG_HOME/gowl/config.c
 *  2. SYSCONFDIR/gowl/config.c
 *  3. DATADIR/gowl/config.c
 *  4. ./data/config.c (development fallback)
 *
 * Returns: (transfer full) (nullable): path to the config.c,
 *          or %NULL if none found.  Free with g_free().
 */
gchar *
gowl_config_compiler_find_config(GowlConfigCompiler *self);

/**
 * gowl_config_compiler_compile:
 * @self: a #GowlConfigCompiler
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
gowl_config_compiler_compile(
    GowlConfigCompiler  *self,
    const gchar         *source_path,
    gboolean             force,
    GError             **error
);

/**
 * gowl_config_compiler_load_and_apply:
 * @self: a #GowlConfigCompiler
 * @so_path: path to the compiled shared object
 * @error: (nullable): return location for a #GError
 *
 * Opens the .so, looks up `gowl_config_init`, and calls it.
 * The init function must have signature:
 *   G_MODULE_EXPORT gboolean gowl_config_init(void);
 *
 * Returns: %TRUE if loaded and applied successfully
 */
gboolean
gowl_config_compiler_load_and_apply(
    GowlConfigCompiler  *self,
    const gchar         *so_path,
    GError             **error
);

/**
 * gowl_config_compiler_dispatch_ready:
 * @self: (nullable): a #GowlConfigCompiler
 *
 * Looks up the optional `gowl_config_ready` symbol from the loaded
 * C config shared object and calls it if present.  This should be
 * called after the compositor is fully started and the Wayland
 * display is ready to accept clients.
 *
 * Use this for spawning status bars, notification daemons, or other
 * Wayland clients that require a running compositor.
 *
 * If @self is %NULL, no C config was loaded, or the symbol is absent,
 * this is a no-op.
 */
void
gowl_config_compiler_dispatch_ready(GowlConfigCompiler *self);

G_END_DECLS

#endif /* GOWL_CONFIG_COMPILER_H */
