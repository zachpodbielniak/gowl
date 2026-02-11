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

#ifndef GOWL_SESSION_LOCK_H
#define GOWL_SESSION_LOCK_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_SESSION_LOCK (gowl_session_lock_get_type())

G_DECLARE_FINAL_TYPE(GowlSessionLock, gowl_session_lock, GOWL, SESSION_LOCK, GObject)

GowlSessionLock *gowl_session_lock_new        (void);

gboolean         gowl_session_lock_is_locked   (GowlSessionLock *self);

G_END_DECLS

#endif /* GOWL_SESSION_LOCK_H */
