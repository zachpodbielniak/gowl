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

#ifndef GOWLBAR_APP_H
#define GOWLBAR_APP_H

#include <glib-object.h>
#include "gowlbar-config.h"
#include "gowlbar-ipc.h"

G_BEGIN_DECLS

#define GOWLBAR_TYPE_APP (gowlbar_app_get_type())

G_DECLARE_FINAL_TYPE(GowlbarApp, gowlbar_app, GOWLBAR, APP, GObject)

/**
 * gowlbar_app_new:
 *
 * Creates a new bar application instance.
 *
 * Returns: (transfer full): a new #GowlbarApp
 */
GowlbarApp *gowlbar_app_new(void);

/**
 * gowlbar_app_set_config:
 * @self: the bar application
 * @config: (transfer none): the bar configuration to apply
 *
 * Sets the configuration object used by the bar application.
 * Must be called before gowlbar_app_run().
 */
void gowlbar_app_set_config(GowlbarApp *self, GowlbarConfig *config);

/**
 * gowlbar_app_get_config:
 * @self: the bar application
 *
 * Returns: (transfer none) (nullable): the current bar configuration
 */
GowlbarConfig *gowlbar_app_get_config(GowlbarApp *self);

/**
 * gowlbar_app_run:
 * @self: the bar application
 * @error: (nullable): return location for a #GError
 *
 * Connects to the Wayland display, binds globals, creates
 * per-output bar surfaces, and enters the main loop.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gowlbar_app_run(GowlbarApp *self, GError **error);

/**
 * gowlbar_app_quit:
 * @self: the bar application
 *
 * Requests the bar to exit its main loop.
 */
void gowlbar_app_quit(GowlbarApp *self);

G_END_DECLS

#endif /* GOWLBAR_APP_H */
