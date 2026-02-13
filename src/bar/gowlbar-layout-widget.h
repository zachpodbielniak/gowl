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

#ifndef GOWLBAR_LAYOUT_WIDGET_H
#define GOWLBAR_LAYOUT_WIDGET_H

#include "gowlbar-widget.h"
#include "gowlbar-config.h"

G_BEGIN_DECLS

#define GOWLBAR_TYPE_LAYOUT_WIDGET (gowlbar_layout_widget_get_type())

G_DECLARE_FINAL_TYPE(GowlbarLayoutWidget, gowlbar_layout_widget,
                     GOWLBAR, LAYOUT_WIDGET, GowlbarWidget)

/**
 * gowlbar_layout_widget_new:
 * @config: (transfer none): the bar configuration for colours
 *
 * Creates a new layout name display widget.
 *
 * Returns: (transfer full): a new #GowlbarLayoutWidget
 */
GowlbarLayoutWidget *gowlbar_layout_widget_new(GowlbarConfig *config);

/**
 * gowlbar_layout_widget_set_layout:
 * @self: the layout widget
 * @layout_name: the current layout symbol (e.g. "[]=", "[M]", "><>")
 *
 * Updates the displayed layout name.
 */
void gowlbar_layout_widget_set_layout(GowlbarLayoutWidget *self,
                                       const gchar *layout_name);

G_END_DECLS

#endif /* GOWLBAR_LAYOUT_WIDGET_H */
