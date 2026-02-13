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

#ifndef GOWLBAR_CONFIG_COMPILER_H
#define GOWLBAR_CONFIG_COMPILER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWLBAR_TYPE_CONFIG_COMPILER (gowlbar_config_compiler_get_type())

G_DECLARE_FINAL_TYPE(GowlbarConfigCompiler, gowlbar_config_compiler,
                     GOWLBAR, CONFIG_COMPILER, GObject)

/**
 * gowlbar_config_compiler_new:
 *
 * Creates a new #GowlbarConfigCompiler.
 *
 * Returns: (transfer full): a newly allocated #GowlbarConfigCompiler
 */
GowlbarConfigCompiler *gowlbar_config_compiler_new(void);

/**
 * gowlbar_config_compiler_compile:
 * @self: a #GowlbarConfigCompiler
 * @source_path: path to the C configuration source file
 * @output_path: path where the compiled .so should be written
 * @error: (nullable): return location for a #GError
 *
 * Compiles the C config source into a shared object.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gowlbar_config_compiler_compile(GowlbarConfigCompiler  *self,
                                          const gchar            *source_path,
                                          const gchar            *output_path,
                                          GError                **error);

/**
 * gowlbar_config_compiler_get_cache_path:
 * @self: a #GowlbarConfigCompiler
 *
 * Returns the default path for the compiled bar config shared object.
 *
 * Returns: (transfer full): a newly allocated path string; free with g_free()
 */
gchar *gowlbar_config_compiler_get_cache_path(GowlbarConfigCompiler *self);

/**
 * gowlbar_config_compiler_load_and_apply:
 * @self: a #GowlbarConfigCompiler
 * @so_path: path to the compiled shared object
 * @error: (nullable): return location for a #GError
 *
 * Opens the shared object, looks up `gowlbar_config_init`, and calls it.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gowlbar_config_compiler_load_and_apply(GowlbarConfigCompiler  *self,
                                                  const gchar            *so_path,
                                                  GError                **error);

G_END_DECLS

#endif /* GOWLBAR_CONFIG_COMPILER_H */
