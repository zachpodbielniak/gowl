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

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-bar"

#include <string.h>

#include "ipc/gowl-ipc.h"
#include "barkit/gowl-bar-theme.h"

#include "bar-slots.h"

/**
 * SECTION:bar-slots
 * @title: Bar slots
 * @short_description: the table of pushed bar items
 *
 * The model behind `bar-slot-set' and friends.  See bar-slots.h for
 * what a slot is; this file is what a request may say and what happens
 * to one that says something else.
 *
 * Everything here runs on the compositor thread (under cmacs, with the
 * gowl lock held), touches no scene node and cannot block: a set is a
 * hash lookup and a handful of bounded string copies.
 */

struct _BarSlotStore {
	GHashTable *slots;     /* id -> BarSlot*, owned */
	guint       next_serial;
};

/* ----------------------------------------------------------------
 * Slots
 * ---------------------------------------------------------------- */

static void
bar_slot_free(
	gpointer data
){
	BarSlot *slot = data;

	if (slot == NULL)
		return;
	g_free(slot->id);
	g_free(slot->owner);
	g_free(slot->output);
	g_free(slot->text);
	g_free(slot->icon);
	g_free(slot->role);
	g_free(slot->tooltip);
	g_free(slot);
}

BarSlotStore *
bar_slot_store_new(void)
{
	BarSlotStore *store;

	store = g_new0(BarSlotStore, 1);
	/* The key is the slot's own id, so only the value is freed. */
	store->slots = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
	                                     bar_slot_free);
	store->next_serial = 1;
	return store;
}

void
bar_slot_store_free(
	BarSlotStore *store
){
	if (store == NULL)
		return;
	g_hash_table_unref(store->slots);
	g_free(store);
}

guint
bar_slot_store_size(
	BarSlotStore *store
){
	g_return_val_if_fail(store != NULL, 0);
	return g_hash_table_size(store->slots);
}

BarSlot *
bar_slot_store_lookup(
	BarSlotStore *store,
	const gchar  *id
){
	g_return_val_if_fail(store != NULL, NULL);
	if (id == NULL)
		return NULL;
	return g_hash_table_lookup(store->slots, id);
}

gboolean
bar_slot_id_valid(
	const gchar *id
){
	gsize i;

	if (id == NULL || id[0] == '\0')
		return FALSE;
	/* Not a `-' or a `.' first: `bar-slot-clear --all' must never be
	   somebody's owner tag, and a dot-file of an id helps nobody. */
	if (!g_ascii_isalnum(id[0]) && id[0] != '_')
		return FALSE;
	for (i = 0; id[i] != '\0'; i++) {
		if (i >= BAR_SLOT_ID_MAX)
			return FALSE;
		if (!g_ascii_isalnum(id[i]) && id[i] != '_' && id[i] != '.' &&
		    id[i] != '-')
			return FALSE;
	}
	return TRUE;
}

const gchar *
bar_slot_urgency_name(
	BarSlotUrgency urgency
){
	switch (urgency) {
	case BAR_SLOT_LOW:      return "low";
	case BAR_SLOT_CRITICAL: return "critical";
	case BAR_SLOT_NORMAL:
	default:                return "normal";
	}
}

gint
bar_slot_compare(
	const BarSlot *a,
	const BarSlot *b
){
	if (a->order != b->order)
		return (a->order < b->order) ? -1 : 1;
	if (a->serial != b->serial)
		return (a->serial < b->serial) ? -1 : 1;
	return 0;
}

/* ----------------------------------------------------------------
 * Reading a request
 * ---------------------------------------------------------------- */

/* The keys a request may carry.  Anything else is refused: a misspelt
   key that is silently ignored is a slot that never changes colour and
   nothing to say why. */
static const gchar * const slot_keys[] = {
	"id", "owner", "bar", "region", "order", "output", "text", "icon",
	"role", "tooltip", "urgency", "click", "ttl", NULL
};

static gboolean
slot_key_known(
	const gchar *key
){
	guint i;

	for (i = 0; slot_keys[i] != NULL; i++) {
		if (strcmp(slot_keys[i], key) == 0)
			return TRUE;
	}
	return FALSE;
}

/*
 * A string member.  *out is left NULL when the key is absent, set to ""
 * for JSON null (a null clears, as "" does), and to the string
 * otherwise -- borrowed from @params.
 */
static gboolean
slot_param_string(
	JsonObject   *params,
	const gchar  *key,
	const gchar **out,
	GError      **error
){
	JsonNode *node;

	*out = NULL;
	node = json_object_get_member(params, key);
	if (node == NULL)
		return TRUE;
	if (JSON_NODE_HOLDS_NULL(node)) {
		*out = "";
		return TRUE;
	}
	if (!JSON_NODE_HOLDS_VALUE(node) ||
	    json_node_get_value_type(node) != G_TYPE_STRING) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
		            "`%s' must be a string", key);
		return FALSE;
	}
	*out = json_node_get_string(node);
	if (*out == NULL)
		*out = "";
	return TRUE;
}

static gboolean
slot_param_int(
	JsonObject  *params,
	const gchar *key,
	gboolean    *present,
	gint64      *out,
	GError     **error
){
	JsonNode *node;

	*present = FALSE;
	node = json_object_get_member(params, key);
	if (node == NULL)
		return TRUE;
	if (!JSON_NODE_HOLDS_VALUE(node) ||
	    json_node_get_value_type(node) != G_TYPE_INT64) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
		            "`%s' must be an integer", key);
		return FALSE;
	}
	*out = json_node_get_int(node);
	*present = TRUE;
	return TRUE;
}

static gboolean
slot_param_bool(
	JsonObject  *params,
	const gchar *key,
	gboolean    *present,
	gboolean    *out,
	GError     **error
){
	JsonNode *node;

	*present = FALSE;
	node = json_object_get_member(params, key);
	if (node == NULL)
		return TRUE;
	if (!JSON_NODE_HOLDS_VALUE(node) ||
	    json_node_get_value_type(node) != G_TYPE_BOOLEAN) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
		            "`%s' must be true or false", key);
		return FALSE;
	}
	*out = json_node_get_boolean(node);
	*present = TRUE;
	return TRUE;
}

/*
 * A copy of @text fit to show: at most @max bytes, cut at a character
 * boundary, with every control character turned into a space (a bar is
 * one line, and an escape sequence in a status text is somebody else's
 * terminal talking).
 *
 * Bounded by @max whatever the length of @text: only the part kept is
 * ever looked at, so a megabyte pushed as a status costs what 256 bytes
 * do.  %NULL with @error set when the part kept is not UTF-8.
 */
static gchar *
slot_clean_text(
	const gchar *key,
	const gchar *text,
	gsize        max,
	GError     **error
){
	gsize  len, i;
	gchar *out;

	len = strnlen(text, max + 1);
	if (len > max) {
		len = max;
		/* Back off a continuation byte at a time, so the cut never
		   lands inside a character. */
		while (len > 0 && ((guchar)text[len] & 0xC0) == 0x80)
			len--;
	}
	if (!g_utf8_validate(text, (gssize)len, NULL)) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
		            "`%s' is not valid UTF-8", key);
		return NULL;
	}

	out = g_strndup(text, len);
	for (i = 0; i < len; i++) {
		/* Control characters are single bytes in UTF-8, so this
		   cannot split a character. */
		if ((guchar)out[i] < 0x20 || (guchar)out[i] == 0x7F)
			out[i] = ' ';
	}
	return out;
}

/* What a request asks for, checked but not yet applied.  A NULL string
   means "not mentioned". */
typedef struct {
	const gchar    *id;
	const gchar    *owner;
	gboolean        has_bar;
	gint            bar;
	gboolean        has_region;
	GowlBarRegion   region;
	gboolean        has_order;
	gint            order;
	const gchar    *output;
	gchar          *text;        /* owned */
	gchar          *icon;        /* owned */
	const gchar    *role;
	gchar          *tooltip;     /* owned */
	gboolean        has_urgency;
	BarSlotUrgency  urgency;
	gboolean        has_click;
	gboolean        click;
	gboolean        has_ttl;
	guint           ttl;
} SlotPatch;

static void
slot_patch_clear(
	SlotPatch *patch
){
	g_free(patch->text);
	g_free(patch->icon);
	g_free(patch->tooltip);
}

/* Check every key of @params into @patch.  FALSE with @error set on the
   first thing wrong; @patch must be cleared either way. */
static gboolean
slot_patch_read(
	JsonObject *params,
	SlotPatch  *patch,
	GError    **error
){
	GList       *members, *l;
	const gchar *str;
	gboolean     present;
	gint64       num;

	memset(patch, 0, sizeof(*patch));

	members = json_object_get_members(params);
	for (l = members; l != NULL; l = l->next) {
		if (!slot_key_known((const gchar *)l->data)) {
			g_set_error(error, GOWL_IPC_ERROR,
			            GOWL_IPC_ERROR_INVALID_PARAMS,
			            "a slot has no `%s'", (const gchar *)l->data);
			g_list_free(members);
			return FALSE;
		}
	}
	g_list_free(members);

	if (!slot_param_string(params, "id", &patch->id, error))
		return FALSE;
	if (!bar_slot_id_valid(patch->id)) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
		            "a slot id is 1 to %d of A-Z a-z 0-9 _ . - and "
		            "starts with a letter, a digit or _",
		            BAR_SLOT_ID_MAX);
		return FALSE;
	}

	if (!slot_param_string(params, "owner", &patch->owner, error))
		return FALSE;
	if (patch->owner != NULL && patch->owner[0] != '\0' &&
	    !bar_slot_id_valid(patch->owner)) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
		            "an owner is at most %d of A-Z a-z 0-9 _ . -",
		            BAR_SLOT_ID_MAX);
		return FALSE;
	}

	if (!slot_param_string(params, "bar", &str, error))
		return FALSE;
	if (str != NULL) {
		if (g_ascii_strcasecmp(str, "top") == 0) {
			patch->bar = 0;
		} else if (g_ascii_strcasecmp(str, "bottom") == 0) {
			patch->bar = 1;
		} else {
			g_set_error(error, GOWL_IPC_ERROR,
			            GOWL_IPC_ERROR_INVALID_PARAMS,
			            "`bar' is top or bottom, not `%s'", str);
			return FALSE;
		}
		patch->has_bar = TRUE;
	}

	if (!slot_param_string(params, "region", &str, error))
		return FALSE;
	if (str != NULL) {
		if (!gowl_bar_layout_region_from_string(str, &patch->region)) {
			g_set_error(error, GOWL_IPC_ERROR,
			            GOWL_IPC_ERROR_INVALID_PARAMS,
			            "`region' is left, center or right, not `%s'",
			            str);
			return FALSE;
		}
		patch->has_region = TRUE;
	}

	if (!slot_param_int(params, "order", &present, &num, error))
		return FALSE;
	if (present) {
		patch->order = (gint)CLAMP(num, -BAR_SLOT_ORDER_MAX,
		                           BAR_SLOT_ORDER_MAX);
		patch->has_order = TRUE;
	}

	/* An output is named, not looked up: a slot for a screen that is
	   unplugged right now is still a slot for that screen. */
	if (!slot_param_string(params, "output", &patch->output, error))
		return FALSE;
	if (patch->output != NULL && patch->output[0] != '\0' &&
	    !bar_slot_id_valid(patch->output)) {
		g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_INVALID_PARAMS,
		            "`output' is an output name such as eDP-1");
		return FALSE;
	}

	if (!slot_param_string(params, "text", &str, error))
		return FALSE;
	if (str != NULL) {
		patch->text = slot_clean_text("text", str, BAR_SLOT_TEXT_MAX,
		                              error);
		if (patch->text == NULL)
			return FALSE;
	}

	if (!slot_param_string(params, "icon", &str, error))
		return FALSE;
	if (str != NULL) {
		patch->icon = slot_clean_text("icon", str, BAR_SLOT_ICON_MAX,
		                              error);
		if (patch->icon == NULL)
			return FALSE;
	}

	if (!slot_param_string(params, "tooltip", &str, error))
		return FALSE;
	if (str != NULL) {
		patch->tooltip = slot_clean_text("tooltip", str,
		                                 BAR_SLOT_TOOLTIP_MAX, error);
		if (patch->tooltip == NULL)
			return FALSE;
	}

	/* A role and nothing else.  The user may put a hex literal in
	   their own configuration; a pusher may not, because what it
	   pushed would stay that colour when the palette changed. */
	if (!slot_param_string(params, "role", &patch->role, error))
		return FALSE;
	if (patch->role != NULL && patch->role[0] != '\0') {
		GowlBarColor role;

		if (!gowl_bar_theme_color_from_name(patch->role, &role)) {
			g_set_error(error, GOWL_IPC_ERROR,
			            GOWL_IPC_ERROR_INVALID_PARAMS,
			            "`%s' is not a theme role (text, accent, red, "
			            "green, yellow, ...); a slot takes a role, "
			            "never a colour", patch->role);
			return FALSE;
		}
	}

	if (!slot_param_string(params, "urgency", &str, error))
		return FALSE;
	if (str != NULL) {
		if (str[0] == '\0' || g_ascii_strcasecmp(str, "normal") == 0) {
			patch->urgency = BAR_SLOT_NORMAL;
		} else if (g_ascii_strcasecmp(str, "low") == 0) {
			patch->urgency = BAR_SLOT_LOW;
		} else if (g_ascii_strcasecmp(str, "critical") == 0) {
			patch->urgency = BAR_SLOT_CRITICAL;
		} else {
			g_set_error(error, GOWL_IPC_ERROR,
			            GOWL_IPC_ERROR_INVALID_PARAMS,
			            "`urgency' is low, normal or critical, not `%s'",
			            str);
			return FALSE;
		}
		patch->has_urgency = TRUE;
	}

	if (!slot_param_bool(params, "click", &patch->has_click,
	                     &patch->click, error))
		return FALSE;

	if (!slot_param_int(params, "ttl", &present, &num, error))
		return FALSE;
	if (present) {
		patch->ttl = (guint)CLAMP(num, 0, BAR_SLOT_TTL_MAX);
		patch->has_ttl = TRUE;
	}

	return TRUE;
}

/* Replace *@field with @value, "" meaning none. */
static void
slot_set_nullable(
	gchar      **field,
	const gchar *value
){
	g_free(*field);
	*field = (value != NULL && value[0] != '\0') ? g_strdup(value) : NULL;
}

BarSlot *
bar_slot_store_set(
	BarSlotStore  *store,
	JsonObject    *params,
	gint64         now,
	gboolean      *out_created,
	gboolean      *out_moved,
	GError       **error
){
	SlotPatch  patch;
	BarSlot   *slot;
	gboolean   created = FALSE;
	gboolean   moved = FALSE;

	g_return_val_if_fail(store != NULL, NULL);
	g_return_val_if_fail(params != NULL, NULL);

	if (!slot_patch_read(params, &patch, error)) {
		slot_patch_clear(&patch);
		return NULL;
	}

	slot = g_hash_table_lookup(store->slots, patch.id);

	if (slot != NULL) {
		/*
		 * A slot is changed by whoever made it.  Two pushers that
		 * picked the same id would otherwise take turns overwriting
		 * each other for ever, each seeing its own push succeed.
		 */
		if (g_strcmp0(slot->owner,
		              (patch.owner != NULL) ? patch.owner : "") != 0) {
			g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_REFUSED,
			            "slot `%s' belongs to %s%s%s", slot->id,
			            (slot->owner[0] != '\0') ? "`" : "",
			            (slot->owner[0] != '\0') ? slot->owner
			                                     : "nobody",
			            (slot->owner[0] != '\0') ? "'" : "");
			slot_patch_clear(&patch);
			return NULL;
		}
	} else {
		if (g_hash_table_size(store->slots) >= BAR_SLOTS_MAX) {
			g_set_error(error, GOWL_IPC_ERROR, GOWL_IPC_ERROR_REFUSED,
			            "there are already %d bar slots; remove one "
			            "first", BAR_SLOTS_MAX);
			slot_patch_clear(&patch);
			return NULL;
		}

		slot = g_new0(BarSlot, 1);
		slot->id      = g_strdup(patch.id);
		slot->owner   = g_strdup((patch.owner != NULL) ? patch.owner : "");
		slot->bar     = 0;
		slot->region  = GOWL_BAR_REGION_RIGHT;
		slot->order   = 0;
		slot->text    = g_strdup("");
		slot->urgency = BAR_SLOT_NORMAL;
		slot->serial  = store->next_serial++;
		g_hash_table_insert(store->slots, slot->id, slot);
		created = TRUE;
		moved   = TRUE;
	}

	/* Nothing below can fail: the request was checked whole. */
	if (patch.has_bar && patch.bar != slot->bar) {
		slot->bar = patch.bar;
		moved = TRUE;
	}
	if (patch.has_region && patch.region != slot->region) {
		slot->region = patch.region;
		moved = TRUE;
	}
	if (patch.has_order && patch.order != slot->order) {
		slot->order = patch.order;
		moved = TRUE;
	}
	if (patch.output != NULL)
		slot_set_nullable(&slot->output, patch.output);
	if (patch.text != NULL) {
		g_free(slot->text);
		slot->text = g_steal_pointer(&patch.text);
	}
	if (patch.icon != NULL)
		slot_set_nullable(&slot->icon, patch.icon);
	if (patch.role != NULL)
		slot_set_nullable(&slot->role, patch.role);
	if (patch.tooltip != NULL)
		slot_set_nullable(&slot->tooltip, patch.tooltip);
	if (patch.has_urgency)
		slot->urgency = patch.urgency;
	if (patch.has_click)
		slot->click = patch.click;
	if (patch.has_ttl)
		slot->ttl = patch.ttl;

	/* Every push is a heartbeat: a slot with a time-to-live lives that
	   long past the LAST thing its owner said, not the first. */
	slot->expires = (slot->ttl > 0)
		? now + (gint64)slot->ttl * G_USEC_PER_SEC : 0;

	slot_patch_clear(&patch);

	if (out_created != NULL)
		*out_created = created;
	if (out_moved != NULL)
		*out_moved = moved;
	return slot;
}

gboolean
bar_slot_store_remove(
	BarSlotStore *store,
	const gchar  *id
){
	g_return_val_if_fail(store != NULL, FALSE);
	if (id == NULL)
		return FALSE;
	return g_hash_table_remove(store->slots, id);
}

GPtrArray *
bar_slot_store_select(
	BarSlotStore *store,
	const gchar  *owner,
	gint64        expired_at
){
	GPtrArray      *ids;
	GHashTableIter  iter;
	gpointer        value;

	g_return_val_if_fail(store != NULL, NULL);

	ids = g_ptr_array_new_with_free_func(g_free);
	g_hash_table_iter_init(&iter, store->slots);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		BarSlot *slot = value;

		if (owner != NULL && strcmp(slot->owner, owner) != 0)
			continue;
		if (expired_at != 0 &&
		    (slot->expires == 0 || slot->expires > expired_at))
			continue;
		g_ptr_array_add(ids, g_strdup(slot->id));
	}
	return ids;
}

static gint
slot_list_compare(
	gconstpointer a,
	gconstpointer b
){
	const BarSlot *sa = *(const BarSlot * const *)a;
	const BarSlot *sb = *(const BarSlot * const *)b;

	if (sa->bar != sb->bar)
		return sa->bar - sb->bar;
	if (sa->region != sb->region)
		return (gint)sa->region - (gint)sb->region;
	return bar_slot_compare(sa, sb);
}

GPtrArray *
bar_slot_store_list(
	BarSlotStore *store
){
	GPtrArray      *list;
	GHashTableIter  iter;
	gpointer        value;

	g_return_val_if_fail(store != NULL, NULL);

	list = g_ptr_array_new();
	g_hash_table_iter_init(&iter, store->slots);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(list, value);
	/* The comparison is total (serials are unique), so an unstable
	   sort still gives one answer. */
	g_ptr_array_sort(list, slot_list_compare);
	return list;
}

JsonNode *
bar_slot_to_json(
	const BarSlot *slot
){
	JsonObject *obj;
	JsonNode   *node;

	g_return_val_if_fail(slot != NULL, NULL);

	obj = json_object_new();
	json_object_set_string_member(obj, "id", slot->id);
	json_object_set_string_member(obj, "owner", slot->owner);
	json_object_set_string_member(obj, "bar",
	                              (slot->bar == 1) ? "bottom" : "top");
	json_object_set_string_member(obj, "region",
		gowl_bar_layout_region_name(slot->region));
	json_object_set_int_member(obj, "order", slot->order);
	json_object_set_string_member(obj, "output",
		(slot->output != NULL) ? slot->output : "");
	json_object_set_string_member(obj, "text", slot->text);
	json_object_set_string_member(obj, "icon",
		(slot->icon != NULL) ? slot->icon : "");
	json_object_set_string_member(obj, "role",
		(slot->role != NULL) ? slot->role : "");
	json_object_set_string_member(obj, "tooltip",
		(slot->tooltip != NULL) ? slot->tooltip : "");
	json_object_set_string_member(obj, "urgency",
		bar_slot_urgency_name(slot->urgency));
	json_object_set_boolean_member(obj, "click", slot->click);
	json_object_set_int_member(obj, "ttl", slot->ttl);
	/* Whether it is on a bar right now.  False for a slot whose bar
	   is switched off, or while the `slot' plugin is held back. */
	json_object_set_boolean_member(obj, "placed", slot->item != NULL);

	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, obj);
	return node;
}

JsonNode *
bar_slot_store_to_json(
	BarSlotStore *store,
	const gchar  *owner
){
	g_autoptr(GPtrArray) list = NULL;
	JsonArray *array;
	JsonNode  *node;
	guint      i;

	g_return_val_if_fail(store != NULL, NULL);

	list  = bar_slot_store_list(store);
	array = json_array_new();
	for (i = 0; i < list->len; i++) {
		const BarSlot *slot = g_ptr_array_index(list, i);

		if (owner != NULL && strcmp(slot->owner, owner) != 0)
			continue;
		json_array_add_element(array, bar_slot_to_json(slot));
	}

	node = json_node_new(JSON_NODE_ARRAY);
	json_node_take_array(node, array);
	return node;
}
