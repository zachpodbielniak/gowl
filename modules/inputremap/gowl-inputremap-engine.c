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
 * gowl-inputremap-engine.c - see the header.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-inputremap"

#include "gowl-inputremap-engine.h"

struct _GowlInputRemapEngine {
	GPtrArray *config;   /* GowlInputRemapRule*, config order */
	GPtrArray *runtime;  /* GowlInputRemapRule*, insertion order */
};

/* The index of the rule called @name in @rules, or -1. */
static gint
find_rule(
	GPtrArray   *rules,
	const gchar *name
){
	guint i;

	for (i = 0; i < rules->len; i++) {
		if (g_strcmp0(gowl_input_remap_rule_get_name(
			    g_ptr_array_index(rules, i)), name) == 0)
			return (gint)i;
	}
	return -1;
}

/* A config rule a runtime rule of the same name has replaced. */
static gboolean
is_shadowed(
	GowlInputRemapEngine *self,
	GowlInputRemapRule   *rule
){
	return find_rule(self->runtime, gowl_input_remap_rule_get_name(rule)) >= 0;
}

/**
 * gowl_input_remap_engine_new: (skip)
 *
 * Returns: (transfer full): an empty engine
 */
GowlInputRemapEngine *
gowl_input_remap_engine_new(void)
{
	GowlInputRemapEngine *self;

	self = g_new0(GowlInputRemapEngine, 1);
	self->config = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_input_remap_rule_unref);
	self->runtime = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_input_remap_rule_unref);
	return self;
}

/**
 * gowl_input_remap_engine_free: (skip)
 * @self: (nullable): an engine
 */
void
gowl_input_remap_engine_free(GowlInputRemapEngine *self)
{
	if (self == NULL)
		return;
	g_ptr_array_unref(self->config);
	g_ptr_array_unref(self->runtime);
	g_free(self);
}

/**
 * gowl_input_remap_engine_set_config_rules: (skip)
 * @self: an engine
 * @rules: (nullable) (element-type GowlInputRemapRule): the config's
 *   rules; the engine takes its own references
 *
 * Replaces the config layer wholesale, as a reload does.  Runtime rules
 * are untouched.
 */
void
gowl_input_remap_engine_set_config_rules(
	GowlInputRemapEngine *self,
	GPtrArray            *rules
){
	guint i;

	g_return_if_fail(self != NULL);

	g_ptr_array_set_size(self->config, 0);
	if (rules == NULL)
		return;
	for (i = 0; i < rules->len; i++)
		g_ptr_array_add(self->config,
		                gowl_input_remap_rule_ref(g_ptr_array_index(rules, i)));
}

/**
 * gowl_input_remap_engine_add: (skip)
 * @self: an engine
 * @rule: a rule; the engine takes its own reference
 *
 * Adds a runtime rule.  One with the same name is replaced, and moves
 * to the end: the rule just added is the one that wins.
 *
 * Returns: %TRUE when a runtime rule of that name was replaced
 */
gboolean
gowl_input_remap_engine_add(
	GowlInputRemapEngine *self,
	GowlInputRemapRule   *rule
){
	gint idx;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(rule != NULL, FALSE);

	idx = find_rule(self->runtime, gowl_input_remap_rule_get_name(rule));
	if (idx >= 0)
		g_ptr_array_remove_index(self->runtime, (guint)idx);
	g_ptr_array_add(self->runtime, gowl_input_remap_rule_ref(rule));
	return idx >= 0;
}

/**
 * gowl_input_remap_engine_remove: (skip)
 * @self: an engine
 * @name: a rule name
 * @out_source: (out) (optional): which layer it came out of
 *
 * Removes the runtime rule @name; failing that, the config rule @name
 * (until the next reload brings it back).
 *
 * Returns: %TRUE when a rule was removed
 */
gboolean
gowl_input_remap_engine_remove(
	GowlInputRemapEngine     *self,
	const gchar              *name,
	GowlInputRemapRuleSource *out_source
){
	gint idx;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(name != NULL, FALSE);

	idx = find_rule(self->runtime, name);
	if (idx >= 0) {
		g_ptr_array_remove_index(self->runtime, (guint)idx);
		if (out_source != NULL)
			*out_source = GOWL_INPUT_REMAP_SOURCE_RUNTIME;
		return TRUE;
	}
	idx = find_rule(self->config, name);
	if (idx >= 0) {
		g_ptr_array_remove_index(self->config, (guint)idx);
		if (out_source != NULL)
			*out_source = GOWL_INPUT_REMAP_SOURCE_CONFIG;
		return TRUE;
	}
	return FALSE;
}

/**
 * gowl_input_remap_engine_clear_runtime: (skip)
 * @self: an engine
 *
 * Returns: how many runtime rules were removed
 */
guint
gowl_input_remap_engine_clear_runtime(GowlInputRemapEngine *self)
{
	guint n;

	g_return_val_if_fail(self != NULL, 0);
	n = self->runtime->len;
	g_ptr_array_set_size(self->runtime, 0);
	return n;
}

/**
 * gowl_input_remap_engine_get_rules: (skip)
 * @self: an engine
 *
 * Returns: (transfer full) (element-type GowlInputRemapRule): the rules
 *   in force, config layer first then runtime, shadowed config rules
 *   left out
 */
GPtrArray *
gowl_input_remap_engine_get_rules(GowlInputRemapEngine *self)
{
	GPtrArray *out;
	guint i;

	g_return_val_if_fail(self != NULL, NULL);

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_input_remap_rule_unref);
	for (i = 0; i < self->config->len; i++) {
		GowlInputRemapRule *r = g_ptr_array_index(self->config, i);

		if (!is_shadowed(self, r))
			g_ptr_array_add(out, gowl_input_remap_rule_ref(r));
	}
	for (i = 0; i < self->runtime->len; i++)
		g_ptr_array_add(out, gowl_input_remap_rule_ref(
			g_ptr_array_index(self->runtime, i)));
	return out;
}

/**
 * gowl_input_remap_engine_get_n_rules: (skip)
 * @self: an engine
 *
 * Returns: the number of rules in force
 */
guint
gowl_input_remap_engine_get_n_rules(GowlInputRemapEngine *self)
{
	g_autoptr(GPtrArray) all = NULL;

	g_return_val_if_fail(self != NULL, 0);
	all = gowl_input_remap_engine_get_rules(self);
	return all->len;
}

/**
 * gowl_input_remap_engine_get_source: (skip)
 * @self: an engine
 * @rule: a rule the engine holds
 *
 * Returns: the layer @rule belongs to
 */
GowlInputRemapRuleSource
gowl_input_remap_engine_get_source(
	GowlInputRemapEngine *self,
	GowlInputRemapRule   *rule
){
	guint i;

	g_return_val_if_fail(self != NULL, GOWL_INPUT_REMAP_SOURCE_CONFIG);
	for (i = 0; i < self->runtime->len; i++)
		if (g_ptr_array_index(self->runtime, i) == rule)
			return GOWL_INPUT_REMAP_SOURCE_RUNTIME;
	return GOWL_INPUT_REMAP_SOURCE_CONFIG;
}

/*
 * Walks the rules in priority order -- runtime newest-first, then config
 * last-first -- calling @fn until it returns TRUE.  Shadowed config
 * rules are skipped.
 */
static GowlInputRemapRule *
walk_by_priority(
	GowlInputRemapEngine  *self,
	gboolean             (*fn)(GowlInputRemapRule *rule, gpointer data),
	gpointer               data
){
	guint i;

	for (i = self->runtime->len; i > 0; i--) {
		GowlInputRemapRule *r = g_ptr_array_index(self->runtime, i - 1);

		if (fn(r, data))
			return r;
	}
	for (i = self->config->len; i > 0; i--) {
		GowlInputRemapRule *r = g_ptr_array_index(self->config, i - 1);

		if (!is_shadowed(self, r) && fn(r, data))
			return r;
	}
	return NULL;
}

static gboolean
rule_matches_device(
	GowlInputRemapRule *rule,
	gpointer            data
){
	return gowl_input_remap_rule_matches(rule,
	                                     (const GowlInputDeviceInfo *)data);
}

/**
 * gowl_input_remap_engine_claims: (skip)
 * @self: an engine
 * @info: a device
 *
 * Returns: %TRUE when any rule in force matches @info
 */
gboolean
gowl_input_remap_engine_claims(
	GowlInputRemapEngine      *self,
	const GowlInputDeviceInfo *info
){
	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(info != NULL, FALSE);
	return walk_by_priority(self, rule_matches_device, (gpointer)info) != NULL;
}

typedef struct {
	const GowlInputDeviceInfo *info;
	guint32                    input;
} MapQuery;

static gboolean
rule_maps_input(
	GowlInputRemapRule *rule,
	gpointer            data
){
	MapQuery *q = (MapQuery *)data;

	return gowl_input_remap_rule_matches(rule, q->info)
	       && gowl_input_remap_rule_lookup(rule, q->input) != NULL;
}

/**
 * gowl_input_remap_engine_map: (skip)
 * @self: an engine
 * @info: the device
 * @input: the input code
 * @out_target: (out) (transfer none): the target, owned by the rule
 *
 * The highest-priority rule for @info that mentions @input decides it.
 * When none does, the highest-priority rule matching @info at all is
 * returned with a %NULL target, and its `unmatched' setting decides.
 * When no rule matches the device, %NULL: pass through.
 *
 * Returns: (transfer full) (nullable): the deciding rule
 */
GowlInputRemapRule *
gowl_input_remap_engine_map(
	GowlInputRemapEngine        *self,
	const GowlInputDeviceInfo   *info,
	guint32                      input,
	const GowlInputRemapTarget **out_target
){
	GowlInputRemapRule *r;
	MapQuery q;

	g_return_val_if_fail(out_target != NULL, NULL);
	*out_target = NULL;
	g_return_val_if_fail(self != NULL && info != NULL, NULL);

	q.info = info;
	q.input = input;
	r = walk_by_priority(self, rule_maps_input, &q);
	if (r != NULL) {
		*out_target = gowl_input_remap_rule_lookup(r, input);
		return gowl_input_remap_rule_ref(r);
	}
	r = walk_by_priority(self, rule_matches_device, (gpointer)info);
	return r != NULL ? gowl_input_remap_rule_ref(r) : NULL;
}
