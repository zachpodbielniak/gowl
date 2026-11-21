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

#ifndef BAR_SLOTS_H
#define BAR_SLOTS_H

#include <glib.h>
#include <json-glib/json-glib.h>

#include "barkit/gowl-bar-layout.h"

G_BEGIN_DECLS

/*
 * Bar slots: bar items whose content is PUSHED from outside.
 *
 * Every other widget finds out what to show by itself -- it polls
 * /proc, runs a command, asks the compositor.  A slot is told.  Whoever
 * owns one (cmacs, a script holding the IPC socket) creates it with an
 * id, says where on the bar it goes, and from then on pushes text, a
 * glyph, a theme role, a tooltip and an urgency whenever they change.
 * A click is reported back to the owner as an event; nothing a slot
 * carries is ever run by the compositor.
 *
 * This file is the model only: the table of slots, what a request may
 * say, and the limits.  It knows nothing of items, plugins or the scene
 * graph -- gowl-module-bar.c places a slot on a bar, and the `slot'
 * plugin (bar-plugins-slots.c) draws it.
 */

/**
 * BAR_SLOTS_MAX:
 *
 * The most slots that can exist at once.  A bar has room for a few
 * dozen items; a pusher that wants more than this has a bug, and an
 * unbounded table is how one client's loop becomes the compositor's
 * memory.
 */
#define BAR_SLOTS_MAX (64)

/**
 * BAR_SLOT_ID_MAX:
 *
 * The longest slot id, owner tag or output name, in bytes.
 */
#define BAR_SLOT_ID_MAX (64)

/**
 * BAR_SLOT_TEXT_MAX:
 *
 * The longest text a slot shows, in bytes of UTF-8.  Longer text is cut
 * at a character boundary, not refused: a status line that grew is
 * still a status line.
 */
#define BAR_SLOT_TEXT_MAX (256)

/**
 * BAR_SLOT_ICON_MAX:
 *
 * The longest icon (a glyph or two), in bytes of UTF-8.
 */
#define BAR_SLOT_ICON_MAX (32)

/**
 * BAR_SLOT_TOOLTIP_MAX:
 *
 * The longest hover text, in bytes of UTF-8.
 */
#define BAR_SLOT_TOOLTIP_MAX (1024)

/**
 * BAR_SLOT_ORDER_MAX:
 *
 * The largest order a slot may ask for, either side of zero.
 */
#define BAR_SLOT_ORDER_MAX (1000)

/**
 * BAR_SLOT_TTL_MAX:
 *
 * The longest time-to-live, in seconds (a day).
 */
#define BAR_SLOT_TTL_MAX (86400)

/**
 * BarSlotUrgency:
 * @BAR_SLOT_LOW: background information; drawn muted unless a role is
 *   given
 * @BAR_SLOT_NORMAL: the default
 * @BAR_SLOT_CRITICAL: wants attention; drawn in the red role over a
 *   wash of it unless a role is given
 *
 * How loudly a slot speaks.
 */
typedef enum {
	BAR_SLOT_LOW = 0,
	BAR_SLOT_NORMAL,
	BAR_SLOT_CRITICAL
} BarSlotUrgency;

/**
 * BarSlot:
 * @id: the slot's name; unique
 * @owner: who made it, "" for nobody in particular
 * @bar: 0 for the top bar, 1 for the bottom one
 * @region: which third of the bar
 * @order: position among the slots of its region; a negative order
 *   sorts before the region's configured widgets, zero and up after
 * @output: (nullable): the one output it shows on, or %NULL for all
 * @text: what it says, "" for nothing
 * @icon: (nullable): a glyph drawn before the text
 * @role: (nullable): a theme role name for its colour, or %NULL for the
 *   urgency's default
 * @tooltip: (nullable): hover text
 * @urgency: how loudly it speaks
 * @click: whether a click is reported to the owner
 * @ttl: seconds after the last push at which it removes itself; 0 for
 *   never
 * @expires: g_get_monotonic_time() at which it is due, or 0
 * @serial: creation order, the tie-break between equal orders
 * @item: the host's item for it, opaque here; %NULL while it is on no
 *   bar
 *
 * One slot.  Owned by its #BarSlotStore.
 */
typedef struct {
	gchar          *id;
	gchar          *owner;
	gint            bar;
	GowlBarRegion   region;
	gint            order;
	gchar          *output;
	gchar          *text;
	gchar          *icon;
	gchar          *role;
	gchar          *tooltip;
	BarSlotUrgency  urgency;
	gboolean        click;
	guint           ttl;
	gint64          expires;
	guint           serial;
	gpointer        item;
} BarSlot;

typedef struct _BarSlotStore BarSlotStore;

/**
 * bar_slot_store_new:
 *
 * Returns: (transfer full): an empty store
 */
BarSlotStore *bar_slot_store_new (void);

/**
 * bar_slot_store_free:
 * @store: (nullable): a store
 *
 * Frees @store and every slot in it.  The host must already have taken
 * the slots' items off its bars.
 */
void bar_slot_store_free (BarSlotStore *store);

/**
 * bar_slot_store_size:
 * @store: a store
 *
 * Returns: how many slots there are
 */
guint bar_slot_store_size (BarSlotStore *store);

/**
 * bar_slot_store_lookup:
 * @store: a store
 * @id: a slot id
 *
 * Returns: (transfer none) (nullable): the slot, or %NULL
 */
BarSlot *bar_slot_store_lookup (BarSlotStore *store, const gchar *id);

/**
 * bar_slot_id_valid:
 * @id: (nullable): a candidate id, owner tag or output name
 *
 * An id is 1 to %BAR_SLOT_ID_MAX of `A-Z a-z 0-9 _ . -', the first of
 * them a letter, a digit or `_'.  Nothing else: an id travels as a word
 * on an event line and as a widget spec, where a space, a `:' or an `@'
 * each mean something, and as an argument, where a leading `-' does.
 *
 * Returns: %TRUE when @id may name a slot
 */
gboolean bar_slot_id_valid (const gchar *id);

/**
 * bar_slot_store_set:
 * @store: a store
 * @params: the request: `id' and any of `text', `icon', `role',
 *   `tooltip', `urgency', `click', `bar', `region', `order', `output',
 *   `owner', `ttl'
 * @now: g_get_monotonic_time()
 * @out_created: (out) (optional): whether the slot is new
 * @out_moved: (out) (optional): whether its bar, region or order
 *   changed, so the host has to place it again
 * @error: return location for a #GError in %GOWL_IPC_ERROR
 *
 * Creates the slot `id' names, or updates it.  Only the keys present
 * change; a string given as "" or null is cleared.  All or nothing: a
 * request with one bad value changes nothing.
 *
 * Refused when the id is not valid, a key is unknown or of the wrong
 * type, a role is not a theme role (a hex colour is not one), the
 * store is full, or the slot exists and belongs to another owner.
 *
 * Returns: (transfer none) (nullable): the slot, or %NULL with @error
 */
BarSlot *bar_slot_store_set (BarSlotStore  *store,
                             JsonObject    *params,
                             gint64         now,
                             gboolean      *out_created,
                             gboolean      *out_moved,
                             GError       **error);

/**
 * bar_slot_store_remove:
 * @store: a store
 * @id: a slot id
 *
 * Forgets one slot.  The host must already have taken its item off the
 * bar.
 *
 * Returns: %TRUE if there was one
 */
gboolean bar_slot_store_remove (BarSlotStore *store, const gchar *id);

/**
 * bar_slot_store_select:
 * @store: a store
 * @owner: (nullable): only slots of this owner, or %NULL for any
 * @expired_at: only slots due at this monotonic time, or 0 for any
 *
 * The ids of the slots that match, as copies: the caller removes them
 * one by one, which it could not do while walking the table.
 *
 * Returns: (transfer full) (element-type utf8): the ids
 */
GPtrArray *bar_slot_store_select (BarSlotStore *store,
                                  const gchar  *owner,
                                  gint64        expired_at);

/**
 * bar_slot_store_list:
 * @store: a store
 *
 * Every slot in the order the bar shows them: by bar, region, order,
 * then age.
 *
 * Returns: (transfer container) (element-type BarSlot): the slots
 */
GPtrArray *bar_slot_store_list (BarSlotStore *store);

/**
 * bar_slot_compare:
 * @a: a slot
 * @b: another
 *
 * Returns: negative, zero or positive as @a sorts before, with or
 *   after @b among the slots of one region
 */
gint bar_slot_compare (const BarSlot *a, const BarSlot *b);

/**
 * bar_slot_to_json:
 * @slot: a slot
 *
 * Returns: (transfer full): the slot as a JSON object
 */
JsonNode *bar_slot_to_json (const BarSlot *slot);

/**
 * bar_slot_store_to_json:
 * @store: a store
 * @owner: (nullable): only slots of this owner, or %NULL for all
 *
 * Returns: (transfer full): a JSON array of slots, in bar order
 */
JsonNode *bar_slot_store_to_json (BarSlotStore *store, const gchar *owner);

/**
 * bar_slot_urgency_name:
 * @urgency: an urgency
 *
 * Returns: (transfer none): `low', `normal' or `critical'
 */
const gchar *bar_slot_urgency_name (BarSlotUrgency urgency);

G_END_DECLS

#endif /* BAR_SLOTS_H */
