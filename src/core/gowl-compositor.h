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

#ifndef GOWL_COMPOSITOR_H
#define GOWL_COMPOSITOR_H

#include "gowl-types.h"
#include "../interfaces/gowl-prefix-key-policy.h"
#include "../interfaces/gowl-workspace-provider.h"
#include <wayland-server-core.h>
#include <sys/types.h>

struct wlr_seat;
struct wlr_renderer;

G_BEGIN_DECLS

#define GOWL_TYPE_COMPOSITOR (gowl_compositor_get_type())

G_DECLARE_FINAL_TYPE(GowlCompositor, gowl_compositor, GOWL, COMPOSITOR, GObject)

/**
 * gowl_compositor_new:
 *
 * Creates a new #GowlCompositor instance.  The compositor is the main
 * singleton that owns the Wayland display, wlroots backend, renderer,
 * allocator, and scene graph.
 *
 * Returns: (transfer full): a newly created #GowlCompositor
 */
GowlCompositor *gowl_compositor_new   (void);

/**
 * gowl_compositor_set_config:
 * @self: a #GowlCompositor
 * @config: (transfer none): the #GowlConfig to use
 *
 * Sets the configuration object.  Must be called before
 * gowl_compositor_start().  The compositor borrows the reference;
 * the caller retains ownership.
 */
void            gowl_compositor_set_config (GowlCompositor *self,
                                            GowlConfig     *config);

/**
 * gowl_compositor_get_config:
 * @self: a #GowlCompositor
 *
 * Returns: (transfer none) (nullable): the current #GowlConfig
 */
GowlConfig     *gowl_compositor_get_config (GowlCompositor *self);

/**
 * gowl_compositor_set_module_manager:
 * @self: a #GowlCompositor
 * @mgr: (transfer none): the #GowlModuleManager to use
 *
 * Sets the module manager.  Must be called before
 * gowl_compositor_start().  The compositor borrows the reference.
 */
void            gowl_compositor_set_module_manager (GowlCompositor  *self,
                                                    GowlModuleManager *mgr);

/**
 * gowl_compositor_set_ipc:
 * @self: a #GowlCompositor
 * @ipc: (transfer none) (nullable): the #GowlIpc server to use
 *
 * Sets the IPC server.  The compositor borrows the reference and
 * will push state events (tags, layout, title, focus) to subscribed
 * clients whenever those change.  May be %NULL to disable IPC events.
 */
void            gowl_compositor_set_ipc (GowlCompositor *self,
                                         GowlIpc        *ipc);

/**
 * gowl_compositor_get_ipc:
 * @self: a #GowlCompositor
 *
 * Returns: (transfer none) (nullable): the current #GowlIpc
 */
GowlIpc        *gowl_compositor_get_ipc (GowlCompositor *self);

/**
 * gowl_compositor_set_prefix_key_policy:
 * @self: a #GowlCompositor
 * @policy: (transfer none) (nullable): a #GowlPrefixKeyPolicy, or %NULL
 *
 * Installs a runtime-pluggable key policy consulted on every key
 * press.  When the policy returns %TRUE for a press, the key
 * intercept path (used by cmacs `--gowl`) redirects keyboard focus
 * to the Emacs client.  Passing %NULL uninstalls any previous
 * policy and restores standalone-compositor behavior (no redirect).
 *
 * The compositor takes a reference on @policy.  Emits
 * `notify::prefix-key-policy`.  Standalone gowl never calls this —
 * the default value is %NULL which makes the intercept a no-op.
 */
void
gowl_compositor_set_prefix_key_policy(GowlCompositor      *self,
                                       GowlPrefixKeyPolicy *policy);

/**
 * gowl_compositor_get_prefix_key_policy:
 * @self: a #GowlCompositor
 *
 * Returns: (transfer none) (nullable): the currently installed
 *          policy, or %NULL if none is set.
 */
GowlPrefixKeyPolicy *
gowl_compositor_get_prefix_key_policy(GowlCompositor *self);

/**
 * gowl_compositor_set_workspace_provider:
 * @self: a #GowlCompositor
 * @provider: (transfer none) (nullable): a #GowlWorkspaceProvider,
 *   or %NULL to uninstall.
 *
 * Installs the workspace manager.  The compositor takes a
 * reference on @provider and becomes the authoritative emitter of
 * the three workspace signals: `workspace-created`,
 * `workspace-switched`, `workspace-destroyed`.
 *
 * Standalone gowl never calls this; workspaces are the
 * cmacs `--gowl` model (1:1 with Emacs frames).  Passing %NULL
 * returns the compositor to the no-workspace baseline.
 */
void
gowl_compositor_set_workspace_provider(GowlCompositor        *self,
                                        GowlWorkspaceProvider *provider);

/**
 * gowl_compositor_get_workspace_provider:
 * @self: a #GowlCompositor
 *
 * Returns: (transfer none) (nullable): the installed workspace
 *          provider, or %NULL.
 */
GowlWorkspaceProvider *
gowl_compositor_get_workspace_provider(GowlCompositor *self);

/**
 * gowl_compositor_emit_workspace_created:
 * @self: a #GowlCompositor
 * @workspace: the new #GowlWorkspace
 *
 * Convenience for provider implementations that create a
 * workspace and want the compositor's signal to fire.  Purely a
 * wrapper around `g_signal_emit_by_name` so the signal is the
 * single observable point for listeners.
 */
void
gowl_compositor_emit_workspace_created(GowlCompositor *self,
                                        GowlWorkspace  *workspace);

/**
 * gowl_compositor_emit_workspace_switched:
 * @self: a #GowlCompositor
 * @from: (nullable): the previously-active workspace
 * @to: (nullable): the newly-active workspace
 */
void
gowl_compositor_emit_workspace_switched(GowlCompositor *self,
                                         GowlWorkspace  *from,
                                         GowlWorkspace  *to);

/**
 * gowl_compositor_emit_workspace_destroyed:
 * @self: a #GowlCompositor
 * @workspace: the workspace that was removed
 */
void
gowl_compositor_emit_workspace_destroyed(GowlCompositor *self,
                                          GowlWorkspace  *workspace);

/**
 * gowl_compositor_start:
 * @self: a #GowlCompositor
 * @error: (nullable): return location for a #GError, or %NULL
 *
 * Initialises the wlroots backend, renderer, allocator, scene graph,
 * Wayland protocols, input devices, and opens a Wayland socket.
 * On failure, @error is set and %FALSE is returned.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean        gowl_compositor_start (GowlCompositor  *self,
                                       GError          **error);

/**
 * gowl_compositor_run:
 * @self: a #GowlCompositor
 *
 * Enters the Wayland event loop.  This call blocks until
 * gowl_compositor_quit() is called.
 */
void            gowl_compositor_run   (GowlCompositor  *self);

/**
 * gowl_compositor_quit:
 * @self: a #GowlCompositor
 *
 * Requests the compositor to exit its event loop.
 */
void            gowl_compositor_quit  (GowlCompositor  *self);

/**
 * gowl_compositor_get_event_loop:
 * @self: a #GowlCompositor
 *
 * Returns the Wayland event loop used by the compositor.
 * Only valid after gowl_compositor_start() succeeds.
 *
 * Returns: (transfer none) (nullable): the event loop
 */
struct wl_event_loop *gowl_compositor_get_event_loop (GowlCompositor *self);

/**
 * gowl_compositor_get_wl_display:
 * @self: a #GowlCompositor
 *
 * Returns the wl_display owned by the compositor.
 * Only valid after gowl_compositor_start() succeeds.
 *
 * Returns: (transfer none) (nullable): the wl_display
 */
struct wl_display *gowl_compositor_get_wl_display (GowlCompositor *self);

/**
 * gowl_compositor_get_wlr_backend:
 * @self: a #GowlCompositor
 *
 * Returns the wlr_backend used by the compositor.
 * Only valid after gowl_compositor_start() succeeds.
 *
 * Returns: (transfer none) (nullable): the wlr_backend
 */
struct wlr_backend *gowl_compositor_get_wlr_backend (GowlCompositor *self);

/**
 * gowl_compositor_get_socket_name:
 * @self: a #GowlCompositor
 *
 * Returns the Wayland socket name (e.g. "wayland-0").
 * Only valid after gowl_compositor_start() succeeds.
 *
 * Returns: (transfer none) (nullable): the socket name string
 */
const gchar    *gowl_compositor_get_socket_name (GowlCompositor *self);

/**
 * gowl_compositor_get_wlr_seat:
 * @self: a #GowlCompositor
 *
 * Returns the wlr_seat owned by the compositor.  Only valid after
 * gowl_compositor_start() succeeds.
 *
 * Returns: (transfer none) (nullable): the wlr_seat, or %NULL
 */
struct wlr_seat *gowl_compositor_get_wlr_seat (GowlCompositor *self);

/**
 * gowl_compositor_get_wlr_renderer:
 * @self: a #GowlCompositor
 *
 * Returns the wlr_renderer used by the compositor for rendering
 * operations.  Only valid after gowl_compositor_start() succeeds.
 *
 * Returns: (transfer none) (nullable): the struct wlr_renderer, or %NULL
 */
struct wlr_renderer *gowl_compositor_get_wlr_renderer (GowlCompositor *self);

/**
 * gowl_compositor_get_scene_layer:
 * @self: a #GowlCompositor
 * @layer: the #GowlSceneLayer index (0 to GOWL_SCENE_LAYER_COUNT-1)
 *
 * Returns the scene tree for the given layer.  Only valid after
 * gowl_compositor_start() succeeds.
 *
 * Returns: (transfer none) (nullable): the wlr_scene_tree for
 *   the layer, or %NULL if the index is out of range
 */
struct wlr_scene_tree *gowl_compositor_get_scene_layer (GowlCompositor *self,
                                                         gint            layer);

/**
 * gowl_compositor_get_clients:
 * @self: a #GowlCompositor
 *
 * Returns the list of managed client windows.  The list and its
 * elements are owned by the compositor; the caller must not free
 * or modify the list.
 *
 * Returns: (transfer none) (element-type GowlClient) (nullable):
 *   the client list, or %NULL if empty
 */
GList *gowl_compositor_get_clients (GowlCompositor *self);

/**
 * gowl_compositor_get_monitors:
 * @self: a #GowlCompositor
 *
 * Returns the list of active monitors.  The list and its elements
 * are owned by the compositor; the caller must not free or modify
 * the list.
 *
 * Returns: (transfer none) (element-type GowlMonitor) (nullable):
 *   the monitor list, or %NULL if empty
 */
GList *gowl_compositor_get_monitors (GowlCompositor *self);

/**
 * gowl_compositor_get_focused_client:
 * @self: a #GowlCompositor
 *
 * Returns the client that currently has keyboard focus.
 *
 * Returns: (transfer none) (nullable): the focused #GowlClient, or %NULL
 */
GowlClient *gowl_compositor_get_focused_client (GowlCompositor *self);

/**
 * gowl_compositor_get_client_count:
 * @self: a #GowlCompositor
 *
 * Returns the number of managed clients.
 *
 * Returns: the client count
 */
guint gowl_compositor_get_client_count (GowlCompositor *self);

/**
 * gowl_compositor_get_monitor_count:
 * @self: a #GowlCompositor
 *
 * Returns the number of active monitors.
 *
 * Returns: the monitor count
 */
guint gowl_compositor_get_monitor_count (GowlCompositor *self);

/**
 * gowl_compositor_get_module_manager:
 * @self: a #GowlCompositor
 *
 * Returns the module manager used by the compositor.
 *
 * Returns: (transfer none) (nullable): the #GowlModuleManager, or %NULL
 */
GowlModuleManager *gowl_compositor_get_module_manager (GowlCompositor *self);

/**
 * gowl_compositor_get_seat:
 * @self: a #GowlCompositor
 *
 * Returns the #GowlSeat GObject wrapping the compositor's input seat.
 * The seat provides the "focus-changed" signal and holds references
 * to the cursor and keyboard group sub-objects.
 * Only valid after gowl_compositor_start() succeeds.
 *
 * Returns: (transfer none) (nullable): the #GowlSeat, or %NULL
 */
GowlSeat *gowl_compositor_get_seat (GowlCompositor *self);

/**
 * gowl_compositor_get_cursor:
 * @self: a #GowlCompositor
 *
 * Returns the #GowlCursor GObject wrapping the compositor's pointer.
 * Provides "motion", "button", and "axis" signals.
 * Only valid after gowl_compositor_start() succeeds.
 *
 * Returns: (transfer none) (nullable): the #GowlCursor, or %NULL
 */
GowlCursor *gowl_compositor_get_cursor (GowlCompositor *self);

/**
 * gowl_compositor_get_keyboard_group:
 * @self: a #GowlCompositor
 *
 * Returns the #GowlKeyboardGroup GObject wrapping the compositor's
 * keyboard group.  Provides "key" and "modifiers" signals and
 * repeat rate/delay properties.
 * Only valid after gowl_compositor_start() succeeds.
 *
 * Returns: (transfer none) (nullable): the #GowlKeyboardGroup, or %NULL
 */
GowlKeyboardGroup *gowl_compositor_get_keyboard_group (GowlCompositor *self);

/**
 * gowl_compositor_get_idle_manager:
 * @self: a #GowlCompositor
 *
 * Returns the #GowlIdleManager GObject.  Provides "idle" and "resume"
 * signals for detecting user inactivity.
 * Only valid after gowl_compositor_start() succeeds.
 *
 * Returns: (transfer none) (nullable): the #GowlIdleManager, or %NULL
 */
GowlIdleManager *gowl_compositor_get_idle_manager (GowlCompositor *self);

/**
 * gowl_compositor_get_bar:
 * @self: a #GowlCompositor
 *
 * Returns the #GowlBar GObject for the status bar, or %NULL if no bar
 * module is active.  Provides "render" and "click" signals.
 *
 * Returns: (transfer none) (nullable): the #GowlBar, or %NULL
 */
GowlBar *gowl_compositor_get_bar (GowlCompositor *self);

/**
 * gowl_compositor_set_bar:
 * @self: a #GowlCompositor
 * @bar: (transfer none) (nullable): the #GowlBar to register
 *
 * Registers a bar object with the compositor.  Called by bar provider
 * modules during initialization.  The compositor borrows the reference.
 */
void gowl_compositor_set_bar (GowlCompositor *self,
                               GowlBar        *bar);

/**
 * gowl_compositor_swap_clients:
 * @self: a #GowlCompositor
 * @c1: the first #GowlClient
 * @c2: the second #GowlClient
 *
 * Swaps the positions of @c1 and @c2 in the tiling stack and
 * re-arranges the layout.
 */
void gowl_compositor_swap_clients (GowlCompositor *self,
                                    GowlClient     *c1,
                                    GowlClient     *c2);

/**
 * gowl_compositor_zoom_client:
 * @self: a #GowlCompositor
 * @client: (nullable): the #GowlClient to promote, or %NULL for focused
 *
 * Promotes @client to the master position in the tiling stack.
 * If @client is already master, promotes the second visible client.
 * If @client is %NULL, operates on the currently focused client.
 */
void gowl_compositor_zoom_client (GowlCompositor *self,
                                   GowlClient     *client);

/**
 * gowl_compositor_is_locked:
 * @self: a #GowlCompositor
 *
 * Returns whether the session is currently locked.
 *
 * Returns: %TRUE if the session is locked
 */
gboolean gowl_compositor_is_locked    (GowlCompositor *self);

/**
 * gowl_compositor_set_locked:
 * @self: a #GowlCompositor
 * @locked: %TRUE to lock, %FALSE to unlock
 *
 * Sets the compositor lock state.  Used by built-in lock handler
 * modules.  External lock clients use the ext-session-lock-v1
 * protocol instead.
 */
void     gowl_compositor_set_locked   (GowlCompositor *self,
                                        gboolean        locked);

/**
 * gowl_compositor_find_client_by_app_id:
 * @self: a #GowlCompositor
 * @pattern: a glob pattern to match against client app_id values
 *
 * Searches for the first client whose app_id matches @pattern
 * using g_pattern_match_simple().
 *
 * Returns: (transfer none) (nullable): the matching #GowlClient, or %NULL
 */
GowlClient *gowl_compositor_find_client_by_app_id (GowlCompositor *self,
                                                     const gchar    *pattern);

/**
 * gowl_compositor_find_client_by_title:
 * @self: a #GowlCompositor
 * @pattern: a glob pattern to match against client title values
 *
 * Searches for the first client whose title matches @pattern
 * using g_pattern_match_simple().
 *
 * Returns: (transfer none) (nullable): the matching #GowlClient, or %NULL
 */
GowlClient *gowl_compositor_find_client_by_title (GowlCompositor *self,
                                                    const gchar    *pattern);

/**
 * gowl_compositor_arrange:
 * @self: a #GowlCompositor
 * @m: the #GowlMonitor to arrange
 *
 * Recalculates the tiling layout for @m: enables/disables scene
 * nodes for tag visibility, reparents floaters to the float layer,
 * and calls the layout function.
 */
void gowl_compositor_arrange (GowlCompositor *self,
                               GowlMonitor    *m);

/**
 * gowl_compositor_prefloat_pid:
 * @self: a #GowlCompositor
 * @pid: the process ID to pre-float
 *
 * Registers @pid so that when a client owned by that process maps,
 * it is immediately made floating and hidden instead of being tiled.
 * The entry is consumed (removed) on first match.  Used by embedders
 * that need to position the client before it becomes visible.
 */
void gowl_compositor_prefloat_pid (GowlCompositor *self,
                                    pid_t           pid);

/**
 * GowlPrefloatMappedFunc:
 * @compositor: the #GowlCompositor
 * @client: the newly mapped #GowlClient that matched the hint
 * @user_data: caller-supplied payload
 *
 * Callback fired from on_client_map() when a client owned by a
 * pid registered via gowl_compositor_prefloat_pid_with_hint()
 * first appears.  The hint has already been consumed and the
 * client has been reparented and sized per the hint; the
 * callback's job is to capture the client pointer and wire any
 * destroy listeners it needs.
 */
typedef void (*GowlPrefloatMappedFunc) (GowlCompositor *compositor,
                                         GowlClient     *client,
                                         gpointer        user_data);

/**
 * gowl_compositor_prefloat_pid_with_hint:
 * @self: a #GowlCompositor
 * @pid: the process ID to pre-float
 * @x: explicit x position for the mapped client
 * @y: explicit y position
 * @width: explicit width in pixels (including borders)
 * @height: explicit height in pixels (including borders)
 * @layer: #GowlSceneLayer to reparent the client to, typically
 *         %GOWL_SCENE_LAYER_OVERLAY for dropdown-style windows
 * @on_mapped: (scope async) (nullable): callback fired after the
 *             client is sized and reparented; receives the captured
 *             #GowlClient pointer for the module to store
 * @user_data: (closure on_mapped): payload passed to @on_mapped
 *
 * Registers a pid hint that, on first client map, reparents the
 * client to @layer at the specified geometry and marks it as
 * floating.  Unlike gowl_compositor_prefloat_pid(), this path
 * does not hide the client or mark it embedded — the client is
 * visible and fully under the compositor's normal lifecycle
 * management.  Used by the dropdown module to capture a spawned
 * terminal at a fixed location on first appearance.
 *
 * The hint is consumed on first match.
 */
void gowl_compositor_prefloat_pid_with_hint (GowlCompositor         *self,
                                               pid_t                  pid,
                                               gint                   x,
                                               gint                   y,
                                               gint                   width,
                                               gint                   height,
                                               gint                   layer,
                                               GowlPrefloatMappedFunc on_mapped,
                                               gpointer               user_data);

/**
 * gowl_compositor_reparent_client:
 * @self: a #GowlCompositor
 * @client: a #GowlClient
 * @layer: the target #GowlSceneLayer
 *
 * Moves @client's scene node to the specified scene @layer.
 * Used by embedders to place a client on a layer that renders
 * above the Emacs frame (e.g. OVERLAY).
 */
void gowl_compositor_reparent_client (GowlCompositor *self,
                                       GowlClient     *client,
                                       gint            layer);

/**
 * gowl_compositor_resize_client:
 * @self: a #GowlCompositor
 * @client: a #GowlClient
 * @x: horizontal position in compositor coordinates
 * @y: vertical position in compositor coordinates
 * @width: width in pixels (including borders)
 * @height: height in pixels (including borders)
 *
 * Positions and sizes @client in the scene graph: moves the scene
 * node, updates borders, and sends an XDG configure to the client.
 * This is the public equivalent of the internal resize_client().
 */
void gowl_compositor_resize_client (GowlCompositor *self,
                                     GowlClient     *client,
                                     gint            x,
                                     gint            y,
                                     gint            width,
                                     gint            height);

/**
 * gowl_compositor_set_floating:
 * @self: a #GowlCompositor
 * @client: a #GowlClient
 * @floating: %TRUE to float, %FALSE to tile
 *
 * Sets @client's floating state and re-arranges the layout.
 * Reparents the scene node to the appropriate layer (FLOAT,
 * TILE, or FS) and re-runs arrange() so neighbouring tiled
 * clients reclaim or yield space.  This is the public
 * equivalent of the internal setfloating() helper; callers
 * outside the compositor should use this instead of
 * gowl_client_set_floating(), which only flips the flag.
 *
 * No-op on embedded clients, since arrange() skips them.
 */
void gowl_compositor_set_floating (GowlCompositor *self,
                                    GowlClient     *client,
                                    gboolean        floating);

/**
 * gowl_compositor_reparent_client_to_client:
 * @self: a #GowlCompositor
 * @child: the client to embed
 * @parent: the client whose scene tree will own @child
 *
 * Reparents @child's scene node into @parent's scene tree so
 * that @child renders as part of @parent.  Positions are then
 * relative to @parent's top-left corner.
 */
void gowl_compositor_reparent_client_to_client (GowlCompositor *self,
                                                 GowlClient     *child,
                                                 GowlClient     *parent);

/**
 * gowl_compositor_position_embedded:
 * @self: a #GowlCompositor
 * @client: an embedded client
 * @x: x position relative to parent scene tree
 * @y: y position relative to parent scene tree
 * @width: width in pixels
 * @height: height in pixels
 *
 * Positions and sizes an embedded client within its parent's
 * scene tree.  Directly sets the scene node position and sends
 * an XDG configure — no bounds checking is applied.
 */
void gowl_compositor_position_embedded (GowlCompositor *self,
                                         GowlClient     *client,
                                         gint            x,
                                         gint            y,
                                         gint            width,
                                         gint            height);

/**
 * gowl_compositor_refresh_client_decoration:
 * @self: a #GowlCompositor
 * @client: a #GowlClient
 *
 * Re-dispatches the active client decorator for @client at its
 * current geom/bw.  If a decorator module is active and
 * @client has bw > 0, renders the decoration at the current
 * dimensions; if bw == 0, destroys any existing decoration.  If
 * no decorator is active, no-op.  Safe to call after changing
 * @client->bw or @client->geom outside the normal resize_client()
 * path (e.g. embed/unembed).
 */
void gowl_compositor_refresh_client_decoration (GowlCompositor *self,
                                                 GowlClient     *client);

/**
 * GowlKeyInterceptFunc:
 * @compositor: the compositor
 * @modifiers: active modifier bitmask
 * @keysym: XKB keysym
 * @keycode: raw evdev keycode
 * @pressed: %TRUE on press, %FALSE on release
 * @user_data: caller-supplied data
 *
 * Called before forwarding unhandled key events to clients.
 * The callback may change keyboard focus via the seat.
 * Return %TRUE to consume the key (not forwarded).
 * Return %FALSE to let normal forwarding proceed.
 */
typedef gboolean (*GowlKeyInterceptFunc)(GowlCompositor *compositor,
                                         guint           modifiers,
                                         guint           keysym,
                                         guint           keycode,
                                         gboolean        pressed,
                                         gpointer        user_data);

/**
 * gowl_compositor_set_key_intercept:
 * @self: a #GowlCompositor
 * @func: (nullable): the intercept callback, or %NULL to clear
 * @user_data: data passed to @func
 *
 * Registers a callback invoked before unhandled key events are
 * forwarded to the focused client.  Used by embedders to redirect
 * keyboard input away from embedded clients.
 */
void gowl_compositor_set_key_intercept (GowlCompositor      *self,
                                        GowlKeyInterceptFunc  func,
                                        gpointer              user_data);

/**
 * GowlClientMapFunc:
 * @compositor: the compositor
 * @client: the newly mapped client
 * @user_data: caller-supplied data
 *
 * Called when a new client surface is mapped (becomes visible).
 * The callback runs after the compositor's own setup (scene tree,
 * monitor assignment, prefloat PID check) has completed.
 * Embedders can use this to catch clients whose PID did not match
 * the prefloat list (e.g. flatpak/sandbox launchers).
 */
typedef void (*GowlClientMapFunc)(GowlCompositor *compositor,
                                  GowlClient     *client,
                                  gpointer        user_data);

/**
 * gowl_compositor_set_client_map_callback:
 * @self: a #GowlCompositor
 * @func: (nullable): the callback, or %NULL to clear
 * @user_data: data passed to @func
 *
 * Registers a callback invoked after a new client surface maps.
 */
void gowl_compositor_set_client_map_callback (GowlCompositor  *self,
                                              GowlClientMapFunc func,
                                              gpointer          user_data);

/**
 * gowl_compositor_screenshot_output:
 * @self: a #GowlCompositor
 * @output_name: (nullable): output name, or %NULL for focused monitor
 * @width: (out): receives the screenshot width
 * @height: (out): receives the screenshot height
 * @error: (nullable): return location for a #GError
 *
 * Captures a screenshot of the specified output.
 *
 * Returns: (transfer full) (nullable): a #GBytes containing RGBA
 *   pixel data, or %NULL on error
 */
GBytes *gowl_compositor_screenshot_output (GowlCompositor  *self,
                                           const gchar     *output_name,
                                           gint            *width,
                                           gint            *height,
                                           GError         **error);

/**
 * gowl_compositor_screenshot_client:
 * @self: a #GowlCompositor
 * @client: the #GowlClient to capture
 * @width: (out): receives the screenshot width
 * @height: (out): receives the screenshot height
 * @error: (nullable): return location for a #GError
 *
 * Captures a screenshot of a specific client surface.
 *
 * Returns: (transfer full) (nullable): a #GBytes containing RGBA
 *   pixel data, or %NULL on error
 */
GBytes *gowl_compositor_screenshot_client (GowlCompositor  *self,
                                            GowlClient      *client,
                                            gint            *width,
                                            gint            *height,
                                            GError         **error);

/**
 * gowl_compositor_screenshot_region:
 * @self: a #GowlCompositor
 * @output_name: (nullable): output name, or %NULL for focused monitor
 * @rx: region X offset within the output
 * @ry: region Y offset within the output
 * @rw: region width
 * @rh: region height
 * @out_width: (out): receives the cropped width
 * @out_height: (out): receives the cropped height
 * @error: (nullable): return location for a #GError
 *
 * Captures a rectangular region from the specified output.
 *
 * Returns: (transfer full) (nullable): cropped RGBA pixel data
 */
GBytes *gowl_compositor_screenshot_region (GowlCompositor  *self,
                                            const gchar     *output_name,
                                            gint             rx,
                                            gint             ry,
                                            gint             rw,
                                            gint             rh,
                                            gint            *out_width,
                                            gint            *out_height,
                                            GError         **error);

/**
 * gowl_compositor_screenshot_all:
 * @self: a #GowlCompositor
 * @width: (out): receives the stitched image width
 * @height: (out): receives the stitched image height
 * @error: (nullable): return location for a #GError
 *
 * Captures all monitors and stitches them into a single image.
 *
 * Returns: (transfer full) (nullable): stitched RGBA pixel data
 */
GBytes *gowl_compositor_screenshot_all (GowlCompositor  *self,
                                         gint            *width,
                                         gint            *height,
                                         GError         **error);

/**
 * gowl_compositor_save_png:
 * @rgba_data: (transfer none): raw RGBA pixel data
 * @width: image width in pixels
 * @height: image height in pixels
 * @path: output file path
 * @error: (nullable): return location for a #GError
 *
 * Saves RGBA pixel data to a PNG file using cairo.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gowl_compositor_save_png (GBytes       *rgba_data,
                                    gint          width,
                                    gint          height,
                                    const gchar  *path,
                                    GError      **error);

/**
 * gowl_compositor_arrangelayers:
 * @self: a #GowlCompositor
 * @m: the #GowlMonitor to arrange
 *
 * Recalculates the usable window area for @m by processing
 * layer-shell exclusive zones and bar height, then re-tiles
 * all clients if the usable area changed.
 */
void gowl_compositor_arrangelayers (GowlCompositor *self,
                                     GowlMonitor    *m);

G_END_DECLS

#endif /* GOWL_COMPOSITOR_H */
