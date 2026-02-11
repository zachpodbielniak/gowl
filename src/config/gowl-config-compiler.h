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

#ifndef GOWL_CONFIG_COMPILER_H
#define GOWL_CONFIG_COMPILER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_CONFIG_COMPILER (gowl_config_compiler_get_type())

G_DECLARE_FINAL_TYPE(GowlConfigCompiler, gowl_config_compiler, GOWL, CONFIG_COMPILER, GObject)

GowlConfigCompiler *gowl_config_compiler_new             (void);

gboolean            gowl_config_compiler_compile          (GowlConfigCompiler  *self,
                                                           const gchar         *source_path,
                                                           const gchar         *output_path,
                                                           GError             **error);

gchar              *gowl_config_compiler_get_cache_path   (GowlConfigCompiler  *self);

gboolean            gowl_config_compiler_load_and_apply   (GowlConfigCompiler  *self,
                                                           const gchar         *so_path,
                                                           GError             **error);

G_END_DECLS

#endif /* GOWL_CONFIG_COMPILER_H */
