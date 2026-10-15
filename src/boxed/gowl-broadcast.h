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
 * gowl-broadcast.h - Teams and rules for input broadcasting.
 *
 * Multiboxing: several copies of one program -- game clients, as a
 * rule -- each a SLOT of a TEAM, and RULES that say, while some window
 * of the team has focus, which keys (and clicks) also go to which other
 * windows of the team, as the same key or another.
 *
 *   broadcast:
 *     teams:
 *       - name: eq
 *         slots:
 *           - {name: main, match: 'app-id = eqgame.exe and title ~ Tank'}
 *           - {name: healer, match: 'app-id = eqgame.exe and title ~ Cleric'}
 *         groups: {casters: [healer]}
 *     rules:
 *       - name: follow
 *         team: eq
 *         when: 'focused-slot = main'
 *         keys: {include: ['1-9', w, a, s, d], exclude: [Return]}
 *         targets: [all-other]
 *
 * These types live in libgowl, not in the broadcast module, for the same
 * reason input-remap rules do: the YAML `broadcast:' section is parsed
 * by the core config loader and a C config runs before any module is
 * loaded.  The module is the engine that applies them.  Everything is
 * validated on the way in -- key names, filters, targets -- so a typo is
 * reported when the config loads, not discovered mid-game.  See
 * docs/broadcast.org.
 */

#ifndef GOWL_BROADCAST_H
#define GOWL_BROADCAST_H

#include <glib-object.h>

#include "gowl-types.h"
#include "gowl-enums.h"

G_BEGIN_DECLS

/**
 * GOWL_BROADCAST_ERROR:
 *
 * Error domain for broadcast teams and rules.
 */
#define GOWL_BROADCAST_ERROR (gowl_broadcast_error_quark())

/**
 * GowlBroadcastError:
 * @GOWL_BROADCAST_ERROR_INVALID: malformed (missing name, unknown
 *   field, wrong value type, a duplicate slot)
 * @GOWL_BROADCAST_ERROR_UNKNOWN_KEY: a key name that is no keysym
 * @GOWL_BROADCAST_ERROR_FILTER: a `match' or `when' that does not parse
 * @GOWL_BROADCAST_ERROR_NOT_ONE_TO_ONE: in strict mode, a rule that can
 *   turn one press into input for more than one window
 *
 * Why a team or rule was refused.
 */
typedef enum {
	GOWL_BROADCAST_ERROR_INVALID,
	GOWL_BROADCAST_ERROR_UNKNOWN_KEY,
	GOWL_BROADCAST_ERROR_FILTER,
	GOWL_BROADCAST_ERROR_NOT_ONE_TO_ONE
} GowlBroadcastError;

GQuark gowl_broadcast_error_quark (void);

/**
 * GowlBroadcastMode:
 * @GOWL_BROADCAST_MODE_MIRROR: every target gets every matching key
 * @GOWL_BROADCAST_MODE_ROUND_ROBIN: each press goes to the NEXT target
 *   only, in turn (ISBoxer's "steps", AutoHotkey's counter); its
 *   release follows it there
 *
 * How a rule spreads a key over its targets.
 */
typedef enum {
	GOWL_BROADCAST_MODE_MIRROR,
	GOWL_BROADCAST_MODE_ROUND_ROBIN
} GowlBroadcastMode;

#define GOWL_TYPE_BROADCAST_MODE (gowl_broadcast_mode_get_type())
GType gowl_broadcast_mode_get_type (void) G_GNUC_CONST;

/**
 * GowlBroadcastMouse:
 * @GOWL_BROADCAST_MOUSE_OFF: clicks are not broadcast
 * @GOWL_BROADCAST_MOUSE_SAME: a click lands at the same surface
 *   coordinates in each target
 * @GOWL_BROADCAST_MOUSE_SCALED: a click lands at the same RELATIVE
 *   position, scaled to each target's size -- right for a team whose
 *   windows differ in size
 *
 * Whether, and where, a rule repeats clicks.
 */
typedef enum {
	GOWL_BROADCAST_MOUSE_OFF,
	GOWL_BROADCAST_MOUSE_SAME,
	GOWL_BROADCAST_MOUSE_SCALED
} GowlBroadcastMouse;

#define GOWL_TYPE_BROADCAST_MOUSE (gowl_broadcast_mouse_get_type())
GType gowl_broadcast_mouse_get_type (void) G_GNUC_CONST;

/**
 * GowlBroadcastTargetKind:
 * @GOWL_BROADCAST_TARGET_ALL: every slot of the team, the focused one
 *   included (which then gets the key twice unless `local: block')
 * @GOWL_BROADCAST_TARGET_ALL_OTHER: every slot but the focused one
 * @GOWL_BROADCAST_TARGET_CURRENT: the focused slot only (with `local:
 *   block' and a translation, a key rewrite for one window)
 * @GOWL_BROADCAST_TARGET_SLOT: one named slot
 * @GOWL_BROADCAST_TARGET_GROUP: the slots of one named group
 * @GOWL_BROADCAST_TARGET_UNDER_MOUSE: the team window under the pointer
 *
 * Who a rule's key goes to.
 */
typedef enum {
	GOWL_BROADCAST_TARGET_ALL,
	GOWL_BROADCAST_TARGET_ALL_OTHER,
	GOWL_BROADCAST_TARGET_CURRENT,
	GOWL_BROADCAST_TARGET_SLOT,
	GOWL_BROADCAST_TARGET_GROUP,
	GOWL_BROADCAST_TARGET_UNDER_MOUSE
} GowlBroadcastTargetKind;

#define GOWL_TYPE_BROADCAST_TARGET_KIND (gowl_broadcast_target_kind_get_type())
GType gowl_broadcast_target_kind_get_type (void) G_GNUC_CONST;

/* --- GowlBroadcastTeam --- */

#define GOWL_TYPE_BROADCAST_TEAM (gowl_broadcast_team_get_type())

/**
 * GowlBroadcastTeam:
 *
 * A named set of slots, each filled by at most one window, plus named
 * groups of slots.  Reference counted.
 */
typedef struct _GowlBroadcastTeam GowlBroadcastTeam;

GType               gowl_broadcast_team_get_type         (void) G_GNUC_CONST;
GowlBroadcastTeam  *gowl_broadcast_team_new              (const gchar        *name);
GowlBroadcastTeam  *gowl_broadcast_team_ref              (GowlBroadcastTeam  *self);
void                gowl_broadcast_team_unref            (GowlBroadcastTeam  *self);
const gchar        *gowl_broadcast_team_get_name         (GowlBroadcastTeam  *self);

gboolean            gowl_broadcast_team_add_slot         (GowlBroadcastTeam  *self,
                                                          const gchar        *name,
                                                          const gchar        *match,
                                                          const gchar        *focus_key,
                                                          GowlKeyDelivery     delivery,
                                                          GError            **error);
guint               gowl_broadcast_team_get_n_slots      (GowlBroadcastTeam  *self);
gint                gowl_broadcast_team_find_slot        (GowlBroadcastTeam  *self,
                                                          const gchar        *name);
const gchar        *gowl_broadcast_team_get_slot_name    (GowlBroadcastTeam  *self,
                                                          guint               index);
const gchar        *gowl_broadcast_team_get_slot_match   (GowlBroadcastTeam  *self,
                                                          guint               index);
gboolean            gowl_broadcast_team_slot_matches     (GowlBroadcastTeam  *self,
                                                          guint               index,
                                                          GHashTable         *fields);
const gchar        *gowl_broadcast_team_get_slot_focus_key (GowlBroadcastTeam *self,
                                                            guint              index);
gboolean            gowl_broadcast_team_get_slot_focus_bind (GowlBroadcastTeam *self,
                                                             guint              index,
                                                             guint32           *out_modifiers,
                                                             guint32           *out_keysym);
GowlKeyDelivery     gowl_broadcast_team_get_slot_delivery (GowlBroadcastTeam *self,
                                                           guint              index);

gboolean            gowl_broadcast_team_add_group        (GowlBroadcastTeam  *self,
                                                          const gchar        *name,
                                                          const gchar * const *slots,
                                                          GError            **error);
const gchar * const *gowl_broadcast_team_get_group       (GowlBroadcastTeam  *self,
                                                          const gchar        *name);
gchar             **gowl_broadcast_team_get_group_names  (GowlBroadcastTeam  *self);

GowlBroadcastTeam  *gowl_broadcast_team_new_from_yaml    (const gchar        *yaml,
                                                          GError            **error);
gchar              *gowl_broadcast_team_to_yaml          (GowlBroadcastTeam  *self);

const gchar * const *gowl_broadcast_slot_fields          (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlBroadcastTeam, gowl_broadcast_team_unref)

/* --- GowlBroadcastRule --- */

#define GOWL_TYPE_BROADCAST_RULE (gowl_broadcast_rule_get_type())

/**
 * GowlBroadcastRule:
 *
 * One broadcasting rule: while @when holds for the focused window of
 * @team, keys in the rule's key set go to its targets.  Reference
 * counted.
 */
typedef struct _GowlBroadcastRule GowlBroadcastRule;

GType               gowl_broadcast_rule_get_type        (void) G_GNUC_CONST;
GowlBroadcastRule  *gowl_broadcast_rule_new             (const gchar        *name);
GowlBroadcastRule  *gowl_broadcast_rule_ref             (GowlBroadcastRule  *self);
void                gowl_broadcast_rule_unref           (GowlBroadcastRule  *self);
const gchar        *gowl_broadcast_rule_get_name        (GowlBroadcastRule  *self);

void                gowl_broadcast_rule_set_team        (GowlBroadcastRule  *self,
                                                         const gchar        *team);
const gchar        *gowl_broadcast_rule_get_team        (GowlBroadcastRule  *self);
gboolean            gowl_broadcast_rule_set_when        (GowlBroadcastRule  *self,
                                                         const gchar        *when,
                                                         GError            **error);
const gchar        *gowl_broadcast_rule_get_when        (GowlBroadcastRule  *self);
gboolean            gowl_broadcast_rule_when_holds      (GowlBroadcastRule  *self,
                                                         GHashTable         *fields);
void                gowl_broadcast_rule_set_enabled     (GowlBroadcastRule  *self,
                                                         gboolean            enabled);
gboolean            gowl_broadcast_rule_get_enabled     (GowlBroadcastRule  *self);

gboolean            gowl_broadcast_rule_set_keys        (GowlBroadcastRule  *self,
                                                         const gchar * const *include,
                                                         const gchar * const *exclude,
                                                         GError            **error);
const gchar * const *gowl_broadcast_rule_get_keys_include (GowlBroadcastRule *self);
const gchar * const *gowl_broadcast_rule_get_keys_exclude (GowlBroadcastRule *self);
gboolean            gowl_broadcast_rule_matches_key     (GowlBroadcastRule  *self,
                                                         guint32             keysym,
                                                         guint32             base_keysym);
void                gowl_broadcast_rule_set_modifiers_none (GowlBroadcastRule *self,
                                                            gboolean           none);
gboolean            gowl_broadcast_rule_get_modifiers_none (GowlBroadcastRule *self);

gboolean            gowl_broadcast_rule_set_targets     (GowlBroadcastRule  *self,
                                                         const gchar * const *targets,
                                                         GError            **error);
guint               gowl_broadcast_rule_get_n_targets   (GowlBroadcastRule  *self);
GowlBroadcastTargetKind
                    gowl_broadcast_rule_get_target_kind (GowlBroadcastRule  *self,
                                                         guint               index);
const gchar        *gowl_broadcast_rule_get_target_arg  (GowlBroadcastRule  *self,
                                                         guint               index);

gboolean            gowl_broadcast_rule_add_translation (GowlBroadcastRule  *self,
                                                         const gchar        *slot,
                                                         const gchar        *from,
                                                         const gchar        *to,
                                                         GError            **error);
gboolean            gowl_broadcast_rule_translate       (GowlBroadcastRule  *self,
                                                         const gchar        *slot,
                                                         guint32             keysym,
                                                         guint32             base_keysym,
                                                         guint32            *out_keysym,
                                                         guint32            *out_modifiers);

void                gowl_broadcast_rule_set_mode        (GowlBroadcastRule  *self,
                                                         GowlBroadcastMode   mode);
GowlBroadcastMode   gowl_broadcast_rule_get_mode        (GowlBroadcastRule  *self);
void                gowl_broadcast_rule_set_reset_ms    (GowlBroadcastRule  *self,
                                                         guint               reset_ms);
guint               gowl_broadcast_rule_get_reset_ms    (GowlBroadcastRule  *self);
void                gowl_broadcast_rule_set_delivery    (GowlBroadcastRule  *self,
                                                         GowlKeyDelivery     delivery);
GowlKeyDelivery     gowl_broadcast_rule_get_delivery    (GowlBroadcastRule  *self);
void                gowl_broadcast_rule_set_mouse       (GowlBroadcastRule  *self,
                                                         GowlBroadcastMouse  mouse);
GowlBroadcastMouse  gowl_broadcast_rule_get_mouse       (GowlBroadcastRule  *self);
void                gowl_broadcast_rule_set_delay_ms    (GowlBroadcastRule  *self,
                                                         guint               delay_ms);
guint               gowl_broadcast_rule_get_delay_ms    (GowlBroadcastRule  *self);
void                gowl_broadcast_rule_set_local_block (GowlBroadcastRule  *self,
                                                         gboolean            block);
gboolean            gowl_broadcast_rule_get_local_block (GowlBroadcastRule  *self);

gboolean            gowl_broadcast_rule_check_one_to_one (GowlBroadcastRule *self,
                                                          GError           **error);

GowlBroadcastRule  *gowl_broadcast_rule_new_from_yaml   (const gchar        *yaml,
                                                         GError            **error);
gchar              *gowl_broadcast_rule_to_yaml         (GowlBroadcastRule  *self);

const gchar * const *gowl_broadcast_rule_fields         (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlBroadcastRule, gowl_broadcast_rule_unref)

/* --- Key names --- */

gboolean            gowl_broadcast_parse_key_set        (const gchar        *spec,
                                                         GHashTable         *into,
                                                         gboolean           *out_all,
                                                         GError            **error);
gboolean            gowl_broadcast_keysym_is_modifier   (guint32             keysym);

G_END_DECLS

#endif /* GOWL_BROADCAST_H */
