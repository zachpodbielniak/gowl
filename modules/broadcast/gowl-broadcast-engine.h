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
 * gowl-broadcast-engine.h - Who gets which key: the broadcast decisions.
 *
 * Pure: windows come in as #GowlBroadcastWindow facts, decisions go out
 * as #GowlBroadcastAction lists, and nothing here touches a compositor,
 * a seat or a clock -- the time is an argument.  The module feeds it
 * and delivers what it decides; tests/test-broadcast-engine.c drives it
 * directly.
 */

#ifndef GOWL_BROADCAST_ENGINE_H
#define GOWL_BROADCAST_ENGINE_H

#include <glib.h>

#include "boxed/gowl-broadcast.h"

G_BEGIN_DECLS

/**
 * GowlBroadcastWindow:
 * @id: the compositor's client id (never 0)
 * @app_id: (nullable): app_id, or the X11 class
 * @title: (nullable): the window title
 * @pid: the client's process id, 0 when unknown
 * @xwayland: whether it is an X11 window
 * @tags: the window's tag mask
 * @monitor: (nullable): its output's name
 * @floating: whether it floats
 * @fullscreen: whether it is fullscreen
 *
 * The facts a slot's `match' and a rule's `when' test.  Strings are
 * borrowed for the length of the call that takes them.
 */
typedef struct {
	guint        id;
	const gchar *app_id;
	const gchar *title;
	gint         pid;
	gboolean     xwayland;
	guint32      tags;
	const gchar *monitor;
	gboolean     floating;
	gboolean     fullscreen;
} GowlBroadcastWindow;

/**
 * GowlBroadcastAction:
 * @client_id: the window to deliver to
 * @slot: (transfer none): the slot it fills
 * @keysym: the key to send instead of the one typed, 0 to send the
 *   typed key itself (same keycode -- the exact physical key)
 * @modifiers: #GowlKeyMod bits to send with it, or -1 for the live ones
 * @delivery: how to reach the window
 * @delay_ms: pause before this delivery, after the previous one
 * @mouse: for a click, where it lands
 * @rule: (transfer none): the rule that decided it
 *
 * One delivery the engine decided on.
 */
typedef struct {
	guint               client_id;
	const gchar        *slot;
	guint32             keysym;
	gint                modifiers;
	GowlKeyDelivery     delivery;
	guint               delay_ms;
	GowlBroadcastMouse  mouse;
	GowlBroadcastRule  *rule;
} GowlBroadcastAction;

typedef struct _GowlBroadcastEngine GowlBroadcastEngine;

GowlBroadcastEngine *gowl_broadcast_engine_new           (void);
void                 gowl_broadcast_engine_free          (GowlBroadcastEngine *self);

/* The set of teams and rules */
void                 gowl_broadcast_engine_set_config    (GowlBroadcastEngine *self,
                                                          GPtrArray           *teams,
                                                          GPtrArray           *rules);
gboolean             gowl_broadcast_engine_add_team      (GowlBroadcastEngine *self,
                                                          GowlBroadcastTeam   *team);
gboolean             gowl_broadcast_engine_add_rule      (GowlBroadcastEngine *self,
                                                          GowlBroadcastRule   *rule);
gboolean             gowl_broadcast_engine_remove_team   (GowlBroadcastEngine *self,
                                                          const gchar         *name);
gboolean             gowl_broadcast_engine_remove_rule   (GowlBroadcastEngine *self,
                                                          const gchar         *name);
void                 gowl_broadcast_engine_clear_runtime (GowlBroadcastEngine *self);
void                 gowl_broadcast_engine_set_strict    (GowlBroadcastEngine *self,
                                                          gboolean             strict);
gboolean             gowl_broadcast_engine_get_strict    (GowlBroadcastEngine *self);
gboolean             gowl_broadcast_engine_set_rule_enabled (GowlBroadcastEngine *self,
                                                             const gchar         *name,
                                                             gboolean             enabled);

guint                gowl_broadcast_engine_get_n_teams   (GowlBroadcastEngine *self);
GowlBroadcastTeam   *gowl_broadcast_engine_get_team      (GowlBroadcastEngine *self,
                                                          guint                index);
GowlBroadcastTeam   *gowl_broadcast_engine_find_team     (GowlBroadcastEngine *self,
                                                          const gchar         *name);
guint                gowl_broadcast_engine_get_n_rules   (GowlBroadcastEngine *self);
GowlBroadcastRule   *gowl_broadcast_engine_get_rule      (GowlBroadcastEngine *self,
                                                          guint                index);
gboolean             gowl_broadcast_engine_rule_is_runtime (GowlBroadcastEngine *self,
                                                            GowlBroadcastRule   *rule);
gboolean             gowl_broadcast_engine_team_is_runtime (GowlBroadcastEngine *self,
                                                            GowlBroadcastTeam   *team);
const gchar         *gowl_broadcast_engine_rule_problem  (GowlBroadcastEngine *self,
                                                          GowlBroadcastRule   *rule);
guint                gowl_broadcast_engine_get_n_problems (GowlBroadcastEngine *self);

/* Windows to slots */
void                 gowl_broadcast_engine_assign        (GowlBroadcastEngine       *self,
                                                          const GowlBroadcastWindow *windows,
                                                          guint                      n_windows);
gboolean             gowl_broadcast_engine_set_manual    (GowlBroadcastEngine *self,
                                                          const gchar         *team,
                                                          const gchar         *slot,
                                                          guint                client_id,
                                                          GError             **error);
guint                gowl_broadcast_engine_client_for    (GowlBroadcastEngine *self,
                                                          const gchar         *team,
                                                          guint                slot);
gboolean             gowl_broadcast_engine_slot_of       (GowlBroadcastEngine *self,
                                                          guint                client_id,
                                                          const gchar        **out_team,
                                                          const gchar        **out_slot);
guint                gowl_broadcast_engine_count_bound   (GowlBroadcastEngine *self,
                                                          const gchar         *team);

/* Decisions */
gboolean             gowl_broadcast_engine_route_key     (GowlBroadcastEngine       *self,
                                                          const GowlBroadcastWindow *focused,
                                                          guint                      under_mouse,
                                                          guint32                    keysym,
                                                          guint32                    base_keysym,
                                                          guint32                    modifiers,
                                                          gint64                     now_ms,
                                                          GArray                    *out_actions,
                                                          gboolean                  *out_local_block);
gboolean             gowl_broadcast_engine_route_button  (GowlBroadcastEngine       *self,
                                                          const GowlBroadcastWindow *clicked,
                                                          gint64                     now_ms,
                                                          GArray                    *out_actions);
gboolean             gowl_broadcast_engine_focus_key     (GowlBroadcastEngine *self,
                                                          guint32              modifiers,
                                                          guint32              keysym,
                                                          const gchar        **out_team,
                                                          const gchar        **out_slot,
                                                          guint               *out_client_id);
void                 gowl_broadcast_engine_reset_steps   (GowlBroadcastEngine *self,
                                                          const gchar         *rule);

GHashTable          *gowl_broadcast_window_fields        (const GowlBroadcastWindow *window);

G_END_DECLS

#endif /* GOWL_BROADCAST_ENGINE_H */
