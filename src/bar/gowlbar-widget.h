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

#ifndef GOWLBAR_WIDGET_H
#define GOWLBAR_WIDGET_H

#include <glib-object.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>

G_BEGIN_DECLS

#define GOWLBAR_TYPE_WIDGET (gowlbar_widget_get_type())

G_DECLARE_DERIVABLE_TYPE(GowlbarWidget, gowlbar_widget, GOWLBAR, WIDGET, GObject)

/**
 * GowlbarWidgetClass:
 * @render: Render widget content at given position and size.
 * @get_width: Return desired width in pixels, or -1 to fill available space.
 * @on_click: Handle a click event. Return %TRUE if consumed.
 * @update: Called when compositor state changes to update widget data.
 *
 * Virtual function table for #GowlbarWidget subclasses.
 */
struct _GowlbarWidgetClass {
	GObjectClass parent_class;

	void     (*render)    (GowlbarWidget *self, cairo_t *cr,
	                       PangoLayout *layout,
	                       gint x, gint y, gint width, gint height);
	gint     (*get_width) (GowlbarWidget *self, cairo_t *cr,
	                       PangoLayout *layout);
	gboolean (*on_click)  (GowlbarWidget *self, gint x, gint y,
	                       guint button);
	void     (*update)    (GowlbarWidget *self);

	/* padding for future expansion */
	gpointer _reserved[8];
};

/**
 * gowlbar_widget_render:
 * @self: the widget
 * @cr: the cairo context to draw into
 * @layout: a shared PangoLayout for text rendering
 * @x: x position
 * @y: y position
 * @width: available width
 * @height: available height
 *
 * Renders the widget content at the given position and size.
 */
void gowlbar_widget_render(GowlbarWidget *self, cairo_t *cr,
                            PangoLayout *layout,
                            gint x, gint y, gint width, gint height);

/**
 * gowlbar_widget_get_width:
 * @self: the widget
 * @cr: the cairo context (for text measurement)
 * @layout: a shared PangoLayout for text measurement
 *
 * Returns: the desired width in pixels, or -1 to expand and fill
 */
gint gowlbar_widget_get_width(GowlbarWidget *self, cairo_t *cr,
                               PangoLayout *layout);

/**
 * gowlbar_widget_on_click:
 * @self: the widget
 * @x: click x coordinate (relative to widget origin)
 * @y: click y coordinate (relative to widget origin)
 * @button: mouse button number
 *
 * Returns: %TRUE if the click was consumed
 */
gboolean gowlbar_widget_on_click(GowlbarWidget *self, gint x, gint y,
                                  guint button);

/**
 * gowlbar_widget_update:
 * @self: the widget
 *
 * Called when compositor state changes to update widget data.
 */
void gowlbar_widget_update(GowlbarWidget *self);

G_END_DECLS

#endif /* GOWLBAR_WIDGET_H */
