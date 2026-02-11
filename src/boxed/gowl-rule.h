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
 * @app_id_pattern: Shell-style glob pattern matched against client app_id.
 *                  %NULL means "match any app_id".
 * @title_pattern: Shell-style glob pattern matched against client title.
 *                 %NULL means "match any title".
 * @tags: Tag bitmask to assign to matching clients.
 * @floating: Whether matching clients should float.
 * @monitor: Preferred monitor index, or -1 for no preference.
 *
 * A window rule that automatically configures clients whose app_id and
 * title match the given glob patterns.
 */
struct _GowlRule {
	gchar    *app_id_pattern;
	gchar    *title_pattern;
	guint32   tags;
	gboolean  floating;
	gint      monitor;
};

GType      gowl_rule_get_type (void) G_GNUC_CONST;

GowlRule * gowl_rule_new      (const gchar    *app_id_pattern,
                                const gchar    *title_pattern,
                                guint32         tags,
                                gboolean        floating,
                                gint            monitor);

GowlRule * gowl_rule_copy     (const GowlRule *self);

void       gowl_rule_free     (GowlRule        *self);

gboolean   gowl_rule_matches  (const GowlRule *self,
                                const gchar    *app_id,
                                const gchar    *title);

G_END_DECLS

#endif /* GOWL_RULE_H */
