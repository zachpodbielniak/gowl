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
 * test-input-remap-engine.c - The inputremap module's rule engine.
 *
 * The engine is a pure translation unit, included here so the decision
 * -- which rule claims a device, which rule decides an input, what wins
 * when two do -- is checked without a compositor or a loaded plugin.
 */

#include <glib.h>
#include <linux/input-event-codes.h>

#include "gowl.h"
#include "../modules/inputremap/gowl-inputremap-engine.c"

typedef struct {
	GowlInputRemapEngine *engine;
	GowlInputDeviceInfo  *pedal;
	GowlInputDeviceInfo  *keyboard;
} Fixture;

static GowlInputRemapRule *
rule_from(const gchar *yaml)
{
	GowlInputRemapRule *rule;
	GError *error = NULL;

	rule = gowl_input_remap_rule_new_from_yaml(yaml, &error);
	g_assert_no_error(error);
	return rule;
}

static void
fixture_setup(
	Fixture       *f,
	gconstpointer  data
){
	(void)data;
	f->engine = gowl_input_remap_engine_new();
	f->pedal = gowl_input_device_info_new(5, GOWL_INPUT_REMAP_DEVICE_KEYBOARD,
	                                      "PCsensor FootSwitch", "event19",
	                                      0x1a86, 0xe026);
	f->keyboard = gowl_input_device_info_new(1,
	                                         GOWL_INPUT_REMAP_DEVICE_KEYBOARD,
	                                         "AT Translated Set 2 keyboard",
	                                         "event3", 0x0001, 0x0001);
}

static void
fixture_teardown(
	Fixture       *f,
	gconstpointer  data
){
	(void)data;
	gowl_input_remap_engine_free(f->engine);
	gowl_input_device_info_free(f->pedal);
	gowl_input_device_info_free(f->keyboard);
}

/* No rules: nothing claimed, every input passes (NULL). */
static void
test_pass_through_without_rules(
	Fixture       *f,
	gconstpointer  data
){
	const GowlInputRemapTarget *t;
	GowlInputRemapRule *r;

	(void)data;
	g_assert_false(gowl_input_remap_engine_claims(f->engine, f->pedal));
	r = gowl_input_remap_engine_map(f->engine, f->pedal, KEY_A, &t);
	g_assert_null(r);
	g_assert_null(t);
	g_assert_cmpuint(gowl_input_remap_engine_get_n_rules(f->engine), ==, 0);
}

/* A rule claims its device and not the keyboard next to it. */
static void
test_claims_only_matching(
	Fixture       *f,
	gconstpointer  data
){
	g_autoptr(GowlInputRemapRule) rule = NULL;
	g_autoptr(GowlInputRemapRule) got = NULL;
	const GowlInputRemapTarget *t;

	(void)data;
	rule = rule_from("{name: p, match: {id: \"1a86:e026\"}, "
	                 "map: {KEY_A: {button: middle}}}");
	gowl_input_remap_engine_add(f->engine, rule);

	g_assert_true(gowl_input_remap_engine_claims(f->engine, f->pedal));
	g_assert_false(gowl_input_remap_engine_claims(f->engine, f->keyboard));

	/* the same keycode from the other keyboard is untouched */
	got = gowl_input_remap_engine_map(f->engine, f->keyboard, KEY_A, &t);
	g_assert_null(got);
	g_clear_pointer(&got, gowl_input_remap_rule_unref);

	got = gowl_input_remap_engine_map(f->engine, f->pedal, KEY_A, &t);
	g_assert_true(got == rule);
	g_assert_cmpint(gowl_input_remap_target_get_kind(t), ==,
	                GOWL_INPUT_REMAP_TARGET_BUTTON);
}

/* An unmapped input on a claimed device: the rule's `unmatched' decides. */
static void
test_unmatched(
	Fixture       *f,
	gconstpointer  data
){
	g_autoptr(GowlInputRemapRule) rule = NULL;
	g_autoptr(GowlInputRemapRule) got = NULL;
	const GowlInputRemapTarget *t;

	(void)data;
	rule = rule_from("{name: p, match: {name: \"*FootSwitch\"}, "
	                 "unmatched: drop, map: {KEY_A: drop}}");
	gowl_input_remap_engine_add(f->engine, rule);

	got = gowl_input_remap_engine_map(f->engine, f->pedal, KEY_Z, &t);
	g_assert_true(got == rule);
	g_assert_null(t);
	g_assert_true(gowl_input_remap_rule_get_drop_unmatched(got));
}

/*
 * Priority: runtime before config, later before earlier -- and a rule
 * that does not mention an input steps aside for one that does.
 */
static void
test_priority(
	Fixture       *f,
	gconstpointer  data
){
	g_autoptr(GPtrArray) config = NULL;
	g_autoptr(GowlInputRemapRule) broad = NULL;
	g_autoptr(GowlInputRemapRule) narrow = NULL;
	g_autoptr(GowlInputRemapRule) runtime = NULL;
	GowlInputRemapRule *got;
	const GowlInputRemapTarget *t;

	(void)data;
	broad = rule_from("{name: broad, match: {vendor: 0x1a86}, "
	                  "map: {KEY_A: drop, KEY_B: drop}}");
	narrow = rule_from("{name: narrow, match: {id: \"1a86:e026\"}, "
	                   "map: {KEY_A: {button: left}}}");
	config = g_ptr_array_new();
	g_ptr_array_add(config, broad);
	g_ptr_array_add(config, narrow);
	gowl_input_remap_engine_set_config_rules(f->engine, config);

	/* the later config rule decides KEY_A... */
	got = gowl_input_remap_engine_map(f->engine, f->pedal, KEY_A, &t);
	g_assert_true(got == narrow);
	gowl_input_remap_rule_unref(got);
	/* ...and the earlier one still decides KEY_B, which narrow lacks */
	got = gowl_input_remap_engine_map(f->engine, f->pedal, KEY_B, &t);
	g_assert_true(got == broad);
	gowl_input_remap_rule_unref(got);

	/* a runtime rule beats both */
	runtime = rule_from("{name: rt, match: {name: \"PCsensor*\"}, "
	                    "map: {KEY_A: {key: KEY_F13}}}");
	gowl_input_remap_engine_add(f->engine, runtime);
	got = gowl_input_remap_engine_map(f->engine, f->pedal, KEY_A, &t);
	g_assert_true(got == runtime);
	g_assert_cmpint(gowl_input_remap_target_get_kind(t), ==,
	                GOWL_INPUT_REMAP_TARGET_KEY);
	gowl_input_remap_rule_unref(got);
}

/* A runtime rule of the same name shadows the config one; removing it
 * brings the config rule back; removing again takes the config rule. */
static void
test_shadow_and_remove(
	Fixture       *f,
	gconstpointer  data
){
	g_autoptr(GPtrArray) config = NULL;
	g_autoptr(GowlInputRemapRule) from_config = NULL;
	g_autoptr(GowlInputRemapRule) override = NULL;
	g_autoptr(GowlInputRemapRule) newer = NULL;
	GowlInputRemapRule *got;
	GowlInputRemapRuleSource source;
	const GowlInputRemapTarget *t;
	g_autoptr(GPtrArray) all = NULL;

	(void)data;
	from_config = rule_from("{name: pedals, match: {id: \"1a86:e026\"}, "
	                        "map: {KEY_A: drop}}");
	override = rule_from("{name: pedals, match: {id: \"1a86:e026\"}, "
	                     "map: {KEY_A: pass}}");
	config = g_ptr_array_new();
	g_ptr_array_add(config, from_config);
	gowl_input_remap_engine_set_config_rules(f->engine, config);

	g_assert_false(gowl_input_remap_engine_add(f->engine, override));
	all = gowl_input_remap_engine_get_rules(f->engine);
	g_assert_cmpuint(all->len, ==, 1);
	g_assert_true(g_ptr_array_index(all, 0) == override);
	g_assert_cmpint(gowl_input_remap_engine_get_source(f->engine, override),
	                ==, GOWL_INPUT_REMAP_SOURCE_RUNTIME);

	/* adding the same name again replaces, and reports it */
	newer = rule_from("{name: pedals, match: {id: \"1a86:e026\"}, "
	                  "map: {KEY_A: {button: right}}}");
	g_assert_true(gowl_input_remap_engine_add(f->engine, newer));
	got = gowl_input_remap_engine_map(f->engine, f->pedal, KEY_A, &t);
	g_assert_true(got == newer);
	gowl_input_remap_rule_unref(got);

	g_assert_true(gowl_input_remap_engine_remove(f->engine, "pedals",
	                                             &source));
	g_assert_cmpint(source, ==, GOWL_INPUT_REMAP_SOURCE_RUNTIME);
	got = gowl_input_remap_engine_map(f->engine, f->pedal, KEY_A, &t);
	g_assert_true(got == from_config);
	gowl_input_remap_rule_unref(got);

	g_assert_true(gowl_input_remap_engine_remove(f->engine, "pedals",
	                                             &source));
	g_assert_cmpint(source, ==, GOWL_INPUT_REMAP_SOURCE_CONFIG);
	g_assert_false(gowl_input_remap_engine_claims(f->engine, f->pedal));
	g_assert_false(gowl_input_remap_engine_remove(f->engine, "pedals", NULL));
}

/* Clearing takes the runtime layer only; a reload replaces the config. */
static void
test_clear_and_reload(
	Fixture       *f,
	gconstpointer  data
){
	g_autoptr(GPtrArray) config = NULL;
	g_autoptr(GowlInputRemapRule) a = NULL;
	g_autoptr(GowlInputRemapRule) b = NULL;

	(void)data;
	a = rule_from("{name: a, match: {name: x}, map: {}}");
	b = rule_from("{name: b, match: {name: y}, map: {}}");
	config = g_ptr_array_new();
	g_ptr_array_add(config, a);
	gowl_input_remap_engine_set_config_rules(f->engine, config);
	gowl_input_remap_engine_add(f->engine, b);
	g_assert_cmpuint(gowl_input_remap_engine_get_n_rules(f->engine), ==, 2);

	g_assert_cmpuint(gowl_input_remap_engine_clear_runtime(f->engine), ==, 1);
	g_assert_cmpuint(gowl_input_remap_engine_get_n_rules(f->engine), ==, 1);

	gowl_input_remap_engine_set_config_rules(f->engine, NULL);
	g_assert_cmpuint(gowl_input_remap_engine_get_n_rules(f->engine), ==, 0);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, fn) \
	g_test_add("/input-remap/engine/" path, Fixture, NULL, \
	           fixture_setup, fn, fixture_teardown)

	ADD("pass-through-without-rules", test_pass_through_without_rules);
	ADD("claims-only-matching", test_claims_only_matching);
	ADD("unmatched", test_unmatched);
	ADD("priority", test_priority);
	ADD("shadow-and-remove", test_shadow_and_remove);
	ADD("clear-and-reload", test_clear_and_reload);

#undef ADD
	return g_test_run();
}
