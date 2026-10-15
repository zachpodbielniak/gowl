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

/*
 * gowl-floatdesk-state.c - the floating desktop's state and decisions.
 *
 * See the header.  Kept free of wlroots and of GowlClient so that
 * tests/test-floatdesk-state.c can drive every transition -- enter,
 * exit, a window moved in and out, the ownership flag, the state file's
 * round trip -- by filling in a plain struct.
 */

#include <errno.h>
#include <string.h>
#include <glib/gstdio.h>

#include "yaml-glib.h"

#include "gowl-floatdesk-state.h"

/* An error domain of its own, so a caller can tell a bad state file
 * from an I/O failure in the same GError. */
#define GOWL_FLOATDESK_STATE_ERROR (gowl_floatdesk_state_error_quark())

static GQuark
gowl_floatdesk_state_error_quark(void)
{
	return g_quark_from_static_string("gowl-floatdesk-state-error");
}

enum {
	GOWL_FLOATDESK_STATE_ERROR_PARSE = 1,
	GOWL_FLOATDESK_STATE_ERROR_VERSION
};

struct _GowlFloatdeskState {
	/* Per-tag explicit settings, GOWL_FLOATDESK_MODE_UNSET when none. */
	gint8    mode[GOWL_FLOATDESK_MAX_TAGS];
	/* The layout each tag had before it floated; NULL when unknown. */
	gchar   *previous[GOWL_FLOATDESK_MAX_TAGS];
	gboolean default_floating;

	/* Dock settings changed at run time; NULL / -1 = not changed, the
	 * config decides. */
	gchar   *dock_position;
	gint     dock_autohide;
	gchar  **dock_pins;
};

/* --- Lifetime ---------------------------------------------------- */

GowlFloatdeskState *
gowl_floatdesk_state_new(void)
{
	GowlFloatdeskState *self;
	guint               i;

	self = g_new0(GowlFloatdeskState, 1);
	for (i = 0; i < GOWL_FLOATDESK_MAX_TAGS; i++)
		self->mode[i] = (gint8)GOWL_FLOATDESK_MODE_UNSET;
	self->dock_autohide = -1;
	return self;
}

/*
 * state_clear:
 * @self: a state
 *
 * Frees what @self owns and puts it back to gowl_floatdesk_state_new()'s
 * shape, so a load can build into a scratch state and then take its
 * place without a second copy of the reset logic.
 */
static void
state_clear(GowlFloatdeskState *self)
{
	guint i;

	for (i = 0; i < GOWL_FLOATDESK_MAX_TAGS; i++) {
		g_clear_pointer(&self->previous[i], g_free);
		self->mode[i] = (gint8)GOWL_FLOATDESK_MODE_UNSET;
	}
	self->default_floating = FALSE;
	g_clear_pointer(&self->dock_position, g_free);
	g_clear_pointer(&self->dock_pins, g_strfreev);
	self->dock_autohide = -1;
}

void
gowl_floatdesk_state_free(GowlFloatdeskState *self)
{
	if (self == NULL)
		return;
	state_clear(self);
	g_free(self);
}

/*
 * state_take:
 * @self: the state to replace
 * @from: (transfer full): the state replacing it
 *
 * Moves @from's contents into @self and frees the shell of @from.
 */
static void
state_take(
	GowlFloatdeskState *self,
	GowlFloatdeskState *from
){
	state_clear(self);
	memcpy(self, from, sizeof(*self));
	g_free(from);
}

/* --- Modes ------------------------------------------------------- */

gboolean
gowl_floatdesk_state_get_default_floating(const GowlFloatdeskState *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	return self->default_floating;
}

void
gowl_floatdesk_state_set_default_floating(
	GowlFloatdeskState *self,
	gboolean            floating
){
	g_return_if_fail(self != NULL);
	self->default_floating = floating ? TRUE : FALSE;
}

GowlFloatdeskMode
gowl_floatdesk_state_get_mode(
	const GowlFloatdeskState *self,
	guint                     tag
){
	g_return_val_if_fail(self != NULL, GOWL_FLOATDESK_MODE_UNSET);

	if (tag >= GOWL_FLOATDESK_MAX_TAGS)
		return GOWL_FLOATDESK_MODE_UNSET;
	return (GowlFloatdeskMode)self->mode[tag];
}

void
gowl_floatdesk_state_set_mode(
	GowlFloatdeskState *self,
	guint               tag,
	GowlFloatdeskMode   mode
){
	g_return_if_fail(self != NULL);

	if (tag >= GOWL_FLOATDESK_MAX_TAGS)
		return;
	if (mode != GOWL_FLOATDESK_MODE_TILED
	    && mode != GOWL_FLOATDESK_MODE_FLOATING)
		mode = GOWL_FLOATDESK_MODE_UNSET;
	self->mode[tag] = (gint8)mode;
}

gboolean
gowl_floatdesk_state_tag_floating(
	const GowlFloatdeskState *self,
	guint                     tag
){
	GowlFloatdeskMode mode;

	g_return_val_if_fail(self != NULL, FALSE);

	if (tag >= GOWL_FLOATDESK_MAX_TAGS)
		return FALSE;
	mode = (GowlFloatdeskMode)self->mode[tag];
	if (mode == GOWL_FLOATDESK_MODE_UNSET)
		return self->default_floating;
	return mode == GOWL_FLOATDESK_MODE_FLOATING;
}

gboolean
gowl_floatdesk_state_mask_floating(
	const GowlFloatdeskState *self,
	guint32                   mask
){
	guint tag;

	g_return_val_if_fail(self != NULL, FALSE);

	/* No tags is no workspace: a minimized window or a hidden overlay,
	 * neither of which the mode may claim. */
	if (mask == 0)
		return FALSE;
	for (tag = 0; tag < GOWL_FLOATDESK_MAX_TAGS; tag++) {
		if ((mask & ((guint32)1u << tag)) == 0)
			continue;
		if (!gowl_floatdesk_state_tag_floating(self, tag))
			return FALSE;
	}
	return TRUE;
}

void
gowl_floatdesk_state_set_mask(
	GowlFloatdeskState *self,
	guint32             mask,
	gboolean            floating
){
	guint tag;

	g_return_if_fail(self != NULL);

	for (tag = 0; tag < GOWL_FLOATDESK_MAX_TAGS; tag++) {
		if ((mask & ((guint32)1u << tag)) != 0)
			self->mode[tag] = (gint8)(floating
				? GOWL_FLOATDESK_MODE_FLOATING
				: GOWL_FLOATDESK_MODE_TILED);
	}
}

const gchar *
gowl_floatdesk_state_get_previous_layout(
	const GowlFloatdeskState *self,
	guint                     tag
){
	g_return_val_if_fail(self != NULL, NULL);

	if (tag >= GOWL_FLOATDESK_MAX_TAGS)
		return NULL;
	return self->previous[tag];
}

void
gowl_floatdesk_state_set_previous_layout(
	GowlFloatdeskState *self,
	guint               tag,
	const gchar        *layout
){
	g_return_if_fail(self != NULL);

	if (tag >= GOWL_FLOATDESK_MAX_TAGS)
		return;
	/* Going "back" to the mode's own layout would leave the workspace
	 * in the mode, which is not leaving it. */
	if (g_strcmp0(layout, GOWL_FLOATDESK_LAYOUT) == 0)
		return;
	g_free(self->previous[tag]);
	self->previous[tag] = (layout != NULL && *layout != '\0')
		? g_strdup(layout) : NULL;
}

const gchar *
gowl_floatdesk_state_previous_layout_for_mask(
	const GowlFloatdeskState *self,
	guint32                   mask
){
	guint tag;

	g_return_val_if_fail(self != NULL, NULL);

	for (tag = 0; tag < GOWL_FLOATDESK_MAX_TAGS; tag++) {
		if ((mask & ((guint32)1u << tag)) != 0
		    && self->previous[tag] != NULL)
			return self->previous[tag];
	}
	return NULL;
}

/* --- The ownership rule ------------------------------------------ */

GowlFloatdeskAction
gowl_floatdesk_state_decide(
	const GowlFloatdeskState      *self,
	const GowlFloatdeskClientView *client
){
	gboolean on_floating;

	g_return_val_if_fail(self != NULL, GOWL_FLOATDESK_ACTION_NONE);
	g_return_val_if_fail(client != NULL, GOWL_FLOATDESK_ACTION_NONE);

	/* Not the mode's to touch at all.  A minimized window keeps its
	 * mark: it is on no tag only until it comes back, and where it
	 * comes back decides. */
	if (client->overlay || client->unmanaged || client->embedded
	    || client->minimized || client->tags == 0)
		return GOWL_FLOATDESK_ACTION_NONE;

	on_floating = gowl_floatdesk_state_mask_floating(self, client->tags);

	if (on_floating) {
		/* A fullscreen window is floated when it leaves fullscreen:
		 * that exit arranges again and comes back through here. */
		if (!client->floating && !client->fullscreen)
			return GOWL_FLOATDESK_ACTION_FLOAT;
		return GOWL_FLOATDESK_ACTION_NONE;
	}

	/* A tiling workspace: give back only what the mode took. */
	if (!client->mode_floated)
		return GOWL_FLOATDESK_ACTION_NONE;
	if (client->floating)
		return GOWL_FLOATDESK_ACTION_UNFLOAT;
	return GOWL_FLOATDESK_ACTION_CLEAR;
}

/* --- Dock settings ----------------------------------------------- */

const gchar *
gowl_floatdesk_state_get_dock_position(const GowlFloatdeskState *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->dock_position;
}

gboolean
gowl_floatdesk_state_set_dock_position(
	GowlFloatdeskState *self,
	const gchar        *position
){
	g_return_val_if_fail(self != NULL, FALSE);

	if (position != NULL
	    && g_strcmp0(position, "bottom") != 0
	    && g_strcmp0(position, "top") != 0
	    && g_strcmp0(position, "left") != 0
	    && g_strcmp0(position, "right") != 0)
		return FALSE;
	g_free(self->dock_position);
	self->dock_position = g_strdup(position);
	return TRUE;
}

gint
gowl_floatdesk_state_get_dock_autohide(const GowlFloatdeskState *self)
{
	g_return_val_if_fail(self != NULL, -1);
	return self->dock_autohide;
}

void
gowl_floatdesk_state_set_dock_autohide(
	GowlFloatdeskState *self,
	gint                autohide
){
	g_return_if_fail(self != NULL);
	self->dock_autohide = autohide < 0 ? -1 : (autohide ? 1 : 0);
}

const gchar *const *
gowl_floatdesk_state_get_dock_pins(const GowlFloatdeskState *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return (const gchar *const *)self->dock_pins;
}

void
gowl_floatdesk_state_set_dock_pins(
	GowlFloatdeskState *self,
	const gchar *const *pins
){
	g_return_if_fail(self != NULL);

	g_strfreev(self->dock_pins);
	self->dock_pins = pins != NULL ? g_strdupv((gchar **)pins) : NULL;
}

/* --- The state file ---------------------------------------------- */

/*
 * yaml_quote:
 * @out: where to append
 * @text: a string
 *
 * Appends @text as a double-quoted YAML scalar.  App ids and layout
 * names are usually plain words, but nothing stops one containing a
 * colon or a quote, and an unquoted one would then change the file's
 * structure.
 */
static void
yaml_quote(
	GString     *out,
	const gchar *text
){
	const gchar *p;

	g_string_append_c(out, '"');
	for (p = text; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_c(out, '\\');
		if (*p == '\n') {
			g_string_append(out, "\\n");
			continue;
		}
		g_string_append_c(out, *p);
	}
	g_string_append_c(out, '"');
}

gchar *
gowl_floatdesk_state_to_yaml(const GowlFloatdeskState *self)
{
	GString *out;
	guint    tag;
	gboolean any_tag = FALSE;

	g_return_val_if_fail(self != NULL, NULL);

	out = g_string_new("# gowl floating desktop -- written by the floatdesk "
	                   "module; edits are\n# read back at the next start.\n");
	g_string_append_printf(out, "version: %d\n", GOWL_FLOATDESK_STATE_VERSION);
	g_string_append_printf(out, "default-floating: %s\n",
	                       self->default_floating ? "true" : "false");

	/* Only tags with something set.  Writing every tag as "tiled"
	 * would freeze them all against a later change of the default. */
	for (tag = 0; tag < GOWL_FLOATDESK_MAX_TAGS; tag++) {
		if (self->mode[tag] == (gint8)GOWL_FLOATDESK_MODE_UNSET
		    && self->previous[tag] == NULL)
			continue;
		if (!any_tag) {
			g_string_append(out, "tags:\n");
			any_tag = TRUE;
		}
		g_string_append_printf(out, "  - tag: %u\n", tag + 1);
		if (self->mode[tag] != (gint8)GOWL_FLOATDESK_MODE_UNSET)
			g_string_append_printf(out, "    mode: %s\n",
				self->mode[tag] == (gint8)GOWL_FLOATDESK_MODE_FLOATING
				? "floating" : "tiled");
		if (self->previous[tag] != NULL) {
			g_string_append(out, "    previous-layout: ");
			yaml_quote(out, self->previous[tag]);
			g_string_append_c(out, '\n');
		}
	}

	if (self->dock_position != NULL || self->dock_autohide >= 0
	    || self->dock_pins != NULL) {
		g_string_append(out, "dock:\n");
		if (self->dock_position != NULL)
			g_string_append_printf(out, "  position: %s\n",
			                       self->dock_position);
		if (self->dock_autohide >= 0)
			g_string_append_printf(out, "  autohide: %s\n",
			                       self->dock_autohide ? "true" : "false");
		if (self->dock_pins != NULL) {
			guint i;

			if (self->dock_pins[0] == NULL) {
				g_string_append(out, "  pins: []\n");
			} else {
				g_string_append(out, "  pins:\n");
				for (i = 0; self->dock_pins[i] != NULL; i++) {
					g_string_append(out, "    - ");
					yaml_quote(out, self->dock_pins[i]);
					g_string_append_c(out, '\n');
				}
			}
		}
	}
	return g_string_free(out, FALSE);
}

/*
 * parse_bool:
 * @text: (nullable): a YAML scalar
 * @out: (out): the value
 *
 * Returns: %TRUE if @text is a boolean YAML 1.1 or 1.2 would read as one
 */
static gboolean
parse_bool(
	const gchar *text,
	gboolean    *out
){
	if (text == NULL)
		return FALSE;
	if (g_ascii_strcasecmp(text, "true") == 0
	    || g_ascii_strcasecmp(text, "yes") == 0
	    || g_ascii_strcasecmp(text, "on") == 0) {
		*out = TRUE;
		return TRUE;
	}
	if (g_ascii_strcasecmp(text, "false") == 0
	    || g_ascii_strcasecmp(text, "no") == 0
	    || g_ascii_strcasecmp(text, "off") == 0) {
		*out = FALSE;
		return TRUE;
	}
	return FALSE;
}

/*
 * scalar_member:
 * @map: a mapping
 * @key: a key
 *
 * Returns: (nullable): the scalar text at @key, or %NULL when it is
 *   missing or not a scalar
 */
static const gchar *
scalar_member(
	YamlMapping *map,
	const gchar *key
){
	YamlNode *node;

	if (map == NULL || !yaml_mapping_has_member(map, key))
		return NULL;
	node = yaml_mapping_get_member(map, key);
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
		return NULL;
	return yaml_node_get_scalar(node);
}

/*
 * load_tags:
 * @into: the scratch state
 * @root: the document's root mapping
 * @error: return location for a #GError
 *
 * Reads `tags:'.  Each entry needs a `tag:' from 1 to 32; a bad entry
 * fails the whole load, because a half-read file that then gets saved
 * back loses whatever was skipped.
 */
static gboolean
load_tags(
	GowlFloatdeskState *into,
	YamlMapping        *root,
	GError            **error
){
	YamlNode     *node;
	YamlSequence *seq;
	guint         i, n;

	if (!yaml_mapping_has_member(root, "tags"))
		return TRUE;
	node = yaml_mapping_get_member(root, "tags");
	if (node == NULL || yaml_node_get_node_type(node) == YAML_NODE_NULL)
		return TRUE;
	seq = yaml_node_get_sequence(node);
	if (seq == NULL) {
		g_set_error(error, GOWL_FLOATDESK_STATE_ERROR,
		            GOWL_FLOATDESK_STATE_ERROR_PARSE,
		            "`tags' must be a list");
		return FALSE;
	}

	n = yaml_sequence_get_length(seq);
	for (i = 0; i < n; i++) {
		YamlMapping *entry;
		const gchar *text;
		gchar       *end = NULL;
		gint64       number;
		guint        tag;

		entry = yaml_sequence_get_mapping_element(seq, i);
		text = scalar_member(entry, "tag");
		if (text == NULL) {
			g_set_error(error, GOWL_FLOATDESK_STATE_ERROR,
			            GOWL_FLOATDESK_STATE_ERROR_PARSE,
			            "tags entry %u has no `tag'", i + 1);
			return FALSE;
		}
		number = g_ascii_strtoll(text, &end, 10);
		if (end == text || *end != '\0' || number < 1
		    || number > GOWL_FLOATDESK_MAX_TAGS) {
			g_set_error(error, GOWL_FLOATDESK_STATE_ERROR,
			            GOWL_FLOATDESK_STATE_ERROR_PARSE,
			            "tags entry %u: `tag' must be 1 to %d, not \"%s\"",
			            i + 1, GOWL_FLOATDESK_MAX_TAGS, text);
			return FALSE;
		}
		tag = (guint)(number - 1);

		text = scalar_member(entry, "mode");
		if (g_strcmp0(text, "floating") == 0)
			into->mode[tag] = (gint8)GOWL_FLOATDESK_MODE_FLOATING;
		else if (g_strcmp0(text, "tiled") == 0)
			into->mode[tag] = (gint8)GOWL_FLOATDESK_MODE_TILED;
		else if (text != NULL && g_strcmp0(text, "unset") != 0) {
			g_set_error(error, GOWL_FLOATDESK_STATE_ERROR,
			            GOWL_FLOATDESK_STATE_ERROR_PARSE,
			            "tag %u: `mode' must be floating or tiled, "
			            "not \"%s\"", tag + 1, text);
			return FALSE;
		}

		gowl_floatdesk_state_set_previous_layout(into, tag,
			scalar_member(entry, "previous-layout"));
	}
	return TRUE;
}

/*
 * load_dock:
 * @into: the scratch state
 * @root: the document's root mapping
 * @error: return location for a #GError
 *
 * Reads `dock:' -- the settings changed at run time.
 */
static gboolean
load_dock(
	GowlFloatdeskState *into,
	YamlMapping        *root,
	GError            **error
){
	YamlMapping *dock;
	YamlNode    *node;
	const gchar *text;
	gboolean     flag;

	if (!yaml_mapping_has_member(root, "dock"))
		return TRUE;
	dock = yaml_mapping_get_mapping_member(root, "dock");
	if (dock == NULL)
		return TRUE;

	text = scalar_member(dock, "position");
	if (text != NULL && !gowl_floatdesk_state_set_dock_position(into, text)) {
		g_set_error(error, GOWL_FLOATDESK_STATE_ERROR,
		            GOWL_FLOATDESK_STATE_ERROR_PARSE,
		            "dock: `position' must be bottom, top, left or "
		            "right, not \"%s\"", text);
		return FALSE;
	}

	text = scalar_member(dock, "autohide");
	if (text != NULL) {
		if (!parse_bool(text, &flag)) {
			g_set_error(error, GOWL_FLOATDESK_STATE_ERROR,
			            GOWL_FLOATDESK_STATE_ERROR_PARSE,
			            "dock: `autohide' must be true or false, "
			            "not \"%s\"", text);
			return FALSE;
		}
		into->dock_autohide = flag ? 1 : 0;
	}

	if (yaml_mapping_has_member(dock, "pins")) {
		YamlSequence *seq;
		GPtrArray    *pins;
		guint         i, n;

		node = yaml_mapping_get_member(dock, "pins");
		seq = node != NULL ? yaml_node_get_sequence(node) : NULL;
		pins = g_ptr_array_new();
		n = seq != NULL ? yaml_sequence_get_length(seq) : 0;
		for (i = 0; i < n; i++) {
			const gchar *id = yaml_sequence_get_string_element(seq, i);

			if (id != NULL && *id != '\0')
				g_ptr_array_add(pins, g_strdup(id));
		}
		g_ptr_array_add(pins, NULL);
		g_strfreev(into->dock_pins);
		into->dock_pins = (gchar **)g_ptr_array_free(pins, FALSE);
	}
	return TRUE;
}

gboolean
gowl_floatdesk_state_load_yaml(
	GowlFloatdeskState *self,
	const gchar        *text,
	GError            **error
){
	g_autoptr(YamlParser) parser = NULL;
	GowlFloatdeskState   *scratch;
	YamlNode             *root;
	YamlMapping          *map;
	const gchar          *value;
	gboolean              flag;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(text != NULL, FALSE);

	parser = yaml_parser_new();
	if (!yaml_parser_load_from_data(parser, text, -1, error))
		return FALSE;

	/* An empty file is an empty state, not an error. */
	root = yaml_parser_get_root(parser);
	if (root == NULL || yaml_node_get_node_type(root) == YAML_NODE_NULL) {
		state_clear(self);
		return TRUE;
	}
	map = yaml_node_get_mapping(root);
	if (map == NULL) {
		g_set_error(error, GOWL_FLOATDESK_STATE_ERROR,
		            GOWL_FLOATDESK_STATE_ERROR_PARSE,
		            "the state file is not a mapping");
		return FALSE;
	}

	value = scalar_member(map, "version");
	if (value != NULL
	    && g_ascii_strtoll(value, NULL, 10) > GOWL_FLOATDESK_STATE_VERSION) {
		g_set_error(error, GOWL_FLOATDESK_STATE_ERROR,
		            GOWL_FLOATDESK_STATE_ERROR_VERSION,
		            "the state file is version %s; this gowl reads %d",
		            value, GOWL_FLOATDESK_STATE_VERSION);
		return FALSE;
	}

	/* Build into a scratch state: a file that fails half way through
	 * must leave the running state exactly as it was. */
	scratch = gowl_floatdesk_state_new();
	value = scalar_member(map, "default-floating");
	if (value != NULL) {
		if (!parse_bool(value, &flag)) {
			g_set_error(error, GOWL_FLOATDESK_STATE_ERROR,
			            GOWL_FLOATDESK_STATE_ERROR_PARSE,
			            "`default-floating' must be true or false, "
			            "not \"%s\"", value);
			gowl_floatdesk_state_free(scratch);
			return FALSE;
		}
		scratch->default_floating = flag;
	}
	if (!load_tags(scratch, map, error) || !load_dock(scratch, map, error)) {
		gowl_floatdesk_state_free(scratch);
		return FALSE;
	}

	state_take(self, scratch);
	return TRUE;
}

gboolean
gowl_floatdesk_state_save(
	const GowlFloatdeskState *self,
	const gchar              *path,
	GError                  **error
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *dir = NULL;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	dir = g_path_get_dirname(path);
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		gint saved = errno;

		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved),
		            "could not create %s: %s", dir, g_strerror(saved));
		return FALSE;
	}
	text = gowl_floatdesk_state_to_yaml(self);
	return g_file_set_contents(path, text, -1, error);
}

gboolean
gowl_floatdesk_state_load(
	GowlFloatdeskState *self,
	const gchar        *path,
	GError            **error
){
	g_autofree gchar *text = NULL;
	GError           *local = NULL;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	if (!g_file_get_contents(path, &text, NULL, &local)) {
		if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
			g_error_free(local);
			return TRUE;
		}
		g_propagate_error(error, local);
		return FALSE;
	}
	if (!gowl_floatdesk_state_load_yaml(self, text, &local)) {
		g_propagate_prefixed_error(error, local, "%s: ", path);
		return FALSE;
	}
	return TRUE;
}

gchar *
gowl_floatdesk_state_default_path(void)
{
	const gchar *env;

	env = g_getenv("GOWL_FLOATDESK_STATE");
	if (env != NULL && *env != '\0')
		return g_strdup(env);
	return g_build_filename(g_get_user_state_dir(), "gowl",
	                        "floatdesk.yaml", NULL);
}

/* --- Placement arithmetic ---------------------------------------- */

void
gowl_floatdesk_place_cascade(
	gint      area_x,
	gint      area_y,
	gint      area_w,
	gint      area_h,
	gint      width,
	gint      height,
	gboolean  have_last,
	gint      last_x,
	gint      last_y,
	gint      step,
	gint     *out_x,
	gint     *out_y
){
	gint cx, cy, nx, ny;

	g_return_if_fail(out_x != NULL && out_y != NULL);

	cx = area_x + (area_w - width) / 2;
	cy = area_y + (area_h - height) / 2;

	/* A window bigger than the area: pin it to the top-left so its
	 * title bar, at least, is reachable. */
	if (width >= area_w)
		cx = area_x;
	if (height >= area_h)
		cy = area_y;

	if (!have_last) {
		*out_x = cx;
		*out_y = cy;
		return;
	}

	nx = last_x + step;
	ny = last_y + step;
	/* Off the bottom or the right: start the cascade over. */
	if (nx < area_x || ny < area_y
	    || nx + width > area_x + area_w || ny + height > area_y + area_h) {
		*out_x = cx;
		*out_y = cy;
		return;
	}
	*out_x = nx;
	*out_y = ny;
}

GowlFloatdeskSnap
gowl_floatdesk_snap_at(
	gint    mon_x,
	gint    mon_y,
	gint    mon_w,
	gint    mon_h,
	gdouble px,
	gdouble py,
	gint    threshold
){
	/* A pointer off this output says nothing about its edges. */
	if (px < mon_x || py < mon_y || px >= mon_x + mon_w
	    || py >= mon_y + mon_h)
		return GOWL_FLOATDESK_SNAP_NONE;
	if (py < mon_y + threshold)
		return GOWL_FLOATDESK_SNAP_MAXIMIZE;
	if (px < mon_x + threshold)
		return GOWL_FLOATDESK_SNAP_LEFT;
	if (px >= mon_x + mon_w - threshold)
		return GOWL_FLOATDESK_SNAP_RIGHT;
	return GOWL_FLOATDESK_SNAP_NONE;
}
