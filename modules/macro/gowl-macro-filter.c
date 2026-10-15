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
 * gowl-macro-filter.c - The fields a trigger filter may name.
 *
 * The language itself is src/util/gowl-filter.c.
 */

#include "gowl-macro-filter.h"

/* Every field the module can supply; see collect_fields() there. */
static const gchar * const known_fields[] = {
	"event",
	"app-id", "title", "floating", "fullscreen", "urgent", "xwayland",
	"focused-app-id", "focused-title",
	"monitor", "layout", "tag", "tags", "clients",
	"arg",
	"time", "hour", "weekday",
	NULL
};

/**
 * gowl_macro_filter_parse: (skip)
 * @text: the filter expression (what goes between the brackets)
 * @error: return location for a #GError naming the column
 *
 * Parses a trigger filter, refusing any field the macro module does
 * not supply.
 *
 * Returns: (transfer full) (nullable): the filter, %NULL on an error
 */
GowlMacroFilter *
gowl_macro_filter_parse(
	const gchar  *text,
	GError      **error
){
	return gowl_filter_parse(text, known_fields, error);
}

/**
 * gowl_macro_filter_known_fields: (skip)
 *
 * Returns: (transfer none) (array zero-terminated=1): every field name
 */
const gchar * const *
gowl_macro_filter_known_fields(void)
{
	return known_fields;
}
