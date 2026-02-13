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

#include "gowlbar-config.h"

#include <glib.h>
#include <glib-object.h>
#include <string.h>

/* yaml-glib headers -- available via -Ideps/yaml-glib/src */
#include "yaml-glib.h"

/* --- Default values --- */
#define GOWLBAR_CONFIG_DEFAULT_HEIGHT          (24)
#define GOWLBAR_CONFIG_DEFAULT_POSITION        "top"
#define GOWLBAR_CONFIG_DEFAULT_FONT            "monospace 10"
#define GOWLBAR_CONFIG_DEFAULT_PADDING         (4)
#define GOWLBAR_CONFIG_DEFAULT_BG              "#222222"
#define GOWLBAR_CONFIG_DEFAULT_FG              "#bbbbbb"
#define GOWLBAR_CONFIG_DEFAULT_TAG_ACTIVE_BG   "#005577"
#define GOWLBAR_CONFIG_DEFAULT_TAG_ACTIVE_FG   "#eeeeee"
#define GOWLBAR_CONFIG_DEFAULT_TAG_URGENT_BG   "#ff0000"
#define GOWLBAR_CONFIG_DEFAULT_TAG_URGENT_FG   "#ffffff"
#define GOWLBAR_CONFIG_DEFAULT_TAG_OCCUPIED_FG "#bbbbbb"
#define GOWLBAR_CONFIG_DEFAULT_TAG_EMPTY_FG    "#555555"
#define GOWLBAR_CONFIG_DEFAULT_LAYOUT_FG       "#bbbbbb"
#define GOWLBAR_CONFIG_DEFAULT_TITLE_FG        "#bbbbbb"
#define GOWLBAR_CONFIG_DEFAULT_STATUS_FG       "#bbbbbb"
#define GOWLBAR_CONFIG_DEFAULT_BORDER_COLOR    "#444444"

/* Configuration file name */
#define GOWLBAR_CONFIG_FILENAME "bar.yaml"

/* --- Instance struct --- */

struct _GowlbarConfig {
	GObject parent_instance;

	/* Bar */
	gint     height;
	gchar   *position;
	gchar   *font;
	gint     padding;

	/* Colours */
	gchar   *background;
	gchar   *foreground;
	gchar   *tag_active_bg;
	gchar   *tag_active_fg;
	gchar   *tag_urgent_bg;
	gchar   *tag_urgent_fg;
	gchar   *tag_occupied_fg;
	gchar   *tag_empty_fg;
	gchar   *layout_fg;
	gchar   *title_fg;
	gchar   *status_fg;
	gchar   *border_color;
};

G_DEFINE_FINAL_TYPE(GowlbarConfig, gowlbar_config, G_TYPE_OBJECT)

/* --- Signal IDs --- */
enum {
	SIGNAL_CHANGED,
	SIGNAL_RELOADED,
	N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

/* --- GObject property storage --- */
static GParamSpec *properties[GOWLBAR_CONFIG_PROP_LAST] = { NULL };

/* --- GObject vfuncs --- */

/**
 * gowlbar_config_set_property:
 *
 * GObject set_property vfunc for #GowlbarConfig.
 * Sets each GObject property and emits the "changed" signal.
 */
static void
gowlbar_config_set_property(
	GObject      *object,
	guint         prop_id,
	const GValue *value,
	GParamSpec   *pspec
){
	GowlbarConfig *self = GOWLBAR_CONFIG(object);

	switch ((GowlbarConfigProp)prop_id) {
	case GOWLBAR_CONFIG_PROP_HEIGHT:
		self->height = g_value_get_int(value);
		break;
	case GOWLBAR_CONFIG_PROP_POSITION:
		g_free(self->position);
		self->position = g_value_dup_string(value);
		break;
	case GOWLBAR_CONFIG_PROP_FONT:
		g_free(self->font);
		self->font = g_value_dup_string(value);
		break;
	case GOWLBAR_CONFIG_PROP_PADDING:
		self->padding = g_value_get_int(value);
		break;
	case GOWLBAR_CONFIG_PROP_BG:
		g_free(self->background);
		self->background = g_value_dup_string(value);
		break;
	case GOWLBAR_CONFIG_PROP_FG:
		g_free(self->foreground);
		self->foreground = g_value_dup_string(value);
		break;
	case GOWLBAR_CONFIG_PROP_TAG_ACTIVE_BG:
		g_free(self->tag_active_bg);
		self->tag_active_bg = g_value_dup_string(value);
		break;
	case GOWLBAR_CONFIG_PROP_TAG_ACTIVE_FG:
		g_free(self->tag_active_fg);
		self->tag_active_fg = g_value_dup_string(value);
		break;
	case GOWLBAR_CONFIG_PROP_TAG_URGENT_BG:
		g_free(self->tag_urgent_bg);
		self->tag_urgent_bg = g_value_dup_string(value);
		break;
	case GOWLBAR_CONFIG_PROP_TAG_URGENT_FG:
		g_free(self->tag_urgent_fg);
		self->tag_urgent_fg = g_value_dup_string(value);
		break;
	case GOWLBAR_CONFIG_PROP_TAG_OCCUPIED_FG:
		g_free(self->tag_occupied_fg);
		self->tag_occupied_fg = g_value_dup_string(value);
		break;
	case GOWLBAR_CONFIG_PROP_TAG_EMPTY_FG:
		g_free(self->tag_empty_fg);
		self->tag_empty_fg = g_value_dup_string(value);
		break;
	case GOWLBAR_CONFIG_PROP_LAYOUT_FG:
		g_free(self->layout_fg);
		self->layout_fg = g_value_dup_string(value);
		break;
	case GOWLBAR_CONFIG_PROP_TITLE_FG:
		g_free(self->title_fg);
		self->title_fg = g_value_dup_string(value);
		break;
	case GOWLBAR_CONFIG_PROP_STATUS_FG:
		g_free(self->status_fg);
		self->status_fg = g_value_dup_string(value);
		break;
	case GOWLBAR_CONFIG_PROP_BORDER_COLOR:
		g_free(self->border_color);
		self->border_color = g_value_dup_string(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		return;
	}

	/* Emit "changed" signal with the property name */
	g_signal_emit(self, signals[SIGNAL_CHANGED], 0, pspec->name);
}

/**
 * gowlbar_config_get_property:
 *
 * GObject get_property vfunc for #GowlbarConfig.
 */
static void
gowlbar_config_get_property(
	GObject    *object,
	guint       prop_id,
	GValue     *value,
	GParamSpec *pspec
){
	GowlbarConfig *self = GOWLBAR_CONFIG(object);

	switch ((GowlbarConfigProp)prop_id) {
	case GOWLBAR_CONFIG_PROP_HEIGHT:
		g_value_set_int(value, self->height);
		break;
	case GOWLBAR_CONFIG_PROP_POSITION:
		g_value_set_string(value, self->position);
		break;
	case GOWLBAR_CONFIG_PROP_FONT:
		g_value_set_string(value, self->font);
		break;
	case GOWLBAR_CONFIG_PROP_PADDING:
		g_value_set_int(value, self->padding);
		break;
	case GOWLBAR_CONFIG_PROP_BG:
		g_value_set_string(value, self->background);
		break;
	case GOWLBAR_CONFIG_PROP_FG:
		g_value_set_string(value, self->foreground);
		break;
	case GOWLBAR_CONFIG_PROP_TAG_ACTIVE_BG:
		g_value_set_string(value, self->tag_active_bg);
		break;
	case GOWLBAR_CONFIG_PROP_TAG_ACTIVE_FG:
		g_value_set_string(value, self->tag_active_fg);
		break;
	case GOWLBAR_CONFIG_PROP_TAG_URGENT_BG:
		g_value_set_string(value, self->tag_urgent_bg);
		break;
	case GOWLBAR_CONFIG_PROP_TAG_URGENT_FG:
		g_value_set_string(value, self->tag_urgent_fg);
		break;
	case GOWLBAR_CONFIG_PROP_TAG_OCCUPIED_FG:
		g_value_set_string(value, self->tag_occupied_fg);
		break;
	case GOWLBAR_CONFIG_PROP_TAG_EMPTY_FG:
		g_value_set_string(value, self->tag_empty_fg);
		break;
	case GOWLBAR_CONFIG_PROP_LAYOUT_FG:
		g_value_set_string(value, self->layout_fg);
		break;
	case GOWLBAR_CONFIG_PROP_TITLE_FG:
		g_value_set_string(value, self->title_fg);
		break;
	case GOWLBAR_CONFIG_PROP_STATUS_FG:
		g_value_set_string(value, self->status_fg);
		break;
	case GOWLBAR_CONFIG_PROP_BORDER_COLOR:
		g_value_set_string(value, self->border_color);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

/**
 * gowlbar_config_finalize:
 *
 * Releases all resources owned by the #GowlbarConfig instance.
 */
static void
gowlbar_config_finalize(GObject *object)
{
	GowlbarConfig *self = GOWLBAR_CONFIG(object);

	g_free(self->position);
	g_free(self->font);
	g_free(self->background);
	g_free(self->foreground);
	g_free(self->tag_active_bg);
	g_free(self->tag_active_fg);
	g_free(self->tag_urgent_bg);
	g_free(self->tag_urgent_fg);
	g_free(self->tag_occupied_fg);
	g_free(self->tag_empty_fg);
	g_free(self->layout_fg);
	g_free(self->title_fg);
	g_free(self->status_fg);
	g_free(self->border_color);

	G_OBJECT_CLASS(gowlbar_config_parent_class)->finalize(object);
}

/* --- Class init --- */

/**
 * gowlbar_config_class_init:
 * @klass: the #GowlbarConfigClass
 *
 * Installs GObject properties and signals on the #GowlbarConfig class.
 */
static void
gowlbar_config_class_init(GowlbarConfigClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->set_property = gowlbar_config_set_property;
	object_class->get_property = gowlbar_config_get_property;
	object_class->finalize     = gowlbar_config_finalize;

	/* --- Install properties --- */

	properties[GOWLBAR_CONFIG_PROP_HEIGHT] =
		g_param_spec_int("height",
		                  "Height",
		                  "Bar height in pixels",
		                  8, 256,
		                  GOWLBAR_CONFIG_DEFAULT_HEIGHT,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWLBAR_CONFIG_PROP_POSITION] =
		g_param_spec_string("position",
		                     "Position",
		                     "Bar position (top or bottom)",
		                     GOWLBAR_CONFIG_DEFAULT_POSITION,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWLBAR_CONFIG_PROP_FONT] =
		g_param_spec_string("font",
		                     "Font",
		                     "Pango font description string",
		                     GOWLBAR_CONFIG_DEFAULT_FONT,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWLBAR_CONFIG_PROP_PADDING] =
		g_param_spec_int("padding",
		                  "Padding",
		                  "Horizontal padding in pixels",
		                  0, 100,
		                  GOWLBAR_CONFIG_DEFAULT_PADDING,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWLBAR_CONFIG_PROP_BG] =
		g_param_spec_string("background",
		                     "Background",
		                     "Background colour hex string",
		                     GOWLBAR_CONFIG_DEFAULT_BG,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWLBAR_CONFIG_PROP_FG] =
		g_param_spec_string("foreground",
		                     "Foreground",
		                     "Default foreground colour hex string",
		                     GOWLBAR_CONFIG_DEFAULT_FG,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWLBAR_CONFIG_PROP_TAG_ACTIVE_BG] =
		g_param_spec_string("tag-active-bg",
		                     "Tag Active Background",
		                     "Active tag background colour hex string",
		                     GOWLBAR_CONFIG_DEFAULT_TAG_ACTIVE_BG,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWLBAR_CONFIG_PROP_TAG_ACTIVE_FG] =
		g_param_spec_string("tag-active-fg",
		                     "Tag Active Foreground",
		                     "Active tag foreground colour hex string",
		                     GOWLBAR_CONFIG_DEFAULT_TAG_ACTIVE_FG,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWLBAR_CONFIG_PROP_TAG_URGENT_BG] =
		g_param_spec_string("tag-urgent-bg",
		                     "Tag Urgent Background",
		                     "Urgent tag background colour hex string",
		                     GOWLBAR_CONFIG_DEFAULT_TAG_URGENT_BG,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWLBAR_CONFIG_PROP_TAG_URGENT_FG] =
		g_param_spec_string("tag-urgent-fg",
		                     "Tag Urgent Foreground",
		                     "Urgent tag foreground colour hex string",
		                     GOWLBAR_CONFIG_DEFAULT_TAG_URGENT_FG,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWLBAR_CONFIG_PROP_TAG_OCCUPIED_FG] =
		g_param_spec_string("tag-occupied-fg",
		                     "Tag Occupied Foreground",
		                     "Occupied tag foreground colour hex string",
		                     GOWLBAR_CONFIG_DEFAULT_TAG_OCCUPIED_FG,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWLBAR_CONFIG_PROP_TAG_EMPTY_FG] =
		g_param_spec_string("tag-empty-fg",
		                     "Tag Empty Foreground",
		                     "Empty tag foreground colour hex string",
		                     GOWLBAR_CONFIG_DEFAULT_TAG_EMPTY_FG,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWLBAR_CONFIG_PROP_LAYOUT_FG] =
		g_param_spec_string("layout-fg",
		                     "Layout Foreground",
		                     "Layout indicator foreground colour hex string",
		                     GOWLBAR_CONFIG_DEFAULT_LAYOUT_FG,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWLBAR_CONFIG_PROP_TITLE_FG] =
		g_param_spec_string("title-fg",
		                     "Title Foreground",
		                     "Window title foreground colour hex string",
		                     GOWLBAR_CONFIG_DEFAULT_TITLE_FG,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWLBAR_CONFIG_PROP_STATUS_FG] =
		g_param_spec_string("status-fg",
		                     "Status Foreground",
		                     "Status text foreground colour hex string",
		                     GOWLBAR_CONFIG_DEFAULT_STATUS_FG,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	properties[GOWLBAR_CONFIG_PROP_BORDER_COLOR] =
		g_param_spec_string("border-color",
		                     "Border Colour",
		                     "Bar border colour hex string",
		                     GOWLBAR_CONFIG_DEFAULT_BORDER_COLOR,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class,
	                                  GOWLBAR_CONFIG_PROP_LAST,
	                                  properties);

	/* --- Install signals --- */

	/**
	 * GowlbarConfig::changed:
	 * @self: the #GowlbarConfig that changed
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
	 * GowlbarConfig::reloaded:
	 * @self: the #GowlbarConfig that was reloaded
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
 * gowlbar_config_init:
 * @self: the #GowlbarConfig instance being initialised
 *
 * Sets all fields to their default values.
 */
static void
gowlbar_config_init(GowlbarConfig *self)
{
	self->height          = GOWLBAR_CONFIG_DEFAULT_HEIGHT;
	self->position        = g_strdup(GOWLBAR_CONFIG_DEFAULT_POSITION);
	self->font            = g_strdup(GOWLBAR_CONFIG_DEFAULT_FONT);
	self->padding         = GOWLBAR_CONFIG_DEFAULT_PADDING;
	self->background      = g_strdup(GOWLBAR_CONFIG_DEFAULT_BG);
	self->foreground      = g_strdup(GOWLBAR_CONFIG_DEFAULT_FG);
	self->tag_active_bg   = g_strdup(GOWLBAR_CONFIG_DEFAULT_TAG_ACTIVE_BG);
	self->tag_active_fg   = g_strdup(GOWLBAR_CONFIG_DEFAULT_TAG_ACTIVE_FG);
	self->tag_urgent_bg   = g_strdup(GOWLBAR_CONFIG_DEFAULT_TAG_URGENT_BG);
	self->tag_urgent_fg   = g_strdup(GOWLBAR_CONFIG_DEFAULT_TAG_URGENT_FG);
	self->tag_occupied_fg = g_strdup(GOWLBAR_CONFIG_DEFAULT_TAG_OCCUPIED_FG);
	self->tag_empty_fg    = g_strdup(GOWLBAR_CONFIG_DEFAULT_TAG_EMPTY_FG);
	self->layout_fg       = g_strdup(GOWLBAR_CONFIG_DEFAULT_LAYOUT_FG);
	self->title_fg        = g_strdup(GOWLBAR_CONFIG_DEFAULT_TITLE_FG);
	self->status_fg       = g_strdup(GOWLBAR_CONFIG_DEFAULT_STATUS_FG);
	self->border_color    = g_strdup(GOWLBAR_CONFIG_DEFAULT_BORDER_COLOR);
}

/* --- Public API --- */

/**
 * gowlbar_config_new:
 *
 * Creates a new #GowlbarConfig populated with default values.
 *
 * Returns: (transfer full): a new #GowlbarConfig
 */
GowlbarConfig *
gowlbar_config_new(void)
{
	return (GowlbarConfig *)g_object_new(GOWLBAR_TYPE_CONFIG, NULL);
}

/* --- YAML loading helpers --- */

/**
 * apply_string_member:
 * @mapping: the YAML mapping to read from
 * @key: the key to look up
 * @self: the config object
 * @prop_name: the GObject property name to set
 *
 * Helper to apply a string member from a YAML mapping to a
 * GObject property if the key is present.
 */
static void
apply_string_member(
	YamlMapping   *mapping,
	const gchar   *key,
	GowlbarConfig *self,
	const gchar   *prop_name
){
	const gchar *val;

	if (!yaml_mapping_has_member(mapping, key))
		return;

	val = yaml_mapping_get_string_member(mapping, key);
	if (val != NULL)
		g_object_set(self, prop_name, val, NULL);
}

/**
 * gowlbar_config_apply_mapping:
 * @self: a #GowlbarConfig
 * @mapping: a #YamlMapping containing top-level config keys
 *
 * Walks the YAML mapping and applies recognised keys to the
 * corresponding GObject properties.
 */
static void
gowlbar_config_apply_mapping(
	GowlbarConfig *self,
	YamlMapping   *mapping
){
	/* Bar section */
	if (yaml_mapping_has_member(mapping, "bar")) {
		YamlMapping *bar = yaml_mapping_get_mapping_member(mapping, "bar");
		if (bar != NULL) {
			if (yaml_mapping_has_member(bar, "height")) {
				gint64 val = yaml_mapping_get_int_member(bar, "height");
				g_object_set(self, "height", (gint)val, NULL);
			}
			apply_string_member(bar, "position", self, "position");
			apply_string_member(bar, "font", self, "font");
			if (yaml_mapping_has_member(bar, "padding")) {
				gint64 val = yaml_mapping_get_int_member(bar, "padding");
				g_object_set(self, "padding", (gint)val, NULL);
			}
		}
	}

	/* Colours section */
	if (yaml_mapping_has_member(mapping, "colors")) {
		YamlMapping *colors = yaml_mapping_get_mapping_member(
			mapping, "colors");
		if (colors != NULL) {
			apply_string_member(colors, "background", self, "background");
			apply_string_member(colors, "foreground", self, "foreground");
			apply_string_member(colors, "tag-active-bg", self, "tag-active-bg");
			apply_string_member(colors, "tag-active-fg", self, "tag-active-fg");
			apply_string_member(colors, "tag-urgent-bg", self, "tag-urgent-bg");
			apply_string_member(colors, "tag-urgent-fg", self, "tag-urgent-fg");
			apply_string_member(colors, "tag-occupied-fg", self, "tag-occupied-fg");
			apply_string_member(colors, "tag-empty-fg", self, "tag-empty-fg");
			apply_string_member(colors, "layout-fg", self, "layout-fg");
			apply_string_member(colors, "title-fg", self, "title-fg");
			apply_string_member(colors, "status-fg", self, "status-fg");
			apply_string_member(colors, "border-color", self, "border-color");
		}
	}

	/* Also support flat keys for top-level overrides from C config */
	if (yaml_mapping_has_member(mapping, "height")) {
		gint64 val = yaml_mapping_get_int_member(mapping, "height");
		g_object_set(self, "height", (gint)val, NULL);
	}
	apply_string_member(mapping, "position", self, "position");
	apply_string_member(mapping, "font", self, "font");
	if (yaml_mapping_has_member(mapping, "padding")) {
		gint64 val = yaml_mapping_get_int_member(mapping, "padding");
		g_object_set(self, "padding", (gint)val, NULL);
	}
	apply_string_member(mapping, "background", self, "background");
	apply_string_member(mapping, "foreground", self, "foreground");
}

/**
 * gowlbar_config_load_yaml:
 * @self: a #GowlbarConfig
 * @path: filesystem path to a YAML configuration file
 * @error: (nullable): return location for a #GError
 *
 * Parses the YAML file at @path using yaml-glib and applies the
 * top-level mapping to the config properties.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
gowlbar_config_load_yaml(
	GowlbarConfig  *self,
	const gchar    *path,
	GError        **error
){
	g_autoptr(YamlParser) parser = NULL;
	YamlNode *root = NULL;
	YamlMapping *mapping = NULL;

	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), FALSE);
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

	gowlbar_config_apply_mapping(self, mapping);

	/* Emit the reloaded signal */
	g_signal_emit(self, signals[SIGNAL_RELOADED], 0);
	return TRUE;
}

/**
 * gowlbar_config_load_yaml_from_search_path:
 * @self: a #GowlbarConfig
 * @error: (nullable): return location for a #GError
 *
 * Searches standard directories for bar.yaml and loads the first
 * one found. If no file exists, the config keeps its defaults and
 * the function returns %TRUE.
 *
 * Returns: %TRUE on success (including no-file-found), %FALSE on error
 */
gboolean
gowlbar_config_load_yaml_from_search_path(
	GowlbarConfig  *self,
	GError        **error
){
	g_autofree gchar *xdg_path = NULL;
	const gchar *search_paths[5];
	guint i;

	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), FALSE);

	/* Build the XDG config path: ~/.config/gowl/bar.yaml */
	xdg_path = g_build_filename(g_get_user_config_dir(),
	                             "gowl",
	                             GOWLBAR_CONFIG_FILENAME,
	                             NULL);

	search_paths[0] = "data/" GOWLBAR_CONFIG_FILENAME;
	search_paths[1] = xdg_path;
	search_paths[2] = "/etc/gowl/" GOWLBAR_CONFIG_FILENAME;
	search_paths[3] = "/usr/local/gowl/" GOWLBAR_CONFIG_FILENAME;
	search_paths[4] = NULL;

	for (i = 0; search_paths[i] != NULL; i++) {
		if (g_file_test(search_paths[i], G_FILE_TEST_EXISTS)) {
			g_debug("gowlbar_config: loading config from '%s'",
			        search_paths[i]);
			return gowlbar_config_load_yaml(self,
			                                 search_paths[i],
			                                 error);
		}
	}

	g_debug("gowlbar_config: no config file found, using defaults");
	return TRUE;
}

/* --- YAML generation --- */

/**
 * gowlbar_config_generate_yaml:
 * @self: a #GowlbarConfig
 *
 * Builds a YAML representation of the current config state.
 *
 * Returns: (transfer full): a newly allocated YAML string
 */
gchar *
gowlbar_config_generate_yaml(GowlbarConfig *self)
{
	GString *yaml;

	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), NULL);

	yaml = g_string_new("# gowlbar configuration\n\n");

	/* Bar section */
	g_string_append(yaml, "bar:\n");
	g_string_append_printf(yaml, "  height: %d\n", self->height);
	g_string_append_printf(yaml, "  position: \"%s\"\n", self->position);
	g_string_append_printf(yaml, "  font: \"%s\"\n", self->font);
	g_string_append_printf(yaml, "  padding: %d\n", self->padding);

	/* Colours section */
	g_string_append(yaml, "\ncolors:\n");
	g_string_append_printf(yaml, "  background: \"%s\"\n", self->background);
	g_string_append_printf(yaml, "  foreground: \"%s\"\n", self->foreground);
	g_string_append_printf(yaml, "  tag-active-bg: \"%s\"\n", self->tag_active_bg);
	g_string_append_printf(yaml, "  tag-active-fg: \"%s\"\n", self->tag_active_fg);
	g_string_append_printf(yaml, "  tag-urgent-bg: \"%s\"\n", self->tag_urgent_bg);
	g_string_append_printf(yaml, "  tag-urgent-fg: \"%s\"\n", self->tag_urgent_fg);
	g_string_append_printf(yaml, "  tag-occupied-fg: \"%s\"\n", self->tag_occupied_fg);
	g_string_append_printf(yaml, "  tag-empty-fg: \"%s\"\n", self->tag_empty_fg);
	g_string_append_printf(yaml, "  layout-fg: \"%s\"\n", self->layout_fg);
	g_string_append_printf(yaml, "  title-fg: \"%s\"\n", self->title_fg);
	g_string_append_printf(yaml, "  status-fg: \"%s\"\n", self->status_fg);
	g_string_append_printf(yaml, "  border-color: \"%s\"\n", self->border_color);

	/* Modules placeholder */
	g_string_append(yaml, "\nmodules: {}\n");

	return g_string_free(yaml, FALSE);
}

/* --- Property getters --- */

gint
gowlbar_config_get_height(GowlbarConfig *self)
{
	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), GOWLBAR_CONFIG_DEFAULT_HEIGHT);
	return self->height;
}

const gchar *
gowlbar_config_get_position(GowlbarConfig *self)
{
	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), GOWLBAR_CONFIG_DEFAULT_POSITION);
	return self->position;
}

const gchar *
gowlbar_config_get_font(GowlbarConfig *self)
{
	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), GOWLBAR_CONFIG_DEFAULT_FONT);
	return self->font;
}

gint
gowlbar_config_get_padding(GowlbarConfig *self)
{
	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), GOWLBAR_CONFIG_DEFAULT_PADDING);
	return self->padding;
}

const gchar *
gowlbar_config_get_background(GowlbarConfig *self)
{
	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), GOWLBAR_CONFIG_DEFAULT_BG);
	return self->background;
}

const gchar *
gowlbar_config_get_foreground(GowlbarConfig *self)
{
	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), GOWLBAR_CONFIG_DEFAULT_FG);
	return self->foreground;
}

const gchar *
gowlbar_config_get_tag_active_bg(GowlbarConfig *self)
{
	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), GOWLBAR_CONFIG_DEFAULT_TAG_ACTIVE_BG);
	return self->tag_active_bg;
}

const gchar *
gowlbar_config_get_tag_active_fg(GowlbarConfig *self)
{
	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), GOWLBAR_CONFIG_DEFAULT_TAG_ACTIVE_FG);
	return self->tag_active_fg;
}

const gchar *
gowlbar_config_get_tag_urgent_bg(GowlbarConfig *self)
{
	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), GOWLBAR_CONFIG_DEFAULT_TAG_URGENT_BG);
	return self->tag_urgent_bg;
}

const gchar *
gowlbar_config_get_tag_urgent_fg(GowlbarConfig *self)
{
	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), GOWLBAR_CONFIG_DEFAULT_TAG_URGENT_FG);
	return self->tag_urgent_fg;
}

const gchar *
gowlbar_config_get_tag_occupied_fg(GowlbarConfig *self)
{
	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), GOWLBAR_CONFIG_DEFAULT_TAG_OCCUPIED_FG);
	return self->tag_occupied_fg;
}

const gchar *
gowlbar_config_get_tag_empty_fg(GowlbarConfig *self)
{
	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), GOWLBAR_CONFIG_DEFAULT_TAG_EMPTY_FG);
	return self->tag_empty_fg;
}

const gchar *
gowlbar_config_get_layout_fg(GowlbarConfig *self)
{
	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), GOWLBAR_CONFIG_DEFAULT_LAYOUT_FG);
	return self->layout_fg;
}

const gchar *
gowlbar_config_get_title_fg(GowlbarConfig *self)
{
	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), GOWLBAR_CONFIG_DEFAULT_TITLE_FG);
	return self->title_fg;
}

const gchar *
gowlbar_config_get_status_fg(GowlbarConfig *self)
{
	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), GOWLBAR_CONFIG_DEFAULT_STATUS_FG);
	return self->status_fg;
}

const gchar *
gowlbar_config_get_border_color(GowlbarConfig *self)
{
	g_return_val_if_fail(GOWLBAR_IS_CONFIG(self), GOWLBAR_CONFIG_DEFAULT_BORDER_COLOR);
	return self->border_color;
}
