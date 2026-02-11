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

#ifndef GOWL_ENUMS_H
#define GOWL_ENUMS_H

#include <glib-object.h>

G_BEGIN_DECLS

/* --- GowlHookPoint --- */

/**
 * GowlHookPoint:
 * @GOWL_HOOK_POINT_STARTUP: Fired once after compositor initialization.
 * @GOWL_HOOK_POINT_SHUTDOWN: Fired once before compositor teardown.
 * @GOWL_HOOK_POINT_CONFIG_RELOAD: Fired when the configuration is reloaded.
 * @GOWL_HOOK_POINT_KEY_PRESS: Fired on keyboard key press.
 * @GOWL_HOOK_POINT_KEY_RELEASE: Fired on keyboard key release.
 * @GOWL_HOOK_POINT_BUTTON_PRESS: Fired on pointer button press.
 * @GOWL_HOOK_POINT_BUTTON_RELEASE: Fired on pointer button release.
 * @GOWL_HOOK_POINT_POINTER_MOTION: Fired on pointer motion.
 * @GOWL_HOOK_POINT_POINTER_AXIS: Fired on pointer axis (scroll).
 * @GOWL_HOOK_POINT_CLIENT_NEW: Fired when a new client surface is created.
 * @GOWL_HOOK_POINT_CLIENT_MAP: Fired when a client surface is mapped.
 * @GOWL_HOOK_POINT_CLIENT_UNMAP: Fired when a client surface is unmapped.
 * @GOWL_HOOK_POINT_CLIENT_DESTROY: Fired when a client is destroyed.
 * @GOWL_HOOK_POINT_CLIENT_FOCUS: Fired when a client gains focus.
 * @GOWL_HOOK_POINT_CLIENT_UNFOCUS: Fired when a client loses focus.
 * @GOWL_HOOK_POINT_CLIENT_SET_TITLE: Fired when a client title changes.
 * @GOWL_HOOK_POINT_CLIENT_SET_APP_ID: Fired when a client app_id changes.
 * @GOWL_HOOK_POINT_CLIENT_REQUEST_FULLSCREEN: Fired on fullscreen request.
 * @GOWL_HOOK_POINT_LAYOUT_ARRANGE: Fired when layout arrangement occurs.
 * @GOWL_HOOK_POINT_LAYOUT_CHANGE: Fired when the active layout changes.
 * @GOWL_HOOK_POINT_TAG_VIEW: Fired when a tag view changes.
 * @GOWL_HOOK_POINT_TAG_CHANGE: Fired when tag assignment changes.
 * @GOWL_HOOK_POINT_MONITOR_NEW: Fired when a new monitor is connected.
 * @GOWL_HOOK_POINT_MONITOR_DESTROY: Fired when a monitor is disconnected.
 * @GOWL_HOOK_POINT_MONITOR_FRAME: Fired on each monitor frame render.
 * @GOWL_HOOK_POINT_FOCUS_STACK: Fired when focus stack order changes.
 * @GOWL_HOOK_POINT_FOCUS_MONITOR: Fired when focus moves between monitors.
 * @GOWL_HOOK_POINT_CLIENT_PLACE: Fired when a client is being placed.
 * @GOWL_HOOK_POINT_CLIENT_DECORATE: Fired when client decoration is applied.
 * @GOWL_HOOK_POINT_CLIENT_RULE_MATCH: Fired when a client matches a rule.
 * @GOWL_HOOK_POINT_IDLE: Fired when the idle timeout is reached.
 * @GOWL_HOOK_POINT_RESUME: Fired when activity resumes after idle.
 * @GOWL_HOOK_POINT_LOCK: Fired when the session is locked.
 * @GOWL_HOOK_POINT_UNLOCK: Fired when the session is unlocked.
 * @GOWL_HOOK_POINT_IPC_COMMAND: Fired when an IPC command is received.
 * @GOWL_HOOK_POINT_LAYER_NEW: Fired when a new layer surface is created.
 * @GOWL_HOOK_POINT_LAYER_MAP: Fired when a layer surface is mapped.
 * @GOWL_HOOK_POINT_LAYER_UNMAP: Fired when a layer surface is unmapped.
 * @GOWL_HOOK_POINT_LAYER_DESTROY: Fired when a layer surface is destroyed.
 * @GOWL_HOOK_POINT_BAR_RENDER: Fired when the bar is being rendered.
 * @GOWL_HOOK_POINT_LAST: Sentinel value; total number of hook points.
 *
 * Enumeration of compositor hook points that modules can subscribe to.
 */
typedef enum {
	GOWL_HOOK_POINT_STARTUP,
	GOWL_HOOK_POINT_SHUTDOWN,
	GOWL_HOOK_POINT_CONFIG_RELOAD,
	GOWL_HOOK_POINT_KEY_PRESS,
	GOWL_HOOK_POINT_KEY_RELEASE,
	GOWL_HOOK_POINT_BUTTON_PRESS,
	GOWL_HOOK_POINT_BUTTON_RELEASE,
	GOWL_HOOK_POINT_POINTER_MOTION,
	GOWL_HOOK_POINT_POINTER_AXIS,
	GOWL_HOOK_POINT_CLIENT_NEW,
	GOWL_HOOK_POINT_CLIENT_MAP,
	GOWL_HOOK_POINT_CLIENT_UNMAP,
	GOWL_HOOK_POINT_CLIENT_DESTROY,
	GOWL_HOOK_POINT_CLIENT_FOCUS,
	GOWL_HOOK_POINT_CLIENT_UNFOCUS,
	GOWL_HOOK_POINT_CLIENT_SET_TITLE,
	GOWL_HOOK_POINT_CLIENT_SET_APP_ID,
	GOWL_HOOK_POINT_CLIENT_REQUEST_FULLSCREEN,
	GOWL_HOOK_POINT_LAYOUT_ARRANGE,
	GOWL_HOOK_POINT_LAYOUT_CHANGE,
	GOWL_HOOK_POINT_TAG_VIEW,
	GOWL_HOOK_POINT_TAG_CHANGE,
	GOWL_HOOK_POINT_MONITOR_NEW,
	GOWL_HOOK_POINT_MONITOR_DESTROY,
	GOWL_HOOK_POINT_MONITOR_FRAME,
	GOWL_HOOK_POINT_FOCUS_STACK,
	GOWL_HOOK_POINT_FOCUS_MONITOR,
	GOWL_HOOK_POINT_CLIENT_PLACE,
	GOWL_HOOK_POINT_CLIENT_DECORATE,
	GOWL_HOOK_POINT_CLIENT_RULE_MATCH,
	GOWL_HOOK_POINT_IDLE,
	GOWL_HOOK_POINT_RESUME,
	GOWL_HOOK_POINT_LOCK,
	GOWL_HOOK_POINT_UNLOCK,
	GOWL_HOOK_POINT_IPC_COMMAND,
	GOWL_HOOK_POINT_LAYER_NEW,
	GOWL_HOOK_POINT_LAYER_MAP,
	GOWL_HOOK_POINT_LAYER_UNMAP,
	GOWL_HOOK_POINT_LAYER_DESTROY,
	GOWL_HOOK_POINT_BAR_RENDER,
	GOWL_HOOK_POINT_LAST
} GowlHookPoint;

#define GOWL_TYPE_HOOK_POINT (gowl_hook_point_get_type())
GType gowl_hook_point_get_type(void) G_GNUC_CONST;

/* --- GowlCursorMode --- */

/**
 * GowlCursorMode:
 * @GOWL_CURSOR_MODE_NORMAL: Normal cursor operation; no grab active.
 * @GOWL_CURSOR_MODE_PRESSED: Button pressed but no move/resize started yet.
 * @GOWL_CURSOR_MODE_MOVE: Cursor is in window-move grab mode.
 * @GOWL_CURSOR_MODE_RESIZE: Cursor is in window-resize grab mode.
 *
 * The current interactive cursor grab mode.
 */
typedef enum {
	GOWL_CURSOR_MODE_NORMAL,
	GOWL_CURSOR_MODE_PRESSED,
	GOWL_CURSOR_MODE_MOVE,
	GOWL_CURSOR_MODE_RESIZE
} GowlCursorMode;

#define GOWL_TYPE_CURSOR_MODE (gowl_cursor_mode_get_type())
GType gowl_cursor_mode_get_type(void) G_GNUC_CONST;

/* --- GowlDirection --- */

/**
 * GowlDirection:
 * @GOWL_DIRECTION_UP: Upward direction.
 * @GOWL_DIRECTION_DOWN: Downward direction.
 * @GOWL_DIRECTION_LEFT: Leftward direction.
 * @GOWL_DIRECTION_RIGHT: Rightward direction.
 *
 * Cardinal direction for navigation and movement operations.
 */
typedef enum {
	GOWL_DIRECTION_UP,
	GOWL_DIRECTION_DOWN,
	GOWL_DIRECTION_LEFT,
	GOWL_DIRECTION_RIGHT
} GowlDirection;

#define GOWL_TYPE_DIRECTION (gowl_direction_get_type())
GType gowl_direction_get_type(void) G_GNUC_CONST;

/* --- GowlLayerShellLayer --- */

/**
 * GowlLayerShellLayer:
 * @GOWL_LAYER_SHELL_LAYER_BACKGROUND: Background layer (below all windows).
 * @GOWL_LAYER_SHELL_LAYER_BOTTOM: Bottom layer (below tiled windows).
 * @GOWL_LAYER_SHELL_LAYER_TOP: Top layer (above tiled windows).
 * @GOWL_LAYER_SHELL_LAYER_OVERLAY: Overlay layer (above everything).
 *
 * Layer shell surface stacking layers, matching the wlr-layer-shell protocol.
 */
typedef enum {
	GOWL_LAYER_SHELL_LAYER_BACKGROUND,
	GOWL_LAYER_SHELL_LAYER_BOTTOM,
	GOWL_LAYER_SHELL_LAYER_TOP,
	GOWL_LAYER_SHELL_LAYER_OVERLAY
} GowlLayerShellLayer;

#define GOWL_TYPE_LAYER_SHELL_LAYER (gowl_layer_shell_layer_get_type())
GType gowl_layer_shell_layer_get_type(void) G_GNUC_CONST;

/* --- GowlKeyMod (flags) --- */

/**
 * GowlKeyMod:
 * @GOWL_KEY_MOD_NONE: No modifier active.
 * @GOWL_KEY_MOD_SHIFT: Shift modifier (1 << 0).
 * @GOWL_KEY_MOD_CAPS: Caps Lock modifier (1 << 1).
 * @GOWL_KEY_MOD_CTRL: Control modifier (1 << 2).
 * @GOWL_KEY_MOD_ALT: Alt / Mod1 modifier (1 << 3).
 * @GOWL_KEY_MOD_MOD2: Mod2 modifier, typically Num Lock (1 << 4).
 * @GOWL_KEY_MOD_MOD3: Mod3 modifier (1 << 5).
 * @GOWL_KEY_MOD_LOGO: Super / Logo modifier (1 << 6).
 * @GOWL_KEY_MOD_MOD5: Mod5 modifier (1 << 7).
 *
 * Bitmask flags for keyboard modifier keys.
 */
typedef enum {
	GOWL_KEY_MOD_NONE  = 0,
	GOWL_KEY_MOD_SHIFT = (1 << 0),
	GOWL_KEY_MOD_CAPS  = (1 << 1),
	GOWL_KEY_MOD_CTRL  = (1 << 2),
	GOWL_KEY_MOD_ALT   = (1 << 3),
	GOWL_KEY_MOD_MOD2  = (1 << 4),
	GOWL_KEY_MOD_MOD3  = (1 << 5),
	GOWL_KEY_MOD_LOGO  = (1 << 6),
	GOWL_KEY_MOD_MOD5  = (1 << 7)
} GowlKeyMod;

#define GOWL_TYPE_KEY_MOD (gowl_key_mod_get_type())
GType gowl_key_mod_get_type(void) G_GNUC_CONST;

/* --- GowlClientState --- */

/**
 * GowlClientState:
 * @GOWL_CLIENT_STATE_TILED: Client is tiled by the layout engine.
 * @GOWL_CLIENT_STATE_FLOATING: Client is floating above tiled windows.
 * @GOWL_CLIENT_STATE_FULLSCREEN: Client occupies the entire monitor.
 *
 * The display state of a compositor client (toplevel surface).
 */
typedef enum {
	GOWL_CLIENT_STATE_TILED,
	GOWL_CLIENT_STATE_FLOATING,
	GOWL_CLIENT_STATE_FULLSCREEN
} GowlClientState;

#define GOWL_TYPE_CLIENT_STATE (gowl_client_state_get_type())
GType gowl_client_state_get_type(void) G_GNUC_CONST;

/* --- GowlAction --- */

/**
 * GowlAction:
 * @GOWL_ACTION_NONE: No-op action.
 * @GOWL_ACTION_SPAWN: Spawn an external process.
 * @GOWL_ACTION_KILL_CLIENT: Close the focused client.
 * @GOWL_ACTION_TOGGLE_FLOAT: Toggle float state on the focused client.
 * @GOWL_ACTION_TOGGLE_FULLSCREEN: Toggle fullscreen on the focused client.
 * @GOWL_ACTION_FOCUS_STACK: Move focus through the client stack.
 * @GOWL_ACTION_FOCUS_MONITOR: Move focus to another monitor.
 * @GOWL_ACTION_TAG_VIEW: View a specific tag.
 * @GOWL_ACTION_TAG_SET: Assign a tag to the focused client.
 * @GOWL_ACTION_TAG_TOGGLE_VIEW: Toggle visibility of a tag.
 * @GOWL_ACTION_TAG_TOGGLE: Toggle a tag on the focused client.
 * @GOWL_ACTION_MOVE_TO_MONITOR: Move the focused client to another monitor.
 * @GOWL_ACTION_SET_MFACT: Set the master area factor.
 * @GOWL_ACTION_INC_NMASTER: Increment the number of master windows.
 * @GOWL_ACTION_SET_LAYOUT: Set a specific layout.
 * @GOWL_ACTION_CYCLE_LAYOUT: Cycle through available layouts.
 * @GOWL_ACTION_ZOOM: Promote the focused client to master.
 * @GOWL_ACTION_QUIT: Quit the compositor.
 * @GOWL_ACTION_RELOAD_CONFIG: Reload the configuration.
 * @GOWL_ACTION_IPC_COMMAND: Execute an IPC command.
 * @GOWL_ACTION_CUSTOM: Custom action handled by a module callback.
 *
 * Compositor actions that can be bound to keys or IPC commands.
 */
typedef enum {
	GOWL_ACTION_NONE,
	GOWL_ACTION_SPAWN,
	GOWL_ACTION_KILL_CLIENT,
	GOWL_ACTION_TOGGLE_FLOAT,
	GOWL_ACTION_TOGGLE_FULLSCREEN,
	GOWL_ACTION_FOCUS_STACK,
	GOWL_ACTION_FOCUS_MONITOR,
	GOWL_ACTION_TAG_VIEW,
	GOWL_ACTION_TAG_SET,
	GOWL_ACTION_TAG_TOGGLE_VIEW,
	GOWL_ACTION_TAG_TOGGLE,
	GOWL_ACTION_MOVE_TO_MONITOR,
	GOWL_ACTION_SET_MFACT,
	GOWL_ACTION_INC_NMASTER,
	GOWL_ACTION_SET_LAYOUT,
	GOWL_ACTION_CYCLE_LAYOUT,
	GOWL_ACTION_ZOOM,
	GOWL_ACTION_QUIT,
	GOWL_ACTION_RELOAD_CONFIG,
	GOWL_ACTION_IPC_COMMAND,
	GOWL_ACTION_CUSTOM
} GowlAction;

#define GOWL_TYPE_ACTION (gowl_action_get_type())
GType gowl_action_get_type(void) G_GNUC_CONST;

/* --- GowlConfigSource --- */

/**
 * GowlConfigSource:
 * @GOWL_CONFIG_SOURCE_BUILTIN: Built-in default configuration.
 * @GOWL_CONFIG_SOURCE_YAML: Configuration loaded from a YAML file.
 * @GOWL_CONFIG_SOURCE_C_MODULE: Configuration provided by a C module.
 * @GOWL_CONFIG_SOURCE_MERGED: Merged configuration from multiple sources.
 *
 * Identifies where a configuration value originated.
 */
typedef enum {
	GOWL_CONFIG_SOURCE_BUILTIN,
	GOWL_CONFIG_SOURCE_YAML,
	GOWL_CONFIG_SOURCE_C_MODULE,
	GOWL_CONFIG_SOURCE_MERGED
} GowlConfigSource;

#define GOWL_TYPE_CONFIG_SOURCE (gowl_config_source_get_type())
GType gowl_config_source_get_type(void) G_GNUC_CONST;

/* --- GowlIdleState --- */

/**
 * GowlIdleState:
 * @GOWL_IDLE_STATE_ACTIVE: User is actively interacting.
 * @GOWL_IDLE_STATE_IDLE: Idle timeout has elapsed; no user activity.
 * @GOWL_IDLE_STATE_LOCKED: Session is locked.
 *
 * The current idle / lock state of the compositor session.
 */
typedef enum {
	GOWL_IDLE_STATE_ACTIVE,
	GOWL_IDLE_STATE_IDLE,
	GOWL_IDLE_STATE_LOCKED
} GowlIdleState;

#define GOWL_TYPE_IDLE_STATE (gowl_idle_state_get_type())
GType gowl_idle_state_get_type(void) G_GNUC_CONST;

/* --- GowlSceneLayer --- */

/**
 * GowlSceneLayer:
 * @GOWL_SCENE_LAYER_BG: Background layer (root background color).
 * @GOWL_SCENE_LAYER_BOTTOM: Bottom layer shell surfaces.
 * @GOWL_SCENE_LAYER_TILE: Tiled client windows.
 * @GOWL_SCENE_LAYER_FLOAT: Floating client windows.
 * @GOWL_SCENE_LAYER_TOP: Top layer shell surfaces.
 * @GOWL_SCENE_LAYER_FS: Fullscreen client windows.
 * @GOWL_SCENE_LAYER_OVERLAY: Overlay layer shell surfaces.
 * @GOWL_SCENE_LAYER_BLOCK: Session lock background.
 * @GOWL_SCENE_LAYER_COUNT: Sentinel; total number of scene layers.
 *
 * Ordering of scene graph layers in the compositor.  Lower values
 * render below higher values.  Matches dwl's layer ordering.
 */
typedef enum {
	GOWL_SCENE_LAYER_BG,
	GOWL_SCENE_LAYER_BOTTOM,
	GOWL_SCENE_LAYER_TILE,
	GOWL_SCENE_LAYER_FLOAT,
	GOWL_SCENE_LAYER_TOP,
	GOWL_SCENE_LAYER_FS,
	GOWL_SCENE_LAYER_OVERLAY,
	GOWL_SCENE_LAYER_BLOCK,
	GOWL_SCENE_LAYER_COUNT
} GowlSceneLayer;

#define GOWL_TYPE_SCENE_LAYER (gowl_scene_layer_get_type())
GType gowl_scene_layer_get_type(void) G_GNUC_CONST;

G_END_DECLS

#endif /* GOWL_ENUMS_H */
