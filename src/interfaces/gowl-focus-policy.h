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

#ifndef GOWL_FOCUS_POLICY_H
#define GOWL_FOCUS_POLICY_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_FOCUS_POLICY (gowl_focus_policy_get_type())

G_DECLARE_INTERFACE(GowlFocusPolicy, gowl_focus_policy, GOWL, FOCUS_POLICY, GObject)

struct _GowlFocusPolicyInterface {
	GTypeInterface parent_iface;

	gboolean (*focus_on_map)   (GowlFocusPolicy *self, gpointer client);
	gboolean (*focus_on_hover) (GowlFocusPolicy *self, gpointer client);
};

/* Public dispatch functions */
gboolean gowl_focus_policy_focus_on_map   (GowlFocusPolicy *self, gpointer client);
gboolean gowl_focus_policy_focus_on_hover (GowlFocusPolicy *self, gpointer client);

G_END_DECLS

#endif /* GOWL_FOCUS_POLICY_H */
