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

#ifndef GOWL_SESSION_DEFAULT_H
#define GOWL_SESSION_DEFAULT_H

#include <glib-object.h>
#include "../interfaces/gowl-session-provider.h"

G_BEGIN_DECLS

#define GOWL_TYPE_SESSION_DEFAULT (gowl_session_default_get_type())

G_DECLARE_DERIVABLE_TYPE(GowlSessionDefault, gowl_session_default,
                          GOWL, SESSION_DEFAULT, GObject)

/**
 * GowlSessionDefaultClass:
 * @parent_class: the parent class
 *
 * Default #GowlSessionProvider implementation.  Writes a compact
 * YAML file summarising per-client and per-monitor state.  Subclass
 * to customise the on-disk schema or to attach extra state (e.g.
 * per-tag pertag state from the pertag module).
 */
struct _GowlSessionDefaultClass {
	GObjectClass parent_class;
};

/**
 * gowl_session_default_new:
 *
 * Creates a new default session provider.
 *
 * Returns: (transfer full): a new #GowlSessionDefault
 */
GowlSessionDefault *gowl_session_default_new(void);

G_END_DECLS

#endif /* GOWL_SESSION_DEFAULT_H */
