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

#ifndef GOWLBAR_TITLE_WIDGET_H
#define GOWLBAR_TITLE_WIDGET_H

#include "gowlbar-widget.h"
#include "gowlbar-config.h"

G_BEGIN_DECLS

#define GOWLBAR_TYPE_TITLE_WIDGET (gowlbar_title_widget_get_type())

G_DECLARE_FINAL_TYPE(GowlbarTitleWidget, gowlbar_title_widget,
                     GOWLBAR, TITLE_WIDGET, GowlbarWidget)

/**
 * gowlbar_title_widget_new:
 * @config: (transfer none): the bar configuration for colours
 *
 * Creates a new window title display widget.  The title widget
 * expands to fill available space (get_width returns -1).
 *
 * Returns: (transfer full): a new #GowlbarTitleWidget
 */
GowlbarTitleWidget *gowlbar_title_widget_new(GowlbarConfig *config);

/**
 * gowlbar_title_widget_set_title:
 * @self: the title widget
 * @title: (nullable): the focused window title, or %NULL to clear
 *
 * Updates the displayed window title text.
 */
void gowlbar_title_widget_set_title(GowlbarTitleWidget *self,
                                     const gchar *title);

G_END_DECLS

#endif /* GOWLBAR_TITLE_WIDGET_H */
