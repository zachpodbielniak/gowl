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

#ifndef GOWL_RULE_H
#define GOWL_RULE_H

#include "gowl-types.h"

G_BEGIN_DECLS

#define GOWL_TYPE_RULE (gowl_rule_get_type())

/**
 * GowlRule:
 * @app_id_pattern: Pattern matched against client app_id.  %NULL
 *                  means "match any app_id".  Interpreted as a
 *                  shell-style glob by default, or as a PCRE
 *                  regular expression when @regex_mode is %TRUE.
 * @title_pattern: Pattern matched against client title.  %NULL
 *                 means "match any title".  Interpreted as glob
 *                 or PCRE regex per @regex_mode.
 * @tags: Tag bitmask to assign to matching clients (0 = leave
 *        unchanged).
 * @floating: Whether matching clients should be forced floating.
 * @monitor: Preferred monitor index, or -1 for no preference.
 * @width: Explicit width in pixels for a floated match, or 0 to
 *         keep the client's natural size.
 * @height: Explicit height in pixels for a floated match, or 0
 *          to keep the client's natural size.
 * @center: When @floating is %TRUE and no explicit x/y is set,
 *          center the client on its target monitor.
 * @regex_mode: When %TRUE, patterns are compiled as PCRE regexes
 *              (via #GRegex) rather than glob patterns.
 * @app_id_regex: (nullable): Compiled regex for @app_id_pattern
 *                when @regex_mode is %TRUE.
 * @title_regex: (nullable): Compiled regex for @title_pattern
 *               when @regex_mode is %TRUE.
 *
 * A window rule that automatically configures clients whose app_id
 * and title match the given patterns.  The existing constructor
 * gowl_rule_new() produces a glob-mode rule with no geometry
 * override; gowl_rule_new_full() exposes every field.
 */
struct _GowlRule {
	gchar    *app_id_pattern;
	gchar    *title_pattern;
	guint32   tags;
	gboolean  floating;
	gint      monitor;

	/* v2 fields: geometry + regex (optional, zero defaults) */
	gint      width;
	gint      height;
	gboolean  center;
	gboolean  regex_mode;

	/* Compiled regex handles (lazy-compiled on first match when
	 * regex_mode is TRUE).  Not part of the public ABI but kept
	 * inside the struct so gowl_rule_free() can release them. */
	GRegex   *app_id_regex;
	GRegex   *title_regex;
};

GType      gowl_rule_get_type     (void) G_GNUC_CONST;

GowlRule * gowl_rule_new          (const gchar    *app_id_pattern,
                                    const gchar    *title_pattern,
                                    guint32         tags,
                                    gboolean        floating,
                                    gint            monitor);

GowlRule * gowl_rule_new_full     (const gchar    *app_id_pattern,
                                    const gchar    *title_pattern,
                                    guint32         tags,
                                    gboolean        floating,
                                    gint            monitor,
                                    gint            width,
                                    gint            height,
                                    gboolean        center,
                                    gboolean        regex_mode);

GowlRule * gowl_rule_copy         (const GowlRule *self);

void       gowl_rule_free         (GowlRule        *self);

gboolean   gowl_rule_matches      (const GowlRule *self,
                                    const gchar    *app_id,
                                    const gchar    *title);

G_END_DECLS

#endif /* GOWL_RULE_H */
