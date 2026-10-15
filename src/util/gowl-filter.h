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
 * gowl-filter.h - A small boolean language over named string fields.
 *
 *   app-id=firefox* and (title=*YouTube* or title=*Twitch*)
 *
 * Pure GLib, no compositor: a filter is parsed once, when the trigger or
 * rule is installed, and evaluated against a table of field -> string
 * values the caller collects.  The macro module's triggers and the
 * broadcast module's slots and rules use it.  Tested directly by
 * tests/test-macro-filter.c and tests/test-filter.c.
 */

#ifndef GOWL_FILTER_H
#define GOWL_FILTER_H

#include <glib.h>

G_BEGIN_DECLS

#define GOWL_FILTER_ERROR (gowl_filter_error_quark())

/**
 * GowlFilterError:
 * @GOWL_FILTER_ERROR_SYNTAX: the expression does not parse
 * @GOWL_FILTER_ERROR_FIELD: an unknown field name
 * @GOWL_FILTER_ERROR_REGEX: a `~' pattern that is not a regex
 */
typedef enum {
	GOWL_FILTER_ERROR_SYNTAX,
	GOWL_FILTER_ERROR_FIELD,
	GOWL_FILTER_ERROR_REGEX
} GowlFilterError;

GQuark gowl_filter_error_quark (void);

typedef struct _GowlFilter GowlFilter;

GowlFilter *gowl_filter_parse      (const gchar         *text,
                                   const gchar * const *fields,
                                   GError             **error);
void        gowl_filter_free       (GowlFilter          *self);
gboolean    gowl_filter_eval       (const GowlFilter    *self,
                                   GHashTable          *fields);
gchar      *gowl_filter_to_string  (const GowlFilter    *self);
gboolean    gowl_filter_split_line (const gchar         *line,
                                   gchar              **out_what,
                                   gchar              **out_filter,
                                   gchar              **out_rest,
                                   GError             **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlFilter, gowl_filter_free)

G_END_DECLS

#endif /* GOWL_FILTER_H */
