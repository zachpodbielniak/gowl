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
 * gowl-xwayland-sync.h - Knowing when Xwayland has caught up.
 *
 * Private to the core.  See gowl-xwayland-sync.c for why it exists.
 */

#ifndef GOWL_XWAYLAND_SYNC_H
#define GOWL_XWAYLAND_SYNC_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _GowlXwaylandSync GowlXwaylandSync;

GowlXwaylandSync *gowl_xwayland_sync_new        (const gchar      *display_name,
                                                 GError          **error);
void              gowl_xwayland_sync_free       (GowlXwaylandSync *self);
gboolean          gowl_xwayland_sync_has_raw    (GowlXwaylandSync *self);
gint              gowl_xwayland_sync_get_fd     (GowlXwaylandSync *self);
gboolean          gowl_xwayland_sync_is_alive   (GowlXwaylandSync *self);
gboolean          gowl_xwayland_sync_wait_focus (GowlXwaylandSync *self,
                                                 guint32           window,
                                                 guint             timeout_ms);
void              gowl_xwayland_sync_drain      (GowlXwaylandSync *self);
gint              gowl_xwayland_sync_key_is_down (GowlXwaylandSync *self,
                                                  guint32           evdev_keycode,
                                                  guint             timeout_ms);
gboolean          gowl_xwayland_sync_wait_key   (GowlXwaylandSync *self,
                                                 guint32           evdev_keycode,
                                                 gboolean          pressed,
                                                 guint             timeout_ms);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlXwaylandSync, gowl_xwayland_sync_free)

G_END_DECLS

#endif /* GOWL_XWAYLAND_SYNC_H */
