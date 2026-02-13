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

#include "gowlbar-title-widget.h"

#include <string.h>
#include <stdlib.h>

/**
 * GowlbarTitleWidget:
 *
 * Renders the focused window title in the bar.  This widget
 * expands to fill all available space (get_width returns -1).
 * Long titles are truncated with an ellipsis.
 */
struct _GowlbarTitleWidget {
	GowlbarWidget  parent_instance;

	GowlbarConfig *config;  /* borrowed ref */
	gchar         *title;   /* current window title */
};

G_DEFINE_FINAL_TYPE(GowlbarTitleWidget, gowlbar_title_widget,
                    GOWLBAR_TYPE_WIDGET)

/* --- Colour parsing helper --- */

static void
parse_hex_color(const gchar *hex, gdouble *r, gdouble *g, gdouble *b)
{
	guint32 val;

	if (hex == NULL || hex[0] != '#' || strlen(hex) < 7) {
		*r = *g = *b = 0.5;
		return;
	}

	val = (guint32)strtoul(hex + 1, NULL, 16);
	*r = ((val >> 16) & 0xFF) / 255.0;
	*g = ((val >>  8) & 0xFF) / 255.0;
	*b = ((val      ) & 0xFF) / 255.0;
}

/* --- Widget vfunc implementations --- */

/**
 * title_widget_render:
 *
 * Renders the window title text, truncated with ellipsis if it
 * exceeds the available width.  Text is vertically centred.
 */
static void
title_widget_render(
	GowlbarWidget *widget,
	cairo_t       *cr,
	PangoLayout   *layout,
	gint           x,
	gint           y,
	gint           width,
	gint           height
){
	GowlbarTitleWidget *self;
	gdouble fg_r, fg_g, fg_b;
	gint text_h;
	gint text_y;

	self = GOWLBAR_TITLE_WIDGET(widget);

	if (self->title == NULL || self->title[0] == '\0')
		return;

	parse_hex_color(
		gowlbar_config_get_title_fg(self->config),
		&fg_r, &fg_g, &fg_b);

	/* Set text with ellipsis truncation */
	pango_layout_set_text(layout, self->title, -1);
	pango_layout_set_width(layout, width * PANGO_SCALE);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

	pango_layout_get_pixel_size(layout, NULL, &text_h);
	text_y = y + (height - text_h) / 2;

	cairo_set_source_rgb(cr, fg_r, fg_g, fg_b);
	cairo_move_to(cr, x, text_y);
	pango_cairo_show_layout(cr, layout);

	/* Reset ellipsize and width so other widgets are not affected */
	pango_layout_set_width(layout, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
}

/**
 * title_widget_get_width:
 *
 * Returns -1 to indicate this widget should expand to fill all
 * remaining space in the bar.
 */
static gint
title_widget_get_width(
	GowlbarWidget *widget,
	cairo_t       *cr,
	PangoLayout   *layout
){
	(void)widget;
	(void)cr;
	(void)layout;

	return -1;
}

/* --- GObject lifecycle --- */

static void
gowlbar_title_widget_finalize(GObject *object)
{
	GowlbarTitleWidget *self;

	self = GOWLBAR_TITLE_WIDGET(object);

	g_free(self->title);

	G_OBJECT_CLASS(gowlbar_title_widget_parent_class)->finalize(object);
}

static void
gowlbar_title_widget_class_init(GowlbarTitleWidgetClass *klass)
{
	GObjectClass *object_class;
	GowlbarWidgetClass *widget_class;

	object_class = G_OBJECT_CLASS(klass);
	widget_class = GOWLBAR_WIDGET_CLASS(klass);

	object_class->finalize = gowlbar_title_widget_finalize;

	widget_class->render    = title_widget_render;
	widget_class->get_width = title_widget_get_width;
}

static void
gowlbar_title_widget_init(GowlbarTitleWidget *self)
{
	self->config = NULL;
	self->title  = NULL;
}

/* --- Public API --- */

/**
 * gowlbar_title_widget_new:
 * @config: (transfer none): the bar configuration
 *
 * Creates a new window title display widget.
 *
 * Returns: (transfer full): a new #GowlbarTitleWidget
 */
GowlbarTitleWidget *
gowlbar_title_widget_new(GowlbarConfig *config)
{
	GowlbarTitleWidget *self;

	self = (GowlbarTitleWidget *)g_object_new(
		GOWLBAR_TYPE_TITLE_WIDGET, NULL);
	self->config = config;

	return self;
}

/**
 * gowlbar_title_widget_set_title:
 * @self: the title widget
 * @title: (nullable): the window title to display
 *
 * Updates the displayed window title.
 */
void
gowlbar_title_widget_set_title(
	GowlbarTitleWidget *self,
	const gchar        *title
){
	g_return_if_fail(GOWLBAR_IS_TITLE_WIDGET(self));

	g_free(self->title);
	self->title = g_strdup(title);
}
