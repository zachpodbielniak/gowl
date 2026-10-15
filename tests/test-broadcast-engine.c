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
 * test-broadcast-engine.c - Broadcast teams, rules and routing.
 *
 * The data model (src/boxed/gowl-broadcast.c) and the broadcast
 * module's routing engine, which is a pure translation unit included
 * here, so every decision -- which window fills which slot, which keys a
 * rule repeats where, what strict mode refuses -- is checked without a
 * compositor.  Delivery is tests/test-broadcast-delivery.c.
 *
 * The example team is three EverQuest clients under Wine (EverQuest's
 * live servers allow boxing; see docs/broadcast.org for the games that
 * do not).
 */

#include <glib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include "gowl.h"
#include "../modules/broadcast/gowl-broadcast-engine.c"

#define EQ_TEAM \
	"{name: eq, slots: [" \
	"{name: tank, match: 'app-id = eqgame.exe and title ~ Tank', " \
	" focus-key: Super+F1}, " \
	"{name: cleric, match: 'app-id = eqgame.exe and title ~ Cleric', " \
	" focus-key: Super+F2}, " \
	"{name: wizard, match: 'app-id = eqgame.exe and title ~ Wizard'}], " \
	"groups: {casters: [cleric, wizard]}}"

/* Window ids as the compositor would hand them out */
#define ID_TANK    (11)
#define ID_CLERIC  (12)
#define ID_WIZARD  (13)
#define ID_EDITOR  (20)

static const GowlBroadcastWindow windows[] = {
	{ ID_EDITOR, "cmacs",      "notes.org",          100, FALSE, 1, "DP-1",
	  FALSE, FALSE },
	{ ID_TANK,   "eqgame.exe", "EverQuest - Tank",   201, TRUE,  2, "DP-1",
	  FALSE, FALSE },
	{ ID_CLERIC, "eqgame.exe", "EverQuest - Cleric", 202, TRUE,  2, "DP-1",
	  FALSE, FALSE },
	{ ID_WIZARD, "eqgame.exe", "EverQuest - Wizard", 203, TRUE,  2, "DP-2",
	  FALSE, FALSE },
};

static GowlBroadcastTeam *
team_from(const gchar *yaml)
{
	g_autoptr(GError) error = NULL;
	GowlBroadcastTeam *team;

	team = gowl_broadcast_team_new_from_yaml(yaml, &error);
	g_assert_no_error(error);
	g_assert_nonnull(team);
	return team;
}

static GowlBroadcastRule *
rule_from(const gchar *yaml)
{
	g_autoptr(GError) error = NULL;
	GowlBroadcastRule *rule;

	rule = gowl_broadcast_rule_new_from_yaml(yaml, &error);
	g_assert_no_error(error);
	g_assert_nonnull(rule);
	return rule;
}

/* An engine with the EQ team and @rules (YAML, NULL-terminated), bound
 * to the windows above. */
static GowlBroadcastEngine *
engine_with(const gchar * const *rules)
{
	GowlBroadcastEngine *e;
	g_autoptr(GPtrArray) teams = NULL;
	g_autoptr(GPtrArray) rs = NULL;
	guint i;

	teams = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_broadcast_team_unref);
	rs = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_broadcast_rule_unref);
	g_ptr_array_add(teams, team_from(EQ_TEAM));
	for (i = 0; rules != NULL && rules[i] != NULL; i++)
		g_ptr_array_add(rs, rule_from(rules[i]));
	e = gowl_broadcast_engine_new();
	gowl_broadcast_engine_set_config(e, teams, rs);
	gowl_broadcast_engine_assign(e, windows, G_N_ELEMENTS(windows));
	return e;
}

static const GowlBroadcastWindow *
window(guint id)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(windows); i++)
		if (windows[i].id == id)
			return &windows[i];
	g_assert_not_reached();
	return NULL;
}

/* Routes a press of @keysym from @from; the actions' window ids, in
 * order, as "11,12" for an easy compare. */
static gchar *
route_press(
	GowlBroadcastEngine *e,
	guint                from,
	guint32              keysym,
	guint32              mods,
	gint64               now,
	gboolean            *block
){
	g_autoptr(GArray) out = NULL;
	GString *s;
	guint i;

	out = g_array_new(FALSE, TRUE, sizeof(GowlBroadcastAction));
	gowl_broadcast_engine_route_key(e, window(from), 0, keysym,
	                                xkb_keysym_to_lower(keysym), mods, now,
	                                out, block);
	s = g_string_new(NULL);
	for (i = 0; i < out->len; i++)
		g_string_append_printf(s, "%s%u", i > 0 ? "," : "",
		                       g_array_index(out, GowlBroadcastAction,
		                                     i).client_id);
	return g_string_free(s, FALSE);
}

/* --- key names --- */

static void
test_key_sets(void)
{
	g_autoptr(GHashTable) set = NULL;
	g_autoptr(GError) error = NULL;
	gboolean all = FALSE;

	set = g_hash_table_new(g_direct_hash, g_direct_equal);
	g_assert_true(gowl_broadcast_parse_key_set("1-9", set, &all, &error));
	g_assert_cmpuint(g_hash_table_size(set), ==, 9);
	g_assert_true(g_hash_table_contains(set, GUINT_TO_POINTER(XKB_KEY_5)));

	g_hash_table_remove_all(set);
	g_assert_true(gowl_broadcast_parse_key_set("F1-F12", set, &all, &error));
	g_assert_cmpuint(g_hash_table_size(set), ==, 12);
	g_assert_true(g_hash_table_contains(set, GUINT_TO_POINTER(XKB_KEY_F12)));

	g_hash_table_remove_all(set);
	g_assert_true(gowl_broadcast_parse_key_set("KP_0-KP_9", set, &all,
	                                           &error));
	g_assert_cmpuint(g_hash_table_size(set), ==, 10);

	/* letters fold to lower case: a rule names a key */
	g_hash_table_remove_all(set);
	g_assert_true(gowl_broadcast_parse_key_set("A", set, &all, &error));
	g_assert_true(g_hash_table_contains(set, GUINT_TO_POINTER(XKB_KEY_a)));

	/* a lone dash is the minus key, not a range */
	g_hash_table_remove_all(set);
	g_assert_true(gowl_broadcast_parse_key_set("-", set, &all, &error));
	g_assert_true(g_hash_table_contains(set,
	                                    GUINT_TO_POINTER(XKB_KEY_minus)));

	g_assert_false(all);
	g_assert_true(gowl_broadcast_parse_key_set("all", set, &all, &error));
	g_assert_true(all);

	g_assert_false(gowl_broadcast_parse_key_set("NotAKey", set, NULL,
	                                            &error));
	g_assert_error(error, GOWL_BROADCAST_ERROR,
	               GOWL_BROADCAST_ERROR_UNKNOWN_KEY);
	g_clear_error(&error);
	g_assert_false(gowl_broadcast_parse_key_set("9-1", set, NULL, &error));
	g_assert_error(error, GOWL_BROADCAST_ERROR,
	               GOWL_BROADCAST_ERROR_UNKNOWN_KEY);
	g_clear_error(&error);
	g_assert_false(gowl_broadcast_parse_key_set("F1-KP_3", set, NULL,
	                                            &error));
	g_clear_error(&error);

	g_assert_true(gowl_broadcast_keysym_is_modifier(XKB_KEY_Shift_L));
	g_assert_true(gowl_broadcast_keysym_is_modifier(XKB_KEY_Super_R));
	g_assert_true(gowl_broadcast_keysym_is_modifier(
		XKB_KEY_ISO_Level3_Shift));
	g_assert_false(gowl_broadcast_keysym_is_modifier(XKB_KEY_a));
}

/* --- teams --- */

static void
test_team_parse(void)
{
	g_autoptr(GowlBroadcastTeam) team = team_from(EQ_TEAM);
	g_autoptr(GowlBroadcastTeam) again = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *yaml2 = NULL;
	const gchar * const *casters;
	guint32 mods;
	guint32 sym;

	g_assert_cmpstr(gowl_broadcast_team_get_name(team), ==, "eq");
	g_assert_cmpuint(gowl_broadcast_team_get_n_slots(team), ==, 3);
	g_assert_cmpint(gowl_broadcast_team_find_slot(team, "cleric"), ==, 1);
	g_assert_cmpint(gowl_broadcast_team_find_slot(team, "3"), ==, 2);
	g_assert_cmpint(gowl_broadcast_team_find_slot(team, "4"), ==, -1);
	g_assert_true(gowl_broadcast_team_get_slot_focus_bind(team, 0, &mods,
	                                                     &sym));
	g_assert_cmpuint(mods, ==, GOWL_KEY_MOD_LOGO);
	g_assert_cmpuint(sym, ==, XKB_KEY_F1);
	g_assert_false(gowl_broadcast_team_get_slot_focus_bind(team, 2, &mods,
	                                                      &sym));
	casters = gowl_broadcast_team_get_group(team, "casters");
	g_assert_nonnull(casters);
	g_assert_cmpstr(casters[1], ==, "wizard");

	/* round trip */
	yaml = gowl_broadcast_team_to_yaml(team);
	again = team_from(yaml);
	yaml2 = gowl_broadcast_team_to_yaml(again);
	g_assert_cmpstr(yaml, ==, yaml2);

	/* bare slot names, filled only by hand */
	{
		g_autoptr(GowlBroadcastTeam) bare = team_from(
			"{name: t, slots: [one, two]}");

		g_assert_null(gowl_broadcast_team_get_slot_match(bare, 0));
		g_assert_cmpuint(gowl_broadcast_team_get_n_slots(bare), ==, 2);
	}
}

static void
test_team_refusals(void)
{
	static const gchar * const bad[] = {
		/* duplicate slot */
		"{name: t, slots: [{name: a}, {name: a}]}",
		/* a filter that does not parse */
		"{name: t, slots: [{name: a, match: 'app-id ='}]}",
		/* a field slots cannot test */
		"{name: t, slots: [{name: a, match: 'focused-slot = a'}]}",
		/* a group naming no slot */
		"{name: t, slots: [a], groups: {g: [b]}}",
		/* unknown key: a typo */
		"{name: t, slotz: [a]}",
		/* no slots */
		"{name: t}",
		/* bad focus key */
		"{name: t, slots: [{name: a, focus-key: 'Super+NoSuchKey'}]}",
		/* a name with a space */
		"{name: 't x', slots: [a]}",
		NULL
	};
	guint i;

	for (i = 0; bad[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;
		g_autoptr(GowlBroadcastTeam) team = NULL;

		team = gowl_broadcast_team_new_from_yaml(bad[i], &error);
		if (team != NULL)
			g_error("accepted a bad team: %s", bad[i]);
		g_assert_nonnull(error);
	}
}

/* --- rules --- */

static void
test_rule_parse(void)
{
	g_autoptr(GowlBroadcastRule) rule = NULL;
	g_autoptr(GowlBroadcastRule) again = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *yaml2 = NULL;
	guint32 sym;
	guint32 mods;

	rule = rule_from("{name: follow, team: eq, when: 'focused-slot = tank',"
	                 " keys: {include: ['1-9', w, a, s, d], exclude: [5]},"
	                 " modifiers: none, targets: [all-other, group:casters],"
	                 " translate: {cleric: {'1': '2'}, '*': {f: Shift+g}},"
	                 " mode: round-robin, reset-ms: 1500, delivery: activate,"
	                 " mouse: scaled, delay-ms: 25, local: block}");

	g_assert_cmpstr(gowl_broadcast_rule_get_team(rule), ==, "eq");
	g_assert_true(gowl_broadcast_rule_matches_key(rule, XKB_KEY_1,
	                                              XKB_KEY_1));
	g_assert_true(gowl_broadcast_rule_matches_key(rule, XKB_KEY_W,
	                                              XKB_KEY_w));
	g_assert_false(gowl_broadcast_rule_matches_key(rule, XKB_KEY_5,
	                                               XKB_KEY_5));
	g_assert_false(gowl_broadcast_rule_matches_key(rule, XKB_KEY_q,
	                                               XKB_KEY_q));
	g_assert_true(gowl_broadcast_rule_get_modifiers_none(rule));
	g_assert_cmpuint(gowl_broadcast_rule_get_n_targets(rule), ==, 2);
	g_assert_cmpint(gowl_broadcast_rule_get_target_kind(rule, 1), ==,
	                GOWL_BROADCAST_TARGET_GROUP);
	g_assert_cmpstr(gowl_broadcast_rule_get_target_arg(rule, 1), ==,
	                "casters");
	g_assert_cmpint(gowl_broadcast_rule_get_mode(rule), ==,
	                GOWL_BROADCAST_MODE_ROUND_ROBIN);
	g_assert_cmpuint(gowl_broadcast_rule_get_reset_ms(rule), ==, 1500);
	g_assert_cmpint(gowl_broadcast_rule_get_delivery(rule), ==,
	                GOWL_KEY_DELIVERY_ACTIVATE);
	g_assert_cmpint(gowl_broadcast_rule_get_mouse(rule), ==,
	                GOWL_BROADCAST_MOUSE_SCALED);
	g_assert_cmpuint(gowl_broadcast_rule_get_delay_ms(rule), ==, 25);
	g_assert_true(gowl_broadcast_rule_get_local_block(rule));

	/* the slot's own translation, then the `*' one */
	g_assert_true(gowl_broadcast_rule_translate(rule, "cleric", XKB_KEY_1,
	                                            XKB_KEY_1, &sym, &mods));
	g_assert_cmpuint(sym, ==, XKB_KEY_2);
	g_assert_cmpuint(mods, ==, 0);
	g_assert_false(gowl_broadcast_rule_translate(rule, "wizard", XKB_KEY_1,
	                                             XKB_KEY_1, &sym, &mods));
	g_assert_true(gowl_broadcast_rule_translate(rule, "wizard", XKB_KEY_f,
	                                            XKB_KEY_f, &sym, &mods));
	g_assert_cmpuint(sym, ==, XKB_KEY_g);
	g_assert_cmpuint(mods, ==, GOWL_KEY_MOD_SHIFT);

	/* round trip */
	yaml = gowl_broadcast_rule_to_yaml(rule);
	again = rule_from(yaml);
	yaml2 = gowl_broadcast_rule_to_yaml(again);
	g_assert_cmpstr(yaml, ==, yaml2);

	/* defaults: every key, every other slot, mirrored, local passes */
	{
		g_autoptr(GowlBroadcastRule) def = rule_from("{name: d, team: eq}");

		g_assert_true(gowl_broadcast_rule_matches_key(def, XKB_KEY_q,
		                                              XKB_KEY_q));
		g_assert_cmpint(gowl_broadcast_rule_get_target_kind(def, 0), ==,
		                GOWL_BROADCAST_TARGET_ALL_OTHER);
		g_assert_false(gowl_broadcast_rule_get_local_block(def));
		/* an explicit empty list is no keys: a clicks-only rule */
		{
			g_autoptr(GowlBroadcastRule) none = rule_from(
				"{name: n, team: eq, keys: [], mouse: same}");
			g_autofree gchar *y = gowl_broadcast_rule_to_yaml(none);
			g_autoptr(GowlBroadcastRule) back = rule_from(y);

			g_assert_false(gowl_broadcast_rule_matches_key(none,
				XKB_KEY_q, XKB_KEY_q));
			g_assert_false(gowl_broadcast_rule_matches_key(back,
				XKB_KEY_q, XKB_KEY_q));
		}
		/* modifier keys never, whatever the key set says */
		g_assert_false(gowl_broadcast_rule_matches_key(def,
			XKB_KEY_Shift_L, XKB_KEY_Shift_L));
	}
}

static void
test_rule_refusals(void)
{
	static const gchar * const bad[] = {
		"{team: eq}",                                     /* no name */
		"{name: r}",                                      /* no team */
		"{name: r, team: eq, keys: [NoSuchKey]}",
		"{name: r, team: eq, targets: [everyone]}",
		"{name: r, team: eq, when: 'tags ~ ('}",          /* bad regex */
		"{name: r, team: eq, when: 'colour = red'}",      /* unknown field */
		"{name: r, team: eq, mode: sometimes}",
		"{name: r, team: eq, mouse: maybe}",
		"{name: r, team: eq, delay-ms: 5000}",
		"{name: r, team: eq, local: drop}",
		"{name: r, team: eq, modifiers: some}",
		"{name: r, team: eq, translate: {a: {'1': NoSuchKey}}}",
		"{name: r, team: eq, colour: red}",               /* typo */
		NULL
	};
	guint i;

	for (i = 0; bad[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;
		g_autoptr(GowlBroadcastRule) rule = NULL;

		rule = gowl_broadcast_rule_new_from_yaml(bad[i], &error);
		if (rule != NULL)
			g_error("accepted a bad rule: %s", bad[i]);
		g_assert_nonnull(error);
	}
}

static void
test_strict(void)
{
	static const struct {
		const gchar *yaml;
		gboolean     ok;
	} cases[] = {
		/* mirroring reaches every window */
		{ "{name: r, team: eq}", FALSE },
		{ "{name: r, team: eq, local: block}", FALSE },
		/* one other window, but the local one too */
		{ "{name: r, team: eq, targets: slot:cleric}", FALSE },
		/* one window per press */
		{ "{name: r, team: eq, targets: slot:cleric, local: block}", TRUE },
		{ "{name: r, team: eq, mode: round-robin, local: block}", TRUE },
		{ "{name: r, team: eq, targets: under-mouse, local: block}", TRUE },
		/* round-robin, but the local window still gets it */
		{ "{name: r, team: eq, mode: round-robin}", FALSE },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autoptr(GowlBroadcastRule) rule = rule_from(cases[i].yaml);
		g_autoptr(GError) error = NULL;
		gboolean ok;

		ok = gowl_broadcast_rule_check_one_to_one(rule, &error);
		if (ok != cases[i].ok)
			g_error("strict check of %s: expected %s", cases[i].yaml,
			        cases[i].ok ? "pass" : "refusal");
		if (!ok)
			g_assert_error(error, GOWL_BROADCAST_ERROR,
			               GOWL_BROADCAST_ERROR_NOT_ONE_TO_ONE);
	}
}

/* --- binding windows --- */

static void
test_assign(void)
{
	GowlBroadcastEngine *e = engine_with(NULL);
	const gchar *team;
	const gchar *slot;
	g_autoptr(GError) error = NULL;

	g_assert_cmpuint(gowl_broadcast_engine_client_for(e, "eq", 0), ==,
	                 ID_TANK);
	g_assert_cmpuint(gowl_broadcast_engine_client_for(e, "eq", 1), ==,
	                 ID_CLERIC);
	g_assert_cmpuint(gowl_broadcast_engine_client_for(e, "eq", 2), ==,
	                 ID_WIZARD);
	g_assert_cmpuint(gowl_broadcast_engine_count_bound(e, "eq"), ==, 3);
	g_assert_true(gowl_broadcast_engine_slot_of(e, ID_CLERIC, &team, &slot));
	g_assert_cmpstr(team, ==, "eq");
	g_assert_cmpstr(slot, ==, "cleric");
	g_assert_false(gowl_broadcast_engine_slot_of(e, ID_EDITOR, NULL, NULL));

	/* by hand: the editor as the wizard, until it goes away */
	g_assert_true(gowl_broadcast_engine_set_manual(e, "eq", "wizard",
	                                               ID_EDITOR, &error));
	gowl_broadcast_engine_assign(e, windows, G_N_ELEMENTS(windows));
	g_assert_cmpuint(gowl_broadcast_engine_client_for(e, "eq", 2), ==,
	                 ID_EDITOR);
	gowl_broadcast_engine_assign(e, windows + 1, G_N_ELEMENTS(windows) - 1);
	g_assert_cmpuint(gowl_broadcast_engine_client_for(e, "eq", 2), ==,
	                 ID_WIZARD);

	g_assert_false(gowl_broadcast_engine_set_manual(e, "eq", "bard", 1,
	                                                &error));
	g_clear_error(&error);
	g_assert_false(gowl_broadcast_engine_set_manual(e, "nope", "tank", 1,
	                                                &error));
	g_clear_error(&error);

	/* two slots with one match take two windows of one program */
	{
		GowlBroadcastEngine *e2 = gowl_broadcast_engine_new();
		g_autoptr(GPtrArray) teams = g_ptr_array_new_with_free_func(
			(GDestroyNotify)gowl_broadcast_team_unref);

		g_ptr_array_add(teams, team_from(
			"{name: any, slots: [{name: a, match: 'app-id = eqgame.exe'},"
			" {name: b, match: 'app-id = eqgame.exe'}]}"));
		gowl_broadcast_engine_set_config(e2, teams, NULL);
		gowl_broadcast_engine_assign(e2, windows, G_N_ELEMENTS(windows));
		g_assert_cmpuint(gowl_broadcast_engine_client_for(e2, "any", 0),
		                 ==, ID_TANK);
		g_assert_cmpuint(gowl_broadcast_engine_client_for(e2, "any", 1),
		                 ==, ID_CLERIC);
		gowl_broadcast_engine_free(e2);
	}
	gowl_broadcast_engine_free(e);
}

/* --- routing --- */

static void
test_route_mirror(void)
{
	static const gchar * const rules[] = {
		"{name: follow, team: eq, keys: {include: ['1-9', w], exclude: [9]}}",
		NULL
	};
	GowlBroadcastEngine *e = engine_with(rules);
	gboolean block = TRUE;
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	g_autofree gchar *d = NULL;

	a = route_press(e, ID_TANK, XKB_KEY_1, 0, 0, &block);
	g_assert_cmpstr(a, ==, "12,13");
	g_assert_false(block);
	/* excluded, not included, from outside the team */
	b = route_press(e, ID_TANK, XKB_KEY_9, 0, 0, NULL);
	g_assert_cmpstr(b, ==, "");
	c = route_press(e, ID_TANK, XKB_KEY_q, 0, 0, NULL);
	g_assert_cmpstr(c, ==, "");
	d = route_press(e, ID_EDITOR, XKB_KEY_1, 0, 0, NULL);
	g_assert_cmpstr(d, ==, "");
	gowl_broadcast_engine_free(e);
}

static void
test_route_when_and_modifiers(void)
{
	static const gchar * const rules[] = {
		"{name: lead, team: eq, when: 'focused-slot = tank',"
		" modifiers: none}",
		NULL
	};
	GowlBroadcastEngine *e = engine_with(rules);
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	g_autofree gchar *d = NULL;

	a = route_press(e, ID_TANK, XKB_KEY_1, 0, 0, NULL);
	g_assert_cmpstr(a, ==, "12,13");
	/* only from the tank */
	b = route_press(e, ID_CLERIC, XKB_KEY_1, 0, 0, NULL);
	g_assert_cmpstr(b, ==, "");
	/* Ctrl+1 stays local; Shift is not a modifier for this */
	c = route_press(e, ID_TANK, XKB_KEY_1, GOWL_KEY_MOD_CTRL, 0, NULL);
	g_assert_cmpstr(c, ==, "");
	d = route_press(e, ID_TANK, XKB_KEY_exclam, GOWL_KEY_MOD_SHIFT, 0, NULL);
	g_assert_cmpstr(d, ==, "12,13");
	gowl_broadcast_engine_free(e);
}

static void
test_route_targets(void)
{
	static const gchar * const rules[] = {
		"{name: heal, team: eq, keys: [h], targets: slot:cleric,"
		" local: block}",
		"{name: nuke, team: eq, keys: [n], targets: group:casters}",
		"{name: all, team: eq, keys: [a], targets: [all]}",
		"{name: me, team: eq, keys: [m], targets: current, local: block,"
		" translate: {'*': {m: k}}}",
		NULL
	};
	GowlBroadcastEngine *e = engine_with(rules);
	g_autoptr(GArray) out = NULL;
	gboolean block = FALSE;
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;

	a = route_press(e, ID_TANK, XKB_KEY_h, 0, 0, &block);
	g_assert_cmpstr(a, ==, "12");
	g_assert_true(block);
	b = route_press(e, ID_TANK, XKB_KEY_n, 0, 0, &block);
	g_assert_cmpstr(b, ==, "12,13");
	g_assert_false(block);
	/* `all' includes the focused window itself */
	c = route_press(e, ID_CLERIC, XKB_KEY_a, 0, 0, NULL);
	g_assert_cmpstr(c, ==, "11,12,13");

	/* a key rewrite for the focused window alone */
	out = g_array_new(FALSE, TRUE, sizeof(GowlBroadcastAction));
	gowl_broadcast_engine_route_key(e, window(ID_WIZARD), 0, XKB_KEY_m,
	                                XKB_KEY_m, 0, 0, out, &block);
	g_assert_cmpuint(out->len, ==, 1);
	g_assert_cmpuint(g_array_index(out, GowlBroadcastAction, 0).client_id,
	                 ==, ID_WIZARD);
	g_assert_cmpuint(g_array_index(out, GowlBroadcastAction, 0).keysym, ==,
	                 XKB_KEY_k);
	g_assert_true(block);
	gowl_broadcast_engine_free(e);
}

static void
test_route_round_robin(void)
{
	static const gchar * const rules[] = {
		"{name: steps, team: eq, keys: [r], mode: round-robin,"
		" reset-ms: 1000, local: block, targets: all}",
		NULL
	};
	GowlBroadcastEngine *e = engine_with(rules);
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	g_autofree gchar *d = NULL;
	g_autofree gchar *x = NULL;

	a = route_press(e, ID_TANK, XKB_KEY_r, 0, 100, NULL);
	b = route_press(e, ID_TANK, XKB_KEY_r, 0, 200, NULL);
	c = route_press(e, ID_TANK, XKB_KEY_r, 0, 300, NULL);
	d = route_press(e, ID_TANK, XKB_KEY_r, 0, 400, NULL);
	g_assert_cmpstr(a, ==, "11");
	g_assert_cmpstr(b, ==, "12");
	g_assert_cmpstr(c, ==, "13");
	g_assert_cmpstr(d, ==, "11");
	/* idle past reset-ms: back to the first */
	x = route_press(e, ID_TANK, XKB_KEY_r, 0, 5000, NULL);
	g_assert_cmpstr(x, ==, "11");
	gowl_broadcast_engine_reset_steps(e, NULL);
	gowl_broadcast_engine_free(e);
}

static void
test_route_button(void)
{
	static const gchar * const rules[] = {
		"{name: clicks, team: eq, mouse: scaled, targets: [all]}",
		"{name: keys-only, team: eq}",
		NULL
	};
	GowlBroadcastEngine *e = engine_with(rules);
	g_autoptr(GArray) out = NULL;

	out = g_array_new(FALSE, TRUE, sizeof(GowlBroadcastAction));
	g_assert_true(gowl_broadcast_engine_route_button(e, window(ID_CLERIC),
	                                                 0, out));
	/* every team window but the one clicked */
	g_assert_cmpuint(out->len, ==, 2);
	g_assert_cmpuint(g_array_index(out, GowlBroadcastAction, 0).client_id,
	                 ==, ID_TANK);
	g_assert_cmpuint(g_array_index(out, GowlBroadcastAction, 1).client_id,
	                 ==, ID_WIZARD);
	g_assert_cmpint(g_array_index(out, GowlBroadcastAction, 0).mouse, ==,
	                GOWL_BROADCAST_MOUSE_SCALED);
	gowl_broadcast_engine_free(e);
}

static void
test_focus_keys(void)
{
	GowlBroadcastEngine *e = engine_with(NULL);
	const gchar *team;
	const gchar *slot;
	guint id;

	g_assert_true(gowl_broadcast_engine_focus_key(e, GOWL_KEY_MOD_LOGO,
	                                              XKB_KEY_F2, &team, &slot,
	                                              &id));
	g_assert_cmpstr(slot, ==, "cleric");
	g_assert_cmpuint(id, ==, ID_CLERIC);
	g_assert_false(gowl_broadcast_engine_focus_key(e, 0, XKB_KEY_F2, &team,
	                                               &slot, &id));
	gowl_broadcast_engine_free(e);
}

/* --- the set --- */

static void
test_problems_and_strict(void)
{
	static const gchar * const rules[] = {
		"{name: ok, team: eq}",
		"{name: ghost, team: nobody}",
		"{name: lost, team: eq, targets: slot:bard}",
		"{name: lonely, team: eq, targets: group:melee}",
		"{name: single, team: eq, targets: slot:cleric, local: block}",
		NULL
	};
	GowlBroadcastEngine *e = engine_with(rules);
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;

	g_assert_cmpuint(gowl_broadcast_engine_get_n_rules(e), ==, 5);
	g_assert_cmpuint(gowl_broadcast_engine_get_n_problems(e), ==, 3);
	g_assert_null(gowl_broadcast_engine_rule_problem(e,
		gowl_broadcast_engine_get_rule(e, 0)));
	g_assert_nonnull(strstr(gowl_broadcast_engine_rule_problem(e,
		gowl_broadcast_engine_get_rule(e, 1)), "nobody"));

	/* strict refuses the mirroring rule, keeps the single-slot one */
	gowl_broadcast_engine_set_strict(e, TRUE);
	g_assert_cmpuint(gowl_broadcast_engine_get_n_problems(e), ==, 4);
	g_assert_nonnull(gowl_broadcast_engine_rule_problem(e,
		gowl_broadcast_engine_get_rule(e, 0)));
	a = route_press(e, ID_TANK, XKB_KEY_1, 0, 0, NULL);
	g_assert_cmpstr(a, ==, "12");
	gowl_broadcast_engine_set_strict(e, FALSE);
	b = route_press(e, ID_TANK, XKB_KEY_1, 0, 0, NULL);
	g_assert_cmpstr(b, ==, "12,13");
	gowl_broadcast_engine_free(e);
}

static void
test_runtime_overlay(void)
{
	static const gchar * const rules[] = { "{name: follow, team: eq}", NULL };
	GowlBroadcastEngine *e = engine_with(rules);
	g_autoptr(GowlBroadcastRule) narrow = NULL;
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;

	/* a runtime rule of the same name replaces the config one */
	narrow = rule_from("{name: follow, team: eq, keys: [1]}");
	g_assert_true(gowl_broadcast_engine_add_rule(e, narrow));
	g_assert_cmpuint(gowl_broadcast_engine_get_n_rules(e), ==, 1);
	g_assert_true(gowl_broadcast_engine_rule_is_runtime(e,
		gowl_broadcast_engine_get_rule(e, 0)));
	a = route_press(e, ID_TANK, XKB_KEY_2, 0, 0, NULL);
	g_assert_cmpstr(a, ==, "");

	/* survives a config reload */
	{
		g_autoptr(GPtrArray) teams = g_ptr_array_new_with_free_func(
			(GDestroyNotify)gowl_broadcast_team_unref);
		g_autoptr(GPtrArray) rs = g_ptr_array_new_with_free_func(
			(GDestroyNotify)gowl_broadcast_rule_unref);

		g_ptr_array_add(teams, team_from(EQ_TEAM));
		g_ptr_array_add(rs, rule_from("{name: follow, team: eq}"));
		gowl_broadcast_engine_set_config(e, teams, rs);
		gowl_broadcast_engine_assign(e, windows, G_N_ELEMENTS(windows));
	}
	b = route_press(e, ID_TANK, XKB_KEY_2, 0, 0, NULL);
	g_assert_cmpstr(b, ==, "");

	/* cleared: the config's comes back */
	gowl_broadcast_engine_clear_runtime(e);
	gowl_broadcast_engine_assign(e, windows, G_N_ELEMENTS(windows));
	c = route_press(e, ID_TANK, XKB_KEY_2, 0, 0, NULL);
	g_assert_cmpstr(c, ==, "12,13");

	g_assert_true(gowl_broadcast_engine_set_rule_enabled(e, "follow",
	                                                     FALSE));
	{
		g_autofree gchar *d = route_press(e, ID_TANK, XKB_KEY_2, 0, 0, NULL);

		g_assert_cmpstr(d, ==, "");
	}
	g_assert_true(gowl_broadcast_engine_remove_rule(e, "follow"));
	g_assert_false(gowl_broadcast_engine_remove_rule(e, "follow"));
	gowl_broadcast_engine_free(e);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/broadcast/keys/sets", test_key_sets);
	g_test_add_func("/broadcast/team/parse", test_team_parse);
	g_test_add_func("/broadcast/team/refusals", test_team_refusals);
	g_test_add_func("/broadcast/rule/parse", test_rule_parse);
	g_test_add_func("/broadcast/rule/refusals", test_rule_refusals);
	g_test_add_func("/broadcast/rule/strict", test_strict);
	g_test_add_func("/broadcast/engine/assign", test_assign);
	g_test_add_func("/broadcast/engine/mirror", test_route_mirror);
	g_test_add_func("/broadcast/engine/when-and-modifiers",
	                test_route_when_and_modifiers);
	g_test_add_func("/broadcast/engine/targets", test_route_targets);
	g_test_add_func("/broadcast/engine/round-robin", test_route_round_robin);
	g_test_add_func("/broadcast/engine/button", test_route_button);
	g_test_add_func("/broadcast/engine/focus-keys", test_focus_keys);
	g_test_add_func("/broadcast/engine/problems-and-strict",
	                test_problems_and_strict);
	g_test_add_func("/broadcast/engine/runtime-overlay",
	                test_runtime_overlay);

	return g_test_run();
}
