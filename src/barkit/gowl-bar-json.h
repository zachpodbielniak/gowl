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

#ifndef GOWL_BAR_JSON_H
#define GOWL_BAR_JSON_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * SECTION:gowl-bar-json
 * @title: Reading a CLI's JSON
 * @short_description: pull a few named fields out of a `--json' reply
 *
 * A bar plugin that shells out to something with a `--json' mode ---
 * `tailscale status', `nmcli -j', `podman inspect' --- needs three or
 * four scalars out of a large document.  Linking a full JSON parser
 * into every such plugin is more dependency than the job deserves, and
 * writing a scanner by hand per plugin is how one of them ends up
 * reading the *next* object's field when its own is absent.
 *
 * These read named fields by anchoring on a complete `"key":' match
 * and, for objects, brace-matching the value --- which is exactly the
 * bound a naive forward search lacks.  Strings are skipped wholesale
 * during the walk, so a brace or a quote inside a machine name cannot
 * unbalance it.
 *
 * This is not a JSON parser.  It does not validate, it does not build
 * a tree, and it cannot address an arbitrary path.  What it does is
 * fail by returning %NULL rather than by returning somebody else's
 * value.
 */

/**
 * gowl_bar_json_string:
 * @json: a JSON document, or any fragment of one
 * @key: the field name, without quotes
 *
 * Reads a string field.  Escape sequences are unescaped one level, so
 * a name containing a quote comes back intact.  A field whose value is
 * not a string --- a number, an object, `null' --- returns %NULL
 * rather than a mangled reading of it.
 *
 * Returns: (transfer full) (nullable): the value, or %NULL
 */
gchar *gowl_bar_json_string (const gchar *json, const gchar *key);

/**
 * gowl_bar_json_bool:
 * @json: a JSON document or fragment
 * @key: the field name
 * @fallback: what to return when the field is absent or not a boolean
 *
 * Returns: the field's value, or @fallback
 */
gboolean gowl_bar_json_bool (const gchar *json,
                              const gchar *key,
                              gboolean     fallback);

/**
 * gowl_bar_json_object:
 * @json: a JSON document or fragment
 * @key: the field name
 *
 * Extracts an object value, brace-matched, so a field subsequently
 * read out of it cannot fall through into whatever follows.
 *
 * This bound is the whole point.  In `tailscale status --json' the
 * `Self' object and every peer share their field names, so reading
 * `DNSName' forward from `"Self":' returns the first peer's name
 * whenever Self has none --- silently, and with a plausible-looking
 * answer.
 *
 * Returns: (transfer full) (nullable): the object including its
 *   braces, or %NULL
 */
gchar *gowl_bar_json_object (const gchar *json, const gchar *key);

/**
 * gowl_bar_json_string_array:
 * @json: a JSON document or fragment
 * @key: the field name
 * @n_values: (out) (optional): how many entries were returned
 *
 * Reads an array of strings.
 *
 * Returns: (transfer full) (array zero-terminated=1) (nullable): the
 *   entries, or %NULL.  Free with g_strfreev().
 */
gchar **gowl_bar_json_string_array (const gchar *json,
                                     const gchar *key,
                                     guint       *n_values);

/**
 * GowlBarJsonForeachFunc:
 * @object: one member's value, including its braces
 * @index: its position, counting from zero
 * @user_data: the pointer given to the iterator
 *
 * Returns: %FALSE to stop iterating
 */
typedef gboolean (*GowlBarJsonForeachFunc) (const gchar *object,
                                             guint        index,
                                             gpointer     user_data);

/**
 * gowl_bar_json_foreach_object:
 * @json: a JSON document or fragment
 * @key: a field whose value is an object-of-objects or an
 *   array-of-objects
 * @func: (scope call): called once per member
 * @user_data: passed to @func
 *
 * Walks the objects inside @key's value.  A JSON *object* keyed by
 * opaque identifiers --- which is the shape `tailscale status' uses
 * for its peers --- and a plain array are both handled, because the
 * caller almost never cares which one it got.
 *
 * Returns: how many members were visited
 */
guint gowl_bar_json_foreach_object (const gchar             *json,
                                     const gchar             *key,
                                     GowlBarJsonForeachFunc   func,
                                     gpointer                 user_data);

G_END_DECLS

#endif /* GOWL_BAR_JSON_H */
