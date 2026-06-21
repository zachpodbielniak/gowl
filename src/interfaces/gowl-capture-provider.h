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

#ifndef GOWL_CAPTURE_PROVIDER_H
#define GOWL_CAPTURE_PROVIDER_H

#include <glib-object.h>

#include "boxed/gowl-capture-source.h"

G_BEGIN_DECLS

/**
 * GowlCaptureCapability:
 * @GOWL_CAPTURE_CAP_NONE: nothing capturable.
 * @GOWL_CAPTURE_CAP_MONITOR: whole-output capture (wlr-screencopy);
 *   available on every supported wlroots.
 * @GOWL_CAPTURE_CAP_WINDOW: per-window capture (ext-image-copy-capture +
 *   foreign-toplevel); wlroots 0.20+ only.
 *
 * Bitmask of what a #GowlCaptureProvider can hand to a screencast
 * client.  Mirrors xdg-desktop-portal's notion of source types so the
 * portal backend can advertise the right AvailableSourceTypes.
 */
typedef enum /*< flags >*/ {
	GOWL_CAPTURE_CAP_NONE    = 0,
	GOWL_CAPTURE_CAP_MONITOR = 1 << 0,
	GOWL_CAPTURE_CAP_WINDOW  = 1 << 1
} GowlCaptureCapability;

#define GOWL_TYPE_CAPTURE_PROVIDER (gowl_capture_provider_get_type())

G_DECLARE_INTERFACE(GowlCaptureProvider, gowl_capture_provider,
                    GOWL, CAPTURE_PROVIDER, GObject)

/**
 * GowlCaptureProviderInterface:
 * @parent_iface: the parent interface.
 * @get_capabilities: return the #GowlCaptureCapability bitmask this
 *   provider supports (depends on the compiled wlroots version).
 * @create_globals: create the Wayland protocol globals on @display
 *   (screencopy, and on 0.20+ the image-copy/foreign-toplevel stack).
 *   Called once during compositor startup.  Returns %TRUE on success.
 * @list_sources: return a #GList of #GowlCaptureSource (transfer full,
 *   deep) describing currently capturable monitors and (if supported)
 *   windows.  Used by tests and introspection; the live portal path
 *   uses the protocol globals directly.
 * @add_window: register a newly-mapped toplevel as a capturable window
 *   (no-op on monitor-only providers).  @client is a #GowlClient.
 * @remove_window: unregister a toplevel on unmap/destroy.
 * @update_window: refresh a toplevel's title/app_id after a change.
 *
 * Abstracts the wlroots-version-specific screencast capture surface
 * behind one GObject interface, so the compositor wires capture without
 * referring to any wlroots type or version.  The concrete provider is
 * selected at build time by the compiled wlroots (see
 * gowl-wlroots-compat.h); a monitor-only implementation backs 0.19 and a
 * monitor+window implementation backs 0.20.
 *
 * Implementations emit #GowlCaptureProvider::source-added and
 * #GowlCaptureProvider::source-removed as windows map and unmap.
 */
struct _GowlCaptureProviderInterface {
	GTypeInterface parent_iface;

	GowlCaptureCapability (*get_capabilities) (GowlCaptureProvider *self);

	gboolean              (*create_globals)   (GowlCaptureProvider *self,
	                                            gpointer             display);

	GList *               (*list_sources)     (GowlCaptureProvider *self);

	void                  (*add_window)       (GowlCaptureProvider *self,
	                                            gpointer             client);

	void                  (*remove_window)    (GowlCaptureProvider *self,
	                                            gpointer             client);

	void                  (*update_window)    (GowlCaptureProvider *self,
	                                            gpointer             client);
};

GowlCaptureCapability gowl_capture_provider_get_capabilities
                                              (GowlCaptureProvider *self);

gboolean              gowl_capture_provider_supports_window_capture
                                              (GowlCaptureProvider *self);

gboolean              gowl_capture_provider_create_globals
                                              (GowlCaptureProvider *self,
                                               gpointer             display);

GList *               gowl_capture_provider_list_sources
                                              (GowlCaptureProvider *self);

void                  gowl_capture_provider_add_window
                                              (GowlCaptureProvider *self,
                                               gpointer             client);

void                  gowl_capture_provider_remove_window
                                              (GowlCaptureProvider *self,
                                               gpointer             client);

void                  gowl_capture_provider_update_window
                                              (GowlCaptureProvider *self,
                                               gpointer             client);

G_END_DECLS

#endif /* GOWL_CAPTURE_PROVIDER_H */
