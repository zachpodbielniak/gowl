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
 * gowl-macro-filter.h - Trigger filters: the shared filter language
 * (src/util/gowl-filter.h) with the macro module's fields.
 *
 *   client-added [app-id=firefox* and (title=*YouTube* or title=*Twitch*)]: x
 *
 * The language moved into libgowl when the broadcast module needed it
 * too; what stays here is the one thing that is the macro module's
 * own, the set of fields collect_fields() supplies.  Tested directly by
 * tests/test-macro-filter.c.
 */

#ifndef GOWL_MACRO_FILTER_H
#define GOWL_MACRO_FILTER_H

#include <glib.h>

#include "util/gowl-filter.h"

G_BEGIN_DECLS

typedef GowlFilter GowlMacroFilter;

#define GOWL_MACRO_FILTER_ERROR        GOWL_FILTER_ERROR
#define GOWL_MACRO_FILTER_ERROR_SYNTAX GOWL_FILTER_ERROR_SYNTAX
#define GOWL_MACRO_FILTER_ERROR_FIELD  GOWL_FILTER_ERROR_FIELD
#define GOWL_MACRO_FILTER_ERROR_REGEX  GOWL_FILTER_ERROR_REGEX

#define gowl_macro_filter_free       gowl_filter_free
#define gowl_macro_filter_eval       gowl_filter_eval
#define gowl_macro_filter_to_string  gowl_filter_to_string
#define gowl_macro_filter_split_line gowl_filter_split_line

GowlMacroFilter     *gowl_macro_filter_parse        (const gchar  *text,
                                                     GError      **error);
const gchar * const *gowl_macro_filter_known_fields (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlMacroFilter, gowl_filter_free)

G_END_DECLS

#endif /* GOWL_MACRO_FILTER_H */
