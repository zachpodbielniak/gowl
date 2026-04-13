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

#ifndef GOWL_DROPDOWN_PROVIDER_H
#define GOWL_DROPDOWN_PROVIDER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_DROPDOWN_PROVIDER (gowl_dropdown_provider_get_type())

G_DECLARE_INTERFACE(GowlDropdownProvider, gowl_dropdown_provider,
                     GOWL, DROPDOWN_PROVIDER, GObject)

/**
 * GowlDropdownProviderInterface:
 * @toggle_by_name: toggle the named dropdown.  Returns %TRUE if a
 *                  matching entry was found and toggled.
 * @refresh:        scan the compositor config for new dropdown
 *                  entries not yet registered in the implementation's
 *                  internal cache, and adopt them.  Returns the
 *                  number of newly adopted entries.
 *
 * Interface implemented by modules that manage Guake-style
 * dropdown windows.  Used by the cmacs gowl DEFUNs to dispatch
 * toggle and refresh requests without knowing which module is
 * actually loaded.
 */
struct _GowlDropdownProviderInterface {
	GTypeInterface parent_iface;

	gboolean (*toggle_by_name) (GowlDropdownProvider *self,
	                             const gchar          *name);
	guint    (*refresh)        (GowlDropdownProvider *self);
};

gboolean gowl_dropdown_provider_toggle_by_name (GowlDropdownProvider *self,
                                                  const gchar          *name);

guint    gowl_dropdown_provider_refresh        (GowlDropdownProvider *self);

G_END_DECLS

#endif /* GOWL_DROPDOWN_PROVIDER_H */
