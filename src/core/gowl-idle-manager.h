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

#ifndef GOWL_IDLE_MANAGER_H
#define GOWL_IDLE_MANAGER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_IDLE_MANAGER (gowl_idle_manager_get_type())

G_DECLARE_FINAL_TYPE(GowlIdleManager, gowl_idle_manager, GOWL, IDLE_MANAGER, GObject)

GowlIdleManager *gowl_idle_manager_new          (void);

gint              gowl_idle_manager_get_state     (GowlIdleManager *self);
gint              gowl_idle_manager_get_timeout   (GowlIdleManager *self);
void              gowl_idle_manager_set_timeout   (GowlIdleManager *self,
                                                   gint             timeout_secs);

/**
 * gowl_idle_manager_get_dpms_timeout:
 * @self: a #GowlIdleManager
 *
 * Returns: seconds of no input before every output is powered off, or
 *   0 for never
 */
gint              gowl_idle_manager_get_dpms_timeout (GowlIdleManager *self);

/**
 * gowl_idle_manager_set_dpms_timeout:
 * @self: a #GowlIdleManager
 * @timeout_secs: seconds, or 0 for never
 *
 * Sets how long the outputs stay on with nobody at the keyboard.  Any
 * input powers them back on.  Held off by an idle inhibitor.
 */
void              gowl_idle_manager_set_dpms_timeout (GowlIdleManager *self,
                                                      gint             timeout_secs);

/**
 * gowl_idle_manager_is_inhibited:
 * @self: a #GowlIdleManager
 *
 * Returns: %TRUE while a visible surface holds an idle inhibitor
 *   (idle-inhibit-v1) -- a video player, a presentation.  Neither the
 *   idle nor the dpms timer runs, and a lock module's own auto-lock
 *   should hold off too.
 */
gboolean          gowl_idle_manager_is_inhibited    (GowlIdleManager *self);

/**
 * gowl_idle_manager_note_activity:
 * @self: a #GowlIdleManager
 *
 * Marks the session active: restarts both timers, emits
 * #GowlIdleManager::resume if it was idle, and tells ext-idle-notify
 * clients.  The compositor calls this for every real and injected
 * input event; an embedder may call it for activity of its own.
 */
void              gowl_idle_manager_note_activity   (GowlIdleManager *self);

G_END_DECLS

#endif /* GOWL_IDLE_MANAGER_H */
