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

#include "gowlbar-app.h"
#include "gowlbar-output.h"
#include "gowlbar-enums.h"
#include "gowlbar-widget.h"
#include "gowlbar-tag-widget.h"
#include "gowlbar-layout-widget.h"
#include "gowlbar-title-widget.h"
#include "gowlbar-status-widget.h"

#include <glib.h>
#include <gio/gio.h>
#include <wayland-client.h>
#include <string.h>
#include <unistd.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/**
 * GowlbarApp:
 *
 * Main bar application object.  Connects to the Wayland display,
 * binds compositor, shm, output, seat, and layer-shell globals,
 * integrates the wl_display fd into the GLib main loop, manages
 * per-output #GowlbarOutput instances, creates widgets, and
 * reads stdin for status text.
 */
struct _GowlbarApp {
	GObject parent_instance;

	/* wayland globals */
	struct wl_display             *wl_display;
	struct wl_registry            *wl_registry;
	struct wl_compositor          *wl_compositor;
	struct wl_shm                 *wl_shm;
	struct wl_seat                *wl_seat;
	struct zwlr_layer_shell_v1    *layer_shell;

	/* per-output bar instances */
	GList                         *outputs;  /* GList of GowlbarOutput* */

	/* GLib main loop integration */
	GMainLoop                     *main_loop;
	GIOChannel                    *wl_channel;
	guint                          wl_source_id;

	/* stdin reading for status text */
	GIOChannel                    *stdin_channel;
	guint                          stdin_source_id;

	/* bar configuration */
	GowlbarConfig                 *config;   /* borrowed ref (may be NULL) */
	gint                           bar_height;
	GowlbarPosition                position;

	/* widgets (shared across all outputs) */
	GList                         *widgets;  /* owned GList of GowlbarWidget* */
	GowlbarTagWidget              *tag_widget;
	GowlbarLayoutWidget           *layout_widget;
	GowlbarTitleWidget            *title_widget;
	GowlbarStatusWidget           *status_widget;

	/* IPC client for compositor communication */
	GowlbarIpc                    *ipc;

	/* state */
	gboolean                       running;
};

G_DEFINE_FINAL_TYPE(GowlbarApp, gowlbar_app, G_TYPE_OBJECT)

/* --- Forward declarations --- */

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version);
static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name);

static const struct wl_registry_listener registry_listener = {
	.global        = registry_global,
	.global_remove = registry_global_remove,
};

/* --- GLib main loop ↔ wl_display integration --- */

/**
 * on_wayland_readable:
 *
 * GIOFunc callback.  Called when the Wayland fd has data to read.
 * Dispatches Wayland events and flushes pending requests.
 */
static gboolean
on_wayland_readable(
	GIOChannel   *source,
	GIOCondition  condition,
	gpointer      user_data
){
	GowlbarApp *self;

	self = (GowlbarApp *)user_data;
	(void)source;

	if (condition & (G_IO_ERR | G_IO_HUP)) {
		g_warning("gowlbar: Wayland connection lost");
		g_main_loop_quit(self->main_loop);
		return FALSE;
	}

	/* Read and dispatch events from the compositor */
	if (wl_display_dispatch(self->wl_display) < 0) {
		g_warning("gowlbar: wl_display_dispatch failed");
		g_main_loop_quit(self->main_loop);
		return FALSE;
	}

	return TRUE;
}

/* --- stdin reading for status text --- */

/**
 * render_all_outputs:
 *
 * Triggers a re-render on every output.
 */
static void
render_all_outputs(GowlbarApp *self)
{
	GList *l;

	for (l = self->outputs; l != NULL; l = l->next) {
		GowlbarOutput *output;

		output = (GowlbarOutput *)l->data;
		gowlbar_output_render(output);
	}

	/* Flush after rendering all outputs */
	if (self->wl_display != NULL)
		wl_display_flush(self->wl_display);
}

/**
 * on_stdin_readable:
 *
 * GIOFunc callback.  Reads a line from stdin and updates the
 * status widget text, then triggers a redraw on all outputs.
 */
static gboolean
on_stdin_readable(
	GIOChannel   *source,
	GIOCondition  condition,
	gpointer      user_data
){
	GowlbarApp *self;
	g_autofree gchar *line = NULL;
	gsize len;
	GIOStatus status;

	self = (GowlbarApp *)user_data;

	if (condition & (G_IO_ERR | G_IO_HUP)) {
		g_debug("gowlbar: stdin closed");
		self->stdin_source_id = 0;
		return FALSE;
	}

	status = g_io_channel_read_line(source, &line, &len, NULL, NULL);
	if (status == G_IO_STATUS_NORMAL && line != NULL) {
		/* Strip trailing newline */
		g_strstrip(line);

		if (self->status_widget != NULL) {
			gowlbar_status_widget_set_text(self->status_widget,
			                                line);
			render_all_outputs(self);
		}
	} else if (status == G_IO_STATUS_EOF) {
		g_debug("gowlbar: stdin EOF");
		self->stdin_source_id = 0;
		return FALSE;
	}

	return TRUE;
}

/* --- Registry callbacks --- */

/**
 * registry_global:
 *
 * Called for each global advertised by the compositor.
 * Binds wl_compositor, wl_shm, wl_output, wl_seat, and
 * zwlr_layer_shell_v1.
 */
static void
registry_global(
	void               *data,
	struct wl_registry *registry,
	uint32_t            name,
	const char         *interface,
	uint32_t            version
){
	GowlbarApp *self;

	self = (GowlbarApp *)data;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		self->wl_compositor = (struct wl_compositor *)wl_registry_bind(
			registry, name, &wl_compositor_interface, 4);

	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		self->wl_shm = (struct wl_shm *)wl_registry_bind(
			registry, name, &wl_shm_interface, 1);

	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		self->wl_seat = (struct wl_seat *)wl_registry_bind(
			registry, name, &wl_seat_interface, 7);

	} else if (strcmp(interface,
	                  zwlr_layer_shell_v1_interface.name) == 0) {
		self->layer_shell =
			(struct zwlr_layer_shell_v1 *)wl_registry_bind(
				registry, name,
				&zwlr_layer_shell_v1_interface, 1);

	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct wl_output *wl_output;
		GowlbarOutput *output;

		wl_output = (struct wl_output *)wl_registry_bind(
			registry, name, &wl_output_interface, 4);

		output = gowlbar_output_new(wl_output, NULL, name);

		/* Apply config and widgets to the output */
		gowlbar_output_set_config(output, self->config);
		gowlbar_output_set_widgets(output, self->widgets);

		self->outputs = g_list_append(self->outputs, output);

		/* If we already have the layer shell, set up the surface */
		if (self->layer_shell != NULL && self->wl_compositor != NULL
		    && self->wl_shm != NULL) {
			gowlbar_output_setup_surface(
				output, self->wl_compositor,
				self->layer_shell, self->wl_shm,
				self->bar_height);
		}

		g_debug("gowlbar: output added (global %u)", name);
	}
}

/**
 * registry_global_remove:
 *
 * Called when a global is removed (e.g. output unplugged).
 * Removes the corresponding GowlbarOutput.
 */
static void
registry_global_remove(
	void               *data,
	struct wl_registry *registry,
	uint32_t            name
){
	GowlbarApp *self;
	GList *l;

	self = (GowlbarApp *)data;
	(void)registry;

	for (l = self->outputs; l != NULL; l = l->next) {
		GowlbarOutput *output;

		output = (GowlbarOutput *)l->data;
		if (gowlbar_output_get_global_name(output) == name) {
			self->outputs = g_list_delete_link(self->outputs, l);
			g_object_unref(output);
			g_debug("gowlbar: output removed (global %u)", name);
			return;
		}
	}
}

/* --- IPC signal handlers --- */

/**
 * on_ipc_connected:
 *
 * Called when the IPC client connects to the compositor.
 */
static void
on_ipc_connected(GowlbarIpc *ipc, gpointer user_data)
{
	(void)ipc;
	(void)user_data;

	g_warning("gowlbar: IPC connected to compositor");
}

/**
 * on_ipc_disconnected:
 *
 * Called when the IPC connection to the compositor is lost.
 */
static void
on_ipc_disconnected(GowlbarIpc *ipc, gpointer user_data)
{
	(void)ipc;
	(void)user_data;

	g_warning("gowlbar: IPC disconnected from compositor");
}

/**
 * on_ipc_tags_changed:
 *
 * Called when tag state changes.  Updates the tag widget and
 * triggers a redraw on all outputs.
 */
static void
on_ipc_tags_changed(
	GowlbarIpc *ipc,
	const gchar *output,
	guint active_mask,
	guint occupied_mask,
	guint urgent_mask,
	guint sel_tags,
	gpointer user_data
){
	GowlbarApp *self;

	(void)ipc;
	(void)output;

	self = (GowlbarApp *)user_data;

	g_warning("gowlbar: on_ipc_tags_changed: output=%s active=%u occupied=%u urgent=%u sel=%u",
		output, active_mask, occupied_mask, urgent_mask, sel_tags);

	gowlbar_tag_widget_set_state(
		self->tag_widget,
		(guint32)active_mask,
		(guint32)occupied_mask,
		(guint32)urgent_mask,
		(guint32)sel_tags);

	render_all_outputs(self);
}

/**
 * on_ipc_layout_changed:
 *
 * Called when the layout changes.  Updates the layout widget and
 * triggers a redraw on all outputs.
 */
static void
on_ipc_layout_changed(
	GowlbarIpc  *ipc,
	const gchar *output,
	const gchar *layout_name,
	gpointer     user_data
){
	GowlbarApp *self;

	(void)ipc;
	(void)output;

	self = (GowlbarApp *)user_data;

	gowlbar_layout_widget_set_layout(self->layout_widget, layout_name);
	render_all_outputs(self);
}

/**
 * on_ipc_title_changed:
 *
 * Called when the focused window title changes.  Updates the title
 * widget and triggers a redraw on all outputs.
 */
static void
on_ipc_title_changed(
	GowlbarIpc  *ipc,
	const gchar *title,
	gpointer     user_data
){
	GowlbarApp *self;

	(void)ipc;

	self = (GowlbarApp *)user_data;

	gowlbar_title_widget_set_title(self->title_widget, title);
	render_all_outputs(self);
}

/* --- Widget creation --- */

/**
 * create_widgets:
 *
 * Creates the built-in widget set: tags, layout, title, status.
 * Widgets are stored in order for the render pipeline:
 * [tags] [layout] [title (expands)] [status]
 */
static void
create_widgets(GowlbarApp *self)
{
	self->tag_widget = gowlbar_tag_widget_new(self->config);
	self->layout_widget = gowlbar_layout_widget_new(self->config);
	self->title_widget = gowlbar_title_widget_new(self->config);
	self->status_widget = gowlbar_status_widget_new(self->config);

	/* Build the widget list in render order */
	self->widgets = NULL;
	self->widgets = g_list_append(self->widgets, self->tag_widget);
	self->widgets = g_list_append(self->widgets, self->layout_widget);
	self->widgets = g_list_append(self->widgets, self->title_widget);
	self->widgets = g_list_append(self->widgets, self->status_widget);
}

/**
 * destroy_widgets:
 *
 * Destroys all widgets and frees the widget list.
 */
static void
destroy_widgets(GowlbarApp *self)
{
	g_list_free(self->widgets);
	self->widgets = NULL;

	g_clear_object(&self->tag_widget);
	g_clear_object(&self->layout_widget);
	g_clear_object(&self->title_widget);
	g_clear_object(&self->status_widget);
}

/* --- GObject lifecycle --- */

static void
gowlbar_app_dispose(GObject *object)
{
	GowlbarApp *self;

	self = GOWLBAR_APP(object);

	if (self->running)
		gowlbar_app_quit(self);

	G_OBJECT_CLASS(gowlbar_app_parent_class)->dispose(object);
}

static void
gowlbar_app_finalize(GObject *object)
{
	GowlbarApp *self;

	self = GOWLBAR_APP(object);

	/* Destroy widgets */
	destroy_widgets(self);

	/* Clean up IPC client */
	g_clear_object(&self->ipc);

	/* Free outputs */
	g_list_free_full(self->outputs, g_object_unref);

	/* Clean up stdin channel */
	if (self->stdin_source_id > 0)
		g_source_remove(self->stdin_source_id);
	if (self->stdin_channel != NULL)
		g_io_channel_unref(self->stdin_channel);

	/* Clean up Wayland */
	if (self->layer_shell != NULL)
		zwlr_layer_shell_v1_destroy(self->layer_shell);
	if (self->wl_seat != NULL)
		wl_seat_destroy(self->wl_seat);
	if (self->wl_shm != NULL)
		wl_shm_destroy(self->wl_shm);
	if (self->wl_compositor != NULL)
		wl_compositor_destroy(self->wl_compositor);
	if (self->wl_registry != NULL)
		wl_registry_destroy(self->wl_registry);
	if (self->wl_display != NULL)
		wl_display_disconnect(self->wl_display);

	if (self->main_loop != NULL)
		g_main_loop_unref(self->main_loop);

	G_OBJECT_CLASS(gowlbar_app_parent_class)->finalize(object);
}

static void
gowlbar_app_class_init(GowlbarAppClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->dispose  = gowlbar_app_dispose;
	object_class->finalize = gowlbar_app_finalize;
}

static void
gowlbar_app_init(GowlbarApp *self)
{
	self->wl_display      = NULL;
	self->wl_registry     = NULL;
	self->wl_compositor   = NULL;
	self->wl_shm          = NULL;
	self->wl_seat         = NULL;
	self->layer_shell     = NULL;
	self->outputs         = NULL;
	self->config          = NULL;
	self->main_loop       = NULL;
	self->wl_channel      = NULL;
	self->wl_source_id    = 0;
	self->stdin_channel   = NULL;
	self->stdin_source_id = 0;
	self->bar_height      = 24;
	self->position        = GOWLBAR_POSITION_TOP;
	self->widgets         = NULL;
	self->tag_widget      = NULL;
	self->layout_widget   = NULL;
	self->title_widget    = NULL;
	self->status_widget   = NULL;
	self->ipc             = NULL;
	self->running         = FALSE;
}

/* --- Public API --- */

/**
 * gowlbar_app_new:
 *
 * Creates a new bar application instance with default settings.
 *
 * Returns: (transfer full): a newly allocated #GowlbarApp
 */
GowlbarApp *
gowlbar_app_new(void)
{
	return (GowlbarApp *)g_object_new(GOWLBAR_TYPE_APP, NULL);
}

/**
 * gowlbar_app_set_config:
 * @self: the bar application
 * @config: (transfer none): the bar configuration to apply
 *
 * Sets the configuration object used by the bar application.
 * Reads bar height and position from the config.  Must be called
 * before gowlbar_app_run().
 */
void
gowlbar_app_set_config(GowlbarApp *self, GowlbarConfig *config)
{
	const gchar *pos;

	g_return_if_fail(GOWLBAR_IS_APP(self));

	self->config = config;

	if (config != NULL) {
		self->bar_height = gowlbar_config_get_height(config);
		pos = gowlbar_config_get_position(config);
		if (pos != NULL && g_strcmp0(pos, "bottom") == 0)
			self->position = GOWLBAR_POSITION_BOTTOM;
		else
			self->position = GOWLBAR_POSITION_TOP;
	}
}

/**
 * gowlbar_app_get_config:
 * @self: the bar application
 *
 * Returns: (transfer none) (nullable): the current bar configuration
 */
GowlbarConfig *
gowlbar_app_get_config(GowlbarApp *self)
{
	g_return_val_if_fail(GOWLBAR_IS_APP(self), NULL);

	return self->config;
}

/**
 * gowlbar_app_run:
 * @self: the bar application
 * @error: (nullable): return location for a #GError
 *
 * Connects to the Wayland display, performs a registry roundtrip
 * to discover globals, creates widgets, sets up per-output bar
 * surfaces, starts stdin monitoring, and enters the GLib main loop.
 * Blocks until gowlbar_app_quit() is called or the Wayland
 * connection is lost.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
gowlbar_app_run(GowlbarApp *self, GError **error)
{
	gint fd;
	GList *l;

	g_return_val_if_fail(GOWLBAR_IS_APP(self), FALSE);

	/* Create widgets before connecting (they need config only) */
	create_widgets(self);

	/* Connect to Wayland display */
	self->wl_display = wl_display_connect(NULL);
	if (self->wl_display == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                    "Failed to connect to Wayland display");
		return FALSE;
	}

	/* Get the registry and perform initial roundtrip */
	self->wl_registry = wl_display_get_registry(self->wl_display);
	wl_registry_add_listener(self->wl_registry,
	                         &registry_listener, self);

	/* Two roundtrips: first discovers globals, second gets output info */
	wl_display_roundtrip(self->wl_display);
	wl_display_roundtrip(self->wl_display);

	/* Verify we got the required globals */
	if (self->wl_compositor == NULL || self->wl_shm == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                    "Missing required Wayland globals "
		                    "(wl_compositor, wl_shm)");
		return FALSE;
	}

	if (self->layer_shell == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                    "Compositor does not support "
		                    "wlr-layer-shell protocol");
		return FALSE;
	}

	/* Set up surfaces for any outputs that were added during roundtrip
	 * but before layer_shell was bound */
	for (l = self->outputs; l != NULL; l = l->next) {
		GowlbarOutput *output;

		output = (GowlbarOutput *)l->data;
		gowlbar_output_setup_surface(
			output, self->wl_compositor,
			self->layer_shell, self->wl_shm,
			self->bar_height);
	}

	/* Commit all surfaces */
	wl_display_flush(self->wl_display);

	/* Integrate wl_display fd into GLib main loop */
	fd = wl_display_get_fd(self->wl_display);
	self->wl_channel = g_io_channel_unix_new(fd);
	self->wl_source_id = g_io_add_watch(
		self->wl_channel,
		G_IO_IN | G_IO_ERR | G_IO_HUP,
		on_wayland_readable,
		self);

	/*
	 * Set up stdin reading for status text.
	 * Only monitor stdin if it is not a terminal (i.e. piped input).
	 */
	if (!isatty(STDIN_FILENO)) {
		self->stdin_channel = g_io_channel_unix_new(STDIN_FILENO);
		g_io_channel_set_encoding(self->stdin_channel, NULL, NULL);
		self->stdin_source_id = g_io_add_watch(
			self->stdin_channel,
			G_IO_IN | G_IO_ERR | G_IO_HUP,
			on_stdin_readable,
			self);
		g_debug("gowlbar: monitoring stdin for status text");
	}

	/* Set up IPC connection to compositor */
	self->ipc = gowlbar_ipc_new(NULL);

	g_signal_connect(self->ipc, "connected",
	                 G_CALLBACK(on_ipc_connected), self);
	g_signal_connect(self->ipc, "disconnected",
	                 G_CALLBACK(on_ipc_disconnected), self);
	g_signal_connect(self->ipc, "tags-changed",
	                 G_CALLBACK(on_ipc_tags_changed), self);
	g_signal_connect(self->ipc, "layout-changed",
	                 G_CALLBACK(on_ipc_layout_changed), self);
	g_signal_connect(self->ipc, "title-changed",
	                 G_CALLBACK(on_ipc_title_changed), self);

	gowlbar_ipc_connect(self->ipc);

	/* Create and run the main loop */
	self->main_loop = g_main_loop_new(NULL, FALSE);
	self->running = TRUE;

	g_debug("gowlbar: entering main loop (%u outputs, %u widgets)",
	        g_list_length(self->outputs),
	        g_list_length(self->widgets));

	g_main_loop_run(self->main_loop);

	/* Cleanup after loop exits */
	self->running = FALSE;

	/* Disconnect IPC */
	if (self->ipc != NULL)
		gowlbar_ipc_disconnect(self->ipc);

	if (self->stdin_source_id > 0) {
		g_source_remove(self->stdin_source_id);
		self->stdin_source_id = 0;
	}
	if (self->stdin_channel != NULL) {
		g_io_channel_unref(self->stdin_channel);
		self->stdin_channel = NULL;
	}
	if (self->wl_source_id > 0) {
		g_source_remove(self->wl_source_id);
		self->wl_source_id = 0;
	}
	if (self->wl_channel != NULL) {
		g_io_channel_unref(self->wl_channel);
		self->wl_channel = NULL;
	}

	return TRUE;
}

/**
 * gowlbar_app_quit:
 * @self: the bar application
 *
 * Requests the bar application to exit its main loop.
 */
void
gowlbar_app_quit(GowlbarApp *self)
{
	g_return_if_fail(GOWLBAR_IS_APP(self));

	if (self->main_loop != NULL && g_main_loop_is_running(self->main_loop))
		g_main_loop_quit(self->main_loop);

	self->running = FALSE;
}
