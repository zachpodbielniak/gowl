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
 * gowl-macro-filter.h - Trigger filters: a small boolean language.
 *
 *   client-added [app-id=firefox* and (title=*YouTube* or title=*Twitch*)]: x
 *
 * Pure GLib, no compositor: a filter is parsed once, when the trigger is
 * installed, and evaluated against a table of field -> string values the
 * module collects from the event.  Tested directly by
 * tests/test-macro-filter.c.
 */

#ifndef GOWL_MACRO_FILTER_H
#define GOWL_MACRO_FILTER_H

#include <glib.h>

G_BEGIN_DECLS

#define GOWL_MACRO_FILTER_ERROR (gowl_macro_filter_error_quark())

/**
 * GowlMacroFilterError:
 * @GOWL_MACRO_FILTER_ERROR_SYNTAX: the expression does not parse
 * @GOWL_MACRO_FILTER_ERROR_FIELD: an unknown field name
 * @GOWL_MACRO_FILTER_ERROR_REGEX: a `~' pattern that is not a regex
 */
typedef enum {
	GOWL_MACRO_FILTER_ERROR_SYNTAX,
	GOWL_MACRO_FILTER_ERROR_FIELD,
	GOWL_MACRO_FILTER_ERROR_REGEX
} GowlMacroFilterError;

GQuark gowl_macro_filter_error_quark (void);

typedef struct _GowlMacroFilter GowlMacroFilter;

GowlMacroFilter     *gowl_macro_filter_parse        (const gchar            *text,
                                                     GError                **error);
void                 gowl_macro_filter_free         (GowlMacroFilter        *self);
gboolean             gowl_macro_filter_eval         (const GowlMacroFilter  *self,
                                                     GHashTable             *fields);
gchar               *gowl_macro_filter_to_string    (const GowlMacroFilter  *self);
const gchar * const *gowl_macro_filter_known_fields (void);
gboolean             gowl_macro_filter_split_line   (const gchar            *line,
                                                     gchar                 **out_what,
                                                     gchar                 **out_filter,
                                                     gchar                 **out_rest,
                                                     GError                **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlMacroFilter, gowl_macro_filter_free)

G_END_DECLS

#endif /* GOWL_MACRO_FILTER_H */
