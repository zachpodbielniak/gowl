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

#include "gowlbar-status-widget.h"

#include <string.h>
#include <stdlib.h>

/**
 * GowlbarStatusWidget:
 *
 * Renders status text in the bar.  Status text is typically set
 * from stdin (line-buffered) — each new line replaces the previous.
 * This allows piping: `while :; do date; sleep 1; done | gowlbar`.
 */
struct _GowlbarStatusWidget {
	GowlbarWidget  parent_instance;

	GowlbarConfig *config;  /* borrowed ref */
	gchar         *text;    /* current status text */
};

G_DEFINE_FINAL_TYPE(GowlbarStatusWidget, gowlbar_status_widget,
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
 * status_widget_render:
 *
 * Renders the status text right-aligned in the allocated area,
 * using the configured status-fg colour.
 */
static void
status_widget_render(
	GowlbarWidget *widget,
	cairo_t       *cr,
	PangoLayout   *layout,
	gint           x,
	gint           y,
	gint           width,
	gint           height
){
	GowlbarStatusWidget *self;
	gdouble fg_r, fg_g, fg_b;
	gint text_w, text_h;
	gint text_x, text_y;

	self = GOWLBAR_STATUS_WIDGET(widget);
	(void)width;

	if (self->text == NULL || self->text[0] == '\0')
		return;

	parse_hex_color(
		gowlbar_config_get_status_fg(self->config),
		&fg_r, &fg_g, &fg_b);

	pango_layout_set_text(layout, self->text, -1);
	pango_layout_get_pixel_size(layout, &text_w, &text_h);

	/* Right-align within allocated width */
	text_x = x + width - text_w;
	if (text_x < x)
		text_x = x;
	text_y = y + (height - text_h) / 2;

	cairo_set_source_rgb(cr, fg_r, fg_g, fg_b);
	cairo_move_to(cr, text_x, text_y);
	pango_cairo_show_layout(cr, layout);
}

/**
 * status_widget_get_width:
 *
 * Returns the pixel width needed to display the current status text.
 */
static gint
status_widget_get_width(
	GowlbarWidget *widget,
	cairo_t       *cr,
	PangoLayout   *layout
){
	GowlbarStatusWidget *self;
	gint text_w, text_h;

	self = GOWLBAR_STATUS_WIDGET(widget);
	(void)cr;

	if (self->text == NULL || self->text[0] == '\0')
		return 0;

	pango_layout_set_text(layout, self->text, -1);
	pango_layout_get_pixel_size(layout, &text_w, &text_h);

	return text_w;
}

/* --- GObject lifecycle --- */

static void
gowlbar_status_widget_finalize(GObject *object)
{
	GowlbarStatusWidget *self;

	self = GOWLBAR_STATUS_WIDGET(object);

	g_free(self->text);

	G_OBJECT_CLASS(gowlbar_status_widget_parent_class)->finalize(object);
}

static void
gowlbar_status_widget_class_init(GowlbarStatusWidgetClass *klass)
{
	GObjectClass *object_class;
	GowlbarWidgetClass *widget_class;

	object_class = G_OBJECT_CLASS(klass);
	widget_class = GOWLBAR_WIDGET_CLASS(klass);

	object_class->finalize = gowlbar_status_widget_finalize;

	widget_class->render    = status_widget_render;
	widget_class->get_width = status_widget_get_width;
}

static void
gowlbar_status_widget_init(GowlbarStatusWidget *self)
{
	self->config = NULL;
	self->text   = NULL;
}

/* --- Public API --- */

/**
 * gowlbar_status_widget_new:
 * @config: (transfer none): the bar configuration
 *
 * Creates a new status text display widget.
 *
 * Returns: (transfer full): a new #GowlbarStatusWidget
 */
GowlbarStatusWidget *
gowlbar_status_widget_new(GowlbarConfig *config)
{
	GowlbarStatusWidget *self;

	self = (GowlbarStatusWidget *)g_object_new(
		GOWLBAR_TYPE_STATUS_WIDGET, NULL);
	self->config = config;

	return self;
}

/**
 * gowlbar_status_widget_set_text:
 * @self: the status widget
 * @text: (nullable): the status text to display
 *
 * Updates the displayed status text.
 */
void
gowlbar_status_widget_set_text(
	GowlbarStatusWidget *self,
	const gchar         *text
){
	g_return_if_fail(GOWLBAR_IS_STATUS_WIDGET(self));

	g_free(self->text);
	self->text = g_strdup(text);
}
