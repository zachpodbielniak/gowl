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

#ifndef GOWL_CONFIG_H
#define GOWL_CONFIG_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_CONFIG (gowl_config_get_type())

G_DECLARE_FINAL_TYPE(GowlConfig, gowl_config, GOWL, CONFIG, GObject)

/* --- GowlKeybindEntry --- */

/**
 * GowlKeybindEntry:
 * @modifiers: bitmask of #GowlKeyMod flags
 * @keysym: XKB keysym value
 * @action: a #GowlAction value
 * @arg: (nullable): optional argument string for the action
 *
 * A single keybind mapping stored in the config.
 */
typedef struct {
	guint  modifiers;
	guint  keysym;
	gint   action;
	gchar *arg;
} GowlKeybindEntry;

/* --- GowlRuleEntry --- */

/**
 * GowlRuleEntry:
 * @app_id: (nullable): Wayland app_id to match, or %NULL for any
 * @title: (nullable): window title pattern to match, or %NULL for any
 * @tags: bitmask of tags to assign when matched
 * @floating: whether the matched client should float
 * @monitor: monitor index to place the client on, or -1 for default
 *
 * A window rule entry stored in the config.
 */
typedef struct {
	gchar    *app_id;
	gchar    *title;
	guint32   tags;
	gboolean  floating;
	gint      monitor;
} GowlRuleEntry;

/* --- Property IDs (for GObject property enumeration) --- */

/**
 * GowlConfigProp:
 * @GOWL_CONFIG_PROP_BORDER_WIDTH: "border-width" property.
 * @GOWL_CONFIG_PROP_BORDER_COLOR_FOCUS: "border-color-focus" property.
 * @GOWL_CONFIG_PROP_BORDER_COLOR_UNFOCUS: "border-color-unfocus" property.
 * @GOWL_CONFIG_PROP_BORDER_COLOR_URGENT: "border-color-urgent" property.
 * @GOWL_CONFIG_PROP_MFACT: "mfact" property.
 * @GOWL_CONFIG_PROP_NMASTER: "nmaster" property.
 * @GOWL_CONFIG_PROP_TAG_COUNT: "tag-count" property.
 * @GOWL_CONFIG_PROP_REPEAT_RATE: "repeat-rate" property.
 * @GOWL_CONFIG_PROP_REPEAT_DELAY: "repeat-delay" property.
 * @GOWL_CONFIG_PROP_TERMINAL: "terminal" property.
 * @GOWL_CONFIG_PROP_MENU: "menu" property.
 * @GOWL_CONFIG_PROP_SLOPPYFOCUS: "sloppyfocus" property.
 * @GOWL_CONFIG_PROP_LOG_LEVEL: "log-level" property.
 * @GOWL_CONFIG_PROP_LOG_FILE: "log-file" property.
 * @GOWL_CONFIG_PROP_LAST: sentinel; total number of properties.
 *
 * Property identifiers for #GowlConfig GObject properties.
 */
typedef enum {
	GOWL_CONFIG_PROP_0 = 0,
	GOWL_CONFIG_PROP_BORDER_WIDTH,
	GOWL_CONFIG_PROP_BORDER_COLOR_FOCUS,
	GOWL_CONFIG_PROP_BORDER_COLOR_UNFOCUS,
	GOWL_CONFIG_PROP_BORDER_COLOR_URGENT,
	GOWL_CONFIG_PROP_MFACT,
	GOWL_CONFIG_PROP_NMASTER,
	GOWL_CONFIG_PROP_TAG_COUNT,
	GOWL_CONFIG_PROP_REPEAT_RATE,
	GOWL_CONFIG_PROP_REPEAT_DELAY,
	GOWL_CONFIG_PROP_TERMINAL,
	GOWL_CONFIG_PROP_MENU,
	GOWL_CONFIG_PROP_SLOPPYFOCUS,
	GOWL_CONFIG_PROP_LOG_LEVEL,
	GOWL_CONFIG_PROP_LOG_FILE,
	GOWL_CONFIG_PROP_LAST
} GowlConfigProp;

/* --- Signals --- */

/**
 * GowlConfig signals:
 *
 * "changed"  - emitted when a config property changes.
 *              Signature: void handler(GowlConfig *config,
 *                                      const gchar *property_name,
 *                                      gpointer user_data);
 *
 * "reloaded" - emitted after a full configuration reload completes.
 *              Signature: void handler(GowlConfig *config,
 *                                      gpointer user_data);
 */

/* --- Construction --- */

/**
 * gowl_config_new:
 *
 * Creates a new #GowlConfig with all default values.
 *
 * Returns: (transfer full): a new #GowlConfig
 */
GowlConfig *gowl_config_new(void);

/* --- YAML Loading --- */

/**
 * gowl_config_load_yaml:
 * @self: a #GowlConfig
 * @path: filesystem path to a YAML configuration file
 * @error: (nullable): return location for a #GError
 *
 * Loads configuration from the YAML file at @path, applying
 * values on top of the current configuration.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
gowl_config_load_yaml(
	GowlConfig   *self,
	const gchar  *path,
	GError      **error
);

/**
 * gowl_config_load_yaml_from_search_path:
 * @self: a #GowlConfig
 * @error: (nullable): return location for a #GError
 *
 * Searches for a gowl.yaml configuration file in the following
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
gowl_config_load_yaml_from_search_path(
	GowlConfig  *self,
	GError     **error
);

/* --- YAML Generation --- */

/**
 * gowl_config_generate_yaml:
 * @self: a #GowlConfig
 *
 * Serialises the current configuration state to a YAML string
 * suitable for writing to disk.
 *
 * Returns: (transfer full): a newly allocated YAML string; free with g_free()
 */
gchar *gowl_config_generate_yaml(GowlConfig *self);

/* --- Property Getters --- */

/**
 * gowl_config_get_border_width:
 * @self: a #GowlConfig
 *
 * Returns: the border width in pixels
 */
gint gowl_config_get_border_width(GowlConfig *self);

/**
 * gowl_config_get_border_color_focus:
 * @self: a #GowlConfig
 *
 * Returns: (transfer none): the focused border colour hex string
 */
const gchar *gowl_config_get_border_color_focus(GowlConfig *self);

/**
 * gowl_config_get_border_color_unfocus:
 * @self: a #GowlConfig
 *
 * Returns: (transfer none): the unfocused border colour hex string
 */
const gchar *gowl_config_get_border_color_unfocus(GowlConfig *self);

/**
 * gowl_config_get_border_color_urgent:
 * @self: a #GowlConfig
 *
 * Returns: (transfer none): the urgent border colour hex string
 */
const gchar *gowl_config_get_border_color_urgent(GowlConfig *self);

/**
 * gowl_config_get_mfact:
 * @self: a #GowlConfig
 *
 * Returns: the master area factor (0.0 - 1.0)
 */
gdouble gowl_config_get_mfact(GowlConfig *self);

/**
 * gowl_config_get_nmaster:
 * @self: a #GowlConfig
 *
 * Returns: the number of master windows
 */
gint gowl_config_get_nmaster(GowlConfig *self);

/**
 * gowl_config_get_tag_count:
 * @self: a #GowlConfig
 *
 * Returns: the number of tags (workspaces)
 */
gint gowl_config_get_tag_count(GowlConfig *self);

/**
 * gowl_config_get_repeat_rate:
 * @self: a #GowlConfig
 *
 * Returns: the keyboard repeat rate (keys per second)
 */
gint gowl_config_get_repeat_rate(GowlConfig *self);

/**
 * gowl_config_get_repeat_delay:
 * @self: a #GowlConfig
 *
 * Returns: the keyboard repeat delay (milliseconds)
 */
gint gowl_config_get_repeat_delay(GowlConfig *self);

/**
 * gowl_config_get_terminal:
 * @self: a #GowlConfig
 *
 * Returns: (transfer none): the terminal command string
 */
const gchar *gowl_config_get_terminal(GowlConfig *self);

/**
 * gowl_config_get_menu:
 * @self: a #GowlConfig
 *
 * Returns: (transfer none): the menu / launcher command string
 */
const gchar *gowl_config_get_menu(GowlConfig *self);

/**
 * gowl_config_get_sloppyfocus:
 * @self: a #GowlConfig
 *
 * Returns: %TRUE if sloppy (mouse-follows-focus) is enabled
 */
gboolean gowl_config_get_sloppyfocus(GowlConfig *self);

/**
 * gowl_config_get_log_level:
 * @self: a #GowlConfig
 *
 * Returns: (transfer none): the log level string (e.g. "warning", "debug")
 */
const gchar *gowl_config_get_log_level(GowlConfig *self);

/**
 * gowl_config_get_log_file:
 * @self: a #GowlConfig
 *
 * Returns: (transfer none): the log file path, or "stderr" for stderr only
 */
const gchar *gowl_config_get_log_file(GowlConfig *self);

/* --- Keybinds --- */

/**
 * gowl_config_add_keybind:
 * @self: a #GowlConfig
 * @modifiers: bitmask of #GowlKeyMod flags
 * @keysym: XKB keysym value
 * @action: a #GowlAction value
 * @arg: (nullable): optional argument string for the action
 *
 * Appends a keybind entry to the configuration.
 */
void
gowl_config_add_keybind(
	GowlConfig  *self,
	guint        modifiers,
	guint        keysym,
	gint         action,
	const gchar *arg
);

/**
 * gowl_config_get_keybinds:
 * @self: a #GowlConfig
 *
 * Gets the array of configured keybinds.
 *
 * Returns: (transfer none) (element-type GowlKeybindEntry): the keybind
 *          array. Do not free.
 */
GArray *gowl_config_get_keybinds(GowlConfig *self);

/* --- Rules --- */

/**
 * gowl_config_add_rule:
 * @self: a #GowlConfig
 * @app_id: (nullable): app_id pattern or %NULL for any
 * @title: (nullable): title pattern or %NULL for any
 * @tags: tag bitmask to assign
 * @floating: whether the matched client should float
 * @monitor: target monitor index, or -1 for default
 *
 * Appends a window rule entry to the configuration.
 */
void
gowl_config_add_rule(
	GowlConfig  *self,
	const gchar *app_id,
	const gchar *title,
	guint32      tags,
	gboolean     floating,
	gint         monitor
);

/**
 * gowl_config_get_rules:
 * @self: a #GowlConfig
 *
 * Gets the array of configured window rules.
 *
 * Returns: (transfer none) (element-type GowlRuleEntry): the rules
 *          array. Do not free.
 */
GPtrArray *gowl_config_get_rules(GowlConfig *self);

/* --- Module Configuration --- */

/**
 * gowl_config_get_module_config:
 * @self: a #GowlConfig
 * @module_name: the name of the module (e.g. "vanitygaps")
 *
 * Returns the per-module settings hash table from the YAML config's
 * `modules:` section.  Keys and values are strings.
 *
 * Returns: (transfer none) (nullable): a #GHashTable, or %NULL
 */
GHashTable *gowl_config_get_module_config(GowlConfig  *self,
                                           const gchar *module_name);

/**
 * gowl_config_get_all_module_configs:
 * @self: a #GowlConfig
 *
 * Returns the entire module configuration table mapping module
 * names to their settings hash tables.
 *
 * Returns: (transfer none) (nullable): a #GHashTable, or %NULL
 */
GHashTable *gowl_config_get_all_module_configs(GowlConfig *self);

G_END_DECLS

#endif /* GOWL_CONFIG_H */
