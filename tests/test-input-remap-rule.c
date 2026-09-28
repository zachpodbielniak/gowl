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
 * test-input-remap-rule.c - The input-remap rule model.
 *
 * Parsing (YAML text and the config's `input-remap:' section), input
 * and button names, device matching, the one-target-per-input table,
 * the text round trip, and -- the point of the whole data model --
 * that anything shaped like a macro is refused before it can exist.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <string.h>
#include <unistd.h>

#include "gowl.h"
#include "boxed/gowl-input-remap-rule-private.h"

/* The WoW pedal rule the docs use, as block YAML. */
static const gchar *const wow_rule_yaml =
	"name: wow-pedals\n"
	"match:\n"
	"  vendor: 0x1a86\n"
	"  product: e026\n"
	"  type: keyboard\n"
	"log: true\n"
	"map:\n"
	"  KEY_A: { button: middle }\n"
	"  KEY_B: { action: focus-client, arg: \"app-id:*wow*\" }\n"
	"  KEY_C: { key: \"Super+9\" }\n"
	"  KEY_D: drop\n"
	"  KEY_E: { command: \"scratchpad-toggle\" }\n";

/*
 * gowl builds with G_LOG_USE_STRUCTURED, so a warning goes through the
 * log writer and g_test_expect_message() never sees it (test-keybind.c
 * explains).  This writer swallows the one warning a test expects.
 */
static const gchar *expected_warning = NULL;
static gboolean saw_expected_warning = FALSE;

static GLogWriterOutput
expect_warning_writer(
	GLogLevelFlags   level,
	const GLogField *fields,
	gsize            n_fields,
	gpointer         user_data
){
	gsize i;

	if (expected_warning != NULL && (level & G_LOG_LEVEL_WARNING)) {
		for (i = 0; i < n_fields; i++) {
			if (g_strcmp0(fields[i].key, "MESSAGE") == 0
			    && strstr((const gchar *)fields[i].value,
			              expected_warning) != NULL) {
				saw_expected_warning = TRUE;
				return G_LOG_WRITER_HANDLED;
			}
		}
	}
	return g_log_writer_default(level, fields, n_fields, user_data);
}

static GowlInputDeviceInfo *
pedal_info(void)
{
	return gowl_input_device_info_new(7, GOWL_INPUT_REMAP_DEVICE_KEYBOARD,
	                                  "PCsensor FootSwitch", "event19",
	                                  0x1a86, 0xe026);
}

/* --- names --- */

static void
test_names(void)
{
	guint32 code;
	g_autofree gchar *n1 = NULL;
	g_autofree gchar *n2 = NULL;
	g_autofree gchar *n3 = NULL;

	g_assert_true(gowl_input_remap_code_from_name("KEY_A", &code));
	g_assert_cmpuint(code, ==, KEY_A);
	g_assert_true(gowl_input_remap_code_from_name("key_f13", &code));
	g_assert_cmpuint(code, ==, KEY_F13);
	g_assert_true(gowl_input_remap_code_from_name("BTN_SIDE", &code));
	g_assert_cmpuint(code, ==, BTN_SIDE);
	g_assert_true(gowl_input_remap_code_from_name("middle", &code));
	g_assert_cmpuint(code, ==, BTN_MIDDLE);
	g_assert_true(gowl_input_remap_code_from_name("wheel-up", &code));
	g_assert_cmpuint(code, ==, GOWL_BUTTON_WHEEL_UP);
	g_assert_true(gowl_input_remap_code_from_name("30", &code));
	g_assert_cmpuint(code, ==, 30);
	g_assert_true(gowl_input_remap_code_from_name("0x110", &code));
	g_assert_cmpuint(code, ==, BTN_LEFT);

	/* 0 is KEY_RESERVED and past KEY_MAX is nothing */
	g_assert_false(gowl_input_remap_code_from_name("0", &code));
	g_assert_false(gowl_input_remap_code_from_name("0x5000", &code));
	g_assert_false(gowl_input_remap_code_from_name("KEY_NOPE", &code));
	g_assert_false(gowl_input_remap_code_from_name("", &code));

	n1 = gowl_input_remap_code_to_name(KEY_A);
	g_assert_cmpstr(n1, ==, "KEY_A");
	n2 = gowl_input_remap_code_to_name(GOWL_BUTTON_WHEEL_DOWN);
	g_assert_cmpstr(n2, ==, "WHEEL_DOWN");
	n3 = gowl_input_remap_code_to_name(0x3ff);
	g_assert_cmpstr(n3, ==, "0x3ff");

	g_assert_true(gowl_input_remap_button_from_name("Middle", &code));
	g_assert_cmpuint(code, ==, BTN_MIDDLE);
	g_assert_true(gowl_input_remap_button_from_name("BTN_EXTRA", &code));
	g_assert_cmpuint(code, ==, BTN_EXTRA);
	g_assert_false(gowl_input_remap_button_from_name("KEY_A", &code));
}

/* --- parsing --- */

static void
test_parse_block(void)
{
	g_autoptr(GowlInputRemapRule) rule = NULL;
	g_autoptr(GError) error = NULL;
	const GowlInputRemapTarget *t;

	rule = gowl_input_remap_rule_new_from_yaml(wow_rule_yaml, &error);
	g_assert_no_error(error);
	g_assert_nonnull(rule);

	g_assert_cmpstr(gowl_input_remap_rule_get_name(rule), ==, "wow-pedals");
	g_assert_cmpuint(gowl_input_remap_rule_get_match_vendor(rule), ==, 0x1a86);
	g_assert_cmpuint(gowl_input_remap_rule_get_match_product(rule), ==, 0xe026);
	g_assert_cmpint(gowl_input_remap_rule_get_match_type(rule), ==,
	                GOWL_INPUT_REMAP_DEVICE_KEYBOARD);
	g_assert_true(gowl_input_remap_rule_get_log(rule));
	g_assert_false(gowl_input_remap_rule_get_drop_unmatched(rule));
	g_assert_cmpuint(gowl_input_remap_rule_get_n_mappings(rule), ==, 5);

	/* pedal A: middle click */
	t = gowl_input_remap_rule_lookup(rule, KEY_A);
	g_assert_nonnull(t);
	g_assert_cmpint(gowl_input_remap_target_get_kind(t), ==,
	                GOWL_INPUT_REMAP_TARGET_BUTTON);
	g_assert_cmpuint(gowl_input_remap_target_get_code(t), ==, BTN_MIDDLE);

	/* pedal B: jump to the game */
	t = gowl_input_remap_rule_lookup(rule, KEY_B);
	g_assert_cmpint(gowl_input_remap_target_get_kind(t), ==,
	                GOWL_INPUT_REMAP_TARGET_ACTION);
	g_assert_cmpint(gowl_input_remap_target_get_action(t), ==,
	                GOWL_ACTION_FOCUS_CLIENT);
	g_assert_cmpstr(gowl_input_remap_target_get_arg(t), ==, "app-id:*wow*");

	/* a keysym with a modifier, resolved at press time */
	t = gowl_input_remap_rule_lookup(rule, KEY_C);
	g_assert_cmpint(gowl_input_remap_target_get_kind(t), ==,
	                GOWL_INPUT_REMAP_TARGET_KEY);
	g_assert_cmpuint(gowl_input_remap_target_get_code(t), ==, 0);
	g_assert_cmpuint(gowl_input_remap_target_get_keysym(t), ==, XKB_KEY_9);
	g_assert_cmpuint(gowl_input_remap_target_get_modifiers(t), ==,
	                 GOWL_KEY_MOD_LOGO);

	t = gowl_input_remap_rule_lookup(rule, KEY_D);
	g_assert_cmpint(gowl_input_remap_target_get_kind(t), ==,
	                GOWL_INPUT_REMAP_TARGET_DROP);
	t = gowl_input_remap_rule_lookup(rule, KEY_E);
	g_assert_cmpint(gowl_input_remap_target_get_kind(t), ==,
	                GOWL_INPUT_REMAP_TARGET_COMMAND);
	g_assert_cmpstr(gowl_input_remap_target_get_arg(t), ==,
	                "scratchpad-toggle");

	/* an input the rule does not mention */
	g_assert_null(gowl_input_remap_rule_lookup(rule, KEY_F));
}

static void
test_parse_flow_and_id(void)
{
	g_autoptr(GowlInputRemapRule) rule = NULL;
	g_autoptr(GError) error = NULL;
	const GowlInputRemapTarget *t;

	rule = gowl_input_remap_rule_new_from_yaml(
		"{name: mouse, match: {id: \"046d:c52b\", name: \"*Mouse*\"}, "
		"unmatched: drop, map: {BTN_SIDE: {key: KEY_PAGEUP}, "
		"WHEEL_UP: {action: tag-view, arg: 1}, BTN_EXTRA: pass}}",
		&error);
	g_assert_no_error(error);
	g_assert_cmpuint(gowl_input_remap_rule_get_match_vendor(rule), ==, 0x046d);
	g_assert_cmpuint(gowl_input_remap_rule_get_match_product(rule), ==, 0xc52b);
	g_assert_cmpstr(gowl_input_remap_rule_get_match_name(rule), ==, "*Mouse*");
	g_assert_true(gowl_input_remap_rule_get_drop_unmatched(rule));

	t = gowl_input_remap_rule_lookup(rule, BTN_SIDE);
	g_assert_cmpint(gowl_input_remap_target_get_kind(t), ==,
	                GOWL_INPUT_REMAP_TARGET_KEY);
	g_assert_cmpuint(gowl_input_remap_target_get_code(t), ==, KEY_PAGEUP);
	t = gowl_input_remap_rule_lookup(rule, GOWL_BUTTON_WHEEL_UP);
	g_assert_cmpint(gowl_input_remap_target_get_action(t), ==,
	                GOWL_ACTION_TAG_VIEW);
	g_assert_cmpstr(gowl_input_remap_target_get_arg(t), ==, "1");
	t = gowl_input_remap_rule_lookup(rule, BTN_EXTRA);
	g_assert_cmpint(gowl_input_remap_target_get_kind(t), ==,
	                GOWL_INPUT_REMAP_TARGET_PASS);
}

/*
 * The hard constraint.  Each of these is a way of asking one press for
 * more than one output, and each must be refused with NOT_ONE_TO_ONE --
 * not accepted and ignored, not accepted with a warning.
 */
static void
test_reject_not_one_to_one(void)
{
	static const gchar *const bad[] = {
		/* a list of targets */
		"{name: a, match: {name: x}, map: {KEY_A: [{key: a}, {key: b}]}}",
		/* the whole map as a list */
		"{name: a, match: {name: x}, map: [{KEY_A: {key: a}}]}",
		/* two kinds in one target */
		"{name: a, match: {name: x}, map: {KEY_A: {key: a, button: left}}}",
		/* a delay */
		"{name: a, match: {name: x}, map: {KEY_A: {key: a, delay: 50}}}",
		/* a repeat */
		"{name: a, match: {name: x}, map: {KEY_A: {key: a, repeat: 3}}}",
		/* a macro key at the top */
		"{name: a, match: {name: x}, macro: [a, b], map: {}}",
		/* a sequence key inside */
		"{name: a, match: {name: x}, map: {KEY_A: {sequence: [a, b]}}}",
		/* two keys in a key target */
		"{name: a, match: {name: x}, map: {KEY_A: {key: \"a+b\"}}}",
		/* a key target that is a list */
		"{name: a, match: {name: x}, map: {KEY_A: {key: [a, b]}}}",
		/* a command holding two lines */
		"{name: a, match: {name: x}, map: {KEY_A: {command: \"one\\ntwo\"}}}",
		NULL
	};
	guint i;

	for (i = 0; bad[i] != NULL; i++) {
		g_autoptr(GowlInputRemapRule) rule = NULL;
		g_autoptr(GError) error = NULL;

		rule = gowl_input_remap_rule_new_from_yaml(bad[i], &error);
		if (rule != NULL || !g_error_matches(error, GOWL_INPUT_REMAP_ERROR,
		                                     GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE))
			g_test_message("case %u: %s -> %s", i, bad[i],
			               error != NULL ? error->message : "accepted");
		g_assert_null(rule);
		g_assert_error(error, GOWL_INPUT_REMAP_ERROR,
		               GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE);
	}
}

/* Malformed, but not macro-shaped: INVALID or UNKNOWN_CODE. */
static void
test_reject_invalid(void)
{
	static const struct {
		const gchar *yaml;
		gint         code;
	} bad[] = {
		{ "{match: {name: x}, map: {}}", GOWL_INPUT_REMAP_ERROR_INVALID },
		{ "{name: a, map: {}}", GOWL_INPUT_REMAP_ERROR_INVALID },
		{ "{name: a, match: {type: keyboard}, map: {}}",
		  GOWL_INPUT_REMAP_ERROR_INVALID },
		{ "{name: a, match: {name: x}, colour: red}",
		  GOWL_INPUT_REMAP_ERROR_INVALID },
		{ "{name: a, match: {vendor: zzzz}}", GOWL_INPUT_REMAP_ERROR_INVALID },
		{ "{name: a, match: {name: x}, map: {KEY_A: {callback: f}}}",
		  GOWL_INPUT_REMAP_ERROR_INVALID },
		{ "{name: a, match: {name: x}, map: {KEY_A: {arg: 1}}}",
		  GOWL_INPUT_REMAP_ERROR_INVALID },
		{ "{name: a, match: {name: x}, map: {KEY_A: {key: a, arg: 1}}}",
		  GOWL_INPUT_REMAP_ERROR_INVALID },
		{ "{name: a, match: {name: x}, map: {KEY_A: middle}}",
		  GOWL_INPUT_REMAP_ERROR_INVALID },
		{ "{name: a, match: {name: x}, map: {KEY_NOPE: drop}}",
		  GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE },
		{ "{name: a, match: {name: x}, map: {KEY_A: {key: NotAKeysym}}}",
		  GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE },
		{ "{name: a, match: {name: x}, map: {KEY_A: {button: KEY_B}}}",
		  GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE },
		{ "{name: a, match: {name: x}, map: {KEY_A: {action: fly}}}",
		  GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE },
		{ NULL, 0 }
	};
	guint i;

	for (i = 0; bad[i].yaml != NULL; i++) {
		g_autoptr(GowlInputRemapRule) rule = NULL;
		g_autoptr(GError) error = NULL;

		rule = gowl_input_remap_rule_new_from_yaml(bad[i].yaml, &error);
		if (rule != NULL || error == NULL || error->code != bad[i].code)
			g_test_message("case %u: %s -> %s", i, bad[i].yaml,
			               error != NULL ? error->message : "accepted");
		g_assert_null(rule);
		g_assert_error(error, GOWL_INPUT_REMAP_ERROR, bad[i].code);
	}
}

/* --- the mapping table --- */

static void
test_mapping_replaces(void)
{
	g_autoptr(GowlInputRemapRule) rule = NULL;
	g_autoptr(GArray) inputs = NULL;
	g_autoptr(GError) error = NULL;
	const GowlInputRemapTarget *t;

	rule = gowl_input_remap_rule_new("table");
	g_assert_true(gowl_input_remap_rule_map_key(rule, KEY_A, KEY_B, 0, 0,
	                                            &error));
	g_assert_true(gowl_input_remap_rule_map_button(rule, KEY_A, BTN_LEFT,
	                                               &error));
	g_assert_no_error(error);

	/* One input, one target: the second mapping replaced the first */
	g_assert_cmpuint(gowl_input_remap_rule_get_n_mappings(rule), ==, 1);
	t = gowl_input_remap_rule_lookup(rule, KEY_A);
	g_assert_cmpint(gowl_input_remap_target_get_kind(t), ==,
	                GOWL_INPUT_REMAP_TARGET_BUTTON);

	gowl_input_remap_rule_map_drop(rule, KEY_C);
	gowl_input_remap_rule_map_pass(rule, BTN_SIDE);
	inputs = gowl_input_remap_rule_get_inputs(rule);
	g_assert_cmpuint(inputs->len, ==, 3);
	/* ascending */
	g_assert_cmpuint(g_array_index(inputs, guint32, 0), ==, KEY_A);
	g_assert_cmpuint(g_array_index(inputs, guint32, 1), ==, KEY_C);
	g_assert_cmpuint(g_array_index(inputs, guint32, 2), ==, BTN_SIDE);

	g_assert_true(gowl_input_remap_rule_unmap(rule, KEY_C));
	g_assert_false(gowl_input_remap_rule_unmap(rule, KEY_C));

	/* invalid targets are refused at the API, not just in YAML */
	g_assert_false(gowl_input_remap_rule_map_button(rule, KEY_A, KEY_B,
	                                                &error));
	g_assert_error(error, GOWL_INPUT_REMAP_ERROR,
	               GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE);
	g_clear_error(&error);
	g_assert_false(gowl_input_remap_rule_map_key(rule, 0, KEY_A, 0, 0,
	                                             &error));
	g_assert_error(error, GOWL_INPUT_REMAP_ERROR,
	               GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE);
	g_clear_error(&error);
	g_assert_false(gowl_input_remap_rule_map_command(rule, KEY_A, "a\nb",
	                                                 &error));
	g_assert_error(error, GOWL_INPUT_REMAP_ERROR,
	               GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE);
	g_clear_error(&error);
	g_assert_false(gowl_input_remap_rule_map_action(rule, KEY_A,
	                                                GOWL_ACTION_NONE, NULL,
	                                                &error));
	g_assert_error(error, GOWL_INPUT_REMAP_ERROR,
	               GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE);
}

/* --- device matching --- */

static void
test_matching(void)
{
	g_autoptr(GowlInputRemapRule) rule = NULL;
	g_autoptr(GowlInputDeviceInfo) pedal = pedal_info();
	g_autoptr(GowlInputDeviceInfo) other = NULL;
	g_autoptr(GowlInputDeviceInfo) mouse = NULL;

	other = gowl_input_device_info_new(1, GOWL_INPUT_REMAP_DEVICE_KEYBOARD,
	                                   "AT Translated Set 2 keyboard",
	                                   "event3", 0x0001, 0x0001);
	mouse = gowl_input_device_info_new(2, GOWL_INPUT_REMAP_DEVICE_POINTER,
	                                   "PCsensor FootSwitch Mouse",
	                                   "event20", 0x1a86, 0xe026);

	/* No criteria: matches nothing, not everything */
	rule = gowl_input_remap_rule_new("r");
	g_assert_false(gowl_input_remap_rule_has_match(rule));
	g_assert_false(gowl_input_remap_rule_matches(rule, pedal));

	/* vendor:product matches both halves of a composite device */
	gowl_input_remap_rule_set_match_ids(rule, 0x1a86, 0xe026);
	g_assert_true(gowl_input_remap_rule_matches(rule, pedal));
	g_assert_true(gowl_input_remap_rule_matches(rule, mouse));
	g_assert_false(gowl_input_remap_rule_matches(rule, other));

	/* ...and a type narrows it to one */
	gowl_input_remap_rule_set_match_type(rule, GOWL_INPUT_REMAP_DEVICE_POINTER);
	g_assert_false(gowl_input_remap_rule_matches(rule, pedal));
	g_assert_true(gowl_input_remap_rule_matches(rule, mouse));
	gowl_input_remap_rule_set_match_type(rule, GOWL_INPUT_REMAP_DEVICE_ANY);

	/* every set criterion must hold */
	gowl_input_remap_rule_set_match_name(rule, "*FootSwitch");
	g_assert_true(gowl_input_remap_rule_matches(rule, pedal));
	g_assert_false(gowl_input_remap_rule_matches(rule, mouse));
	gowl_input_remap_rule_set_match_sysname(rule, "event1?");
	g_assert_true(gowl_input_remap_rule_matches(rule, pedal));
	gowl_input_remap_rule_set_match_sysname(rule, "event2*");
	g_assert_false(gowl_input_remap_rule_matches(rule, pedal));

	/* a name glob alone, product only */
	gowl_input_remap_rule_set_match_sysname(rule, NULL);
	gowl_input_remap_rule_set_match_ids(rule, 0, 0xe026);
	g_assert_true(gowl_input_remap_rule_matches(rule, pedal));
}

/* --- text round trip --- */

static void
test_roundtrip(void)
{
	g_autoptr(GowlInputRemapRule) rule = NULL;
	g_autoptr(GowlInputRemapRule) again = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;

	rule = gowl_input_remap_rule_new_from_yaml(wow_rule_yaml, &error);
	g_assert_no_error(error);
	first = gowl_input_remap_rule_to_yaml(rule);
	again = gowl_input_remap_rule_new_from_yaml(first, &error);
	g_assert_no_error(error);
	second = gowl_input_remap_rule_to_yaml(again);
	g_assert_cmpstr(first, ==, second);
	g_assert_nonnull(strstr(first, "KEY_A: {button: BTN_MIDDLE}"));
	g_assert_nonnull(strstr(first, "{key: \"Super+9\"}"));
}

/* --- callbacks --- */

static guint callback_calls;

static void
count_callback(
	GowlInputRemapRule        *rule,
	const GowlInputRemapEvent *event,
	GowlCompositor            *compositor,
	gpointer                   user_data
){
	(void)rule;
	(void)compositor;
	g_assert_cmpuint(event->code, ==, KEY_A);
	callback_calls += GPOINTER_TO_UINT(user_data);
}

static void
test_callback(void)
{
	g_autoptr(GowlInputRemapRule) rule = NULL;
	const GowlInputRemapTarget *t;
	GowlInputRemapEvent ev;
	guint gen;
	g_autofree gchar *yaml = NULL;

	gen = gowl_input_remap_callback_generation();
	rule = gowl_input_remap_rule_new("cb");
	gowl_input_remap_rule_set_match_name(rule, "x");
	gowl_input_remap_rule_map_callback(rule, KEY_A, count_callback,
	                                   GUINT_TO_POINTER(1), NULL);
	g_assert_cmpuint(gowl_input_remap_callback_generation(), !=, gen);

	memset(&ev, 0, sizeof ev);
	ev.kind = GOWL_INPUT_REMAP_EVENT_KEY;
	ev.code = KEY_A;
	ev.pressed = TRUE;
	callback_calls = 0;
	t = gowl_input_remap_rule_lookup(rule, KEY_A);
	gowl_input_remap_target_invoke(t, rule, &ev, NULL);
	ev.pressed = FALSE;
	gowl_input_remap_target_invoke(t, rule, &ev, NULL);
	g_assert_cmpuint(callback_calls, ==, 2);

	/* listed, but a callback cannot come back in through text */
	yaml = gowl_input_remap_rule_to_yaml(rule);
	g_assert_nonnull(strstr(yaml, "{callback: native}"));
}

/* --- the config section --- */

static void
test_config_section(void)
{
	g_autoptr(GowlConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *yaml = NULL;
	g_autoptr(GowlInputRemapRule) replacement = NULL;
	GPtrArray *rules;
	gint fd;

	fd = g_file_open_tmp("gowl-remap-XXXXXX.yaml", &path, &error);
	g_assert_no_error(error);
	close(fd);
	g_assert_true(g_file_set_contents(path,
		"input-remap:\n"
		"  - name: wow-pedals\n"
		"    match: { id: \"1a86:e026\" }\n"
		"    map:\n"
		"      KEY_A: { button: middle }\n"
		"      KEY_B: { action: focus-client, arg: \"app-id:*wow*\" }\n"
		"  - name: bad\n"
		"    match: { name: x }\n"
		"    map: { KEY_A: [ {key: a}, {key: b} ] }\n"
		"  - name: mouse\n"
		"    match: { name: \"*Mouse*\" }\n"
		"    map: { BTN_SIDE: drop }\n",
		-1, &error));
	g_assert_no_error(error);

	config = gowl_config_new();
	expected_warning = "input-remap rule 2";
	saw_expected_warning = FALSE;
	g_assert_true(gowl_config_load_yaml(config, path, &error));
	expected_warning = NULL;
	g_assert_true(saw_expected_warning);
	g_assert_no_error(error);
	g_unlink(path);

	/* the macro-shaped rule was refused and counted; the rest loaded */
	rules = gowl_config_get_input_remap_rules(config);
	g_assert_cmpuint(rules->len, ==, 2);
	g_assert_cmpuint(gowl_config_get_problem_count(config), >=, 1);
	g_assert_cmpstr(gowl_input_remap_rule_get_name(
		g_ptr_array_index(rules, 0)), ==, "wow-pedals");

	/* the C API replaces by name, in place */
	replacement = gowl_input_remap_rule_new("wow-pedals");
	gowl_input_remap_rule_set_match_name(replacement, "*Foot*");
	gowl_config_add_input_remap_rule(config, replacement);
	g_assert_cmpuint(rules->len, ==, 2);
	g_assert_true(g_ptr_array_index(rules, 0) == replacement);

	/* written back out */
	yaml = gowl_config_generate_yaml(config);
	g_assert_nonnull(strstr(yaml, "input-remap:"));
	g_assert_nonnull(strstr(yaml, "name: \"mouse\""));

	g_assert_true(gowl_config_remove_input_remap_rule(config, "mouse"));
	g_assert_false(gowl_config_remove_input_remap_rule(config, "mouse"));
	gowl_config_clear_input_remap_rules(config);
	g_assert_cmpuint(rules->len, ==, 0);
}

/* A list of rules is all or nothing. */
static void
test_rules_list(void)
{
	g_autoptr(GPtrArray) rules = NULL;
	g_autoptr(GError) error = NULL;

	rules = gowl_input_remap_rules_from_yaml(
		"- {name: a, match: {name: x}, map: {KEY_A: drop}}\n"
		"- {name: b, match: {name: y}, map: {KEY_B: pass}}\n", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rules->len, ==, 2);
	g_clear_pointer(&rules, g_ptr_array_unref);

	rules = gowl_input_remap_rules_from_yaml(
		"- {name: a, match: {name: x}, map: {KEY_A: drop}}\n"
		"- {name: b, match: {name: y}, map: {KEY_B: {key: a, delay: 5}}}\n",
		&error);
	g_assert_null(rules);
	g_assert_error(error, GOWL_INPUT_REMAP_ERROR,
	               GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE);
}

/* The example the docs point people at must load, cleanly. */
static void
test_shipped_example(void)
{
	g_autoptr(GowlConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	GowlInputRemapRule *wow;
	const GowlInputRemapTarget *t;
	GPtrArray *rules;

	path = g_build_filename(GOWL_DEV_DATADIR, "example-input-remap.yaml",
	                        NULL);
	config = gowl_config_new();
	g_assert_true(gowl_config_load_yaml(config, path, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(gowl_config_get_problem_count(config), ==, 0);

	rules = gowl_config_get_input_remap_rules(config);
	g_assert_cmpuint(rules->len, ==, 2);
	wow = g_ptr_array_index(rules, 0);
	g_assert_cmpstr(gowl_input_remap_rule_get_name(wow), ==, "wow-pedals");
	t = gowl_input_remap_rule_lookup(wow, KEY_A);
	g_assert_cmpuint(gowl_input_remap_target_get_code(t), ==, BTN_MIDDLE);
	t = gowl_input_remap_rule_lookup(wow, KEY_B);
	g_assert_cmpint(gowl_input_remap_target_get_action(t), ==,
	                GOWL_ACTION_FOCUS_CLIENT);
	g_assert_nonnull(gowl_config_get_module_config(config, "inputremap"));
}

/* The new action is a real action: nick, and reachable by name. */
static void
test_focus_client_action(void)
{
	GEnumClass *klass;
	GEnumValue *val;

	klass = (GEnumClass *)g_type_class_ref(GOWL_TYPE_ACTION);
	val = g_enum_get_value_by_nick(klass, "focus-client");
	g_assert_nonnull(val);
	g_assert_cmpint(val->value, ==, GOWL_ACTION_FOCUS_CLIENT);
	g_type_class_unref(klass);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);
	g_log_set_writer_func(expect_warning_writer, NULL, NULL);

	g_test_add_func("/input-remap/rule/names", test_names);
	g_test_add_func("/input-remap/rule/parse-block", test_parse_block);
	g_test_add_func("/input-remap/rule/parse-flow-and-id",
	                test_parse_flow_and_id);
	g_test_add_func("/input-remap/rule/reject-not-one-to-one",
	                test_reject_not_one_to_one);
	g_test_add_func("/input-remap/rule/reject-invalid", test_reject_invalid);
	g_test_add_func("/input-remap/rule/mapping-replaces",
	                test_mapping_replaces);
	g_test_add_func("/input-remap/rule/matching", test_matching);
	g_test_add_func("/input-remap/rule/roundtrip", test_roundtrip);
	g_test_add_func("/input-remap/rule/callback", test_callback);
	g_test_add_func("/input-remap/rule/config-section", test_config_section);
	g_test_add_func("/input-remap/rule/rules-list", test_rules_list);
	g_test_add_func("/input-remap/rule/shipped-example", test_shipped_example);
	g_test_add_func("/input-remap/rule/focus-client-action",
	                test_focus_client_action);

	return g_test_run();
}
