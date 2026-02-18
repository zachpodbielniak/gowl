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

#include "gowl-config.h"
#include "gowl-keybind.h"
#include "gowl-enums.h"
#include "gowl-types.h"

#include <glib.h>
#include <glib-object.h>
#include <string.h>

/* yaml-glib headers -- available via -Ideps/yaml-glib/src */
#include "yaml-glib.h"

/* --- Default values --- */
#define GOWL_CONFIG_DEFAULT_BORDER_WIDTH        (2)
#define GOWL_CONFIG_DEFAULT_BORDER_COLOR_FOCUS   "#bbbbbb"
#define GOWL_CONFIG_DEFAULT_BORDER_COLOR_UNFOCUS "#444444"
#define GOWL_CONFIG_DEFAULT_BORDER_COLOR_URGENT  "#ff0000"
#define GOWL_CONFIG_DEFAULT_MFACT               (0.55)
#define GOWL_CONFIG_DEFAULT_NMASTER             (1)
#define GOWL_CONFIG_DEFAULT_TAG_COUNT           (9)
#define GOWL_CONFIG_DEFAULT_REPEAT_RATE         (25)
#define GOWL_CONFIG_DEFAULT_REPEAT_DELAY        (600)
#define GOWL_CONFIG_DEFAULT_TERMINAL            "gst"
#define GOWL_CONFIG_DEFAULT_MENU                "bemenu-run"
#define GOWL_CONFIG_DEFAULT_SLOPPYFOCUS         (TRUE)
#define GOWL_CONFIG_DEFAULT_LOG_LEVEL           "warning"
#define GOWL_CONFIG_DEFAULT_LOG_FILE            "~/.config/gowl/gowl.log"

/* Configuration file name */
#define GOWL_CONFIG_FILENAME "gowl.yaml"

/* --- Instance struct --- */

struct _GowlConfig {
	GObject parent_instance;

	/* Appearance */
	gint     border_width;
	gchar   *border_color_focus;
	gchar   *border_color_unfocus;
	gchar   *border_color_urgent;

	/* Layout */
	gdouble  mfact;
	gint     nmaster;
	gint     tag_count;

	/* Input */
	gint     repeat_rate;
	gint     repeat_delay;
	gboolean sloppyfocus;

	/* Programs */
	gchar   *terminal;
	gchar   *menu;

	/* Logging */
	gchar   *log_level;
	gchar   *log_file;

	/* Keybinds - array of GowlKeybindEntry */
	GArray  *keybinds;

	/* Rules - array of GowlRuleEntry* (heap-allocated) */
	GPtrArray *rules;

	/* Module configs - maps module name (gchar*) to per-module
	 * GHashTable<gchar*, gchar*> of key-value settings parsed
	 * from the YAML modules section. */
	GHashTable *module_configs;
};

G_DEFINE_FINAL_TYPE(GowlConfig, gowl_config, G_TYPE_OBJECT)

/* --- Signal IDs --- */
enum {
	SIGNAL_CHANGED,
	SIGNAL_RELOADED,
	N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

/* --- GObject property storage --- */
static GParamSpec *properties[GOWL_CONFIG_PROP_LAST] = { NULL };

/* --- Helper: free a GowlRuleEntry --- */

/**
 * gowl_rule_entry_free:
 * @entry: a heap-allocated #GowlRuleEntry
 *
 * Frees all strings inside the rule entry and then the entry itself.
 */
static void
gowl_rule_entry_free(gpointer entry)
{
	GowlRuleEntry *r = (GowlRuleEntry *)entry;

	if (r == NULL)
		return;
	g_free(r->app_id);
	g_free(r->title);
	g_free(r);
}

/* --- Helper: free a GowlKeybindEntry (array element) --- */

/**
 * gowl_keybind_entry_clear:
 * @entry: pointer to a #GowlKeybindEntry stored in a GArray
 *
 * Frees the arg string inside the keybind entry.
 */
static void
gowl_keybind_entry_clear(gpointer entry)
{
	GowlKeybindEntry *kb = (GowlKeybindEntry *)entry;

	g_clear_pointer(&kb->arg, g_free);
}

/* --- GObject vfuncs --- */

/**
 * gowl_config_set_property:
 *
 * GObject set_property vfunc for #GowlConfig.
 * Sets each GObject property and emits the "changed" signal.
 */
static void
gowl_config_set_property(
	GObject      *object,
	guint         prop_id,
	const GValue *value,
	GParamSpec   *pspec
){
	GowlConfig *self = GOWL_CONFIG(object);

	switch ((GowlConfigProp)prop_id) {
	case GOWL_CONFIG_PROP_BORDER_WIDTH:
		self->border_width = g_value_get_int(value);
		break;
	case GOWL_CONFIG_PROP_BORDER_COLOR_FOCUS:
		g_free(self->border_color_focus);
		self->border_color_focus = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_BORDER_COLOR_UNFOCUS:
		g_free(self->border_color_unfocus);
		self->border_color_unfocus = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_BORDER_COLOR_URGENT:
		g_free(self->border_color_urgent);
		self->border_color_urgent = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_MFACT:
		self->mfact = g_value_get_double(value);
		break;
	case GOWL_CONFIG_PROP_NMASTER:
		self->nmaster = g_value_get_int(value);
		break;
	case GOWL_CONFIG_PROP_TAG_COUNT:
		self->tag_count = g_value_get_int(value);
		break;
	case GOWL_CONFIG_PROP_REPEAT_RATE:
		self->repeat_rate = g_value_get_int(value);
		break;
	case GOWL_CONFIG_PROP_REPEAT_DELAY:
		self->repeat_delay = g_value_get_int(value);
		break;
	case GOWL_CONFIG_PROP_TERMINAL:
		g_free(self->terminal);
		self->terminal = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_MENU:
		g_free(self->menu);
		self->menu = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_SLOPPYFOCUS:
		self->sloppyfocus = g_value_get_boolean(value);
		break;
	case GOWL_CONFIG_PROP_LOG_LEVEL:
		g_free(self->log_level);
		self->log_level = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_LOG_FILE:
		g_free(self->log_file);
		self->log_file = g_value_dup_string(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		return;
	}

	/* Emit "changed" signal with the property name */
	g_signal_emit(self, signals[SIGNAL_CHANGED], 0, pspec->name);
}

/**
 * gowl_config_get_property:
 *
 * GObject get_property vfunc for #GowlConfig.
 */
static void
gowl_config_get_property(
	GObject    *object,
	guint       prop_id,
	GValue     *value,
	GParamSpec *pspec
){
	GowlConfig *self = GOWL_CONFIG(object);

	switch ((GowlConfigProp)prop_id) {
	case GOWL_CONFIG_PROP_BORDER_WIDTH:
		g_value_set_int(value, self->border_width);
		break;
	case GOWL_CONFIG_PROP_BORDER_COLOR_FOCUS:
		g_value_set_string(value, self->border_color_focus);
		break;
	case GOWL_CONFIG_PROP_BORDER_COLOR_UNFOCUS:
		g_value_set_string(value, self->border_color_unfocus);
		break;
	case GOWL_CONFIG_PROP_BORDER_COLOR_URGENT:
		g_value_set_string(value, self->border_color_urgent);
		break;
	case GOWL_CONFIG_PROP_MFACT:
		g_value_set_double(value, self->mfact);
		break;
	case GOWL_CONFIG_PROP_NMASTER:
		g_value_set_int(value, self->nmaster);
		break;
	case GOWL_CONFIG_PROP_TAG_COUNT:
		g_value_set_int(value, self->tag_count);
		break;
	case GOWL_CONFIG_PROP_REPEAT_RATE:
		g_value_set_int(value, self->repeat_rate);
		break;
	case GOWL_CONFIG_PROP_REPEAT_DELAY:
		g_value_set_int(value, self->repeat_delay);
		break;
	case GOWL_CONFIG_PROP_TERMINAL:
		g_value_set_string(value, self->terminal);
		break;
	case GOWL_CONFIG_PROP_MENU:
		g_value_set_string(value, self->menu);
		break;
	case GOWL_CONFIG_PROP_SLOPPYFOCUS:
		g_value_set_boolean(value, self->sloppyfocus);
		break;
	case GOWL_CONFIG_PROP_LOG_LEVEL:
		g_value_set_string(value, self->log_level);
		break;
	case GOWL_CONFIG_PROP_LOG_FILE:
		g_value_set_string(value, self->log_file);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

/**
 * gowl_config_finalize:
 *
 * Releases all resources owned by the #GowlConfig instance.
 */
static void
gowl_config_finalize(GObject *object)
{
	GowlConfig *self = GOWL_CONFIG(object);

	g_free(self->border_color_focus);
	g_free(self->border_color_unfocus);
	g_free(self->border_color_urgent);
	g_free(self->terminal);
	g_free(self->menu);
	g_free(self->log_level);
	g_free(self->log_file);

	if (self->keybinds != NULL)
		g_array_unref(self->keybinds);
	if (self->rules != NULL)
		g_ptr_array_unref(self->rules);

	g_clear_pointer(&self->module_configs, g_hash_table_unref);

	G_OBJECT_CLASS(gowl_config_parent_class)->finalize(object);
}

/* --- Class init --- */

/**
 * gowl_config_class_init:
 * @klass: the #GowlConfigClass
 *
 * Installs GObject properties and signals on the #GowlConfig class.
 */
static void
gowl_config_class_init(GowlConfigClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->set_property = gowl_config_set_property;
	object_class->get_property = gowl_config_get_property;
	object_class->finalize     = gowl_config_finalize;

	/* --- Install properties --- */

	properties[GOWL_CONFIG_PROP_BORDER_WIDTH] =
		g_param_spec_int("border-width",
		                  "Border Width",
		                  "Window border width in pixels",
		                  0, 100,
		                  GOWL_CONFIG_DEFAULT_BORDER_WIDTH,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_BORDER_COLOR_FOCUS] =
		g_param_spec_string("border-color-focus",
		                     "Border Color Focus",
		                     "Hex colour for focused window border",
		                     GOWL_CONFIG_DEFAULT_BORDER_COLOR_FOCUS,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_BORDER_COLOR_UNFOCUS] =
		g_param_spec_string("border-color-unfocus",
		                     "Border Color Unfocus",
		                     "Hex colour for unfocused window border",
		                     GOWL_CONFIG_DEFAULT_BORDER_COLOR_UNFOCUS,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_BORDER_COLOR_URGENT] =
		g_param_spec_string("border-color-urgent",
		                     "Border Color Urgent",
		                     "Hex colour for urgent window border",
		                     GOWL_CONFIG_DEFAULT_BORDER_COLOR_URGENT,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_MFACT] =
		g_param_spec_double("mfact",
		                     "Master Factor",
		                     "Fraction of screen width for the master area",
		                     0.05, 0.95,
		                     GOWL_CONFIG_DEFAULT_MFACT,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_NMASTER] =
		g_param_spec_int("nmaster",
		                  "Number of Masters",
		                  "Number of windows in the master area",
		                  0, 100,
		                  GOWL_CONFIG_DEFAULT_NMASTER,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_TAG_COUNT] =
		g_param_spec_int("tag-count",
		                  "Tag Count",
		                  "Number of tag (workspace) slots",
		                  1, GOWL_MAX_TAGS,
		                  GOWL_CONFIG_DEFAULT_TAG_COUNT,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_REPEAT_RATE] =
		g_param_spec_int("repeat-rate",
		                  "Repeat Rate",
		                  "Keyboard repeat rate in keys per second",
		                  1, 1000,
		                  GOWL_CONFIG_DEFAULT_REPEAT_RATE,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_REPEAT_DELAY] =
		g_param_spec_int("repeat-delay",
		                  "Repeat Delay",
		                  "Keyboard repeat delay in milliseconds",
		                  1, 10000,
		                  GOWL_CONFIG_DEFAULT_REPEAT_DELAY,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_TERMINAL] =
		g_param_spec_string("terminal",
		                     "Terminal",
		                     "Default terminal emulator command",
		                     GOWL_CONFIG_DEFAULT_TERMINAL,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_MENU] =
		g_param_spec_string("menu",
		                     "Menu",
		                     "Application launcher / menu command",
		                     GOWL_CONFIG_DEFAULT_MENU,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_SLOPPYFOCUS] =
		g_param_spec_boolean("sloppyfocus",
		                      "Sloppy Focus",
		                      "Whether focus follows the mouse pointer",
		                      GOWL_CONFIG_DEFAULT_SLOPPYFOCUS,
		                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_LOG_LEVEL] =
		g_param_spec_string("log-level",
		                     "Log Level",
		                     "Logging verbosity (debug, info, warning, error)",
		                     GOWL_CONFIG_DEFAULT_LOG_LEVEL,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_LOG_FILE] =
		g_param_spec_string("log-file",
		                     "Log File",
		                     "Path to log file (\"stderr\" for stderr only)",
		                     GOWL_CONFIG_DEFAULT_LOG_FILE,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class,
	                                  GOWL_CONFIG_PROP_LAST,
	                                  properties);

	/* --- Install signals --- */

	/**
	 * GowlConfig::changed:
	 * @self: the #GowlConfig that changed
	 * @property_name: the name of the property that changed
	 *
	 * Emitted whenever a configuration property is modified.
	 */
	signals[SIGNAL_CHANGED] =
		g_signal_new("changed",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE, 1,
		             G_TYPE_STRING);

	/**
	 * GowlConfig::reloaded:
	 * @self: the #GowlConfig that was reloaded
	 *
	 * Emitted after a full configuration reload completes successfully.
	 */
	signals[SIGNAL_RELOADED] =
		g_signal_new("reloaded",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE, 0);
}

/* --- Instance init --- */

/**
 * gowl_config_init:
 * @self: the #GowlConfig instance being initialised
 *
 * Sets all fields to their default values and allocates the
 * keybind and rule arrays.
 */
static void
gowl_config_init(GowlConfig *self)
{
	self->border_width        = GOWL_CONFIG_DEFAULT_BORDER_WIDTH;
	self->border_color_focus  = g_strdup(GOWL_CONFIG_DEFAULT_BORDER_COLOR_FOCUS);
	self->border_color_unfocus = g_strdup(GOWL_CONFIG_DEFAULT_BORDER_COLOR_UNFOCUS);
	self->border_color_urgent = g_strdup(GOWL_CONFIG_DEFAULT_BORDER_COLOR_URGENT);
	self->mfact               = GOWL_CONFIG_DEFAULT_MFACT;
	self->nmaster             = GOWL_CONFIG_DEFAULT_NMASTER;
	self->tag_count           = GOWL_CONFIG_DEFAULT_TAG_COUNT;
	self->repeat_rate         = GOWL_CONFIG_DEFAULT_REPEAT_RATE;
	self->repeat_delay        = GOWL_CONFIG_DEFAULT_REPEAT_DELAY;
	self->sloppyfocus         = GOWL_CONFIG_DEFAULT_SLOPPYFOCUS;
	self->terminal            = g_strdup(GOWL_CONFIG_DEFAULT_TERMINAL);
	self->menu                = g_strdup(GOWL_CONFIG_DEFAULT_MENU);
	self->log_level           = g_strdup(GOWL_CONFIG_DEFAULT_LOG_LEVEL);
	self->log_file            = g_strdup(GOWL_CONFIG_DEFAULT_LOG_FILE);

	self->keybinds = g_array_new(FALSE, TRUE, sizeof(GowlKeybindEntry));
	g_array_set_clear_func(self->keybinds, gowl_keybind_entry_clear);

	self->rules = g_ptr_array_new_with_free_func(gowl_rule_entry_free);

	/* Module configs: outer table maps module name -> inner table,
	 * inner table maps setting key -> string value.
	 * Both keys and values are owned (g_free). */
	self->module_configs = g_hash_table_new_full(
		g_str_hash, g_str_equal,
		g_free, (GDestroyNotify)g_hash_table_unref);
}

/* --- Public API --- */

/**
 * gowl_config_new:
 *
 * Creates a new #GowlConfig populated with default values.
 *
 * Returns: (transfer full): a new #GowlConfig
 */
GowlConfig *
gowl_config_new(void)
{
	return (GowlConfig *)g_object_new(GOWL_TYPE_CONFIG, NULL);
}

/* --- YAML loading helpers --- */

/**
 * gowl_config_apply_mapping:
 * @self: a #GowlConfig
 * @mapping: a #YamlMapping containing top-level config keys
 *
 * Walks the YAML mapping and applies recognised keys to the
 * corresponding GObject properties. Unrecognised keys are logged
 * as warnings and skipped.
 */
static void
gowl_config_apply_mapping(
	GowlConfig  *self,
	YamlMapping *mapping
){
	/* Scalar properties: read from the mapping if present */
	if (yaml_mapping_has_member(mapping, "border-width")) {
		gint64 val = yaml_mapping_get_int_member(mapping, "border-width");
		g_object_set(self, "border-width", (gint)val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "border-color-focus")) {
		const gchar *val = yaml_mapping_get_string_member(mapping, "border-color-focus");
		if (val != NULL)
			g_object_set(self, "border-color-focus", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "border-color-unfocus")) {
		const gchar *val = yaml_mapping_get_string_member(mapping, "border-color-unfocus");
		if (val != NULL)
			g_object_set(self, "border-color-unfocus", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "border-color-urgent")) {
		const gchar *val = yaml_mapping_get_string_member(mapping, "border-color-urgent");
		if (val != NULL)
			g_object_set(self, "border-color-urgent", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "mfact")) {
		gdouble val = yaml_mapping_get_double_member(mapping, "mfact");
		g_object_set(self, "mfact", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "nmaster")) {
		gint64 val = yaml_mapping_get_int_member(mapping, "nmaster");
		g_object_set(self, "nmaster", (gint)val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "tag-count")) {
		gint64 val = yaml_mapping_get_int_member(mapping, "tag-count");
		g_object_set(self, "tag-count", (gint)val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "repeat-rate")) {
		gint64 val = yaml_mapping_get_int_member(mapping, "repeat-rate");
		g_object_set(self, "repeat-rate", (gint)val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "repeat-delay")) {
		gint64 val = yaml_mapping_get_int_member(mapping, "repeat-delay");
		g_object_set(self, "repeat-delay", (gint)val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "terminal")) {
		const gchar *val = yaml_mapping_get_string_member(mapping, "terminal");
		if (val != NULL)
			g_object_set(self, "terminal", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "menu")) {
		const gchar *val = yaml_mapping_get_string_member(mapping, "menu");
		if (val != NULL)
			g_object_set(self, "menu", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "sloppyfocus")) {
		gboolean val = yaml_mapping_get_boolean_member(mapping, "sloppyfocus");
		g_object_set(self, "sloppyfocus", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "log-level")) {
		const gchar *val = yaml_mapping_get_string_member(mapping, "log-level");
		if (val != NULL)
			g_object_set(self, "log-level", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "log-file")) {
		const gchar *val = yaml_mapping_get_string_member(mapping, "log-file");
		if (val != NULL)
			g_object_set(self, "log-file", val, NULL);
	}

	/* Keybinds: mapping of "Mod+Key": { action: <name>, arg: "<value>" }
	 *
	 * Example:
	 *   "Super+Return": { action: spawn, arg: "gst" }
	 *   "Super+Shift+q": { action: quit }
	 */
	if (yaml_mapping_has_member(mapping, "keybinds")) {
		YamlMapping *kb_mapping = yaml_mapping_get_mapping_member(
			mapping, "keybinds");
		if (kb_mapping != NULL) {
			guint kb_count = yaml_mapping_get_size(kb_mapping);
			guint i;

			for (i = 0; i < kb_count; i++) {
				const gchar *bind_str = yaml_mapping_get_key(kb_mapping, i);
				YamlNode *val_node = yaml_mapping_get_value(kb_mapping, i);
				YamlMapping *val_map;
				const gchar *action_str;
				const gchar *arg_str;
				guint mods;
				guint keysym;
				GEnumClass *action_class;
				GEnumValue *enum_val;
				gint action;

				if (bind_str == NULL || val_node == NULL)
					continue;

				val_map = yaml_node_get_mapping(val_node);
				if (val_map == NULL)
					continue;

				action_str = yaml_mapping_get_string_member(val_map, "action");
				if (action_str == NULL)
					continue;

				arg_str = NULL;
				if (yaml_mapping_has_member(val_map, "arg"))
					arg_str = yaml_mapping_get_string_member(val_map, "arg");

				/* Parse bind string into modifiers + keysym */
				mods = 0;
				keysym = 0;
				if (!gowl_keybind_parse(bind_str, &mods, &keysym)) {
					g_warning("gowl_config: failed to parse keybind '%s'",
					          bind_str);
					continue;
				}

				/* Resolve action name to GowlAction enum.
				 * Normalise underscores to hyphens so that both
				 * "kill_client" and "kill-client" map to the nick.
				 */
				action_class = (GEnumClass *)g_type_class_ref(
					gowl_action_get_type());
				{
					g_autofree gchar *norm = g_strdup(action_str);
					g_strdelimit(norm, "_", '-');
					enum_val = g_enum_get_value_by_nick(action_class, norm);
				}
				action = GOWL_ACTION_NONE;
				if (enum_val != NULL)
					action = enum_val->value;
				else
					g_warning("gowl_config: unknown action '%s'", action_str);
				g_type_class_unref(action_class);

				g_debug("gowl_config: keybind '%s' -> mods=0x%x sym=0x%x action=%d",
			        bind_str, mods, keysym, action);
			gowl_config_add_keybind(self, mods, keysym, action, arg_str);
			}
		}
	}

	/* Rules: expect a sequence of mappings with keys:
	 *   app-id: "firefox"          (optional)
	 *   title: ".*"                (optional)
	 *   tags: 2                    (optional, default 0)
	 *   floating: true             (optional, default false)
	 *   monitor: 0                 (optional, default -1)
	 */
	if (yaml_mapping_has_member(mapping, "rules")) {
		YamlSequence *seq = yaml_mapping_get_sequence_member(mapping, "rules");
		if (seq != NULL) {
			guint len = yaml_sequence_get_length(seq);
			guint i;

			for (i = 0; i < len; i++) {
				YamlMapping *rule_map = yaml_sequence_get_mapping_element(seq, i);
				if (rule_map == NULL)
					continue;

				const gchar *app_id = NULL;
				const gchar *title = NULL;
				guint32 tags = 0;
				gboolean floating = FALSE;
				gint monitor = -1;

				if (yaml_mapping_has_member(rule_map, "app-id"))
					app_id = yaml_mapping_get_string_member(rule_map, "app-id");
				if (yaml_mapping_has_member(rule_map, "title"))
					title = yaml_mapping_get_string_member(rule_map, "title");
				if (yaml_mapping_has_member(rule_map, "tags"))
					tags = (guint32)yaml_mapping_get_int_member(rule_map, "tags");
				if (yaml_mapping_has_member(rule_map, "floating"))
					floating = yaml_mapping_get_boolean_member(rule_map, "floating");
				if (yaml_mapping_has_member(rule_map, "monitor"))
					monitor = (gint)yaml_mapping_get_int_member(rule_map, "monitor");

				gowl_config_add_rule(self, app_id, title, tags, floating, monitor);
			}
		}
	}

	/* Modules: each top-level key is a module name, value is a mapping
	 * of setting keys to scalar values.  We flatten each module's
	 * mapping into a GHashTable<string, string> for generic consumption
	 * by the module's configure() method.
	 *
	 * Example YAML:
	 *   modules:
	 *     vanitygaps:
	 *       enabled: true
	 *       inner-h: 10
	 */
	if (yaml_mapping_has_member(mapping, "modules")) {
		YamlMapping *mod_mapping;

		mod_mapping = yaml_mapping_get_mapping_member(mapping, "modules");
		if (mod_mapping != NULL) {
			guint mod_count = yaml_mapping_get_size(mod_mapping);
			guint mi;

			/* Clear any previously-loaded module configs on reload */
			g_hash_table_remove_all(self->module_configs);

			for (mi = 0; mi < mod_count; mi++) {
				const gchar *mod_name;
				YamlNode *mod_val_node;
				YamlMapping *mod_cfg_map;
				GHashTable *settings;
				guint si, setting_count;

				mod_name = yaml_mapping_get_key(mod_mapping, mi);
				mod_val_node = yaml_mapping_get_value(mod_mapping, mi);
				if (mod_name == NULL || mod_val_node == NULL)
					continue;

				mod_cfg_map = yaml_node_get_mapping(mod_val_node);
				if (mod_cfg_map == NULL)
					continue;

				/* Build settings hash for this module */
				settings = g_hash_table_new_full(
					g_str_hash, g_str_equal, g_free, g_free);

				setting_count = yaml_mapping_get_size(mod_cfg_map);
				for (si = 0; si < setting_count; si++) {
					const gchar *key;
					YamlNode *val_node;
					const gchar *val_str;

					key = yaml_mapping_get_key(mod_cfg_map, si);
					val_node = yaml_mapping_get_value(mod_cfg_map, si);
					if (key == NULL || val_node == NULL)
						continue;

					/* Get the raw scalar string from the YAML node.
					 * For numbers and bools this is the text form
					 * (e.g. "5", "true"). */
					val_str = yaml_node_get_scalar(val_node);
					if (val_str != NULL) {
						g_hash_table_insert(settings,
						                    g_strdup(key),
						                    g_strdup(val_str));
					} else {
						/* Handle sequence values (e.g. commands list).
						 * Join elements with newline so modules can
						 * split them back via g_strsplit(). */
						YamlSequence *seq;

						seq = yaml_node_get_sequence(val_node);
						if (seq != NULL) {
							guint slen;
							guint si2;
							GString *joined;

							slen = yaml_sequence_get_length(seq);
							joined = g_string_new(NULL);
							for (si2 = 0; si2 < slen; si2++) {
								const gchar *elem;

								elem = yaml_sequence_get_string_element(
									seq, si2);
								if (elem == NULL)
									continue;
								if (joined->len > 0)
									g_string_append_c(joined, '\n');
								g_string_append(joined, elem);
							}
							g_hash_table_insert(settings,
							                    g_strdup(key),
							                    g_string_free(joined, FALSE));
						}
					}
				}

				g_debug("gowl_config: loaded %u settings for module '%s'",
				        g_hash_table_size(settings), mod_name);

				g_hash_table_insert(self->module_configs,
				                    g_strdup(mod_name),
				                    settings);
			}
		}
	}
}

/**
 * gowl_config_load_yaml:
 * @self: a #GowlConfig
 * @path: filesystem path to a YAML configuration file
 * @error: (nullable): return location for a #GError
 *
 * Parses the YAML file at @path using yaml-glib and applies the
 * top-level mapping to the config properties.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
gowl_config_load_yaml(
	GowlConfig   *self,
	const gchar  *path,
	GError      **error
){
	g_autoptr(YamlParser) parser = NULL;
	YamlNode *root = NULL;
	YamlMapping *mapping = NULL;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	parser = yaml_parser_new();

	if (!yaml_parser_load_from_file(parser, path, error))
		return FALSE;

	root = yaml_parser_get_root(parser);
	if (root == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		            "YAML file '%s' has no root node", path);
		return FALSE;
	}

	if (yaml_node_get_node_type(root) != YAML_NODE_MAPPING) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		            "YAML file '%s' root is not a mapping", path);
		return FALSE;
	}

	mapping = yaml_node_get_mapping(root);
	if (mapping == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		            "YAML file '%s' contains a null mapping", path);
		return FALSE;
	}

	/* check ignore_yaml: if true, discard everything and keep defaults */
	if (yaml_mapping_has_member(mapping, "ignore_yaml")) {
		if (yaml_mapping_get_boolean_member(mapping, "ignore_yaml")) {
			g_debug("gowl_config: ignore_yaml set, keeping defaults");
			return TRUE;
		}
	}

	gowl_config_apply_mapping(self, mapping);

	/* Emit the reloaded signal */
	g_signal_emit(self, signals[SIGNAL_RELOADED], 0);
	return TRUE;
}

/**
 * gowl_config_load_yaml_from_search_path:
 * @self: a #GowlConfig
 * @error: (nullable): return location for a #GError
 *
 * Searches standard directories for gowl.yaml and loads the first
 * one found. If no file exists, the config keeps its defaults and
 * the function returns %TRUE.
 *
 * Returns: %TRUE on success (including no-file-found), %FALSE on error
 */
gboolean
gowl_config_load_yaml_from_search_path(
	GowlConfig  *self,
	GError     **error
){
	g_autofree gchar *xdg_path = NULL;
	const gchar *search_paths[5];
	guint i;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);

	/* Build the XDG config path: ~/.config/gowl/gowl.yaml */
	xdg_path = g_build_filename(g_get_user_config_dir(),
	                             "gowl",
	                             GOWL_CONFIG_FILENAME,
	                             NULL);

	search_paths[0] = "data/" GOWL_CONFIG_FILENAME;
	search_paths[1] = xdg_path;
	search_paths[2] = GOWL_SYSCONFDIR "/gowl/" GOWL_CONFIG_FILENAME;
	search_paths[3] = GOWL_DATADIR "/gowl/" GOWL_CONFIG_FILENAME;
	search_paths[4] = NULL;

	for (i = 0; search_paths[i] != NULL; i++) {
		if (g_file_test(search_paths[i], G_FILE_TEST_EXISTS)) {
			g_debug("gowl_config: loading config from '%s'", search_paths[i]);
			return gowl_config_load_yaml(self, search_paths[i], error);
		}
	}

	g_debug("gowl_config: no config file found, using defaults");
	return TRUE;
}

/* --- YAML generation --- */

/**
 * gowl_config_generate_yaml:
 * @self: a #GowlConfig
 *
 * Builds a YAML representation of the current config state using
 * g_string_append_printf(). This is intentionally simple rather than
 * using the full yaml-glib generator, since the schema is known and fixed.
 *
 * Returns: (transfer full): a newly allocated YAML string
 */
gchar *
gowl_config_generate_yaml(GowlConfig *self)
{
	GString *yaml;
	guint i;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);

	yaml = g_string_new("# gowl configuration\n\n");

	/* Appearance */
	g_string_append_printf(yaml, "border-width: %d\n", self->border_width);
	g_string_append_printf(yaml, "border-color-focus: \"%s\"\n", self->border_color_focus);
	g_string_append_printf(yaml, "border-color-unfocus: \"%s\"\n", self->border_color_unfocus);
	g_string_append_printf(yaml, "border-color-urgent: \"%s\"\n", self->border_color_urgent);

	/* Layout */
	g_string_append_printf(yaml, "mfact: %.2f\n", self->mfact);
	g_string_append_printf(yaml, "nmaster: %d\n", self->nmaster);
	g_string_append_printf(yaml, "tag-count: %d\n", self->tag_count);

	/* Input */
	g_string_append_printf(yaml, "repeat-rate: %d\n", self->repeat_rate);
	g_string_append_printf(yaml, "repeat-delay: %d\n", self->repeat_delay);
	g_string_append_printf(yaml, "sloppyfocus: %s\n", self->sloppyfocus ? "true" : "false");

	/* Programs */
	g_string_append_printf(yaml, "terminal: \"%s\"\n", self->terminal);
	g_string_append_printf(yaml, "menu: \"%s\"\n", self->menu);

	/* Logging */
	g_string_append_printf(yaml, "log-level: \"%s\"\n", self->log_level);
	g_string_append_printf(yaml, "log-file: \"%s\"\n", self->log_file);

	/* Keybinds */
	if (self->keybinds->len > 0) {
		g_string_append(yaml, "\nkeybinds:\n");
		for (i = 0; i < self->keybinds->len; i++) {
			GowlKeybindEntry *kb = &g_array_index(self->keybinds, GowlKeybindEntry, i);
			g_autofree gchar *bind_str = gowl_keybind_to_string(kb->modifiers, kb->keysym);

			/* Resolve action enum value to nick */
			GEnumClass *action_class = (GEnumClass *)g_type_class_ref(
				gowl_action_get_type());
			GEnumValue *enum_val = g_enum_get_value(action_class, kb->action);
			const gchar *action_nick = (enum_val != NULL) ? enum_val->value_nick : "none";

			g_string_append_printf(yaml, "  - bind: \"%s\"\n", bind_str);
			g_string_append_printf(yaml, "    action: \"%s\"\n", action_nick);
			if (kb->arg != NULL)
				g_string_append_printf(yaml, "    arg: \"%s\"\n", kb->arg);

			g_type_class_unref(action_class);
		}
	}

	/* Rules */
	if (self->rules->len > 0) {
		g_string_append(yaml, "\nrules:\n");
		for (i = 0; i < self->rules->len; i++) {
			GowlRuleEntry *rule = (GowlRuleEntry *)g_ptr_array_index(self->rules, i);

			g_string_append(yaml, "  - ");
			if (rule->app_id != NULL)
				g_string_append_printf(yaml, "app-id: \"%s\"\n    ", rule->app_id);
			if (rule->title != NULL)
				g_string_append_printf(yaml, "title: \"%s\"\n    ", rule->title);
			g_string_append_printf(yaml, "tags: %u\n    ", (guint)rule->tags);
			g_string_append_printf(yaml, "floating: %s\n    ", rule->floating ? "true" : "false");
			g_string_append_printf(yaml, "monitor: %d\n", rule->monitor);
		}
	}

	return g_string_free(yaml, FALSE);
}

/* --- Property getters --- */

gint
gowl_config_get_border_width(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_BORDER_WIDTH);
	return self->border_width;
}

const gchar *
gowl_config_get_border_color_focus(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_BORDER_COLOR_FOCUS);
	return self->border_color_focus;
}

const gchar *
gowl_config_get_border_color_unfocus(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_BORDER_COLOR_UNFOCUS);
	return self->border_color_unfocus;
}

const gchar *
gowl_config_get_border_color_urgent(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_BORDER_COLOR_URGENT);
	return self->border_color_urgent;
}

gdouble
gowl_config_get_mfact(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_MFACT);
	return self->mfact;
}

gint
gowl_config_get_nmaster(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_NMASTER);
	return self->nmaster;
}

gint
gowl_config_get_tag_count(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_TAG_COUNT);
	return self->tag_count;
}

gint
gowl_config_get_repeat_rate(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_REPEAT_RATE);
	return self->repeat_rate;
}

gint
gowl_config_get_repeat_delay(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_REPEAT_DELAY);
	return self->repeat_delay;
}

const gchar *
gowl_config_get_terminal(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_TERMINAL);
	return self->terminal;
}

const gchar *
gowl_config_get_menu(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_MENU);
	return self->menu;
}

gboolean
gowl_config_get_sloppyfocus(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_SLOPPYFOCUS);
	return self->sloppyfocus;
}

const gchar *
gowl_config_get_log_level(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_LOG_LEVEL);
	return self->log_level;
}

const gchar *
gowl_config_get_log_file(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_LOG_FILE);
	return self->log_file;
}

/* --- Keybind management --- */

/**
 * gowl_config_add_keybind:
 * @self: a #GowlConfig
 * @modifiers: bitmask of #GowlKeyMod flags
 * @keysym: XKB keysym value
 * @action: a #GowlAction value
 * @arg: (nullable): optional argument string (will be copied)
 *
 * Appends a keybind entry to the internal keybind array.
 */
void
gowl_config_add_keybind(
	GowlConfig  *self,
	guint        modifiers,
	guint        keysym,
	gint         action,
	const gchar *arg
){
	GowlKeybindEntry entry;

	g_return_if_fail(GOWL_IS_CONFIG(self));

	entry.modifiers = modifiers;
	entry.keysym    = keysym;
	entry.action    = action;
	entry.arg       = g_strdup(arg);

	g_array_append_val(self->keybinds, entry);
}

/**
 * gowl_config_get_keybinds:
 * @self: a #GowlConfig
 *
 * Returns: (transfer none): the keybind GArray (element-type GowlKeybindEntry)
 */
GArray *
gowl_config_get_keybinds(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->keybinds;
}

/* --- Rule management --- */

/**
 * gowl_config_add_rule:
 * @self: a #GowlConfig
 * @app_id: (nullable): app_id pattern or %NULL
 * @title: (nullable): title pattern or %NULL
 * @tags: tag bitmask
 * @floating: whether to float
 * @monitor: target monitor index or -1
 *
 * Allocates a new #GowlRuleEntry, copies the strings, and appends
 * it to the rules array.
 */
void
gowl_config_add_rule(
	GowlConfig  *self,
	const gchar *app_id,
	const gchar *title,
	guint32      tags,
	gboolean     floating,
	gint         monitor
){
	GowlRuleEntry *rule;

	g_return_if_fail(GOWL_IS_CONFIG(self));

	rule = g_new0(GowlRuleEntry, 1);
	rule->app_id   = g_strdup(app_id);
	rule->title    = g_strdup(title);
	rule->tags     = tags;
	rule->floating = floating;
	rule->monitor  = monitor;

	g_ptr_array_add(self->rules, rule);
}

/**
 * gowl_config_get_rules:
 * @self: a #GowlConfig
 *
 * Returns: (transfer none): the rules GPtrArray (element-type GowlRuleEntry)
 */
GPtrArray *
gowl_config_get_rules(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->rules;
}

/**
 * gowl_config_get_module_config:
 * @self: a #GowlConfig
 * @module_name: the name of the module (e.g. "vanitygaps")
 *
 * Returns the per-module settings parsed from the YAML config's
 * `modules:` section.  The returned hash table maps setting keys
 * (e.g. "inner-h") to string values (e.g. "10").  Callers must
 * convert to the appropriate type.
 *
 * Returns: (transfer none) (nullable): a #GHashTable of string
 *          key-value pairs, or %NULL if no config for @module_name
 */
GHashTable *
gowl_config_get_module_config(
	GowlConfig  *self,
	const gchar *module_name
){
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	g_return_val_if_fail(module_name != NULL, NULL);

	return (GHashTable *)g_hash_table_lookup(
		self->module_configs, module_name);
}

/**
 * gowl_config_get_all_module_configs:
 * @self: a #GowlConfig
 *
 * Returns the entire module configuration table.  The outer hash
 * maps module names (strings) to inner #GHashTable objects of
 * string key-value settings.
 *
 * Returns: (transfer none) (nullable): the module configs hash table
 */
GHashTable *
gowl_config_get_all_module_configs(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->module_configs;
}
