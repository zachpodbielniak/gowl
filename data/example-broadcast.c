/*
 * gowl - Example C configuration: input broadcasting (multiboxing)
 * GObject Wayland Compositor
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
 *
 * The EverQuest team of example-broadcast.yaml as C config, built two
 * ways: a team through the typed API, and rules both through the setters
 * and from YAML text (the same schema the config file and `gowl-msg
 * broadcast-add-rule' read).  Check the rules of the game you play before
 * broadcasting anything; see docs/broadcast.org.
 *
 * Merge add_broadcast() into ~/.config/gowl/config.c and call it from
 * gowl_config_init(); then `gowl --recompile'.  Teams and rules only take
 * effect with the opt-in module enabled -- in YAML:
 *
 *   modules:
 *     broadcast: { enabled: true }
 */

#include <gowl/gowl.h>

extern GowlConfig *gowl_config;

/* Every EverQuest client's X11 class under Wine */
#define EQ_MATCH "app-id = eqgame.exe"

/*
 * add_eq_team:
 *
 * Three slots matching the same program, so they take the clients in
 * the order they were opened (pin one by hand with `gowl-msg
 * broadcast-assign eq tank' while it has focus), each with a key that
 * focuses it from anywhere, and a group for the two casters.
 *
 * Returns: %TRUE when the team was added
 */
static gboolean
add_eq_team(void)
{
	static const gchar * const casters[] = { "cleric", "wizard", NULL };
	g_autoptr(GowlBroadcastTeam) team = NULL;
	g_autoptr(GError) error = NULL;

	team = gowl_broadcast_team_new("eq");
	if (!gowl_broadcast_team_add_slot(team, "tank", EQ_MATCH, "Super+F1",
	                                  GOWL_KEY_DELIVERY_AUTO, &error)
	    || !gowl_broadcast_team_add_slot(team, "cleric", EQ_MATCH,
	                                     "Super+F2", GOWL_KEY_DELIVERY_AUTO,
	                                     &error)
	    || !gowl_broadcast_team_add_slot(team, "wizard", EQ_MATCH,
	                                     "Super+F3", GOWL_KEY_DELIVERY_AUTO,
	                                     &error)
	    || !gowl_broadcast_team_add_group(team, "casters", casters,
	                                      &error)) {
		g_warning("example-broadcast: team: %s", error->message);
		return FALSE;
	}
	gowl_config_add_broadcast_team(gowl_config, team);
	return TRUE;
}

/*
 * add_follow_rule:
 *
 * The hotbar and movement follow the tank, built with the setters: only
 * while the tank has focus, never with Ctrl/Alt/Super held, never Return
 * or `/' (chat), into every other window of the team.
 *
 * Returns: %TRUE when the rule was added
 */
static gboolean
add_follow_rule(void)
{
	static const gchar * const keys[] = {
		"1-9", "w", "a", "s", "d", "q", "e", "space", NULL
	};
	static const gchar * const not_these[] = { "Return", "slash", NULL };
	static const gchar * const targets[] = { "all-other", NULL };
	g_autoptr(GowlBroadcastRule) rule = NULL;
	g_autoptr(GError) error = NULL;

	rule = gowl_broadcast_rule_new("follow");
	gowl_broadcast_rule_set_team(rule, "eq");
	gowl_broadcast_rule_set_modifiers_none(rule, TRUE);
	if (!gowl_broadcast_rule_set_when(rule, "focused-slot = tank", &error)
	    || !gowl_broadcast_rule_set_keys(rule, keys, not_these, &error)
	    || !gowl_broadcast_rule_set_targets(rule, targets, &error)) {
		g_warning("example-broadcast: follow: %s", error->message);
		return FALSE;
	}
	gowl_config_add_broadcast_rule(gowl_config, rule);
	return TRUE;
}

/*
 * add_rules_from_yaml:
 *
 * The rest from YAML text -- often the shorter way to write a rule with
 * translations.  H heals (the cleric presses 3, the tank nothing); N has
 * each caster press its own 4; F12 cycles one window per press.
 *
 * Returns: %TRUE when every rule was added
 */
static gboolean
add_rules_from_yaml(void)
{
	static const gchar * const rules[] = {
		"{name: heal, team: eq, keys: [h], targets: slot:cleric,"
		" local: block, translate: {cleric: {h: '3'}}}",
		"{name: nuke, team: eq, when: 'focused-slot = tank', keys: [n],"
		" targets: group:casters, local: block,"
		" translate: {'*': {n: '4'}}}",
		"{name: cycle-target, team: eq, keys: [F12], mode: round-robin,"
		" reset-ms: 2000, targets: all, local: block}",
		NULL
	};
	guint i;

	for (i = 0; rules[i] != NULL; i++) {
		g_autoptr(GowlBroadcastRule) rule = NULL;
		g_autoptr(GError) error = NULL;

		rule = gowl_broadcast_rule_new_from_yaml(rules[i], &error);
		if (rule == NULL) {
			g_warning("example-broadcast: %s", error->message);
			return FALSE;
		}
		gowl_config_add_broadcast_rule(gowl_config, rule);
	}
	return TRUE;
}

/*
 * add_broadcast:
 *
 * Returns: %TRUE when the whole example was added
 */
static gboolean
add_broadcast(void)
{
	return add_eq_team() && add_follow_rule() && add_rules_from_yaml();
}

/*
 * gowl_config_init:
 *
 * The C config's entry point.  In your own config.c, call
 * add_broadcast() from the gowl_config_init() you already have.
 *
 * Returns: %TRUE to keep the config
 */
G_MODULE_EXPORT gboolean
gowl_config_init(void)
{
	return add_broadcast();
}
