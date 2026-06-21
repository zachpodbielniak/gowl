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

#ifndef GOWL_CAPTURE_WLROOTS_H
#define GOWL_CAPTURE_WLROOTS_H

#include <glib-object.h>

#include "interfaces/gowl-capture-provider.h"

G_BEGIN_DECLS

#define GOWL_TYPE_CAPTURE_WLROOTS (gowl_capture_wlroots_get_type())

G_DECLARE_FINAL_TYPE(GowlCaptureWlroots, gowl_capture_wlroots,
                     GOWL, CAPTURE_WLROOTS, GObject)

/**
 * gowl_capture_wlroots_new:
 * @compositor: (not nullable): the owning #GowlCompositor.
 *
 * Creates the wlroots-backed #GowlCaptureProvider.  The instance adapts
 * to the wlroots version gowl was compiled against (see
 * gowl-wlroots-compat.h): it always provides monitor capture
 * (wlr-screencopy + output image-capture), and additionally provides
 * per-window capture (ext-image-copy-capture + foreign-toplevel) when
 * built with wlroots >= 0.20.
 *
 * The provider keeps a borrowed pointer to @compositor for the lifetime
 * of the instance (the compositor owns the provider, not vice versa).
 *
 * Returns: (transfer full): a new #GowlCaptureWlroots as a
 *          #GowlCaptureProvider.
 */
GowlCaptureProvider *gowl_capture_wlroots_new(gpointer compositor);

G_END_DECLS

#endif /* GOWL_CAPTURE_WLROOTS_H */
