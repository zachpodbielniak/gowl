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
 * GowlDropdownEntry:
 * @name: identifier used to toggle the dropdown (e.g. "term").
 *        Duplicates across entries are disallowed.
 * @spawn_cmd: shell command spawned on first toggle.  Interpreted
 *             by g_spawn_command_line_async().
 * @keybind: keybind string parsed via gowl_keybind_parse(), e.g.
 *           "Super+grave".  May be %NULL, in which case the
 *           dropdown is only toggleable via the elisp API.
 * @width_pct: dropdown width as a fraction of the output width
 *             (0.0 – 1.0).  0.0 means "not set".
 * @height_pct: dropdown height as a fraction of the output
 *              height (0.0 – 1.0).
 * @width_abs: absolute width in pixels, takes precedence over
 *             @width_pct when non-zero.
 * @height_abs: absolute height in pixels, takes precedence over
 *              @height_pct when non-zero.
 * @anchor: 0=top, 1=bottom, 2=left, 3=right.  Controls which
 *          edge of the output the dropdown attaches to.
 *
 * A dropdown (Guake-style toggleable terminal/window) entry.
 */
typedef struct {
	gchar    *name;
	gchar    *spawn_cmd;
	gchar    *keybind;
	gdouble   width_pct;
	gdouble   height_pct;
	gint      width_abs;
	gint      height_abs;
	gint      anchor;
} GowlDropdownEntry;

/**
 * GowlRuleEntry:
 * @app_id: (nullable): Wayland app_id to match, or %NULL for any
 * @title: (nullable): window title pattern to match, or %NULL for any
 * @tags: bitmask of tags to assign when matched
 * @floating: whether the matched client should float
 * @monitor: monitor index to place the client on, or -1 for default
 * @width: explicit width in pixels for floated matches, or 0 to
 *         keep the client's natural size
 * @height: explicit height in pixels for floated matches, or 0
 *          to keep the client's natural size
 * @center: when @floating is %TRUE, center the client on its
 *          target monitor's usable area
 * @regex_mode: when %TRUE, interpret @app_id and @title as PCRE
 *              regexes (via #GRegex) rather than shell globs
 *
 * A window rule entry stored in the config.
 */
typedef struct {
	gchar    *app_id;
	gchar    *title;
	guint32   tags;
	gboolean  floating;
	gint      monitor;
	gint      width;
	gint      height;
	gboolean  center;
	gboolean  regex_mode;
} GowlRuleEntry;

/* --- GowlMonitorConfig --- */

/**
 * GowlMonitorConfig:
 * @width: preferred width in pixels, or 0 for "unset" (use the
 *         output's preferred mode).  Only honoured when paired
 *         with a non-zero @height.
 * @height: preferred height in pixels, or 0 for "unset".
 * @refresh: refresh rate in Hz (e.g. 60.0), or 0.0 for "unset" /
 *           "preferred refresh".  Internally converted to mHz when
 *           applied via wlroots.
 * @x: preferred X position in layout coordinates, or %G_MININT
 *     for "unset".  0 is a valid value (top-left), hence the
 *     out-of-band sentinel.
 * @y: preferred Y position, or %G_MININT for "unset".
 * @scale: HiDPI scale factor (e.g. 1.0, 1.5, 2.0), or 0.0 for
 *         "unset" (leave the compositor default).
 * @transform: wl_output transform code, 0..7, or -1 for "unset".
 *             0=normal, 1=90, 2=180, 3=270, 4=flipped,
 *             5=flipped-90, 6=flipped-180, 7=flipped-270.
 * @enabled: tri-state -1=unset, 0=disabled, 1=enabled.
 *
 * A per-output configuration parsed from the YAML `monitors:`
 * mapping.  Every field is independently optional so callers may
 * cherry-pick what to override (e.g. only `transform:` for a
 * tablet that boots in the wrong orientation).  The compositor
 * applies each set field to the corresponding #GowlMonitor when
 * the output comes online and again on `reload_config`.
 */
typedef struct {
	gint     width;
	gint     height;
	gdouble  refresh;
	gint     x;
	gint     y;
	gdouble  scale;
	gint     transform;
	gint     enabled;
} GowlMonitorConfig;

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
 * @GOWL_CONFIG_PROP_MANAGE_LID: "manage-lid" property.
 * @GOWL_CONFIG_PROP_LOG_LEVEL: "log-level" property.
 * @GOWL_CONFIG_PROP_LOG_FILE: "log-file" property.
 * @GOWL_CONFIG_PROP_EVALUATE_GOWL_CONFIG_WITH_CMACS:
 *   "evaluate-gowl-config-with-cmacs" property.  cmacs-only semantic:
 *   when %FALSE, cmacs `--gowl` startup resets all other values in the
 *   config back to defaults after parsing.  Ignored by gowl's
 *   standalone main.
 * @GOWL_CONFIG_PROP_EVALUATE_C_CONFIG_WITH_CMACS:
 *   "evaluate-c-config-with-cmacs" property.  cmacs-only semantic:
 *   when %FALSE, cmacs `--gowl` startup skips loading the user's C
 *   config entirely.  Ignored by gowl's standalone main.
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
	GOWL_CONFIG_PROP_MANAGE_LID,
	GOWL_CONFIG_PROP_LOG_LEVEL,
	GOWL_CONFIG_PROP_LOG_FILE,
	GOWL_CONFIG_PROP_EVALUATE_GOWL_CONFIG_WITH_CMACS,
	GOWL_CONFIG_PROP_EVALUATE_C_CONFIG_WITH_CMACS,
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
 * Searches for a config.yaml configuration file in the following
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
 * gowl_config_get_manage_lid:
 * @self: a #GowlConfig
 *
 * Returns: %TRUE if laptop-lid output management is enabled.  When on,
 *   an internal panel (eDP/LVDS/DSI) is powered off while the lid is
 *   shut and at least one external display is connected.
 */
gboolean gowl_config_get_manage_lid(GowlConfig *self);

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

/**
 * gowl_config_get_evaluate_gowl_config_with_cmacs:
 * @self: a #GowlConfig
 *
 * Reads the root-level cmacs evaluation gate.  When %FALSE, cmacs
 * `--gowl` startup resets all other config values to their defaults
 * after the YAML / C config load, preventing user customisations from
 * affecting the embedded compositor.  Standalone and nested gowl
 * ignore this flag; it is semantically a cmacs-only hint carried
 * through the #GowlConfig property surface so it is introspectable.
 *
 * Returns: %TRUE if cmacs should honour this config
 */
gboolean
gowl_config_get_evaluate_gowl_config_with_cmacs(GowlConfig *self);

/**
 * gowl_config_set_evaluate_gowl_config_with_cmacs:
 * @self: a #GowlConfig
 * @value: new value
 *
 * Sets the root-level cmacs evaluation gate.  Emits `notify::
 * evaluate-gowl-config-with-cmacs` when the value changes.
 */
void
gowl_config_set_evaluate_gowl_config_with_cmacs(GowlConfig *self,
                                                 gboolean    value);

/**
 * gowl_config_get_evaluate_c_config_with_cmacs:
 * @self: a #GowlConfig
 *
 * Reads the C-config evaluation gate.  When %FALSE, cmacs `--gowl`
 * startup skips loading the user's C config entirely.  Independent
 * from #gowl_config_get_evaluate_gowl_config_with_cmacs so a user can
 * apply YAML but not C, or C but not YAML, or neither.  Standalone
 * and nested gowl ignore this flag.
 *
 * Returns: %TRUE if cmacs should load the C config
 */
gboolean
gowl_config_get_evaluate_c_config_with_cmacs(GowlConfig *self);

/**
 * gowl_config_set_evaluate_c_config_with_cmacs:
 * @self: a #GowlConfig
 * @value: new value
 *
 * Sets the C-config evaluation gate.  Emits `notify::
 * evaluate-c-config-with-cmacs` when the value changes.
 */
void
gowl_config_set_evaluate_c_config_with_cmacs(GowlConfig *self,
                                              gboolean    value);

/**
 * gowl_config_reset_values_to_defaults:
 * @self: a #GowlConfig
 *
 * Resets every config property except the two cmacs evaluation gates
 * (`evaluate-gowl-config-with-cmacs` and
 * `evaluate-c-config-with-cmacs`) back to its compile-time default.
 * Also clears keybinds, rules, dropdowns, and module configs.  Used
 * by cmacs `--gowl` when `evaluate-gowl-config-with-cmacs` is %FALSE
 * to discard YAML-loaded customisations while keeping the gates
 * themselves observable.
 */
void gowl_config_reset_values_to_defaults(GowlConfig *self);

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

/**
 * gowl_config_remove_keybind:
 * @self: a #GowlConfig
 * @modifiers: bitmask of #GowlKeyMod flags
 * @keysym: XKB keysym value
 *
 * Removes every keybind entry matching @modifiers + @keysym (action
 * and arg are not compared).  Returns the number removed.
 */
guint gowl_config_remove_keybind(GowlConfig *self,
                                  guint       modifiers,
                                  guint       keysym);

/**
 * gowl_config_clear_keybinds:
 * @self: a #GowlConfig
 *
 * Removes every keybind from the config.
 */
void gowl_config_clear_keybinds(GowlConfig *self);

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
 * gowl_config_add_rule_full:
 * @self: a #GowlConfig
 * @app_id: (nullable): app_id pattern or %NULL for any
 * @title: (nullable): title pattern or %NULL for any
 * @tags: tag bitmask to assign
 * @floating: whether the matched client should float
 * @monitor: target monitor index, or -1 for default
 * @width: explicit width in pixels for floated matches, or 0
 * @height: explicit height in pixels for floated matches, or 0
 * @center: center the client on its monitor when floating
 * @regex_mode: interpret @app_id and @title as PCRE regexes
 *
 * Appends a window rule entry with every tunable field exposed.
 * gowl_config_add_rule() is a thin wrapper that defaults the v2
 * fields to glob mode with no geometry override.
 */
void
gowl_config_add_rule_full(
	GowlConfig  *self,
	const gchar *app_id,
	const gchar *title,
	guint32      tags,
	gboolean     floating,
	gint         monitor,
	gint         width,
	gint         height,
	gboolean     center,
	gboolean     regex_mode
);

/**
 * gowl_config_remove_rule:
 * @self: a #GowlConfig
 * @app_id: (nullable): app_id pattern to match the rule by, or %NULL
 * @title: (nullable): title pattern to match the rule by, or %NULL
 *
 * Removes the first rule whose @app_id and @title strings match
 * the arguments exactly (both %NULL matches the first rule with
 * no app_id and no title).  Returns the number of rules removed
 * (0 or 1).
 */
guint
gowl_config_remove_rule(
	GowlConfig  *self,
	const gchar *app_id,
	const gchar *title
);

/**
 * gowl_config_clear_rules:
 * @self: a #GowlConfig
 *
 * Removes every rule from the config.  Useful when an elisp
 * customisation wants to fully replace the set of rules inherited
 * from YAML.
 */
void gowl_config_clear_rules (GowlConfig *self);

/* --- Dropdown management --- */

/**
 * gowl_config_add_dropdown:
 * @self: a #GowlConfig
 * @name: unique dropdown identifier
 * @spawn_cmd: command to spawn on first toggle
 * @keybind: (nullable): keybind string, or %NULL for elisp-only
 * @width_pct: fractional width (0.0 – 1.0), or 0 if using @width_abs
 * @height_pct: fractional height (0.0 – 1.0), or 0 if using @height_abs
 * @width_abs: absolute width in pixels, or 0 if using @width_pct
 * @height_abs: absolute height in pixels, or 0 if using @height_pct
 * @anchor: 0=top, 1=bottom, 2=left, 3=right
 *
 * Appends a dropdown entry to the config.  Used by YAML parsing
 * and runtime customisation from elisp.
 */
void
gowl_config_add_dropdown(
	GowlConfig  *self,
	const gchar *name,
	const gchar *spawn_cmd,
	const gchar *keybind,
	gdouble      width_pct,
	gdouble      height_pct,
	gint         width_abs,
	gint         height_abs,
	gint         anchor
);

/**
 * gowl_config_remove_dropdown:
 * @self: a #GowlConfig
 * @name: the dropdown name to remove
 *
 * Removes the dropdown entry whose @name matches exactly.
 *
 * Returns: 1 if removed, 0 if not found
 */
guint gowl_config_remove_dropdown (GowlConfig  *self,
                                    const gchar *name);

/**
 * gowl_config_get_dropdowns:
 * @self: a #GowlConfig
 *
 * Returns: (transfer none) (element-type GowlDropdownEntry): the
 *          dropdowns array.  Do not free.
 */
GPtrArray *gowl_config_get_dropdowns (GowlConfig *self);

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

/* --- Monitor Configuration --- */

/**
 * gowl_config_get_monitor_config:
 * @self: a #GowlConfig
 * @name: the output name (e.g. "eDP-1", "HDMI-A-1")
 *
 * Returns the per-output configuration parsed from the YAML
 * `monitors:` section, or %NULL if no entry exists for @name.
 * The returned pointer is owned by @self and remains valid until
 * the next YAML load or until @self is destroyed.
 *
 * Returns: (transfer none) (nullable): a #GowlMonitorConfig
 */
const GowlMonitorConfig *
gowl_config_get_monitor_config(GowlConfig  *self,
                                const gchar *name);

/**
 * gowl_config_get_monitor_names:
 * @self: a #GowlConfig
 *
 * Lists every output name that has an entry in the YAML
 * `monitors:` mapping.  Useful for reload-config code that
 * needs to re-apply configs to currently-attached outputs.
 *
 * Returns: (transfer container) (element-type utf8): a #GList of
 *          internal string pointers (owned by @self); the caller
 *          must g_list_free() the list itself but not the strings
 */
GList *gowl_config_get_monitor_names(GowlConfig *self);

G_END_DECLS

#endif /* GOWL_CONFIG_H */
