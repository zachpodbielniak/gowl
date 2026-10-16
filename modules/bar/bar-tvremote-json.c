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
 * Path lookups into the JSON Samsung and LG send.
 *
 * Their messages nest what matters two or three objects deep
 * (`payload.volumeStatus.volume', `data.data[].appId'), and reading
 * those by scanning text is how a field missing from one object gets
 * answered from the next.  JSON-GLib is already linked by libgowl, so
 * these parse properly and walk by member name; a path that does not
 * exist, or a value of the wrong type, is simply "absent".
 */

#include <stdarg.h>
#include <string.h>
#include <json-glib/json-glib.h>

#include "bar-tvremote-model.h"

/* Walk @first and the varargs member names from the root object.
   Returns a node owned by @parser, or NULL. */
static JsonNode *
walk(JsonParser *parser, const gchar *json, const gchar *first, va_list ap)
{
	JsonNode *node;
	const gchar *name;

	if (json == NULL || !json_parser_load_from_data(parser, json, -1, NULL))
		return NULL;
	node = json_parser_get_root(parser);
	for (name = first; name != NULL; name = va_arg(ap, const gchar *)) {
		JsonObject *obj;

		if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
			return NULL;
		obj = json_node_get_object(node);
		if (!json_object_has_member(obj, name))
			return NULL;
		node = json_object_get_member(obj, name);
	}
	return node;
}

/* A string member of @o, or NULL when absent or not a string. */
static const gchar *
string_member(JsonObject *o, const gchar *name)
{
	JsonNode *m;

	if (!json_object_has_member(o, name))
		return NULL;
	m = json_object_get_member(o, name);
	if (m == NULL || !JSON_NODE_HOLDS_VALUE(m) ||
	    json_node_get_value_type(m) != G_TYPE_STRING)
		return NULL;
	return json_node_get_string(m);
}

/**
 * bar_tvremote_json_string:
 * @json: (nullable): a JSON document
 * @first: the first member name
 * @...: further member names, %NULL-terminated
 *
 * A number is returned as its decimal text: Samsung has sent its
 * pairing token both as a string and as a number.
 *
 * Returns: (transfer full) (nullable): the string at the path
 */
gchar *
bar_tvremote_json_string(const gchar *json, const gchar *first, ...)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *node;
	va_list ap;
	gchar *out = NULL;

	va_start(ap, first);
	node = walk(parser, json, first, ap);
	va_end(ap);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
		return NULL;
	if (json_node_get_value_type(node) == G_TYPE_STRING)
		out = g_strdup(json_node_get_string(node));
	else if (json_node_get_value_type(node) == G_TYPE_INT64)
		out = g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));
	return out;
}

/**
 * bar_tvremote_json_int:
 * @json: (nullable): a JSON document
 * @fallback: what to return when the path is absent or not a number
 * @first: the first member name
 * @...: further member names, %NULL-terminated
 *
 * Returns: the integer at the path, or @fallback
 */
gint64
bar_tvremote_json_int(const gchar *json, gint64 fallback, const gchar *first,
                      ...)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *node;
	va_list ap;

	va_start(ap, first);
	node = walk(parser, json, first, ap);
	va_end(ap);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
		return fallback;
	if (json_node_get_value_type(node) == G_TYPE_INT64)
		return json_node_get_int(node);
	if (json_node_get_value_type(node) == G_TYPE_DOUBLE)
		return (gint64)json_node_get_double(node);
	return fallback;
}

/**
 * bar_tvremote_json_bool:
 * @json: (nullable): a JSON document
 * @first: the first member name
 * @...: further member names, %NULL-terminated
 *
 * Returns: 1 or 0 for a boolean at the path, -1 when there is none
 */
gint
bar_tvremote_json_bool(const gchar *json, const gchar *first, ...)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *node;
	va_list ap;

	va_start(ap, first);
	node = walk(parser, json, first, ap);
	va_end(ap);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
	    json_node_get_value_type(node) != G_TYPE_BOOLEAN)
		return -1;
	return json_node_get_boolean(node) ? 1 : 0;
}

/**
 * bar_tvremote_json_apps:
 * @json: (nullable): a JSON document
 * @id_key: the member holding each app's id
 * @name_key: the member holding its name
 * @first: the first member name of the path to the ARRAY
 * @...: further member names, %NULL-terminated
 *
 * Collects { id, name } out of an array of objects.  Entries with an
 * id that is not a plausible app id are skipped -- the id is later put
 * into a launch request -- and at most 500 are taken.
 *
 * Returns: (transfer full) (element-type BarTvremoteApp): the apps,
 *   empty when the path is absent
 */
GPtrArray *
bar_tvremote_json_apps(const gchar *json, const gchar *id_key,
                       const gchar *name_key, const gchar *first, ...)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	GPtrArray *apps;
	JsonNode *node;
	JsonArray *arr;
	va_list ap;
	guint i, n;

	apps = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_tvremote_app_free);
	va_start(ap, first);
	node = walk(parser, json, first, ap);
	va_end(ap);
	if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
		return apps;
	arr = json_node_get_array(node);
	n = json_array_get_length(arr);
	for (i = 0; i < n && apps->len < 500; i++) {
		JsonNode *e = json_array_get_element(arr, i);
		JsonObject *o;
		const gchar *id, *name;

		if (!JSON_NODE_HOLDS_OBJECT(e))
			continue;
		o = json_node_get_object(e);
		id = string_member(o, id_key);
		name = string_member(o, name_key);
		if (!bar_tvremote_valid_app_id(id))
			continue;
		g_ptr_array_add(apps, bar_tvremote_app_new(id, name));
	}
	return apps;
}
