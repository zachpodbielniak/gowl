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

#ifndef GOWL_MODULE_MANAGER_H
#define GOWL_MODULE_MANAGER_H

#include <glib-object.h>
#include <gmodule.h>

#include "gowl-module.h"
#include "gowl-module-info.h"

G_BEGIN_DECLS

#define GOWL_TYPE_MODULE_MANAGER (gowl_module_manager_get_type())

G_DECLARE_FINAL_TYPE(GowlModuleManager, gowl_module_manager, GOWL, MODULE_MANAGER, GObject)

/**
 * GowlModuleManager:
 *
 * Manages the lifecycle of compositor modules.  It handles loading
 * shared-object plugins, registering in-process module types,
 * activating/deactivating modules, and dispatching events to the
 * appropriate interface-based handler arrays sorted by priority.
 */

GowlModuleManager *gowl_module_manager_new              (void);

gboolean            gowl_module_manager_load_module      (GowlModuleManager *self,
                                                           const gchar       *path,
                                                           GError           **error);

gboolean            gowl_module_manager_register         (GowlModuleManager *self,
                                                           GType              module_type,
                                                           GError           **error);

void                gowl_module_manager_activate_all     (GowlModuleManager *self);

void                gowl_module_manager_deactivate_all   (GowlModuleManager *self);

GList              *gowl_module_manager_get_modules      (GowlModuleManager *self);

void                gowl_module_manager_load_from_directory(GowlModuleManager *self,
                                                             const gchar       *dir_path);

gboolean            gowl_module_manager_dispatch_key     (GowlModuleManager *self,
                                                           guint              modifiers,
                                                           guint              keysym,
                                                           gboolean           pressed);

gboolean            gowl_module_manager_dispatch_button  (GowlModuleManager *self,
                                                           guint              button,
                                                           guint              state,
                                                           guint              modifiers);

void                gowl_module_manager_dispatch_startup (GowlModuleManager *self,
                                                           gpointer           compositor);

void                gowl_module_manager_dispatch_shutdown(GowlModuleManager *self,
                                                           gpointer           compositor);

G_END_DECLS

#endif /* GOWL_MODULE_MANAGER_H */
