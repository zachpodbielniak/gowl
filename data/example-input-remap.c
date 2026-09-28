/*
 * gowl - Example C configuration: per-device input remapping
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
 * The WoW foot-pedal example (see example-input-remap.yaml) as C config,
 * plus the one thing only C can do: map an input to arbitrary code.
 *
 *   pedal A -> a middle click at the cursor (one press, one click)
 *   pedal B -> a callback: jump to WoW if it is running, launch it if
 *              not.  Nothing is ever sent to the game.
 *
 * Merge add_input_remap() into ~/.config/gowl/config.c and call it from
 * gowl_config_init(); then `gowl --recompile'.  The rules only take
 * effect with the opt-in module enabled -- in YAML:
 *
 *   modules:
 *     inputremap: { enabled: true }
 *
 * A config that maps a callback is kept loaded for the life of the
 * session (the compositor holds a pointer into it), so a reload is safe.
 */

#include <gowl/gowl.h>
#include <linux/input-event-codes.h>

extern GowlCompositor *gowl_compositor;
extern GowlConfig     *gowl_config;

/* The pedal, by vendor:product (hex, from `gowl-msg inputremap-devices') */
#define PEDAL_VENDOR  (0x1a86)
#define PEDAL_PRODUCT (0xe026)

/* How to find WoW, and how to start it when it is not running */
#define WOW_TITLE  "World of Warcraft*"
#define WOW_LAUNCH "lutris lutris:rungame/world-of-warcraft"

/**
 * wow_pedal_b:
 * @rule: the rule the pedal matched
 * @event: the press or release
 * @compositor: the compositor
 * @user_data: unused
 *
 * Runs on press and on release, on the compositor thread.  Acts on the
 * press only.  Arbitrary code: the one-to-one guarantee covers the
 * declarative targets, and a callback is trusted to keep to it itself.
 */
static void
wow_pedal_b(
	GowlInputRemapRule        *rule,
	const GowlInputRemapEvent *event,
	GowlCompositor            *compositor,
	gpointer                   user_data
){
	GowlClient *wow;
	GError     *error;

	(void)rule;
	(void)user_data;
	if (!event->pressed || compositor == NULL)
		return;

	/* Running: view its tag, select its monitor, focus it */
	wow = gowl_compositor_find_client_by_title(compositor, WOW_TITLE);
	if (wow != NULL) {
		gowl_compositor_show_client(compositor, wow);
		return;
	}

	/* Not running: start it */
	error = NULL;
	if (!g_spawn_command_line_async(WOW_LAUNCH, &error)) {
		g_warning("wow pedal: %s", error->message);
		g_error_free(error);
	}
}

/**
 * add_input_remap:
 *
 * Registers the pedal rule with the config.  Same rule as the YAML
 * example's `wow-pedals', with pedal B as a callback.
 */
static void
add_input_remap(void)
{
	GowlInputRemapRule *rule;
	GError             *error;

	rule = gowl_input_remap_rule_new("wow-pedals");
	gowl_input_remap_rule_set_match_ids(rule, PEDAL_VENDOR, PEDAL_PRODUCT);
	gowl_input_remap_rule_set_match_type(rule,
	                                     GOWL_INPUT_REMAP_DEVICE_KEYBOARD);
	gowl_input_remap_rule_set_drop_unmatched(rule, TRUE);
	gowl_input_remap_rule_set_log(rule, TRUE);

	/* pedal A: one middle click */
	error = NULL;
	if (!gowl_input_remap_rule_map_button(rule, KEY_A, BTN_MIDDLE, &error)) {
		g_warning("wow pedal A: %s", error->message);
		g_clear_error(&error);
	}

	/* pedal B: code */
	gowl_input_remap_rule_map_callback(rule, KEY_B, wow_pedal_b, NULL, NULL);

	/*
	 * The declarative alternative for pedal B, without the launch:
	 *
	 *   gowl_input_remap_rule_map_action(rule, KEY_B,
	 *       GOWL_ACTION_FOCUS_CLIENT, "title:" WOW_TITLE, &error);
	 */

	gowl_config_add_input_remap_rule(gowl_config, rule);
	gowl_input_remap_rule_unref(rule);
}

/**
 * gowl_config_init:
 *
 * Returns: %TRUE when the config applied
 */
G_MODULE_EXPORT gboolean
gowl_config_init(void)
{
	add_input_remap();
	return TRUE;
}
