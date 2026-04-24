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

#ifndef GOWL_SESSION_PROVIDER_H
#define GOWL_SESSION_PROVIDER_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GOWL_TYPE_SESSION_PROVIDER (gowl_session_provider_get_type())

G_DECLARE_INTERFACE(GowlSessionProvider, gowl_session_provider,
                    GOWL, SESSION_PROVIDER, GObject)

/**
 * GowlSessionProviderInterface:
 * @parent_iface: parent GTypeInterface
 * @save: vfunc — serialise the compositor's current session state
 *   (per-monitor layout, focused-tag set, client list with geometry
 *   and tags) to @dest.  Returns %TRUE on success.
 * @load: vfunc — read session state from @src and apply what can
 *   reasonably be applied at this point in the lifecycle
 *   (per-monitor state always, client overrides queued for the next
 *   matching map).  Returns %TRUE on success.
 *
 * Pluggable session serialisation contract.  Implementors decide
 * the on-disk schema (YAML / JSON / binary).  The default
 * implementation #GowlSessionDefault writes a human-readable YAML
 * file via yaml-glib.
 */
struct _GowlSessionProviderInterface {
	GTypeInterface parent_iface;

	gboolean (*save) (GowlSessionProvider *self,
	                  gpointer             compositor,
	                  GFile               *dest,
	                  GError             **error);

	gboolean (*load) (GowlSessionProvider *self,
	                  gpointer             compositor,
	                  GFile               *src,
	                  GError             **error);
};

/**
 * gowl_session_provider_save:
 * @self: a #GowlSessionProvider
 * @compositor: the #GowlCompositor to snapshot (passed as gpointer
 *   to avoid a circular header dep)
 * @dest: the #GFile to write to.  Overwrites if present.
 * @error: (nullable): return location for a #GError
 *
 * Writes the compositor's session state to @dest.  The specific
 * schema is provider-defined.
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_session_provider_save(GowlSessionProvider *self,
                            gpointer             compositor,
                            GFile               *dest,
                            GError             **error);

/**
 * gowl_session_provider_load:
 * @self: a #GowlSessionProvider
 * @compositor: the #GowlCompositor to restore into
 * @src: the #GFile to read from
 * @error: (nullable): return location for a #GError
 *
 * Reads session state from @src and applies it to @compositor.
 * Client-level state is best-effort — clients that no longer
 * exist are queued as rules to apply on the next matching map.
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_session_provider_load(GowlSessionProvider *self,
                            gpointer             compositor,
                            GFile               *src,
                            GError             **error);

G_END_DECLS

#endif /* GOWL_SESSION_PROVIDER_H */
