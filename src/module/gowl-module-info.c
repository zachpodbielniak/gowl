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

#include "gowl-module-info.h"

G_DEFINE_BOXED_TYPE(GowlModuleInfo, gowl_module_info,
                    gowl_module_info_copy, gowl_module_info_free)

/**
 * gowl_module_info_new:
 * @name: (nullable): the module name
 * @description: (nullable): the module description
 * @version: (nullable): the module version string
 *
 * Creates a new #GowlModuleInfo with the given metadata.
 * All string arguments are copied internally via g_strdup().
 *
 * Returns: (transfer full): a newly allocated #GowlModuleInfo
 */
GowlModuleInfo *
gowl_module_info_new(
	const gchar *name,
	const gchar *description,
	const gchar *version
){
	GowlModuleInfo *info;

	info = g_slice_new0(GowlModuleInfo);
	info->name = g_strdup(name);
	info->description = g_strdup(description);
	info->version = g_strdup(version);

	return info;
}

/**
 * gowl_module_info_copy:
 * @info: a #GowlModuleInfo to copy
 *
 * Creates a deep copy of @info, duplicating all internal strings.
 *
 * Returns: (transfer full): a newly allocated copy of @info
 */
GowlModuleInfo *
gowl_module_info_copy(const GowlModuleInfo *info)
{
	g_return_val_if_fail(info != NULL, NULL);

	return gowl_module_info_new(info->name, info->description, info->version);
}

/**
 * gowl_module_info_free:
 * @info: a #GowlModuleInfo to free
 *
 * Frees a #GowlModuleInfo and all its internal strings.
 */
void
gowl_module_info_free(GowlModuleInfo *info)
{
	if (info == NULL)
		return;

	g_free(info->name);
	g_free(info->description);
	g_free(info->version);
	g_slice_free(GowlModuleInfo, info);
}

/**
 * gowl_module_info_get_name:
 * @info: a #GowlModuleInfo
 *
 * Returns the module name.
 *
 * Returns: (transfer none) (nullable): the module name string
 */
const gchar *
gowl_module_info_get_name(const GowlModuleInfo *info)
{
	g_return_val_if_fail(info != NULL, NULL);

	return info->name;
}

/**
 * gowl_module_info_get_description:
 * @info: a #GowlModuleInfo
 *
 * Returns the module description.
 *
 * Returns: (transfer none) (nullable): the module description string
 */
const gchar *
gowl_module_info_get_description(const GowlModuleInfo *info)
{
	g_return_val_if_fail(info != NULL, NULL);

	return info->description;
}

/**
 * gowl_module_info_get_version:
 * @info: a #GowlModuleInfo
 *
 * Returns the module version string.
 *
 * Returns: (transfer none) (nullable): the module version string
 */
const gchar *
gowl_module_info_get_version(const GowlModuleInfo *info)
{
	g_return_val_if_fail(info != NULL, NULL);

	return info->version;
}
