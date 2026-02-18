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

#include "gowl-enums.h"

/*
 * Suppress -Wdiscarded-qualifiers caused by GLib's g_once_init_enter()
 * macro using __atomic_load on a volatile gsize pointer. This is a known
 * GLib header issue (not our code) and is harmless.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"

/* --- GowlHookPoint GType registration --- */

/**
 * gowl_hook_point_get_type:
 *
 * Retrieves the #GType for #GowlHookPoint.
 *
 * Returns: the #GType for #GowlHookPoint
 */
GType
gowl_hook_point_get_type(void)
{
	static volatile gsize g_type_id = 0;

	if (g_once_init_enter(&g_type_id)) {
		static const GEnumValue values[] = {
			{ GOWL_HOOK_POINT_STARTUP,                  "GOWL_HOOK_POINT_STARTUP",                  "startup" },
			{ GOWL_HOOK_POINT_SHUTDOWN,                  "GOWL_HOOK_POINT_SHUTDOWN",                  "shutdown" },
			{ GOWL_HOOK_POINT_CONFIG_RELOAD,             "GOWL_HOOK_POINT_CONFIG_RELOAD",             "config-reload" },
			{ GOWL_HOOK_POINT_KEY_PRESS,                 "GOWL_HOOK_POINT_KEY_PRESS",                 "key-press" },
			{ GOWL_HOOK_POINT_KEY_RELEASE,               "GOWL_HOOK_POINT_KEY_RELEASE",               "key-release" },
			{ GOWL_HOOK_POINT_BUTTON_PRESS,              "GOWL_HOOK_POINT_BUTTON_PRESS",              "button-press" },
			{ GOWL_HOOK_POINT_BUTTON_RELEASE,            "GOWL_HOOK_POINT_BUTTON_RELEASE",            "button-release" },
			{ GOWL_HOOK_POINT_POINTER_MOTION,            "GOWL_HOOK_POINT_POINTER_MOTION",            "pointer-motion" },
			{ GOWL_HOOK_POINT_POINTER_AXIS,              "GOWL_HOOK_POINT_POINTER_AXIS",              "pointer-axis" },
			{ GOWL_HOOK_POINT_CLIENT_NEW,                "GOWL_HOOK_POINT_CLIENT_NEW",                "client-new" },
			{ GOWL_HOOK_POINT_CLIENT_MAP,                "GOWL_HOOK_POINT_CLIENT_MAP",                "client-map" },
			{ GOWL_HOOK_POINT_CLIENT_UNMAP,              "GOWL_HOOK_POINT_CLIENT_UNMAP",              "client-unmap" },
			{ GOWL_HOOK_POINT_CLIENT_DESTROY,            "GOWL_HOOK_POINT_CLIENT_DESTROY",            "client-destroy" },
			{ GOWL_HOOK_POINT_CLIENT_FOCUS,              "GOWL_HOOK_POINT_CLIENT_FOCUS",              "client-focus" },
			{ GOWL_HOOK_POINT_CLIENT_UNFOCUS,            "GOWL_HOOK_POINT_CLIENT_UNFOCUS",            "client-unfocus" },
			{ GOWL_HOOK_POINT_CLIENT_SET_TITLE,          "GOWL_HOOK_POINT_CLIENT_SET_TITLE",          "client-set-title" },
			{ GOWL_HOOK_POINT_CLIENT_SET_APP_ID,         "GOWL_HOOK_POINT_CLIENT_SET_APP_ID",         "client-set-app-id" },
			{ GOWL_HOOK_POINT_CLIENT_REQUEST_FULLSCREEN, "GOWL_HOOK_POINT_CLIENT_REQUEST_FULLSCREEN", "client-request-fullscreen" },
			{ GOWL_HOOK_POINT_LAYOUT_ARRANGE,            "GOWL_HOOK_POINT_LAYOUT_ARRANGE",            "layout-arrange" },
			{ GOWL_HOOK_POINT_LAYOUT_CHANGE,             "GOWL_HOOK_POINT_LAYOUT_CHANGE",             "layout-change" },
			{ GOWL_HOOK_POINT_TAG_VIEW,                  "GOWL_HOOK_POINT_TAG_VIEW",                  "tag-view" },
			{ GOWL_HOOK_POINT_TAG_CHANGE,                "GOWL_HOOK_POINT_TAG_CHANGE",                "tag-change" },
			{ GOWL_HOOK_POINT_MONITOR_NEW,               "GOWL_HOOK_POINT_MONITOR_NEW",               "monitor-new" },
			{ GOWL_HOOK_POINT_MONITOR_DESTROY,           "GOWL_HOOK_POINT_MONITOR_DESTROY",           "monitor-destroy" },
			{ GOWL_HOOK_POINT_MONITOR_FRAME,             "GOWL_HOOK_POINT_MONITOR_FRAME",             "monitor-frame" },
			{ GOWL_HOOK_POINT_FOCUS_STACK,               "GOWL_HOOK_POINT_FOCUS_STACK",               "focus-stack" },
			{ GOWL_HOOK_POINT_FOCUS_MONITOR,             "GOWL_HOOK_POINT_FOCUS_MONITOR",             "focus-monitor" },
			{ GOWL_HOOK_POINT_CLIENT_PLACE,              "GOWL_HOOK_POINT_CLIENT_PLACE",              "client-place" },
			{ GOWL_HOOK_POINT_CLIENT_DECORATE,           "GOWL_HOOK_POINT_CLIENT_DECORATE",           "client-decorate" },
			{ GOWL_HOOK_POINT_CLIENT_RULE_MATCH,         "GOWL_HOOK_POINT_CLIENT_RULE_MATCH",         "client-rule-match" },
			{ GOWL_HOOK_POINT_IDLE,                      "GOWL_HOOK_POINT_IDLE",                      "idle" },
			{ GOWL_HOOK_POINT_RESUME,                    "GOWL_HOOK_POINT_RESUME",                    "resume" },
			{ GOWL_HOOK_POINT_LOCK,                      "GOWL_HOOK_POINT_LOCK",                      "lock" },
			{ GOWL_HOOK_POINT_UNLOCK,                    "GOWL_HOOK_POINT_UNLOCK",                    "unlock" },
			{ GOWL_HOOK_POINT_IPC_COMMAND,               "GOWL_HOOK_POINT_IPC_COMMAND",               "ipc-command" },
			{ GOWL_HOOK_POINT_LAYER_NEW,                 "GOWL_HOOK_POINT_LAYER_NEW",                 "layer-new" },
			{ GOWL_HOOK_POINT_LAYER_MAP,                 "GOWL_HOOK_POINT_LAYER_MAP",                 "layer-map" },
			{ GOWL_HOOK_POINT_LAYER_UNMAP,               "GOWL_HOOK_POINT_LAYER_UNMAP",               "layer-unmap" },
			{ GOWL_HOOK_POINT_LAYER_DESTROY,             "GOWL_HOOK_POINT_LAYER_DESTROY",             "layer-destroy" },
			{ GOWL_HOOK_POINT_BAR_RENDER,                "GOWL_HOOK_POINT_BAR_RENDER",                "bar-render" },
			{ GOWL_HOOK_POINT_LAST,                      "GOWL_HOOK_POINT_LAST",                      "last" },
			{ 0, NULL, NULL }
		};
		GType type_id = g_enum_register_static("GowlHookPoint", values);
		g_once_init_leave(&g_type_id, type_id);
	}

	return (GType)g_type_id;
}

/* --- GowlCursorMode GType registration --- */

/**
 * gowl_cursor_mode_get_type:
 *
 * Retrieves the #GType for #GowlCursorMode.
 *
 * Returns: the #GType for #GowlCursorMode
 */
GType
gowl_cursor_mode_get_type(void)
{
	static volatile gsize g_type_id = 0;

	if (g_once_init_enter(&g_type_id)) {
		static const GEnumValue values[] = {
			{ GOWL_CURSOR_MODE_NORMAL,  "GOWL_CURSOR_MODE_NORMAL",  "normal" },
			{ GOWL_CURSOR_MODE_PRESSED, "GOWL_CURSOR_MODE_PRESSED", "pressed" },
			{ GOWL_CURSOR_MODE_MOVE,    "GOWL_CURSOR_MODE_MOVE",    "move" },
			{ GOWL_CURSOR_MODE_RESIZE,  "GOWL_CURSOR_MODE_RESIZE",  "resize" },
			{ 0, NULL, NULL }
		};
		GType type_id = g_enum_register_static("GowlCursorMode", values);
		g_once_init_leave(&g_type_id, type_id);
	}

	return (GType)g_type_id;
}

/* --- GowlDirection GType registration --- */

/**
 * gowl_direction_get_type:
 *
 * Retrieves the #GType for #GowlDirection.
 *
 * Returns: the #GType for #GowlDirection
 */
GType
gowl_direction_get_type(void)
{
	static volatile gsize g_type_id = 0;

	if (g_once_init_enter(&g_type_id)) {
		static const GEnumValue values[] = {
			{ GOWL_DIRECTION_UP,    "GOWL_DIRECTION_UP",    "up" },
			{ GOWL_DIRECTION_DOWN,  "GOWL_DIRECTION_DOWN",  "down" },
			{ GOWL_DIRECTION_LEFT,  "GOWL_DIRECTION_LEFT",  "left" },
			{ GOWL_DIRECTION_RIGHT, "GOWL_DIRECTION_RIGHT", "right" },
			{ 0, NULL, NULL }
		};
		GType type_id = g_enum_register_static("GowlDirection", values);
		g_once_init_leave(&g_type_id, type_id);
	}

	return (GType)g_type_id;
}

/* --- GowlLayerShellLayer GType registration --- */

/**
 * gowl_layer_shell_layer_get_type:
 *
 * Retrieves the #GType for #GowlLayerShellLayer.
 *
 * Returns: the #GType for #GowlLayerShellLayer
 */
GType
gowl_layer_shell_layer_get_type(void)
{
	static volatile gsize g_type_id = 0;

	if (g_once_init_enter(&g_type_id)) {
		static const GEnumValue values[] = {
			{ GOWL_LAYER_SHELL_LAYER_BACKGROUND, "GOWL_LAYER_SHELL_LAYER_BACKGROUND", "background" },
			{ GOWL_LAYER_SHELL_LAYER_BOTTOM,     "GOWL_LAYER_SHELL_LAYER_BOTTOM",     "bottom" },
			{ GOWL_LAYER_SHELL_LAYER_TOP,        "GOWL_LAYER_SHELL_LAYER_TOP",        "top" },
			{ GOWL_LAYER_SHELL_LAYER_OVERLAY,    "GOWL_LAYER_SHELL_LAYER_OVERLAY",    "overlay" },
			{ 0, NULL, NULL }
		};
		GType type_id = g_enum_register_static("GowlLayerShellLayer", values);
		g_once_init_leave(&g_type_id, type_id);
	}

	return (GType)g_type_id;
}

/* --- GowlKeyMod GType registration (flags) --- */

/**
 * gowl_key_mod_get_type:
 *
 * Retrieves the #GType for #GowlKeyMod flags.
 *
 * Returns: the #GType for #GowlKeyMod
 */
GType
gowl_key_mod_get_type(void)
{
	static volatile gsize g_type_id = 0;

	if (g_once_init_enter(&g_type_id)) {
		static const GFlagsValue values[] = {
			{ GOWL_KEY_MOD_NONE,  "GOWL_KEY_MOD_NONE",  "none" },
			{ GOWL_KEY_MOD_SHIFT, "GOWL_KEY_MOD_SHIFT", "shift" },
			{ GOWL_KEY_MOD_CAPS,  "GOWL_KEY_MOD_CAPS",  "caps" },
			{ GOWL_KEY_MOD_CTRL,  "GOWL_KEY_MOD_CTRL",  "ctrl" },
			{ GOWL_KEY_MOD_ALT,   "GOWL_KEY_MOD_ALT",   "alt" },
			{ GOWL_KEY_MOD_MOD2,  "GOWL_KEY_MOD_MOD2",  "mod2" },
			{ GOWL_KEY_MOD_MOD3,  "GOWL_KEY_MOD_MOD3",  "mod3" },
			{ GOWL_KEY_MOD_LOGO,  "GOWL_KEY_MOD_LOGO",  "logo" },
			{ GOWL_KEY_MOD_MOD5,  "GOWL_KEY_MOD_MOD5",  "mod5" },
			{ 0, NULL, NULL }
		};
		GType type_id = g_flags_register_static("GowlKeyMod", values);
		g_once_init_leave(&g_type_id, type_id);
	}

	return (GType)g_type_id;
}

/* --- GowlClientState GType registration --- */

/**
 * gowl_client_state_get_type:
 *
 * Retrieves the #GType for #GowlClientState.
 *
 * Returns: the #GType for #GowlClientState
 */
GType
gowl_client_state_get_type(void)
{
	static volatile gsize g_type_id = 0;

	if (g_once_init_enter(&g_type_id)) {
		static const GEnumValue values[] = {
			{ GOWL_CLIENT_STATE_TILED,      "GOWL_CLIENT_STATE_TILED",      "tiled" },
			{ GOWL_CLIENT_STATE_FLOATING,    "GOWL_CLIENT_STATE_FLOATING",    "floating" },
			{ GOWL_CLIENT_STATE_FULLSCREEN,  "GOWL_CLIENT_STATE_FULLSCREEN",  "fullscreen" },
			{ 0, NULL, NULL }
		};
		GType type_id = g_enum_register_static("GowlClientState", values);
		g_once_init_leave(&g_type_id, type_id);
	}

	return (GType)g_type_id;
}

/* --- GowlAction GType registration --- */

/**
 * gowl_action_get_type:
 *
 * Retrieves the #GType for #GowlAction.
 *
 * Returns: the #GType for #GowlAction
 */
GType
gowl_action_get_type(void)
{
	static volatile gsize g_type_id = 0;

	if (g_once_init_enter(&g_type_id)) {
		static const GEnumValue values[] = {
			{ GOWL_ACTION_NONE,              "GOWL_ACTION_NONE",              "none" },
			{ GOWL_ACTION_SPAWN,             "GOWL_ACTION_SPAWN",             "spawn" },
			{ GOWL_ACTION_KILL_CLIENT,       "GOWL_ACTION_KILL_CLIENT",       "kill-client" },
			{ GOWL_ACTION_TOGGLE_FLOAT,      "GOWL_ACTION_TOGGLE_FLOAT",      "toggle-float" },
			{ GOWL_ACTION_TOGGLE_FULLSCREEN, "GOWL_ACTION_TOGGLE_FULLSCREEN", "toggle-fullscreen" },
			{ GOWL_ACTION_FOCUS_STACK,       "GOWL_ACTION_FOCUS_STACK",       "focus-stack" },
			{ GOWL_ACTION_FOCUS_MONITOR,     "GOWL_ACTION_FOCUS_MONITOR",     "focus-monitor" },
			{ GOWL_ACTION_TAG_VIEW,          "GOWL_ACTION_TAG_VIEW",          "tag-view" },
			{ GOWL_ACTION_TAG_SET,           "GOWL_ACTION_TAG_SET",           "tag-set" },
			{ GOWL_ACTION_TAG_TOGGLE_VIEW,   "GOWL_ACTION_TAG_TOGGLE_VIEW",   "tag-toggle-view" },
			{ GOWL_ACTION_TAG_TOGGLE,        "GOWL_ACTION_TAG_TOGGLE",        "tag-toggle" },
			{ GOWL_ACTION_MOVE_TO_MONITOR,   "GOWL_ACTION_MOVE_TO_MONITOR",   "move-to-monitor" },
			{ GOWL_ACTION_SET_MFACT,         "GOWL_ACTION_SET_MFACT",         "set-mfact" },
			{ GOWL_ACTION_INC_NMASTER,       "GOWL_ACTION_INC_NMASTER",       "inc-nmaster" },
			{ GOWL_ACTION_SET_LAYOUT,        "GOWL_ACTION_SET_LAYOUT",        "set-layout" },
			{ GOWL_ACTION_CYCLE_LAYOUT,      "GOWL_ACTION_CYCLE_LAYOUT",      "cycle-layout" },
			{ GOWL_ACTION_ZOOM,              "GOWL_ACTION_ZOOM",              "zoom" },
			{ GOWL_ACTION_QUIT,              "GOWL_ACTION_QUIT",              "quit" },
			{ GOWL_ACTION_RELOAD_CONFIG,     "GOWL_ACTION_RELOAD_CONFIG",     "reload-config" },
			{ GOWL_ACTION_IPC_COMMAND,       "GOWL_ACTION_IPC_COMMAND",       "ipc-command" },
			{ GOWL_ACTION_LOCK,              "GOWL_ACTION_LOCK",              "lock" },
			{ GOWL_ACTION_CUSTOM,            "GOWL_ACTION_CUSTOM",            "custom" },
			{ 0, NULL, NULL }
		};
		GType type_id = g_enum_register_static("GowlAction", values);
		g_once_init_leave(&g_type_id, type_id);
	}

	return (GType)g_type_id;
}

/* --- GowlConfigSource GType registration --- */

/**
 * gowl_config_source_get_type:
 *
 * Retrieves the #GType for #GowlConfigSource.
 *
 * Returns: the #GType for #GowlConfigSource
 */
GType
gowl_config_source_get_type(void)
{
	static volatile gsize g_type_id = 0;

	if (g_once_init_enter(&g_type_id)) {
		static const GEnumValue values[] = {
			{ GOWL_CONFIG_SOURCE_BUILTIN,  "GOWL_CONFIG_SOURCE_BUILTIN",  "builtin" },
			{ GOWL_CONFIG_SOURCE_YAML,     "GOWL_CONFIG_SOURCE_YAML",     "yaml" },
			{ GOWL_CONFIG_SOURCE_C_MODULE, "GOWL_CONFIG_SOURCE_C_MODULE", "c-module" },
			{ GOWL_CONFIG_SOURCE_MERGED,   "GOWL_CONFIG_SOURCE_MERGED",   "merged" },
			{ 0, NULL, NULL }
		};
		GType type_id = g_enum_register_static("GowlConfigSource", values);
		g_once_init_leave(&g_type_id, type_id);
	}

	return (GType)g_type_id;
}

/* --- GowlSceneLayer GType registration --- */

/**
 * gowl_scene_layer_get_type:
 *
 * Retrieves the #GType for #GowlSceneLayer.
 *
 * Returns: the #GType for #GowlSceneLayer
 */
GType
gowl_scene_layer_get_type(void)
{
	static volatile gsize g_type_id = 0;

	if (g_once_init_enter(&g_type_id)) {
		static const GEnumValue values[] = {
			{ GOWL_SCENE_LAYER_BG,      "GOWL_SCENE_LAYER_BG",      "bg" },
			{ GOWL_SCENE_LAYER_BOTTOM,   "GOWL_SCENE_LAYER_BOTTOM",  "bottom" },
			{ GOWL_SCENE_LAYER_TILE,     "GOWL_SCENE_LAYER_TILE",    "tile" },
			{ GOWL_SCENE_LAYER_FLOAT,    "GOWL_SCENE_LAYER_FLOAT",   "float" },
			{ GOWL_SCENE_LAYER_TOP,      "GOWL_SCENE_LAYER_TOP",     "top" },
			{ GOWL_SCENE_LAYER_FS,       "GOWL_SCENE_LAYER_FS",      "fs" },
			{ GOWL_SCENE_LAYER_OVERLAY,  "GOWL_SCENE_LAYER_OVERLAY", "overlay" },
			{ GOWL_SCENE_LAYER_BLOCK,    "GOWL_SCENE_LAYER_BLOCK",   "block" },
			{ GOWL_SCENE_LAYER_COUNT,    "GOWL_SCENE_LAYER_COUNT",   "count" },
			{ 0, NULL, NULL }
		};
		GType type_id = g_enum_register_static("GowlSceneLayer", values);
		g_once_init_leave(&g_type_id, type_id);
	}

	return (GType)g_type_id;
}

/* --- GowlIdleState GType registration --- */

/**
 * gowl_idle_state_get_type:
 *
 * Retrieves the #GType for #GowlIdleState.
 *
 * Returns: the #GType for #GowlIdleState
 */
GType
gowl_idle_state_get_type(void)
{
	static volatile gsize g_type_id = 0;

	if (g_once_init_enter(&g_type_id)) {
		static const GEnumValue values[] = {
			{ GOWL_IDLE_STATE_ACTIVE, "GOWL_IDLE_STATE_ACTIVE", "active" },
			{ GOWL_IDLE_STATE_IDLE,   "GOWL_IDLE_STATE_IDLE",   "idle" },
			{ GOWL_IDLE_STATE_LOCKED, "GOWL_IDLE_STATE_LOCKED", "locked" },
			{ 0, NULL, NULL }
		};
		GType type_id = g_enum_register_static("GowlIdleState", values);
		g_once_init_leave(&g_type_id, type_id);
	}

	return (GType)g_type_id;
}

#pragma GCC diagnostic pop
