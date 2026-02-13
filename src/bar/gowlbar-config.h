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

#ifndef GOWLBAR_CONFIG_H
#define GOWLBAR_CONFIG_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWLBAR_TYPE_CONFIG (gowlbar_config_get_type())

G_DECLARE_FINAL_TYPE(GowlbarConfig, gowlbar_config, GOWLBAR, CONFIG, GObject)

/* --- Property IDs --- */

/**
 * GowlbarConfigProp:
 * @GOWLBAR_CONFIG_PROP_HEIGHT: "height" property.
 * @GOWLBAR_CONFIG_PROP_POSITION: "position" property.
 * @GOWLBAR_CONFIG_PROP_FONT: "font" property.
 * @GOWLBAR_CONFIG_PROP_PADDING: "padding" property.
 * @GOWLBAR_CONFIG_PROP_BG: "background" property.
 * @GOWLBAR_CONFIG_PROP_FG: "foreground" property.
 * @GOWLBAR_CONFIG_PROP_TAG_ACTIVE_BG: "tag-active-bg" property.
 * @GOWLBAR_CONFIG_PROP_TAG_ACTIVE_FG: "tag-active-fg" property.
 * @GOWLBAR_CONFIG_PROP_TAG_URGENT_BG: "tag-urgent-bg" property.
 * @GOWLBAR_CONFIG_PROP_TAG_URGENT_FG: "tag-urgent-fg" property.
 * @GOWLBAR_CONFIG_PROP_TAG_OCCUPIED_FG: "tag-occupied-fg" property.
 * @GOWLBAR_CONFIG_PROP_TAG_EMPTY_FG: "tag-empty-fg" property.
 * @GOWLBAR_CONFIG_PROP_LAYOUT_FG: "layout-fg" property.
 * @GOWLBAR_CONFIG_PROP_TITLE_FG: "title-fg" property.
 * @GOWLBAR_CONFIG_PROP_STATUS_FG: "status-fg" property.
 * @GOWLBAR_CONFIG_PROP_BORDER_COLOR: "border-color" property.
 * @GOWLBAR_CONFIG_PROP_LAST: sentinel; total number of properties.
 *
 * Property identifiers for #GowlbarConfig GObject properties.
 */
typedef enum {
	GOWLBAR_CONFIG_PROP_0 = 0,
	GOWLBAR_CONFIG_PROP_HEIGHT,
	GOWLBAR_CONFIG_PROP_POSITION,
	GOWLBAR_CONFIG_PROP_FONT,
	GOWLBAR_CONFIG_PROP_PADDING,
	GOWLBAR_CONFIG_PROP_BG,
	GOWLBAR_CONFIG_PROP_FG,
	GOWLBAR_CONFIG_PROP_TAG_ACTIVE_BG,
	GOWLBAR_CONFIG_PROP_TAG_ACTIVE_FG,
	GOWLBAR_CONFIG_PROP_TAG_URGENT_BG,
	GOWLBAR_CONFIG_PROP_TAG_URGENT_FG,
	GOWLBAR_CONFIG_PROP_TAG_OCCUPIED_FG,
	GOWLBAR_CONFIG_PROP_TAG_EMPTY_FG,
	GOWLBAR_CONFIG_PROP_LAYOUT_FG,
	GOWLBAR_CONFIG_PROP_TITLE_FG,
	GOWLBAR_CONFIG_PROP_STATUS_FG,
	GOWLBAR_CONFIG_PROP_BORDER_COLOR,
	GOWLBAR_CONFIG_PROP_LAST
} GowlbarConfigProp;

/* --- Signals --- */

/**
 * GowlbarConfig signals:
 *
 * "changed"  - emitted when a config property changes.
 *              Signature: void handler(GowlbarConfig *config,
 *                                      const gchar *property_name,
 *                                      gpointer user_data);
 *
 * "reloaded" - emitted after a full configuration reload completes.
 *              Signature: void handler(GowlbarConfig *config,
 *                                      gpointer user_data);
 */

/* --- Construction --- */

/**
 * gowlbar_config_new:
 *
 * Creates a new #GowlbarConfig with all default values.
 *
 * Returns: (transfer full): a new #GowlbarConfig
 */
GowlbarConfig *gowlbar_config_new(void);

/* --- YAML Loading --- */

/**
 * gowlbar_config_load_yaml:
 * @self: a #GowlbarConfig
 * @path: filesystem path to a YAML configuration file
 * @error: (nullable): return location for a #GError
 *
 * Loads configuration from the YAML file at @path, applying
 * values on top of the current configuration.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
gowlbar_config_load_yaml(
	GowlbarConfig  *self,
	const gchar    *path,
	GError        **error
);

/**
 * gowlbar_config_load_yaml_from_search_path:
 * @self: a #GowlbarConfig
 * @error: (nullable): return location for a #GError
 *
 * Searches for a bar.yaml configuration file in the following
 * directories (first match wins):
 *   1. ./data/
 *   2. ~/.config/gowl/
 *   3. /etc/gowl/
 *   4. /usr/local/gowl/
 *
 * If no file is found, the config retains its defaults and the
 * function returns %TRUE (no error).
 *
 * Returns: %TRUE on success, %FALSE if a file was found but invalid
 */
gboolean
gowlbar_config_load_yaml_from_search_path(
	GowlbarConfig  *self,
	GError        **error
);

/* --- YAML Generation --- */

/**
 * gowlbar_config_generate_yaml:
 * @self: a #GowlbarConfig
 *
 * Serialises the current configuration state to a YAML string
 * suitable for writing to disk.
 *
 * Returns: (transfer full): a newly allocated YAML string; free with g_free()
 */
gchar *gowlbar_config_generate_yaml(GowlbarConfig *self);

/* --- Property Getters --- */

/**
 * gowlbar_config_get_height:
 * @self: a #GowlbarConfig
 *
 * Returns: the bar height in pixels
 */
gint gowlbar_config_get_height(GowlbarConfig *self);

/**
 * gowlbar_config_get_position:
 * @self: a #GowlbarConfig
 *
 * Returns: (transfer none): the bar position string ("top" or "bottom")
 */
const gchar *gowlbar_config_get_position(GowlbarConfig *self);

/**
 * gowlbar_config_get_font:
 * @self: a #GowlbarConfig
 *
 * Returns: (transfer none): the Pango font description string
 */
const gchar *gowlbar_config_get_font(GowlbarConfig *self);

/**
 * gowlbar_config_get_padding:
 * @self: a #GowlbarConfig
 *
 * Returns: the horizontal padding in pixels
 */
gint gowlbar_config_get_padding(GowlbarConfig *self);

/**
 * gowlbar_config_get_background:
 * @self: a #GowlbarConfig
 *
 * Returns: (transfer none): the background colour hex string
 */
const gchar *gowlbar_config_get_background(GowlbarConfig *self);

/**
 * gowlbar_config_get_foreground:
 * @self: a #GowlbarConfig
 *
 * Returns: (transfer none): the foreground colour hex string
 */
const gchar *gowlbar_config_get_foreground(GowlbarConfig *self);

/**
 * gowlbar_config_get_tag_active_bg:
 * @self: a #GowlbarConfig
 *
 * Returns: (transfer none): the active tag background colour hex string
 */
const gchar *gowlbar_config_get_tag_active_bg(GowlbarConfig *self);

/**
 * gowlbar_config_get_tag_active_fg:
 * @self: a #GowlbarConfig
 *
 * Returns: (transfer none): the active tag foreground colour hex string
 */
const gchar *gowlbar_config_get_tag_active_fg(GowlbarConfig *self);

/**
 * gowlbar_config_get_tag_urgent_bg:
 * @self: a #GowlbarConfig
 *
 * Returns: (transfer none): the urgent tag background colour hex string
 */
const gchar *gowlbar_config_get_tag_urgent_bg(GowlbarConfig *self);

/**
 * gowlbar_config_get_tag_urgent_fg:
 * @self: a #GowlbarConfig
 *
 * Returns: (transfer none): the urgent tag foreground colour hex string
 */
const gchar *gowlbar_config_get_tag_urgent_fg(GowlbarConfig *self);

/**
 * gowlbar_config_get_tag_occupied_fg:
 * @self: a #GowlbarConfig
 *
 * Returns: (transfer none): the occupied tag foreground colour hex string
 */
const gchar *gowlbar_config_get_tag_occupied_fg(GowlbarConfig *self);

/**
 * gowlbar_config_get_tag_empty_fg:
 * @self: a #GowlbarConfig
 *
 * Returns: (transfer none): the empty tag foreground colour hex string
 */
const gchar *gowlbar_config_get_tag_empty_fg(GowlbarConfig *self);

/**
 * gowlbar_config_get_layout_fg:
 * @self: a #GowlbarConfig
 *
 * Returns: (transfer none): the layout indicator foreground colour hex string
 */
const gchar *gowlbar_config_get_layout_fg(GowlbarConfig *self);

/**
 * gowlbar_config_get_title_fg:
 * @self: a #GowlbarConfig
 *
 * Returns: (transfer none): the window title foreground colour hex string
 */
const gchar *gowlbar_config_get_title_fg(GowlbarConfig *self);

/**
 * gowlbar_config_get_status_fg:
 * @self: a #GowlbarConfig
 *
 * Returns: (transfer none): the status text foreground colour hex string
 */
const gchar *gowlbar_config_get_status_fg(GowlbarConfig *self);

/**
 * gowlbar_config_get_border_color:
 * @self: a #GowlbarConfig
 *
 * Returns: (transfer none): the border colour hex string
 */
const gchar *gowlbar_config_get_border_color(GowlbarConfig *self);

G_END_DECLS

#endif /* GOWLBAR_CONFIG_H */
