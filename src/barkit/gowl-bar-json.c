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

#include "barkit/gowl-bar-json.h"

#include <string.h>

/* Skip past a complete JSON string, starting at its opening quote.
   Used by every structural walk here so a brace, a bracket or a quote
   inside a value cannot unbalance the counter. */
static const gchar *
json_skip_string(const gchar *p)
{
	if (*p != '"')
		return p;
	p++;
	while (*p != '\0' && *p != '"') {
		if (*p == '\\' && p[1] != '\0')
			p++;
		p++;
	}
	return (*p == '"') ? p + 1 : p;
}

/* Find the value of "@key": in @json, returning a pointer to the first
   character of the value.  Anchors on a complete quoted key so a field
   named `Node' cannot match `NodeKey'. */
static const gchar *
json_find_value(const gchar *json, const gchar *key)
{
	g_autofree gchar *needle = NULL;
	const gchar *p;

	if (json == NULL || key == NULL)
		return NULL;

	needle = g_strdup_printf("\"%s\"", key);
	p = json;
	while ((p = strstr(p, needle)) != NULL) {
		const gchar *after = p + strlen(needle);

		while (*after == ' ' || *after == '\t' || *after == '\n' ||
		       *after == '\r')
			after++;
		if (*after != ':') {
			p = after;
			continue;
		}
		after++;
		while (*after == ' ' || *after == '\t' || *after == '\n' ||
		       *after == '\r')
			after++;
		return after;
	}
	return NULL;
}

/* Read the string starting at @p (which must be its opening quote),
   unescaping one level. */
static gchar *
json_read_string(const gchar *p)
{
	GString *out;

	if (*p != '"')
		return NULL;
	p++;

	out = g_string_new(NULL);
	while (*p != '\0' && *p != '"') {
		if (*p == '\\' && p[1] != '\0') {
			p++;
			switch (*p) {
			case 'n': g_string_append_c(out, '\n'); break;
			case 't': g_string_append_c(out, '\t'); break;
			case 'r': g_string_append_c(out, '\r'); break;
			default:  g_string_append_c(out, *p);   break;
			}
			p++;
			continue;
		}
		g_string_append_c(out, *p);
		p++;
	}
	return g_string_free(out, FALSE);
}

/* Return the extent of the bracketed value starting at @p, which must
   be '{' or '['. */
static const gchar *
json_skip_bracketed(const gchar *p)
{
	gchar open, close;
	gint depth;

	open  = *p;
	close = (open == '{') ? '}' : ']';
	depth = 1;
	p++;

	while (*p != '\0' && depth > 0) {
		if (*p == '"') {
			p = json_skip_string(p);
			continue;
		}
		if (*p == open)
			depth++;
		else if (*p == close)
			depth--;
		p++;
	}
	return p;
}

/**
 * gowl_bar_json_string:
 * @json: a JSON document or fragment
 * @key: the field name
 *
 * Returns: (transfer full) (nullable): the value
 */
gchar *
gowl_bar_json_string(const gchar *json, const gchar *key)
{
	const gchar *value;

	value = json_find_value(json, key);
	if (value == NULL || *value != '"')
		return NULL;
	return json_read_string(value);
}

/**
 * gowl_bar_json_bool:
 * @json: a JSON document or fragment
 * @key: the field name
 * @fallback: what to return when the field is absent
 *
 * Returns: the field's value, or @fallback
 */
gboolean
gowl_bar_json_bool(const gchar *json, const gchar *key, gboolean fallback)
{
	const gchar *value;

	value = json_find_value(json, key);
	if (value == NULL)
		return fallback;
	if (strncmp(value, "true", 4) == 0)
		return TRUE;
	if (strncmp(value, "false", 5) == 0)
		return FALSE;
	return fallback;
}

/**
 * gowl_bar_json_object:
 * @json: a JSON document or fragment
 * @key: the field name
 *
 * Returns: (transfer full) (nullable): the object, braces included
 */
gchar *
gowl_bar_json_object(const gchar *json, const gchar *key)
{
	const gchar *value;
	const gchar *end;

	value = json_find_value(json, key);
	if (value == NULL || *value != '{')
		return NULL;

	end = json_skip_bracketed(value);
	return g_strndup(value, (gsize)(end - value));
}

/**
 * gowl_bar_json_string_array:
 * @json: a JSON document or fragment
 * @key: the field name
 * @n_values: (out) (optional): how many entries
 *
 * Returns: (transfer full) (array zero-terminated=1) (nullable): the entries
 */
gchar **
gowl_bar_json_string_array(const gchar *json, const gchar *key,
                           guint *n_values)
{
	const gchar *value;
	const gchar *end;
	GPtrArray *out;

	if (n_values != NULL)
		*n_values = 0;

	value = json_find_value(json, key);
	if (value == NULL || *value != '[')
		return NULL;

	end = json_skip_bracketed(value);
	out = g_ptr_array_new();

	value++;
	while (value < end) {
		if (*value == '"') {
			g_ptr_array_add(out, json_read_string(value));
			value = json_skip_string(value);
			continue;
		}
		if (*value == '{' || *value == '[') {
			value = json_skip_bracketed(value);
			continue;
		}
		value++;
	}

	if (n_values != NULL)
		*n_values = out->len;
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

/**
 * gowl_bar_json_foreach_object:
 * @json: a JSON document or fragment
 * @key: a field holding objects
 * @func: (scope call): called per member
 * @user_data: passed to @func
 *
 * Returns: how many members were visited
 */
guint
gowl_bar_json_foreach_object(const gchar *json, const gchar *key,
                             GowlBarJsonForeachFunc func,
                             gpointer user_data)
{
	const gchar *value;
	const gchar *end;
	const gchar *p;
	guint index;

	if (func == NULL)
		return 0;

	value = json_find_value(json, key);
	if (value == NULL || (*value != '{' && *value != '['))
		return 0;

	end = json_skip_bracketed(value);
	p = value + 1;
	index = 0;

	/* Both shapes reduce to the same walk: find the next '{' that is
	   not inside a string, take it whole, hand it over.  For an
	   object-of-objects that skips the member keys on the way, which
	   is what the caller wanted anyway. */
	while (p < end) {
		if (*p == '"') {
			p = json_skip_string(p);
			continue;
		}
		if (*p != '{') {
			p++;
			continue;
		}
		{
			const gchar *obj_end;
			g_autofree gchar *obj = NULL;

			obj_end = json_skip_bracketed(p);
			obj = g_strndup(p, (gsize)(obj_end - p));
			p = obj_end;
			index++;
			if (!func(obj, index - 1, user_data))
				break;
		}
	}
	return index;
}
