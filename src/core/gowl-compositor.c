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

#include "gowl-core-private.h"
#include "config/gowl-keybind.h"
#include "module/gowl-module-manager.h"
#include "interfaces/gowl-client-decorator.h"

#ifdef GOWL_HAVE_LIBDECOR
#include "gowl-decor.h"
#endif

#include <gio/gio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <cairo.h>
#include <drm_fourcc.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_damage_ring.h>

/**
 * GowlCompositor:
 *
 * The main compositor singleton.  Owns the Wayland display, wlroots
 * backend, renderer, allocator, scene graph, and all protocol globals.
 * Ported from dwl's setup()/run()/cleanup() architecture.
 */

G_DEFINE_FINAL_TYPE(GowlCompositor, gowl_compositor, G_TYPE_OBJECT)

/* Signal identifiers */
enum {
	SIGNAL_STARTUP,
	SIGNAL_SHUTDOWN,
	SIGNAL_CLIENT_ADDED,
	SIGNAL_CLIENT_REMOVED,
	SIGNAL_FOCUS_CHANGED,
	SIGNAL_FRAME_RENDERED,
	N_SIGNALS
};

static guint compositor_signals[N_SIGNALS] = { 0, };

/* -----------------------------------------------------------
 * Colour parsing helper
 * ----------------------------------------------------------- */

/**
 * gowl_color_parse_to_floats:
 * @hex: hex colour string "#rrggbb" or "#rrggbbaa"
 * @out: (out): 4-element float array (RGBA, 0.0-1.0)
 *
 * Parses a hex colour string into an RGBA float array suitable
 * for wlroots scene rect colours.
 */
void
gowl_color_parse_to_floats(
	const gchar *hex,
	float        out[4]
){
	guint32 val;
	guint r, g, b, a;

	if (hex == NULL || hex[0] != '#') {
		out[0] = out[1] = out[2] = 0.5f;
		out[3] = 1.0f;
		return;
	}

	val = (guint32)strtoul(hex + 1, NULL, 16);

	if (strlen(hex + 1) == 8) {
		/* #rrggbbaa */
		r = (val >> 24) & 0xFF;
		g = (val >> 16) & 0xFF;
		b = (val >> 8)  & 0xFF;
		a = val & 0xFF;
	} else {
		/* #rrggbb */
		r = (val >> 16) & 0xFF;
		g = (val >> 8)  & 0xFF;
		b = val & 0xFF;
		a = 255;
	}

	out[0] = (float)r / 255.0f;
	out[1] = (float)g / 255.0f;
	out[2] = (float)b / 255.0f;
	out[3] = (float)a / 255.0f;
}

/* -----------------------------------------------------------
 * Forward declarations for wl_listener callbacks
 * ----------------------------------------------------------- */

static void on_new_output         (struct wl_listener *listener, void *data);
static void on_new_input          (struct wl_listener *listener, void *data);
static void on_new_xdg_toplevel   (struct wl_listener *listener, void *data);
static void on_new_xdg_popup      (struct wl_listener *listener, void *data);
static void on_new_layer_surface   (struct wl_listener *listener, void *data);
static void on_new_xdg_decoration (struct wl_listener *listener, void *data);
static void on_layout_change      (struct wl_listener *listener, void *data);
static void on_gpu_reset          (struct wl_listener *listener, void *data);
static void on_request_cursor     (struct wl_listener *listener, void *data);
static void on_request_set_sel    (struct wl_listener *listener, void *data);
static void on_request_set_psel   (struct wl_listener *listener, void *data);
static void on_request_start_drag (struct wl_listener *listener, void *data);
static void on_start_drag         (struct wl_listener *listener, void *data);
static void on_cursor_motion      (struct wl_listener *listener, void *data);
static void on_cursor_motion_abs  (struct wl_listener *listener, void *data);
static void on_cursor_button      (struct wl_listener *listener, void *data);
static void on_cursor_axis        (struct wl_listener *listener, void *data);
static void on_cursor_frame       (struct wl_listener *listener, void *data);
static void on_kb_key             (struct wl_listener *listener, void *data);
static void on_kb_modifiers       (struct wl_listener *listener, void *data);
static void on_monitor_frame      (struct wl_listener *listener, void *data);
static void on_monitor_destroy    (struct wl_listener *listener, void *data);
static void on_monitor_request_state(struct wl_listener *listener, void *data);

/* key repeat timer callback */
static int  on_key_repeat         (void *data);

/* layer surface lifecycle callbacks */
static void on_layer_commit       (struct wl_listener *listener, void *data);
static void on_layer_unmap        (struct wl_listener *listener, void *data);
static void on_layer_destroy      (struct wl_listener *listener, void *data);

/* session lock callbacks */
static void on_new_session_lock   (struct wl_listener *listener, void *data);
static void on_session_lock_destroy(struct wl_listener *listener, void *data);
static void on_session_unlock     (struct wl_listener *listener, void *data);
static void on_lock_surface_create(struct wl_listener *listener, void *data);

/* client lifecycle callbacks */
static void on_client_commit      (struct wl_listener *listener, void *data);
static void on_client_map         (struct wl_listener *listener, void *data);
static void on_client_unmap       (struct wl_listener *listener, void *data);
static void on_client_destroy     (struct wl_listener *listener, void *data);
static void on_client_fullscreen  (struct wl_listener *listener, void *data);
static void on_client_maximize    (struct wl_listener *listener, void *data);
static void on_client_set_title   (struct wl_listener *listener, void *data);

/* decoration callbacks */
static void on_request_decoration_mode(struct wl_listener *listener, void *data);
static void on_destroy_decoration     (struct wl_listener *listener, void *data);

/* popup callbacks */
static void on_popup_commit           (struct wl_listener *listener, void *data);

/* internal helpers */
static void create_keyboard       (GowlCompositor *self,
                                   struct wlr_keyboard *keyboard);
static void create_pointer        (GowlCompositor *self,
                                   struct wlr_pointer *pointer);
/* motionnotify is now non-static: gowl_compositor_motionnotify() */
static gboolean keybinding        (GowlCompositor *self,
                                   guint mods,
                                   xkb_keysym_t sym);

/* layout helpers */
static void tile                  (GowlCompositor *self, GowlMonitor *m);
static void monocle               (GowlCompositor *self, GowlMonitor *m);
static void resize_client         (GowlCompositor *self, GowlClient *c,
                                   struct wlr_box geo, gboolean interact);
static void applybounds           (GowlClient *c, struct wlr_box *bbox);
static void setfloating           (GowlCompositor *self, GowlClient *c,
                                   gboolean floating);
static void setfullscreen         (GowlCompositor *self, GowlClient *c,
                                   gboolean fullscreen);
static void setmon                (GowlCompositor *self, GowlClient *c,
                                   GowlMonitor *m, guint32 newtags);

/* client query helpers */
static GowlClient *focustop       (GowlCompositor *self, GowlMonitor *m);
static void xytonode              (GowlCompositor *self, gdouble x, gdouble y,
                                   struct wlr_surface **psurface,
                                   GowlClient **pc, gdouble *nx, gdouble *ny);
static void pointerfocus          (GowlCompositor *self, GowlClient *c,
                                   struct wlr_surface *surface,
                                   gdouble sx, gdouble sy, guint32 time_msec);
static GowlMonitor *xytomon       (GowlCompositor *self,
                                   gdouble x, gdouble y);

/* Macros ported from dwl */
#define VISIBLEON(C, M)  ((M) && (C)->mon == (M) && \
	((C)->tags & (M)->tagset[(M)->seltags]))
#define TAGMASK          ((1u << 9) - 1)

/* -----------------------------------------------------------
 * GObject lifecycle
 * ----------------------------------------------------------- */

static void
gowl_compositor_dispose(GObject *object)
{
	GowlCompositor *self;

	self = GOWL_COMPOSITOR(object);
	self->running = FALSE;

	/* Release GObject sub-object wrappers */
	g_clear_object(&self->seat);
	g_clear_object(&self->cursor_obj);
	g_clear_object(&self->kb_group_obj);
	g_clear_object(&self->idle_mgr);
	g_clear_object(&self->bar);

	G_OBJECT_CLASS(gowl_compositor_parent_class)->dispose(object);
}

static void
gowl_compositor_finalize(GObject *object)
{
	GowlCompositor *self;

	self = GOWL_COMPOSITOR(object);

	/* Teardown in reverse order of setup, following dwl's cleanup() */
	if (self->wl_display != NULL) {
		wl_display_destroy_clients(self->wl_display);

#ifdef GOWL_HAVE_LIBDECOR
		gowl_decor_destroy(self->decor);
		self->decor = NULL;
		g_free(self->parent_wl_display);
		self->parent_wl_display = NULL;
#endif

		if (self->xcursor_mgr != NULL)
			wlr_xcursor_manager_destroy(self->xcursor_mgr);

		if (self->wlr_kb_group != NULL) {
			wlr_keyboard_group_destroy(self->wlr_kb_group);
			self->wlr_kb_group = NULL;
		}

		if (self->backend != NULL)
			wlr_backend_destroy(self->backend);

		wl_display_destroy(self->wl_display);

		/* Destroy scene after display (monitors already gone) */
		if (self->scene != NULL)
			wlr_scene_node_destroy(&self->scene->tree.node);
	}

	g_list_free(self->monitors);
	g_list_free(self->clients);
	g_list_free(self->fstack);

	if (self->prefloat_pids != NULL)
		g_array_unref(self->prefloat_pids);

	G_OBJECT_CLASS(gowl_compositor_parent_class)->finalize(object);
}

/* -----------------------------------------------------------
 * class / instance init
 * ----------------------------------------------------------- */

static void
gowl_compositor_class_init(GowlCompositorClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->dispose  = gowl_compositor_dispose;
	object_class->finalize = gowl_compositor_finalize;

	/**
	 * GowlCompositor::startup:
	 * @compositor: the #GowlCompositor that emitted the signal
	 *
	 * Emitted after the compositor has been successfully started.
	 */
	compositor_signals[SIGNAL_STARTUP] =
		g_signal_new("startup",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlCompositor::shutdown:
	 * @compositor: the #GowlCompositor that emitted the signal
	 *
	 * Emitted when the compositor is about to shut down.
	 */
	compositor_signals[SIGNAL_SHUTDOWN] =
		g_signal_new("shutdown",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlCompositor::client-added:
	 * @compositor: the #GowlCompositor that emitted the signal
	 * @client: the #GowlClient that was mapped
	 *
	 * Emitted when a new client surface has been mapped and is
	 * ready for display.  The client has already been inserted
	 * into the compositor's client list and assigned a monitor.
	 */
	compositor_signals[SIGNAL_CLIENT_ADDED] =
		g_signal_new("client-added",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             1,
		             G_TYPE_OBJECT);

	/**
	 * GowlCompositor::client-removed:
	 * @compositor: the #GowlCompositor that emitted the signal
	 * @client: the #GowlClient that is being unmapped
	 *
	 * Emitted when a client surface is about to be unmapped.
	 * The client is still in the compositor's client list when
	 * this signal fires; it is removed immediately after.
	 */
	compositor_signals[SIGNAL_CLIENT_REMOVED] =
		g_signal_new("client-removed",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             1,
		             G_TYPE_OBJECT);

	/**
	 * GowlCompositor::focus-changed:
	 * @compositor: the #GowlCompositor that emitted the signal
	 * @client: (nullable): the #GowlClient that gained focus,
	 *   or %NULL if focus was cleared
	 *
	 * Emitted when the focused client changes.  This is a
	 * compositor-level mirror of GowlSeat::focus-changed.
	 */
	compositor_signals[SIGNAL_FOCUS_CHANGED] =
		g_signal_new("focus-changed",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             1,
		             G_TYPE_OBJECT);

	/**
	 * GowlCompositor::frame-rendered:
	 * @self: the compositor
	 * @monitor: the #GowlMonitor that just rendered a frame
	 *
	 * Emitted on the compositor dispatch thread after a monitor
	 * completes its frame render.  Signal handlers run on the
	 * same thread that owns the wlroots renderer and EGL context,
	 * so they can safely call screenshot APIs.
	 *
	 * Used by the recording module to capture frames.
	 */
	compositor_signals[SIGNAL_FRAME_RENDERED] =
		g_signal_new("frame-rendered",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             1,
		             G_TYPE_OBJECT);
}

static void
gowl_compositor_init(GowlCompositor *self)
{
	/* GObject already zeroes the allocation; just set non-zero defaults */

	/* default colours (grey background, white focus border) */
	self->root_color[0] = 0.3f;
	self->root_color[1] = 0.3f;
	self->root_color[2] = 0.3f;
	self->root_color[3] = 1.0f;
	self->fullscreen_bg_color[0] = 0.1f;
	self->fullscreen_bg_color[1] = 0.1f;
	self->fullscreen_bg_color[2] = 0.1f;
	self->fullscreen_bg_color[3] = 1.0f;

	self->prefloat_pids = g_array_new(FALSE, FALSE, sizeof(pid_t));
}

/* -----------------------------------------------------------
 * Public API
 * ----------------------------------------------------------- */

/**
 * gowl_compositor_new:
 *
 * Creates a new #GowlCompositor.
 *
 * Returns: (transfer full): a newly allocated #GowlCompositor
 */
GowlCompositor *
gowl_compositor_new(void)
{
	return (GowlCompositor *)g_object_new(GOWL_TYPE_COMPOSITOR, NULL);
}

/**
 * gowl_compositor_set_config:
 * @self: a #GowlCompositor
 * @config: (transfer none): the #GowlConfig to use
 *
 * Stores a borrowed reference to the config object.
 */
void
gowl_compositor_set_config(
	GowlCompositor *self,
	GowlConfig     *config
){
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	self->config = config;
}

/**
 * gowl_compositor_get_config:
 * @self: a #GowlCompositor
 *
 * Returns: (transfer none) (nullable): the current #GowlConfig
 */
GowlConfig *
gowl_compositor_get_config(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	return self->config;
}

/**
 * gowl_compositor_set_module_manager:
 * @self: a #GowlCompositor
 * @mgr: (transfer none): the #GowlModuleManager to use
 *
 * Stores a borrowed reference to the module manager.
 */
void
gowl_compositor_set_module_manager(
	GowlCompositor  *self,
	GowlModuleManager *mgr
){
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	self->module_mgr = mgr;
}

/**
 * gowl_compositor_set_ipc:
 * @self: a #GowlCompositor
 * @ipc: (transfer none) (nullable): the #GowlIpc server to use
 *
 * Stores a borrowed reference to the IPC server.  The compositor
 * will push state events to subscribed clients when state changes.
 */
void
gowl_compositor_set_ipc(
	GowlCompositor *self,
	GowlIpc        *ipc
){
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	self->ipc = ipc;
}

void
gowl_compositor_set_key_intercept(
	GowlCompositor      *self,
	GowlKeyInterceptFunc func,
	gpointer              user_data
){
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	self->key_intercept_func = func;
	self->key_intercept_data = user_data;
}

void
gowl_compositor_set_client_map_callback(
	GowlCompositor   *self,
	GowlClientMapFunc func,
	gpointer           user_data
){
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	self->client_map_func = func;
	self->client_map_data = user_data;
}

/**
 * gowl_compositor_get_ipc:
 * @self: a #GowlCompositor
 *
 * Returns: (transfer none) (nullable): the current #GowlIpc
 */
GowlIpc *
gowl_compositor_get_ipc(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	return self->ipc;
}

/**
 * gowl_compositor_get_event_loop:
 * @self: a #GowlCompositor
 *
 * Returns: (transfer none) (nullable): the Wayland event loop
 */
struct wl_event_loop *
gowl_compositor_get_event_loop(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	return self->event_loop;
}

/**
 * gowl_compositor_get_wl_display:
 * @self: a #GowlCompositor
 *
 * Returns: (transfer none) (nullable): the wl_display
 */
struct wl_display *
gowl_compositor_get_wl_display(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	return self->wl_display;
}

/**
 * gowl_compositor_get_wlr_backend:
 * @self: a #GowlCompositor
 *
 * Returns: (transfer none) (nullable): the wlr_backend
 */
struct wlr_backend *
gowl_compositor_get_wlr_backend(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	return self->backend;
}

/**
 * gowl_compositor_get_socket_name:
 * @self: a #GowlCompositor
 *
 * Returns: (transfer none) (nullable): the Wayland socket name
 */
const gchar *
gowl_compositor_get_socket_name(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	return self->socket_name;
}

/**
 * gowl_compositor_get_wlr_seat:
 * @self: a #GowlCompositor
 *
 * Returns the wlr_seat used by the compositor for input handling.
 * Only valid after gowl_compositor_start() has been called successfully.
 *
 * Returns: (transfer none) (nullable): the struct wlr_seat, or %NULL
 */
struct wlr_seat *
gowl_compositor_get_wlr_seat(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	return self->wlr_seat;
}

/**
 * gowl_compositor_get_wlr_renderer:
 * @self: a #GowlCompositor
 *
 * Returns the wlr_renderer used by the compositor.
 *
 * Returns: (transfer none) (nullable): the struct wlr_renderer, or %NULL
 */
struct wlr_renderer *
gowl_compositor_get_wlr_renderer(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	return self->renderer;
}

/**
 * gowl_compositor_get_scene_layer:
 * @self: a #GowlCompositor
 * @layer: the #GowlSceneLayer index
 *
 * Returns the scene tree for the given layer.
 *
 * Returns: (transfer none) (nullable): the wlr_scene_tree, or %NULL
 */
struct wlr_scene_tree *
gowl_compositor_get_scene_layer(
	GowlCompositor *self,
	gint            layer
){
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);
	g_return_val_if_fail(layer >= 0 && layer < GOWL_SCENE_LAYER_COUNT, NULL);

	return self->layers[layer];
}

/**
 * gowl_compositor_get_clients:
 * @self: a #GowlCompositor
 *
 * Returns the list of managed client windows.  The list and its
 * elements are owned by the compositor; the caller must not free
 * or modify the list.
 *
 * Returns: (transfer none) (element-type GowlClient) (nullable):
 *   the client list, or %NULL
 */
GList *
gowl_compositor_get_clients(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	return self->clients;
}

/**
 * gowl_compositor_get_monitors:
 * @self: a #GowlCompositor
 *
 * Returns the list of active monitors.  The list and its elements
 * are owned by the compositor; the caller must not free or modify
 * the list.
 *
 * Returns: (transfer none) (element-type GowlMonitor) (nullable):
 *   the monitor list, or %NULL
 */
GList *
gowl_compositor_get_monitors(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	return self->monitors;
}

/**
 * gowl_compositor_get_focused_client:
 * @self: a #GowlCompositor
 *
 * Returns the client that currently has keyboard focus.  This is
 * the first client in the focus stack.
 *
 * Returns: (transfer none) (nullable): the focused #GowlClient, or %NULL
 */
GowlClient *
gowl_compositor_get_focused_client(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	if (self->fstack == NULL)
		return NULL;

	return GOWL_CLIENT(self->fstack->data);
}

/**
 * gowl_compositor_get_client_count:
 * @self: a #GowlCompositor
 *
 * Returns the number of managed clients.
 *
 * Returns: the client count
 */
guint
gowl_compositor_get_client_count(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), 0);

	return g_list_length(self->clients);
}

/**
 * gowl_compositor_get_monitor_count:
 * @self: a #GowlCompositor
 *
 * Returns the number of active monitors.
 *
 * Returns: the monitor count
 */
guint
gowl_compositor_get_monitor_count(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), 0);

	return g_list_length(self->monitors);
}

/**
 * gowl_compositor_get_module_manager:
 * @self: a #GowlCompositor
 *
 * Returns the module manager used by the compositor.
 *
 * Returns: (transfer none) (nullable): the #GowlModuleManager, or %NULL
 */
GowlModuleManager *
gowl_compositor_get_module_manager(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	return self->module_mgr;
}

/**
 * gowl_compositor_is_locked:
 * @self: a #GowlCompositor
 *
 * Returns whether the session is currently locked.
 *
 * Returns: %TRUE if the session is locked
 */
gboolean
gowl_compositor_is_locked(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);

	return self->locked;
}

/**
 * gowl_compositor_set_locked:
 * @self: a #GowlCompositor
 * @locked: %TRUE to lock, %FALSE to unlock
 *
 * Sets the compositor lock state.  When locking, enables the locked
 * background and unfocuses all clients.  When unlocking, disables
 * the locked background and restores focus to the top client on
 * the selected monitor.
 *
 * This is used by built-in lock handler modules.  External session
 * lock clients use the ext-session-lock-v1 protocol instead.
 */
void
gowl_compositor_set_locked(
	GowlCompositor *self,
	gboolean        locked
){
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	if (self->locked == locked)
		return;

	self->locked = locked;

	if (locked) {
		/* Enable locked background */
		wlr_scene_node_set_enabled(&self->locked_bg->node, TRUE);
		/* Unfocus all clients */
		gowl_compositor_focus_client(self, NULL, FALSE);
		g_debug("Session locked (built-in)");
	} else {
		/* Disable locked background */
		wlr_scene_node_set_enabled(&self->locked_bg->node, FALSE);
		/* Restore focus and cursor */
		gowl_compositor_focus_client(self,
			focustop(self, self->selmon), TRUE);
		gowl_compositor_motionnotify(self, 0);
		g_debug("Session unlocked (built-in)");
	}
}

/**
 * gowl_compositor_find_client_by_app_id:
 * @self: a #GowlCompositor
 * @pattern: a glob pattern to match against app_id values
 *
 * Searches the client list for the first client whose app_id matches
 * @pattern using g_pattern_match_simple().
 *
 * Returns: (transfer none) (nullable): the matching #GowlClient, or %NULL
 */
GowlClient *
gowl_compositor_find_client_by_app_id(
	GowlCompositor *self,
	const gchar    *pattern
){
	GList *l;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);
	g_return_val_if_fail(pattern != NULL, NULL);

	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c;
		const gchar *app_id;

		c = GOWL_CLIENT(l->data);
		app_id = gowl_client_get_app_id(c);

		if (app_id != NULL && g_pattern_match_simple(pattern, app_id))
			return c;
	}

	return NULL;
}

/**
 * gowl_compositor_find_client_by_title:
 * @self: a #GowlCompositor
 * @pattern: a glob pattern to match against title values
 *
 * Searches the client list for the first client whose title matches
 * @pattern using g_pattern_match_simple().
 *
 * Returns: (transfer none) (nullable): the matching #GowlClient, or %NULL
 */
GowlClient *
gowl_compositor_find_client_by_title(
	GowlCompositor *self,
	const gchar    *pattern
){
	GList *l;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);
	g_return_val_if_fail(pattern != NULL, NULL);

	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c;
		const gchar *title;

		c = GOWL_CLIENT(l->data);
		title = gowl_client_get_title(c);

		if (title != NULL && g_pattern_match_simple(pattern, title))
			return c;
	}

	return NULL;
}

/**
 * gowl_compositor_get_seat:
 * @self: a #GowlCompositor
 *
 * Returns the #GowlSeat wrapping the Wayland seat.  The seat is
 * created during gowl_compositor_start() and owned by the compositor.
 *
 * Returns: (transfer none) (nullable): the #GowlSeat, or %NULL if
 *   the compositor has not been started
 */
GowlSeat *
gowl_compositor_get_seat(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	return self->seat;
}

/**
 * gowl_compositor_get_cursor:
 * @self: a #GowlCompositor
 *
 * Returns the #GowlCursor wrapping the wlroots cursor and
 * xcursor manager.
 *
 * Returns: (transfer none) (nullable): the #GowlCursor, or %NULL
 *   if the compositor has not been started
 */
GowlCursor *
gowl_compositor_get_cursor(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	return self->cursor_obj;
}

/**
 * gowl_compositor_get_keyboard_group:
 * @self: a #GowlCompositor
 *
 * Returns the #GowlKeyboardGroup wrapping the XKB keyboard group,
 * including repeat rate and delay settings.
 *
 * Returns: (transfer none) (nullable): the #GowlKeyboardGroup,
 *   or %NULL if the compositor has not been started
 */
GowlKeyboardGroup *
gowl_compositor_get_keyboard_group(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	return self->kb_group_obj;
}

/**
 * gowl_compositor_get_idle_manager:
 * @self: a #GowlCompositor
 *
 * Returns the #GowlIdleManager wrapping the idle notification
 * and idle-inhibit subsystems.
 *
 * Returns: (transfer none) (nullable): the #GowlIdleManager,
 *   or %NULL if the compositor has not been started
 */
GowlIdleManager *
gowl_compositor_get_idle_manager(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	return self->idle_mgr;
}

/**
 * gowl_compositor_get_bar:
 * @self: a #GowlCompositor
 *
 * Returns the #GowlBar status bar for the compositor, or %NULL
 * if no bar module is active.
 *
 * Returns: (transfer none) (nullable): the #GowlBar, or %NULL
 */
GowlBar *
gowl_compositor_get_bar(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	return self->bar;
}

/**
 * gowl_compositor_set_bar:
 * @self: a #GowlCompositor
 * @bar: (nullable): a #GowlBar to set, or %NULL to clear
 *
 * Sets or clears the compositor's status bar.  The compositor
 * takes a reference on @bar which is released on dispose or
 * when a new bar is set.
 */
void
gowl_compositor_set_bar(
	GowlCompositor *self,
	GowlBar        *bar
){
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	if (self->bar == bar)
		return;

	g_clear_object(&self->bar);

	if (bar != NULL)
		self->bar = g_object_ref(bar);
}

/**
 * gowl_compositor_swap_clients:
 * @self: a #GowlCompositor
 * @c1: a #GowlClient
 * @c2: a #GowlClient
 *
 * Swaps the positions of @c1 and @c2 in the tiling client list
 * and re-arranges the layout on each affected monitor.
 */
void
gowl_compositor_swap_clients(
	GowlCompositor *self,
	GowlClient     *c1,
	GowlClient     *c2
){
	GList *l1, *l2;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(c1 != NULL);
	g_return_if_fail(c2 != NULL);

	if (c1 == c2)
		return;

	l1 = g_list_find(self->clients, c1);
	l2 = g_list_find(self->clients, c2);

	if (l1 == NULL || l2 == NULL)
		return;

	/* Swap data pointers (faster than relinking) */
	l1->data = c2;
	l2->data = c1;

	/* Re-arrange affected monitors */
	gowl_compositor_arrange(self, c1->mon);
	if (c2->mon != c1->mon)
		gowl_compositor_arrange(self, c2->mon);
}

/**
 * gowl_compositor_zoom_client:
 * @self: a #GowlCompositor
 * @client: (nullable): the client to zoom, or %NULL for focused
 *
 * Promotes @client to the head of the tiling list (master
 * position).  If @client is already master, promotes the second
 * visible tiled client instead.  Floating clients are ignored.
 *
 * Ported from dwl's zoom().
 */
void
gowl_compositor_zoom_client(
	GowlCompositor *self,
	GowlClient     *client
){
	GowlClient *sel, *first_visible;
	GList *l;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	sel = client;
	if (sel == NULL)
		sel = focustop(self, self->selmon);

	if (sel == NULL || sel->isfloating)
		return;

	/* Find the first visible tiled client on this monitor */
	first_visible = NULL;
	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *tc = (GowlClient *)l->data;
		if (VISIBLEON(tc, self->selmon) && !tc->isfloating) {
			first_visible = tc;
			break;
		}
	}

	/* If sel is already master, swap with next visible */
	if (sel == first_visible) {
		for (l = g_list_find(self->clients, sel)->next;
		     l != NULL; l = l->next) {
			GowlClient *tc = (GowlClient *)l->data;
			if (VISIBLEON(tc, self->selmon) && !tc->isfloating) {
				sel = tc;
				break;
			}
		}
	}

	/* Move sel to the front of the client list */
	if (sel != first_visible) {
		self->clients = g_list_remove(self->clients, sel);
		self->clients = g_list_prepend(self->clients, sel);
		gowl_compositor_focus_client(self, sel, TRUE);
		gowl_compositor_arrange(self, self->selmon);
	}
}

/**
 * gowl_compositor_screenshot_output:
 * @self: a #GowlCompositor
 * @output_name: (nullable): output name, or %NULL for focused monitor
 * @width: (out): receives the screenshot width
 * @height: (out): receives the screenshot height
 * @error: (nullable): return location for a #GError
 *
 * Captures a screenshot of the specified output by reading back
 * pixels from the output's framebuffer.
 *
 * Returns: (transfer full) (nullable): a #GBytes containing RGBA
 *   pixel data (4 bytes per pixel, row-major), or %NULL on error
 */
GBytes *
gowl_compositor_screenshot_output(
	GowlCompositor  *self,
	const gchar     *output_name,
	gint            *width,
	gint            *height,
	GError         **error
){
	GowlMonitor *mon;
	struct wlr_output *output;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	/* Find the target output */
	if (output_name != NULL) {
		GList *l;

		mon = NULL;
		for (l = self->monitors; l != NULL; l = l->next) {
			GowlMonitor *m = GOWL_MONITOR(l->data);
			if (g_strcmp0(m->wlr_output->name, output_name) == 0) {
				mon = m;
				break;
			}
		}
	} else {
		mon = self->selmon;
	}

	if (mon == NULL || mon->wlr_output == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                    "No output found for screenshot");
		return NULL;
	}

	output = mon->wlr_output;

	/* Render the scene to a buffer via wlr_scene_output_build_state
	 * and read back pixels with wlr_buffer_begin_data_ptr_access. */
	{
		struct wlr_output_state state;
		struct wlr_buffer *buffer;
		void *data;
		uint32_t fmt;
		size_t stride, size;
		guint8 *pixels;

		if (mon->scene_output == NULL) {
			if (width != NULL)  *width  = 0;
			if (height != NULL) *height = 0;
			g_set_error_literal(error, G_IO_ERROR,
			                    G_IO_ERROR_FAILED,
			                    "Monitor has no scene output");
			return NULL;
		}

		wlr_output_state_init(&state);

		/* Force full damage so build_state always produces a buffer,
		 * even when the scene hasn't changed since the last frame.
		 * Required for recording (repeated captures). */
		wlr_damage_ring_add_whole(&mon->scene_output->damage_ring);

		if (!wlr_scene_output_build_state(mon->scene_output,
		                                   &state, NULL)) {
			wlr_output_state_finish(&state);
			if (width != NULL)  *width  = 0;
			if (height != NULL) *height = 0;
			g_set_error_literal(error, G_IO_ERROR,
			                    G_IO_ERROR_FAILED,
			                    "Failed to render scene for screenshot");
			return NULL;
		}

		if (!(state.committed & WLR_OUTPUT_STATE_BUFFER) ||
		    state.buffer == NULL) {
			wlr_output_state_finish(&state);
			if (width != NULL)  *width  = 0;
			if (height != NULL) *height = 0;
			g_set_error_literal(error, G_IO_ERROR,
			                    G_IO_ERROR_FAILED,
			                    "Scene render produced no buffer");
			return NULL;
		}

		buffer = state.buffer;

		if (width != NULL)  *width  = buffer->width;
		if (height != NULL) *height = buffer->height;

		/* Try direct CPU readback first (works for SHM buffers) */
		if (wlr_buffer_begin_data_ptr_access(buffer,
		        WLR_BUFFER_DATA_PTR_ACCESS_READ,
		        &data, &fmt, &stride)) {
			size = stride * (size_t)buffer->height;
			pixels = g_malloc(size);
			memcpy(pixels, data, size);
			wlr_buffer_end_data_ptr_access(buffer);
			wlr_output_state_finish(&state);
			return g_bytes_new_take(pixels, size);
		}

		/* Fallback: GPU texture readback (for DMA-BUF / nested) */
		{
			struct wlr_texture *tex;

			tex = wlr_texture_from_buffer(self->renderer, buffer);
			if (tex != NULL) {
				struct wlr_texture_read_pixels_options opts;

				stride = (size_t)buffer->width * 4;
				size   = stride * (size_t)buffer->height;
				pixels = g_malloc(size);

				memset(&opts, 0, sizeof(opts));
				opts.data   = pixels;
				opts.format = DRM_FORMAT_ARGB8888;
				opts.stride = (uint32_t)stride;

				if (wlr_texture_read_pixels(tex, &opts)) {
					wlr_texture_destroy(tex);
					wlr_output_state_finish(&state);
					return g_bytes_new_take(pixels, size);
				}

				g_free(pixels);
				wlr_texture_destroy(tex);
			}
		}

		wlr_output_state_finish(&state);
		if (width != NULL)  *width  = 0;
		if (height != NULL) *height = 0;
		g_set_error_literal(error, G_IO_ERROR,
		                    G_IO_ERROR_FAILED,
		                    "Cannot read output buffer pixels");
		return NULL;
	}
}

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
GBytes *
gowl_compositor_screenshot_client(
	GowlCompositor  *self,
	GowlClient      *client,
	gint            *width,
	gint            *height,
	GError         **error
){
	struct wlr_surface *surface;
	struct wlr_buffer *buffer;
	void *data;
	uint32_t fmt;
	size_t stride, size;
	guint8 *pixels;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);
	g_return_val_if_fail(GOWL_IS_CLIENT(client), NULL);

	surface = gowl_client_get_wlr_surface(client);
	if (surface == NULL || surface->buffer == NULL) {
		if (width != NULL)  *width  = 0;
		if (height != NULL) *height = 0;
		g_set_error_literal(error, G_IO_ERROR,
		                    G_IO_ERROR_NOT_FOUND,
		                    "Client has no surface buffer");
		return NULL;
	}

	buffer = &surface->buffer->base;

	if (!wlr_buffer_begin_data_ptr_access(buffer,
	        WLR_BUFFER_DATA_PTR_ACCESS_READ,
	        &data, &fmt, &stride)) {
		if (width != NULL)  *width  = 0;
		if (height != NULL) *height = 0;
		g_set_error_literal(error, G_IO_ERROR,
		                    G_IO_ERROR_FAILED,
		                    "Cannot access client buffer data");
		return NULL;
	}

	if (width != NULL)  *width  = buffer->width;
	if (height != NULL) *height = buffer->height;

	/* Copy pixel data — 4 bytes per pixel (ARGB/XRGB). */
	size = stride * (size_t)buffer->height;
	pixels = g_malloc(size);
	memcpy(pixels, data, size);

	wlr_buffer_end_data_ptr_access(buffer);

	return g_bytes_new_take(pixels, size);
}

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
 * The region is clamped to the output dimensions.
 *
 * Returns: (transfer full) (nullable): cropped RGBA pixel data
 */
GBytes *
gowl_compositor_screenshot_region(
	GowlCompositor  *self,
	const gchar     *output_name,
	gint             rx,
	gint             ry,
	gint             rw,
	gint             rh,
	gint            *out_width,
	gint            *out_height,
	GError         **error
){
	g_autoptr(GBytes) full = NULL;
	const guint8 *src;
	guint8 *dst;
	gsize full_size;
	gint fw, fh, stride, y;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	full = gowl_compositor_screenshot_output(self, output_name,
	                                         &fw, &fh, error);
	if (full == NULL)
		return NULL;

	/* Clamp region to output bounds */
	if (rx < 0) { rw += rx; rx = 0; }
	if (ry < 0) { rh += ry; ry = 0; }
	if (rx + rw > fw) rw = fw - rx;
	if (ry + rh > fh) rh = fh - ry;

	if (rw <= 0 || rh <= 0) {
		if (out_width != NULL)  *out_width  = 0;
		if (out_height != NULL) *out_height = 0;
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		                    "Region is empty after clamping");
		return NULL;
	}

	src = g_bytes_get_data(full, &full_size);
	stride = (gint)(full_size / (gsize)fh);  /* bytes per row */

	dst = g_malloc((gsize)rw * 4 * (gsize)rh);
	for (y = 0; y < rh; y++) {
		memcpy(dst + (gsize)y * (gsize)rw * 4,
		       src + (gsize)(ry + y) * (gsize)stride + (gsize)rx * 4,
		       (gsize)rw * 4);
	}

	if (out_width != NULL)  *out_width  = rw;
	if (out_height != NULL) *out_height = rh;

	return g_bytes_new_take(dst, (gsize)rw * 4 * (gsize)rh);
}

/**
 * gowl_compositor_screenshot_all:
 * @self: a #GowlCompositor
 * @width: (out): receives the stitched image width
 * @height: (out): receives the stitched image height
 * @error: (nullable): return location for a #GError
 *
 * Captures all monitors and stitches them into a single image
 * using the output layout positions.
 *
 * Returns: (transfer full) (nullable): stitched RGBA pixel data
 */
GBytes *
gowl_compositor_screenshot_all(
	GowlCompositor  *self,
	gint            *width,
	gint            *height,
	GError         **error
){
	GList *l;
	gint min_x, min_y, max_x, max_y, cw, ch;
	guint8 *canvas;
	gsize canvas_size;
	gboolean first;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), NULL);

	if (self->monitors == NULL) {
		if (width != NULL)  *width  = 0;
		if (height != NULL) *height = 0;
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		                    "No monitors available");
		return NULL;
	}

	/* Compute bounding box from output layout */
	min_x = min_y = G_MAXINT;
	max_x = max_y = G_MININT;
	first = TRUE;

	for (l = self->monitors; l != NULL; l = l->next) {
		GowlMonitor *m = GOWL_MONITOR(l->data);
		struct wlr_box box;

		if (m->wlr_output == NULL)
			continue;
		wlr_output_layout_get_box(self->output_layout,
		                          m->wlr_output, &box);
		if (box.width <= 0 || box.height <= 0)
			continue;

		if (box.x < min_x) min_x = box.x;
		if (box.y < min_y) min_y = box.y;
		if (box.x + box.width > max_x)  max_x = box.x + box.width;
		if (box.y + box.height > max_y) max_y = box.y + box.height;
		first = FALSE;
	}

	if (first) {
		if (width != NULL)  *width  = 0;
		if (height != NULL) *height = 0;
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		                    "No active outputs found");
		return NULL;
	}

	cw = max_x - min_x;
	ch = max_y - min_y;
	canvas_size = (gsize)cw * 4 * (gsize)ch;
	canvas = g_malloc0(canvas_size);

	/* Blit each output into the canvas */
	for (l = self->monitors; l != NULL; l = l->next) {
		GowlMonitor *m = GOWL_MONITOR(l->data);
		struct wlr_box box;
		g_autoptr(GBytes) shot = NULL;
		const guint8 *src;
		gsize src_size;
		gint mw, mh, src_stride, y, ox, oy;

		if (m->wlr_output == NULL)
			continue;
		wlr_output_layout_get_box(self->output_layout,
		                          m->wlr_output, &box);
		if (box.width <= 0 || box.height <= 0)
			continue;

		shot = gowl_compositor_screenshot_output(self,
		         m->wlr_output->name, &mw, &mh, NULL);
		if (shot == NULL)
			continue;

		src = g_bytes_get_data(shot, &src_size);
		src_stride = (gint)(src_size / (gsize)mh);

		ox = box.x - min_x;
		oy = box.y - min_y;

		for (y = 0; y < mh && (oy + y) < ch; y++) {
			gint copy_w = mw;
			if (ox + copy_w > cw) copy_w = cw - ox;
			if (copy_w <= 0) continue;

			memcpy(canvas + (gsize)(oy + y) * (gsize)cw * 4
			              + (gsize)ox * 4,
			       src + (gsize)y * (gsize)src_stride,
			       (gsize)copy_w * 4);
		}
	}

	if (width != NULL)  *width  = cw;
	if (height != NULL) *height = ch;

	return g_bytes_new_take(canvas, canvas_size);
}

/**
 * gowl_compositor_save_png:
 * @rgba_data: (transfer none): raw RGBA pixel data
 * @width: image width in pixels
 * @height: image height in pixels
 * @path: output file path
 * @error: (nullable): return location for a #GError
 *
 * Saves RGBA pixel data to a PNG file using cairo.  The RGBA data
 * is swizzled to cairo's ARGB32 format before writing.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
gowl_compositor_save_png(
	GBytes       *rgba_data,
	gint          width,
	gint          height,
	const gchar  *path,
	GError      **error
){
	cairo_surface_t *surface;
	cairo_status_t status;
	const guint8 *src;
	guint8 *dst;
	gsize src_size;
	gint x, y, src_stride, dst_stride;

	g_return_val_if_fail(rgba_data != NULL, FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	src = g_bytes_get_data(rgba_data, &src_size);
	src_stride = (gint)(src_size / (gsize)height);

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
	                                     width, height);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                    "Failed to create cairo surface");
		cairo_surface_destroy(surface);
		return FALSE;
	}

	dst = cairo_image_surface_get_data(surface);
	dst_stride = cairo_image_surface_get_stride(surface);

	cairo_surface_flush(surface);

	/*
	 * Swizzle from RGBA/BGRA (GPU format) to cairo ARGB32.
	 * wlroots typically returns DRM_FORMAT_ARGB8888 which in
	 * little-endian memory is [B, G, R, A] — matching cairo's
	 * ARGB32.  So we can memcpy row-by-row.
	 */
	for (y = 0; y < height; y++) {
		memcpy(dst + (gsize)y * (gsize)dst_stride,
		       src + (gsize)y * (gsize)src_stride,
		       (gsize)width * 4);
	}

	(void)x; /* suppress unused warning */

	cairo_surface_mark_dirty(surface);

	status = cairo_surface_write_to_png(surface, path);
	cairo_surface_destroy(surface);

	if (status != CAIRO_STATUS_SUCCESS) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		            "cairo PNG write failed: %s",
		            cairo_status_to_string(status));
		return FALSE;
	}

	return TRUE;
}

/**
 * gowl_compositor_start:
 * @self: a #GowlCompositor
 * @error: (nullable): return location for a #GError
 *
 * Initialises the full wlroots stack following dwl's setup() order:
 * display -> backend -> scene -> renderer -> allocator -> protocols
 * -> output_layout -> input -> socket -> backend_start.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
gowl_compositor_start(
	GowlCompositor  *self,
	GError          **error
){
	gint i;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);

	/* Parse border colours from config */
	if (self->config != NULL) {
		gowl_color_parse_to_floats(
			gowl_config_get_border_color_focus(self->config),
			self->focus_color);
		gowl_color_parse_to_floats(
			gowl_config_get_border_color_unfocus(self->config),
			self->unfocus_color);
		gowl_color_parse_to_floats(
			gowl_config_get_border_color_urgent(self->config),
			self->urgent_color);
	} else {
		/* sensible defaults if no config */
		gowl_color_parse_to_floats("#bbbbbb", self->focus_color);
		gowl_color_parse_to_floats("#444444", self->unfocus_color);
		gowl_color_parse_to_floats("#ff0000", self->urgent_color);
	}

	/* 1. Create the Wayland display */
	self->wl_display = wl_display_create();
	if (self->wl_display == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                    "Failed to create wl_display");
		return FALSE;
	}
	self->event_loop = wl_display_get_event_loop(self->wl_display);

	/* 2. Create the backend (DRM/libinput on bare metal, or nested) */
	self->backend = wlr_backend_autocreate(self->event_loop, &self->session);
	if (self->backend == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                    "Failed to create wlr_backend");
		return FALSE;
	}

	/* 3. Create the scene graph with layer trees */
	self->scene = wlr_scene_create();
	self->root_bg = wlr_scene_rect_create(&self->scene->tree,
	                                       0, 0, self->root_color);
	for (i = 0; i < GOWL_SCENE_LAYER_COUNT; i++)
		self->layers[i] = wlr_scene_tree_create(&self->scene->tree);
	self->drag_icon = wlr_scene_tree_create(&self->scene->tree);
	wlr_scene_node_place_below(&self->drag_icon->node,
	                           &self->layers[GOWL_SCENE_LAYER_BLOCK]->node);

	/* 4. Create the renderer */
	self->renderer = wlr_renderer_autocreate(self->backend);
	if (self->renderer == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                    "Failed to create wlr_renderer");
		return FALSE;
	}
	LISTEN(&self->renderer->events.lost, &self->gpu_reset, on_gpu_reset);

	/* Init SHM and DMA-BUF buffer interfaces */
	wlr_renderer_init_wl_shm(self->renderer, self->wl_display);
	if (wlr_renderer_get_texture_formats(self->renderer,
	                                     WLR_BUFFER_CAP_DMABUF)) {
		wlr_drm_create(self->wl_display, self->renderer);
		wlr_scene_set_linux_dmabuf_v1(self->scene,
			wlr_linux_dmabuf_v1_create_with_renderer(
				self->wl_display, 5, self->renderer));
	}

	/* 5. Create the allocator */
	self->allocator = wlr_allocator_autocreate(self->backend,
	                                           self->renderer);
	if (self->allocator == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                    "Failed to create wlr_allocator");
		return FALSE;
	}

	/* 6. Create Wayland protocol globals */
	self->wlr_compositor = wlr_compositor_create(self->wl_display, 6,
	                                             self->renderer);
	wlr_subcompositor_create(self->wl_display);
	wlr_data_device_manager_create(self->wl_display);
	wlr_export_dmabuf_manager_v1_create(self->wl_display);
	wlr_screencopy_manager_v1_create(self->wl_display);
	wlr_data_control_manager_v1_create(self->wl_display);
	wlr_primary_selection_v1_device_manager_create(self->wl_display);
	wlr_viewporter_create(self->wl_display);
	wlr_single_pixel_buffer_manager_v1_create(self->wl_display);
	wlr_fractional_scale_manager_v1_create(self->wl_display, 1);
	wlr_presentation_create(self->wl_display, self->backend, 2);

	/* 7. Output layout */
	self->output_layout = wlr_output_layout_create(self->wl_display);
	LISTEN(&self->output_layout->events.change,
	       &self->layout_change, on_layout_change);
	wlr_xdg_output_manager_v1_create(self->wl_display,
	                                  self->output_layout);

	/* 8. Listen for new outputs */
	LISTEN(&self->backend->events.new_output,
	       &self->new_output, on_new_output);

	/* 9. XDG shell (client windows) */
	self->xdg_shell = wlr_xdg_shell_create(self->wl_display, 6);
	LISTEN(&self->xdg_shell->events.new_toplevel,
	       &self->new_xdg_toplevel, on_new_xdg_toplevel);
	LISTEN(&self->xdg_shell->events.new_popup,
	       &self->new_xdg_popup, on_new_xdg_popup);

	/* 10. Layer shell (panels, overlays) */
	self->layer_shell = wlr_layer_shell_v1_create(self->wl_display, 3);
	LISTEN(&self->layer_shell->events.new_surface,
	       &self->new_layer_surface, on_new_layer_surface);

	/* 11. Idle notification */
	self->idle_notifier = wlr_idle_notifier_v1_create(self->wl_display);

	/* 12. Session lock */
	self->session_lock_mgr =
		wlr_session_lock_manager_v1_create(self->wl_display);
	LISTEN(&self->session_lock_mgr->events.new_lock,
	       &self->new_session_lock, on_new_session_lock);
	self->locked_bg = wlr_scene_rect_create(
		self->layers[GOWL_SCENE_LAYER_BLOCK], 0, 0,
		(float [4]){0.1f, 0.1f, 0.1f, 1.0f});
	wlr_scene_node_set_enabled(&self->locked_bg->node, 0);

	/* 13. Decoration negotiation (server-side borders) */
	wlr_server_decoration_manager_set_default_mode(
		wlr_server_decoration_manager_create(self->wl_display),
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	self->xdg_decoration_mgr =
		wlr_xdg_decoration_manager_v1_create(self->wl_display);
	LISTEN(&self->xdg_decoration_mgr->events.new_toplevel_decoration,
	       &self->new_xdg_decoration, on_new_xdg_decoration);

	/* 14. Cursor */
	self->wlr_cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(self->wlr_cursor,
	                                self->output_layout);
	self->xcursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	setenv("XCURSOR_SIZE", "24", 1);

	LISTEN(&self->wlr_cursor->events.motion,
	       &self->cursor_motion, on_cursor_motion);
	LISTEN(&self->wlr_cursor->events.motion_absolute,
	       &self->cursor_motion_absolute, on_cursor_motion_abs);
	LISTEN(&self->wlr_cursor->events.button,
	       &self->cursor_button, on_cursor_button);
	LISTEN(&self->wlr_cursor->events.axis,
	       &self->cursor_axis, on_cursor_axis);
	LISTEN(&self->wlr_cursor->events.frame,
	       &self->cursor_frame, on_cursor_frame);

	/* Cursor shape manager */
	wlr_cursor_shape_manager_v1_create(self->wl_display, 1);

	/* 15. Input device listener */
	LISTEN(&self->backend->events.new_input,
	       &self->new_input, on_new_input);

	/* Virtual keyboard + pointer managers */
	wlr_virtual_keyboard_manager_v1_create(self->wl_display);
	wlr_virtual_pointer_manager_v1_create(self->wl_display);

	/* 16. Seat */
	self->wlr_seat = wlr_seat_create(self->wl_display, "seat0");
	LISTEN(&self->wlr_seat->events.request_set_cursor,
	       &self->request_cursor, on_request_cursor);
	LISTEN(&self->wlr_seat->events.request_set_selection,
	       &self->request_set_sel, on_request_set_sel);
	LISTEN(&self->wlr_seat->events.request_set_primary_selection,
	       &self->request_set_psel, on_request_set_psel);
	LISTEN(&self->wlr_seat->events.request_start_drag,
	       &self->request_start_drag, on_request_start_drag);
	LISTEN(&self->wlr_seat->events.start_drag,
	       &self->start_drag, on_start_drag);

	/* 17. Keyboard group (XKB context + keymap) */
	{
		struct xkb_context *xkb_ctx;
		struct xkb_keymap  *keymap;
		struct xkb_rule_names rules = { 0 };
		gint repeat_rate  = 25;
		gint repeat_delay = 600;

		if (self->config != NULL) {
			repeat_rate  = gowl_config_get_repeat_rate(self->config);
			repeat_delay = gowl_config_get_repeat_delay(self->config);
		}

		self->wlr_kb_group = wlr_keyboard_group_create();

		xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		keymap = xkb_keymap_new_from_names(xkb_ctx, &rules,
		                                   XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (keymap == NULL) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			                    "Failed to compile XKB keymap");
			xkb_context_unref(xkb_ctx);
			return FALSE;
		}

		wlr_keyboard_set_keymap(&self->wlr_kb_group->keyboard, keymap);
		xkb_keymap_unref(keymap);
		xkb_context_unref(xkb_ctx);

		wlr_keyboard_set_repeat_info(&self->wlr_kb_group->keyboard,
		                             repeat_rate, repeat_delay);

		LISTEN(&self->wlr_kb_group->keyboard.events.key,
		       &self->kb_key, on_kb_key);
		LISTEN(&self->wlr_kb_group->keyboard.events.modifiers,
		       &self->kb_modifiers, on_kb_modifiers);

		self->key_repeat_source =
			wl_event_loop_add_timer(self->event_loop,
			                        on_key_repeat, self);

		wlr_seat_set_keyboard(self->wlr_seat,
		                      &self->wlr_kb_group->keyboard);
	}

	/* 17b. GObject sub-object wrappers */
	self->seat = gowl_seat_new();
	self->seat->wlr_seat = self->wlr_seat;

	self->cursor_obj = gowl_cursor_new();
	self->cursor_obj->wlr_cursor      = self->wlr_cursor;
	self->cursor_obj->xcursor_manager = self->xcursor_mgr;

	self->kb_group_obj = gowl_keyboard_group_new();
	self->kb_group_obj->wlr_group = self->wlr_kb_group;

	self->idle_mgr = gowl_idle_manager_new();
	self->idle_mgr->wlr_idle_notifier = self->idle_notifier;

	/* Wire cross-references */
	self->seat->keyboard_group = self->kb_group_obj;
	self->seat->cursor         = self->cursor_obj;

	/* 18. Output manager */
	self->output_mgr = wlr_output_manager_v1_create(self->wl_display);

	/* Unset DISPLAY to prevent XWayland confusion */
	unsetenv("DISPLAY");

	/* 19. Add Wayland socket */
	self->socket_name = wl_display_add_socket_auto(self->wl_display);
	if (self->socket_name == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                    "Failed to add Wayland socket");
		return FALSE;
	}
#ifdef GOWL_HAVE_LIBDECOR
	/* Save the parent compositor's display name before we overwrite it.
	 * libdecor's GTK plugin calls gtk_init_check() which connects to
	 * $WAYLAND_DISPLAY — if it points to gowl's socket instead of the
	 * parent, GTK connects back to us and deadlocks. */
	self->parent_wl_display = g_strdup(g_getenv("WAYLAND_DISPLAY"));
#endif
	setenv("WAYLAND_DISPLAY", self->socket_name, 1);

	/* 20. Start the backend (enumerates outputs and inputs) */
	if (!wlr_backend_start(self->backend)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                    "Failed to start wlr_backend");
		return FALSE;
	}

#ifdef GOWL_HAVE_LIBDECOR
	/* Now that start() has returned, set up libdecor if we detected
	 * a nested Wayland session in on_new_output().  We could not do
	 * this inside the callback because roundtripping the parent
	 * display from within wlr_backend_start() would deadlock. */
	if (self->nested_wl_backend != NULL) {
		self->decor = gowl_decor_new(self, self->nested_wl_backend);
		if (self->decor != NULL && gowl_decor_setup(self->decor)) {
			/* Success — destroy the undecorated default output */
			if (self->default_wl_output != NULL) {
				wlr_output_destroy(self->default_wl_output);
				self->default_wl_output = NULL;
			}
		} else {
			/* Failed — fall back to undecorated output */
			g_warning("gowl: libdecor setup failed, "
			          "falling back to undecorated output");
			gowl_decor_destroy(self->decor);
			self->decor = NULL;
			/* Destroy the skipped default and create a fresh one.
			 * on_new_output will handle it normally this time. */
			if (self->default_wl_output != NULL) {
				wlr_output_destroy(self->default_wl_output);
				self->default_wl_output = NULL;
			}
			wlr_wl_output_create(self->nested_wl_backend);
		}
	}
#endif

	/* Set default cursor image */
	wlr_cursor_set_xcursor(self->wlr_cursor, self->xcursor_mgr,
	                       "default");

	self->running = TRUE;
	g_signal_emit(self, compositor_signals[SIGNAL_STARTUP], 0);

	g_message("Compositor started on WAYLAND_DISPLAY=%s",
	          self->socket_name);

	return TRUE;
}

/**
 * gowl_compositor_run:
 * @self: a #GowlCompositor
 *
 * Enters the Wayland event loop.  Blocks until quit() is called.
 */
void
gowl_compositor_run(GowlCompositor *self)
{
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(self->wl_display != NULL);

	wl_display_run(self->wl_display);
}

/**
 * gowl_compositor_quit:
 * @self: a #GowlCompositor
 *
 * Terminates the Wayland event loop.
 */
void
gowl_compositor_quit(GowlCompositor *self)
{
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	g_signal_emit(self, compositor_signals[SIGNAL_SHUTDOWN], 0);
	self->running = FALSE;

	if (self->wl_display != NULL)
		wl_display_terminate(self->wl_display);
}

/* -----------------------------------------------------------
 * Internal helpers (Phase 4 - full implementations)
 * ----------------------------------------------------------- */

/**
 * xytomon:
 *
 * Returns the monitor at the given layout coordinates.
 */
static GowlMonitor *
xytomon(
	GowlCompositor *self,
	gdouble         x,
	gdouble         y
){
	struct wlr_output *o;

	o = wlr_output_layout_output_at(self->output_layout, x, y);
	if (o != NULL)
		return (GowlMonitor *)o->data;
	return self->selmon;
}

/**
 * focustop:
 *
 * Returns the top client on the focus stack that is visible
 * on monitor @m.  Does not change focus.
 * Ported from dwl's focustop().
 */
static GowlClient *
focustop(
	GowlCompositor *self,
	GowlMonitor    *m
){
	GList *l;

	for (l = self->fstack; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;
		if (VISIBLEON(c, m))
			return c;
	}
	return NULL;
}

/**
 * applybounds:
 *
 * Ensures client @c stays within the bounding box @bbox.
 * Ported from dwl's applybounds().
 */
static void
applybounds(
	GowlClient  *c,
	struct wlr_box *bbox
){
	gint bw2;

	bw2 = 2 * (gint)c->bw;

	/* set minimum possible */
	if (c->geom.width < 1 + bw2)
		c->geom.width = 1 + bw2;
	if (c->geom.height < 1 + bw2)
		c->geom.height = 1 + bw2;

	if (c->geom.x >= bbox->x + bbox->width)
		c->geom.x = bbox->x + bbox->width - c->geom.width;
	if (c->geom.y >= bbox->y + bbox->height)
		c->geom.y = bbox->y + bbox->height - c->geom.height;
	if (c->geom.x + c->geom.width <= bbox->x)
		c->geom.x = bbox->x;
	if (c->geom.y + c->geom.height <= bbox->y)
		c->geom.y = bbox->y;
}

/**
 * resize_client:
 *
 * Positions and sizes a client in the scene graph, updates borders,
 * and sends a configure request.  If @interact is TRUE, the bounding
 * box is the full output layout; otherwise it's the monitor's window area.
 * Ported from dwl's resize().
 */
static void
resize_client(
	GowlCompositor *self,
	GowlClient     *c,
	struct wlr_box  geo,
	gboolean        interact
){
	struct wlr_box *bbox;
	struct wlr_box sgeom;
	struct wlr_box clip;

	if (c->mon == NULL)
		return;

	if (interact) {
		wlr_output_layout_get_box(self->output_layout, NULL, &sgeom);
		bbox = &sgeom;
	} else {
		bbox = &c->mon->w;
	}

	c->geom = geo;
	applybounds(c, bbox);

	/* Update scene-graph positions, including borders */
	wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
	wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);

	/* Borders: delegate to decorator module if active, else use rects */
	{
		GowlClientDecorator *dec;

		dec = (GowlClientDecorator *)gowl_module_manager_get_decorator(
		          self->module_mgr);
		if (dec != NULL) {
			gint bi;

			/* Hide rect borders when decorator is active */
			for (bi = 0; bi < 4; bi++) {
				if (c->border[bi] != NULL)
					wlr_scene_node_set_enabled(
						&c->border[bi]->node, FALSE);
			}
			/* Decorator renders the rounded frame */
			gowl_client_decorator_render_decoration(
				dec, c, c->geom.width, c->geom.height, c->bw,
				(c == gowl_compositor_get_focused_client(self))
					? self->focus_color : self->unfocus_color);
		} else {
			gint bi;

			/* Re-enable rect borders if decorator was deactivated */
			for (bi = 0; bi < 4; bi++) {
				if (c->border[bi] != NULL)
					wlr_scene_node_set_enabled(
						&c->border[bi]->node, TRUE);
			}
			/* top, bottom, left, right borders */
			wlr_scene_rect_set_size(c->border[0],
			                        c->geom.width, c->bw);
			wlr_scene_rect_set_size(c->border[1],
			                        c->geom.width, c->bw);
			wlr_scene_rect_set_size(c->border[2], c->bw,
			                        c->geom.height - 2 * (gint)c->bw);
			wlr_scene_rect_set_size(c->border[3], c->bw,
			                        c->geom.height - 2 * (gint)c->bw);
			wlr_scene_node_set_position(&c->border[1]->node,
			                            0,
			                            c->geom.height - (gint)c->bw);
			wlr_scene_node_set_position(&c->border[2]->node,
			                            0, c->bw);
			wlr_scene_node_set_position(&c->border[3]->node,
			                            c->geom.width - (gint)c->bw,
			                            c->bw);
		}
	}

	/* Send configure to the XDG toplevel */
	c->resize = wlr_xdg_toplevel_set_size(c->xdg_toplevel,
	                                       c->geom.width - 2 * (gint)c->bw,
	                                       c->geom.height - 2 * (gint)c->bw);

	/*
	 * Clip the surface to the geometry minus border.
	 * Matches dwl's client_get_clip(): origin at the XDG geometry
	 * offset so CSD shadow areas are excluded but content is not
	 * cut off.  GTK apps (Firefox, Ptyxis) have non-zero geometry.x/y
	 * due to invisible resize-grab shadows around the content.
	 */
	clip.x = c->xdg_toplevel->base->geometry.x;
	clip.y = c->xdg_toplevel->base->geometry.y;
	clip.width  = c->geom.width - (gint)c->bw;
	clip.height = c->geom.height - (gint)c->bw;
	wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);
}

/**
 * setfloating:
 *
 * Sets the floating state of a client and re-parents it in the
 * scene graph to the appropriate layer.
 * Ported from dwl's setfloating().
 */
static void
setfloating(
	GowlCompositor *self,
	GowlClient     *c,
	gboolean        floating
){
	c->isfloating = floating;

	if (c->mon == NULL)
		return;

	/* Re-parent in the scene graph */
	wlr_scene_node_reparent(&c->scene->node,
		self->layers[c->isfullscreen ? GOWL_SCENE_LAYER_FS
		             : c->isfloating ? GOWL_SCENE_LAYER_FLOAT
		             : GOWL_SCENE_LAYER_TILE]);
	gowl_compositor_arrange(self, c->mon);
}

/**
 * setfullscreen:
 *
 * Toggles fullscreen for a client.  Saves/restores geometry.
 * Ported from dwl's setfullscreen().
 */
static void
setfullscreen(
	GowlCompositor *self,
	GowlClient     *c,
	gboolean        fullscreen
){
	gint border_width;

	c->isfullscreen = fullscreen;

	if (c->mon == NULL)
		return;

	/* Get border width from config */
	border_width = 1;
	if (self->config != NULL)
		g_object_get(self->config, "border-width", &border_width, NULL);

	c->bw = fullscreen ? 0 : (guint)border_width;
	wlr_xdg_toplevel_set_fullscreen(c->xdg_toplevel, fullscreen);

	/* Re-parent to appropriate layer */
	wlr_scene_node_reparent(&c->scene->node,
		self->layers[c->isfullscreen ? GOWL_SCENE_LAYER_FS
		             : c->isfloating ? GOWL_SCENE_LAYER_FLOAT
		             : GOWL_SCENE_LAYER_TILE]);

	if (fullscreen) {
		c->prev = c->geom;
		resize_client(self, c, c->mon->m, FALSE);
	} else {
		resize_client(self, c, c->prev, FALSE);
	}
	gowl_compositor_arrange(self, c->mon);
}

/**
 * setmon:
 *
 * Assigns a client to a monitor with optional new tags.
 * Ported from dwl's setmon().
 */
static void
setmon(
	GowlCompositor *self,
	GowlClient     *c,
	GowlMonitor    *m,
	guint32         newtags
){
	GowlMonitor *oldmon;

	oldmon = c->mon;

	if (oldmon == m)
		return;

	c->mon = m;
	c->prev = c->geom;

	/* Arrange old monitor */
	if (oldmon != NULL)
		gowl_compositor_arrange(self, oldmon);

	if (m != NULL) {
		/* Make sure window overlaps with the new monitor */
		resize_client(self, c, c->geom, FALSE);
		/* Assign tags of target monitor */
		c->tags = newtags ? newtags : m->tagset[m->seltags];
		setfullscreen(self, c, c->isfullscreen);
		setfloating(self, c, c->isfloating);
	}
	gowl_compositor_focus_client(self, focustop(self, self->selmon), TRUE);
}

/**
 * xytonode:
 *
 * Finds the client and wlr_surface under the given coordinates
 * by walking the scene graph from top layer to bottom.
 * Ported from dwl's xytonode().
 */
static void
xytonode(
	GowlCompositor    *self,
	gdouble            x,
	gdouble            y,
	struct wlr_surface **psurface,
	GowlClient        **pc,
	gdouble            *nx,
	gdouble            *ny
){
	struct wlr_scene_node *node, *pnode;
	struct wlr_surface *surface;
	GowlClient *c;
	gint layer;
	gdouble lx, ly;

	surface = NULL;
	c = NULL;
	lx = ly = 0;

	/* Search from top layer down */
	for (layer = GOWL_SCENE_LAYER_COUNT - 1; surface == NULL && layer >= 0; layer--) {
		node = wlr_scene_node_at(&self->layers[layer]->node, x, y, &lx, &ly);
		if (node == NULL)
			continue;

		if (node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *buf;
			struct wlr_scene_surface *ss;

			buf = wlr_scene_buffer_from_node(node);
			ss = wlr_scene_surface_try_from_buffer(buf);
			if (ss != NULL)
				surface = ss->surface;
		}

		/* Walk the tree upward to find a node with client data.
		 * Layer surfaces (e.g. gowlbar) store GowlLayerSurface in
		 * node.data, so we must verify the type with GOWL_IS_CLIENT.
		 * The walk may also reach the root where parent is NULL. */
		for (pnode = node; pnode != NULL && c == NULL;
		     pnode = pnode->parent ? &pnode->parent->node : NULL) {
			if (pnode->data != NULL && GOWL_IS_CLIENT(pnode->data))
				c = (GowlClient *)pnode->data;
		}
	}

	if (psurface != NULL) *psurface = surface;
	if (pc != NULL)       *pc = c;
	if (nx != NULL)       *nx = lx;
	if (ny != NULL)       *ny = ly;
}

/**
 * pointerfocus:
 *
 * Handles pointer focus: notifies the seat of pointer enter/motion,
 * and optionally applies sloppy focus.
 * Ported from dwl's pointerfocus().
 */
static void
pointerfocus(
	GowlCompositor    *self,
	GowlClient        *c,
	struct wlr_surface *surface,
	gdouble             sx,
	gdouble             sy,
	guint32             time_msec
){
	struct timespec now;
	gboolean sloppy;

	/* Check sloppy focus config */
	sloppy = FALSE;
	if (self->config != NULL)
		sloppy = gowl_config_get_sloppyfocus(self->config);

	if (surface != self->wlr_seat->pointer_state.focused_surface &&
	    sloppy && time_msec && c != NULL &&
	    !gowl_client_get_embedded(c))
		gowl_compositor_focus_client(self, c, FALSE);

	/* If no surface, clear pointer focus */
	if (surface == NULL) {
		wlr_seat_pointer_notify_clear_focus(self->wlr_seat);
		return;
	}

	if (time_msec == 0) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		time_msec = (guint32)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
	}

	wlr_seat_pointer_notify_enter(self->wlr_seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(self->wlr_seat, time_msec, sx, sy);
}

/**
 * client_set_border_color:
 *
 * Sets the border colour of all 4 border rects on a client.
 */
static void
client_set_border_color(
	GowlCompositor *self,
	GowlClient     *c,
	float           color[4]
){
	gint i;
	GowlClientDecorator *dec;

	/* If a decorator module is active, delegate to it */
	dec = (GowlClientDecorator *)gowl_module_manager_get_decorator(
	          self->module_mgr);
	if (dec != NULL) {
		gowl_client_decorator_render_decoration(
			dec, c, c->geom.width, c->geom.height, c->bw, color);
		return;
	}

	for (i = 0; i < 4; i++) {
		if (c->border[i] != NULL)
			wlr_scene_rect_set_color(c->border[i], color);
	}
}

/**
 * gowl_compositor_arrange:
 *
 * Arranges all clients on monitor @m: enables/disables scene nodes
 * for visibility, re-parents floaters to the correct layer, and
 * calls the layout function (tile or monocle).
 * Ported from dwl's arrange().
 */
void
gowl_compositor_arrange(
	GowlCompositor *self,
	GowlMonitor    *m
){
	GowlClient *c, *top;
	GList *l;

	if (m == NULL || m->wlr_output == NULL)
		return;

	/* Enable/disable client scene nodes based on tag visibility.
	 * Embedded clients are externally managed — skip them. */
	for (l = self->clients; l != NULL; l = l->next) {
		c = (GowlClient *)l->data;
		if (c->isembedded)
			continue;
		if (c->mon == m) {
			gboolean vis = VISIBLEON(c, m);
			wlr_scene_node_set_enabled(&c->scene->node, vis);
		}
	}

	/* Fullscreen background */
	top = focustop(self, m);
	wlr_scene_node_set_enabled(&m->fullscreen_bg->node,
	                           top != NULL && top->isfullscreen);

	/* Update layout symbol */
	g_free(m->layout_symbol);
	m->layout_symbol = g_strdup("[]=");

	/* Re-parent floaters to correct layer.
	 * Embedded clients are externally managed — skip them. */
	for (l = self->clients; l != NULL; l = l->next) {
		c = (GowlClient *)l->data;
		if (c->isembedded)
			continue;
		if (c->mon != m)
			continue;
		if (c->scene->node.parent == self->layers[GOWL_SCENE_LAYER_FS])
			continue;

		if (c->isfloating)
			wlr_scene_node_reparent(&c->scene->node,
			                        self->layers[GOWL_SCENE_LAYER_FLOAT]);
		/* non-floating stays in current parent (tile) */
	}

	/* Call layout function - always use tile for now */
	if (m->sellt == 0)
		tile(self, m);
	else
		monocle(self, m);

	/* Re-size fullscreen clients to the current monitor area.
	 * Tile/monocle skip fullscreen clients, so after a mode or
	 * scale change they would remain at the old dimensions. */
	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;
		if (c->mon == m && c->isfullscreen && VISIBLEON(c, m))
			resize_client(self, c, m->m, FALSE);
	}

	/* Restore pointer focus */
	gowl_compositor_motionnotify(self, 0);
}

/**
 * gowl_compositor_prefloat_pid:
 *
 * Registers @pid so that when a client owned by that process maps
 * it is immediately made floating and hidden.  The entry is consumed
 * on first match.
 */
void
gowl_compositor_prefloat_pid(
	GowlCompositor *self,
	pid_t           pid
){
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_array_append_val(self->prefloat_pids, pid);
}

/**
 * gowl_compositor_reparent_client:
 *
 * Moves a client's scene node to the specified scene layer.
 */
void
gowl_compositor_reparent_client(
	GowlCompositor *self,
	GowlClient     *client,
	gint            layer
){
	struct wlr_scene_tree *target;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(GOWL_IS_CLIENT(client));
	g_return_if_fail(layer >= 0 && layer < GOWL_SCENE_LAYER_COUNT);

	target = self->layers[layer];
	if (target != NULL && client->scene != NULL)
		wlr_scene_node_reparent(&client->scene->node, target);
}

/**
 * gowl_compositor_resize_client:
 *
 * Public wrapper around the internal resize_client().
 * Positions and sizes a client in the scene graph.
 */
void
gowl_compositor_resize_client(
	GowlCompositor *self,
	GowlClient     *client,
	gint            x,
	gint            y,
	gint            width,
	gint            height
){
	struct wlr_box geo;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(GOWL_IS_CLIENT(client));

	geo.x = x;
	geo.y = y;
	geo.width = width;
	geo.height = height;
	resize_client(self, client, geo, TRUE);
}

/**
 * gowl_compositor_reparent_client_to_client:
 *
 * Reparents child's scene node into parent's scene tree.
 * After this call, child renders as part of parent and
 * its position is relative to parent's top-left corner.
 */
void
gowl_compositor_reparent_client_to_client(
	GowlCompositor *self,
	GowlClient     *child,
	GowlClient     *parent
){
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(GOWL_IS_CLIENT(child));
	g_return_if_fail(GOWL_IS_CLIENT(parent));

	if (child->scene != NULL && parent->scene != NULL)
		wlr_scene_node_reparent(&child->scene->node, parent->scene);
}

/**
 * gowl_compositor_position_embedded:
 *
 * Positions and sizes an embedded client within its parent's
 * scene tree.  No bounds checking — coordinates are parent-relative.
 */
void
gowl_compositor_position_embedded(
	GowlCompositor *self,
	GowlClient     *client,
	gint            x,
	gint            y,
	gint            width,
	gint            height
){
	struct wlr_box clip;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(GOWL_IS_CLIENT(client));

	client->geom.x = x;
	client->geom.y = y;
	client->geom.width = width;
	client->geom.height = height;

	/* Move scene node (parent-relative) */
	wlr_scene_node_set_position(&client->scene->node, x, y);
	wlr_scene_node_set_position(&client->scene_surface->node,
	                            (gint)client->bw, (gint)client->bw);

	/* Request the client to render at this size */
	client->resize = wlr_xdg_toplevel_set_size(client->xdg_toplevel,
	    width - 2 * (gint)client->bw,
	    height - 2 * (gint)client->bw);

	/* Clip to content area (exclude CSD shadows) */
	clip.x = client->xdg_toplevel->base->geometry.x;
	clip.y = client->xdg_toplevel->base->geometry.y;
	clip.width  = width - 2 * (gint)client->bw;
	clip.height = height - 2 * (gint)client->bw;
	wlr_scene_subsurface_tree_set_clip(&client->scene_surface->node, &clip);

	/* Embed path bypasses resize_client(), so the decorator hook
	   never ran at the new dimensions.  Refresh it here so any
	   active decoration (e.g. roundcorners) follows the embed
	   geometry instead of staying at the stale tiled size. */
	gowl_compositor_refresh_client_decoration(self, client);
}

void
gowl_compositor_refresh_client_decoration(
	GowlCompositor *self,
	GowlClient     *client
){
	GowlClientDecorator *dec;
	const float         *color;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(GOWL_IS_CLIENT(client));

	if (self->module_mgr == NULL)
		return;

	dec = (GowlClientDecorator *)gowl_module_manager_get_decorator(
	          self->module_mgr);
	if (dec == NULL)
		return;

	if (client->bw == 0 || client->geom.width <= 0
	    || client->geom.height <= 0) {
		gowl_client_decorator_destroy_decoration(dec, client);
		return;
	}

	color = (client == gowl_compositor_get_focused_client(self))
	        ? self->focus_color : self->unfocus_color;
	gowl_client_decorator_render_decoration(
		dec, client, client->geom.width, client->geom.height,
		client->bw, color);
}

/**
 * gowl_compositor_focus_client:
 *
 * Focuses client @c.  Updates borders, activates the surface,
 * sends keyboard focus, and manages the focus stack.
 * Ported from dwl's focusclient().
 */
void
gowl_compositor_focus_client(
	GowlCompositor *self,
	GowlClient     *c,
	gboolean        lift
){
	struct wlr_surface *old;
	struct wlr_keyboard *kb;

	if (self->locked)
		return;

	/* Embedded clients never receive keyboard focus.
	 * They are managed by the parent (Emacs) and only
	 * receive pointer events for mouse interaction.
	 */
	if (c != NULL && gowl_client_get_embedded(c))
		return;

	/* Raise client in stacking order if requested */
	if (c != NULL && lift)
		wlr_scene_node_raise_to_top(&c->scene->node);

	old = self->wlr_seat->keyboard_state.focused_surface;

	/* Already focused */
	if (c != NULL && c->xdg_toplevel != NULL &&
	    c->xdg_toplevel->base->surface == old)
		return;

	/* Put the new client atop the focus stack and select its monitor */
	if (c != NULL) {
		self->fstack = g_list_remove(self->fstack, c);
		self->fstack = g_list_prepend(self->fstack, c);
		self->selmon = c->mon;
		c->isurgent = FALSE;

		/* Set focused border colour */
		if (self->wlr_seat->drag == NULL)
			client_set_border_color(self, c, self->focus_color);
	}

	/* Deactivate old client */
	if (old != NULL) {
		struct wlr_xdg_toplevel *old_toplevel;

		old_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(old);
		if (old_toplevel != NULL) {
			/* Close any popups on the old focused client */
			struct wlr_xdg_popup *popup, *tmp;
			wl_list_for_each_safe(popup, tmp,
			                      &old_toplevel->base->popups, link)
				wlr_xdg_popup_destroy(popup);

			/* Set unfocused border colour */
			if (c == NULL || c->xdg_toplevel->base->surface != old) {
				GowlClient *old_c;

				old_c = (GowlClient *)old_toplevel->base->data;
				if (old_c != NULL)
					client_set_border_color(self, old_c, self->unfocus_color);
				wlr_xdg_toplevel_set_activated(old_toplevel, FALSE);
			}
		}
	}

	if (c == NULL) {
		/* No client: clear focus */
		wlr_seat_keyboard_notify_clear_focus(self->wlr_seat);

		if (self->seat != NULL)
			gowl_seat_set_focused_client(self->seat, NULL);
		g_signal_emit(self, compositor_signals[SIGNAL_FOCUS_CHANGED],
		              0, NULL);

		/* Push empty title to IPC subscribers */
		if (self->ipc != NULL)
			gowl_ipc_push_event(self->ipc, "EVENT title ");
		return;
	}

	/* Restore pointer focus */
	gowl_compositor_motionnotify(self, 0);

	/* Focus the client's surface */
	kb = wlr_seat_get_keyboard(self->wlr_seat);
	if (kb != NULL)
		wlr_seat_keyboard_notify_enter(self->wlr_seat,
		                               c->xdg_toplevel->base->surface,
		                               kb->keycodes, kb->num_keycodes,
		                               &kb->modifiers);
	else
		wlr_seat_keyboard_notify_enter(self->wlr_seat,
		                               c->xdg_toplevel->base->surface,
		                               NULL, 0, NULL);

	/* Activate the surface */
	wlr_xdg_toplevel_set_activated(c->xdg_toplevel, TRUE);

	/* Sync GowlSeat focused client (emits seat "focus-changed" signal) */
	if (self->seat != NULL)
		gowl_seat_set_focused_client(self->seat, c);

	/* Emit compositor-level focus-changed signal */
	g_signal_emit(self, compositor_signals[SIGNAL_FOCUS_CHANGED], 0, c);

	/* Push title to IPC subscribers */
	if (self->ipc != NULL)
		gowl_ipc_push_event(self->ipc, "EVENT title %s",
		                     c->title != NULL ? c->title : "");
}

/**
 * tile:
 *
 * Master-stack tiling layout with gap support.
 * Queries the module manager for gap values from GowlGapProvider
 * modules (e.g. vanitygaps).  Outer gaps shrink the usable area,
 * inner gaps add spacing between tiled windows.
 * Ported from dwl's tile().
 */
static void
tile(
	GowlCompositor *self,
	GowlMonitor    *m
){
	guint mw, my, ty;
	gint i, n;
	gint ih, iv, oh, ov;
	gint aw, ah, ax, ay;
	GList *l;

	/* Count visible tiling clients */
	n = 0;
	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen)
			n++;
	}
	if (n == 0)
		return;

	/* Query gap provider for gap values */
	ih = iv = oh = ov = 0;
	if (self->module_mgr != NULL)
		gowl_module_manager_get_gaps(self->module_mgr, (gpointer)m,
		                             &ih, &iv, &oh, &ov);

	/* Compute usable area after outer gaps */
	ax = m->w.x + oh;
	ay = m->w.y + ov;
	aw = m->w.width - 2 * oh;
	ah = m->w.height - 2 * ov;

	if (aw <= 0 || ah <= 0)
		return;

	if (n > m->nmaster)
		mw = m->nmaster ? (guint)roundf((float)aw * (float)m->mfact) : 0;
	else
		mw = (guint)aw;

	i = 0;
	my = ty = 0;
	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;
		struct wlr_box geo;
		gint remaining;
		gint nmaster_count;

		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;

		nmaster_count = m->nmaster < n ? m->nmaster : n;

		if (i < nmaster_count) {
			/* Master area (left side) */
			remaining = nmaster_count - i;
			geo.x = ax;
			geo.y = ay + (gint)my;
			geo.width = (gint)mw - (n > nmaster_count ? ih / 2 : 0);
			geo.height = (ah - (gint)my - (remaining - 1) * iv) / remaining;
			resize_client(self, c, geo, FALSE);
			my += (guint)c->geom.height + (guint)iv;
		} else {
			/* Stack area (right side) */
			remaining = n - i;
			geo.x = ax + (gint)mw + (nmaster_count > 0 ? ih / 2 : 0);
			geo.y = ay + (gint)ty;
			geo.width = aw - (gint)mw - (nmaster_count > 0 ? ih / 2 : 0);
			geo.height = (ah - (gint)ty - (remaining - 1) * iv) / remaining;
			resize_client(self, c, geo, FALSE);
			ty += (guint)c->geom.height + (guint)iv;
		}
		i++;
	}
}

/**
 * monocle:
 *
 * Monocle layout with gap support: all tiled clients fill the
 * window area minus outer gaps.
 * Ported from dwl's monocle().
 */
static void
monocle(
	GowlCompositor *self,
	GowlMonitor    *m
){
	GList *l;
	GowlClient *top;
	gint n;
	gint oh, ov;
	struct wlr_box area;

	/* Query gap provider for outer gaps only (monocle has no inner gaps) */
	oh = ov = 0;
	if (self->module_mgr != NULL)
		gowl_module_manager_get_gaps(self->module_mgr, (gpointer)m,
		                             NULL, NULL, &oh, &ov);

	area.x = m->w.x + oh;
	area.y = m->w.y + ov;
	area.width = m->w.width - 2 * oh;
	area.height = m->w.height - 2 * ov;

	n = 0;
	for (l = self->clients; l != NULL; l = l->next) {
		GowlClient *c = (GowlClient *)l->data;
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		resize_client(self, c, area, FALSE);
		n++;
	}
	if (n > 0) {
		g_free(m->layout_symbol);
		m->layout_symbol = g_strdup_printf("[%d]", n);
	}
	top = focustop(self, m);
	if (top != NULL)
		wlr_scene_node_raise_to_top(&top->scene->node);
}

/* -----------------------------------------------------------
 * Output (monitor) callbacks
 * ----------------------------------------------------------- */

/**
 * on_new_output:
 *
 * Called when a new output (display/monitor) becomes available.
 * Creates a GowlMonitor, initialises rendering, sets preferred mode,
 * and adds the output to the scene graph and layout.
 * Ported from dwl's createmon().
 */
static void
on_new_output(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	struct wlr_output *wlr_output;
	struct wlr_output_state state;
	GowlMonitor *m;
	gdouble mfact;
	gint nmaster;

	self = wl_container_of(listener, self, new_output);
	wlr_output = (struct wlr_output *)data;

#ifdef GOWL_HAVE_LIBDECOR
	/* Detect nested Wayland mode from the first Wayland output.
	 * wlr_backend_autocreate() wraps backends in a multi-backend,
	 * so we check the output (not the top-level backend).
	 *
	 * We must NOT do heavy init here (roundtrips, libdecor_new) because
	 * this fires from inside wlr_backend_start() and roundtripping
	 * the parent display would deadlock.  Just save references and
	 * defer setup to after start() returns. */
	if (wlr_output_is_wl(wlr_output)
	    && self->nested_wl_backend == NULL && self->decor == NULL) {
		self->nested_wl_backend = wlr_output->backend;
		self->default_wl_output = wlr_output;
		g_debug("gowl-decor: detected nested Wayland output %s, "
		        "deferring libdecor setup", wlr_output->name);
		return;
	}

#endif

	/* Set title/app_id on nested Wayland outputs.
	 * The decorated output has no xdg_toplevel (libdecor owns it),
	 * so skip — libdecor_frame_set_title() handles it instead. */
	if (wlr_output_is_wl(wlr_output)
#ifdef GOWL_HAVE_LIBDECOR
	    && self->decor == NULL
#endif
	    ) {
		wlr_wl_output_set_title(wlr_output, "CMacs");
		wlr_wl_output_set_app_id(wlr_output, "cmacs");
	}

	/* Initialise rendering on this output */
	if (!wlr_output_init_render(wlr_output, self->allocator,
	                            self->renderer))
		return;

	/* Create a GowlMonitor to wrap this output */
	m = (GowlMonitor *)g_object_new(GOWL_TYPE_MONITOR, NULL);
	m->wlr_output  = wlr_output;
	m->compositor   = self;
	wlr_output->data = m;

	/* Initialise output state */
	wlr_output_state_init(&state);

	/* Set per-monitor defaults from config */
	mfact   = 0.55;
	nmaster = 1;
	if (self->config != NULL) {
		mfact   = gowl_config_get_mfact(self->config);
		nmaster = gowl_config_get_nmaster(self->config);
	}
	m->tagset[0] = m->tagset[1] = 1;
	m->mfact     = mfact;
	m->nmaster   = nmaster;
	m->seltags   = 0;
	m->sellt     = 0;
	g_free(m->layout_symbol);
	m->layout_symbol = g_strdup("[]=");

	/* Set preferred mode and scale.
	 * Wayland outputs created from a surface (libdecor) have no mode
	 * list — their size comes from the first configure event. */
	{
		struct wlr_output_mode *pref;

		pref = wlr_output_preferred_mode(wlr_output);
		if (pref != NULL)
			wlr_output_state_set_mode(&state, pref);
		else if (wlr_output->width > 0 && wlr_output->height > 0)
			wlr_output_state_set_custom_mode(&state,
				wlr_output->width, wlr_output->height, 0);
		else
			wlr_output_state_set_custom_mode(&state,
				1280, 720, 0);
	}
	wlr_output_state_set_scale(&state, 1);
	wlr_output_state_set_enabled(&state, 1);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	/* Set up output-level event listeners */
	LISTEN(&wlr_output->events.frame,
	       &m->frame, on_monitor_frame);
	LISTEN(&wlr_output->events.destroy,
	       &m->destroy, on_monitor_destroy);
	LISTEN(&wlr_output->events.request_state,
	       &m->request_state, on_monitor_request_state);

	/* Add to monitor list */
	self->monitors = g_list_append(self->monitors, m);

	/* Fullscreen background rect (hidden by default) */
	m->fullscreen_bg = wlr_scene_rect_create(
		self->layers[GOWL_SCENE_LAYER_FS],
		0, 0, self->fullscreen_bg_color);
	wlr_scene_node_set_enabled(&m->fullscreen_bg->node, 0);

	/* Create scene output and add to layout */
	m->scene_output = wlr_scene_output_create(self->scene, wlr_output);
	wlr_output_layout_add_auto(self->output_layout, wlr_output);

	/* Select this monitor if it's the first one */
	if (self->selmon == NULL)
		self->selmon = m;

	g_debug("New output: %s (%dx%d)",
	        wlr_output->name,
	        wlr_output->width, wlr_output->height);
}

/**
 * on_monitor_frame:
 *
 * Called at the output's refresh rate.  Commits the scene graph
 * and sends frame-done events to clients.
 * Ported from dwl's rendermon().
 */
static void
on_monitor_frame(struct wl_listener *listener, void *data)
{
	GowlMonitor *m;
	struct timespec now;

	m = wl_container_of(listener, m, frame);
	(void)data;

	/* Commit the scene graph to this output */
	wlr_scene_output_commit(m->scene_output, NULL);

	/* Notify clients that a frame has been rendered */
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(m->scene_output, &now);

	/* Emit frame-rendered on the compositor so modules (e.g. recording)
	 * can safely capture from the dispatch thread.  EGL is idle at
	 * this point. */
	if (m->compositor != NULL) {
		g_signal_emit(m->compositor,
		              compositor_signals[SIGNAL_FRAME_RENDERED],
		              0, m);
	}
}

/**
 * on_monitor_destroy:
 *
 * Called when an output is disconnected.  Cleans up the monitor,
 * moves clients to the next available monitor.
 * Ported from dwl's cleanupmon()/closemon().
 */
static void
on_monitor_destroy(struct wl_listener *listener, void *data)
{
	GowlMonitor *m;
	GowlCompositor *self;

	m = wl_container_of(listener, m, destroy);
	self = m->compositor;
	(void)data;

	g_debug("Output destroyed: %s", m->wlr_output->name);

	/* Remove event listeners */
	wl_list_remove(&m->frame.link);
	wl_list_remove(&m->destroy.link);
	wl_list_remove(&m->request_state.link);

	/* Remove from compositor's monitor list */
	self->monitors = g_list_remove(self->monitors, m);

	/* Clear wlr_output back-pointer */
	m->wlr_output->data = NULL;

	/* Notify wallpaper providers before tearing down the scene output */
	if (self->module_mgr != NULL)
		gowl_module_manager_dispatch_wallpaper_output_destroy(
			self->module_mgr, m);

	/* Remove from output layout */
	wlr_output_layout_remove(self->output_layout, m->wlr_output);
	wlr_scene_output_destroy(m->scene_output);

	/* Select a new monitor if this was the selected one */
	if (m == self->selmon) {
		if (self->monitors != NULL)
			self->selmon = (GowlMonitor *)self->monitors->data;
		else
			self->selmon = NULL;
	}

	/* Move clients from destroyed monitor to selmon */
	{
		GList *l, *next;
		for (l = self->clients; l != NULL; l = next) {
			GowlClient *c = (GowlClient *)l->data;
			next = l->next;
			if (c->mon == m)
				setmon(self, c, self->selmon, c->tags);
		}
	}

	/* Destroy fullscreen bg */
	if (m->fullscreen_bg != NULL)
		wlr_scene_node_destroy(&m->fullscreen_bg->node);

	/* Free layer surfaces list (surfaces themselves will be
	 * destroyed by wlroots when the output goes away) */
	g_list_free(m->layer_surfaces);
	m->layer_surfaces = NULL;

	g_object_unref(m);
}

/**
 * on_monitor_request_state:
 *
 * Handles output state change requests (e.g. mode change, DPMS).
 */
static void
on_monitor_request_state(struct wl_listener *listener, void *data)
{
	GowlMonitor *m;
	GowlCompositor *self;
	struct wlr_output_event_request_state *ev;
	struct wlr_box new_box;

	m = wl_container_of(listener, m, request_state);
	self = m->compositor;
	ev = (struct wlr_output_event_request_state *)data;
	wlr_output_commit_state(ev->output, ev->state);

	/* After committing a state change (e.g. nested window resize),
	 * read the effective dimensions directly from the output —
	 * the layout's commit listener may not have fired yet. */
	new_box.x = m->m.x;
	new_box.y = m->m.y;
	new_box.width  = ev->output->width;
	new_box.height = ev->output->height;

	if (!wlr_box_equal(&new_box, &m->m)) {
		m->m = new_box;
		m->w = m->m;
		if (self->root_bg != NULL)
			wlr_scene_rect_set_size(self->root_bg,
			                        new_box.width, new_box.height);
		if (self->module_mgr != NULL) {
			gowl_module_manager_dispatch_wallpaper_output(
				self->module_mgr, self, m);
			gowl_module_manager_dispatch_bar_render(
				self->module_mgr, self, m);
		}
		gowl_compositor_arrangelayers(self, m);
	}
}

/**
 * on_layout_change:
 *
 * Called when the output layout changes (output added/removed/moved).
 * Updates monitor geometry.
 */
static void
on_layout_change(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	GList *l;

	self = wl_container_of(listener, self, layout_change);
	(void)data;

	/* Update each monitor's area from the layout.
	 * Use wlr_output_layout_get_box() which accounts for scale
	 * and transform, giving the correct logical dimensions. */
	for (l = self->monitors; l != NULL; l = l->next) {
		GowlMonitor *m = (GowlMonitor *)l->data;
		struct wlr_box box;

		wlr_output_layout_get_box(self->output_layout,
		                          m->wlr_output, &box);
		if (wlr_box_empty(&box))
			continue;

		m->m = box;

		/* Window area starts as full monitor area; layer-shell
		 * surfaces will subtract their exclusive zones later */
		m->w = m->m;
	}

	/* Resize root background to cover all outputs */
	if (self->root_bg != NULL) {
		struct wlr_box full;
		wlr_output_layout_get_box(self->output_layout, NULL, &full);
		wlr_scene_rect_set_size(self->root_bg, full.width, full.height);
	}

	/* Notify wallpaper and bar providers so they can resize */
	if (self->module_mgr != NULL) {
		for (l = self->monitors; l != NULL; l = l->next) {
			gowl_module_manager_dispatch_wallpaper_output(
				self->module_mgr, self, l->data);
			gowl_module_manager_dispatch_bar_render(
				self->module_mgr, self, l->data);
		}
	}

	/* Recalculate usable area (m->w) from the new monitor geometry
	 * (m->m), subtracting bar height and layer-shell exclusive zones.
	 * arrangelayers calls arrange() internally if m->w changed. */
	for (l = self->monitors; l != NULL; l = l->next)
		gowl_compositor_arrangelayers(self, (GowlMonitor *)l->data);
}

static void
on_gpu_reset(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;

	self = wl_container_of(listener, self, gpu_reset);
	(void)data;

	g_warning("GPU reset detected");
	wlr_renderer_autocreate(self->backend);
}

/* -----------------------------------------------------------
 * Input callbacks
 * ----------------------------------------------------------- */

/**
 * on_new_input:
 *
 * Called when a new input device (keyboard, pointer, etc.) is
 * detected.  Routes keyboards to the keyboard group and pointers
 * to the cursor.  Ported from dwl's inputdevice().
 */
static void
on_new_input(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	struct wlr_input_device *device;
	guint32 caps;

	self = wl_container_of(listener, self, new_input);
	device = (struct wlr_input_device *)data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		create_keyboard(self,
		                wlr_keyboard_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_POINTER:
		create_pointer(self,
		               wlr_pointer_from_input_device(device));
		break;
	default:
		break;
	}

	/* Update seat capabilities */
	caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&self->wlr_kb_group->devices))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(self->wlr_seat, caps);
}

/**
 * create_keyboard:
 *
 * Configures a new keyboard device and adds it to the keyboard group.
 */
static void
create_keyboard(
	GowlCompositor      *self,
	struct wlr_keyboard  *keyboard
){
	struct xkb_context *ctx;
	struct xkb_keymap  *keymap;
	struct xkb_rule_names rules = { 0 };

	/* Set up XKB keymap for this keyboard */
	ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	keymap = xkb_keymap_new_from_names(ctx, &rules,
	                                   XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (keymap != NULL) {
		wlr_keyboard_set_keymap(keyboard, keymap);
		xkb_keymap_unref(keymap);
	}
	xkb_context_unref(ctx);

	/* Add to the keyboard group (shared XKB state) */
	wlr_keyboard_group_add_keyboard(self->wlr_kb_group, keyboard);
}

/**
 * create_pointer:
 *
 * Attaches a new pointer device to the cursor.
 */
static void
create_pointer(
	GowlCompositor     *self,
	struct wlr_pointer  *pointer
){
	wlr_cursor_attach_input_device(self->wlr_cursor,
	                               &pointer->base);
}

/* -----------------------------------------------------------
 * Keyboard callbacks
 * ----------------------------------------------------------- */

/**
 * keybinding:
 *
 * Checks if a key matches a configured keybind and dispatches
 * the associated action.  Returns TRUE if the key was consumed.
 */
static gboolean
keybinding(
	GowlCompositor *self,
	guint           mods,
	xkb_keysym_t   sym
){
	GArray *keybinds;
	guint i;
	guint clean_mods;

	if (self->config == NULL)
		return FALSE;

	keybinds = gowl_config_get_keybinds(self->config);
	if (keybinds == NULL)
		return FALSE;

	clean_mods = GOWL_CLEANMASK(mods);

	g_debug("keybinding: sym=0x%04x mods=0x%x clean=0x%x",
	        (guint)sym, mods, clean_mods);

	for (i = 0; i < keybinds->len; i++) {
		GowlKeybindEntry *kb;

		kb = &g_array_index(keybinds, GowlKeybindEntry, i);
		if (GOWL_CLEANMASK(kb->modifiers) == clean_mods &&
		    kb->keysym == (guint)sym) {
			/* Dispatch the action */
			switch (kb->action) {
			case GOWL_ACTION_QUIT:
				gowl_compositor_quit(self);
				return TRUE;
			case GOWL_ACTION_SPAWN:
				if (kb->arg != NULL) {
					GError *err = NULL;
					if (!g_spawn_command_line_async(kb->arg,
					                               &err)) {
						g_warning("spawn '%s': %s",
						          kb->arg, err->message);
						g_error_free(err);
					}
				}
				return TRUE;
			case GOWL_ACTION_KILL_CLIENT: {
				GowlClient *sel = focustop(self, self->selmon);
				if (sel != NULL)
					wlr_xdg_toplevel_send_close(sel->xdg_toplevel);
				return TRUE;
			}
			case GOWL_ACTION_FOCUS_STACK: {
				GowlClient *sel, *found;
				GList *start, *l;
				gint dir;

				sel = focustop(self, self->selmon);
				if (sel == NULL)
					return TRUE;

				dir = (kb->arg != NULL) ? atoi(kb->arg) : 1;
				start = g_list_find(self->clients, sel);
				if (start == NULL)
					return TRUE;

				if (dir > 0) {
					/* Forward */
					l = start->next;
					if (l == NULL) l = self->clients;
					while (l != start) {
						found = (GowlClient *)l->data;
						if (VISIBLEON(found, self->selmon))
							break;
						l = l->next;
						if (l == NULL) l = self->clients;
					}
				} else {
					/* Backward */
					l = start->prev;
					if (l == NULL) l = g_list_last(self->clients);
					while (l != start) {
						found = (GowlClient *)l->data;
						if (VISIBLEON(found, self->selmon))
							break;
						l = l->prev;
						if (l == NULL) l = g_list_last(self->clients);
					}
				}
				found = (GowlClient *)l->data;
				gowl_compositor_focus_client(self, found, TRUE);
				return TRUE;
			}
			case GOWL_ACTION_SET_MFACT: {
				gdouble f;
				if (self->selmon == NULL || kb->arg == NULL)
					return TRUE;
				f = g_ascii_strtod(kb->arg, NULL) + self->selmon->mfact;
				if (f < 0.1 || f > 0.9)
					return TRUE;
				self->selmon->mfact = f;
				gowl_compositor_arrange(self, self->selmon);
				return TRUE;
			}
			case GOWL_ACTION_INC_NMASTER: {
				gint delta;
				if (self->selmon == NULL || kb->arg == NULL)
					return TRUE;
				delta = atoi(kb->arg);
				if (self->selmon->nmaster + delta >= 0)
					self->selmon->nmaster += delta;
				gowl_compositor_arrange(self, self->selmon);
				return TRUE;
			}
			case GOWL_ACTION_TOGGLE_FLOAT: {
				GowlClient *sel = focustop(self, self->selmon);
				if (sel != NULL && !sel->isfullscreen)
					setfloating(self, sel, !sel->isfloating);
				return TRUE;
			}
			case GOWL_ACTION_TOGGLE_FULLSCREEN: {
				GowlClient *sel = focustop(self, self->selmon);
				if (sel != NULL)
					setfullscreen(self, sel, !sel->isfullscreen);
				return TRUE;
			}
			case GOWL_ACTION_TAG_VIEW: {
				guint32 newtags;
				guint32 occupied;
				GList *cl;
				if (self->selmon == NULL || kb->arg == NULL)
					return TRUE;
				newtags = (guint32)atoi(kb->arg) & TAGMASK;
				if (newtags == 0)
					return TRUE;
				self->selmon->tagset[self->selmon->seltags] = newtags;
				gowl_compositor_focus_client(self, focustop(self, self->selmon), TRUE);
				gowl_compositor_arrange(self, self->selmon);

				/* Push tag state to IPC subscribers */
				if (self->ipc != NULL) {
					occupied = 0;
					for (cl = self->clients; cl != NULL; cl = cl->next) {
						GowlClient *tc = (GowlClient *)cl->data;
						if (tc->mon == self->selmon)
							occupied |= tc->tags;
					}
					gowl_ipc_push_event(self->ipc,
						"EVENT tags %s %u %u 0 %u",
						self->selmon->wlr_output->name,
						newtags, occupied, newtags);
				}
				return TRUE;
			}
			case GOWL_ACTION_TAG_SET: {
				GowlClient *sel;
				guint32 newtags;
				guint32 occupied;
				guint32 active;
				GList *cl;
				if (self->selmon == NULL || kb->arg == NULL)
					return TRUE;
				sel = focustop(self, self->selmon);
				if (sel == NULL)
					return TRUE;
				newtags = (guint32)atoi(kb->arg) & TAGMASK;
				if (newtags == 0)
					return TRUE;
				sel->tags = newtags;
				gowl_compositor_focus_client(self, focustop(self, self->selmon), TRUE);
				gowl_compositor_arrange(self, self->selmon);

				/* Push tag state to IPC subscribers */
				if (self->ipc != NULL) {
					active = self->selmon->tagset[self->selmon->seltags];
					occupied = 0;
					for (cl = self->clients; cl != NULL; cl = cl->next) {
						GowlClient *tc = (GowlClient *)cl->data;
						if (tc->mon == self->selmon)
							occupied |= tc->tags;
					}
					gowl_ipc_push_event(self->ipc,
						"EVENT tags %s %u %u 0 %u",
						self->selmon->wlr_output->name,
						active, occupied, active);
				}
				return TRUE;
			}
			case GOWL_ACTION_SET_LAYOUT: {
				if (self->selmon == NULL)
					return TRUE;
				if (kb->arg != NULL && g_strcmp0(kb->arg, "monocle") == 0)
					self->selmon->sellt = 1;
				else
					self->selmon->sellt = 0;
				gowl_compositor_arrange(self, self->selmon);

				/* Push layout to IPC subscribers */
				if (self->ipc != NULL) {
					gowl_ipc_push_event(self->ipc,
						"EVENT layout %s %s",
						self->selmon->wlr_output->name,
						self->selmon->layout_symbol != NULL
							? self->selmon->layout_symbol
							: "tile");
				}
				return TRUE;
			}
			case GOWL_ACTION_ZOOM: {
				gowl_compositor_zoom_client(self, NULL);
				return TRUE;
			}
			case GOWL_ACTION_FOCUS_MONITOR: {
				/*
				 * Move keyboard focus to the next/previous monitor.
				 * arg "+1" = next, "-1" = previous.
				 * Ported from dwl's focusmon().
				 */
				GowlMonitor *target;
				GList *cur, *next;
				gint dir;

				if (self->selmon == NULL || kb->arg == NULL)
					return TRUE;

				dir = atoi(kb->arg);
				cur = g_list_find(self->monitors, self->selmon);
				if (cur == NULL)
					return TRUE;

				if (dir > 0) {
					next = cur->next;
					if (next == NULL)
						next = self->monitors;
				} else {
					next = cur->prev;
					if (next == NULL)
						next = g_list_last(self->monitors);
				}

				target = (GowlMonitor *)next->data;
				if (target != self->selmon) {
					self->selmon = target;
					gowl_compositor_focus_client(self,
						focustop(self, target), TRUE);
				}
				return TRUE;
			}
			case GOWL_ACTION_MOVE_TO_MONITOR: {
				/*
				 * Move the focused client to the next/previous
				 * monitor.  arg "+1" = next, "-1" = previous.
				 * Ported from dwl's tagmon().
				 */
				GowlClient *sel;
				GowlMonitor *target;
				GList *cur, *next;
				gint dir;

				sel = focustop(self, self->selmon);
				if (sel == NULL || self->selmon == NULL || kb->arg == NULL)
					return TRUE;

				dir = atoi(kb->arg);
				cur = g_list_find(self->monitors, self->selmon);
				if (cur == NULL)
					return TRUE;

				if (dir > 0) {
					next = cur->next;
					if (next == NULL)
						next = self->monitors;
				} else {
					next = cur->prev;
					if (next == NULL)
						next = g_list_last(self->monitors);
				}

				target = (GowlMonitor *)next->data;
				if (target != self->selmon)
					setmon(self, sel, target, 0);
				return TRUE;
			}
			case GOWL_ACTION_TAG_TOGGLE_VIEW: {
				/*
				 * Toggle the visibility of a specific tag on the
				 * current monitor.  arg is the tag bitmask.
				 * Ported from dwl's toggleview().
				 */
				guint32 newtags;
				guint32 occupied;
				GList *cl;

				if (self->selmon == NULL || kb->arg == NULL)
					return TRUE;

				newtags = self->selmon->tagset[self->selmon->seltags] ^
				          ((guint32)atoi(kb->arg) & TAGMASK);

				/* Must have at least one tag visible */
				if (newtags == 0)
					return TRUE;

				self->selmon->tagset[self->selmon->seltags] = newtags;
				gowl_compositor_focus_client(self,
					focustop(self, self->selmon), TRUE);
				gowl_compositor_arrange(self, self->selmon);

				/* Push tag state to IPC subscribers */
				if (self->ipc != NULL) {
					occupied = 0;
					for (cl = self->clients; cl != NULL; cl = cl->next) {
						GowlClient *tc = (GowlClient *)cl->data;
						if (tc->mon == self->selmon)
							occupied |= tc->tags;
					}
					gowl_ipc_push_event(self->ipc,
						"EVENT tags %s %u %u 0 %u",
						self->selmon->wlr_output->name,
						newtags, occupied, newtags);
				}
				return TRUE;
			}
			case GOWL_ACTION_TAG_TOGGLE: {
				/*
				 * Toggle a specific tag on the focused client.
				 * arg is the tag bitmask.
				 * Ported from dwl's toggletag().
				 */
				GowlClient *sel;
				guint32 newtags;
				guint32 occupied;
				guint32 active;
				GList *cl;

				if (self->selmon == NULL || kb->arg == NULL)
					return TRUE;

				sel = focustop(self, self->selmon);
				if (sel == NULL)
					return TRUE;

				newtags = sel->tags ^ ((guint32)atoi(kb->arg) & TAGMASK);

				/* Client must have at least one tag */
				if (newtags == 0)
					return TRUE;

				sel->tags = newtags;
				gowl_compositor_focus_client(self,
					focustop(self, self->selmon), TRUE);
				gowl_compositor_arrange(self, self->selmon);

				/* Push tag state to IPC subscribers */
				if (self->ipc != NULL) {
					active = self->selmon->tagset[self->selmon->seltags];
					occupied = 0;
					for (cl = self->clients; cl != NULL; cl = cl->next) {
						GowlClient *tc = (GowlClient *)cl->data;
						if (tc->mon == self->selmon)
							occupied |= tc->tags;
					}
					gowl_ipc_push_event(self->ipc,
						"EVENT tags %s %u %u 0 %u",
						self->selmon->wlr_output->name,
						active, occupied, active);
				}
				return TRUE;
			}
			case GOWL_ACTION_CYCLE_LAYOUT: {
				/*
				 * Cycle between tile (0) and monocle (1) layouts
				 * on the selected monitor.
				 */
				if (self->selmon == NULL)
					return TRUE;

				self->selmon->sellt = (self->selmon->sellt + 1) % 2;
				gowl_compositor_arrange(self, self->selmon);

				/* Push layout to IPC subscribers */
				if (self->ipc != NULL) {
					gowl_ipc_push_event(self->ipc,
						"EVENT layout %s %s",
						self->selmon->wlr_output->name,
						self->selmon->layout_symbol != NULL
							? self->selmon->layout_symbol
							: "tile");
				}
				return TRUE;
			}
			case GOWL_ACTION_RELOAD_CONFIG: {
				/*
				 * Reload the YAML configuration from disk.  Keybinds
				 * and appearance settings take effect immediately.
				 * The new config object is intentionally kept alive
				 * (the compositor borrows the reference).
				 */
				GowlConfig *new_config;
				GError *err = NULL;

				new_config = gowl_config_new();
				if (!gowl_config_load_yaml_from_search_path(new_config,
				                                            &err)) {
					g_warning("reload_config: %s", err->message);
					g_error_free(err);
					g_object_unref(new_config);
					return TRUE;
				}

				/* Drop ref on old config if compositor owns one */
				if (self->config != NULL)
					g_object_unref(self->config);

				gowl_compositor_set_config(self, new_config);
				g_info("Configuration reloaded");

				/* Re-arrange all monitors with new settings */
				{
					GList *ml;
					for (ml = self->monitors; ml != NULL; ml = ml->next) {
						GowlMonitor *m = (GowlMonitor *)ml->data;
						gowl_compositor_arrange(self, m);
					}
				}
				return TRUE;
			}
			case GOWL_ACTION_IPC_COMMAND: {
				/*
				 * Execute an arbitrary IPC command string.
				 * The arg is forwarded to the IPC handler.
				 */
				if (self->ipc != NULL && kb->arg != NULL) {
					gowl_ipc_push_event(self->ipc,
						"EVENT command %s", kb->arg);
				}
				return TRUE;
			}
			case GOWL_ACTION_LOCK:
				if (self->module_mgr != NULL && !self->locked)
					gowl_module_manager_dispatch_lock(
						self->module_mgr, (gpointer)self);
				return TRUE;
			case GOWL_ACTION_NONE:
			case GOWL_ACTION_CUSTOM:
			default:
				g_debug("Unhandled action %d for keybind",
				        kb->action);
				return TRUE;
			}
		}
	}

	return FALSE;
}

/**
 * on_kb_key:
 *
 * Called when a key is pressed or released.  Extracts keysyms,
 * checks against configured keybinds, and forwards unconsumed
 * events to the focused client via the seat.
 * Ported from dwl's keypress().
 */
static void
on_kb_key(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	struct wlr_keyboard_key_event *event;
	struct wlr_keyboard *kb;
	const xkb_keysym_t *syms;
	guint32 mods;
	gint nsyms, i;
	gboolean handled;
	xkb_keycode_t keycode;

	self = wl_container_of(listener, self, kb_key);
	event = (struct wlr_keyboard_key_event *)data;
	kb = &self->wlr_kb_group->keyboard;
	keycode = event->keycode + 8;

	/* Translate keycode to keysyms using XKB state
	 * (includes Shift / layout transforms).
	 */
	nsyms = xkb_state_key_get_syms(kb->xkb_state, keycode, &syms);
	mods = wlr_keyboard_get_modifiers(kb);

	if (nsyms > 0 && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		g_info("on_kb_key: keycode=%u sym=0x%04x mods=0x%x clean=0x%x",
		       keycode, (guint)syms[0], mods, GOWL_CLEANMASK(mods));
	}

	/* Notify idle system of activity */
	wlr_idle_notifier_v1_notify_activity(self->idle_notifier,
	                                     self->wlr_seat);

	/* When session is locked, route all key events exclusively to
	 * the lock handler module.  No compositor keybinds fire and no
	 * events are forwarded to clients.
	 */
	if (self->locked) {
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED &&
		    self->module_mgr != NULL) {
			guint32 codepoint;

			codepoint = xkb_state_key_get_utf32(
				kb->xkb_state, keycode);
			for (i = 0; i < nsyms; i++) {
				gowl_module_manager_dispatch_lock_key(
					self->module_mgr,
					(guint)syms[i], codepoint, TRUE);
			}
		}
		/* Cancel key repeat during lock */
		wl_event_source_timer_update(self->key_repeat_source, 0);
		return;
	}

	/* Notify lock handlers of activity for idle timer reset */
	if (self->module_mgr != NULL)
		gowl_module_manager_notify_lock_activity(self->module_mgr);

	handled = FALSE;

	/* Only check keybinds on press events */
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* First try the state-resolved keysyms (handles things
		 * like XKB_KEY_Return that don't change with Shift).
		 */
		for (i = 0; i < nsyms; i++) {
			if (keybinding(self, mods, syms[i])) {
				handled = TRUE;
				break;
			}
		}

		/* If not handled, try the base (level-0) keysym.
		 * This is needed because the config parser stores
		 * lowercase keysyms (e.g. XKB_KEY_c = 0x63), but when
		 * Shift is held xkb_state_key_get_syms returns the
		 * shifted keysym (e.g. XKB_KEY_C = 0x43).  By also
		 * checking the raw keysym from layout level 0, we match
		 * "Super+Shift+c" correctly.
		 */
		if (!handled) {
			const xkb_keysym_t *raw_syms;
			xkb_layout_index_t layout;
			gint n_raw;

			layout = xkb_state_key_get_layout(kb->xkb_state,
			                                  keycode);
			n_raw = xkb_keymap_key_get_syms_by_level(
				kb->keymap, keycode, layout, 0, &raw_syms);

			if (n_raw > 0) {
				g_debug("on_kb_key: level-0 fallback sym=0x%04x (n=%d)",
				        (guint)raw_syms[0], n_raw);
			}

			for (i = 0; i < n_raw; i++) {
				if (keybinding(self, mods, raw_syms[i])) {
					handled = TRUE;
					break;
				}
			}
		}
	}

	if (!handled && self->key_intercept_func != NULL) {
		for (i = 0; i < nsyms; i++) {
			if (self->key_intercept_func(
				    self, mods, (guint)syms[i],
				    event->keycode,
				    event->state == WL_KEYBOARD_KEY_STATE_PRESSED,
				    self->key_intercept_data)) {
				handled = TRUE;
				break;
			}
		}
	}

	if (!handled) {
		/* Forward to the focused client */
		wlr_seat_set_keyboard(self->wlr_seat, kb);
		wlr_seat_keyboard_notify_key(self->wlr_seat,
		                             event->time_msec,
		                             event->keycode,
		                             event->state);
	}

	/* Set up key repeat state */
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && handled) {
		self->kb_nsyms   = nsyms;
		self->kb_keysyms = syms;
		self->kb_mods    = mods;
		if (kb->repeat_info.rate > 0 && kb->repeat_info.delay > 0)
			wl_event_source_timer_update(
				self->key_repeat_source,
				kb->repeat_info.delay);
	} else {
		self->kb_nsyms = 0;
		wl_event_source_timer_update(self->key_repeat_source, 0);
	}
}

/**
 * on_kb_modifiers:
 *
 * Called when modifier keys change.  Forwards to the seat so
 * clients see the updated modifier state.
 * Ported from dwl's keypressmod().
 */
static void
on_kb_modifiers(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;

	self = wl_container_of(listener, self, kb_modifiers);
	(void)data;

	wlr_seat_set_keyboard(self->wlr_seat,
	                      &self->wlr_kb_group->keyboard);
	wlr_seat_keyboard_notify_modifiers(self->wlr_seat,
	                                   &self->wlr_kb_group->keyboard.modifiers);
}

/**
 * on_key_repeat:
 *
 * Timer callback for key repeat.  Re-dispatches the last pressed
 * keybind at the configured repeat rate.
 */
static int
on_key_repeat(void *data)
{
	GowlCompositor *self;
	gint i;

	self = (GowlCompositor *)data;

	/* Suppress key repeat while session is locked */
	if (self->locked)
		return 0;

	if (self->kb_nsyms > 0 &&
	    self->wlr_kb_group->keyboard.repeat_info.rate > 0) {
		/* Re-fire the keybind */
		for (i = 0; i < self->kb_nsyms; i++)
			keybinding(self, self->kb_mods, self->kb_keysyms[i]);

		/* Schedule next repeat */
		wl_event_source_timer_update(
			self->key_repeat_source,
			1000 / self->wlr_kb_group->keyboard.repeat_info.rate);
	}

	return 0;
}

/* -----------------------------------------------------------
 * Cursor callbacks
 * ----------------------------------------------------------- */

/**
 * gowl_compositor_motionnotify:
 * @self: the #GowlCompositor
 * @time_msec: event timestamp in milliseconds (0 for synthetic)
 *
 * Common cursor motion handler.  Handles interactive move/resize,
 * sloppy focus, and pointer focus updates.
 * Ported from dwl's motionnotify().
 */
void
gowl_compositor_motionnotify(GowlCompositor *self, guint32 time_msec)
{
	gdouble sx, sy;
	GowlClient *c;
	struct wlr_surface *surface;

	sx = sy = 0;
	c = NULL;
	surface = NULL;

	/* Update drag icon position */
	if (self->drag_icon != NULL)
		wlr_scene_node_set_position(&self->drag_icon->node,
		                            (gint)round(self->wlr_cursor->x),
		                            (gint)round(self->wlr_cursor->y));

	/* Handle interactive move */
	if (self->cursor_mode == GOWL_CURSOR_MODE_MOVE) {
		struct wlr_box geo;

		geo.x = (gint)round(self->wlr_cursor->x) - (gint)self->grab_x;
		geo.y = (gint)round(self->wlr_cursor->y) - (gint)self->grab_y;
		geo.width = self->grabbed_client->geom.width;
		geo.height = self->grabbed_client->geom.height;
		resize_client(self, self->grabbed_client, geo, TRUE);
		return;
	}

	/* Handle interactive resize */
	if (self->cursor_mode == GOWL_CURSOR_MODE_RESIZE) {
		struct wlr_box geo;

		geo.x = self->grabbed_client->geom.x;
		geo.y = self->grabbed_client->geom.y;
		geo.width = (gint)round(self->wlr_cursor->x) - geo.x;
		geo.height = (gint)round(self->wlr_cursor->y) - geo.y;
		resize_client(self, self->grabbed_client, geo, TRUE);
		return;
	}

	/* Find surface under cursor */
	xytonode(self, self->wlr_cursor->x, self->wlr_cursor->y,
	         &surface, &c, &sx, &sy);

	/* Update selmon based on cursor position */
	if (time_msec) {
		GowlMonitor *m = xytomon(self, self->wlr_cursor->x,
		                         self->wlr_cursor->y);
		if (m != NULL)
			self->selmon = m;
	}

	/* If no surface under cursor, set default cursor image */
	if (surface == NULL && self->wlr_seat->drag == NULL)
		wlr_cursor_set_xcursor(self->wlr_cursor,
		                       self->xcursor_mgr, "default");

	pointerfocus(self, c, surface, sx, sy, time_msec);
}

static void
on_cursor_motion(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	struct wlr_pointer_motion_event *event;

	self = wl_container_of(listener, self, cursor_motion);
	event = (struct wlr_pointer_motion_event *)data;

	wlr_cursor_move(self->wlr_cursor, &event->pointer->base,
	                event->delta_x, event->delta_y);
	gowl_compositor_motionnotify(self, event->time_msec);
}

static void
on_cursor_motion_abs(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	struct wlr_pointer_motion_absolute_event *event;

	self = wl_container_of(listener, self, cursor_motion_absolute);
	event = (struct wlr_pointer_motion_absolute_event *)data;

	wlr_cursor_warp_absolute(self->wlr_cursor, &event->pointer->base,
	                         event->x, event->y);
	gowl_compositor_motionnotify(self, event->time_msec);
}

/**
 * on_cursor_button:
 *
 * Handles mouse button events.  On press: focuses the client under
 * the cursor.  On release: ends interactive move/resize.
 * Ported from dwl's buttonpress().
 */
static void
on_cursor_button(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	struct wlr_pointer_button_event *event;
	GowlClient *c;

	self = wl_container_of(listener, self, cursor_button);
	event = (struct wlr_pointer_button_event *)data;

	wlr_idle_notifier_v1_notify_activity(self->idle_notifier,
	                                     self->wlr_seat);

	switch (event->state) {
	case WL_POINTER_BUTTON_STATE_PRESSED:
		self->cursor_mode = GOWL_CURSOR_MODE_PRESSED;

		if (self->locked)
			break;

		/* Focus client under cursor on click.
		 * Embedded clients receive pointer events but do not
		 * steal keyboard focus — Emacs keybinds take priority.
		 */
		xytonode(self, self->wlr_cursor->x, self->wlr_cursor->y,
		         NULL, &c, NULL, NULL);
		if (c != NULL && !gowl_client_get_embedded(c))
			gowl_compositor_focus_client(self, c, TRUE);
		break;

	case WL_POINTER_BUTTON_STATE_RELEASED:
		/* End interactive move/resize on button release */
		if (!self->locked &&
		    self->cursor_mode != GOWL_CURSOR_MODE_NORMAL &&
		    self->cursor_mode != GOWL_CURSOR_MODE_PRESSED) {
			wlr_cursor_set_xcursor(self->wlr_cursor,
			                       self->xcursor_mgr, "default");
			self->cursor_mode = GOWL_CURSOR_MODE_NORMAL;

			/* Drop the window on its new monitor */
			if (self->grabbed_client != NULL) {
				GowlMonitor *m;
				m = xytomon(self, self->wlr_cursor->x,
				            self->wlr_cursor->y);
				self->selmon = m;
				setmon(self, self->grabbed_client, m, 0);
				self->grabbed_client = NULL;
			}
			return;
		}
		self->cursor_mode = GOWL_CURSOR_MODE_NORMAL;
		break;
	}

	/* Forward button event to the focused client */
	wlr_seat_pointer_notify_button(self->wlr_seat,
	                               event->time_msec,
	                               event->button,
	                               event->state);
}

static void
on_cursor_axis(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	struct wlr_pointer_axis_event *event;

	self = wl_container_of(listener, self, cursor_axis);
	event = (struct wlr_pointer_axis_event *)data;

	wlr_idle_notifier_v1_notify_activity(self->idle_notifier,
	                                     self->wlr_seat);

	/* Forward scroll event to the focused client */
	wlr_seat_pointer_notify_axis(self->wlr_seat,
	                             event->time_msec,
	                             event->orientation,
	                             event->delta,
	                             event->delta_discrete,
	                             event->source,
	                             event->relative_direction);
}

static void
on_cursor_frame(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;

	self = wl_container_of(listener, self, cursor_frame);
	(void)data;

	wlr_seat_pointer_notify_frame(self->wlr_seat);
}

/* -----------------------------------------------------------
 * Seat callbacks
 * ----------------------------------------------------------- */

static void
on_request_cursor(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	struct wlr_seat_pointer_request_set_cursor_event *event;

	self = wl_container_of(listener, self, request_cursor);
	event = (struct wlr_seat_pointer_request_set_cursor_event *)data;

	/* Only honour cursor changes from the focused client */
	if (self->wlr_seat->pointer_state.focused_client == event->seat_client)
		wlr_cursor_set_surface(self->wlr_cursor,
		                       event->surface,
		                       event->hotspot_x,
		                       event->hotspot_y);
}

static void
on_request_set_sel(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	struct wlr_seat_request_set_selection_event *event;

	self = wl_container_of(listener, self, request_set_sel);
	event = (struct wlr_seat_request_set_selection_event *)data;

	wlr_seat_set_selection(self->wlr_seat,
	                       event->source, event->serial);

	/* Notify GObject layer that clipboard changed */
	if (self->seat != NULL)
		gowl_seat_emit_clipboard_changed(self->seat);
}

static void
on_request_set_psel(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	struct wlr_seat_request_set_primary_selection_event *event;

	self = wl_container_of(listener, self, request_set_psel);
	event = (struct wlr_seat_request_set_primary_selection_event *)data;

	wlr_seat_set_primary_selection(self->wlr_seat,
	                               event->source, event->serial);

	/* Notify GObject layer that primary selection changed */
	if (self->seat != NULL)
		gowl_seat_emit_primary_selection_changed(self->seat);
}

static void
on_request_start_drag(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	struct wlr_seat_request_start_drag_event *event;

	self = wl_container_of(listener, self, request_start_drag);
	event = (struct wlr_seat_request_start_drag_event *)data;

	/* Accept drags from the focused client */
	if (wlr_seat_validate_pointer_grab_serial(
		    self->wlr_seat, event->origin, event->serial))
		wlr_seat_start_pointer_drag(self->wlr_seat,
		                            event->drag, event->serial);
	else
		wlr_data_source_destroy(event->drag->source);
}

static void
on_start_drag(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	struct wlr_drag *drag;

	self = wl_container_of(listener, self, start_drag);
	drag = (struct wlr_drag *)data;

	/* If the drag has an icon, place it in the drag_icon layer */
	if (drag->icon != NULL)
		wlr_scene_drag_icon_create(self->drag_icon, drag->icon);
}

/* -----------------------------------------------------------
 * XDG shell callbacks - client lifecycle
 * ----------------------------------------------------------- */

/**
 * on_new_xdg_toplevel:
 *
 * Called when a new XDG toplevel surface is created.
 * Allocates a GowlClient, registers surface listeners.
 * Ported from dwl's createnotify().
 */
static void
on_new_xdg_toplevel(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	struct wlr_xdg_toplevel *toplevel;
	GowlClient *c;
	gint border_width;

	self = wl_container_of(listener, self, new_xdg_toplevel);
	toplevel = (struct wlr_xdg_toplevel *)data;

	/* Create a GowlClient for this surface */
	c = (GowlClient *)g_object_new(GOWL_TYPE_CLIENT, NULL);
	c->xdg_toplevel = toplevel;
	c->compositor   = self;
	c->decoration   = NULL;
	toplevel->base->data = c;

	/* Set border width from config */
	border_width = 1;
	if (self->config != NULL)
		g_object_get(self->config, "border-width", &border_width, NULL);
	c->bw = (guint)border_width;

	/* Register surface event listeners */
	LISTEN(&toplevel->base->surface->events.commit,  &c->commit,   on_client_commit);
	LISTEN(&toplevel->base->surface->events.map,     &c->map,      on_client_map);
	LISTEN(&toplevel->base->surface->events.unmap,   &c->unmap,    on_client_unmap);
	LISTEN(&toplevel->events.destroy,                &c->destroy_surface, on_client_destroy);
	LISTEN(&toplevel->events.request_fullscreen,     &c->fullscreen, on_client_fullscreen);
	LISTEN(&toplevel->events.request_maximize,       &c->maximize,  on_client_maximize);
	LISTEN(&toplevel->events.set_title,              &c->set_title, on_client_set_title);

	g_debug("New XDG toplevel surface created");
}

/**
 * on_client_commit:
 *
 * Called on every surface commit.  On initial commit, sets WM
 * capabilities and configures size.  On subsequent commits,
 * handles resize completion.
 * Ported from dwl's commitnotify().
 */
static void
on_client_commit(struct wl_listener *listener, void *data)
{
	GowlClient *c;
	GowlCompositor *self;

	c = wl_container_of(listener, c, commit);
	self = c->compositor;

	if (c->xdg_toplevel->base->initial_commit) {
		/* Initial commit: set WM capabilities, apply decoration, configure */
		wlr_xdg_toplevel_set_wm_capabilities(c->xdg_toplevel,
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
		if (c->decoration != NULL)
			on_request_decoration_mode(
				&c->set_decoration_mode, c->decoration);
		wlr_xdg_toplevel_set_size(c->xdg_toplevel, 0, 0);
		return;
	}

	/* Scene not yet created (between initial commit and map) */
	if (c->scene == NULL)
		return;

	/* Subsequent commits: update geometry */
	resize_client(self, c, c->geom,
	              c->isfloating && !c->isfullscreen);

	/* Mark pending resize as completed */
	if (c->resize != 0 &&
	    c->resize <= c->xdg_toplevel->base->current.configure_serial)
		c->resize = 0;

	/* Re-apply client opacity.  wlr_scene_surface's commit handler
	 * resets the scene buffer opacity to 1.0 on every commit, so
	 * we must re-apply our custom alpha after each frame.
	 * Skip embedded clients — they render on top of their parent
	 * and should stay fully opaque (alpha 1.0). */
	if (c->alpha < 1.0f && !c->isembedded)
		gowl_client_set_alpha(c, c->alpha);
}

/**
 * on_client_map:
 *
 * Called when a client surface is ready to be displayed.
 * Creates the scene tree, borders, applies rules, and focuses.
 * Ported from dwl's mapnotify().
 */
static void
on_client_map(struct wl_listener *listener, void *data)
{
	GowlClient *c;
	GowlCompositor *self;
	gint i;

	c = wl_container_of(listener, c, map);
	self = c->compositor;
	(void)data;

	/* Create scene tree in the tiling layer */
	c->scene = wlr_scene_tree_create(self->layers[GOWL_SCENE_LAYER_TILE]);
	c->xdg_toplevel->base->surface->data = c->scene;
	/* Disabled until arrange() turns it on */
	wlr_scene_node_set_enabled(&c->scene->node, FALSE);

	/* Create the XDG surface scene tree within the client container.
	 * wlr_scene_xdg_surface_create adds an internal commit listener
	 * that resets scene buffer opacity to 1.0 on every frame.
	 * Re-register our commit listener AFTER this so gowl's handler
	 * fires last and can re-apply per-client alpha. */
	c->scene_surface = wlr_scene_xdg_surface_create(c->scene,
	                                                  c->xdg_toplevel->base);
	wl_list_remove(&c->commit.link);
	wl_signal_add(&c->xdg_toplevel->base->surface->events.commit,
	              &c->commit);
	c->scene->node.data = c;
	c->scene_surface->node.data = c;

	/* Get initial geometry from XDG surface */
	c->geom = c->xdg_toplevel->base->geometry;

	/* Create border rects */
	for (i = 0; i < 4; i++) {
		c->border[i] = wlr_scene_rect_create(c->scene, 0, 0,
		                                      self->unfocus_color);
		c->border[i]->node.data = c;
	}

	/* Tell the surface it's tiled on all edges */
	wlr_xdg_toplevel_set_tiled(c->xdg_toplevel,
		WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);

	/* Account for borders in geometry */
	c->geom.width  += 2 * (gint)c->bw;
	c->geom.height += 2 * (gint)c->bw;

	/* Insert into client lists */
	self->clients = g_list_prepend(self->clients, c);
	self->fstack  = g_list_prepend(self->fstack, c);

	/* Assign to selected monitor with current tags */
	setmon(self, c, self->selmon,
	       self->selmon ? self->selmon->tagset[self->selmon->seltags] : 1);

	/* Center floating clients within usable area if they have
	 * no explicit position (default 0,0 from XDG). */
	if (c->isfloating && !c->isfullscreen && c->mon != NULL) {
		if (c->geom.x == 0 && c->geom.y == 0) {
			c->geom.x = c->mon->w.x +
				(c->mon->w.width - c->geom.width) / 2;
			c->geom.y = c->mon->w.y +
				(c->mon->w.height - c->geom.height) / 2;
			resize_client(self, c, c->geom, TRUE);
		}
	}

	/* Check if this PID was registered for embedding.
	 * Mark it embedded+floating, reparent to OVERLAY (above FS),
	 * hide it, and re-arrange so tiling reclaims the space.
	 * The embedded flag prevents arrange() from touching this
	 * client's layer or visibility in the future. */
	if (self->prefloat_pids != NULL && self->prefloat_pids->len > 0) {
		pid_t cpid;
		guint pi;

		cpid = gowl_client_get_pid(c);
		for (pi = 0; pi < self->prefloat_pids->len; pi++) {
			if (g_array_index(self->prefloat_pids, pid_t, pi) == cpid) {
				g_array_remove_index_fast(self->prefloat_pids, pi);
				c->isfloating = TRUE;
				c->isembedded = TRUE;
				wlr_scene_node_reparent(&c->scene->node,
					self->layers[GOWL_SCENE_LAYER_OVERLAY]);
				wlr_scene_node_set_enabled(&c->scene->node, FALSE);
				/* Re-arrange: the embedded flag means arrange()
				   skips this client, so tiling reclaims all
				   space and our OVERLAY reparent is preserved. */
				gowl_compositor_arrange(self, c->mon);
				break;
			}
		}
	}

	/* Notify embedder callback (if registered). */
	if (self->client_map_func != NULL)
		self->client_map_func(self, c, self->client_map_data);

	g_signal_emit(self, compositor_signals[SIGNAL_CLIENT_ADDED], 0, c);

	g_debug("Client mapped: %s (%s)",
	        c->xdg_toplevel->title ? c->xdg_toplevel->title : "(untitled)",
	        c->xdg_toplevel->app_id ? c->xdg_toplevel->app_id : "(no app_id)");
}

/**
 * on_client_unmap:
 *
 * Called when a client surface is unmapped (hidden).
 * Removes from lists, destroys scene node.
 * Ported from dwl's unmapnotify().
 */
static void
on_client_unmap(struct wl_listener *listener, void *data)
{
	GowlClient *c;
	GowlCompositor *self;

	c = wl_container_of(listener, c, unmap);
	self = c->compositor;
	(void)data;

	g_signal_emit(self, compositor_signals[SIGNAL_CLIENT_REMOVED], 0, c);

	/* Cancel any interactive grab */
	if (c == self->grabbed_client) {
		self->cursor_mode = GOWL_CURSOR_MODE_NORMAL;
		self->grabbed_client = NULL;
	}

	/* Remove from compositor lists */
	self->clients = g_list_remove(self->clients, c);
	setmon(self, c, NULL, 0);
	self->fstack = g_list_remove(self->fstack, c);

	/* Destroy the scene tree (including borders) */
	wlr_scene_node_destroy(&c->scene->node);
	c->scene = NULL;
	c->scene_surface = NULL;
	memset(c->border, 0, sizeof(c->border));

	/* Restore focus */
	gowl_compositor_motionnotify(self, 0);

	g_debug("Client unmapped");
}

/**
 * on_client_destroy:
 *
 * Called when the XDG toplevel is destroyed.
 * Removes listeners and frees the GowlClient.
 * Ported from dwl's destroynotify().
 */
static void
on_client_destroy(struct wl_listener *listener, void *data)
{
	GowlClient *c;

	c = wl_container_of(listener, c, destroy_surface);
	(void)data;

	/* Remove all listeners */
	wl_list_remove(&c->destroy_surface.link);
	wl_list_remove(&c->set_title.link);
	wl_list_remove(&c->fullscreen.link);
	wl_list_remove(&c->maximize.link);
	wl_list_remove(&c->commit.link);
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);

	g_debug("Client destroyed");
	g_object_unref(c);
}

/**
 * on_client_fullscreen:
 *
 * Called when a client requests fullscreen toggle.
 */
static void
on_client_fullscreen(struct wl_listener *listener, void *data)
{
	GowlClient *c;

	c = wl_container_of(listener, c, fullscreen);
	(void)data;

	if (c->compositor != NULL)
		setfullscreen(c->compositor, c, !c->isfullscreen);
}

/**
 * on_client_maximize:
 *
 * Called when a client requests maximize.  We don't support
 * maximize, so just send an empty configure to ack the request.
 */
static void
on_client_maximize(struct wl_listener *listener, void *data)
{
	GowlClient *c;

	c = wl_container_of(listener, c, maximize);
	(void)data;

	if (c->xdg_toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(c->xdg_toplevel->base);
}

/**
 * on_client_set_title:
 *
 * Called when a client updates its title.
 */
static void
on_client_set_title(struct wl_listener *listener, void *data)
{
	GowlClient *c;
	GowlCompositor *self;
	GowlClient *focused;

	c = wl_container_of(listener, c, set_title);
	self = c->compositor;
	(void)data;

	g_free(c->title);
	c->title = g_strdup(c->xdg_toplevel->title);

	g_free(c->app_id);
	c->app_id = g_strdup(c->xdg_toplevel->app_id);

	/* Push title to IPC if this is the focused client */
	focused = focustop(self, self->selmon);
	if (self->ipc != NULL && focused == c)
		gowl_ipc_push_event(self->ipc, "EVENT title %s",
		                     c->title != NULL ? c->title : "");
}

/**
 * on_new_xdg_popup:
 *
 * Called when an XDG popup is created.  Defers scene tree creation
 * until the popup's initial commit, matching dwl's createpopup().
 */
static void
on_new_xdg_popup(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_popup *popup;

	(void)listener;
	popup = (struct wlr_xdg_popup *)data;

	/* Register a heap-allocated commit listener; the callback will
	 * create the scene surface on initial_commit and then free it. */
	LISTEN_STATIC(&popup->base->surface->events.commit, on_popup_commit);
}

/**
 * on_popup_commit:
 *
 * Called on each commit of a popup surface.  On the initial commit,
 * creates the scene XDG surface under the parent's scene tree,
 * constrains the popup to the monitor bounds, then removes itself.
 * Ported from dwl's commitpopup().
 */
static void
on_popup_commit(struct wl_listener *listener, void *data)
{
	struct wlr_surface *surface;
	struct wlr_xdg_popup *popup;
	struct wlr_scene_tree *parent_tree;
	struct wlr_box box;
	GowlClient *c;

	surface = (struct wlr_surface *)data;
	popup = wlr_xdg_popup_try_from_wlr_surface(surface);
	if (popup == NULL)
		goto done;

	if (!popup->base->initial_commit)
		return;

	if (popup->parent == NULL)
		goto done;

	parent_tree = (struct wlr_scene_tree *)popup->parent->data;
	if (parent_tree == NULL)
		goto done;

	popup->base->surface->data = wlr_scene_xdg_surface_create(
		parent_tree, popup->base);

	/* Walk up to find the owning client for constraining the popup
	 * to the monitor bounds */
	c = NULL;
	{
		struct wlr_surface *s;

		for (s = popup->parent; s != NULL; ) {
			struct wlr_xdg_surface *xdg;
			struct wlr_xdg_popup *p;

			xdg = wlr_xdg_surface_try_from_wlr_surface(s);
			if (xdg == NULL)
				break;

			if (xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
				c = (GowlClient *)xdg->data;
				break;
			}

			p = wlr_xdg_popup_try_from_wlr_surface(s);
			if (p != NULL)
				s = p->parent;
			else
				break;
		}
	}

	/* Constrain popup to monitor bounds */
	if (c != NULL && c->mon != NULL) {
		box = c->mon->w;
		box.x -= c->geom.x;
		box.y -= c->geom.y;
		wlr_xdg_popup_unconstrain_from_box(popup, &box);
	}

done:
	wl_list_remove(&listener->link);
	g_free(listener);
}

/* -----------------------------------------------------------
 * Layer shell map from wlr layer to gowl scene layer
 * ----------------------------------------------------------- */
static gint
layermap(enum zwlr_layer_shell_v1_layer layer)
{
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return GOWL_SCENE_LAYER_BG;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:     return GOWL_SCENE_LAYER_BOTTOM;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:        return GOWL_SCENE_LAYER_TOP;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:    return GOWL_SCENE_LAYER_OVERLAY;
	default:                                    return GOWL_SCENE_LAYER_TOP;
	}
}

/**
 * gowl_compositor_arrangelayers:
 *
 * Arranges all layer surfaces on monitor @m, computing exclusive
 * zones and updating the monitor's usable window area.
 * Ported from dwl's arrangelayers()/arrangelayer().
 */
void
gowl_compositor_arrangelayers(
	GowlCompositor *self,
	GowlMonitor    *m
){
	struct wlr_box usable_area;
	GList *l;
	gint pass;

	if (m == NULL || m->wlr_output == NULL)
		return;

	usable_area = m->m;

	/* Two-pass arrangement: exclusive first, then non-exclusive.
	 * wlr_scene_layer_surface_v1_configure() subtracts exclusive
	 * zones from usable_area when the surface has exclusive_zone > 0. */
	for (pass = 0; pass < 2; pass++) {
		for (l = m->layer_surfaces; l != NULL; l = l->next) {
			GowlLayerSurface *ls = (GowlLayerSurface *)l->data;

			if (ls->scene_layer_surface == NULL)
				continue;

			if (pass == 0 &&
			    ls->wlr_layer_surface->current.exclusive_zone <= 0)
				continue;
			if (pass == 1 &&
			    ls->wlr_layer_surface->current.exclusive_zone > 0)
				continue;

			wlr_scene_layer_surface_v1_configure(
				ls->scene_layer_surface, &m->m, &usable_area);
		}
	}

	/* Subtract bar insets.  The active bar provider reports separate
	   top and bottom insets so a two-bar layout (top + bottom) shrinks
	   the usable area from both edges. */
	if (self->module_mgr != NULL) {
		gint top = 0, bottom = 0;
		gowl_module_manager_get_bar_insets(self->module_mgr, m,
		                                    &top, &bottom);
		if (top > 0) {
			usable_area.y += top;
			usable_area.height -= top;
		}
		if (bottom > 0)
			usable_area.height -= bottom;
	}

	/* If usable area changed, update window area and re-tile */
	if (!wlr_box_equal(&usable_area, &m->w)) {
		m->w = usable_area;
		gowl_compositor_arrange(self, m);
	}

	/* Check for keyboard-interactive layer surfaces (top, overlay) */
	for (l = m->layer_surfaces; l != NULL; l = l->next) {
		GowlLayerSurface *ls = (GowlLayerSurface *)l->data;

		if (self->locked || !ls->mapped)
			continue;

		if (!ls->wlr_layer_surface->current.keyboard_interactive)
			continue;

		if (ls->wlr_layer_surface->current.layer >=
		    ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
			gowl_compositor_focus_client(self, NULL, FALSE);
			wlr_seat_keyboard_notify_enter(self->wlr_seat,
				ls->wlr_layer_surface->surface,
				NULL, 0, NULL);
			return;
		}
	}
}

/**
 * on_new_layer_surface:
 *
 * Called when a new layer shell surface is created.
 * Creates a GowlLayerSurface, registers lifecycle listeners, and
 * assigns it to the appropriate scene layer.
 * Ported from dwl's createlayersurface().
 */
static void
on_new_layer_surface(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	struct wlr_layer_surface_v1 *wlr_layer_surface;
	struct wlr_scene_tree *scene_layer;
	GowlLayerSurface *ls;
	GowlMonitor *m;

	self = wl_container_of(listener, self, new_layer_surface);
	wlr_layer_surface = (struct wlr_layer_surface_v1 *)data;

	/* Assign to selected monitor if not specified */
	if (wlr_layer_surface->output == NULL) {
		if (self->selmon == NULL || self->selmon->wlr_output == NULL) {
			wlr_layer_surface_v1_destroy(wlr_layer_surface);
			return;
		}
		wlr_layer_surface->output = self->selmon->wlr_output;
	}

	m = (GowlMonitor *)wlr_layer_surface->output->data;
	if (m == NULL)
		return;

	scene_layer = self->layers[layermap(wlr_layer_surface->pending.layer)];

	/* Create a GowlLayerSurface to wrap this layer surface */
	ls = (GowlLayerSurface *)g_object_new(GOWL_TYPE_LAYER_SURFACE, NULL);
	ls->wlr_layer_surface  = wlr_layer_surface;
	ls->mon                = m;
	ls->compositor         = self;
	ls->layer              = (gint)wlr_layer_surface->pending.layer;
	wlr_layer_surface->data = ls;

	/* Create the scene layer surface */
	ls->scene_layer_surface = wlr_scene_layer_surface_v1_create(
		scene_layer, wlr_layer_surface);
	ls->scene = ls->scene_layer_surface->tree;
	/* NOTE: do NOT set ls->scene->node.data here.  The xytonode()
	 * tree walk casts all non-NULL node.data as GowlClient*, so
	 * storing a GowlLayerSurface* would cause a type confusion
	 * crash when the pointer hovers over a layer surface. Layer
	 * surface data is accessed via wlr_layer_surface->data or
	 * wl_container_of() instead. */

	/* Register surface event listeners */
	LISTEN(&wlr_layer_surface->surface->events.commit,
	       &ls->commit, on_layer_commit);
	LISTEN(&wlr_layer_surface->surface->events.unmap,
	       &ls->unmap, on_layer_unmap);
	LISTEN(&wlr_layer_surface->events.destroy,
	       &ls->destroy_surface, on_layer_destroy);

	/* Add to monitor's layer surface list */
	m->layer_surfaces = g_list_append(m->layer_surfaces, ls);

	/* Send enter event for the output */
	wlr_surface_send_enter(wlr_layer_surface->surface,
	                       wlr_layer_surface->output);

	g_debug("New layer surface on %s (layer %d)",
	        m->wlr_output->name, wlr_layer_surface->pending.layer);
}

/**
 * on_layer_commit:
 *
 * Called on every layer surface commit.  On initial commit,
 * arranges layers immediately.  On subsequent commits, handles
 * layer changes and re-arranges.
 * Ported from dwl's commitlayersurfacenotify().
 */
static void
on_layer_commit(struct wl_listener *listener, void *data)
{
	GowlLayerSurface *ls;
	GowlCompositor *self;
	struct wlr_scene_tree *target_layer;
	struct wlr_layer_surface_v1_state old_state;

	ls = wl_container_of(listener, ls, commit);
	self = ls->compositor;
	(void)data;

	if (ls->wlr_layer_surface->initial_commit) {
		/* Temporarily set current state to pending for initial arrange */
		old_state = ls->wlr_layer_surface->current;
		ls->wlr_layer_surface->current = ls->wlr_layer_surface->pending;
		gowl_compositor_arrangelayers(self, ls->mon);
		ls->wlr_layer_surface->current = old_state;
		return;
	}

	if (ls->wlr_layer_surface->current.committed == 0 &&
	    ls->mapped == ls->wlr_layer_surface->surface->mapped)
		return;
	ls->mapped = ls->wlr_layer_surface->surface->mapped;

	/* Check if the surface moved to a different layer */
	target_layer = self->layers[layermap(
		ls->wlr_layer_surface->current.layer)];
	if (target_layer != ls->scene->node.parent) {
		wlr_scene_node_reparent(&ls->scene->node, target_layer);
		ls->layer = (gint)ls->wlr_layer_surface->current.layer;
	}

	gowl_compositor_arrangelayers(self, ls->mon);
}

/**
 * on_layer_unmap:
 *
 * Called when a layer surface is unmapped (hidden).
 * Ported from dwl's unmaplayersurfacenotify().
 */
static void
on_layer_unmap(struct wl_listener *listener, void *data)
{
	GowlLayerSurface *ls;
	GowlCompositor *self;

	ls = wl_container_of(listener, ls, unmap);
	self = ls->compositor;
	(void)data;

	ls->mapped = FALSE;
	wlr_scene_node_set_enabled(&ls->scene->node, FALSE);

	if (ls->mon != NULL)
		gowl_compositor_arrangelayers(self, ls->mon);

	/* If this layer surface had keyboard focus, restore client focus */
	if (ls->wlr_layer_surface->surface ==
	    self->wlr_seat->keyboard_state.focused_surface)
		gowl_compositor_focus_client(self,
			focustop(self, self->selmon), TRUE);

	gowl_compositor_motionnotify(self, 0);
}

/**
 * on_layer_destroy:
 *
 * Called when a layer surface is destroyed.
 * Removes listeners, removes from monitor list, frees GowlLayerSurface.
 * Ported from dwl's destroylayersurfacenotify().
 */
static void
on_layer_destroy(struct wl_listener *listener, void *data)
{
	GowlLayerSurface *ls;

	ls = wl_container_of(listener, ls, destroy_surface);
	(void)data;

	/* Remove event listeners */
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->unmap.link);
	wl_list_remove(&ls->destroy_surface.link);

	/* Remove from monitor's layer surface list */
	if (ls->mon != NULL)
		ls->mon->layer_surfaces = g_list_remove(
			ls->mon->layer_surfaces, ls);

	g_debug("Layer surface destroyed");
	g_object_unref(ls);
}

static void
on_new_xdg_decoration(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_toplevel_decoration_v1 *dec;
	GowlClient *c;

	(void)listener;
	dec = (struct wlr_xdg_toplevel_decoration_v1 *)data;
	c = (GowlClient *)dec->toplevel->base->data;
	c->decoration = dec;

	LISTEN(&dec->events.request_mode, &c->set_decoration_mode,
	       on_request_decoration_mode);
	LISTEN(&dec->events.destroy, &c->destroy_decoration,
	       on_destroy_decoration);

	on_request_decoration_mode(&c->set_decoration_mode, dec);
}

/**
 * on_request_decoration_mode:
 *
 * Called when a client requests a decoration mode change.
 * Forces server-side decorations, but only if the XDG surface
 * has been initialized (i.e. has received its initial commit).
 * Ported from dwl's requestdecorationmode().
 */
static void
on_request_decoration_mode(struct wl_listener *listener, void *data)
{
	GowlClient *c;

	c = wl_container_of(listener, c, set_decoration_mode);

	if (c->xdg_toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(c->decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

/**
 * on_destroy_decoration:
 *
 * Called when the decoration object is destroyed by the client.
 * Cleans up the decoration pointer and removes listeners.
 */
static void
on_destroy_decoration(struct wl_listener *listener, void *data)
{
	GowlClient *c;

	c = wl_container_of(listener, c, destroy_decoration);
	c->decoration = NULL;

	wl_list_remove(&c->set_decoration_mode.link);
	wl_list_remove(&c->destroy_decoration.link);
}

/* -----------------------------------------------------------
 * Session lock callbacks
 * ----------------------------------------------------------- */

/**
 * on_new_session_lock:
 *
 * Called when a lock client (e.g. swaylock) acquires the session lock.
 * Enables the locked background, unfocuses all clients, and listens
 * for lock surface creation and unlock events.
 * Ported from dwl's locksession().
 */
static void
on_new_session_lock(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	struct wlr_session_lock_v1 *session_lock;

	self = wl_container_of(listener, self, new_session_lock);
	session_lock = (struct wlr_session_lock_v1 *)data;

	/* Enable locked background */
	wlr_scene_node_set_enabled(&self->locked_bg->node, TRUE);

	/* Reject if already locked */
	if (self->cur_lock != NULL) {
		wlr_session_lock_v1_destroy(session_lock);
		return;
	}

	/* Unfocus all clients */
	gowl_compositor_focus_client(self, NULL, FALSE);

	/* Store lock reference */
	self->cur_lock = session_lock;
	self->locked = TRUE;

	/* Register lock event listeners */
	LISTEN(&session_lock->events.new_surface,
	       &self->lock_new_surface, on_lock_surface_create);
	LISTEN(&session_lock->events.destroy,
	       &self->lock_destroy, on_session_lock_destroy);
	LISTEN(&session_lock->events.unlock,
	       &self->lock_unlock, on_session_unlock);

	/* Confirm lock to the client */
	wlr_session_lock_v1_send_locked(session_lock);

	g_debug("Session locked");
}

/**
 * on_lock_surface_create:
 *
 * Called when the lock client creates a lock surface for an output.
 * Creates a scene tree for the lock surface in the block layer.
 */
static void
on_lock_surface_create(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_scene_tree *tree;
	GowlMonitor *m;

	self = wl_container_of(listener, self, lock_new_surface);
	lock_surface = (struct wlr_session_lock_surface_v1 *)data;

	/* Create scene surface in the block layer */
	tree = wlr_scene_subsurface_tree_create(
		self->layers[GOWL_SCENE_LAYER_BLOCK],
		lock_surface->surface);

	/* Position on the correct output */
	m = (GowlMonitor *)lock_surface->output->data;
	if (m != NULL) {
		wlr_scene_node_set_position(&tree->node, m->m.x, m->m.y);
		wlr_session_lock_surface_v1_configure(lock_surface,
			(guint32)m->m.width, (guint32)m->m.height);
	}

	/* Give the lock surface keyboard focus */
	wlr_seat_keyboard_notify_enter(self->wlr_seat,
		lock_surface->surface, NULL, 0, NULL);
}

/**
 * on_session_lock_destroy:
 *
 * Called when the lock client disconnects without unlocking.
 * The session stays locked.
 */
static void
on_session_lock_destroy(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;

	self = wl_container_of(listener, self, lock_destroy);
	(void)data;

	wl_list_remove(&self->lock_new_surface.link);
	wl_list_remove(&self->lock_destroy.link);
	wl_list_remove(&self->lock_unlock.link);

	self->cur_lock = NULL;

	/* Session remains locked; locked_bg stays enabled */
	g_debug("Lock client destroyed (session still locked)");
}

/**
 * on_session_unlock:
 *
 * Called when the lock client unlocks the session.
 * Disables the locked background, restores focus.
 */
static void
on_session_unlock(struct wl_listener *listener, void *data)
{
	GowlCompositor *self;

	self = wl_container_of(listener, self, lock_unlock);
	(void)data;

	wl_list_remove(&self->lock_new_surface.link);
	wl_list_remove(&self->lock_destroy.link);
	wl_list_remove(&self->lock_unlock.link);

	self->cur_lock = NULL;
	self->locked = FALSE;

	wlr_scene_node_set_enabled(&self->locked_bg->node, FALSE);

	/* Restore focus and cursor */
	gowl_compositor_focus_client(self,
		focustop(self, self->selmon), TRUE);
	gowl_compositor_motionnotify(self, 0);

	g_debug("Session unlocked");
}
