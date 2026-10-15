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
 * gowl-broadcast.c - Teams and rules for input broadcasting.
 *
 * Pure data and validation: nothing here knows about windows, the seat
 * or the compositor.  A team says how windows are recognised; a rule
 * says what is repeated where.  The broadcast module binds slots to live
 * windows and does the delivery.
 *
 * Validation happens on the way in, so the config loader (and the IPC
 * `broadcast-add-*' verbs) can reject a bad entry with a reason:
 *
 *   - a key name must be a keysym (`a', `F1', `KP_Add', `space') or a
 *     range of them (`1-9', `a-z', `F1-F12', `KP_0-KP_9');
 *   - `match' and `when' are gowl-filter expressions over the fields in
 *     gowl_broadcast_slot_fields() / gowl_broadcast_rule_fields();
 *   - targets are `all', `all-other', `current', `under-mouse',
 *     `slot:NAME' or `group:NAME'.
 *
 * Whether a rule's team, slots and groups actually exist is a question
 * about the whole configuration, answered by the module when it loads
 * the set, not here.
 */

#include "gowl-broadcast.h"
#include "gowl-broadcast-private.h"

#include "config/gowl-keybind.h"
#include "util/gowl-filter.h"

#include <string.h>
#include <xkbcommon/xkbcommon.h>

G_DEFINE_QUARK(gowl-broadcast-error-quark, gowl_broadcast_error)

G_DEFINE_ENUM_TYPE(GowlBroadcastMode, gowl_broadcast_mode,
	G_DEFINE_ENUM_VALUE(GOWL_BROADCAST_MODE_MIRROR, "mirror"),
	G_DEFINE_ENUM_VALUE(GOWL_BROADCAST_MODE_ROUND_ROBIN, "round-robin"))

G_DEFINE_ENUM_TYPE(GowlBroadcastMouse, gowl_broadcast_mouse,
	G_DEFINE_ENUM_VALUE(GOWL_BROADCAST_MOUSE_OFF, "off"),
	G_DEFINE_ENUM_VALUE(GOWL_BROADCAST_MOUSE_SAME, "same"),
	G_DEFINE_ENUM_VALUE(GOWL_BROADCAST_MOUSE_SCALED, "scaled"))

G_DEFINE_ENUM_TYPE(GowlBroadcastTargetKind, gowl_broadcast_target_kind,
	G_DEFINE_ENUM_VALUE(GOWL_BROADCAST_TARGET_ALL, "all"),
	G_DEFINE_ENUM_VALUE(GOWL_BROADCAST_TARGET_ALL_OTHER, "all-other"),
	G_DEFINE_ENUM_VALUE(GOWL_BROADCAST_TARGET_CURRENT, "current"),
	G_DEFINE_ENUM_VALUE(GOWL_BROADCAST_TARGET_SLOT, "slot"),
	G_DEFINE_ENUM_VALUE(GOWL_BROADCAST_TARGET_GROUP, "group"),
	G_DEFINE_ENUM_VALUE(GOWL_BROADCAST_TARGET_UNDER_MOUSE, "under-mouse"))

/* What a slot's `match' may test: facts about one window. */
static const gchar * const slot_fields[] = {
	"app-id", "title", "pid", "xwayland",
	"tag", "tags", "monitor", "floating", "fullscreen",
	NULL
};

/* What a rule's `when' may test: the focused window, and where it sits
 * in its team. */
static const gchar * const rule_fields[] = {
	"team", "focused-slot",
	"app-id", "title", "pid", "xwayland",
	"tag", "tags", "monitor", "floating", "fullscreen",
	NULL
};

/* ── Small helpers ───────────────────────────────────────────────────── */

/* A YAML double-quoted scalar. */
static void
append_quoted(
	GString     *out,
	const gchar *s
){
	const gchar *p;

	g_string_append_c(out, '"');
	for (p = s; p != NULL && *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_c(out, '\\');
		if (*p == '\n')
			g_string_append(out, "\\n");
		else
			g_string_append_c(out, *p);
	}
	g_string_append_c(out, '"');
}

/* "[a, b, c]" in quoted form, or "[]" */
static void
append_strv(
	GString             *out,
	const gchar * const *v
){
	guint i;

	g_string_append_c(out, '[');
	for (i = 0; v != NULL && v[i] != NULL; i++) {
		if (i > 0)
			g_string_append(out, ", ");
		append_quoted(out, v[i]);
	}
	g_string_append_c(out, ']');
}

/* Names are identifiers people type into IPC and Lisp: no spaces, no
 * colons (slot:NAME), no control characters. */
static gboolean
name_ok(
	const gchar  *name,
	const gchar  *what,
	GError      **error
){
	const gchar *p;

	if (name == NULL || *name == '\0') {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_INVALID, "%s needs a name", what);
		return FALSE;
	}
	for (p = name; *p != '\0'; p++) {
		if (g_ascii_isspace(*p) || *p == ':' || g_ascii_iscntrl(*p)) {
			g_set_error(error, GOWL_BROADCAST_ERROR,
			            GOWL_BROADCAST_ERROR_INVALID,
			            "%s name `%s' may not contain spaces or `:'",
			            what, name);
			return FALSE;
		}
	}
	return TRUE;
}

/* One keysym name to a keysym: the xkb name, case-sensitive first
 * (`A' and `a' are different keysyms), then case-insensitive, then a
 * single character by its code point.  Letters are folded to lower
 * case: a rule names a KEY, and the key's level-0 symbol is lower. */
static gboolean
keysym_from_name(
	const gchar *name,
	guint32     *out
){
	xkb_keysym_t sym;

	sym = xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
	if (sym == XKB_KEY_NoSymbol)
		sym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
	if (sym == XKB_KEY_NoSymbol && g_utf8_strlen(name, -1) == 1)
		sym = xkb_utf32_to_keysym(g_utf8_get_char(name));
	if (sym == XKB_KEY_NoSymbol)
		return FALSE;
	*out = (guint32)xkb_keysym_to_lower(sym);
	return TRUE;
}

/* gowl_keybind_parse() without its warning: the last `+' token must be
 * a keysym before it is asked, so a bad name is one GError, reported
 * once, with the entry that holds it. */
static gboolean
bind_parse_quiet(
	const gchar *text,
	guint       *out_mods,
	guint       *out_sym
){
	const gchar *last;

	if (text == NULL || *text == '\0')
		return FALSE;
	last = strrchr(text, '+');
	last = (last != NULL && last[1] != '\0') ? last + 1 : text;
	if (xkb_keysym_from_name(last, XKB_KEYSYM_CASE_INSENSITIVE)
	    == XKB_KEY_NoSymbol)
		return FALSE;
	return gowl_keybind_parse(text, out_mods, out_sym) && *out_sym != 0;
}

/* Splits "F12" into "F" and 12; FALSE when there are no trailing digits
 * or nothing before them. */
static gboolean
split_number(
	const gchar  *s,
	gchar       **out_prefix,
	guint        *out_number
){
	gsize len;
	gsize i;
	guint64 n;

	len = strlen(s);
	i = len;
	while (i > 0 && g_ascii_isdigit(s[i - 1]))
		i--;
	if (i == len || i == 0)
		return FALSE;
	if (!g_ascii_string_to_unsigned(s + i, 10, 0, 999, &n, NULL))
		return FALSE;
	*out_prefix = g_strndup(s, i);
	*out_number = (guint)n;
	return TRUE;
}

/**
 * gowl_broadcast_keysym_is_modifier:
 * @keysym: an XKB keysym
 *
 * Modifier and lock keys are never broadcast as keys of their own: the
 * modifiers a key was typed with travel with it, and a Shift pressed
 * into a background window on its own would leave it shifted.
 *
 * Returns: %TRUE for Shift, Control, Alt, Meta, Super, Hyper, the ISO
 *   level and group keys, Caps Lock, Num Lock and Mode_switch
 */
gboolean
gowl_broadcast_keysym_is_modifier(guint32 keysym)
{
	if (keysym >= XKB_KEY_Shift_L && keysym <= XKB_KEY_Hyper_R)
		return TRUE;
	if (keysym >= XKB_KEY_ISO_Lock && keysym <= XKB_KEY_ISO_Last_Group_Lock)
		return TRUE;
	return keysym == XKB_KEY_Num_Lock || keysym == XKB_KEY_Mode_switch;
}

/**
 * gowl_broadcast_parse_key_set:
 * @spec: one key name, a range (`1-9', `a-z', `F1-F12', `KP_0-KP_9'),
 *   or `all'
 * @into: (element-type guint32 guint32): a set of keysyms to add to
 *   (keys and values GUINT_TO_POINTER(keysym))
 * @out_all: (out) (optional): set to %TRUE for `all'
 * @error: return location for a #GError
 *
 * A dash is a range only between two names (`-' alone, or `minus', is
 * the minus key); the two ends are either single characters, giving a
 * code-point range, or one prefix with two numbers.
 *
 * Returns: %TRUE when @spec was understood
 */
gboolean
gowl_broadcast_parse_key_set(
	const gchar  *spec,
	GHashTable   *into,
	gboolean     *out_all,
	GError      **error
){
	const gchar *dash;
	guint32 sym;

	g_return_val_if_fail(spec != NULL, FALSE);
	g_return_val_if_fail(into != NULL, FALSE);

	if (g_ascii_strcasecmp(spec, "all") == 0) {
		if (out_all != NULL)
			*out_all = TRUE;
		return TRUE;
	}

	/* A range: a dash with something on both sides */
	dash = strchr(spec + 1, '-');
	if (dash != NULL && dash[1] != '\0' && spec[0] != '\0') {
		g_autofree gchar *lo = g_strndup(spec, (gsize)(dash - spec));
		const gchar *hi = dash + 1;

		if (g_utf8_strlen(lo, -1) == 1 && g_utf8_strlen(hi, -1) == 1) {
			gunichar a = g_utf8_get_char(lo);
			gunichar b = g_utf8_get_char(hi);
			gunichar c;

			if (a > b || b - a > 255) {
				g_set_error(error, GOWL_BROADCAST_ERROR,
				            GOWL_BROADCAST_ERROR_UNKNOWN_KEY,
				            "key range `%s' runs backwards or is "
				            "too wide", spec);
				return FALSE;
			}
			for (c = a; c <= b; c++) {
				xkb_keysym_t s = xkb_utf32_to_keysym(c);

				if (s != XKB_KEY_NoSymbol)
					g_hash_table_add(into, GUINT_TO_POINTER(
						(guint32)xkb_keysym_to_lower(s)));
			}
			return TRUE;
		} else {
			g_autofree gchar *pa = NULL;
			g_autofree gchar *pb = NULL;
			guint na;
			guint nb;
			guint n;

			if (!split_number(lo, &pa, &na)
			    || !split_number(hi, &pb, &nb)
			    || strcmp(pa, pb) != 0 || na > nb) {
				g_set_error(error, GOWL_BROADCAST_ERROR,
				            GOWL_BROADCAST_ERROR_UNKNOWN_KEY,
				            "`%s' is not a key range (try 1-9, a-z, "
				            "F1-F12 or KP_0-KP_9)", spec);
				return FALSE;
			}
			for (n = na; n <= nb; n++) {
				g_autofree gchar *name = g_strdup_printf("%s%u",
				                                         pa, n);

				if (!keysym_from_name(name, &sym)) {
					g_set_error(error, GOWL_BROADCAST_ERROR,
					            GOWL_BROADCAST_ERROR_UNKNOWN_KEY,
					            "`%s' in range `%s' is no key",
					            name, spec);
					return FALSE;
				}
				g_hash_table_add(into, GUINT_TO_POINTER(sym));
			}
			return TRUE;
		}
	}

	if (!keysym_from_name(spec, &sym)) {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_UNKNOWN_KEY,
		            "`%s' is no key name (xkb keysym names: a, F1, "
		            "space, KP_Add, ...)", spec);
		return FALSE;
	}
	g_hash_table_add(into, GUINT_TO_POINTER(sym));
	return TRUE;
}

/* ── Teams ───────────────────────────────────────────────────────────── */

typedef struct {
	gchar           *name;
	gchar           *match;       /* filter text, nullable */
	GowlFilter      *filter;      /* parsed @match, nullable */
	gchar           *focus_key;   /* keybind text, nullable */
	guint32          focus_mods;
	guint32          focus_sym;
	GowlKeyDelivery  delivery;
} Slot;

struct _GowlBroadcastTeam {
	gint        ref_count;
	gchar      *name;
	GPtrArray  *slots;      /* element-type Slot* */
	GPtrArray  *group_order; /* element-type gchar*, insertion order */
	GHashTable *groups;     /* name -> GStrv of slot names */
};

G_DEFINE_BOXED_TYPE(GowlBroadcastTeam, gowl_broadcast_team,
                    gowl_broadcast_team_ref, gowl_broadcast_team_unref)

static void
slot_free(gpointer data)
{
	Slot *s = data;

	g_free(s->name);
	g_free(s->match);
	g_clear_pointer(&s->filter, gowl_filter_free);
	g_free(s->focus_key);
	g_free(s);
}

/**
 * gowl_broadcast_team_new:
 * @name: the team's name
 *
 * Returns: (transfer full): a new, empty team
 */
GowlBroadcastTeam *
gowl_broadcast_team_new(const gchar *name)
{
	GowlBroadcastTeam *self;

	g_return_val_if_fail(name != NULL, NULL);

	self = g_new0(GowlBroadcastTeam, 1);
	self->ref_count = 1;
	self->name = g_strdup(name);
	self->slots = g_ptr_array_new_with_free_func(slot_free);
	self->group_order = g_ptr_array_new_with_free_func(g_free);
	self->groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                     (GDestroyNotify)g_strfreev);
	return self;
}

/**
 * gowl_broadcast_team_ref:
 * @self: a #GowlBroadcastTeam
 *
 * Returns: (transfer full): @self
 */
GowlBroadcastTeam *
gowl_broadcast_team_ref(GowlBroadcastTeam *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	g_atomic_int_inc(&self->ref_count);
	return self;
}

/**
 * gowl_broadcast_team_unref:
 * @self: (transfer full): a #GowlBroadcastTeam
 */
void
gowl_broadcast_team_unref(GowlBroadcastTeam *self)
{
	if (self == NULL)
		return;
	if (!g_atomic_int_dec_and_test(&self->ref_count))
		return;
	g_free(self->name);
	g_ptr_array_unref(self->slots);
	g_ptr_array_unref(self->group_order);
	g_hash_table_unref(self->groups);
	g_free(self);
}

/**
 * gowl_broadcast_team_get_name:
 * @self: a #GowlBroadcastTeam
 *
 * Returns: (transfer none): the team's name
 */
const gchar *
gowl_broadcast_team_get_name(GowlBroadcastTeam *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->name;
}

/**
 * gowl_broadcast_team_add_slot:
 * @self: a #GowlBroadcastTeam
 * @name: the slot's name, unique in the team
 * @match: (nullable): a gowl-filter expression over
 *   gowl_broadcast_slot_fields() picking the window that fills the
 *   slot; %NULL for a slot only ever filled by hand (`broadcast-assign')
 * @focus_key: (nullable): a keybind (`Super+F1') that focuses the slot's
 *   window from anywhere -- ISBoxer's per-slot focus hotkey
 * @delivery: how keys reach this slot's window, unless a rule says
 *   otherwise
 * @error: return location for a #GError
 *
 * Slots are filled in order: the first window matching slot 1, then the
 * first other window matching slot 2, and so on, so two slots with the
 * same @match take two windows of the same program.
 *
 * Returns: %TRUE when added
 */
gboolean
gowl_broadcast_team_add_slot(
	GowlBroadcastTeam  *self,
	const gchar        *name,
	const gchar        *match,
	const gchar        *focus_key,
	GowlKeyDelivery     delivery,
	GError            **error
){
	g_autoptr(GowlFilter) filter = NULL;
	Slot *slot;
	guint mods = 0;
	guint sym = 0;

	g_return_val_if_fail(self != NULL, FALSE);

	if (!name_ok(name, "a slot", error))
		return FALSE;
	if (gowl_broadcast_team_find_slot(self, name) >= 0) {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_INVALID,
		            "team `%s' already has a slot `%s'", self->name, name);
		return FALSE;
	}
	if (match != NULL && *match != '\0') {
		g_autoptr(GError) ferr = NULL;

		filter = gowl_filter_parse(match, slot_fields, &ferr);
		if (filter == NULL) {
			g_set_error(error, GOWL_BROADCAST_ERROR,
			            GOWL_BROADCAST_ERROR_FILTER,
			            "slot `%s' match: %s", name, ferr->message);
			return FALSE;
		}
	}
	if (focus_key != NULL && *focus_key != '\0'
	    && !bind_parse_quiet(focus_key, &mods, &sym)) {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_UNKNOWN_KEY,
		            "slot `%s' focus-key `%s' is no keybind", name,
		            focus_key);
		return FALSE;
	}

	slot = g_new0(Slot, 1);
	slot->name = g_strdup(name);
	slot->match = (match != NULL && *match != '\0') ? g_strdup(match) : NULL;
	slot->filter = g_steal_pointer(&filter);
	slot->focus_key = (focus_key != NULL && *focus_key != '\0')
	                  ? g_strdup(focus_key) : NULL;
	slot->focus_mods = (guint32)mods;
	slot->focus_sym = (guint32)sym;
	slot->delivery = delivery;
	g_ptr_array_add(self->slots, slot);
	return TRUE;
}

/**
 * gowl_broadcast_team_get_n_slots:
 * @self: a #GowlBroadcastTeam
 *
 * Returns: how many slots the team has
 */
guint
gowl_broadcast_team_get_n_slots(GowlBroadcastTeam *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->slots->len;
}

/**
 * gowl_broadcast_team_find_slot:
 * @self: a #GowlBroadcastTeam
 * @name: a slot name, or a 1-based slot number ("2")
 *
 * Returns: the slot's index, or -1
 */
gint
gowl_broadcast_team_find_slot(
	GowlBroadcastTeam *self,
	const gchar       *name
){
	guint64 n;
	guint i;

	g_return_val_if_fail(self != NULL, -1);
	if (name == NULL)
		return -1;
	for (i = 0; i < self->slots->len; i++)
		if (strcmp(((Slot *)g_ptr_array_index(self->slots, i))->name,
		           name) == 0)
			return (gint)i;
	if (self->slots->len > 0
	    && g_ascii_string_to_unsigned(name, 10, 1, self->slots->len, &n,
	                                  NULL))
		return (gint)n - 1;
	return -1;
}

static Slot *
slot_at(
	GowlBroadcastTeam *self,
	guint              index
){
	g_return_val_if_fail(self != NULL, NULL);
	g_return_val_if_fail(index < self->slots->len, NULL);
	return g_ptr_array_index(self->slots, index);
}

/**
 * gowl_broadcast_team_get_slot_name:
 * @self: a #GowlBroadcastTeam
 * @index: a slot index
 *
 * Returns: (transfer none): the slot's name
 */
const gchar *
gowl_broadcast_team_get_slot_name(
	GowlBroadcastTeam *self,
	guint              index
){
	Slot *s = slot_at(self, index);

	return s != NULL ? s->name : NULL;
}

/**
 * gowl_broadcast_team_get_slot_match:
 * @self: a #GowlBroadcastTeam
 * @index: a slot index
 *
 * Returns: (transfer none) (nullable): the slot's match expression
 */
const gchar *
gowl_broadcast_team_get_slot_match(
	GowlBroadcastTeam *self,
	guint              index
){
	Slot *s = slot_at(self, index);

	return s != NULL ? s->match : NULL;
}

/**
 * gowl_broadcast_team_slot_matches:
 * @self: a #GowlBroadcastTeam
 * @index: a slot index
 * @fields: (element-type utf8 utf8): one window's facts, keyed by the
 *   names in gowl_broadcast_slot_fields()
 *
 * Returns: %TRUE when the window may fill the slot; always %FALSE for a
 *   slot without a match (those are filled by hand)
 */
gboolean
gowl_broadcast_team_slot_matches(
	GowlBroadcastTeam *self,
	guint              index,
	GHashTable        *fields
){
	Slot *s = slot_at(self, index);

	if (s == NULL || s->filter == NULL)
		return FALSE;
	return gowl_filter_eval(s->filter, fields);
}

/**
 * gowl_broadcast_team_get_slot_focus_key:
 * @self: a #GowlBroadcastTeam
 * @index: a slot index
 *
 * Returns: (transfer none) (nullable): the slot's focus keybind text
 */
const gchar *
gowl_broadcast_team_get_slot_focus_key(
	GowlBroadcastTeam *self,
	guint              index
){
	Slot *s = slot_at(self, index);

	return s != NULL ? s->focus_key : NULL;
}

/**
 * gowl_broadcast_team_get_slot_focus_bind:
 * @self: a #GowlBroadcastTeam
 * @index: a slot index
 * @out_modifiers: (out): #GowlKeyMod bits of the focus keybind
 * @out_keysym: (out): its keysym
 *
 * Returns: %TRUE when the slot has a focus keybind
 */
gboolean
gowl_broadcast_team_get_slot_focus_bind(
	GowlBroadcastTeam *self,
	guint              index,
	guint32           *out_modifiers,
	guint32           *out_keysym
){
	Slot *s = slot_at(self, index);

	if (s == NULL || s->focus_key == NULL)
		return FALSE;
	*out_modifiers = s->focus_mods;
	*out_keysym = s->focus_sym;
	return TRUE;
}

/**
 * gowl_broadcast_team_get_slot_delivery:
 * @self: a #GowlBroadcastTeam
 * @index: a slot index
 *
 * Returns: how keys reach the slot's window by default
 */
GowlKeyDelivery
gowl_broadcast_team_get_slot_delivery(
	GowlBroadcastTeam *self,
	guint              index
){
	Slot *s = slot_at(self, index);

	return s != NULL ? s->delivery : GOWL_KEY_DELIVERY_AUTO;
}

/**
 * gowl_broadcast_team_add_group:
 * @self: a #GowlBroadcastTeam
 * @name: the group's name
 * @slots: (array zero-terminated=1): slot names (or 1-based numbers),
 *   each of which must already be a slot of the team
 * @error: return location for a #GError
 *
 * A named subset of slots, for targets like `group:casters'.  Replaces
 * a group of the same name.
 *
 * Returns: %TRUE when added
 */
gboolean
gowl_broadcast_team_add_group(
	GowlBroadcastTeam   *self,
	const gchar         *name,
	const gchar * const *slots,
	GError             **error
){
	GPtrArray *names;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	if (!name_ok(name, "a group", error))
		return FALSE;
	names = g_ptr_array_new();
	for (i = 0; slots != NULL && slots[i] != NULL; i++) {
		gint idx = gowl_broadcast_team_find_slot(self, slots[i]);

		if (idx < 0) {
			g_set_error(error, GOWL_BROADCAST_ERROR,
			            GOWL_BROADCAST_ERROR_INVALID,
			            "group `%s' names `%s', which is not a slot of "
			            "team `%s'", name, slots[i], self->name);
			g_ptr_array_unref(names);
			return FALSE;
		}
		g_ptr_array_add(names, g_strdup(
			gowl_broadcast_team_get_slot_name(self, (guint)idx)));
	}
	g_ptr_array_add(names, NULL);
	if (!g_hash_table_contains(self->groups, name))
		g_ptr_array_add(self->group_order, g_strdup(name));
	g_hash_table_replace(self->groups, g_strdup(name),
	                     g_ptr_array_free(names, FALSE));
	return TRUE;
}

/**
 * gowl_broadcast_team_get_group:
 * @self: a #GowlBroadcastTeam
 * @name: a group name
 *
 * Returns: (transfer none) (nullable) (array zero-terminated=1): the
 *   group's slot names, or %NULL for no such group
 */
const gchar * const *
gowl_broadcast_team_get_group(
	GowlBroadcastTeam *self,
	const gchar       *name
){
	g_return_val_if_fail(self != NULL, NULL);
	return name != NULL ? g_hash_table_lookup(self->groups, name) : NULL;
}

/**
 * gowl_broadcast_team_get_group_names:
 * @self: a #GowlBroadcastTeam
 *
 * Returns: (transfer full) (array zero-terminated=1): every group name,
 *   in the order they were added
 */
gchar **
gowl_broadcast_team_get_group_names(GowlBroadcastTeam *self)
{
	GPtrArray *out;
	guint i;

	g_return_val_if_fail(self != NULL, NULL);
	out = g_ptr_array_new();
	for (i = 0; i < self->group_order->len; i++)
		g_ptr_array_add(out, g_strdup(
			g_ptr_array_index(self->group_order, i)));
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

/**
 * gowl_broadcast_team_to_yaml:
 * @self: a #GowlBroadcastTeam
 *
 * One flow-style YAML mapping that gowl_broadcast_team_new_from_yaml()
 * reads back to an equal team.
 *
 * Returns: (transfer full): the YAML text
 */
gchar *
gowl_broadcast_team_to_yaml(GowlBroadcastTeam *self)
{
	GString *out;
	guint i;

	g_return_val_if_fail(self != NULL, NULL);

	out = g_string_new("{name: ");
	append_quoted(out, self->name);
	g_string_append(out, ", slots: [");
	for (i = 0; i < self->slots->len; i++) {
		Slot *s = g_ptr_array_index(self->slots, i);

		g_string_append(out, i == 0 ? "{name: " : ", {name: ");
		append_quoted(out, s->name);
		if (s->match != NULL) {
			g_string_append(out, ", match: ");
			append_quoted(out, s->match);
		}
		if (s->focus_key != NULL) {
			g_string_append(out, ", focus-key: ");
			append_quoted(out, s->focus_key);
		}
		if (s->delivery != GOWL_KEY_DELIVERY_AUTO)
			g_string_append_printf(out, ", delivery: %s",
			                       s->delivery == GOWL_KEY_DELIVERY_SEAT
			                       ? "seat" : "activate");
		g_string_append_c(out, '}');
	}
	g_string_append_c(out, ']');
	if (self->group_order->len > 0) {
		g_string_append(out, ", groups: {");
		for (i = 0; i < self->group_order->len; i++) {
			const gchar *g = g_ptr_array_index(self->group_order, i);

			if (i > 0)
				g_string_append(out, ", ");
			append_quoted(out, g);
			g_string_append(out, ": ");
			append_strv(out, g_hash_table_lookup(self->groups, g));
		}
		g_string_append_c(out, '}');
	}
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

/**
 * gowl_broadcast_slot_fields: (skip)
 *
 * Returns: (transfer none) (array zero-terminated=1): the field names a
 *   slot's `match' may test
 */
const gchar * const *
gowl_broadcast_slot_fields(void)
{
	return slot_fields;
}

/* ── Rules ───────────────────────────────────────────────────────────── */

typedef struct {
	GowlBroadcastTargetKind  kind;
	gchar                   *arg;   /* SLOT / GROUP name */
} Target;

typedef struct {
	guint32 keysym;
	guint32 modifiers;
} Translated;

struct _GowlBroadcastRule {
	gint                ref_count;
	gchar              *name;
	gchar              *team;
	gchar              *when;
	GowlFilter         *when_filter;
	gboolean            enabled;

	gchar             **keys_include;  /* as written, for to_yaml */
	gchar             **keys_exclude;
	GHashTable         *include;       /* keysym set */
	GHashTable         *exclude;
	gboolean            include_all;
	gboolean            modifiers_none;

	GArray             *targets;       /* element-type Target */

	/* slot name ("*" for any) -> (from keysym -> Translated*) */
	GHashTable         *translate;
	/* insertion-ordered (slot, from, to) text, for to_yaml */
	GPtrArray          *translate_text;

	GowlBroadcastMode   mode;
	guint               reset_ms;
	GowlKeyDelivery     delivery;
	GowlBroadcastMouse  mouse;
	guint               delay_ms;
	gboolean            local_block;
};

G_DEFINE_BOXED_TYPE(GowlBroadcastRule, gowl_broadcast_rule,
                    gowl_broadcast_rule_ref, gowl_broadcast_rule_unref)

static void
target_clear(gpointer data)
{
	Target *t = data;

	g_clear_pointer(&t->arg, g_free);
}

/**
 * gowl_broadcast_rule_new:
 * @name: the rule's name
 *
 * A rule that broadcasts every key to every other slot of its team --
 * the AutoHotkey default -- until told otherwise.  It needs a team
 * before it does anything.
 *
 * Returns: (transfer full): a new rule
 */
GowlBroadcastRule *
gowl_broadcast_rule_new(const gchar *name)
{
	GowlBroadcastRule *self;
	Target t;

	g_return_val_if_fail(name != NULL, NULL);

	self = g_new0(GowlBroadcastRule, 1);
	self->ref_count = 1;
	self->name = g_strdup(name);
	self->enabled = TRUE;
	self->include = g_hash_table_new(g_direct_hash, g_direct_equal);
	self->exclude = g_hash_table_new(g_direct_hash, g_direct_equal);
	self->include_all = TRUE;
	self->targets = g_array_new(FALSE, TRUE, sizeof(Target));
	g_array_set_clear_func(self->targets, target_clear);
	t.kind = GOWL_BROADCAST_TARGET_ALL_OTHER;
	t.arg = NULL;
	g_array_append_val(self->targets, t);
	self->translate = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                        g_free,
	                                        (GDestroyNotify)g_hash_table_unref);
	self->translate_text = g_ptr_array_new_with_free_func(
		(GDestroyNotify)g_strfreev);
	self->mode = GOWL_BROADCAST_MODE_MIRROR;
	self->delivery = GOWL_KEY_DELIVERY_AUTO;
	self->mouse = GOWL_BROADCAST_MOUSE_OFF;
	return self;
}

/**
 * gowl_broadcast_rule_ref:
 * @self: a #GowlBroadcastRule
 *
 * Returns: (transfer full): @self
 */
GowlBroadcastRule *
gowl_broadcast_rule_ref(GowlBroadcastRule *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	g_atomic_int_inc(&self->ref_count);
	return self;
}

/**
 * gowl_broadcast_rule_unref:
 * @self: (transfer full): a #GowlBroadcastRule
 */
void
gowl_broadcast_rule_unref(GowlBroadcastRule *self)
{
	if (self == NULL)
		return;
	if (!g_atomic_int_dec_and_test(&self->ref_count))
		return;
	g_free(self->name);
	g_free(self->team);
	g_free(self->when);
	g_clear_pointer(&self->when_filter, gowl_filter_free);
	g_strfreev(self->keys_include);
	g_strfreev(self->keys_exclude);
	g_hash_table_unref(self->include);
	g_hash_table_unref(self->exclude);
	g_array_unref(self->targets);
	g_hash_table_unref(self->translate);
	g_ptr_array_unref(self->translate_text);
	g_free(self);
}

/**
 * gowl_broadcast_rule_get_name:
 * @self: a #GowlBroadcastRule
 *
 * Returns: (transfer none): the rule's name
 */
const gchar *
gowl_broadcast_rule_get_name(GowlBroadcastRule *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->name;
}

/**
 * gowl_broadcast_rule_set_team:
 * @self: a #GowlBroadcastRule
 * @team: the name of the team whose windows the rule is about
 */
void
gowl_broadcast_rule_set_team(
	GowlBroadcastRule *self,
	const gchar       *team
){
	g_return_if_fail(self != NULL);
	g_free(self->team);
	self->team = g_strdup(team);
}

/**
 * gowl_broadcast_rule_get_team:
 * @self: a #GowlBroadcastRule
 *
 * Returns: (transfer none) (nullable): the team's name
 */
const gchar *
gowl_broadcast_rule_get_team(GowlBroadcastRule *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->team;
}

/**
 * gowl_broadcast_rule_set_when:
 * @self: a #GowlBroadcastRule
 * @when: (nullable): a gowl-filter expression over
 *   gowl_broadcast_rule_fields(), about the focused window; %NULL or
 *   empty for "whenever a window of the team has focus"
 * @error: return location for a #GError
 *
 * Returns: %TRUE when @when parses (the rule is unchanged otherwise)
 */
gboolean
gowl_broadcast_rule_set_when(
	GowlBroadcastRule  *self,
	const gchar        *when,
	GError            **error
){
	g_autoptr(GowlFilter) filter = NULL;

	g_return_val_if_fail(self != NULL, FALSE);

	if (when != NULL && *when != '\0') {
		g_autoptr(GError) ferr = NULL;

		filter = gowl_filter_parse(when, rule_fields, &ferr);
		if (filter == NULL) {
			g_set_error(error, GOWL_BROADCAST_ERROR,
			            GOWL_BROADCAST_ERROR_FILTER,
			            "rule `%s' when: %s", self->name, ferr->message);
			return FALSE;
		}
	}
	g_free(self->when);
	self->when = filter != NULL ? g_strdup(when) : NULL;
	g_clear_pointer(&self->when_filter, gowl_filter_free);
	self->when_filter = g_steal_pointer(&filter);
	return TRUE;
}

/**
 * gowl_broadcast_rule_get_when:
 * @self: a #GowlBroadcastRule
 *
 * Returns: (transfer none) (nullable): the rule's condition text
 */
const gchar *
gowl_broadcast_rule_get_when(GowlBroadcastRule *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->when;
}

/**
 * gowl_broadcast_rule_when_holds:
 * @self: a #GowlBroadcastRule
 * @fields: (element-type utf8 utf8): the focused window's facts
 *
 * Returns: %TRUE when the rule's condition holds (always, without one)
 */
gboolean
gowl_broadcast_rule_when_holds(
	GowlBroadcastRule *self,
	GHashTable        *fields
){
	g_return_val_if_fail(self != NULL, FALSE);
	return self->when_filter == NULL
	       || gowl_filter_eval(self->when_filter, fields);
}

/**
 * gowl_broadcast_rule_set_enabled:
 * @self: a #GowlBroadcastRule
 * @enabled: whether the rule is in force
 */
void
gowl_broadcast_rule_set_enabled(
	GowlBroadcastRule *self,
	gboolean           enabled
){
	g_return_if_fail(self != NULL);
	self->enabled = enabled;
}

/**
 * gowl_broadcast_rule_get_enabled:
 * @self: a #GowlBroadcastRule
 *
 * Returns: whether the rule is in force
 */
gboolean
gowl_broadcast_rule_get_enabled(GowlBroadcastRule *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	return self->enabled;
}

/**
 * gowl_broadcast_rule_set_keys:
 * @self: a #GowlBroadcastRule
 * @include: (nullable) (array zero-terminated=1): key names or ranges
 *   the rule repeats; %NULL or `all' for every key, an empty list for
 *   none (a rule that only repeats clicks)
 * @exclude: (nullable) (array zero-terminated=1): key names or ranges
 *   never repeated, even when @include names them
 * @error: return location for a #GError
 *
 * Modifier keys are never repeated on their own whatever these say;
 * see gowl_broadcast_keysym_is_modifier().
 *
 * Returns: %TRUE when every name parses (the rule is unchanged
 *   otherwise)
 */
gboolean
gowl_broadcast_rule_set_keys(
	GowlBroadcastRule   *self,
	const gchar * const *include,
	const gchar * const *exclude,
	GError             **error
){
	g_autoptr(GHashTable) inc = NULL;
	g_autoptr(GHashTable) exc = NULL;
	gboolean all;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	inc = g_hash_table_new(g_direct_hash, g_direct_equal);
	exc = g_hash_table_new(g_direct_hash, g_direct_equal);
	/* NULL is every key; an EMPTY list is none (a clicks-only rule) */
	all = include == NULL;
	for (i = 0; include != NULL && include[i] != NULL; i++)
		if (!gowl_broadcast_parse_key_set(include[i], inc, &all, error))
			return FALSE;
	for (i = 0; exclude != NULL && exclude[i] != NULL; i++)
		if (!gowl_broadcast_parse_key_set(exclude[i], exc, NULL, error))
			return FALSE;

	g_strfreev(self->keys_include);
	g_strfreev(self->keys_exclude);
	self->keys_include = g_strdupv((gchar **)include);
	self->keys_exclude = g_strdupv((gchar **)exclude);
	g_hash_table_unref(self->include);
	g_hash_table_unref(self->exclude);
	self->include = g_steal_pointer(&inc);
	self->exclude = g_steal_pointer(&exc);
	self->include_all = all;
	return TRUE;
}

/**
 * gowl_broadcast_rule_get_keys_include:
 * @self: a #GowlBroadcastRule
 *
 * Returns: (transfer none) (nullable) (array zero-terminated=1): the
 *   included key names, as given
 */
const gchar * const *
gowl_broadcast_rule_get_keys_include(GowlBroadcastRule *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return (const gchar * const *)self->keys_include;
}

/**
 * gowl_broadcast_rule_get_keys_exclude:
 * @self: a #GowlBroadcastRule
 *
 * Returns: (transfer none) (nullable) (array zero-terminated=1): the
 *   excluded key names, as given
 */
const gchar * const *
gowl_broadcast_rule_get_keys_exclude(GowlBroadcastRule *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return (const gchar * const *)self->keys_exclude;
}

/**
 * gowl_broadcast_rule_matches_key:
 * @self: a #GowlBroadcastRule
 * @keysym: the key's symbol with modifiers applied
 * @base_keysym: the key's level-0 symbol
 *
 * A key is named by either symbol, case folded: `1' matches the 1 key
 * with or without Shift, `exclam' only with it.
 *
 * Returns: %TRUE when the rule repeats the key
 */
gboolean
gowl_broadcast_rule_matches_key(
	GowlBroadcastRule *self,
	guint32            keysym,
	guint32            base_keysym
){
	gpointer a;
	gpointer b;

	g_return_val_if_fail(self != NULL, FALSE);

	if (gowl_broadcast_keysym_is_modifier(keysym)
	    || gowl_broadcast_keysym_is_modifier(base_keysym))
		return FALSE;
	a = GUINT_TO_POINTER((guint32)xkb_keysym_to_lower(keysym));
	b = GUINT_TO_POINTER((guint32)xkb_keysym_to_lower(base_keysym));
	if (g_hash_table_contains(self->exclude, a)
	    || g_hash_table_contains(self->exclude, b))
		return FALSE;
	return self->include_all
	       || g_hash_table_contains(self->include, a)
	       || g_hash_table_contains(self->include, b);
}

/**
 * gowl_broadcast_rule_set_modifiers_none:
 * @self: a #GowlBroadcastRule
 * @none: %TRUE to repeat a key only when no modifier is held (so Ctrl+1
 *   stays local while 1 is broadcast)
 */
void
gowl_broadcast_rule_set_modifiers_none(
	GowlBroadcastRule *self,
	gboolean           none
){
	g_return_if_fail(self != NULL);
	self->modifiers_none = none;
}

/**
 * gowl_broadcast_rule_get_modifiers_none:
 * @self: a #GowlBroadcastRule
 *
 * Returns: whether the rule ignores keys typed with a modifier held
 */
gboolean
gowl_broadcast_rule_get_modifiers_none(GowlBroadcastRule *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	return self->modifiers_none;
}

/* "all-other" / "slot:healer" to a Target */
static gboolean
parse_target(
	const gchar  *text,
	Target       *out,
	GError      **error
){
	static const struct {
		const gchar             *name;
		GowlBroadcastTargetKind  kind;
	} plain[] = {
		{ "all",         GOWL_BROADCAST_TARGET_ALL },
		{ "all-other",   GOWL_BROADCAST_TARGET_ALL_OTHER },
		{ "others",      GOWL_BROADCAST_TARGET_ALL_OTHER },
		{ "current",     GOWL_BROADCAST_TARGET_CURRENT },
		{ "under-mouse", GOWL_BROADCAST_TARGET_UNDER_MOUSE },
	};
	guint i;

	out->arg = NULL;
	for (i = 0; i < G_N_ELEMENTS(plain); i++) {
		if (g_ascii_strcasecmp(text, plain[i].name) == 0) {
			out->kind = plain[i].kind;
			return TRUE;
		}
	}
	if (g_str_has_prefix(text, "slot:") && text[5] != '\0') {
		out->kind = GOWL_BROADCAST_TARGET_SLOT;
		out->arg = g_strdup(text + 5);
		return TRUE;
	}
	if (g_str_has_prefix(text, "group:") && text[6] != '\0') {
		out->kind = GOWL_BROADCAST_TARGET_GROUP;
		out->arg = g_strdup(text + 6);
		return TRUE;
	}
	g_set_error(error, GOWL_BROADCAST_ERROR, GOWL_BROADCAST_ERROR_INVALID,
	            "`%s' is no target (all, all-other, current, under-mouse, "
	            "slot:NAME, group:NAME)", text);
	return FALSE;
}

/**
 * gowl_broadcast_rule_set_targets:
 * @self: a #GowlBroadcastRule
 * @targets: (array zero-terminated=1): target names: `all',
 *   `all-other', `current', `under-mouse', `slot:NAME', `group:NAME'
 * @error: return location for a #GError
 *
 * A window named by more than one target gets the key once.
 *
 * Returns: %TRUE when every target parses (the rule is unchanged
 *   otherwise)
 */
gboolean
gowl_broadcast_rule_set_targets(
	GowlBroadcastRule   *self,
	const gchar * const *targets,
	GError             **error
){
	g_autoptr(GArray) parsed = NULL;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	if (targets == NULL || targets[0] == NULL) {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_INVALID,
		            "rule `%s' needs at least one target", self->name);
		return FALSE;
	}
	parsed = g_array_new(FALSE, TRUE, sizeof(Target));
	g_array_set_clear_func(parsed, target_clear);
	for (i = 0; targets[i] != NULL; i++) {
		Target t;

		if (!parse_target(targets[i], &t, error))
			return FALSE;
		g_array_append_val(parsed, t);
	}
	g_array_unref(self->targets);
	self->targets = g_steal_pointer(&parsed);
	return TRUE;
}

/**
 * gowl_broadcast_rule_get_n_targets:
 * @self: a #GowlBroadcastRule
 *
 * Returns: how many targets the rule names
 */
guint
gowl_broadcast_rule_get_n_targets(GowlBroadcastRule *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->targets->len;
}

/**
 * gowl_broadcast_rule_get_target_kind:
 * @self: a #GowlBroadcastRule
 * @index: a target index
 *
 * Returns: the target's kind
 */
GowlBroadcastTargetKind
gowl_broadcast_rule_get_target_kind(
	GowlBroadcastRule *self,
	guint              index
){
	g_return_val_if_fail(self != NULL, GOWL_BROADCAST_TARGET_ALL_OTHER);
	g_return_val_if_fail(index < self->targets->len,
	                     GOWL_BROADCAST_TARGET_ALL_OTHER);
	return g_array_index(self->targets, Target, index).kind;
}

/**
 * gowl_broadcast_rule_get_target_arg:
 * @self: a #GowlBroadcastRule
 * @index: a target index
 *
 * Returns: (transfer none) (nullable): the slot or group a `slot:' /
 *   `group:' target names
 */
const gchar *
gowl_broadcast_rule_get_target_arg(
	GowlBroadcastRule *self,
	guint              index
){
	g_return_val_if_fail(self != NULL, NULL);
	g_return_val_if_fail(index < self->targets->len, NULL);
	return g_array_index(self->targets, Target, index).arg;
}

/**
 * gowl_broadcast_rule_add_translation:
 * @self: a #GowlBroadcastRule
 * @slot: the slot whose window gets the other key, or `*' for every
 *   target without a translation of its own
 * @from: the key typed (one key name)
 * @to: the key sent instead, optionally with modifiers (`Shift+2')
 * @error: return location for a #GError
 *
 * ISBoxer's per-slot keys and AutoHotkey's key translations: `1' typed
 * in the main window can be `2' in the healer's.  A later translation
 * of the same key for the same slot replaces the earlier one.
 *
 * Returns: %TRUE when both keys parse
 */
gboolean
gowl_broadcast_rule_add_translation(
	GowlBroadcastRule  *self,
	const gchar        *slot,
	const gchar        *from,
	const gchar        *to,
	GError            **error
){
	GHashTable *map;
	Translated *t;
	guint32 from_sym;
	guint mods = 0;
	guint sym = 0;
	gchar *text[4];

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(slot != NULL && from != NULL && to != NULL, FALSE);

	if (strcmp(slot, "*") != 0 && !name_ok(slot, "a translation slot",
	                                       error))
		return FALSE;
	if (!keysym_from_name(from, &from_sym)) {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_UNKNOWN_KEY,
		            "translation from `%s': no such key", from);
		return FALSE;
	}
	if (!bind_parse_quiet(to, &mods, &sym)) {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_UNKNOWN_KEY,
		            "translation to `%s': no such key", to);
		return FALSE;
	}

	map = g_hash_table_lookup(self->translate, slot);
	if (map == NULL) {
		map = g_hash_table_new_full(g_direct_hash, g_direct_equal,
		                            NULL, g_free);
		g_hash_table_insert(self->translate, g_strdup(slot), map);
	}
	t = g_new0(Translated, 1);
	t->keysym = (guint32)sym;
	t->modifiers = (guint32)mods;
	g_hash_table_replace(map, GUINT_TO_POINTER(from_sym), t);

	text[0] = g_strdup(slot);
	text[1] = g_strdup(from);
	text[2] = g_strdup(to);
	text[3] = NULL;
	g_ptr_array_add(self->translate_text, g_strdupv(text));
	g_free(text[0]);
	g_free(text[1]);
	g_free(text[2]);
	return TRUE;
}

/**
 * gowl_broadcast_rule_translate:
 * @self: a #GowlBroadcastRule
 * @slot: (nullable): the target's slot name
 * @keysym: the key's symbol with modifiers applied
 * @base_keysym: the key's level-0 symbol
 * @out_keysym: (out): the symbol to send instead
 * @out_modifiers: (out): the #GowlKeyMod bits to send it with
 *
 * The slot's own translation wins over the `*' one.
 *
 * Returns: %TRUE when the key is translated for that slot; %FALSE when
 *   it goes through as typed
 */
gboolean
gowl_broadcast_rule_translate(
	GowlBroadcastRule *self,
	const gchar       *slot,
	guint32            keysym,
	guint32            base_keysym,
	guint32           *out_keysym,
	guint32           *out_modifiers
){
	const gchar *keys[2];
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	keys[0] = slot;
	keys[1] = "*";
	for (i = 0; i < G_N_ELEMENTS(keys); i++) {
		GHashTable *map;
		Translated *t;

		if (keys[i] == NULL)
			continue;
		map = g_hash_table_lookup(self->translate, keys[i]);
		if (map == NULL)
			continue;
		t = g_hash_table_lookup(map, GUINT_TO_POINTER(
			(guint32)xkb_keysym_to_lower(base_keysym)));
		if (t == NULL)
			t = g_hash_table_lookup(map, GUINT_TO_POINTER(
				(guint32)xkb_keysym_to_lower(keysym)));
		if (t != NULL) {
			*out_keysym = t->keysym;
			*out_modifiers = t->modifiers;
			return TRUE;
		}
	}
	return FALSE;
}

/**
 * gowl_broadcast_rule_set_mode:
 * @self: a #GowlBroadcastRule
 * @mode: mirror to every target, or round-robin one press per target
 */
void
gowl_broadcast_rule_set_mode(
	GowlBroadcastRule *self,
	GowlBroadcastMode  mode
){
	g_return_if_fail(self != NULL);
	self->mode = mode;
}

/**
 * gowl_broadcast_rule_get_mode:
 * @self: a #GowlBroadcastRule
 *
 * Returns: the rule's mode
 */
GowlBroadcastMode
gowl_broadcast_rule_get_mode(GowlBroadcastRule *self)
{
	g_return_val_if_fail(self != NULL, GOWL_BROADCAST_MODE_MIRROR);
	return self->mode;
}

/**
 * gowl_broadcast_rule_set_reset_ms:
 * @self: a #GowlBroadcastRule
 * @reset_ms: for round-robin, start again at the first target after this
 *   long without a press; 0 never resets
 */
void
gowl_broadcast_rule_set_reset_ms(
	GowlBroadcastRule *self,
	guint              reset_ms
){
	g_return_if_fail(self != NULL);
	self->reset_ms = reset_ms;
}

/**
 * gowl_broadcast_rule_get_reset_ms:
 * @self: a #GowlBroadcastRule
 *
 * Returns: the round-robin reset delay, 0 for none
 */
guint
gowl_broadcast_rule_get_reset_ms(GowlBroadcastRule *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->reset_ms;
}

/**
 * gowl_broadcast_rule_set_delivery:
 * @self: a #GowlBroadcastRule
 * @delivery: how the rule's keys reach windows; %GOWL_KEY_DELIVERY_AUTO
 *   defers to each slot's own delivery
 */
void
gowl_broadcast_rule_set_delivery(
	GowlBroadcastRule *self,
	GowlKeyDelivery    delivery
){
	g_return_if_fail(self != NULL);
	self->delivery = delivery;
}

/**
 * gowl_broadcast_rule_get_delivery:
 * @self: a #GowlBroadcastRule
 *
 * Returns: the rule's delivery
 */
GowlKeyDelivery
gowl_broadcast_rule_get_delivery(GowlBroadcastRule *self)
{
	g_return_val_if_fail(self != NULL, GOWL_KEY_DELIVERY_AUTO);
	return self->delivery;
}

/**
 * gowl_broadcast_rule_set_mouse:
 * @self: a #GowlBroadcastRule
 * @mouse: whether and where clicks are repeated
 */
void
gowl_broadcast_rule_set_mouse(
	GowlBroadcastRule  *self,
	GowlBroadcastMouse  mouse
){
	g_return_if_fail(self != NULL);
	self->mouse = mouse;
}

/**
 * gowl_broadcast_rule_get_mouse:
 * @self: a #GowlBroadcastRule
 *
 * Returns: the rule's click handling
 */
GowlBroadcastMouse
gowl_broadcast_rule_get_mouse(GowlBroadcastRule *self)
{
	g_return_val_if_fail(self != NULL, GOWL_BROADCAST_MOUSE_OFF);
	return self->mouse;
}

/**
 * gowl_broadcast_rule_set_delay_ms:
 * @self: a #GowlBroadcastRule
 * @delay_ms: a pause between one target's key and the next one's
 *   (ISBoxer's keystroke delay), at most 1000; 0 for none
 */
void
gowl_broadcast_rule_set_delay_ms(
	GowlBroadcastRule *self,
	guint              delay_ms
){
	g_return_if_fail(self != NULL);
	self->delay_ms = MIN(delay_ms, 1000);
}

/**
 * gowl_broadcast_rule_get_delay_ms:
 * @self: a #GowlBroadcastRule
 *
 * Returns: the delay between targets, in milliseconds
 */
guint
gowl_broadcast_rule_get_delay_ms(GowlBroadcastRule *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->delay_ms;
}

/**
 * gowl_broadcast_rule_set_local_block:
 * @self: a #GowlBroadcastRule
 * @block: %TRUE to keep the key FROM the focused window, so it reaches
 *   only the targets
 */
void
gowl_broadcast_rule_set_local_block(
	GowlBroadcastRule *self,
	gboolean           block
){
	g_return_if_fail(self != NULL);
	self->local_block = block;
}

/**
 * gowl_broadcast_rule_get_local_block:
 * @self: a #GowlBroadcastRule
 *
 * Returns: whether the focused window is kept from the key
 */
gboolean
gowl_broadcast_rule_get_local_block(GowlBroadcastRule *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	return self->local_block;
}

/**
 * gowl_broadcast_rule_check_one_to_one:
 * @self: a #GowlBroadcastRule
 * @error: return location for a #GError
 *
 * Strict mode's test.  Some games allow several clients per player but
 * only on the terms "one key press, one action": a press may reach ONE
 * window, which may be another than the one with focus.  So with the
 * focused window counted when the key also reaches it (`local: pass'),
 * a rule passes when a single press -- or click -- can reach at most
 * one window: round-robin, or a single `slot:', `current' or
 * `under-mouse' target, each only with `local: block'.
 *
 * Returns: %TRUE when the rule keeps to one window per press
 */
gboolean
gowl_broadcast_rule_check_one_to_one(
	GowlBroadcastRule  *self,
	GError            **error
){
	guint reach;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	reach = self->local_block ? 0 : 1;
	if (self->mode == GOWL_BROADCAST_MODE_ROUND_ROBIN) {
		reach += 1;
	} else {
		for (i = 0; i < self->targets->len; i++) {
			switch (g_array_index(self->targets, Target, i).kind) {
			case GOWL_BROADCAST_TARGET_ALL:
			case GOWL_BROADCAST_TARGET_ALL_OTHER:
			case GOWL_BROADCAST_TARGET_GROUP:
				reach += 2;   /* any number of windows */
				break;
			case GOWL_BROADCAST_TARGET_CURRENT:
			case GOWL_BROADCAST_TARGET_SLOT:
			case GOWL_BROADCAST_TARGET_UNDER_MOUSE:
				reach += 1;
				break;
			}
		}
	}
	if (reach <= 1)
		return TRUE;
	g_set_error(error, GOWL_BROADCAST_ERROR,
	            GOWL_BROADCAST_ERROR_NOT_ONE_TO_ONE,
	            "rule `%s' can send one press to more than one window; "
	            "strict mode allows one (local: block with round-robin or "
	            "a single slot:/current/under-mouse target)", self->name);
	return FALSE;
}

/**
 * gowl_broadcast_rule_fields: (skip)
 *
 * Returns: (transfer none) (array zero-terminated=1): the field names a
 *   rule's `when' may test
 */
const gchar * const *
gowl_broadcast_rule_fields(void)
{
	return rule_fields;
}

static const gchar *
target_kind_name(GowlBroadcastTargetKind kind)
{
	switch (kind) {
	case GOWL_BROADCAST_TARGET_ALL:         return "all";
	case GOWL_BROADCAST_TARGET_CURRENT:     return "current";
	case GOWL_BROADCAST_TARGET_UNDER_MOUSE: return "under-mouse";
	case GOWL_BROADCAST_TARGET_SLOT:        return "slot";
	case GOWL_BROADCAST_TARGET_GROUP:       return "group";
	case GOWL_BROADCAST_TARGET_ALL_OTHER:
	default:                                return "all-other";
	}
}

static const gchar *
delivery_name(GowlKeyDelivery d)
{
	switch (d) {
	case GOWL_KEY_DELIVERY_SEAT:     return "seat";
	case GOWL_KEY_DELIVERY_ACTIVATE: return "activate";
	case GOWL_KEY_DELIVERY_AUTO:
	default:                         return "auto";
	}
}

/**
 * gowl_broadcast_rule_to_yaml:
 * @self: a #GowlBroadcastRule
 *
 * One flow-style YAML mapping that gowl_broadcast_rule_new_from_yaml()
 * reads back to an equal rule.
 *
 * Returns: (transfer full): the YAML text
 */
gchar *
gowl_broadcast_rule_to_yaml(GowlBroadcastRule *self)
{
	GString *out;
	guint i;

	g_return_val_if_fail(self != NULL, NULL);

	out = g_string_new("{name: ");
	append_quoted(out, self->name);
	if (self->team != NULL) {
		g_string_append(out, ", team: ");
		append_quoted(out, self->team);
	}
	if (self->when != NULL) {
		g_string_append(out, ", when: ");
		append_quoted(out, self->when);
	}
	if (!self->enabled)
		g_string_append(out, ", enabled: false");
	if (self->keys_include != NULL || self->keys_exclude != NULL) {
		g_string_append(out, ", keys: {include: ");
		if (self->keys_include != NULL)
			append_strv(out, (const gchar * const *)self->keys_include);
		else
			g_string_append(out, "[all]");
		g_string_append(out, ", exclude: ");
		append_strv(out, (const gchar * const *)self->keys_exclude);
		g_string_append_c(out, '}');
	}
	if (self->modifiers_none)
		g_string_append(out, ", modifiers: none");
	g_string_append(out, ", targets: [");
	for (i = 0; i < self->targets->len; i++) {
		Target *t = &g_array_index(self->targets, Target, i);
		g_autofree gchar *text = NULL;

		if (t->kind == GOWL_BROADCAST_TARGET_SLOT)
			text = g_strconcat("slot:", t->arg, NULL);
		else if (t->kind == GOWL_BROADCAST_TARGET_GROUP)
			text = g_strconcat("group:", t->arg, NULL);
		else
			text = g_strdup(target_kind_name(t->kind));
		if (i > 0)
			g_string_append(out, ", ");
		append_quoted(out, text);
	}
	g_string_append_c(out, ']');
	if (self->translate_text->len > 0) {
		g_autoptr(GHashTable) seen = NULL;
		g_autoptr(GPtrArray) slots = NULL;

		/* grouped by slot, in first-seen order */
		seen = g_hash_table_new(g_str_hash, g_str_equal);
		slots = g_ptr_array_new();
		for (i = 0; i < self->translate_text->len; i++) {
			gchar **t = g_ptr_array_index(self->translate_text, i);

			if (!g_hash_table_contains(seen, t[0])) {
				g_hash_table_add(seen, t[0]);
				g_ptr_array_add(slots, t[0]);
			}
		}
		g_string_append(out, ", translate: {");
		for (i = 0; i < slots->len; i++) {
			const gchar *slot = g_ptr_array_index(slots, i);
			gboolean first = TRUE;
			guint j;

			if (i > 0)
				g_string_append(out, ", ");
			append_quoted(out, slot);
			g_string_append(out, ": {");
			for (j = 0; j < self->translate_text->len; j++) {
				gchar **t = g_ptr_array_index(self->translate_text, j);

				if (strcmp(t[0], slot) != 0)
					continue;
				if (!first)
					g_string_append(out, ", ");
				append_quoted(out, t[1]);
				g_string_append(out, ": ");
				append_quoted(out, t[2]);
				first = FALSE;
			}
			g_string_append_c(out, '}');
		}
		g_string_append_c(out, '}');
	}
	if (self->mode == GOWL_BROADCAST_MODE_ROUND_ROBIN)
		g_string_append(out, ", mode: round-robin");
	if (self->reset_ms > 0)
		g_string_append_printf(out, ", reset-ms: %u", self->reset_ms);
	if (self->delivery != GOWL_KEY_DELIVERY_AUTO)
		g_string_append_printf(out, ", delivery: %s",
		                       delivery_name(self->delivery));
	if (self->mouse == GOWL_BROADCAST_MOUSE_SAME)
		g_string_append(out, ", mouse: same");
	else if (self->mouse == GOWL_BROADCAST_MOUSE_SCALED)
		g_string_append(out, ", mouse: scaled");
	if (self->delay_ms > 0)
		g_string_append_printf(out, ", delay-ms: %u", self->delay_ms);
	if (self->local_block)
		g_string_append(out, ", local: block");
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

/* ── YAML loading ────────────────────────────────────────────────────── */

static const gchar *
node_scalar(YamlNode *node)
{
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_SCALAR)
		return NULL;
	return yaml_node_get_scalar(node);
}

static gboolean
want_scalar(
	YamlNode     *node,
	const gchar  *what,
	const gchar **out,
	GError      **error
){
	*out = node_scalar(node);
	if (*out == NULL || **out == '\0') {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_INVALID,
		            "`%s' needs a value", what);
		return FALSE;
	}
	return TRUE;
}

static gboolean
parse_bool(
	const gchar  *text,
	const gchar  *what,
	gboolean     *out,
	GError      **error
){
	static const gchar * const yes[] = { "true", "yes", "on", "1", NULL };
	static const gchar * const no[]  = { "false", "no", "off", "0", NULL };
	guint i;

	for (i = 0; yes[i] != NULL; i++) {
		if (g_ascii_strcasecmp(text, yes[i]) == 0) {
			*out = TRUE;
			return TRUE;
		}
		if (g_ascii_strcasecmp(text, no[i]) == 0) {
			*out = FALSE;
			return TRUE;
		}
	}
	g_set_error(error, GOWL_BROADCAST_ERROR, GOWL_BROADCAST_ERROR_INVALID,
	            "`%s' is a yes/no, not `%s'", what, text);
	return FALSE;
}

static gboolean
parse_uint(
	const gchar  *text,
	const gchar  *what,
	guint         max,
	guint        *out,
	GError      **error
){
	guint64 n;

	if (!g_ascii_string_to_unsigned(text, 10, 0, max, &n, NULL)) {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_INVALID,
		            "`%s' wants a number 0..%u, not `%s'", what, max, text);
		return FALSE;
	}
	*out = (guint)n;
	return TRUE;
}

static gboolean
parse_delivery(
	const gchar      *text,
	GowlKeyDelivery  *out,
	GError          **error
){
	if (g_ascii_strcasecmp(text, "auto") == 0)
		*out = GOWL_KEY_DELIVERY_AUTO;
	else if (g_ascii_strcasecmp(text, "seat") == 0)
		*out = GOWL_KEY_DELIVERY_SEAT;
	else if (g_ascii_strcasecmp(text, "activate") == 0)
		*out = GOWL_KEY_DELIVERY_ACTIVATE;
	else {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_INVALID,
		            "delivery is auto, seat or activate, not `%s'", text);
		return FALSE;
	}
	return TRUE;
}

/* A scalar or a sequence of scalars, as a NULL-terminated vector. */
static gchar **
node_strv(
	YamlNode     *node,
	const gchar  *what,
	GError      **error
){
	GPtrArray *out;
	YamlSequence *seq;
	guint i;
	guint n;

	if (node == NULL)
		return NULL;
	out = g_ptr_array_new();
	if (yaml_node_get_node_type(node) == YAML_NODE_SCALAR) {
		g_ptr_array_add(out, g_strdup(yaml_node_get_scalar(node)));
	} else if (yaml_node_get_node_type(node) == YAML_NODE_SEQUENCE) {
		seq = yaml_node_get_sequence(node);
		n = seq != NULL ? yaml_sequence_get_length(seq) : 0;
		for (i = 0; i < n; i++) {
			const gchar *v = node_scalar(yaml_sequence_get_element(seq, i));

			if (v == NULL) {
				g_set_error(error, GOWL_BROADCAST_ERROR,
				            GOWL_BROADCAST_ERROR_INVALID,
				            "`%s' is a list of names", what);
				g_ptr_array_free(out, TRUE);
				return NULL;
			}
			g_ptr_array_add(out, g_strdup(v));
		}
	} else {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_INVALID,
		            "`%s' is a name or a list of names", what);
		g_ptr_array_free(out, TRUE);
		return NULL;
	}
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

/* Refuses a mapping key outside @known: a typo should say so. */
static gboolean
check_keys(
	YamlMapping          *m,
	const gchar * const  *known,
	const gchar          *what,
	GError              **error
){
	guint i;
	guint n;

	n = yaml_mapping_get_size(m);
	for (i = 0; i < n; i++) {
		const gchar *k = yaml_mapping_get_key(m, i);

		if (!g_strv_contains(known, k)) {
			g_autofree gchar *all = g_strjoinv(", ", (gchar **)known);

			g_set_error(error, GOWL_BROADCAST_ERROR,
			            GOWL_BROADCAST_ERROR_INVALID,
			            "%s: unknown field `%s' (known: %s)", what, k, all);
			return FALSE;
		}
	}
	return TRUE;
}

/* One entry of a team's `slots:': a mapping, or a bare name. */
static gboolean
add_slot_from_node(
	GowlBroadcastTeam  *team,
	YamlNode           *node,
	GError            **error
){
	static const gchar * const known[] = {
		"name", "match", "focus-key", "delivery", NULL
	};
	YamlMapping *m;
	const gchar *name;
	const gchar *match;
	const gchar *focus;
	const gchar *v;
	GowlKeyDelivery delivery;

	if (node_scalar(node) != NULL)
		return gowl_broadcast_team_add_slot(team, node_scalar(node), NULL,
		                                    NULL, GOWL_KEY_DELIVERY_AUTO,
		                                    error);
	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING) {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_INVALID,
		            "team `%s': a slot is a name or {name: ..., match: ...}",
		            gowl_broadcast_team_get_name(team));
		return FALSE;
	}
	m = yaml_node_get_mapping(node);
	if (!check_keys(m, known, "slot", error))
		return FALSE;
	if (!want_scalar(yaml_mapping_get_member(m, "name"), "slot name",
	                 &name, error))
		return FALSE;
	match = node_scalar(yaml_mapping_get_member(m, "match"));
	focus = node_scalar(yaml_mapping_get_member(m, "focus-key"));
	delivery = GOWL_KEY_DELIVERY_AUTO;
	v = node_scalar(yaml_mapping_get_member(m, "delivery"));
	if (v != NULL && !parse_delivery(v, &delivery, error))
		return FALSE;
	return gowl_broadcast_team_add_slot(team, name, match, focus, delivery,
	                                    error);
}

/**
 * gowl_broadcast_team_new_from_node: (skip)
 * @node: one entry of `broadcast: teams:'
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (nullable): the team, or %NULL with @error set
 */
GowlBroadcastTeam *
gowl_broadcast_team_new_from_node(
	YamlNode  *node,
	GError   **error
){
	static const gchar * const known[] = { "name", "slots", "groups", NULL };
	g_autoptr(GowlBroadcastTeam) team = NULL;
	YamlMapping *m;
	YamlNode *slots;
	YamlNode *groups;
	const gchar *name;

	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING) {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_INVALID,
		            "a team is a mapping: {name: ..., slots: [...]}");
		return NULL;
	}
	m = yaml_node_get_mapping(node);
	if (!check_keys(m, known, "team", error))
		return NULL;
	if (!want_scalar(yaml_mapping_get_member(m, "name"), "team name", &name,
	                 error))
		return NULL;
	if (!name_ok(name, "a team", error))
		return NULL;
	team = gowl_broadcast_team_new(name);

	slots = yaml_mapping_get_member(m, "slots");
	if (slots == NULL || yaml_node_get_node_type(slots) != YAML_NODE_SEQUENCE) {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_INVALID,
		            "team `%s' needs a `slots:' list", name);
		return NULL;
	} else {
		YamlSequence *seq = yaml_node_get_sequence(slots);
		guint n = seq != NULL ? yaml_sequence_get_length(seq) : 0;
		guint i;

		for (i = 0; i < n; i++)
			if (!add_slot_from_node(team,
			        yaml_sequence_get_element(seq, i), error))
				return NULL;
	}

	groups = yaml_mapping_get_member(m, "groups");
	if (groups != NULL) {
		YamlMapping *gm;
		guint i;
		guint n;

		if (yaml_node_get_node_type(groups) != YAML_NODE_MAPPING) {
			g_set_error(error, GOWL_BROADCAST_ERROR,
			            GOWL_BROADCAST_ERROR_INVALID,
			            "team `%s': groups is {NAME: [slot, ...]}", name);
			return NULL;
		}
		gm = yaml_node_get_mapping(groups);
		n = yaml_mapping_get_size(gm);
		for (i = 0; i < n; i++) {
			g_auto(GStrv) members = NULL;
			const gchar *gname = yaml_mapping_get_key(gm, i);

			members = node_strv(yaml_mapping_get_value(gm, i), gname,
			                    error);
			if (members == NULL && error != NULL && *error != NULL)
				return NULL;
			if (!gowl_broadcast_team_add_group(team, gname,
			        (const gchar * const *)members, error))
				return NULL;
		}
	}
	return (GowlBroadcastTeam *)g_steal_pointer(&team);
}

/* `keys:' -- `all', a list (the include list), or {include, exclude} */
static gboolean
rule_keys_from_node(
	GowlBroadcastRule  *rule,
	YamlNode           *node,
	GError            **error
){
	static const gchar * const known[] = { "include", "exclude", NULL };
	g_auto(GStrv) include = NULL;
	g_auto(GStrv) exclude = NULL;

	if (yaml_node_get_node_type(node) == YAML_NODE_MAPPING) {
		YamlMapping *m = yaml_node_get_mapping(node);

		if (!check_keys(m, known, "keys", error))
			return FALSE;
		include = node_strv(yaml_mapping_get_member(m, "include"),
		                    "include", error);
		if (include == NULL && error != NULL && *error != NULL)
			return FALSE;
		exclude = node_strv(yaml_mapping_get_member(m, "exclude"),
		                    "exclude", error);
		if (exclude == NULL && error != NULL && *error != NULL)
			return FALSE;
	} else {
		include = node_strv(node, "keys", error);
		if (include == NULL)
			return FALSE;
	}
	return gowl_broadcast_rule_set_keys(rule,
		(const gchar * const *)include,
		(const gchar * const *)exclude, error);
}

/* `translate:' -- {SLOT: {FROM: TO, ...}, ...} */
static gboolean
rule_translate_from_node(
	GowlBroadcastRule  *rule,
	YamlNode           *node,
	GError            **error
){
	YamlMapping *m;
	guint i;
	guint n;

	if (yaml_node_get_node_type(node) != YAML_NODE_MAPPING) {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_INVALID,
		            "translate is {SLOT: {FROM: TO}}");
		return FALSE;
	}
	m = yaml_node_get_mapping(node);
	n = yaml_mapping_get_size(m);
	for (i = 0; i < n; i++) {
		const gchar *slot = yaml_mapping_get_key(m, i);
		YamlNode *inner = yaml_mapping_get_value(m, i);
		YamlMapping *im;
		guint j;
		guint k;

		if (inner == NULL
		    || yaml_node_get_node_type(inner) != YAML_NODE_MAPPING) {
			g_set_error(error, GOWL_BROADCAST_ERROR,
			            GOWL_BROADCAST_ERROR_INVALID,
			            "translate `%s' is {FROM: TO}", slot);
			return FALSE;
		}
		im = yaml_node_get_mapping(inner);
		k = yaml_mapping_get_size(im);
		for (j = 0; j < k; j++) {
			const gchar *to;

			if (!want_scalar(yaml_mapping_get_value(im, j), "translate",
			                 &to, error))
				return FALSE;
			if (!gowl_broadcast_rule_add_translation(rule, slot,
			        yaml_mapping_get_key(im, j), to, error))
				return FALSE;
		}
	}
	return TRUE;
}

/**
 * gowl_broadcast_rule_new_from_node: (skip)
 * @node: one entry of `broadcast: rules:'
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (nullable): the rule, or %NULL with @error set
 */
GowlBroadcastRule *
gowl_broadcast_rule_new_from_node(
	YamlNode  *node,
	GError   **error
){
	static const gchar * const known[] = {
		"name", "team", "when", "enabled", "keys", "modifiers",
		"targets", "translate", "mode", "reset-ms", "delivery",
		"mouse", "delay-ms", "local", NULL
	};
	g_autoptr(GowlBroadcastRule) rule = NULL;
	YamlMapping *m;
	YamlNode *v;
	const gchar *name;
	const gchar *s;

	if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING) {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_INVALID,
		            "a rule is a mapping: {name: ..., team: ...}");
		return NULL;
	}
	m = yaml_node_get_mapping(node);
	if (!check_keys(m, known, "rule", error))
		return NULL;
	if (!want_scalar(yaml_mapping_get_member(m, "name"), "rule name", &name,
	                 error))
		return NULL;
	if (!name_ok(name, "a rule", error))
		return NULL;
	rule = gowl_broadcast_rule_new(name);

	if (!want_scalar(yaml_mapping_get_member(m, "team"), "team", &s, error))
		return NULL;
	gowl_broadcast_rule_set_team(rule, s);

	s = node_scalar(yaml_mapping_get_member(m, "when"));
	if (s != NULL && !gowl_broadcast_rule_set_when(rule, s, error))
		return NULL;

	s = node_scalar(yaml_mapping_get_member(m, "enabled"));
	if (s != NULL) {
		gboolean b;

		if (!parse_bool(s, "enabled", &b, error))
			return NULL;
		gowl_broadcast_rule_set_enabled(rule, b);
	}

	v = yaml_mapping_get_member(m, "keys");
	if (v != NULL && !rule_keys_from_node(rule, v, error))
		return NULL;

	s = node_scalar(yaml_mapping_get_member(m, "modifiers"));
	if (s != NULL) {
		if (g_ascii_strcasecmp(s, "none") == 0)
			gowl_broadcast_rule_set_modifiers_none(rule, TRUE);
		else if (g_ascii_strcasecmp(s, "any") != 0) {
			g_set_error(error, GOWL_BROADCAST_ERROR,
			            GOWL_BROADCAST_ERROR_INVALID,
			            "modifiers is any or none, not `%s'", s);
			return NULL;
		}
	}

	v = yaml_mapping_get_member(m, "targets");
	if (v != NULL) {
		g_auto(GStrv) targets = node_strv(v, "targets", error);

		if (targets == NULL)
			return NULL;
		if (!gowl_broadcast_rule_set_targets(rule,
		        (const gchar * const *)targets, error))
			return NULL;
	}

	v = yaml_mapping_get_member(m, "translate");
	if (v != NULL && !rule_translate_from_node(rule, v, error))
		return NULL;

	s = node_scalar(yaml_mapping_get_member(m, "mode"));
	if (s != NULL) {
		if (g_ascii_strcasecmp(s, "mirror") == 0)
			gowl_broadcast_rule_set_mode(rule, GOWL_BROADCAST_MODE_MIRROR);
		else if (g_ascii_strcasecmp(s, "round-robin") == 0)
			gowl_broadcast_rule_set_mode(rule,
			                             GOWL_BROADCAST_MODE_ROUND_ROBIN);
		else {
			g_set_error(error, GOWL_BROADCAST_ERROR,
			            GOWL_BROADCAST_ERROR_INVALID,
			            "mode is mirror or round-robin, not `%s'", s);
			return NULL;
		}
	}

	s = node_scalar(yaml_mapping_get_member(m, "reset-ms"));
	if (s != NULL) {
		guint n;

		if (!parse_uint(s, "reset-ms", 600000, &n, error))
			return NULL;
		gowl_broadcast_rule_set_reset_ms(rule, n);
	}

	s = node_scalar(yaml_mapping_get_member(m, "delivery"));
	if (s != NULL) {
		GowlKeyDelivery d;

		if (!parse_delivery(s, &d, error))
			return NULL;
		gowl_broadcast_rule_set_delivery(rule, d);
	}

	s = node_scalar(yaml_mapping_get_member(m, "mouse"));
	if (s != NULL) {
		if (g_ascii_strcasecmp(s, "off") == 0
		    || g_ascii_strcasecmp(s, "false") == 0)
			gowl_broadcast_rule_set_mouse(rule, GOWL_BROADCAST_MOUSE_OFF);
		else if (g_ascii_strcasecmp(s, "same") == 0)
			gowl_broadcast_rule_set_mouse(rule, GOWL_BROADCAST_MOUSE_SAME);
		else if (g_ascii_strcasecmp(s, "scaled") == 0)
			gowl_broadcast_rule_set_mouse(rule,
			                              GOWL_BROADCAST_MOUSE_SCALED);
		else {
			g_set_error(error, GOWL_BROADCAST_ERROR,
			            GOWL_BROADCAST_ERROR_INVALID,
			            "mouse is off, same or scaled, not `%s'", s);
			return NULL;
		}
	}

	s = node_scalar(yaml_mapping_get_member(m, "delay-ms"));
	if (s != NULL) {
		guint n;

		if (!parse_uint(s, "delay-ms", 1000, &n, error))
			return NULL;
		gowl_broadcast_rule_set_delay_ms(rule, n);
	}

	s = node_scalar(yaml_mapping_get_member(m, "local"));
	if (s != NULL) {
		if (g_ascii_strcasecmp(s, "block") == 0)
			gowl_broadcast_rule_set_local_block(rule, TRUE);
		else if (g_ascii_strcasecmp(s, "pass") != 0) {
			g_set_error(error, GOWL_BROADCAST_ERROR,
			            GOWL_BROADCAST_ERROR_INVALID,
			            "local is pass or block, not `%s'", s);
			return NULL;
		}
	}
	return (GowlBroadcastRule *)g_steal_pointer(&rule);
}

/* Parses @yaml and hands its root to @ctor. */
static gpointer
from_yaml(
	const gchar  *yaml,
	gpointer    (*ctor)(YamlNode *, GError **),
	GError      **error
){
	g_autoptr(YamlParser) parser = NULL;
	YamlNode *root;

	if (yaml == NULL || *yaml == '\0') {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_INVALID, "no text");
		return NULL;
	}
	parser = yaml_parser_new();
	if (!yaml_parser_load_from_data(parser, yaml, -1, error))
		return NULL;
	root = yaml_parser_get_root(parser);
	if (root == NULL) {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_INVALID, "no text");
		return NULL;
	}
	return ctor(root, error);
}

/**
 * gowl_broadcast_team_new_from_yaml:
 * @yaml: one team as YAML (or JSON) text, block or flow style
 * @error: return location for a #GError
 *
 * What `broadcast-add-team' reads, and the shape of an entry of the
 * config's `broadcast: teams:' list.
 *
 * Returns: (transfer full) (nullable): the team, or %NULL with @error set
 */
GowlBroadcastTeam *
gowl_broadcast_team_new_from_yaml(
	const gchar  *yaml,
	GError      **error
){
	return from_yaml(yaml,
		(gpointer (*)(YamlNode *, GError **))gowl_broadcast_team_new_from_node,
		error);
}

/**
 * gowl_broadcast_rule_new_from_yaml:
 * @yaml: one rule as YAML (or JSON) text, block or flow style
 * @error: return location for a #GError
 *
 * What `broadcast-add-rule' reads, and the shape of an entry of the
 * config's `broadcast: rules:' list.
 *
 * Returns: (transfer full) (nullable): the rule, or %NULL with @error set
 */
GowlBroadcastRule *
gowl_broadcast_rule_new_from_yaml(
	const gchar  *yaml,
	GError      **error
){
	return from_yaml(yaml,
		(gpointer (*)(YamlNode *, GError **))gowl_broadcast_rule_new_from_node,
		error);
}
