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
 * gowl-mcp-tools-input.c - Synthetic input MCP tools.
 *
 * Implements:
 *   - send_key        : Send a named key press/release to the focused client
 *   - send_text       : Type a string character-by-character
 *   - send_mouse      : Send a mouse button event
 *   - send_mouse_move : Move the cursor to absolute coordinates
 *   - send_scroll     : Send a scroll (axis) event
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-mcp"

#include "gowl-module-mcp.h"
#include "gowl-mcp-dispatch.h"
#include "gowl-mcp-tools.h"

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"

#include <json-glib/json-glib.h>
#include <linux/input-event-codes.h>
#include <time.h>

/* ========================================================================== */
/* Helpers                                                                    */
/* ========================================================================== */

/**
 * get_time_ms:
 *
 * Returns the current CLOCK_MONOTONIC time in milliseconds.
 * Used as the timestamp for synthetic input events.
 */
static guint32
get_time_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (guint32)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

/**
 * keysym_to_keycode:
 * @keymap: the xkb keymap to search
 * @keysym: the keysym to find
 * @out_layout: (out) (optional): layout index where the keysym was found
 * @out_level: (out) (optional): level index where the keysym was found
 *
 * Reverse-lookup: finds the first keycode in the keymap that
 * produces @keysym at any layout/level.  Returns the xkb keycode
 * (evdev + 8), or 0 if not found.  When non-NULL, @out_layout and
 * @out_level are set to the layout and level at which the match
 * was found (useful for determining required modifiers).
 */
static xkb_keycode_t
keysym_to_keycode(
	struct xkb_keymap  *keymap,
	xkb_keysym_t        keysym,
	xkb_layout_index_t *out_layout,
	xkb_level_index_t  *out_level
){
	xkb_keycode_t min_kc;
	xkb_keycode_t max_kc;
	xkb_keycode_t kc;

	min_kc = xkb_keymap_min_keycode(keymap);
	max_kc = xkb_keymap_max_keycode(keymap);

	for (kc = min_kc; kc <= max_kc; kc++) {
		xkb_layout_index_t num_layouts;
		xkb_layout_index_t layout;

		num_layouts = xkb_keymap_num_layouts_for_key(keymap, kc);
		for (layout = 0; layout < num_layouts; layout++) {
			xkb_level_index_t num_levels;
			xkb_level_index_t level;

			num_levels = xkb_keymap_num_levels_for_key(
				keymap, kc, layout);
			for (level = 0; level < num_levels; level++) {
				const xkb_keysym_t *syms;
				int nsyms;
				int i;

				nsyms = xkb_keymap_key_get_syms_by_level(
					keymap, kc, layout, level, &syms);
				for (i = 0; i < nsyms; i++) {
					if (syms[i] == keysym) {
						if (out_layout != NULL)
							*out_layout = layout;
						if (out_level != NULL)
							*out_level = level;
						return kc;
					}
				}
			}
		}
	}

	if (out_layout != NULL)
		*out_layout = 0;
	if (out_level != NULL)
		*out_level = 0;
	return 0;
}

/**
 * parse_button_name:
 * @name: button name ("left", "right", "middle", or numeric code)
 *
 * Converts a button name to a Linux input event code.
 * Returns 0 on failure.
 */
static guint32
parse_button_name(const gchar *name)
{
	if (g_ascii_strcasecmp(name, "left") == 0)
		return BTN_LEFT;
	if (g_ascii_strcasecmp(name, "right") == 0)
		return BTN_RIGHT;
	if (g_ascii_strcasecmp(name, "middle") == 0)
		return BTN_MIDDLE;

	/* Try numeric */
	{
		gchar *end;
		gulong val;

		val = strtoul(name, &end, 0);
		if (end != name && *end == '\0' && val > 0)
			return (guint32)val;
	}

	return 0;
}

/* ========================================================================== */
/* Tool: send_key                                                             */
/* ========================================================================== */

/**
 * tool_send_key:
 *
 * Sends a key press followed by a release to the currently focused
 * client.  The key is specified by its XKB name (e.g. "Return",
 * "space", "a", "Escape").
 */
static McpToolResult *
tool_send_key(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	McpToolResult *result;
	const gchar *key_name;
	xkb_keysym_t keysym;
	xkb_keycode_t xkb_kc;
	guint32 evdev_kc;
	guint32 time_ms;
	struct wlr_keyboard *kb;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "key"))
	{
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result,
			"Missing required parameter: key");
		return result;
	}

	key_name = json_object_get_string_member(arguments, "key");

	/* Convert key name to keysym */
	keysym = xkb_keysym_from_name(key_name,
		XKB_KEYSYM_CASE_INSENSITIVE);
	if (keysym == XKB_KEY_NoSymbol) {
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result,
			"Unknown key name");
		return result;
	}

	/* Find the keycode in the current keymap */
	kb = &module->compositor->wlr_kb_group->keyboard;
	xkb_kc = keysym_to_keycode(kb->keymap, keysym, NULL, NULL);
	if (xkb_kc == 0) {
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result,
			"Key not found in current keymap");
		return result;
	}

	/* xkb keycode = evdev keycode + 8 */
	evdev_kc = xkb_kc - 8;
	time_ms = get_time_ms();

	/* Ensure the keyboard is set on the seat */
	wlr_seat_set_keyboard(module->compositor->wlr_seat, kb);

	/* Press and release */
	wlr_seat_keyboard_notify_key(module->compositor->wlr_seat,
		time_ms, evdev_kc, WL_KEYBOARD_KEY_STATE_PRESSED);
	wlr_seat_keyboard_notify_key(module->compositor->wlr_seat,
		time_ms + 1, evdev_kc, WL_KEYBOARD_KEY_STATE_RELEASED);

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, "Key sent");
	return result;
}

static McpToolResult *
handle_send_key(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_send_key, arguments, NULL);
}

/* ========================================================================== */
/* Tool: send_text                                                            */
/* ========================================================================== */

/**
 * tool_send_text:
 *
 * Types a string character-by-character.  Each Unicode codepoint
 * is converted to a keysym, looked up in the keymap, and sent as
 * a key press/release pair.  Characters that live on a higher
 * keymap level (e.g. Shift for uppercase letters, symbols like
 * '(', ')', ':') have the required modifier state sent to the
 * client before the key event and restored afterwards.
 * Characters that cannot be mapped are skipped.
 */
static McpToolResult *
tool_send_text(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	McpToolResult *result;
	const gchar *text;
	const gchar *p;
	struct wlr_keyboard *kb;
	struct wlr_keyboard_modifiers orig_mods;
	guint32 time_ms;
	gint sent;
	gint skipped;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "text"))
	{
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result,
			"Missing required parameter: text");
		return result;
	}

	text = json_object_get_string_member(arguments, "text");
	kb = &module->compositor->wlr_kb_group->keyboard;
	time_ms = get_time_ms();
	sent = 0;
	skipped = 0;

	/* Save original modifier state to restore between characters */
	orig_mods = kb->modifiers;

	wlr_seat_set_keyboard(module->compositor->wlr_seat, kb);

	for (p = text; *p != '\0'; ) {
		gunichar uc;
		xkb_keysym_t keysym;
		xkb_keycode_t xkb_kc;
		xkb_layout_index_t layout;
		xkb_level_index_t level;
		guint32 evdev_kc;
		gboolean mods_changed;

		uc = g_utf8_get_char(p);
		p = g_utf8_next_char(p);

		/* Convert Unicode codepoint to keysym */
		keysym = xkb_utf32_to_keysym((uint32_t)uc);
		if (keysym == XKB_KEY_NoSymbol) {
			skipped++;
			continue;
		}

		/* Find keycode and level in keymap */
		xkb_kc = keysym_to_keycode(kb->keymap, keysym,
			&layout, &level);
		if (xkb_kc == 0) {
			skipped++;
			continue;
		}

		evdev_kc = xkb_kc - 8;
		mods_changed = FALSE;

		/*
		 * If the keysym lives on a level that requires modifiers
		 * (e.g. level 1 = Shift for uppercase and symbols like
		 * '(', ')', ':', '!', '@', etc.), query the keymap for
		 * the modifier mask needed to reach that level and send
		 * the modifier state to the client before the key event.
		 */
		if (level > 0) {
			xkb_mod_mask_t masks[8];
			size_t nmasks;

			nmasks = xkb_keymap_key_get_mods_for_level(
				kb->keymap, xkb_kc, layout, level,
				masks, G_N_ELEMENTS(masks));
			if (nmasks > 0) {
				struct wlr_keyboard_modifiers mods;

				mods = orig_mods;
				mods.depressed |= masks[0];
				wlr_seat_keyboard_notify_modifiers(
					module->compositor->wlr_seat, &mods);
				mods_changed = TRUE;
			}
		}

		wlr_seat_keyboard_notify_key(
			module->compositor->wlr_seat,
			time_ms, evdev_kc,
			WL_KEYBOARD_KEY_STATE_PRESSED);
		wlr_seat_keyboard_notify_key(
			module->compositor->wlr_seat,
			time_ms + 1, evdev_kc,
			WL_KEYBOARD_KEY_STATE_RELEASED);

		/* Restore original modifier state if we changed it */
		if (mods_changed) {
			wlr_seat_keyboard_notify_modifiers(
				module->compositor->wlr_seat, &orig_mods);
		}

		time_ms += 2;
		sent++;
	}

	{
		g_autofree gchar *msg = NULL;

		msg = g_strdup_printf(
			"Typed %d characters (%d skipped)", sent, skipped);
		result = mcp_tool_result_new(FALSE);
		mcp_tool_result_add_text(result, msg);
	}

	return result;
}

static McpToolResult *
handle_send_text(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_send_text, arguments, NULL);
}

/* ========================================================================== */
/* Tool: send_mouse                                                           */
/* ========================================================================== */

/**
 * tool_send_mouse:
 *
 * Sends a mouse button press and/or release.  The action can be
 * "press", "release", or "click" (press + release).
 */
static McpToolResult *
tool_send_mouse(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	McpToolResult *result;
	const gchar *button_name;
	const gchar *action;
	guint32 button;
	guint32 time_ms;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "button"))
	{
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result,
			"Missing required parameter: button");
		return result;
	}

	button_name = json_object_get_string_member(arguments, "button");
	button = parse_button_name(button_name);
	if (button == 0) {
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result,
			"Unknown button. Use: left, right, middle, "
			"or a numeric code.");
		return result;
	}

	/* Default action is "click" */
	action = "click";
	if (json_object_has_member(arguments, "action"))
		action = json_object_get_string_member(arguments, "action");

	time_ms = get_time_ms();

	if (g_ascii_strcasecmp(action, "press") == 0) {
		wlr_seat_pointer_notify_button(
			module->compositor->wlr_seat,
			time_ms, button,
			WL_POINTER_BUTTON_STATE_PRESSED);
	} else if (g_ascii_strcasecmp(action, "release") == 0) {
		wlr_seat_pointer_notify_button(
			module->compositor->wlr_seat,
			time_ms, button,
			WL_POINTER_BUTTON_STATE_RELEASED);
	} else {
		/* click = press + release */
		wlr_seat_pointer_notify_button(
			module->compositor->wlr_seat,
			time_ms, button,
			WL_POINTER_BUTTON_STATE_PRESSED);
		wlr_seat_pointer_notify_button(
			module->compositor->wlr_seat,
			time_ms + 1, button,
			WL_POINTER_BUTTON_STATE_RELEASED);
	}

	wlr_seat_pointer_notify_frame(module->compositor->wlr_seat);

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, "Mouse event sent");
	return result;
}

static McpToolResult *
handle_send_mouse(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_send_mouse, arguments, NULL);
}

/* ========================================================================== */
/* Tool: send_mouse_move                                                      */
/* ========================================================================== */

/**
 * tool_send_mouse_move:
 *
 * Moves the cursor to absolute layout coordinates (x, y) and
 * updates pointer focus accordingly.
 */
static McpToolResult *
tool_send_mouse_move(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	McpToolResult *result;
	gdouble x, y;
	guint32 time_ms;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "x") ||
	    !json_object_has_member(arguments, "y"))
	{
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result,
			"Missing required parameters: x, y");
		return result;
	}

	x = json_object_get_double_member(arguments, "x");
	y = json_object_get_double_member(arguments, "y");
	time_ms = get_time_ms();

	/*
	 * Warp the cursor to the requested position and trigger
	 * the compositor's motion handling (focus updates, etc.).
	 */
	wlr_cursor_warp_closest(module->compositor->wlr_cursor,
		NULL, x, y);
	gowl_compositor_motionnotify(module->compositor, time_ms);

	{
		g_autofree gchar *msg = NULL;

		msg = g_strdup_printf(
			"Cursor moved to (%.0f, %.0f)", x, y);
		result = mcp_tool_result_new(FALSE);
		mcp_tool_result_add_text(result, msg);
	}

	return result;
}

static McpToolResult *
handle_send_mouse_move(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_send_mouse_move, arguments, NULL);
}

/* ========================================================================== */
/* Tool: send_scroll                                                          */
/* ========================================================================== */

/**
 * tool_send_scroll:
 *
 * Sends a scroll (axis) event.  Specify direction and optional
 * amount (default 1 step = 15px / 120 high-res units).
 */
static McpToolResult *
tool_send_scroll(
	GowlModuleMcp *module,
	JsonObject    *arguments,
	gpointer       user_data
){
	McpToolResult *result;
	const gchar *direction;
	gdouble amount;
	guint32 time_ms;
	enum wl_pointer_axis orientation;
	gdouble delta;
	gint32 delta_discrete;

	if (arguments == NULL ||
	    !json_object_has_member(arguments, "direction"))
	{
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result,
			"Missing required parameter: direction");
		return result;
	}

	direction = json_object_get_string_member(arguments, "direction");

	/* Amount in scroll steps (default 1) */
	amount = 1.0;
	if (json_object_has_member(arguments, "amount"))
		amount = json_object_get_double_member(arguments, "amount");

	if (g_ascii_strcasecmp(direction, "up") == 0) {
		orientation = WL_POINTER_AXIS_VERTICAL_SCROLL;
		delta = -15.0 * amount;
	} else if (g_ascii_strcasecmp(direction, "down") == 0) {
		orientation = WL_POINTER_AXIS_VERTICAL_SCROLL;
		delta = 15.0 * amount;
	} else if (g_ascii_strcasecmp(direction, "left") == 0) {
		orientation = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
		delta = -15.0 * amount;
	} else if (g_ascii_strcasecmp(direction, "right") == 0) {
		orientation = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
		delta = 15.0 * amount;
	} else {
		result = mcp_tool_result_new(TRUE);
		mcp_tool_result_add_text(result,
			"Invalid direction. Use: up, down, left, right");
		return result;
	}

	/* 120 high-res units per discrete step */
	delta_discrete = (gint32)(120.0 * amount);
	if (delta < 0)
		delta_discrete = -delta_discrete;

	time_ms = get_time_ms();

	wlr_seat_pointer_notify_axis(module->compositor->wlr_seat,
		time_ms, orientation, delta, delta_discrete,
		WL_POINTER_AXIS_SOURCE_WHEEL,
		WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
	wlr_seat_pointer_notify_frame(module->compositor->wlr_seat);

	result = mcp_tool_result_new(FALSE);
	mcp_tool_result_add_text(result, "Scroll event sent");
	return result;
}

static McpToolResult *
handle_send_scroll(
	McpServer   *server,
	const gchar *name,
	JsonObject  *arguments,
	gpointer     user_data
){
	return gowl_mcp_dispatch_call(
		(GowlModuleMcp *)user_data,
		tool_send_scroll, arguments, NULL);
}

/* ========================================================================== */
/* Registration                                                               */
/* ========================================================================== */

void
gowl_mcp_register_input_tools(
	McpServer     *server,
	GowlModuleMcp *module
){
	g_return_if_fail(server != NULL);
	g_return_if_fail(module != NULL);

	/* send_key */
	if (gowl_module_mcp_is_tool_allowed(module, "send_key")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("send_key",
			"Send a key press and release to the focused window. "
			"Use XKB key names: Return, space, a, Escape, "
			"Tab, BackSpace, etc.");
		mcp_tool_set_destructive_hint(tool, TRUE);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "key");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"XKB key name (e.g. Return, space, a, "
			"Escape, F1, BackSpace)");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "key");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_send_key,
		                    module, NULL);
	}

	/* send_text */
	if (gowl_module_mcp_is_tool_allowed(module, "send_text")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("send_text",
			"Type a string character-by-character to the "
			"focused window. Characters not in the current "
			"keymap are skipped.");
		mcp_tool_set_destructive_hint(tool, TRUE);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "text");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Text string to type (UTF-8)");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "text");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_send_text,
		                    module, NULL);
	}

	/* send_mouse */
	if (gowl_module_mcp_is_tool_allowed(module, "send_mouse")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("send_mouse",
			"Send a mouse button event. Default action is "
			"click (press + release).");
		mcp_tool_set_destructive_hint(tool, TRUE);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "button");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Button name: left, right, middle, "
			"or numeric code");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "action");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Action: press, release, or click "
			"(default: click)");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "button");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_send_mouse,
		                    module, NULL);
	}

	/* send_mouse_move */
	if (gowl_module_mcp_is_tool_allowed(module, "send_mouse_move")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("send_mouse_move",
			"Move the cursor to absolute layout coordinates. "
			"Updates pointer focus automatically.");
		mcp_tool_set_destructive_hint(tool, TRUE);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "x");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "number");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"X coordinate (layout-absolute pixels)");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "y");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "number");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Y coordinate (layout-absolute pixels)");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "x");
		json_builder_add_string_value(b, "y");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_send_mouse_move,
		                    module, NULL);
	}

	/* send_scroll */
	if (gowl_module_mcp_is_tool_allowed(module, "send_scroll")) {
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) schema = NULL;

		tool = mcp_tool_new("send_scroll",
			"Send a scroll event to the focused window.");
		mcp_tool_set_destructive_hint(tool, TRUE);

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "object");
		json_builder_set_member_name(b, "properties");
		json_builder_begin_object(b);

		json_builder_set_member_name(b, "direction");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "string");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Scroll direction: up, down, left, right");
		json_builder_end_object(b);

		json_builder_set_member_name(b, "amount");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "number");
		json_builder_set_member_name(b, "description");
		json_builder_add_string_value(b,
			"Number of scroll steps (default: 1)");
		json_builder_end_object(b);

		json_builder_end_object(b);
		json_builder_set_member_name(b, "required");
		json_builder_begin_array(b);
		json_builder_add_string_value(b, "direction");
		json_builder_end_array(b);
		json_builder_end_object(b);

		schema = json_builder_get_root(b);
		mcp_tool_set_input_schema(tool, schema);

		mcp_server_add_tool(server, tool, handle_send_scroll,
		                    module, NULL);
	}

	g_debug("input tools registered");
}
