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

#include "gowlbar-layout-widget.h"

#include <string.h>
#include <stdlib.h>

/**
 * GowlbarLayoutWidget:
 *
 * Renders the current layout symbol (e.g. "[]=", "[M]", "><>")
 * in the bar.  The text is drawn using the configured layout-fg
 * colour.  Clicking the widget cycles the layout via IPC (Phase 4).
 */
struct _GowlbarLayoutWidget {
	GowlbarWidget  parent_instance;

	GowlbarConfig *config;        /* borrowed ref */
	gchar         *layout_name;   /* current layout symbol */
};

G_DEFINE_FINAL_TYPE(GowlbarLayoutWidget, gowlbar_layout_widget,
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
 * layout_widget_render:
 *
 * Renders the layout symbol text centred vertically in the
 * allocated area, using the configured layout-fg colour.
 */
static void
layout_widget_render(
	GowlbarWidget *widget,
	cairo_t       *cr,
	PangoLayout   *layout,
	gint           x,
	gint           y,
	gint           width,
	gint           height
){
	GowlbarLayoutWidget *self;
	gdouble fg_r, fg_g, fg_b;
	gint text_w, text_h;
	gint text_x, text_y;

	self = GOWLBAR_LAYOUT_WIDGET(widget);
	(void)width;

	if (self->layout_name == NULL || self->layout_name[0] == '\0')
		return;

	parse_hex_color(
		gowlbar_config_get_layout_fg(self->config),
		&fg_r, &fg_g, &fg_b);

	pango_layout_set_text(layout, self->layout_name, -1);
	pango_layout_get_pixel_size(layout, &text_w, &text_h);

	text_x = x;
	text_y = y + (height - text_h) / 2;

	cairo_set_source_rgb(cr, fg_r, fg_g, fg_b);
	cairo_move_to(cr, text_x, text_y);
	pango_cairo_show_layout(cr, layout);
}

/**
 * layout_widget_get_width:
 *
 * Returns the pixel width needed to display the current layout symbol.
 */
static gint
layout_widget_get_width(
	GowlbarWidget *widget,
	cairo_t       *cr,
	PangoLayout   *layout
){
	GowlbarLayoutWidget *self;
	gint text_w, text_h;

	self = GOWLBAR_LAYOUT_WIDGET(widget);
	(void)cr;

	if (self->layout_name == NULL || self->layout_name[0] == '\0')
		return 0;

	pango_layout_set_text(layout, self->layout_name, -1);
	pango_layout_get_pixel_size(layout, &text_w, &text_h);

	return text_w;
}

/**
 * layout_widget_on_click:
 *
 * Handles click to cycle layout.  (IPC wired in Phase 4.)
 */
static gboolean
layout_widget_on_click(
	GowlbarWidget *widget,
	gint           x,
	gint           y,
	guint          button
){
	(void)widget;
	(void)x;
	(void)y;
	(void)button;

	g_debug("gowlbar: layout widget clicked");
	return TRUE;
}

/* --- GObject lifecycle --- */

static void
gowlbar_layout_widget_finalize(GObject *object)
{
	GowlbarLayoutWidget *self;

	self = GOWLBAR_LAYOUT_WIDGET(object);

	g_free(self->layout_name);

	G_OBJECT_CLASS(gowlbar_layout_widget_parent_class)->finalize(object);
}

static void
gowlbar_layout_widget_class_init(GowlbarLayoutWidgetClass *klass)
{
	GObjectClass *object_class;
	GowlbarWidgetClass *widget_class;

	object_class = G_OBJECT_CLASS(klass);
	widget_class = GOWLBAR_WIDGET_CLASS(klass);

	object_class->finalize = gowlbar_layout_widget_finalize;

	widget_class->render    = layout_widget_render;
	widget_class->get_width = layout_widget_get_width;
	widget_class->on_click  = layout_widget_on_click;
}

static void
gowlbar_layout_widget_init(GowlbarLayoutWidget *self)
{
	self->config      = NULL;
	self->layout_name = g_strdup("[]=");  /* default tile layout */
}

/* --- Public API --- */

/**
 * gowlbar_layout_widget_new:
 * @config: (transfer none): the bar configuration
 *
 * Creates a new layout display widget.
 *
 * Returns: (transfer full): a new #GowlbarLayoutWidget
 */
GowlbarLayoutWidget *
gowlbar_layout_widget_new(GowlbarConfig *config)
{
	GowlbarLayoutWidget *self;

	self = (GowlbarLayoutWidget *)g_object_new(
		GOWLBAR_TYPE_LAYOUT_WIDGET, NULL);
	self->config = config;

	return self;
}

/**
 * gowlbar_layout_widget_set_layout:
 * @self: the layout widget
 * @layout_name: the layout symbol to display (e.g. "[]=")
 *
 * Updates the displayed layout name.
 */
void
gowlbar_layout_widget_set_layout(
	GowlbarLayoutWidget *self,
	const gchar         *layout_name
){
	g_return_if_fail(GOWLBAR_IS_LAYOUT_WIDGET(self));

	g_free(self->layout_name);
	self->layout_name = g_strdup(layout_name);
}
