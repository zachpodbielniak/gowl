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

#ifndef GOWL_MODULE_INFO_H
#define GOWL_MODULE_INFO_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_MODULE_INFO (gowl_module_info_get_type())

typedef struct _GowlModuleInfo GowlModuleInfo;

/**
 * GowlModuleInfo:
 * @name: the human-readable name of the module
 * @description: a short description of what the module does
 * @version: the version string of the module (e.g. "0.1.0")
 *
 * A boxed type that holds metadata about a loaded compositor module.
 * This is returned by the module manager to allow introspection of
 * registered modules without exposing the module instances directly.
 */
struct _GowlModuleInfo {
	gchar *name;
	gchar *description;
	gchar *version;
};

GType          gowl_module_info_get_type        (void) G_GNUC_CONST;

GowlModuleInfo *gowl_module_info_new            (const gchar *name,
                                                  const gchar *description,
                                                  const gchar *version);

GowlModuleInfo *gowl_module_info_copy           (const GowlModuleInfo *info);

void            gowl_module_info_free           (GowlModuleInfo *info);

const gchar    *gowl_module_info_get_name       (const GowlModuleInfo *info);

const gchar    *gowl_module_info_get_description(const GowlModuleInfo *info);

const gchar    *gowl_module_info_get_version    (const GowlModuleInfo *info);

G_END_DECLS

#endif /* GOWL_MODULE_INFO_H */
