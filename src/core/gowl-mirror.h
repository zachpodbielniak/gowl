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

#ifndef GOWL_MIRROR_H
#define GOWL_MIRROR_H

#include <glib-object.h>

struct wlr_scene_buffer;

G_BEGIN_DECLS

#define GOWL_TYPE_MIRROR (gowl_mirror_get_type())

G_DECLARE_DERIVABLE_TYPE(GowlMirror, gowl_mirror, GOWL, MIRROR, GObject)

/**
 * GowlMirrorClass:
 * @parent_class: parent GObjectClass
 *
 * Concrete derivable class representing one additional scene node
 * that mirrors a #GowlClient's root surface buffer.  Enables
 * showing the same live client in multiple Emacs windows under
 * cmacs `--gowl` (C-x 3 over a gowl app buffer), because each
 * window can back its paint region with an independent
 * `wlr_scene_buffer' pointing at the same `wlr_buffer'.
 *
 * Mirrors are owned by the #GowlClient that created them.  Remove
 * them via #gowl_client_remove_mirror — the scene node and any
 * wlroots listeners are torn down then.  Subscribers to the
 * client's `mirror-added' / `mirror-removed' signals are notified
 * of lifecycle events.
 *
 * Sub-surfaces and popups are NOT mirrored — only the root
 * surface buffer is duplicated, matching emskin's scope.  A
 * future extension could walk the wl_subsurface tree, but the
 * common embedded-app use case (one GTK / one terminal) is
 * satisfied by the root-only duplication.
 */
struct _GowlMirrorClass {
	GObjectClass parent_class;
};

/**
 * gowl_mirror_get_view_id:
 * @self: a #GowlMirror
 *
 * Returns: the monotonic identifier the creating client assigned
 *          to this mirror.  Used to match up Elisp-side mirror
 *          bookkeeping with the compositor's live state.
 */
guint64
gowl_mirror_get_view_id(GowlMirror *self);

/**
 * gowl_mirror_get_client:
 * @self: a #GowlMirror
 *
 * Returns: (transfer none) (nullable): the client that owns this
 *          mirror, or %NULL once the client has been destroyed.
 */
gpointer
gowl_mirror_get_client(GowlMirror *self);

/**
 * gowl_mirror_get_geometry:
 * @self: a #GowlMirror
 * @x: (out): x position
 * @y: (out): y position
 * @w: (out): width
 * @h: (out): height
 *
 * Reads the current geometry.  Prefer this over direct field access
 * for bindings cleanliness.
 */
void
gowl_mirror_get_geometry(GowlMirror *self,
                          gint       *x,
                          gint       *y,
                          gint       *w,
                          gint       *h);

/*
 * Internal constructor helpers live in core/gowl-mirror.c.  Public
 * creation goes through `gowl_client_add_mirror` so the client's
 * ownership bookkeeping stays centralised.
 */

G_END_DECLS

#endif /* GOWL_MIRROR_H */
