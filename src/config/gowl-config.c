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
#include "boxed/gowl-palette.h"
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
/*
 * The defaults are palette names, not literals.  A hex default
 * would be a fourth independent palette that no theme change can
 * reach --- which is exactly the state this replaced.
 */
#define GOWL_CONFIG_DEFAULT_BORDER_COLOR_FOCUS   "accent"
#define GOWL_CONFIG_DEFAULT_BORDER_COLOR_UNFOCUS "surface"
#define GOWL_CONFIG_DEFAULT_BORDER_COLOR_URGENT  "red"
#define GOWL_CONFIG_DEFAULT_PALETTE              "mocha"
#define GOWL_CONFIG_DEFAULT_MFACT               (0.55)
/* Two columns visible at a time, matching Omarchy's Hyprland default. */
#define GOWL_CONFIG_DEFAULT_SCROLL_COLUMN_WIDTH (0.5)
/* Layout motion settles quickly; entrances get their own longer beat. */
#define GOWL_CONFIG_DEFAULT_ANIMATION_DURATION  (260)
/*
 * -1 means "use animation-duration".  Opening is an arrival rather
 * than a correction, so it can afford a longer beat than a re-tile.
 */
#define GOWL_CONFIG_DEFAULT_ANIMATION_DURATION_OPEN (360)
/* Shorter than either: a closed window is finished, and holding its
 * ghost on screen is holding up the re-tile behind it. */
#define GOWL_CONFIG_DEFAULT_ANIMATION_DURATION_CLOSE (180)
#define GOWL_CONFIG_DEFAULT_ANIMATION_CURVE     "ease-out-expo"
#define GOWL_CONFIG_DEFAULT_ANIMATION_CURVE_OPEN "ease-out-back"
#define GOWL_CONFIG_DEFAULT_ANIMATION_POPIN_SCALE (0.84)
#define GOWL_CONFIG_DEFAULT_NMASTER             (1)
#define GOWL_CONFIG_DEFAULT_TAG_COUNT           (9)
#define GOWL_CONFIG_DEFAULT_REPEAT_RATE         (25)
#define GOWL_CONFIG_DEFAULT_REPEAT_DELAY        (600)
#define GOWL_CONFIG_DEFAULT_TERMINAL            "gst"
#define GOWL_CONFIG_DEFAULT_MENU                "bemenu-run"
#define GOWL_CONFIG_DEFAULT_SLOPPYFOCUS         (TRUE)
#define GOWL_CONFIG_DEFAULT_MANAGE_LID          (TRUE)
#define GOWL_CONFIG_DEFAULT_INPUT_RECORDING     (FALSE)
#define GOWL_CONFIG_DEFAULT_INPUT_RECORDING_DENY_APPS ""
#define GOWL_CONFIG_DEFAULT_LOG_LEVEL           "warning"
#define GOWL_CONFIG_DEFAULT_LOG_FILE            "~/.config/gowl/gowl.log"
#define GOWL_CONFIG_DEFAULT_EVALUATE_GOWL_CONFIG_WITH_CMACS  (TRUE)
#define GOWL_CONFIG_DEFAULT_EVALUATE_C_CONFIG_WITH_CMACS     (TRUE)

/* Configuration file name */
#define GOWL_CONFIG_FILENAME "config.yaml"

static void gowl_config_reresolve_colors(GowlConfig *self);

/* --- Instance struct --- */

struct _GowlConfig {
	GObject parent_instance;

	/* Appearance */
	gint     border_width;
	gchar   *border_color_focus;
	gchar   *border_color_unfocus;
	gchar   *border_color_urgent;

	/*
	 * Colour specs above are stored exactly as written, so that
	 * to_yaml round-trips a palette reference rather than baking it
	 * into a literal the next theme change cannot reach.  The
	 * resolved forms are shadows, recomputed whenever either the
	 * spec or the palette changes, because every consumer wants a
	 * hex string and none of them should have to know about
	 * palettes.
	 */
	gchar   *border_hex_focus;
	gchar   *border_hex_unfocus;
	gchar   *border_hex_urgent;

	/* Palette */
	GowlPalette *palette;             /* effective, after merging */
	GowlPalette *palette_override;    /* pushed in at runtime */
	gchar       *palette_name;

	/* Layout */
	gint     animation_duration_open;
	gint     animation_duration_close;
	gdouble  mfact;
	gdouble  scroll_column_width;
	gboolean animations;
	gint     animation_duration;
	gchar   *animation_curve;
	gchar   *animation_curve_open;
	gdouble  animation_popin_scale;
	gdouble  animation_jiggle_strength;
	gint     nmaster;
	gint     tag_count;

	/* Input */
	gint     repeat_rate;
	gint     repeat_delay;
	gboolean sloppyfocus;
	gboolean manage_lid;
	gboolean input_recording;
	gchar   *input_recording_deny_apps;

	/* Programs */
	gchar   *terminal;
	gchar   *menu;

	/* Logging */
	gchar   *log_level;
	gchar   *log_file;

	/* cmacs evaluation gates (root-level in YAML / C config).
	 * Only consulted by cmacs `--gowl` startup logic; gowl's
	 * standalone main.c never reads these fields. */
	gboolean evaluate_gowl_config_with_cmacs;
	gboolean evaluate_c_config_with_cmacs;

	/* Keybinds - array of GowlKeybindEntry */
	GArray  *keybinds;

	/* Rules - array of GowlRuleEntry* (heap-allocated) */
	GPtrArray *rules;

	/* Dropdowns - array of GowlDropdownEntry* (heap-allocated) */
	GPtrArray *dropdowns;

	/* Module configs - maps module name (gchar*) to per-module
	 * GHashTable<gchar*, gchar*> of key-value settings parsed
	 * from the YAML modules section. */
	GHashTable *module_configs;

	/* Monitor configs - maps output name (gchar*) to a heap-allocated
	 * GowlMonitorConfig* parsed from the YAML monitors: section.
	 * Each field is independently optional (sentinel-driven). */
	GHashTable *monitor_configs;
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

/**
 * gowl_dropdown_entry_free:
 * @entry: a heap-allocated #GowlDropdownEntry
 *
 * Frees the strings owned by the entry and then the entry.
 */
static void
gowl_dropdown_entry_free(gpointer entry)
{
	GowlDropdownEntry *d = (GowlDropdownEntry *)entry;

	if (d == NULL)
		return;
	g_free(d->name);
	g_free(d->spawn_cmd);
	g_free(d->keybind);
	g_free(d);
}

/* --- Helper: escape a string for a YAML double-quoted scalar --- */

/**
 * gowl_config_escape_yaml:
 * @str: (nullable): the string to escape
 *
 * Escapes @str for inclusion inside a YAML double-quoted scalar.
 * A double-quoted scalar uses JSON-style backslash escapes, so a
 * literal backslash or quote in a spawn command (a regex in a rule,
 * a shell argument) must be doubled or the emitted document does not
 * parse.  Control characters are escaped as \xNN.
 *
 * Deliberately NOT g_strescape(): that emits octal escapes for bytes
 * >= 0x80, which YAML does not define, so any non-ASCII description
 * would come back as literal backslash-digits.  UTF-8 is passed
 * through untouched instead.
 *
 * Returns: (transfer full): a newly allocated escaped string
 */
static gchar *
gowl_config_escape_yaml(const gchar *str)
{
	GString *out;
	const gchar *p;

	if (str == NULL)
		return g_strdup("");

	out = g_string_sized_new(strlen(str) + 8);

	for (p = str; *p != '\0'; p++) {
		guchar c = (guchar)*p;

		switch (c) {
		case '"':  g_string_append(out, "\\\""); break;
		case '\\': g_string_append(out, "\\\\"); break;
		case '\n': g_string_append(out, "\\n");  break;
		case '\r': g_string_append(out, "\\r");  break;
		case '\t': g_string_append(out, "\\t");  break;
		default:
			/* Escape the remaining C0 controls; pass every other
			 * byte (UTF-8 continuation bytes included) through. */
			if (c < 0x20 || c == 0x7f)
				g_string_append_printf(out, "\\x%02x", c);
			else
				g_string_append_c(out, (gchar)c);
			break;
		}
	}

	return g_string_free(out, FALSE);
}

/* --- Helper: free a GowlKeybindEntry (array element) --- */

/**
 * gowl_keybind_entry_clear:
 * @entry: pointer to a #GowlKeybindEntry stored in a GArray
 *
 * Frees the owned strings inside the keybind entry.
 */
static void
gowl_keybind_entry_clear(gpointer entry)
{
	GowlKeybindEntry *kb = (GowlKeybindEntry *)entry;

	g_clear_pointer(&kb->arg, g_free);
	g_clear_pointer(&kb->desc, g_free);
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
		gowl_config_reresolve_colors(self);
		break;
	case GOWL_CONFIG_PROP_BORDER_COLOR_UNFOCUS:
		g_free(self->border_color_unfocus);
		self->border_color_unfocus = g_value_dup_string(value);
		gowl_config_reresolve_colors(self);
		break;
	case GOWL_CONFIG_PROP_BORDER_COLOR_URGENT:
		g_free(self->border_color_urgent);
		self->border_color_urgent = g_value_dup_string(value);
		gowl_config_reresolve_colors(self);
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
	case GOWL_CONFIG_PROP_MANAGE_LID:
		self->manage_lid = g_value_get_boolean(value);
		break;
	case GOWL_CONFIG_PROP_INPUT_RECORDING:
		self->input_recording = g_value_get_boolean(value);
		break;
	case GOWL_CONFIG_PROP_INPUT_RECORDING_DENY_APPS:
		g_free(self->input_recording_deny_apps);
		self->input_recording_deny_apps = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_LOG_LEVEL:
		g_free(self->log_level);
		self->log_level = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_LOG_FILE:
		g_free(self->log_file);
		self->log_file = g_value_dup_string(value);
		break;
	case GOWL_CONFIG_PROP_EVALUATE_GOWL_CONFIG_WITH_CMACS:
		self->evaluate_gowl_config_with_cmacs = g_value_get_boolean(value);
		break;
	case GOWL_CONFIG_PROP_EVALUATE_C_CONFIG_WITH_CMACS:
		self->evaluate_c_config_with_cmacs = g_value_get_boolean(value);
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
	case GOWL_CONFIG_PROP_MANAGE_LID:
		g_value_set_boolean(value, self->manage_lid);
		break;
	case GOWL_CONFIG_PROP_INPUT_RECORDING:
		g_value_set_boolean(value, self->input_recording);
		break;
	case GOWL_CONFIG_PROP_INPUT_RECORDING_DENY_APPS:
		g_value_set_string(value, self->input_recording_deny_apps);
		break;
	case GOWL_CONFIG_PROP_LOG_LEVEL:
		g_value_set_string(value, self->log_level);
		break;
	case GOWL_CONFIG_PROP_LOG_FILE:
		g_value_set_string(value, self->log_file);
		break;
	case GOWL_CONFIG_PROP_EVALUATE_GOWL_CONFIG_WITH_CMACS:
		g_value_set_boolean(value, self->evaluate_gowl_config_with_cmacs);
		break;
	case GOWL_CONFIG_PROP_EVALUATE_C_CONFIG_WITH_CMACS:
		g_value_set_boolean(value, self->evaluate_c_config_with_cmacs);
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
/*
 * Recomputes every resolved colour shadow from its spec.  Called after
 * anything that can change a spec or the palette; cheap enough (three
 * hash lookups) that no attempt is made to work out which ones
 * actually changed.
 */
static void
gowl_config_reresolve_colors(GowlConfig *self)
{
	g_free(self->border_hex_focus);
	g_free(self->border_hex_unfocus);
	g_free(self->border_hex_urgent);

	self->border_hex_focus = gowl_palette_resolve(self->palette,
	                                              self->border_color_focus);
	self->border_hex_unfocus = gowl_palette_resolve(self->palette,
	                                                self->border_color_unfocus);
	self->border_hex_urgent = gowl_palette_resolve(self->palette,
	                                               self->border_color_urgent);
}

/*
 * Rebuilds the effective palette: the named built-in, then whatever the
 * config file defined on top of it, then whatever was pushed in at
 * runtime.  Layered in that order so a reload cannot discard a palette
 * an editor theme pushed in --- the file is the base, the override is
 * the last word.
 */
static void
gowl_config_rebuild_palette(GowlConfig *self, const GowlPalette *from_file)
{
	g_clear_pointer(&self->palette, gowl_palette_free);
	self->palette = gowl_palette_new_builtin(self->palette_name);
	gowl_palette_merge(self->palette, from_file);
	gowl_palette_merge(self->palette, self->palette_override);
	gowl_config_reresolve_colors(self);
}

static void
gowl_config_finalize(GObject *object)
{
	GowlConfig *self = GOWL_CONFIG(object);

	g_free(self->border_color_focus);
	g_free(self->border_color_unfocus);
	g_free(self->border_color_urgent);
	g_free(self->border_hex_focus);
	g_free(self->border_hex_unfocus);
	g_free(self->border_hex_urgent);
	g_free(self->palette_name);
	g_clear_pointer(&self->palette, gowl_palette_free);
	g_clear_pointer(&self->palette_override, gowl_palette_free);
	g_free(self->terminal);
	g_free(self->menu);
	g_free(self->log_level);
	g_free(self->log_file);
	g_free(self->input_recording_deny_apps);
	g_free(self->animation_curve);
	g_free(self->animation_curve_open);

	if (self->keybinds != NULL)
		g_array_unref(self->keybinds);
	if (self->rules != NULL)
		g_ptr_array_unref(self->rules);
	if (self->dropdowns != NULL)
		g_ptr_array_unref(self->dropdowns);

	g_clear_pointer(&self->module_configs, g_hash_table_unref);
	g_clear_pointer(&self->monitor_configs, g_hash_table_unref);

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

	properties[GOWL_CONFIG_PROP_MANAGE_LID] =
		g_param_spec_boolean("manage-lid",
		                      "Manage Lid",
		                      "Power off internal panels when the laptop "
		                      "lid is shut and an external display is present",
		                      GOWL_CONFIG_DEFAULT_MANAGE_LID,
		                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_INPUT_RECORDING] =
		g_param_spec_boolean("input-recording",
		                      "Input Recording",
		                      "Allow a recording of real key and pointer "
		                      "input to be started.  Separate from -- and "
		                      "never implied by -- the tools that inject "
		                      "input, because capturing what somebody "
		                      "types is a different permission from "
		                      "clicking on their behalf.",
		                      GOWL_CONFIG_DEFAULT_INPUT_RECORDING,
		                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_INPUT_RECORDING_DENY_APPS] =
		g_param_spec_string("input-recording-deny-apps",
		                     "Input Recording Deny Apps",
		                     "Comma-separated glob patterns.  Input is "
		                     "not recorded while the focused window's "
		                     "app-id or title matches one.  Added to the "
		                     "built-in list of credential prompts, never "
		                     "replacing it.",
		                     GOWL_CONFIG_DEFAULT_INPUT_RECORDING_DENY_APPS,
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

	properties[GOWL_CONFIG_PROP_EVALUATE_GOWL_CONFIG_WITH_CMACS] =
		g_param_spec_boolean("evaluate-gowl-config-with-cmacs",
		                      "Evaluate Gowl Config With Cmacs",
		                      "When FALSE, cmacs `--gowl` resets all "
		                      "other config values to defaults after "
		                      "parsing.  Ignored by standalone gowl.",
		                      GOWL_CONFIG_DEFAULT_EVALUATE_GOWL_CONFIG_WITH_CMACS,
		                      G_PARAM_READWRITE
		                      | G_PARAM_EXPLICIT_NOTIFY
		                      | G_PARAM_STATIC_STRINGS);

	properties[GOWL_CONFIG_PROP_EVALUATE_C_CONFIG_WITH_CMACS] =
		g_param_spec_boolean("evaluate-c-config-with-cmacs",
		                      "Evaluate C Config With Cmacs",
		                      "When FALSE, cmacs `--gowl` skips loading "
		                      "the user's C config entirely.  Ignored "
		                      "by standalone gowl.",
		                      GOWL_CONFIG_DEFAULT_EVALUATE_C_CONFIG_WITH_CMACS,
		                      G_PARAM_READWRITE
		                      | G_PARAM_EXPLICIT_NOTIFY
		                      | G_PARAM_STATIC_STRINGS);

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
	self->palette_name        = g_strdup(GOWL_CONFIG_DEFAULT_PALETTE);
	self->palette             = gowl_palette_new_builtin(self->palette_name);
	self->palette_override    = gowl_palette_new();
	gowl_config_reresolve_colors(self);
	self->mfact               = GOWL_CONFIG_DEFAULT_MFACT;
	self->scroll_column_width = GOWL_CONFIG_DEFAULT_SCROLL_COLUMN_WIDTH;
	self->animations          = TRUE;
	self->animation_duration  = GOWL_CONFIG_DEFAULT_ANIMATION_DURATION;
	self->animation_duration_open =
		GOWL_CONFIG_DEFAULT_ANIMATION_DURATION_OPEN;
	self->animation_duration_close =
		GOWL_CONFIG_DEFAULT_ANIMATION_DURATION_CLOSE;
	self->animation_curve     = g_strdup(GOWL_CONFIG_DEFAULT_ANIMATION_CURVE);
	self->animation_curve_open = g_strdup(GOWL_CONFIG_DEFAULT_ANIMATION_CURVE_OPEN);
	self->animation_popin_scale = GOWL_CONFIG_DEFAULT_ANIMATION_POPIN_SCALE;
	self->animation_jiggle_strength = 1.0;
	self->nmaster             = GOWL_CONFIG_DEFAULT_NMASTER;
	self->tag_count           = GOWL_CONFIG_DEFAULT_TAG_COUNT;
	self->repeat_rate         = GOWL_CONFIG_DEFAULT_REPEAT_RATE;
	self->repeat_delay        = GOWL_CONFIG_DEFAULT_REPEAT_DELAY;
	self->sloppyfocus         = GOWL_CONFIG_DEFAULT_SLOPPYFOCUS;
	self->manage_lid          = GOWL_CONFIG_DEFAULT_MANAGE_LID;
	self->input_recording     = GOWL_CONFIG_DEFAULT_INPUT_RECORDING;
	self->input_recording_deny_apps =
		g_strdup(GOWL_CONFIG_DEFAULT_INPUT_RECORDING_DENY_APPS);
	self->terminal            = g_strdup(GOWL_CONFIG_DEFAULT_TERMINAL);
	self->menu                = g_strdup(GOWL_CONFIG_DEFAULT_MENU);
	self->log_level           = g_strdup(GOWL_CONFIG_DEFAULT_LOG_LEVEL);
	self->log_file            = g_strdup(GOWL_CONFIG_DEFAULT_LOG_FILE);
	self->evaluate_gowl_config_with_cmacs =
		GOWL_CONFIG_DEFAULT_EVALUATE_GOWL_CONFIG_WITH_CMACS;
	self->evaluate_c_config_with_cmacs =
		GOWL_CONFIG_DEFAULT_EVALUATE_C_CONFIG_WITH_CMACS;

	self->keybinds = g_array_new(FALSE, TRUE, sizeof(GowlKeybindEntry));
	g_array_set_clear_func(self->keybinds, gowl_keybind_entry_clear);

	self->rules = g_ptr_array_new_with_free_func(gowl_rule_entry_free);
	self->dropdowns = g_ptr_array_new_with_free_func(
		gowl_dropdown_entry_free);

	/* Module configs: outer table maps module name -> inner table,
	 * inner table maps setting key -> string value.
	 * Both keys and values are owned (g_free). */
	self->module_configs = g_hash_table_new_full(
		g_str_hash, g_str_equal,
		g_free, (GDestroyNotify)g_hash_table_unref);

	/* Monitor configs: maps output name -> heap-allocated
	 * GowlMonitorConfig*.  Values are plain structs with no inner
	 * pointers, so g_free is sufficient as the destroy func. */
	self->monitor_configs = g_hash_table_new_full(
		g_str_hash, g_str_equal, g_free, g_free);
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

/* Transform names indexed by wl_output_transform value.  Matches
 * the dictionary used by cmacs's eval-dispatch transform_names[],
 * so the YAML schema, the JSON emitted by cmacs-gowl-list-monitors,
 * and the symbols accepted by `(gowl-set-monitor-transform)` are all
 * symmetric. */
static const gchar *const gowl_monitor_transform_names[] = {
	"normal",       /* 0 */
	"90",           /* 1 */
	"180",          /* 2 */
	"270",          /* 3 */
	"flipped",      /* 4 */
	"flipped-90",   /* 5 */
	"flipped-180",  /* 6 */
	"flipped-270"   /* 7 */
};

/**
 * gowl_parse_monitor_transform:
 * @cm: a #YamlMapping describing one monitor's config
 *
 * Reads the `transform:` member and returns its
 * wl_output_transform code (0..7).  Accepts either an integer
 * 0..7 (`transform: 3`) or one of the canonical names listed
 * above (`transform: flipped-270`).  Names that happen to look
 * like integers (e.g. `90`, `180`, `270`) are accepted via the
 * name table when the numeric parse is out of range.  Returns -1
 * and warns on miss or unparseable input -- the field is optional.
 */
static gint
gowl_parse_monitor_transform(YamlMapping *cm)
{
	const gchar *raw;
	gchar *end;
	gint64 num;
	gsize i;

	raw = yaml_mapping_get_string_member(cm, "transform");
	if (raw == NULL)
		return -1;

	/* Try integer 0..7 first -- the most common case for users
	 * who learn the codes from wl_output_transform docs. */
	end = NULL;
	num = g_ascii_strtoll(raw, &end, 10);
	if (end != raw && *end == '\0' && num >= 0 && num <= 7)
		return (gint)num;

	/* Fall through to the name table.  This also catches
	 * "90"/"180"/"270" -- those are canonical *names* (degrees of
	 * rotation), not transform codes, but users write them
	 * intuitively and the table maps them to the right codes. */
	for (i = 0; i < G_N_ELEMENTS(gowl_monitor_transform_names); i++) {
		if (g_strcmp0(raw, gowl_monitor_transform_names[i]) == 0)
			return (gint)i;
	}

	g_warning("gowl_config: invalid transform '%s' (expected 0..7 "
	          "or one of: normal, 90, 180, 270, flipped, flipped-90, "
	          "flipped-180, flipped-270)", raw);
	return -1;
}

/*
 * Reads a `palette:' block and rebuilds the effective palette.
 *
 *   palette:
 *     name: latte          # a built-in to start from
 *     accent: "#d20f39"    # anything else overrides one entry
 *
 * `name' is consumed rather than stored as an entry, so a palette
 * cannot accidentally define a colour called "name".  A config with no
 * palette block still gets one --- the default flavour --- so a colour
 * key naming `accent' resolves in every config, including one written
 * before palettes existed.
 */
static void
gowl_config_apply_palette_mapping(GowlConfig *self, YamlMapping *mapping)
{
	g_autoptr(GowlPalette) from_file = NULL;
	YamlNode *node;
	YamlMapping *pal_map;
	guint i, count;

	from_file = gowl_palette_new();

	if (!yaml_mapping_has_member(mapping, "palette")) {
		gowl_config_rebuild_palette(self, from_file);
		return;
	}

	node = yaml_mapping_get_member(mapping, "palette");
	pal_map = node != NULL ? yaml_node_get_mapping(node) : NULL;
	if (pal_map == NULL) {
		g_warning("gowl_config: `palette:' is not a mapping, ignoring");
		gowl_config_rebuild_palette(self, from_file);
		return;
	}

	count = yaml_mapping_get_size(pal_map);
	for (i = 0; i < count; i++) {
		const gchar *key;
		YamlNode *val_node;
		const gchar *val;

		key = yaml_mapping_get_key(pal_map, i);
		val_node = yaml_mapping_get_value(pal_map, i);
		if (key == NULL || val_node == NULL)
			continue;

		val = yaml_node_get_scalar(val_node);
		if (val == NULL)
			continue;

		if (g_strcmp0(key, "name") == 0) {
			g_free(self->palette_name);
			self->palette_name = g_strdup(val);
			continue;
		}

		gowl_palette_set(from_file, key, val);
	}

	gowl_config_rebuild_palette(self, from_file);
}

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
	/*
	 * The palette is applied before anything else in the mapping,
	 * regardless of where it appears in the file.  Colour keys resolve
	 * against it as they are read, so a `palette:' block written at the
	 * bottom of a config would otherwise silently do nothing to the
	 * keys above it --- a failure with no error and a plausible result.
	 */
	gowl_config_apply_palette_mapping(self, mapping);

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
	if (yaml_mapping_has_member(mapping, "animations")) {
		self->animations = yaml_mapping_get_boolean_member(
			mapping, "animations");
	}
	if (yaml_mapping_has_member(mapping, "animation-duration")) {
		self->animation_duration = (gint)yaml_mapping_get_int_member(
			mapping, "animation-duration");
	}
	if (yaml_mapping_has_member(mapping, "animation-duration-open")) {
		self->animation_duration_open =
			(gint)yaml_mapping_get_int_member(
				mapping, "animation-duration-open");
	}
	if (yaml_mapping_has_member(mapping, "animation-duration-close")) {
		self->animation_duration_close =
			(gint)yaml_mapping_get_int_member(
				mapping, "animation-duration-close");
	}
	if (yaml_mapping_has_member(mapping, "animation-curve-open")) {
		const gchar *v = yaml_mapping_get_string_member(mapping, "animation-curve-open");

		if (v != NULL) {
			g_free(self->animation_curve_open);
			self->animation_curve_open = g_strdup(v);
		}
	}
	if (yaml_mapping_has_member(mapping, "animation-popin-scale")) {
		gdouble v = yaml_mapping_get_double_member(mapping, "animation-popin-scale");

		/* Keep invalid input from collapsing or inverting a window. */
		if (v >= 0.5 && v <= 1.0)
			self->animation_popin_scale = v;
	}
	if (yaml_mapping_has_member(mapping, "animation-jiggle-strength")) {
		gdouble v = yaml_mapping_get_double_member(mapping, "animation-jiggle-strength");

		if (v >= 0.0 && v <= 2.0)
			self->animation_jiggle_strength = v;
	}

	if (yaml_mapping_has_member(mapping, "animation-curve")) {
		const gchar *v = yaml_mapping_get_string_member(
			mapping, "animation-curve");
		if (v != NULL) {
			g_free(self->animation_curve);
			self->animation_curve = g_strdup(v);
		}
	}

	if (yaml_mapping_has_member(mapping, "scroll-column-width")) {
		self->scroll_column_width = yaml_mapping_get_double_member(
			mapping, "scroll-column-width");
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
	if (yaml_mapping_has_member(mapping, "manage_lid")) {
		gboolean val = yaml_mapping_get_boolean_member(mapping, "manage_lid");
		g_object_set(self, "manage-lid", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "input-recording")) {
		gboolean val = yaml_mapping_get_boolean_member(mapping,
			"input-recording");
		g_object_set(self, "input-recording", val, NULL);
	}
	if (yaml_mapping_has_member(mapping, "input-recording-deny-apps")) {
		const gchar *val = yaml_mapping_get_string_member(mapping,
			"input-recording-deny-apps");
		if (val != NULL)
			g_object_set(self, "input-recording-deny-apps",
			             val, NULL);
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

	/* cmacs evaluation gates.  Accept both snake_case (matches the
	 * symbol form used in C configs) and kebab-case (matches every
	 * other YAML key).  First match wins. */
	{
		const gchar *evaluate_gowl_keys[] = {
			"evaluate_gowl_config_with_cmacs",
			"evaluate-gowl-config-with-cmacs",
			NULL
		};
		const gchar *evaluate_c_keys[] = {
			"evaluate_c_config_with_cmacs",
			"evaluate-c-config-with-cmacs",
			NULL
		};
		guint k;

		for (k = 0; evaluate_gowl_keys[k] != NULL; k++) {
			if (yaml_mapping_has_member(mapping, evaluate_gowl_keys[k])) {
				gboolean val = yaml_mapping_get_boolean_member(
					mapping, evaluate_gowl_keys[k]);
				g_object_set(self,
				             "evaluate-gowl-config-with-cmacs",
				             val, NULL);
				break;
			}
		}
		for (k = 0; evaluate_c_keys[k] != NULL; k++) {
			if (yaml_mapping_has_member(mapping, evaluate_c_keys[k])) {
				gboolean val = yaml_mapping_get_boolean_member(
					mapping, evaluate_c_keys[k]);
				g_object_set(self,
				             "evaluate-c-config-with-cmacs",
				             val, NULL);
				break;
			}
		}
	}

	/* Keybinds: mapping of
	 *   "Mod+Key": { action: <name>, arg: "<value>", desc: "<text>" }
	 *
	 * Example:
	 *   "Super+Return": { action: spawn, arg: "gst", desc: "Terminal" }
	 *   "Super+Shift+q": { action: quit }
	 *
	 * "desc" is optional and never affects dispatch; it is what
	 * a cheatsheet renders instead of an action number.
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
				const gchar *desc_str;
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

				desc_str = NULL;
				if (yaml_mapping_has_member(val_map, "desc"))
					desc_str = yaml_mapping_get_string_member(val_map, "desc");

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
			gowl_config_add_keybind_full(self, mods, keysym, action,
			                              arg_str, desc_str);
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
				const gchar *app_id;
				const gchar *title;
				guint32 tags;
				gboolean floating;
				gint monitor;
				gint width;
				gint height;
				gboolean center;
				gboolean regex_mode;

				if (rule_map == NULL)
					continue;

				app_id = NULL;
				title = NULL;
				tags = 0;
				floating = FALSE;
				monitor = -1;
				width = 0;
				height = 0;
				center = TRUE;
				regex_mode = FALSE;

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
				if (yaml_mapping_has_member(rule_map, "width"))
					width = (gint)yaml_mapping_get_int_member(rule_map, "width");
				if (yaml_mapping_has_member(rule_map, "height"))
					height = (gint)yaml_mapping_get_int_member(rule_map, "height");
				if (yaml_mapping_has_member(rule_map, "center"))
					center = yaml_mapping_get_boolean_member(rule_map, "center");
				if (yaml_mapping_has_member(rule_map, "regex"))
					regex_mode = yaml_mapping_get_boolean_member(rule_map, "regex");

				gowl_config_add_rule_full(self, app_id, title, tags,
				                           floating, monitor,
				                           width, height, center,
				                           regex_mode);
			}
		}
	}

	/* Dropdowns: sequence of mappings with keys:
	 *   name: "term"                 (required)
	 *   spawn-cmd: "foot"            (required)
	 *   keybind: "Super+grave"       (optional)
	 *   width-pct: 1.0               (optional, default 1.0)
	 *   height-pct: 0.4              (optional, default 0.4)
	 *   width: 800                   (optional, absolute px)
	 *   height: 600                  (optional, absolute px)
	 *   anchor: "top"|"bottom"|"left"|"right"  (optional, default top)
	 */
	if (yaml_mapping_has_member(mapping, "dropdowns")) {
		YamlSequence *seq = yaml_mapping_get_sequence_member(mapping,
		                                                      "dropdowns");
		if (seq != NULL) {
			guint len = yaml_sequence_get_length(seq);
			guint i;

			for (i = 0; i < len; i++) {
				YamlMapping *dd_map;
				const gchar *name;
				const gchar *spawn_cmd;
				const gchar *keybind;
				const gchar *anchor_str;
				gdouble width_pct;
				gdouble height_pct;
				gint width_abs;
				gint height_abs;
				gint anchor;

				dd_map = yaml_sequence_get_mapping_element(seq, i);
				if (dd_map == NULL)
					continue;

				name = NULL;
				spawn_cmd = NULL;
				keybind = NULL;
				anchor_str = NULL;
				width_pct = 1.0;
				height_pct = 0.4;
				width_abs = 0;
				height_abs = 0;
				anchor = 0; /* top */

				if (yaml_mapping_has_member(dd_map, "name"))
					name = yaml_mapping_get_string_member(dd_map, "name");
				if (yaml_mapping_has_member(dd_map, "spawn-cmd"))
					spawn_cmd = yaml_mapping_get_string_member(dd_map, "spawn-cmd");
				if (yaml_mapping_has_member(dd_map, "keybind"))
					keybind = yaml_mapping_get_string_member(dd_map, "keybind");
				if (yaml_mapping_has_member(dd_map, "anchor"))
					anchor_str = yaml_mapping_get_string_member(dd_map, "anchor");
				if (yaml_mapping_has_member(dd_map, "width-pct"))
					width_pct = yaml_mapping_get_double_member(dd_map, "width-pct");
				if (yaml_mapping_has_member(dd_map, "height-pct"))
					height_pct = yaml_mapping_get_double_member(dd_map, "height-pct");
				if (yaml_mapping_has_member(dd_map, "width"))
					width_abs = (gint)yaml_mapping_get_int_member(dd_map, "width");
				if (yaml_mapping_has_member(dd_map, "height"))
					height_abs = (gint)yaml_mapping_get_int_member(dd_map, "height");

				if (anchor_str != NULL) {
					if (g_ascii_strcasecmp(anchor_str, "bottom") == 0)
						anchor = 1;
					else if (g_ascii_strcasecmp(anchor_str, "left") == 0)
						anchor = 2;
					else if (g_ascii_strcasecmp(anchor_str, "right") == 0)
						anchor = 3;
					else
						anchor = 0;
				}

				if (name == NULL || spawn_cmd == NULL) {
					g_warning("gowl-config: dropdown entry %u missing name or spawn-cmd", i);
					continue;
				}

				gowl_config_add_dropdown(self, name, spawn_cmd, keybind,
				                          width_pct, height_pct,
				                          width_abs, height_abs,
				                          anchor);
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
						/* A colour setting is resolved against the
						 * palette here rather than in each module.
						 * Module settings are an untyped string map
						 * with no schema, so the key name is all
						 * there is to go on --- and doing it here
						 * means a module written tomorrow gets
						 * palette support without knowing the
						 * feature exists. */
						g_hash_table_insert(settings,
						                    g_strdup(key),
						                    gowl_palette_key_is_color(key)
						                    ? gowl_palette_resolve(
						                              self->palette, val_str)
						                    : g_strdup(val_str));
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

	/* Monitor configs: each child of `monitors:` is keyed by output
	 * name and maps to a per-output mapping with optional fields
	 * (width, height, refresh, x, y, scale, enabled, transform).
	 * Every field is independently optional -- unset fields are
	 * left at compositor defaults.
	 *
	 * Example YAML:
	 *   monitors:
	 *     eDP-1:
	 *       transform: 90       # rotate a portrait-default tablet
	 *     HDMI-A-1:
	 *       x: 1080
	 *       scale: 1.5
	 */
	if (yaml_mapping_has_member(mapping, "monitors")) {
		YamlMapping *mon_mapping;

		mon_mapping = yaml_mapping_get_mapping_member(mapping,
		                                               "monitors");
		if (mon_mapping != NULL) {
			guint mon_count = yaml_mapping_get_size(mon_mapping);
			guint mi;

			/* Clear any previously-loaded monitor configs on reload */
			g_hash_table_remove_all(self->monitor_configs);

			for (mi = 0; mi < mon_count; mi++) {
				const gchar *mon_name;
				YamlNode *mon_val_node;
				YamlMapping *mon_cfg_map;
				GowlMonitorConfig *mc;

				mon_name = yaml_mapping_get_key(mon_mapping, mi);
				mon_val_node = yaml_mapping_get_value(mon_mapping, mi);
				if (mon_name == NULL || mon_val_node == NULL)
					continue;

				mon_cfg_map = yaml_node_get_mapping(mon_val_node);
				if (mon_cfg_map == NULL)
					continue;

				mc = g_new0(GowlMonitorConfig, 1);
				mc->x = G_MININT;
				mc->y = G_MININT;
				mc->transform = -1;
				mc->enabled = -1;

				if (yaml_mapping_has_member(mon_cfg_map, "width"))
					mc->width = (gint)yaml_mapping_get_int_member(
						mon_cfg_map, "width");
				if (yaml_mapping_has_member(mon_cfg_map, "height"))
					mc->height = (gint)yaml_mapping_get_int_member(
						mon_cfg_map, "height");
				if (yaml_mapping_has_member(mon_cfg_map, "refresh"))
					mc->refresh = yaml_mapping_get_double_member(
						mon_cfg_map, "refresh");
				if (yaml_mapping_has_member(mon_cfg_map, "x"))
					mc->x = (gint)yaml_mapping_get_int_member(
						mon_cfg_map, "x");
				if (yaml_mapping_has_member(mon_cfg_map, "y"))
					mc->y = (gint)yaml_mapping_get_int_member(
						mon_cfg_map, "y");
				if (yaml_mapping_has_member(mon_cfg_map, "scale"))
					mc->scale = yaml_mapping_get_double_member(
						mon_cfg_map, "scale");
				if (yaml_mapping_has_member(mon_cfg_map, "enabled"))
					mc->enabled = yaml_mapping_get_boolean_member(
						mon_cfg_map, "enabled") ? 1 : 0;
				if (yaml_mapping_has_member(mon_cfg_map, "transform"))
					mc->transform = gowl_parse_monitor_transform(
						mon_cfg_map);

				g_debug("gowl_config: monitor '%s': "
				        "w=%d h=%d refresh=%.1f x=%d y=%d "
				        "scale=%.2f transform=%d enabled=%d",
				        mon_name, mc->width, mc->height,
				        mc->refresh, mc->x, mc->y,
				        mc->scale, mc->transform, mc->enabled);

				g_hash_table_insert(self->monitor_configs,
				                    g_strdup(mon_name), mc);
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
 * gowl_config_load_rules_d:
 * @self: a #GowlConfig
 * @config_path: the config file that was loaded
 *
 * Loads window rules from `rules.d/' beside @config_path, one file per
 * application, merged after the flat `rules:' list in the main config.
 *
 * A single growing `rules:' array is the wrong shape for this: a rule
 * for Steam and a rule for a screenshot overlay have nothing to do with
 * each other, and a config that ships rules cannot be edited by a user
 * without merging.  Omarchy keeps one Lua file per application for
 * exactly this reason.
 *
 * Files are read in sorted order so a numeric prefix decides
 * precedence, and each is a fragment carrying its own `rules:'
 * sequence, so it is a valid config file in its own right.
 *
 * Returns: how many files were loaded.
 */
guint
gowl_config_load_rules_d(GowlConfig *self, const gchar *config_path)
{
	g_autofree gchar *dir_path = NULL;
	g_autofree gchar *parent = NULL;
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GPtrArray) files = NULL;
	const gchar *name;
	guint i, loaded = 0;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0);
	g_return_val_if_fail(config_path != NULL, 0);

	parent = g_path_get_dirname(config_path);
	dir_path = g_build_filename(parent, "rules.d", NULL);

	dir = g_dir_open(dir_path, 0, NULL);
	if (dir == NULL)
		return 0;         /* no rules.d is the normal case */

	files = g_ptr_array_new_with_free_func(g_free);
	while ((name = g_dir_read_name(dir)) != NULL) {
		if (!g_str_has_suffix(name, ".yaml")
		    && !g_str_has_suffix(name, ".yml"))
			continue;
		g_ptr_array_add(files, g_build_filename(dir_path, name, NULL));
	}

	/* Sorted, so a `10-' prefix means what it looks like it means.
	 * Directory order is not sorted and differs between filesystems. */
	g_ptr_array_sort_values(files, (GCompareFunc)g_strcmp0);

	for (i = 0; i < files->len; i++) {
		const gchar *file = g_ptr_array_index(files, i);
		GError *err = NULL;

		/* A broken fragment costs itself, not the whole session: the
		 * rules that did parse stay, and the compositor still starts. */
		if (!gowl_config_load_yaml(self, file, &err)) {
			g_warning("gowl_config: %s: %s", file,
			          err ? err->message : "failed to load");
			g_clear_error(&err);
			continue;
		}
		g_debug("gowl_config: loaded rules from '%s'", file);
		loaded++;
	}

	return loaded;
}

/**
 * gowl_config_load_yaml_from_search_path:
 * @self: a #GowlConfig
 * @error: (nullable): return location for a #GError
 *
 * Searches standard directories for config.yaml and loads the first
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

	/* Build the XDG config path: ~/.config/gowl/config.yaml */
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
			gboolean ok;

			g_debug("gowl_config: loading config from '%s'",
			        search_paths[i]);
			ok = gowl_config_load_yaml(self, search_paths[i], error);
			if (ok)
				gowl_config_load_rules_d(self, search_paths[i]);
			return ok;
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

	/*
	 * The palette first, and the specs below unresolved --- a generated
	 * config that baked in today's literals would be one a theme change
	 * could no longer reach, which is the state this replaced.
	 */
	{
		g_auto(GStrv) names = gowl_palette_names(self->palette_override);
		gsize i;

		g_string_append(yaml, "palette:\n");
		g_string_append_printf(yaml, "  name: %s\n", self->palette_name);
		/* Only the overrides: the flavour supplies the rest, and
		 * writing all fifteen out would freeze them. */
		for (i = 0; names != NULL && names[i] != NULL; i++) {
			g_string_append_printf(yaml, "  %s: \"%s\"\n", names[i],
			                       gowl_palette_lookup(
			                               self->palette_override, names[i]));
		}
		g_string_append_c(yaml, '\n');
	}

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
	g_string_append_printf(yaml, "manage_lid: %s\n", self->manage_lid ? "true" : "false");
	g_string_append_printf(yaml, "input-recording: %s\n",
	                       self->input_recording ? "true" : "false");
	g_string_append_printf(yaml, "input-recording-deny-apps: \"%s\"\n",
	                       self->input_recording_deny_apps != NULL
	                       ? self->input_recording_deny_apps : "");

	/* Programs */
	g_string_append_printf(yaml, "terminal: \"%s\"\n", self->terminal);
	g_string_append_printf(yaml, "menu: \"%s\"\n", self->menu);

	/* Logging */
	g_string_append_printf(yaml, "log-level: \"%s\"\n", self->log_level);
	g_string_append_printf(yaml, "log-file: \"%s\"\n", self->log_file);

	/* cmacs evaluation gates (kebab-case; snake_case also accepted on load) */
	g_string_append_printf(yaml,
	                       "evaluate-gowl-config-with-cmacs: %s\n",
	                       self->evaluate_gowl_config_with_cmacs
	                       ? "true" : "false");
	g_string_append_printf(yaml,
	                       "evaluate-c-config-with-cmacs: %s\n",
	                       self->evaluate_c_config_with_cmacs
	                       ? "true" : "false");

	/* Keybinds.
	 *
	 * Emitted as a MAPPING of "bind": { action: ..., arg: ..., desc: ... },
	 * which is the shape gowl_config_apply_mapping() reads back.  An
	 * earlier version wrote a sequence of "- bind:" items; the parser
	 * asks for a mapping member, got NULL for a sequence and dropped
	 * every keybind silently, so a config saved from the dashboard came
	 * back with no binds at all. */
	if (self->keybinds->len > 0) {
		GEnumClass *action_class = (GEnumClass *)g_type_class_ref(
			gowl_action_get_type());

		g_string_append(yaml, "\nkeybinds:\n");
		for (i = 0; i < self->keybinds->len; i++) {
			GowlKeybindEntry *kb = &g_array_index(self->keybinds, GowlKeybindEntry, i);
			g_autofree gchar *bind_str = gowl_keybind_to_string(kb->modifiers, kb->keysym);
			GEnumValue *enum_val = g_enum_get_value(action_class, kb->action);
			const gchar *action_nick = (enum_val != NULL) ? enum_val->value_nick : "none";

			g_string_append_printf(yaml, "  \"%s\": { action: %s",
			                       bind_str, action_nick);
			if (kb->arg != NULL) {
				g_autofree gchar *esc = gowl_config_escape_yaml(kb->arg);
				g_string_append_printf(yaml, ", arg: \"%s\"", esc);
			}
			if (kb->desc != NULL) {
				g_autofree gchar *esc = gowl_config_escape_yaml(kb->desc);
				g_string_append_printf(yaml, ", desc: \"%s\"", esc);
			}
			g_string_append(yaml, " }\n");
		}

		g_type_class_unref(action_class);
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
			if (rule->width != 0)
				g_string_append_printf(yaml, "    width: %d\n", rule->width);
			if (rule->height != 0)
				g_string_append_printf(yaml, "    height: %d\n", rule->height);
			if (!rule->center)
				g_string_append(yaml, "    center: false\n");
			if (rule->regex_mode)
				g_string_append(yaml, "    regex: true\n");
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
	g_return_val_if_fail(GOWL_IS_CONFIG(self), "#89b4fa");
	return self->border_hex_focus;
}

const gchar *
gowl_config_get_border_color_unfocus(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), "#313244");
	return self->border_hex_unfocus;
}

const gchar *
gowl_config_get_border_color_urgent(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), "#f38ba8");
	return self->border_hex_urgent;
}

gboolean
gowl_config_get_animations(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), FALSE);

	return self->animations;
}

gint
gowl_config_get_animation_duration(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0);

	return self->animation_duration;
}

const gchar *
gowl_config_get_animation_curve(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);

	return self->animation_curve;
}

gdouble
gowl_config_get_scroll_column_width(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_SCROLL_COLUMN_WIDTH);

	return self->scroll_column_width;
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

gboolean
gowl_config_get_manage_lid(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_MANAGE_LID);
	return self->manage_lid;
}

gboolean
gowl_config_get_input_recording(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_INPUT_RECORDING);
	return self->input_recording;
}

const gchar *
gowl_config_get_input_recording_deny_apps(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_INPUT_RECORDING_DENY_APPS);
	return self->input_recording_deny_apps;
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

/* --- cmacs evaluation gates --- */

gboolean
gowl_config_get_evaluate_gowl_config_with_cmacs(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_EVALUATE_GOWL_CONFIG_WITH_CMACS);
	return self->evaluate_gowl_config_with_cmacs;
}

void
gowl_config_set_evaluate_gowl_config_with_cmacs(GowlConfig *self,
                                                 gboolean    value)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	value = value ? TRUE : FALSE;
	if (self->evaluate_gowl_config_with_cmacs == value)
		return;

	self->evaluate_gowl_config_with_cmacs = value;
	g_object_notify_by_pspec(
		G_OBJECT(self),
		properties[GOWL_CONFIG_PROP_EVALUATE_GOWL_CONFIG_WITH_CMACS]);
	g_signal_emit(self, signals[SIGNAL_CHANGED], 0,
	              "evaluate-gowl-config-with-cmacs");
}

gboolean
gowl_config_get_evaluate_c_config_with_cmacs(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self),
	                     GOWL_CONFIG_DEFAULT_EVALUATE_C_CONFIG_WITH_CMACS);
	return self->evaluate_c_config_with_cmacs;
}

void
gowl_config_set_evaluate_c_config_with_cmacs(GowlConfig *self,
                                              gboolean    value)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	value = value ? TRUE : FALSE;
	if (self->evaluate_c_config_with_cmacs == value)
		return;

	self->evaluate_c_config_with_cmacs = value;
	g_object_notify_by_pspec(
		G_OBJECT(self),
		properties[GOWL_CONFIG_PROP_EVALUATE_C_CONFIG_WITH_CMACS]);
	g_signal_emit(self, signals[SIGNAL_CHANGED], 0,
	              "evaluate-c-config-with-cmacs");
}

/**
 * gowl_config_reset_values_to_defaults:
 *
 * Restores every config property except the two cmacs evaluation
 * gates to its compile-time default.  Used by cmacs `--gowl` startup
 * when `evaluate-gowl-config-with-cmacs` is %FALSE: the YAML was
 * parsed fully (so notify:: fired for user intent) but the resulting
 * state is discarded except for the gates themselves.
 *
 * Also clears keybinds, rules, dropdowns, and module configs.  Emits
 * "reloaded" at the end so downstream listeners treat this like a
 * full reload.
 */
void
gowl_config_reset_values_to_defaults(GowlConfig *self)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	g_object_freeze_notify(G_OBJECT(self));

	g_object_set(self,
	             "border-width",        GOWL_CONFIG_DEFAULT_BORDER_WIDTH,
	             "border-color-focus",  GOWL_CONFIG_DEFAULT_BORDER_COLOR_FOCUS,
	             "border-color-unfocus", GOWL_CONFIG_DEFAULT_BORDER_COLOR_UNFOCUS,
	             "border-color-urgent",  GOWL_CONFIG_DEFAULT_BORDER_COLOR_URGENT,
	             "mfact",               GOWL_CONFIG_DEFAULT_MFACT,
	             "nmaster",             GOWL_CONFIG_DEFAULT_NMASTER,
	             "tag-count",           GOWL_CONFIG_DEFAULT_TAG_COUNT,
	             "repeat-rate",         GOWL_CONFIG_DEFAULT_REPEAT_RATE,
	             "repeat-delay",        GOWL_CONFIG_DEFAULT_REPEAT_DELAY,
	             "terminal",            GOWL_CONFIG_DEFAULT_TERMINAL,
	             "menu",                GOWL_CONFIG_DEFAULT_MENU,
	             "sloppyfocus",         GOWL_CONFIG_DEFAULT_SLOPPYFOCUS,
	             "manage-lid",          GOWL_CONFIG_DEFAULT_MANAGE_LID,
	             "input-recording",     GOWL_CONFIG_DEFAULT_INPUT_RECORDING,
	             "input-recording-deny-apps",
	                 GOWL_CONFIG_DEFAULT_INPUT_RECORDING_DENY_APPS,
	             "log-level",           GOWL_CONFIG_DEFAULT_LOG_LEVEL,
	             "log-file",            GOWL_CONFIG_DEFAULT_LOG_FILE,
	             NULL);

	if (self->keybinds != NULL)
		g_array_set_size(self->keybinds, 0);
	if (self->rules != NULL)
		g_ptr_array_set_size(self->rules, 0);
	if (self->dropdowns != NULL)
		g_ptr_array_set_size(self->dropdowns, 0);
	if (self->module_configs != NULL)
		g_hash_table_remove_all(self->module_configs);
	if (self->monitor_configs != NULL)
		g_hash_table_remove_all(self->monitor_configs);

	g_object_thaw_notify(G_OBJECT(self));

	g_signal_emit(self, signals[SIGNAL_RELOADED], 0);
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
 * Appends a keybind entry with no description.  Thin wrapper over
 * gowl_config_add_keybind_full(); kept so that every pre-existing
 * caller compiles unchanged.
 */
void
gowl_config_add_keybind(
	GowlConfig  *self,
	guint        modifiers,
	guint        keysym,
	gint         action,
	const gchar *arg
){
	gowl_config_add_keybind_full(self, modifiers, keysym, action,
	                              arg, NULL);
}

/**
 * gowl_config_add_keybind_full:
 * @self: a #GowlConfig
 * @modifiers: bitmask of #GowlKeyMod flags
 * @keysym: XKB keysym value
 * @action: a #GowlAction value
 * @arg: (nullable): optional argument string (will be copied)
 * @desc: (nullable): human-readable description (will be copied)
 *
 * Appends a keybind entry to the internal keybind array.
 */
void
gowl_config_add_keybind_full(
	GowlConfig  *self,
	guint        modifiers,
	guint        keysym,
	gint         action,
	const gchar *arg,
	const gchar *desc
){
	GowlKeybindEntry entry;

	g_return_if_fail(GOWL_IS_CONFIG(self));

	entry.modifiers = modifiers;
	entry.keysym    = keysym;
	entry.action    = action;
	entry.arg       = g_strdup(arg);
	entry.desc      = g_strdup(desc);

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

/**
 * gowl_config_remove_keybind:
 * @self: a #GowlConfig
 * @modifiers: bitmask of #GowlKeyMod flags
 * @keysym: XKB keysym value
 *
 * Removes every keybind entry whose @modifiers and @keysym match the
 * arguments.  The @action and @arg fields are not compared, so all
 * binds registered for the same key combo are removed -- this is the
 * shape callers need to replace a stale bind with an authoritative one
 * (gowl's dispatch takes the first matching entry, so an older
 * duplicate would otherwise shadow a freshly-added one).  Returns the
 * number of entries removed.
 */
guint
gowl_config_remove_keybind(
	GowlConfig  *self,
	guint        modifiers,
	guint        keysym
){
	guint i;
	guint removed = 0;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0);

	for (i = 0; i < self->keybinds->len; ) {
		GowlKeybindEntry *kb;

		kb = &g_array_index(self->keybinds, GowlKeybindEntry, i);
		if (kb->modifiers == modifiers && kb->keysym == keysym) {
			/* Order-preserving remove: shifts the tail down so the
			 * next candidate slides into slot i -- do not advance. */
			g_array_remove_index(self->keybinds, i);
			removed++;
			continue;
		}
		i++;
	}

	return removed;
}

/**
 * gowl_config_clear_keybinds:
 * @self: a #GowlConfig
 *
 * Removes every keybind from the config.  The per-entry clear func
 * frees each entry's arg string.
 */
void
gowl_config_clear_keybinds(GowlConfig *self)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	if (self->keybinds != NULL)
		g_array_set_size(self->keybinds, 0);
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
	gowl_config_add_rule_full(self, app_id, title, tags, floating,
	                           monitor, 0, 0, TRUE, FALSE);
}

/**
 * gowl_config_add_rule_full:
 * @self: a #GowlConfig
 * @app_id: (nullable): app_id pattern or %NULL
 * @title: (nullable): title pattern or %NULL
 * @tags: tag bitmask
 * @floating: whether to float
 * @monitor: target monitor index or -1
 * @width: explicit width in pixels, or 0 for natural
 * @height: explicit height in pixels, or 0 for natural
 * @center: center on monitor when floating
 * @regex_mode: interpret patterns as PCRE regexes
 *
 * Allocates a new #GowlRuleEntry with every tunable field and
 * appends it to the rules array.  Called by gowl_config_add_rule()
 * with sensible defaults for the v2 fields.
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
){
	GowlRuleEntry *rule;

	g_return_if_fail(GOWL_IS_CONFIG(self));

	rule = g_new0(GowlRuleEntry, 1);
	rule->app_id     = g_strdup(app_id);
	rule->title      = g_strdup(title);
	rule->tags       = tags;
	rule->floating   = floating;
	rule->monitor    = monitor;
	rule->width      = width;
	rule->height     = height;
	rule->center     = center;
	rule->regex_mode = regex_mode;

	g_ptr_array_add(self->rules, rule);
}

/**
 * gowl_config_remove_rule:
 * @self: a #GowlConfig
 * @app_id: (nullable): app_id pattern to match the rule by
 * @title: (nullable): title pattern to match the rule by
 *
 * Removes the first rule entry whose @app_id and @title strings
 * match the arguments verbatim.  Comparison is by literal string;
 * %NULL matches %NULL, non-%NULL uses g_strcmp0().  Returns the
 * number of rules removed.
 */
guint
gowl_config_remove_rule(
	GowlConfig  *self,
	const gchar *app_id,
	const gchar *title
){
	guint i;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0);

	for (i = 0; i < self->rules->len; i++) {
		GowlRuleEntry *rule;

		rule = (GowlRuleEntry *)g_ptr_array_index(self->rules, i);
		if (g_strcmp0(rule->app_id, app_id) == 0 &&
		    g_strcmp0(rule->title, title) == 0) {
			g_ptr_array_remove_index(self->rules, i);
			return 1;
		}
	}

	return 0;
}

/**
 * gowl_config_clear_rules:
 * @self: a #GowlConfig
 *
 * Removes every rule from the config.
 */
void
gowl_config_clear_rules(GowlConfig *self)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	if (self->rules->len > 0)
		g_ptr_array_remove_range(self->rules, 0, self->rules->len);
}

/* --- Dropdown management --- */

/**
 * gowl_config_add_dropdown:
 *
 * Allocates a new #GowlDropdownEntry, copies the strings, and
 * appends it to the dropdowns array.  Adding an entry with a
 * duplicate @name silently replaces the existing entry.
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
){
	GowlDropdownEntry *dd;

	g_return_if_fail(GOWL_IS_CONFIG(self));
	g_return_if_fail(name != NULL);
	g_return_if_fail(spawn_cmd != NULL);

	gowl_config_remove_dropdown(self, name);

	dd = g_new0(GowlDropdownEntry, 1);
	dd->name       = g_strdup(name);
	dd->spawn_cmd  = g_strdup(spawn_cmd);
	dd->keybind    = g_strdup(keybind);
	dd->width_pct  = width_pct;
	dd->height_pct = height_pct;
	dd->width_abs  = width_abs;
	dd->height_abs = height_abs;
	dd->anchor     = anchor;

	g_ptr_array_add(self->dropdowns, dd);
}

/**
 * gowl_config_remove_dropdown:
 *
 * Removes the dropdown entry whose @name matches exactly.
 * Returns 1 on removal, 0 if nothing matched.
 */
guint
gowl_config_remove_dropdown(
	GowlConfig  *self,
	const gchar *name
){
	guint i;

	g_return_val_if_fail(GOWL_IS_CONFIG(self), 0);
	g_return_val_if_fail(name != NULL, 0);

	for (i = 0; i < self->dropdowns->len; i++) {
		GowlDropdownEntry *dd;

		dd = (GowlDropdownEntry *)g_ptr_array_index(self->dropdowns, i);
		if (g_strcmp0(dd->name, name) == 0) {
			g_ptr_array_remove_index(self->dropdowns, i);
			return 1;
		}
	}

	return 0;
}

/**
 * gowl_config_get_dropdowns:
 *
 * Returns the #GPtrArray backing the dropdown entries.  The
 * array is borrowed; the caller must not free it.
 */
GPtrArray *
gowl_config_get_dropdowns(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->dropdowns;
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

/**
 * gowl_config_get_monitor_config:
 * @self: a #GowlConfig
 * @name: the output name (e.g. "eDP-1")
 *
 * Looks up the per-output config parsed from `monitors:`.
 *
 * Returns: (transfer none) (nullable): #GowlMonitorConfig owned by
 *          @self, or %NULL if no entry exists for @name
 */
const GowlMonitorConfig *
gowl_config_get_monitor_config(
	GowlConfig  *self,
	const gchar *name
){
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	g_return_val_if_fail(name != NULL, NULL);

	return (const GowlMonitorConfig *)g_hash_table_lookup(
		self->monitor_configs, name);
}

/**
 * gowl_config_get_monitor_names:
 * @self: a #GowlConfig
 *
 * Lists every output name that has an entry in the parsed
 * `monitors:` mapping.  The list itself is owned by the caller
 * (g_list_free), but the string elements are borrowed from
 * @self's internal hash and must not be freed.
 *
 * Returns: (transfer container) (element-type utf8): a #GList
 */
GList *
gowl_config_get_monitor_names(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return g_hash_table_get_keys(self->monitor_configs);
}

/* ── Palette ─────────────────────────────────────────────────────── */

/**
 * gowl_config_get_palette:
 * @self: a #GowlConfig
 *
 * The effective palette: the named built-in, with the config file's
 * `palette:' entries and then any runtime overrides layered on it.
 *
 * Returns: (transfer none): the palette.  Never %NULL.
 */
GowlPalette *
gowl_config_get_palette(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->palette;
}

/**
 * gowl_config_get_palette_name:
 * @self: a #GowlConfig
 *
 * Returns: (transfer none): the built-in flavour the palette starts from.
 */
const gchar *
gowl_config_get_palette_name(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), NULL);
	return self->palette_name;
}

/**
 * gowl_config_set_palette_name:
 * @self: a #GowlConfig
 * @name: (nullable): a built-in palette name
 *
 * Switches the flavour the palette starts from, keeping every override.
 * Everything that reads a colour through the config picks the change up
 * on the next arrange; nothing is repainted here.
 */
void
gowl_config_set_palette_name(GowlConfig *self, const gchar *name)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));

	g_free(self->palette_name);
	self->palette_name = g_strdup(name != NULL
	                              ? name : GOWL_CONFIG_DEFAULT_PALETTE);

	/* No from_file layer: the file's entries were folded into the
	 * override-free base at load time and are re-read on reload.  A
	 * flavour switch between reloads keeps only the runtime
	 * overrides, which is the layer the caller owns. */
	gowl_config_rebuild_palette(self, NULL);
}

/**
 * gowl_config_set_palette_color:
 * @self: a #GowlConfig
 * @name: a palette entry name
 * @hex: (nullable): a literal colour, or %NULL to drop the override
 *
 * Overrides one palette entry at runtime.  Overrides sit above the
 * config file, so they survive a reload --- which is what makes
 * "follow the editor's theme" work: the theme pushes its colours in
 * once and a later `gowl-reload-config' does not undo it.
 */
void
gowl_config_set_palette_color(GowlConfig  *self,
                              const gchar *name,
                              const gchar *hex)
{
	g_return_if_fail(GOWL_IS_CONFIG(self));
	g_return_if_fail(name != NULL);

	gowl_palette_set(self->palette_override, name, hex);

	/* Rebuilding from the built-in drops the file's entries until the
	 * next reload.  Setting the entry on the effective palette too
	 * keeps them, at the cost of the override being invisible in
	 * `palette_override' order --- which nothing depends on. */
	gowl_palette_set(self->palette, name, hex);
	gowl_config_reresolve_colors(self);
}

/**
 * gowl_config_resolve_color:
 * @self: a #GowlConfig
 * @spec: (nullable): a colour spec --- a literal, a palette entry name,
 *   or `name/aa'
 *
 * Resolves a colour spec against the config's palette.  Public so that
 * a module handling colours outside the `*color*' setting convention,
 * or building one at runtime, can still go through the palette.
 *
 * Returns: (transfer full) (nullable): a newly allocated hex string.
 */
gchar *
gowl_config_resolve_color(GowlConfig *self, const gchar *spec)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), g_strdup(spec));
	return gowl_palette_resolve(self->palette, spec);
}

/**
 * gowl_config_get_animation_duration_open:
 * @self: a #GowlConfig
 *
 * How long a window's open animation runs, in milliseconds.
 *
 * Returns: the duration, or -1 to mean "use the general
 *   `animation-duration'".
 */
gint
gowl_config_get_animation_duration_open(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), -1);
	return self->animation_duration_open;
}

/**
 * gowl_config_get_animation_duration_close:
 * @self: a #GowlConfig
 *
 * How long a window's close animation runs, in milliseconds.
 *
 * Returns: the duration, or -1 to mean "use the general
 *   `animation-duration'".
 */
gint
gowl_config_get_animation_duration_close(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), -1);
	return self->animation_duration_close;
}

const gchar *
gowl_config_get_animation_curve_open(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_ANIMATION_CURVE_OPEN);
	return self->animation_curve_open;
}

gdouble
gowl_config_get_animation_popin_scale(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), GOWL_CONFIG_DEFAULT_ANIMATION_POPIN_SCALE);
	return self->animation_popin_scale;
}

gdouble
gowl_config_get_animation_jiggle_strength(GowlConfig *self)
{
	g_return_val_if_fail(GOWL_IS_CONFIG(self), 1.0);
	return self->animation_jiggle_strength;
}
