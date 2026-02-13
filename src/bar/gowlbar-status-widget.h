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

#ifndef GOWLBAR_STATUS_WIDGET_H
#define GOWLBAR_STATUS_WIDGET_H

#include "gowlbar-widget.h"
#include "gowlbar-config.h"

G_BEGIN_DECLS

#define GOWLBAR_TYPE_STATUS_WIDGET (gowlbar_status_widget_get_type())

G_DECLARE_FINAL_TYPE(GowlbarStatusWidget, gowlbar_status_widget,
                     GOWLBAR, STATUS_WIDGET, GowlbarWidget)

/**
 * gowlbar_status_widget_new:
 * @config: (transfer none): the bar configuration for colours
 *
 * Creates a new status text display widget.  Status text is
 * typically read from stdin (line-buffered) or set via IPC.
 *
 * Returns: (transfer full): a new #GowlbarStatusWidget
 */
GowlbarStatusWidget *gowlbar_status_widget_new(GowlbarConfig *config);

/**
 * gowlbar_status_widget_set_text:
 * @self: the status widget
 * @text: (nullable): the status text to display, or %NULL to clear
 *
 * Updates the displayed status text.  Each call replaces the
 * previous text entirely.
 */
void gowlbar_status_widget_set_text(GowlbarStatusWidget *self,
                                     const gchar *text);

G_END_DECLS

#endif /* GOWLBAR_STATUS_WIDGET_H */
