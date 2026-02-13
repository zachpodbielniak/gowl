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

#include "gowlbar-tag-widget.h"

#include <string.h>
#include <stdlib.h>

/**
 * GowlbarTagWidget:
 *
 * Renders numbered tag indicator boxes in the bar.  Each tag is
 * drawn as a small numbered box whose colour depends on its state:
 *   - active (selected on this output): tag-active-bg / tag-active-fg
 *   - urgent (has urgent client):       tag-urgent-bg / tag-urgent-fg
 *   - occupied (has clients):           background    / tag-occupied-fg
 *   - empty (no clients):               background    / tag-empty-fg
 *
 * Clicking on a tag box will later send a tag_view IPC command
 * (wired in Phase 4).
 */
struct _GowlbarTagWidget {
	GowlbarWidget  parent_instance;

	GowlbarConfig *config;   /* borrowed ref */

	guint32        active_mask;
	guint32        occupied_mask;
	guint32        urgent_mask;
	guint32        sel_tags;
};

G_DEFINE_FINAL_TYPE(GowlbarTagWidget, gowlbar_tag_widget,
                    GOWLBAR_TYPE_WIDGET)

/* --- Colour parsing helper --- */

/**
 * parse_hex_color:
 * @hex: a hex colour string like "#rrggbb"
 * @r: (out): red component 0.0–1.0
 * @g: (out): green component 0.0–1.0
 * @b: (out): blue component 0.0–1.0
 *
 * Parses a "#rrggbb" hex string into normalised RGB values.
 */
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
 * tag_widget_render:
 *
 * Renders each tag as a numbered box.  Box size is based on the bar
 * height to keep tags square.  Tag numbers are 1-9.
 */
static void
tag_widget_render(
	GowlbarWidget *widget,
	cairo_t       *cr,
	PangoLayout   *layout,
	gint           x,
	gint           y,
	gint           width,
	gint           height
){
	GowlbarTagWidget *self;
	gint padding;
	gint box_size;
	gint i;
	gint cur_x;

	self = GOWLBAR_TAG_WIDGET(widget);
	(void)width;

	padding = gowlbar_config_get_padding(self->config);
	box_size = height;
	cur_x = x;

	for (i = 0; i < GOWLBAR_TAG_COUNT; i++) {
		guint32 tag_bit;
		gboolean is_active;
		gboolean is_urgent;
		gboolean is_occupied;
		gdouble bg_r, bg_g, bg_b;
		gdouble fg_r, fg_g, fg_b;
		gchar label[4];
		gint text_w, text_h;
		gint text_x, text_y;

		tag_bit = (guint32)(1 << i);
		is_active  = (self->sel_tags & tag_bit) != 0;
		is_urgent  = (self->urgent_mask & tag_bit) != 0;
		is_occupied = (self->occupied_mask & tag_bit) != 0;

		/* Determine colours based on tag state */
		if (is_active) {
			parse_hex_color(
				gowlbar_config_get_tag_active_bg(self->config),
				&bg_r, &bg_g, &bg_b);
			parse_hex_color(
				gowlbar_config_get_tag_active_fg(self->config),
				&fg_r, &fg_g, &fg_b);
		} else if (is_urgent) {
			parse_hex_color(
				gowlbar_config_get_tag_urgent_bg(self->config),
				&bg_r, &bg_g, &bg_b);
			parse_hex_color(
				gowlbar_config_get_tag_urgent_fg(self->config),
				&fg_r, &fg_g, &fg_b);
		} else if (is_occupied) {
			parse_hex_color(
				gowlbar_config_get_background(self->config),
				&bg_r, &bg_g, &bg_b);
			parse_hex_color(
				gowlbar_config_get_tag_occupied_fg(self->config),
				&fg_r, &fg_g, &fg_b);
		} else {
			parse_hex_color(
				gowlbar_config_get_background(self->config),
				&bg_r, &bg_g, &bg_b);
			parse_hex_color(
				gowlbar_config_get_tag_empty_fg(self->config),
				&fg_r, &fg_g, &fg_b);
		}

		/* Draw tag background box */
		cairo_set_source_rgb(cr, bg_r, bg_g, bg_b);
		cairo_rectangle(cr, cur_x, y, box_size, height);
		cairo_fill(cr);

		/* Draw tag number label */
		g_snprintf(label, sizeof(label), "%d", i + 1);
		pango_layout_set_text(layout, label, -1);
		pango_layout_get_pixel_size(layout, &text_w, &text_h);

		/* Centre the label in the box */
		text_x = cur_x + (box_size - text_w) / 2;
		text_y = y + (height - text_h) / 2;

		cairo_set_source_rgb(cr, fg_r, fg_g, fg_b);
		cairo_move_to(cr, text_x, text_y);
		pango_cairo_show_layout(cr, layout);

		cur_x += box_size + padding;
	}
}

/**
 * tag_widget_get_width:
 *
 * Returns the total width needed: 9 tag boxes + 8 padding gaps.
 * Each tag box is square (height x height).
 */
static gint
tag_widget_get_width(
	GowlbarWidget *widget,
	cairo_t       *cr,
	PangoLayout   *layout
){
	GowlbarTagWidget *self;
	gint padding;
	gint box_size;

	self = GOWLBAR_TAG_WIDGET(widget);
	(void)cr;
	(void)layout;

	padding = gowlbar_config_get_padding(self->config);
	box_size = gowlbar_config_get_height(self->config);

	return (box_size * GOWLBAR_TAG_COUNT) +
	       (padding * (GOWLBAR_TAG_COUNT - 1));
}

/**
 * tag_widget_on_click:
 *
 * Determines which tag was clicked based on x position.
 * Returns %TRUE if the click was within a tag box.
 * (IPC command sending wired in Phase 4.)
 */
static gboolean
tag_widget_on_click(
	GowlbarWidget *widget,
	gint           x,
	gint           y,
	guint          button
){
	GowlbarTagWidget *self;
	gint padding;
	gint box_size;
	gint tag_idx;

	self = GOWLBAR_TAG_WIDGET(widget);
	(void)y;
	(void)button;

	padding = gowlbar_config_get_padding(self->config);
	box_size = gowlbar_config_get_height(self->config);

	/* Determine which tag was clicked */
	tag_idx = x / (box_size + padding);
	if (tag_idx >= 0 && tag_idx < GOWLBAR_TAG_COUNT) {
		g_debug("gowlbar: tag %d clicked", tag_idx + 1);
		return TRUE;
	}

	return FALSE;
}

/* --- GObject lifecycle --- */

static void
gowlbar_tag_widget_class_init(GowlbarTagWidgetClass *klass)
{
	GowlbarWidgetClass *widget_class;

	widget_class = GOWLBAR_WIDGET_CLASS(klass);

	widget_class->render    = tag_widget_render;
	widget_class->get_width = tag_widget_get_width;
	widget_class->on_click  = tag_widget_on_click;
}

static void
gowlbar_tag_widget_init(GowlbarTagWidget *self)
{
	self->config        = NULL;
	self->active_mask   = 0;
	self->occupied_mask = 0;
	self->urgent_mask   = 0;
	self->sel_tags      = 1;  /* tag 1 selected by default */
}

/* --- Public API --- */

/**
 * gowlbar_tag_widget_new:
 * @config: (transfer none): the bar configuration
 *
 * Creates a new tag indicator widget that reads colours from @config.
 *
 * Returns: (transfer full): a new #GowlbarTagWidget
 */
GowlbarTagWidget *
gowlbar_tag_widget_new(GowlbarConfig *config)
{
	GowlbarTagWidget *self;

	self = (GowlbarTagWidget *)g_object_new(
		GOWLBAR_TYPE_TAG_WIDGET, NULL);
	self->config = config;

	return self;
}

/**
 * gowlbar_tag_widget_set_state:
 * @self: the tag widget
 * @active_mask: bitmask of active tags
 * @occupied_mask: bitmask of occupied tags
 * @urgent_mask: bitmask of urgent tags
 * @sel_tags: bitmask of selected tags on this output
 *
 * Updates the tag state bitmasks for rendering.
 */
void
gowlbar_tag_widget_set_state(
	GowlbarTagWidget *self,
	guint32           active_mask,
	guint32           occupied_mask,
	guint32           urgent_mask,
	guint32           sel_tags
){
	g_return_if_fail(GOWLBAR_IS_TAG_WIDGET(self));

	self->active_mask   = active_mask;
	self->occupied_mask = occupied_mask;
	self->urgent_mask   = urgent_mask;
	self->sel_tags      = sel_tags;
}
