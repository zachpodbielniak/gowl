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
 * gowl-broadcast-engine.c - Who gets which key.
 *
 * Three jobs:
 *
 *   1. The SET: the config's teams and rules, overlaid by ones added at
 *      runtime (IPC, Elisp) -- a runtime entry replaces a config entry
 *      of the same name and survives a config reload.  Each rule is
 *      checked against the set (does its team exist, its slots, its
 *      groups; in strict mode, is it one press to one window) and a
 *      rule that fails is kept, reported, and never used.
 *
 *   2. BINDING windows to slots, from the facts the module hands in:
 *      a slot assigned by hand keeps its window while that window
 *      exists; the rest are filled in slot order, each by the first
 *      window its `match' accepts that holds no other slot of the team.
 *
 *   3. ROUTING one key press (or click) from the focused window: every
 *      enabled, usable rule of a team the window is in, whose `when'
 *      holds and whose key set has the key, contributes its targets.  A
 *      window named twice gets the key once, from the first rule.
 *
 * Releases are not routed here.  The module remembers where each press
 * went and sends the release to exactly those windows, whatever has
 * changed since -- the same guarantee input remapping makes, for the
 * same reason: a key must never be left held.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-broadcast"

#include "gowl-broadcast-engine.h"

#include <string.h>
#include <xkbcommon/xkbcommon.h>

/* Round-robin position, per rule. */
typedef struct {
	guint  next;
	gint64 last_ms;
} Steps;

struct _GowlBroadcastEngine {
	GPtrArray  *config_teams;   /* element-type GowlBroadcastTeam* */
	GPtrArray  *config_rules;   /* element-type GowlBroadcastRule* */
	GPtrArray  *runtime_teams;
	GPtrArray  *runtime_rules;

	/* What is in force: config overlaid by runtime, by name */
	GPtrArray  *teams;
	GPtrArray  *rules;
	GHashTable *problems;       /* GowlBroadcastRule* -> gchar* reason */
	gboolean    strict;

	/* team name -> GArray<guint> of client ids, one per slot (0: none) */
	GHashTable *bound;
	/* "team\037slot" -> client id, set by hand */
	GHashTable *manual;
	/* rule name -> Steps* */
	GHashTable *steps;
};

static void
teams_unref(gpointer data)
{
	gowl_broadcast_team_unref((GowlBroadcastTeam *)data);
}

static void
rules_unref(gpointer data)
{
	gowl_broadcast_rule_unref((GowlBroadcastRule *)data);
}

/**
 * gowl_broadcast_engine_new: (skip)
 *
 * Returns: (transfer full): an engine with nothing in it
 */
GowlBroadcastEngine *
gowl_broadcast_engine_new(void)
{
	GowlBroadcastEngine *self;

	self = g_new0(GowlBroadcastEngine, 1);
	self->config_teams = g_ptr_array_new_with_free_func(teams_unref);
	self->config_rules = g_ptr_array_new_with_free_func(rules_unref);
	self->runtime_teams = g_ptr_array_new_with_free_func(teams_unref);
	self->runtime_rules = g_ptr_array_new_with_free_func(rules_unref);
	self->teams = g_ptr_array_new_with_free_func(teams_unref);
	self->rules = g_ptr_array_new_with_free_func(rules_unref);
	self->problems = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                       NULL, g_free);
	self->bound = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    (GDestroyNotify)g_array_unref);
	self->manual = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                     NULL);
	self->steps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    g_free);
	return self;
}

/**
 * gowl_broadcast_engine_free: (skip)
 * @self: (nullable): an engine
 */
void
gowl_broadcast_engine_free(GowlBroadcastEngine *self)
{
	if (self == NULL)
		return;
	g_ptr_array_unref(self->config_teams);
	g_ptr_array_unref(self->config_rules);
	g_ptr_array_unref(self->runtime_teams);
	g_ptr_array_unref(self->runtime_rules);
	g_ptr_array_unref(self->teams);
	g_ptr_array_unref(self->rules);
	g_hash_table_unref(self->problems);
	g_hash_table_unref(self->bound);
	g_hash_table_unref(self->manual);
	g_hash_table_unref(self->steps);
	g_free(self);
}

/* ── The set ─────────────────────────────────────────────────────────── */

static const gchar *
team_name(gpointer t)
{
	return gowl_broadcast_team_get_name((GowlBroadcastTeam *)t);
}

static const gchar *
rule_name(gpointer r)
{
	return gowl_broadcast_rule_get_name((GowlBroadcastRule *)r);
}

static gint
index_of_name(
	GPtrArray      *array,
	const gchar    *name,
	const gchar *(*name_of)(gpointer)
){
	guint i;

	for (i = 0; i < array->len; i++)
		if (g_strcmp0(name_of(g_ptr_array_index(array, i)), name) == 0)
			return (gint)i;
	return -1;
}

/* @base with @overlay laid over it by name: an overlay entry replaces
 * the base entry in place, a new one is appended. */
static void
overlay_into(
	GPtrArray      *out,
	GPtrArray      *base,
	GPtrArray      *overlay,
	const gchar *(*name_of)(gpointer),
	gpointer     (*ref)(gpointer)
){
	guint i;

	g_ptr_array_set_size(out, 0);
	for (i = 0; i < base->len; i++) {
		gpointer item = g_ptr_array_index(base, i);
		gint o = index_of_name(overlay, name_of(item), name_of);

		g_ptr_array_add(out, ref(o >= 0 ? g_ptr_array_index(overlay,
		                                                    (guint)o)
		                                : item));
	}
	for (i = 0; i < overlay->len; i++) {
		gpointer item = g_ptr_array_index(overlay, i);

		if (index_of_name(base, name_of(item), name_of) < 0)
			g_ptr_array_add(out, ref(item));
	}
}

/* Whether @rule can run against the set, and if not, why. */
static gchar *
rule_check(
	GowlBroadcastEngine *self,
	GowlBroadcastRule   *rule
){
	g_autoptr(GError) error = NULL;
	GowlBroadcastTeam *team;
	const gchar *tname;
	guint i;

	tname = gowl_broadcast_rule_get_team(rule);
	if (tname == NULL)
		return g_strdup("no team");
	team = gowl_broadcast_engine_find_team(self, tname);
	if (team == NULL)
		return g_strdup_printf("no team named `%s'", tname);

	for (i = 0; i < gowl_broadcast_rule_get_n_targets(rule); i++) {
		const gchar *arg = gowl_broadcast_rule_get_target_arg(rule, i);

		switch (gowl_broadcast_rule_get_target_kind(rule, i)) {
		case GOWL_BROADCAST_TARGET_SLOT:
			if (gowl_broadcast_team_find_slot(team, arg) < 0)
				return g_strdup_printf("team `%s' has no slot `%s'",
				                       tname, arg);
			break;
		case GOWL_BROADCAST_TARGET_GROUP:
			if (gowl_broadcast_team_get_group(team, arg) == NULL)
				return g_strdup_printf("team `%s' has no group `%s'",
				                       tname, arg);
			break;
		default:
			break;
		}
	}

	if (self->strict && !gowl_broadcast_rule_check_one_to_one(rule, &error))
		return g_strdup(error->message);
	return NULL;
}

/* Recomputes what is in force, and each rule's problem. */
static void
rebuild(GowlBroadcastEngine *self)
{
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	guint i;

	overlay_into(self->teams, self->config_teams, self->runtime_teams,
	             team_name, (gpointer (*)(gpointer))gowl_broadcast_team_ref);
	overlay_into(self->rules, self->config_rules, self->runtime_rules,
	             rule_name, (gpointer (*)(gpointer))gowl_broadcast_rule_ref);

	g_hash_table_remove_all(self->problems);
	for (i = 0; i < self->rules->len; i++) {
		GowlBroadcastRule *r = g_ptr_array_index(self->rules, i);
		gchar *why = rule_check(self, r);

		if (why != NULL)
			g_hash_table_insert(self->problems, r, why);
	}

	/* Bindings of teams that are gone, or changed shape, go */
	g_hash_table_iter_init(&iter, self->bound);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		GowlBroadcastTeam *t;

		t = gowl_broadcast_engine_find_team(self, (const gchar *)key);
		if (t == NULL || gowl_broadcast_team_get_n_slots(t)
		                 != ((GArray *)value)->len)
			g_hash_table_iter_remove(&iter);
	}
	/* Step positions of rules that are gone */
	g_hash_table_iter_init(&iter, self->steps);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		if (index_of_name(self->rules, (const gchar *)key, rule_name) < 0)
			g_hash_table_iter_remove(&iter);
}

/**
 * gowl_broadcast_engine_set_config: (skip)
 * @self: an engine
 * @teams: (nullable) (element-type GowlBroadcastTeam): the config's teams
 * @rules: (nullable) (element-type GowlBroadcastRule): the config's rules
 *
 * Replaces the config half of the set.  Runtime entries stay.  Window
 * bindings are kept where the team still has the same slots; call
 * gowl_broadcast_engine_assign() afterwards.
 */
void
gowl_broadcast_engine_set_config(
	GowlBroadcastEngine *self,
	GPtrArray           *teams,
	GPtrArray           *rules
){
	guint i;

	g_return_if_fail(self != NULL);

	g_ptr_array_set_size(self->config_teams, 0);
	g_ptr_array_set_size(self->config_rules, 0);
	for (i = 0; teams != NULL && i < teams->len; i++)
		g_ptr_array_add(self->config_teams,
			gowl_broadcast_team_ref(g_ptr_array_index(teams, i)));
	for (i = 0; rules != NULL && i < rules->len; i++)
		g_ptr_array_add(self->config_rules,
			gowl_broadcast_rule_ref(g_ptr_array_index(rules, i)));
	rebuild(self);
}

/* Adds or replaces @item in @array by name; TRUE when it replaced. */
static gboolean
put_named(
	GPtrArray      *array,
	gpointer        item,
	const gchar *(*name_of)(gpointer),
	gpointer     (*ref)(gpointer),
	void         (*unref)(gpointer)
){
	gint i = index_of_name(array, name_of(item), name_of);

	if (i >= 0) {
		gpointer old = array->pdata[i];

		array->pdata[i] = ref(item);
		unref(old);
		return TRUE;
	}
	g_ptr_array_add(array, ref(item));
	return FALSE;
}

/**
 * gowl_broadcast_engine_add_team: (skip)
 * @self: an engine
 * @team: a team to add for this session
 *
 * Returns: %TRUE when it replaced one of the same name
 */
gboolean
gowl_broadcast_engine_add_team(
	GowlBroadcastEngine *self,
	GowlBroadcastTeam   *team
){
	gboolean replaced;

	g_return_val_if_fail(self != NULL && team != NULL, FALSE);
	replaced = put_named(self->runtime_teams, team, team_name,
	                     (gpointer (*)(gpointer))gowl_broadcast_team_ref,
	                     teams_unref)
	           || index_of_name(self->config_teams,
	                            gowl_broadcast_team_get_name(team),
	                            team_name) >= 0;
	rebuild(self);
	return replaced;
}

/**
 * gowl_broadcast_engine_add_rule: (skip)
 * @self: an engine
 * @rule: a rule to add for this session
 *
 * Returns: %TRUE when it replaced one of the same name
 */
gboolean
gowl_broadcast_engine_add_rule(
	GowlBroadcastEngine *self,
	GowlBroadcastRule   *rule
){
	gboolean replaced;

	g_return_val_if_fail(self != NULL && rule != NULL, FALSE);
	replaced = put_named(self->runtime_rules, rule, rule_name,
	                     (gpointer (*)(gpointer))gowl_broadcast_rule_ref,
	                     rules_unref)
	           || index_of_name(self->config_rules,
	                            gowl_broadcast_rule_get_name(rule),
	                            rule_name) >= 0;
	rebuild(self);
	return replaced;
}

/* Removes @name from the runtime half, or else from the config half
 * (which the next config load brings back). */
static gboolean
remove_named(
	GowlBroadcastEngine *self,
	GPtrArray           *runtime,
	GPtrArray           *config,
	const gchar         *name,
	const gchar      *(*name_of)(gpointer)
){
	gint i;

	i = index_of_name(runtime, name, name_of);
	if (i >= 0) {
		g_ptr_array_remove_index(runtime, (guint)i);
		rebuild(self);
		return TRUE;
	}
	i = index_of_name(config, name, name_of);
	if (i >= 0) {
		g_ptr_array_remove_index(config, (guint)i);
		rebuild(self);
		return TRUE;
	}
	return FALSE;
}

/**
 * gowl_broadcast_engine_remove_team: (skip)
 * @self: an engine
 * @name: a team name
 *
 * Returns: %TRUE when a team by that name was in force
 */
gboolean
gowl_broadcast_engine_remove_team(
	GowlBroadcastEngine *self,
	const gchar         *name
){
	g_return_val_if_fail(self != NULL && name != NULL, FALSE);
	return remove_named(self, self->runtime_teams, self->config_teams,
	                    name, team_name);
}

/**
 * gowl_broadcast_engine_remove_rule: (skip)
 * @self: an engine
 * @name: a rule name
 *
 * Returns: %TRUE when a rule by that name was in force
 */
gboolean
gowl_broadcast_engine_remove_rule(
	GowlBroadcastEngine *self,
	const gchar         *name
){
	g_return_val_if_fail(self != NULL && name != NULL, FALSE);
	return remove_named(self, self->runtime_rules, self->config_rules,
	                    name, rule_name);
}

/**
 * gowl_broadcast_engine_clear_runtime: (skip)
 * @self: an engine
 *
 * Forgets every team and rule added at runtime, and every manual slot
 * assignment.
 */
void
gowl_broadcast_engine_clear_runtime(GowlBroadcastEngine *self)
{
	g_return_if_fail(self != NULL);
	g_ptr_array_set_size(self->runtime_teams, 0);
	g_ptr_array_set_size(self->runtime_rules, 0);
	g_hash_table_remove_all(self->manual);
	rebuild(self);
}

/**
 * gowl_broadcast_engine_set_strict: (skip)
 * @self: an engine
 * @strict: %TRUE to refuse, at run time, any rule that can send one
 *   press to more than one window
 */
void
gowl_broadcast_engine_set_strict(
	GowlBroadcastEngine *self,
	gboolean             strict
){
	g_return_if_fail(self != NULL);
	if (self->strict == strict)
		return;
	self->strict = strict;
	rebuild(self);
}

/**
 * gowl_broadcast_engine_get_strict: (skip)
 * @self: an engine
 *
 * Returns: whether strict one-to-one mode is on
 */
gboolean
gowl_broadcast_engine_get_strict(GowlBroadcastEngine *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	return self->strict;
}

/**
 * gowl_broadcast_engine_set_rule_enabled: (skip)
 * @self: an engine
 * @name: a rule name
 * @enabled: whether it is in force
 *
 * Returns: %TRUE when a rule by that name exists
 */
gboolean
gowl_broadcast_engine_set_rule_enabled(
	GowlBroadcastEngine *self,
	const gchar         *name,
	gboolean             enabled
){
	gint i;

	g_return_val_if_fail(self != NULL && name != NULL, FALSE);
	i = index_of_name(self->rules, name, rule_name);
	if (i < 0)
		return FALSE;
	gowl_broadcast_rule_set_enabled(
		g_ptr_array_index(self->rules, (guint)i), enabled);
	return TRUE;
}

/**
 * gowl_broadcast_engine_get_n_teams: (skip)
 * @self: an engine
 *
 * Returns: how many teams are in force
 */
guint
gowl_broadcast_engine_get_n_teams(GowlBroadcastEngine *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->teams->len;
}

/**
 * gowl_broadcast_engine_get_team: (skip)
 * @self: an engine
 * @index: a team index
 *
 * Returns: (transfer none): the team
 */
GowlBroadcastTeam *
gowl_broadcast_engine_get_team(
	GowlBroadcastEngine *self,
	guint                index
){
	g_return_val_if_fail(self != NULL && index < self->teams->len, NULL);
	return g_ptr_array_index(self->teams, index);
}

/**
 * gowl_broadcast_engine_find_team: (skip)
 * @self: an engine
 * @name: a team name
 *
 * Returns: (transfer none) (nullable): the team in force by that name
 */
GowlBroadcastTeam *
gowl_broadcast_engine_find_team(
	GowlBroadcastEngine *self,
	const gchar         *name
){
	gint i;

	g_return_val_if_fail(self != NULL, NULL);
	i = index_of_name(self->teams, name, team_name);
	return i >= 0 ? g_ptr_array_index(self->teams, (guint)i) : NULL;
}

/**
 * gowl_broadcast_engine_get_n_rules: (skip)
 * @self: an engine
 *
 * Returns: how many rules are in force (usable or not)
 */
guint
gowl_broadcast_engine_get_n_rules(GowlBroadcastEngine *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->rules->len;
}

/**
 * gowl_broadcast_engine_get_rule: (skip)
 * @self: an engine
 * @index: a rule index
 *
 * Returns: (transfer none): the rule
 */
GowlBroadcastRule *
gowl_broadcast_engine_get_rule(
	GowlBroadcastEngine *self,
	guint                index
){
	g_return_val_if_fail(self != NULL && index < self->rules->len, NULL);
	return g_ptr_array_index(self->rules, index);
}

/**
 * gowl_broadcast_engine_rule_is_runtime: (skip)
 * @self: an engine
 * @rule: a rule in force
 *
 * Returns: %TRUE when it was added at runtime rather than by the config
 */
gboolean
gowl_broadcast_engine_rule_is_runtime(
	GowlBroadcastEngine *self,
	GowlBroadcastRule   *rule
){
	g_return_val_if_fail(self != NULL && rule != NULL, FALSE);
	return g_ptr_array_find(self->runtime_rules, rule, NULL);
}

/**
 * gowl_broadcast_engine_team_is_runtime: (skip)
 * @self: an engine
 * @team: a team in force
 *
 * Returns: %TRUE when it was added at runtime rather than by the config
 */
gboolean
gowl_broadcast_engine_team_is_runtime(
	GowlBroadcastEngine *self,
	GowlBroadcastTeam   *team
){
	g_return_val_if_fail(self != NULL && team != NULL, FALSE);
	return g_ptr_array_find(self->runtime_teams, team, NULL);
}

/**
 * gowl_broadcast_engine_rule_problem: (skip)
 * @self: an engine
 * @rule: a rule in force
 *
 * Returns: (transfer none) (nullable): why the rule cannot run, or %NULL
 *   when it can
 */
const gchar *
gowl_broadcast_engine_rule_problem(
	GowlBroadcastEngine *self,
	GowlBroadcastRule   *rule
){
	g_return_val_if_fail(self != NULL, NULL);
	return g_hash_table_lookup(self->problems, rule);
}

/**
 * gowl_broadcast_engine_get_n_problems: (skip)
 * @self: an engine
 *
 * Returns: how many rules in force cannot run
 */
guint
gowl_broadcast_engine_get_n_problems(GowlBroadcastEngine *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return g_hash_table_size(self->problems);
}

/* ── Windows to slots ────────────────────────────────────────────────── */

/**
 * gowl_broadcast_window_fields: (skip)
 * @window: one window's facts
 *
 * Returns: (transfer full) (element-type utf8 utf8): the fields a slot's
 *   `match' (and, with team and focused-slot added, a rule's `when')
 *   tests
 */
GHashTable *
gowl_broadcast_window_fields(const GowlBroadcastWindow *window)
{
	GHashTable *f;
	GString *tags;
	guint i;

	f = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	g_hash_table_insert(f, (gpointer)"app-id",
	                    g_strdup(window->app_id != NULL ? window->app_id : ""));
	g_hash_table_insert(f, (gpointer)"title",
	                    g_strdup(window->title != NULL ? window->title : ""));
	g_hash_table_insert(f, (gpointer)"pid",
	                    g_strdup_printf("%d", window->pid));
	g_hash_table_insert(f, (gpointer)"xwayland",
	                    g_strdup(window->xwayland ? "true" : "false"));
	g_hash_table_insert(f, (gpointer)"floating",
	                    g_strdup(window->floating ? "true" : "false"));
	g_hash_table_insert(f, (gpointer)"fullscreen",
	                    g_strdup(window->fullscreen ? "true" : "false"));
	g_hash_table_insert(f, (gpointer)"monitor",
	                    g_strdup(window->monitor != NULL ? window->monitor : ""));

	/* `tag' is the first tag (1-based), `tags' every one */
	tags = g_string_new(NULL);
	for (i = 0; i < 32; i++) {
		if ((window->tags & (1u << i)) == 0)
			continue;
		if (tags->len == 0)
			g_hash_table_insert(f, (gpointer)"tag",
			                    g_strdup_printf("%u", i + 1));
		g_string_append_printf(tags, "%s%u", tags->len > 0 ? "," : "",
		                       i + 1);
	}
	if (!g_hash_table_contains(f, "tag"))
		g_hash_table_insert(f, (gpointer)"tag", g_strdup(""));
	g_hash_table_insert(f, (gpointer)"tags", g_string_free(tags, FALSE));
	return f;
}

static gchar *
manual_key(
	const gchar *team,
	const gchar *slot
){
	return g_strconcat(team, "\037", slot, NULL);
}

static gboolean
window_present(
	const GowlBroadcastWindow *windows,
	guint                      n,
	guint                      id
){
	guint i;

	for (i = 0; i < n; i++)
		if (windows[i].id == id)
			return TRUE;
	return FALSE;
}

/**
 * gowl_broadcast_engine_assign: (skip)
 * @self: an engine
 * @windows: (array length=n_windows): every mapped window, in the order
 *   slots should be filled from (the compositor's client order)
 * @n_windows: how many
 *
 * Re-binds every team's slots.  A manual assignment holds while its
 * window exists and is dropped when it goes; the rest are filled from
 * the slots' `match'.
 */
void
gowl_broadcast_engine_assign(
	GowlBroadcastEngine       *self,
	const GowlBroadcastWindow *windows,
	guint                      n_windows
){
	g_autoptr(GPtrArray) fields = NULL;
	GHashTableIter iter;
	gpointer value;
	guint t;
	guint w;

	g_return_if_fail(self != NULL);

	/* Hand-made assignments whose window is gone */
	g_hash_table_iter_init(&iter, self->manual);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		if (!window_present(windows, n_windows, GPOINTER_TO_UINT(value)))
			g_hash_table_iter_remove(&iter);

	fields = g_ptr_array_new_with_free_func(
		(GDestroyNotify)g_hash_table_unref);
	for (w = 0; w < n_windows; w++)
		g_ptr_array_add(fields, gowl_broadcast_window_fields(&windows[w]));

	g_hash_table_remove_all(self->bound);
	for (t = 0; t < self->teams->len; t++) {
		GowlBroadcastTeam *team = g_ptr_array_index(self->teams, t);
		const gchar *tname = gowl_broadcast_team_get_name(team);
		guint n = gowl_broadcast_team_get_n_slots(team);
		g_autoptr(GHashTable) taken = NULL;
		GArray *slots;
		guint s;

		slots = g_array_sized_new(FALSE, TRUE, sizeof(guint), n);
		g_array_set_size(slots, n);
		taken = g_hash_table_new(g_direct_hash, g_direct_equal);

		/* Manual first, so a match cannot take a window that is
		 * assigned by hand to a later slot */
		for (s = 0; s < n; s++) {
			g_autofree gchar *k = manual_key(tname,
				gowl_broadcast_team_get_slot_name(team, s));
			gpointer id;

			if (g_hash_table_lookup_extended(self->manual, k, NULL, &id)) {
				g_array_index(slots, guint, s) = GPOINTER_TO_UINT(id);
				g_hash_table_add(taken, id);
			}
		}
		for (s = 0; s < n; s++) {
			if (g_array_index(slots, guint, s) != 0)
				continue;
			for (w = 0; w < n_windows; w++) {
				if (g_hash_table_contains(taken,
				        GUINT_TO_POINTER(windows[w].id)))
					continue;
				if (gowl_broadcast_team_slot_matches(team, s,
				        g_ptr_array_index(fields, w))) {
					g_array_index(slots, guint, s) = windows[w].id;
					g_hash_table_add(taken,
					                 GUINT_TO_POINTER(windows[w].id));
					break;
				}
			}
		}
		g_hash_table_insert(self->bound, g_strdup(tname), slots);
	}
}

/**
 * gowl_broadcast_engine_set_manual: (skip)
 * @self: an engine
 * @team: a team name
 * @slot: a slot name or 1-based number
 * @client_id: the window to put in the slot, 0 to go back to `match'
 * @error: return location for a #GError
 *
 * ISBoxer binds a slot to the client it launched; this is that, by hand
 * (`broadcast-assign').  Takes effect at the next
 * gowl_broadcast_engine_assign().  The window leaves any other slot of
 * the team it was assigned to.
 *
 * Returns: %TRUE when the team and slot exist
 */
gboolean
gowl_broadcast_engine_set_manual(
	GowlBroadcastEngine  *self,
	const gchar          *team,
	const gchar          *slot,
	guint                 client_id,
	GError              **error
){
	GowlBroadcastTeam *t;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	g_autofree gchar *k = NULL;
	g_autofree gchar *prefix = NULL;
	gint s;

	g_return_val_if_fail(self != NULL, FALSE);

	t = gowl_broadcast_engine_find_team(self, team);
	if (t == NULL) {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_INVALID, "no team named `%s'",
		            team != NULL ? team : "");
		return FALSE;
	}
	s = gowl_broadcast_team_find_slot(t, slot);
	if (s < 0) {
		g_set_error(error, GOWL_BROADCAST_ERROR,
		            GOWL_BROADCAST_ERROR_INVALID,
		            "team `%s' has no slot `%s'", team,
		            slot != NULL ? slot : "");
		return FALSE;
	}
	k = manual_key(team, gowl_broadcast_team_get_slot_name(t, (guint)s));
	if (client_id == 0) {
		g_hash_table_remove(self->manual, k);
		return TRUE;
	}

	/* One slot per window in a team */
	prefix = g_strconcat(team, "\037", NULL);
	g_hash_table_iter_init(&iter, self->manual);
	while (g_hash_table_iter_next(&iter, &key, &value))
		if (g_str_has_prefix((const gchar *)key, prefix)
		    && GPOINTER_TO_UINT(value) == client_id)
			g_hash_table_iter_remove(&iter);
	g_hash_table_replace(self->manual, g_steal_pointer(&k),
	                     GUINT_TO_POINTER(client_id));
	return TRUE;
}

/**
 * gowl_broadcast_engine_client_for: (skip)
 * @self: an engine
 * @team: a team name
 * @slot: a slot index
 *
 * Returns: the client id bound to the slot, 0 for none
 */
guint
gowl_broadcast_engine_client_for(
	GowlBroadcastEngine *self,
	const gchar         *team,
	guint                slot
){
	GArray *slots;

	g_return_val_if_fail(self != NULL, 0);
	slots = team != NULL ? g_hash_table_lookup(self->bound, team) : NULL;
	if (slots == NULL || slot >= slots->len)
		return 0;
	return g_array_index(slots, guint, slot);
}

/**
 * gowl_broadcast_engine_slot_of: (skip)
 * @self: an engine
 * @client_id: a window
 * @out_team: (out) (optional) (transfer none): its (first) team
 * @out_slot: (out) (optional) (transfer none): its slot in that team
 *
 * Returns: %TRUE when the window fills a slot of some team
 */
gboolean
gowl_broadcast_engine_slot_of(
	GowlBroadcastEngine  *self,
	guint                 client_id,
	const gchar         **out_team,
	const gchar         **out_slot
){
	guint t;

	g_return_val_if_fail(self != NULL, FALSE);
	if (client_id == 0)
		return FALSE;
	for (t = 0; t < self->teams->len; t++) {
		GowlBroadcastTeam *team = g_ptr_array_index(self->teams, t);
		GArray *slots;
		guint s;

		slots = g_hash_table_lookup(self->bound,
		                            gowl_broadcast_team_get_name(team));
		if (slots == NULL)
			continue;
		for (s = 0; s < slots->len; s++) {
			if (g_array_index(slots, guint, s) != client_id)
				continue;
			if (out_team != NULL)
				*out_team = gowl_broadcast_team_get_name(team);
			if (out_slot != NULL)
				*out_slot = gowl_broadcast_team_get_slot_name(team, s);
			return TRUE;
		}
	}
	return FALSE;
}

/**
 * gowl_broadcast_engine_count_bound: (skip)
 * @self: an engine
 * @team: a team name
 *
 * Returns: how many of the team's slots have a window
 */
guint
gowl_broadcast_engine_count_bound(
	GowlBroadcastEngine *self,
	const gchar         *team
){
	GArray *slots;
	guint n = 0;
	guint s;

	g_return_val_if_fail(self != NULL, 0);
	slots = team != NULL ? g_hash_table_lookup(self->bound, team) : NULL;
	for (s = 0; slots != NULL && s < slots->len; s++)
		if (g_array_index(slots, guint, s) != 0)
			n++;
	return n;
}

/* ── Routing ─────────────────────────────────────────────────────────── */

/* Slot index of @client_id in @team, or -1. */
static gint
slot_index(
	GowlBroadcastEngine *self,
	GowlBroadcastTeam   *team,
	guint                client_id
){
	GArray *slots;
	guint s;

	slots = g_hash_table_lookup(self->bound,
	                            gowl_broadcast_team_get_name(team));
	for (s = 0; slots != NULL && s < slots->len; s++)
		if (g_array_index(slots, guint, s) == client_id)
			return (gint)s;
	return -1;
}

/* Appends slot @s's window to @out (as a slot index) unless it is
 * empty, already there, or the focused window when @skip_focused. */
static void
collect_slot(
	GowlBroadcastEngine *self,
	GowlBroadcastTeam   *team,
	guint                s,
	guint                focused_id,
	gboolean             skip_focused,
	GArray              *out
){
	guint id;
	guint i;

	id = gowl_broadcast_engine_client_for(self,
		gowl_broadcast_team_get_name(team), s);
	if (id == 0 || (skip_focused && id == focused_id))
		return;
	for (i = 0; i < out->len; i++)
		if (g_array_index(out, guint, i) == s)
			return;
	g_array_append_val(out, s);
}

/* The slots a rule's targets name, in order, without repeats. */
static void
rule_slots(
	GowlBroadcastEngine *self,
	GowlBroadcastRule   *rule,
	GowlBroadcastTeam   *team,
	guint                focused_id,
	guint                under_mouse,
	GArray              *out
){
	guint n_slots;
	guint i;
	guint s;

	n_slots = gowl_broadcast_team_get_n_slots(team);
	for (i = 0; i < gowl_broadcast_rule_get_n_targets(rule); i++) {
		const gchar *arg = gowl_broadcast_rule_get_target_arg(rule, i);
		const gchar * const *group;
		gint idx;

		switch (gowl_broadcast_rule_get_target_kind(rule, i)) {
		case GOWL_BROADCAST_TARGET_ALL:
			for (s = 0; s < n_slots; s++)
				collect_slot(self, team, s, focused_id, FALSE, out);
			break;
		case GOWL_BROADCAST_TARGET_ALL_OTHER:
			for (s = 0; s < n_slots; s++)
				collect_slot(self, team, s, focused_id, TRUE, out);
			break;
		case GOWL_BROADCAST_TARGET_CURRENT:
			idx = slot_index(self, team, focused_id);
			if (idx >= 0)
				collect_slot(self, team, (guint)idx, focused_id, FALSE,
				             out);
			break;
		case GOWL_BROADCAST_TARGET_SLOT:
			idx = gowl_broadcast_team_find_slot(team, arg);
			if (idx >= 0)
				collect_slot(self, team, (guint)idx, focused_id, FALSE,
				             out);
			break;
		case GOWL_BROADCAST_TARGET_GROUP:
			group = gowl_broadcast_team_get_group(team, arg);
			for (s = 0; group != NULL && group[s] != NULL; s++) {
				idx = gowl_broadcast_team_find_slot(team, group[s]);
				if (idx >= 0)
					collect_slot(self, team, (guint)idx, focused_id,
					             FALSE, out);
			}
			break;
		case GOWL_BROADCAST_TARGET_UNDER_MOUSE:
			idx = slot_index(self, team, under_mouse);
			if (idx >= 0)
				collect_slot(self, team, (guint)idx, focused_id, FALSE,
				             out);
			break;
		}
	}
}

/* Picks the next slot of a round-robin rule, advancing its position;
 * the position starts over after `reset-ms' without a press. */
static guint
next_step(
	GowlBroadcastEngine *self,
	GowlBroadcastRule   *rule,
	guint                n,
	gint64               now_ms
){
	const gchar *name = gowl_broadcast_rule_get_name(rule);
	Steps *st;
	guint reset;
	guint pick;

	st = g_hash_table_lookup(self->steps, name);
	if (st == NULL) {
		st = g_new0(Steps, 1);
		g_hash_table_insert(self->steps, g_strdup(name), st);
	}
	reset = gowl_broadcast_rule_get_reset_ms(rule);
	if (reset > 0 && st->last_ms > 0 && now_ms - st->last_ms > (gint64)reset)
		st->next = 0;
	st->last_ms = now_ms;
	pick = st->next % n;
	st->next = (pick + 1) % n;
	return pick;
}

/* Is @client_id already an action in @out? */
static gboolean
has_action(
	GArray *out,
	guint   client_id
){
	guint i;

	for (i = 0; i < out->len; i++)
		if (g_array_index(out, GowlBroadcastAction, i).client_id == client_id)
			return TRUE;
	return FALSE;
}

/* The `when' fields: the focused window's, plus its team and slot. */
static GHashTable *
when_fields(
	const GowlBroadcastWindow *focused,
	const gchar               *team,
	const gchar               *slot
){
	GHashTable *f = gowl_broadcast_window_fields(focused);

	g_hash_table_insert(f, (gpointer)"team", g_strdup(team));
	g_hash_table_insert(f, (gpointer)"focused-slot",
	                    g_strdup(slot != NULL ? slot : ""));
	return f;
}

/* Shared by keys and clicks: every usable rule of every team @focused
 * is in, filtered by @want, contributing its windows to @out. */
static gboolean
route(
	GowlBroadcastEngine       *self,
	const GowlBroadcastWindow *focused,
	guint                      under_mouse,
	guint32                    keysym,
	guint32                    base_keysym,
	guint32                    modifiers,
	gboolean                   is_click,
	gint64                     now_ms,
	GArray                    *out,
	gboolean                  *out_local_block
){
	gboolean any = FALSE;
	guint r;

	for (r = 0; r < self->rules->len; r++) {
		GowlBroadcastRule *rule = g_ptr_array_index(self->rules, r);
		g_autoptr(GHashTable) fields = NULL;
		g_autoptr(GArray) slots = NULL;
		GowlBroadcastTeam *team;
		gint self_slot;
		guint i;

		if (!gowl_broadcast_rule_get_enabled(rule)
		    || g_hash_table_contains(self->problems, rule))
			continue;
		team = gowl_broadcast_engine_find_team(self,
			gowl_broadcast_rule_get_team(rule));
		if (team == NULL)
			continue;
		self_slot = slot_index(self, team, focused->id);
		if (self_slot < 0)
			continue;   /* the focused window is not in this team */

		if (is_click) {
			if (gowl_broadcast_rule_get_mouse(rule)
			    == GOWL_BROADCAST_MOUSE_OFF)
				continue;
		} else {
			if (!gowl_broadcast_rule_matches_key(rule, keysym,
			                                     base_keysym))
				continue;
			if (gowl_broadcast_rule_get_modifiers_none(rule)
			    && (modifiers & (GOWL_KEY_MOD_CTRL | GOWL_KEY_MOD_ALT
			                     | GOWL_KEY_MOD_LOGO | GOWL_KEY_MOD_MOD3
			                     | GOWL_KEY_MOD_MOD5)) != 0)
				continue;
		}
		fields = when_fields(focused, gowl_broadcast_team_get_name(team),
			gowl_broadcast_team_get_slot_name(team, (guint)self_slot));
		if (!gowl_broadcast_rule_when_holds(rule, fields))
			continue;

		any = TRUE;
		if (!is_click && out_local_block != NULL
		    && gowl_broadcast_rule_get_local_block(rule))
			*out_local_block = TRUE;

		slots = g_array_new(FALSE, FALSE, sizeof(guint));
		rule_slots(self, rule, team, focused->id, under_mouse, slots);
		if (is_click) {
			/* never click the clicked window again */
			for (i = 0; i < slots->len; ) {
				if (gowl_broadcast_engine_client_for(self,
				        gowl_broadcast_team_get_name(team),
				        g_array_index(slots, guint, i)) == focused->id)
					g_array_remove_index(slots, i);
				else
					i++;
			}
		}
		if (slots->len == 0)
			continue;
		if (gowl_broadcast_rule_get_mode(rule)
		    == GOWL_BROADCAST_MODE_ROUND_ROBIN) {
			guint pick = next_step(self, rule, slots->len, now_ms);
			guint keep = g_array_index(slots, guint, pick);

			g_array_set_size(slots, 1);
			g_array_index(slots, guint, 0) = keep;
		}

		for (i = 0; i < slots->len; i++) {
			GowlBroadcastAction a;
			guint s = g_array_index(slots, guint, i);
			guint32 tsym;
			guint32 tmods;

			memset(&a, 0, sizeof a);
			a.client_id = gowl_broadcast_engine_client_for(self,
				gowl_broadcast_team_get_name(team), s);
			if (a.client_id == 0 || has_action(out, a.client_id))
				continue;
			a.slot = gowl_broadcast_team_get_slot_name(team, s);
			a.modifiers = -1;
			if (!is_click && gowl_broadcast_rule_translate(rule, a.slot,
			        keysym, base_keysym, &tsym, &tmods)) {
				a.keysym = tsym;
				a.modifiers = (gint)tmods;
			}
			a.delivery = gowl_broadcast_rule_get_delivery(rule);
			if (a.delivery == GOWL_KEY_DELIVERY_AUTO)
				a.delivery = gowl_broadcast_team_get_slot_delivery(team,
				                                                    s);
			a.delay_ms = gowl_broadcast_rule_get_delay_ms(rule);
			a.mouse = gowl_broadcast_rule_get_mouse(rule);
			a.rule = rule;
			g_array_append_val(out, a);
		}
	}
	return any;
}

/**
 * gowl_broadcast_engine_route_key: (skip)
 * @self: an engine
 * @focused: the focused window
 * @under_mouse: the client id under the pointer, 0 for none (for
 *   `under-mouse' targets)
 * @keysym: the key's symbol with modifiers applied
 * @base_keysym: the key's level-0 symbol
 * @modifiers: #GowlKeyMod bits held
 * @now_ms: a monotonic time, for round-robin resets
 * @out_actions: (element-type GowlBroadcastAction): receives the
 *   deliveries, appended
 * @out_local_block: (out) (optional): set %TRUE when a matching rule
 *   keeps the key from the focused window
 *
 * Decides where one key PRESS goes.  Advances round-robin positions, so
 * call it once per press.
 *
 * Returns: %TRUE when some rule matched (even if it found no window)
 */
gboolean
gowl_broadcast_engine_route_key(
	GowlBroadcastEngine       *self,
	const GowlBroadcastWindow *focused,
	guint                      under_mouse,
	guint32                    keysym,
	guint32                    base_keysym,
	guint32                    modifiers,
	gint64                     now_ms,
	GArray                    *out_actions,
	gboolean                  *out_local_block
){
	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(out_actions != NULL, FALSE);

	if (out_local_block != NULL)
		*out_local_block = FALSE;
	if (focused == NULL || focused->id == 0)
		return FALSE;
	return route(self, focused, under_mouse, keysym, base_keysym,
	             modifiers, FALSE, now_ms, out_actions, out_local_block);
}

/**
 * gowl_broadcast_engine_route_button: (skip)
 * @self: an engine
 * @clicked: the window the click went to
 * @now_ms: a monotonic time, for round-robin resets
 * @out_actions: (element-type GowlBroadcastAction): receives the
 *   deliveries, appended
 *
 * Decides where one button PRESS goes: rules with `mouse' on, never the
 * clicked window itself.
 *
 * Returns: %TRUE when some rule matched
 */
gboolean
gowl_broadcast_engine_route_button(
	GowlBroadcastEngine       *self,
	const GowlBroadcastWindow *clicked,
	gint64                     now_ms,
	GArray                    *out_actions
){
	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(out_actions != NULL, FALSE);

	if (clicked == NULL || clicked->id == 0)
		return FALSE;
	return route(self, clicked, clicked->id, 0, 0, 0, TRUE, now_ms,
	             out_actions, NULL);
}

/**
 * gowl_broadcast_engine_focus_key: (skip)
 * @self: an engine
 * @modifiers: #GowlKeyMod bits held (lock keys cleaned)
 * @keysym: the key's level-0 symbol
 * @out_team: (out) (transfer none): the team of the slot it focuses
 * @out_slot: (out) (transfer none): the slot
 * @out_client_id: (out): the slot's window, 0 when the slot is empty
 *
 * Returns: %TRUE when the key is some slot's focus keybind
 */
gboolean
gowl_broadcast_engine_focus_key(
	GowlBroadcastEngine  *self,
	guint32               modifiers,
	guint32               keysym,
	const gchar         **out_team,
	const gchar         **out_slot,
	guint                *out_client_id
){
	guint t;

	g_return_val_if_fail(self != NULL, FALSE);

	for (t = 0; t < self->teams->len; t++) {
		GowlBroadcastTeam *team = g_ptr_array_index(self->teams, t);
		guint s;

		for (s = 0; s < gowl_broadcast_team_get_n_slots(team); s++) {
			guint32 bmods;
			guint32 bsym;

			if (!gowl_broadcast_team_get_slot_focus_bind(team, s, &bmods,
			                                            &bsym))
				continue;
			if (bmods != modifiers
			    || xkb_keysym_to_lower(bsym) != xkb_keysym_to_lower(keysym))
				continue;
			*out_team = gowl_broadcast_team_get_name(team);
			*out_slot = gowl_broadcast_team_get_slot_name(team, s);
			*out_client_id = gowl_broadcast_engine_client_for(self,
				*out_team, s);
			return TRUE;
		}
	}
	return FALSE;
}

/**
 * gowl_broadcast_engine_reset_steps: (skip)
 * @self: an engine
 * @rule: (nullable): a rule name, or %NULL for every rule
 *
 * Starts round-robin over at the first target.
 */
void
gowl_broadcast_engine_reset_steps(
	GowlBroadcastEngine *self,
	const gchar         *rule
){
	g_return_if_fail(self != NULL);
	if (rule == NULL)
		g_hash_table_remove_all(self->steps);
	else
		g_hash_table_remove(self->steps, rule);
}
