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
 * test-input-remap-pad-rule.c - Game controllers in the rule model.
 *
 * Axis and direction names and their aliases, a bare axis meaning its
 * positive direction under a digital target, the continuous targets and
 * where they are allowed, the virtual-pad targets and their need for a
 * declared pad, the analog settings and their validation, the new match
 * types, and the text round trip -- without a compositor.
 */

#include <glib.h>
#include <linux/input-event-codes.h>
#include <string.h>

#include "gowl.h"

static GowlInputRemapRule *
load_ok(const gchar *yaml)
{
	GowlInputRemapRule *rule;
	GError *error = NULL;

	rule = gowl_input_remap_rule_new_from_yaml(yaml, &error);
	if (error != NULL)
		g_test_message("%s -> %s", yaml, error->message);
	g_assert_no_error(error);
	g_assert_nonnull(rule);
	return rule;
}

static void
load_refused(
	const gchar *yaml,
	gint         code,
	const gchar *fragment
){
	GowlInputRemapRule *rule;
	GError *error = NULL;

	rule = gowl_input_remap_rule_new_from_yaml(yaml, &error);
	g_assert_null(rule);
	g_assert_error(error, GOWL_INPUT_REMAP_ERROR, code);
	if (fragment != NULL && strstr(error->message, fragment) == NULL)
		g_test_message("message: %s", error->message);
	if (fragment != NULL)
		g_assert_nonnull(strstr(error->message, fragment));
	g_error_free(error);
}

static void
test_names(void)
{
	guint32 code;
	g_autofree gchar *n1 = NULL;
	g_autofree gchar *n2 = NULL;
	g_autofree gchar *n3 = NULL;

	g_assert_true(gowl_input_remap_code_from_name("ABS_X+", &code));
	g_assert_cmpuint(code, ==, GOWL_INPUT_REMAP_ABS_INPUT(ABS_X, GOWL_INPUT_REMAP_ABS_POS));
	g_assert_true(gowl_input_remap_code_from_name("abs_ry-", &code));
	g_assert_cmpuint(code, ==, GOWL_INPUT_REMAP_ABS_INPUT(ABS_RY, GOWL_INPUT_REMAP_ABS_NEG));
	g_assert_true(gowl_input_remap_code_from_name("ABS_HAT0X", &code));
	g_assert_cmpuint(code, ==, GOWL_INPUT_REMAP_ABS_INPUT(ABS_HAT0X, GOWL_INPUT_REMAP_ABS_WHOLE));
	g_assert_true(gowl_input_remap_code_from_name("left-stick-up", &code));
	g_assert_cmpuint(code, ==, GOWL_INPUT_REMAP_ABS_INPUT(ABS_Y, GOWL_INPUT_REMAP_ABS_NEG));
	g_assert_true(gowl_input_remap_code_from_name("dpad_left", &code));
	g_assert_cmpuint(code, ==, GOWL_INPUT_REMAP_ABS_INPUT(ABS_HAT0X, GOWL_INPUT_REMAP_ABS_NEG));
	g_assert_true(gowl_input_remap_code_from_name("pad-a", &code));
	g_assert_cmpuint(code, ==, BTN_SOUTH);
	g_assert_true(gowl_input_remap_code_from_name("BTN_SOUTH", &code));
	g_assert_cmpuint(code, ==, BTN_SOUTH);
	g_assert_true(gowl_input_remap_code_from_name("mouse-x", &code));
	g_assert_cmpuint(code, ==, GOWL_INPUT_REMAP_REL_X);
	/* `a' and `x' stay keyboard-free names: not pad aliases */
	g_assert_false(gowl_input_remap_code_from_name("a", &code)
	               && code == BTN_SOUTH);

	n1 = gowl_input_remap_code_to_name(
		GOWL_INPUT_REMAP_ABS_INPUT(ABS_RX, GOWL_INPUT_REMAP_ABS_NEG));
	n2 = gowl_input_remap_code_to_name(
		GOWL_INPUT_REMAP_ABS_INPUT(ABS_Z, GOWL_INPUT_REMAP_ABS_WHOLE));
	n3 = gowl_input_remap_code_to_name(GOWL_INPUT_REMAP_REL_Y);
	g_assert_cmpstr(n1, ==, "ABS_RX-");
	g_assert_cmpstr(n2, ==, "ABS_Z");
	g_assert_cmpstr(n3, ==, "REL_Y");
}

/* Left stick as WASD, a trigger as a click, a button as a key. */
static void
test_stick_to_keys(void)
{
	g_autoptr(GowlInputRemapRule) rule = NULL;
	const GowlInputRemapTarget *t;
	GowlInputDeviceInfo *pad;
	GowlInputRemapAnalog a;

	rule = load_ok(
		"{name: wasd, match: {id: \"045e:028e\", type: gamepad}, grab: true,"
		" analog: {press: 0.6, release: 0.4, deadzone: 0.2, invert: [ABS_Y]},"
		" map: {left-stick-up: {key: KEY_W}, left-stick-left: {key: KEY_A},"
		"       ABS_Y+: {key: KEY_S}, ABS_X+: {key: KEY_D},"
		"       right-trigger: {button: left}, pad-a: {key: KEY_SPACE}}}");
	g_assert_true(gowl_input_remap_rule_get_grab(rule));
	gowl_input_remap_rule_get_analog(rule, &a);
	g_assert_cmpfloat(a.press, ==, 0.6);
	g_assert_cmpfloat(a.release, ==, 0.4);
	g_assert_cmpfloat(a.deadzone, ==, 0.2);
	g_assert_cmpuint(a.invert, ==, (guint64)1 << ABS_Y);

	t = gowl_input_remap_rule_lookup(rule,
		GOWL_INPUT_REMAP_ABS_INPUT(ABS_Y, GOWL_INPUT_REMAP_ABS_NEG));
	g_assert_nonnull(t);
	g_assert_cmpint(gowl_input_remap_target_get_kind(t), ==, GOWL_INPUT_REMAP_TARGET_KEY);
	g_assert_cmpuint(gowl_input_remap_target_get_code(t), ==, KEY_W);
	/* the bare trigger name under a digital target is its + direction */
	t = gowl_input_remap_rule_lookup(rule,
		GOWL_INPUT_REMAP_ABS_INPUT(ABS_RZ, GOWL_INPUT_REMAP_ABS_POS));
	g_assert_nonnull(t);
	g_assert_cmpint(gowl_input_remap_target_get_kind(t), ==, GOWL_INPUT_REMAP_TARGET_BUTTON);
	g_assert_null(gowl_input_remap_rule_lookup(rule,
		GOWL_INPUT_REMAP_ABS_INPUT(ABS_RZ, GOWL_INPUT_REMAP_ABS_WHOLE)));
	g_assert_false(gowl_input_remap_rule_has_continuous(rule));

	/* matching: type gamepad matches a gamepad, not a keyboard */
	pad = gowl_input_device_info_new(1, GOWL_INPUT_REMAP_DEVICE_GAMEPAD,
	                                 "Xbox 360 Pad", "event30", 0x045e, 0x028e);
	g_assert_true(gowl_input_remap_rule_matches(rule, pad));
	pad->type = GOWL_INPUT_REMAP_DEVICE_KEYBOARD;
	g_assert_false(gowl_input_remap_rule_matches(rule, pad));
	gowl_input_device_info_free(pad);
}

/* Right stick as the mouse: continuous, declared, and only on whole axes */
static void
test_continuous(void)
{
	g_autoptr(GowlInputRemapRule) rule = NULL;
	const GowlInputRemapTarget *t;

	rule = load_ok(
		"{name: mouse, match: {name: \"*Pad*\", type: gamepad},"
		" analog: {pointer-speed: 900, pointer-curve: 1.5, scroll-rate: 12},"
		" map: {right-stick-x: {pointer: x}, ABS_RY: {pointer: y},"
		"       ABS_HAT0Y: {scroll: vertical}, pad-a: {button: left}}}");
	t = gowl_input_remap_rule_lookup(rule,
		GOWL_INPUT_REMAP_ABS_INPUT(ABS_RX, GOWL_INPUT_REMAP_ABS_WHOLE));
	g_assert_nonnull(t);
	g_assert_cmpint(gowl_input_remap_target_get_kind(t), ==, GOWL_INPUT_REMAP_TARGET_POINTER);
	g_assert_true(gowl_input_remap_target_is_continuous(t));
	g_assert_true(gowl_input_remap_rule_has_continuous(rule));

	/* a key cannot drive the pointer continuously */
	load_refused("{name: x, match: {name: p}, map: {KEY_A: {pointer: x}}}",
	             GOWL_INPUT_REMAP_ERROR_INVALID, "whole analog axis");
	/* nor can a direction */
	load_refused("{name: x, match: {name: p}, map: {ABS_X+: {scroll: vertical}}}",
	             GOWL_INPUT_REMAP_ERROR_INVALID, "whole analog axis");
	load_refused("{name: x, match: {name: p}, map: {ABS_X: {pointer: z}}}",
	             GOWL_INPUT_REMAP_ERROR_INVALID, "x or y");
}

/* A keypad as a controller: stick, trigger, pad-button targets */
static void
test_virtual_pad_targets(void)
{
	g_autoptr(GowlInputRemapRule) rule = NULL;
	const GowlInputRemapTarget *t;
	g_autoptr(GArray) buttons = NULL;

	rule = load_ok(
		"{name: keypad-pad, match: {id: \"1234:5678\", type: keyboard},"
		" virtual-pad: {name: game},"
		" map: {KEY_KP8: {stick: left, y: -1.0}, KEY_KP2: {stick: left, y: 1},"
		"       KEY_KP4: {stick: left, x: -1}, KEY_KP6: {stick: left, x: 1},"
		"       KEY_KP0: {pad-button: pad-a}, KEY_KPENTER: {trigger: right},"
		"       KEY_KPPLUS: {trigger: left, value: 0.5},"
		"       KEY_KP7: {stick: dpad, x: -1}}}");
	g_assert_cmpstr(gowl_input_remap_rule_get_virtual_pad(rule), ==, "game");
	g_assert_cmpuint(gowl_input_remap_rule_get_virtual_pad_vendor(rule), ==,
	                 GOWL_INPUT_REMAP_PAD_VENDOR);
	buttons = gowl_input_remap_rule_get_virtual_pad_buttons(rule);
	g_assert_cmpuint(buttons->len, >=, 10);

	t = gowl_input_remap_rule_lookup(rule, KEY_KP8);
	g_assert_cmpint(gowl_input_remap_target_get_kind(t), ==, GOWL_INPUT_REMAP_TARGET_STICK);
	g_assert_cmpuint(gowl_input_remap_target_get_side(t), ==, GOWL_INPUT_REMAP_SIDE_LEFT);
	g_assert_cmpuint(gowl_input_remap_target_get_component(t), ==, GOWL_INPUT_REMAP_COMPONENT_Y);
	g_assert_cmpfloat(gowl_input_remap_target_get_value(t), ==, -1.0);
	g_assert_true(gowl_input_remap_target_is_pad(t));
	/* a key held is one held state: inside one-to-one */
	g_assert_false(gowl_input_remap_target_is_continuous(t));
	t = gowl_input_remap_rule_lookup(rule, KEY_KPENTER);
	g_assert_cmpint(gowl_input_remap_target_get_kind(t), ==, GOWL_INPUT_REMAP_TARGET_TRIGGER);
	g_assert_cmpfloat(gowl_input_remap_target_get_value(t), ==, 1.0);
	t = gowl_input_remap_rule_lookup(rule, KEY_KP0);
	g_assert_cmpuint(gowl_input_remap_target_get_code(t), ==, BTN_SOUTH);
}

static void
test_virtual_pad_refusals(void)
{
	/* a stick target needs a declared pad */
	load_refused("{name: x, match: {name: p}, map: {KEY_A: {stick: left, x: 1}}}",
	             GOWL_INPUT_REMAP_ERROR_INVALID, "declares none");
	/* exactly one of x and y: a diagonal is two keys */
	load_refused("{name: x, match: {name: p}, virtual-pad: {name: v},"
	             " map: {KEY_A: {stick: left, x: 1, y: 1}}}",
	             GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE, "exactly one");
	load_refused("{name: x, match: {name: p}, virtual-pad: {name: v},"
	             " map: {KEY_A: {stick: left}}}",
	             GOWL_INPUT_REMAP_ERROR_INVALID, "exactly one");
	/* range */
	load_refused("{name: x, match: {name: p}, virtual-pad: {name: v},"
	             " map: {KEY_A: {stick: left, x: 2}}}",
	             GOWL_INPUT_REMAP_ERROR_INVALID, "[-1, 1]");
	/* a button the pad does not have */
	load_refused("{name: x, match: {name: p}, virtual-pad: {name: v, buttons: [BTN_SOUTH]},"
	             " map: {KEY_A: {pad-button: BTN_EAST}}}",
	             GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE, "no button");
	/* an axis the pad does not have */
	load_refused("{name: x, match: {name: p}, virtual-pad: {name: v, axes: [ABS_X, ABS_Y]},"
	             " map: {KEY_A: {trigger: right}}}",
	             GOWL_INPUT_REMAP_ERROR_INVALID, "no axis");
	/* x/y and value only with their kinds */
	load_refused("{name: x, match: {name: p}, map: {KEY_A: {key: KEY_B, x: 1}}}",
	             GOWL_INPUT_REMAP_ERROR_INVALID, "only go with `stick'");
	/* motion only drives a stick */
	load_refused("{name: x, match: {name: p}, map: {REL_X: {key: KEY_B}}}",
	             GOWL_INPUT_REMAP_ERROR_INVALID, "no press");
	/* the one-to-one refusals still hold for the new shapes */
	load_refused("{name: x, match: {name: p}, virtual-pad: {name: v},"
	             " map: {KEY_A: [{stick: left, x: 1}, {key: KEY_B}]}}",
	             GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE, NULL);
	load_refused("{name: x, match: {name: p}, virtual-pad: {name: v},"
	             " map: {KEY_A: {stick: left, x: 1, repeat: 3}}}",
	             GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE, NULL);
	load_refused("{name: x, match: {name: p}, virtual-pad: {name: v},"
	             " map: {KEY_A: {stick: left, x: 1, key: KEY_B}}}",
	             GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE, NULL);
	/* bad pad names */
	load_refused("{name: x, match: {name: p}, virtual-pad: {name: \"a/b\"}}",
	             GOWL_INPUT_REMAP_ERROR_INVALID, "letters");
}

/* Mouse motion onto a stick: continuous, declared */
static void
test_stick_follow(void)
{
	g_autoptr(GowlInputRemapRule) rule = NULL;
	const GowlInputRemapTarget *t;
	GowlInputRemapAnalog a;

	rule = load_ok(
		"{name: look, match: {name: \"Trackball*\", type: pointer},"
		" virtual-pad: {name: look, id: \"1209:0042\"},"
		" analog: {mouse-gain: 0.05, mouse-idle-ms: 60},"
		" map: {mouse-x: {stick: right}, REL_Y: {stick: right}}}");
	t = gowl_input_remap_rule_lookup(rule, GOWL_INPUT_REMAP_REL_Y);
	g_assert_nonnull(t);
	g_assert_true(gowl_input_remap_target_is_follow(t));
	g_assert_true(gowl_input_remap_target_is_continuous(t));
	g_assert_cmpuint(gowl_input_remap_target_get_component(t), ==, GOWL_INPUT_REMAP_COMPONENT_Y);
	g_assert_cmpuint(gowl_input_remap_rule_get_virtual_pad_product(rule), ==, 0x0042);
	gowl_input_remap_rule_get_analog(rule, &a);
	g_assert_cmpfloat(a.mouse_gain, ==, 0.05);
	g_assert_cmpuint(a.mouse_idle_ms, ==, 60);
}

static void
test_analog_validation(void)
{
	load_refused("{name: x, match: {name: p}, analog: {press: 0.3}}",
	             GOWL_INPUT_REMAP_ERROR_INVALID, "release");
	load_refused("{name: x, match: {name: p}, analog: {deadzone: 0.99}}",
	             GOWL_INPUT_REMAP_ERROR_INVALID, "deadzone");
	load_refused("{name: x, match: {name: p}, analog: {pointer-curve: 9}}",
	             GOWL_INPUT_REMAP_ERROR_INVALID, "pointer-curve");
	load_refused("{name: x, match: {name: p}, analog: {invert: [ABS_NOPE]}}",
	             GOWL_INPUT_REMAP_ERROR_UNKNOWN_CODE, NULL);
	load_refused("{name: x, match: {name: p}, analog: {wobble: 1}}",
	             GOWL_INPUT_REMAP_ERROR_INVALID, "unknown analog key");
	/* a forbidden key is refused as one-to-one, wherever it hides */
	load_refused("{name: x, match: {name: p}, analog: {deadzone: 0.1}, delay: 5}",
	             GOWL_INPUT_REMAP_ERROR_NOT_ONE_TO_ONE, NULL);
}

/* Everything written is read back the same */
static void
test_round_trip(void)
{
	g_autoptr(GowlInputRemapRule) a = NULL;
	g_autoptr(GowlInputRemapRule) b = NULL;
	g_autofree gchar *ya = NULL;
	g_autofree gchar *yb = NULL;

	a = load_ok(
		"{name: all, match: {id: \"045e:028e\", type: gamepad}, grab: true,"
		" analog: {press: 0.7, release: 0.2, invert: [ABS_Y, ABS_RY],"
		"          centred: [ABS_Z], pointer-speed: 2000},"
		" virtual-pad: {name: out, id: \"1209:0010\", buttons: [BTN_SOUTH, BTN_EAST],"
		"               axes: [ABS_X, ABS_Y, ABS_RZ]},"
		" map: {ABS_X-: {key: KEY_A}, ABS_RX: {pointer: x},"
		"       ABS_RY: {scroll: horizontal}, BTN_EAST: {stick: left, x: -0.5},"
		"       BTN_NORTH: {trigger: right, value: 0.25},"
		"       BTN_WEST: {pad-button: BTN_SOUTH}}}");
	ya = gowl_input_remap_rule_to_yaml(a);
	b = load_ok(ya);
	yb = gowl_input_remap_rule_to_yaml(b);
	g_assert_cmpstr(ya, ==, yb);
	g_assert_nonnull(strstr(ya, "type: gamepad"));
	g_assert_nonnull(strstr(ya, "grab: true"));
	g_assert_nonnull(strstr(ya, "ABS_X-: {key: \"KEY_A\"}"));
	g_assert_nonnull(strstr(ya, "invert: [ABS_Y, ABS_RY]"));
}

/* The worked examples of docs/input-remap.org, as one config list:
 * what the page shows must load. */
static void
test_docs_examples(void)
{
	g_autoptr(GPtrArray) rules = NULL;
	GError *error = NULL;

	rules = gowl_input_remap_rules_from_yaml(
		"- name: stick-wasd\n"
		"  match: { id: \"045e:028e\", type: gamepad }\n"
		"  grab: true\n"
		"  analog: { press: 0.5, release: 0.3 }\n"
		"  map:\n"
		"    left-stick-up:    { key: KEY_W }\n"
		"    left-stick-left:  { key: KEY_A }\n"
		"    left-stick-down:  { key: KEY_S }\n"
		"    left-stick-right: { key: KEY_D }\n"
		"    pad-a:            { key: KEY_SPACE }\n"
		"    right-trigger:    { button: left }\n"
		"- name: pad-mouse\n"
		"  match: { id: \"045e:028e\", type: gamepad }\n"
		"  analog: { pointer-speed: 1500, pointer-curve: 2.0, scroll-rate: 12 }\n"
		"  map:\n"
		"    right-stick-x: { pointer: x }\n"
		"    right-stick-y: { pointer: y }\n"
		"    left-stick-y:  { scroll: vertical }\n"
		"    pad-a:         { button: left }\n"
		"    pad-b:         { button: right }\n"
		"- name: keypad-pad\n"
		"  match: { id: \"1234:5678\", type: keyboard }\n"
		"  unmatched: drop\n"
		"  virtual-pad: { name: keypad }\n"
		"  map:\n"
		"    KEY_KP8:     { stick: left, y: -1 }\n"
		"    KEY_KP2:     { stick: left, y: 1 }\n"
		"    KEY_KP4:     { stick: left, x: -1 }\n"
		"    KEY_KP6:     { stick: left, x: 1 }\n"
		"    KEY_KP0:     { pad-button: BTN_SOUTH }\n"
		"    KEY_KPENTER: { trigger: right }\n"
		"- name: trackball-look\n"
		"  match: { name: \"*Trackball*\", type: pointer }\n"
		"  virtual-pad: { name: look }\n"
		"  analog: { mouse-gain: 0.03, mouse-idle-ms: 80 }\n"
		"  map:\n"
		"    mouse-x: { stick: right }\n"
		"    mouse-y: { stick: right }\n", &error);
	g_assert_no_error(error);
	g_assert_nonnull(rules);
	g_assert_cmpuint(rules->len, ==, 4);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/input-remap/pad-rule/names", test_names);
	g_test_add_func("/input-remap/pad-rule/stick-to-keys", test_stick_to_keys);
	g_test_add_func("/input-remap/pad-rule/continuous", test_continuous);
	g_test_add_func("/input-remap/pad-rule/virtual-pad-targets",
	                test_virtual_pad_targets);
	g_test_add_func("/input-remap/pad-rule/virtual-pad-refusals",
	                test_virtual_pad_refusals);
	g_test_add_func("/input-remap/pad-rule/stick-follow", test_stick_follow);
	g_test_add_func("/input-remap/pad-rule/analog-validation",
	                test_analog_validation);
	g_test_add_func("/input-remap/pad-rule/round-trip", test_round_trip);
	g_test_add_func("/input-remap/pad-rule/docs-examples", test_docs_examples);
	return g_test_run();
}
