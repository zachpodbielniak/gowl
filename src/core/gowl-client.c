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

#include <string.h>
#include <unistd.h>

/* monotonically increasing client ID counter */
static guint next_client_id = 1;

/**
 * GowlClient:
 *
 * Represents a toplevel client surface (XDG or XWayland).  Holds
 * geometry, tag assignment, display state, and wlroots surface objects.
 * The struct definition lives in gowl-core-private.h.
 */

G_DEFINE_FINAL_TYPE(GowlClient, gowl_client, G_TYPE_OBJECT)

/* Signal identifiers */
enum {
	SIGNAL_MAP,
	SIGNAL_UNMAP,
	SIGNAL_SET_TITLE,
	SIGNAL_SET_APP_ID,
	SIGNAL_REQUEST_FULLSCREEN,
	SIGNAL_DESTROY,
	SIGNAL_TAGS_CHANGED,
	SIGNAL_STATE_CHANGED,
	SIGNAL_MIRROR_ADDED,
	SIGNAL_MIRROR_REMOVED,
	N_SIGNALS
};

static guint client_signals[N_SIGNALS] = { 0, };

/* --- GObject lifecycle --- */

/* Forward declaration from gowl-mirror.c. */
extern void _gowl_mirror_detach_from_client(GowlMirror *self);

static void
gowl_client_dispose(GObject *object)
{
	GowlClient *self;
	GList      *l;

	self = GOWL_CLIENT(object);

	/* Tear down mirrors BEFORE the underlying surface is freed so
	 * each mirror can drop its commit listener while the surface
	 * pointer is still valid.  After detach the mirrors are dead
	 * handles and we drop our strong refs. */
	for (l = self->mirrors; l != NULL; l = l->next) {
		GowlMirror *m = GOWL_MIRROR(l->data);
		if (m != NULL)
			_gowl_mirror_detach_from_client(m);
	}
	g_list_free_full(self->mirrors, g_object_unref);
	self->mirrors = NULL;

	self->mon = NULL;
	self->compositor = NULL;

	G_OBJECT_CLASS(gowl_client_parent_class)->dispose(object);
}

static void
gowl_client_finalize(GObject *object)
{
	GowlClient *self;

	self = GOWL_CLIENT(object);

	g_free(self->title);
	g_free(self->app_id);

	G_OBJECT_CLASS(gowl_client_parent_class)->finalize(object);
}

/* --- class / instance init --- */

static void
gowl_client_class_init(GowlClientClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->dispose  = gowl_client_dispose;
	object_class->finalize = gowl_client_finalize;

	/**
	 * GowlClient::map:
	 * @client: the #GowlClient that emitted the signal
	 *
	 * Emitted when the client surface is mapped (becomes visible).
	 */
	client_signals[SIGNAL_MAP] =
		g_signal_new("map",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlClient::unmap:
	 * @client: the #GowlClient that emitted the signal
	 *
	 * Emitted when the client surface is unmapped (hidden).
	 */
	client_signals[SIGNAL_UNMAP] =
		g_signal_new("unmap",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlClient::set-title:
	 * @client: the #GowlClient that emitted the signal
	 * @title: the new title string
	 *
	 * Emitted when the client title changes.
	 */
	client_signals[SIGNAL_SET_TITLE] =
		g_signal_new("set-title",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             1,
		             G_TYPE_STRING);

	/**
	 * GowlClient::set-app-id:
	 * @client: the #GowlClient that emitted the signal
	 * @app_id: the new app_id string
	 *
	 * Emitted when the client app_id changes.
	 */
	client_signals[SIGNAL_SET_APP_ID] =
		g_signal_new("set-app-id",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             1,
		             G_TYPE_STRING);

	/**
	 * GowlClient::request-fullscreen:
	 * @client: the #GowlClient that emitted the signal
	 *
	 * Emitted when the client requests fullscreen mode.
	 */
	client_signals[SIGNAL_REQUEST_FULLSCREEN] =
		g_signal_new("request-fullscreen",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlClient::destroy:
	 * @client: the #GowlClient that emitted the signal
	 *
	 * Emitted when the client is being destroyed.
	 */
	client_signals[SIGNAL_DESTROY] =
		g_signal_new("destroy",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlClient::tags-changed:
	 * @client: the #GowlClient that emitted the signal
	 * @old_tags: the previous tag bitmask
	 * @new_tags: the new tag bitmask
	 *
	 * Emitted when the client tag assignment changes.
	 */
	client_signals[SIGNAL_TAGS_CHANGED] =
		g_signal_new("tags-changed",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             2,
		             G_TYPE_UINT,
		             G_TYPE_UINT);

	/**
	 * GowlClient::state-changed:
	 * @client: the #GowlClient that emitted the signal
	 *
	 * Emitted when the client display state (floating, fullscreen,
	 * urgent) changes.
	 */
	client_signals[SIGNAL_STATE_CHANGED] =
		g_signal_new("state-changed",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlClient::mirror-added:
	 * @client: the #GowlClient
	 * @mirror: the newly-added #GowlMirror
	 *
	 * Emitted when #gowl_client_add_mirror creates a new mirror
	 * scene node.  Listeners can record the mirror for
	 * reconciliation with Emacs window state.
	 */
	client_signals[SIGNAL_MIRROR_ADDED] =
		g_signal_new("mirror-added",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             1,
		             GOWL_TYPE_MIRROR);

	/**
	 * GowlClient::mirror-removed:
	 * @client: the #GowlClient
	 * @mirror: the mirror about to be dropped
	 *
	 * Emitted just before the client's last reference on @mirror
	 * is released by #gowl_client_remove_mirror.  Listeners that
	 * index mirrors by id must drop their bookkeeping here.
	 */
	client_signals[SIGNAL_MIRROR_REMOVED] =
		g_signal_new("mirror-removed",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             1,
		             GOWL_TYPE_MIRROR);
}

static void
gowl_client_init(GowlClient *self)
{
	self->id            = next_client_id++;
	self->xdg_toplevel  = NULL;
	self->scene         = NULL;
	self->scene_surface = NULL;
	memset(self->border, 0, sizeof(self->border));
	memset(&self->geom, 0, sizeof(self->geom));
	memset(&self->prev, 0, sizeof(self->prev));
	self->tags          = 1;
	self->bw            = 1;
	self->alpha         = 1.0f;
	self->isfloating    = FALSE;
	self->isurgent      = FALSE;
	self->isfullscreen  = FALSE;
	self->resize        = 0;
	self->title         = NULL;
	self->app_id        = NULL;
	self->mon           = NULL;
	self->compositor    = NULL;
	self->pending_rule_tags     = 0;
	self->pending_rule_monitor  = -1;
	self->pending_rule_geom_set = FALSE;
	self->mirrors               = NULL;
	self->next_mirror_id        = 1;
}

/* --- Public API --- */

/**
 * gowl_client_new:
 *
 * Creates a new #GowlClient with default state.
 *
 * Returns: (transfer full): a newly allocated #GowlClient
 */
GowlClient *
gowl_client_new(void)
{
	return (GowlClient *)g_object_new(GOWL_TYPE_CLIENT, NULL);
}

/**
 * gowl_client_get_tags:
 * @self: a #GowlClient
 *
 * Returns the tag bitmask for this client.
 *
 * Returns: the tag bitmask
 */
guint32
gowl_client_get_tags(GowlClient *self)
{
	g_return_val_if_fail(GOWL_IS_CLIENT(self), 0);

	return self->tags;
}

/**
 * gowl_client_set_tags:
 * @self: a #GowlClient
 * @tags: the new tag bitmask
 *
 * Sets the tag bitmask and emits "tags-changed" if it differs.
 */
void
gowl_client_set_tags(
	GowlClient *self,
	guint32     tags
){
	guint32 old_tags;

	g_return_if_fail(GOWL_IS_CLIENT(self));

	old_tags = self->tags;
	self->tags = tags;

	if (old_tags != tags)
		g_signal_emit(self, client_signals[SIGNAL_TAGS_CHANGED], 0,
		              old_tags, tags);
}

/**
 * gowl_client_get_floating:
 * @self: a #GowlClient
 *
 * Returns whether the client is floating.
 *
 * Returns: %TRUE if floating
 */
gboolean
gowl_client_get_floating(GowlClient *self)
{
	g_return_val_if_fail(GOWL_IS_CLIENT(self), FALSE);

	return self->isfloating;
}

/**
 * gowl_client_set_floating:
 * @self: a #GowlClient
 * @floating: %TRUE to float the client
 *
 * Sets the floating state and emits "state-changed".
 */
void
gowl_client_set_floating(
	GowlClient *self,
	gboolean    floating
){
	g_return_if_fail(GOWL_IS_CLIENT(self));

	if (self->isfloating != floating) {
		self->isfloating = floating;
		g_signal_emit(self, client_signals[SIGNAL_STATE_CHANGED], 0);
	}
}

/**
 * gowl_client_get_fullscreen:
 * @self: a #GowlClient
 *
 * Returns whether the client is in fullscreen mode.
 *
 * Returns: %TRUE if fullscreen
 */
gboolean
gowl_client_get_fullscreen(GowlClient *self)
{
	g_return_val_if_fail(GOWL_IS_CLIENT(self), FALSE);

	return self->isfullscreen;
}

/**
 * gowl_client_set_fullscreen:
 * @self: a #GowlClient
 * @fullscreen: %TRUE to enter fullscreen
 *
 * Sets the fullscreen state.  When entering fullscreen the current
 * geometry is saved; the actual resize will be handled by the
 * compositor.  Emits "state-changed".
 */
void
gowl_client_set_fullscreen(
	GowlClient *self,
	gboolean    fullscreen
){
	g_return_if_fail(GOWL_IS_CLIENT(self));

	if (self->isfullscreen != fullscreen) {
		/* save geometry before going fullscreen */
		if (fullscreen && !self->isfullscreen)
			self->prev = self->geom;

		self->isfullscreen = fullscreen;
		g_signal_emit(self, client_signals[SIGNAL_STATE_CHANGED], 0);
	}
}

/**
 * gowl_client_get_urgent:
 * @self: a #GowlClient
 *
 * Returns whether the client has the urgent hint set.
 *
 * Returns: %TRUE if urgent
 */
gboolean
gowl_client_get_urgent(GowlClient *self)
{
	g_return_val_if_fail(GOWL_IS_CLIENT(self), FALSE);

	return self->isurgent;
}

/**
 * gowl_client_set_urgent:
 * @self: a #GowlClient
 * @urgent: %TRUE to mark urgent
 *
 * Sets the urgent flag and emits "state-changed".
 */
void
gowl_client_set_urgent(
	GowlClient *self,
	gboolean    urgent
){
	g_return_if_fail(GOWL_IS_CLIENT(self));

	if (self->isurgent != urgent) {
		self->isurgent = urgent;
		g_signal_emit(self, client_signals[SIGNAL_STATE_CHANGED], 0);
	}
}

/**
 * gowl_client_get_title:
 * @self: a #GowlClient
 *
 * Returns the client window title.
 *
 * Returns: (transfer none) (nullable): the title string
 */
const gchar *
gowl_client_get_title(GowlClient *self)
{
	g_return_val_if_fail(GOWL_IS_CLIENT(self), NULL);

	return self->title;
}

/**
 * gowl_client_set_title:
 * @self: a #GowlClient
 * @title: (nullable): the new title
 *
 * Sets the client title and emits "set-title".
 */
void
gowl_client_set_title(
	GowlClient  *self,
	const gchar *title
){
	g_return_if_fail(GOWL_IS_CLIENT(self));

	g_free(self->title);
	self->title = g_strdup(title);

	g_signal_emit(self, client_signals[SIGNAL_SET_TITLE], 0, self->title);
}

/**
 * gowl_client_get_app_id:
 * @self: a #GowlClient
 *
 * Returns the client application identifier.
 *
 * Returns: (transfer none) (nullable): the app_id string
 */
const gchar *
gowl_client_get_app_id(GowlClient *self)
{
	g_return_val_if_fail(GOWL_IS_CLIENT(self), NULL);

	return self->app_id;
}

/**
 * gowl_client_set_app_id:
 * @self: a #GowlClient
 * @app_id: (nullable): the new app_id
 *
 * Sets the client app_id and emits "set-app-id".
 */
void
gowl_client_set_app_id(
	GowlClient  *self,
	const gchar *app_id
){
	g_return_if_fail(GOWL_IS_CLIENT(self));

	g_free(self->app_id);
	self->app_id = g_strdup(app_id);

	g_signal_emit(self, client_signals[SIGNAL_SET_APP_ID], 0, self->app_id);
}

/**
 * gowl_client_get_geometry:
 * @self: a #GowlClient
 * @x: (out) (nullable): return location for x
 * @y: (out) (nullable): return location for y
 * @width: (out) (nullable): return location for width
 * @height: (out) (nullable): return location for height
 *
 * Retrieves the current geometry of the client.
 */
void
gowl_client_get_geometry(
	GowlClient *self,
	gint       *x,
	gint       *y,
	gint       *width,
	gint       *height
){
	g_return_if_fail(GOWL_IS_CLIENT(self));

	if (x != NULL)
		*x = self->geom.x;
	if (y != NULL)
		*y = self->geom.y;
	if (width != NULL)
		*width = self->geom.width;
	if (height != NULL)
		*height = self->geom.height;
}

/**
 * gowl_client_set_geometry:
 * @self: a #GowlClient
 * @x: horizontal position
 * @y: vertical position
 * @width: width in pixels
 * @height: height in pixels
 *
 * Sets the geometry of the client.
 */
void
gowl_client_set_geometry(
	GowlClient *self,
	gint        x,
	gint        y,
	gint        width,
	gint        height
){
	g_return_if_fail(GOWL_IS_CLIENT(self));

	self->geom.x      = x;
	self->geom.y      = y;
	self->geom.width   = width;
	self->geom.height  = height;
}

/**
 * gowl_client_get_monitor:
 * @self: a #GowlClient
 *
 * Returns the monitor this client is assigned to.
 *
 * Returns: (transfer none) (nullable): a #GowlMonitor pointer, or %NULL
 */
gpointer
gowl_client_get_monitor(GowlClient *self)
{
	g_return_val_if_fail(GOWL_IS_CLIENT(self), NULL);

	return (gpointer)self->mon;
}

/**
 * gowl_client_set_monitor:
 * @self: a #GowlClient
 * @monitor: (nullable): the #GowlMonitor to assign, or %NULL
 *
 * Assigns the client to a monitor.
 */
void
gowl_client_set_monitor(
	GowlClient *self,
	gpointer    monitor
){
	g_return_if_fail(GOWL_IS_CLIENT(self));

	self->mon = (GowlMonitor *)monitor;
}

/**
 * gowl_client_get_id:
 * @self: a #GowlClient
 *
 * Returns a unique identifier for this client.  The ID is assigned
 * at creation time from a monotonically increasing counter and is
 * never reused within the compositor session.
 *
 * Returns: the unique client ID
 */
guint
gowl_client_get_id(GowlClient *self)
{
	g_return_val_if_fail(GOWL_IS_CLIENT(self), 0);

	return self->id;
}

/**
 * gowl_client_close:
 * @self: a #GowlClient
 *
 * Sends a close request to the client's XDG toplevel surface.
 * The client may choose to ignore the request (e.g. to prompt
 * the user about unsaved changes).
 */
void
gowl_client_close(GowlClient *self)
{
	g_return_if_fail(GOWL_IS_CLIENT(self));

	if (self->xdg_toplevel != NULL)
		wlr_xdg_toplevel_send_close(self->xdg_toplevel);
}

/**
 * gowl_client_get_pid:
 * @self: a #GowlClient
 *
 * Returns the PID of the process that owns this client's Wayland
 * connection.  Uses wl_client_get_credentials() on the underlying
 * Wayland client resource.
 *
 * Returns: the process ID, or (pid_t)-1 if unavailable
 */
pid_t
gowl_client_get_pid(GowlClient *self)
{
	struct wl_client *wl_client;
	pid_t pid;

	g_return_val_if_fail(GOWL_IS_CLIENT(self), (pid_t)-1);

	if (self->xdg_toplevel == NULL ||
	    self->xdg_toplevel->base == NULL ||
	    self->xdg_toplevel->base->resource == NULL)
		return (pid_t)-1;

	wl_client = wl_resource_get_client(self->xdg_toplevel->base->resource);
	if (wl_client == NULL)
		return (pid_t)-1;

	wl_client_get_credentials(wl_client, &pid, NULL, NULL);
	return pid;
}

/**
 * gowl_client_get_process_info:
 * @self: a #GowlClient
 *
 * Returns process information for the owning process by reading
 * /proc.  Returns %NULL if the PID is unavailable.
 *
 * Returns: (transfer full) (nullable): a #GowlProcessInfo
 */
GowlProcessInfo *
gowl_client_get_process_info(GowlClient *self)
{
	pid_t pid;

	g_return_val_if_fail(GOWL_IS_CLIENT(self), NULL);

	pid = gowl_client_get_pid(self);
	if (pid == (pid_t)-1)
		return NULL;

	return gowl_process_info_new(pid);
}

/**
 * gowl_client_get_wlr_surface:
 * @self: a #GowlClient
 *
 * Returns the underlying wlr_surface for this client.  The surface
 * is owned by wlroots and must not be freed by the caller.
 *
 * Returns: (transfer none) (nullable): the wlr_surface, or %NULL
 */
struct wlr_surface *
gowl_client_get_wlr_surface(GowlClient *self)
{
	g_return_val_if_fail(GOWL_IS_CLIENT(self), NULL);

	if (self->xdg_toplevel == NULL ||
	    self->xdg_toplevel->base == NULL)
		return NULL;

	return self->xdg_toplevel->base->surface;
}

struct wlr_scene_tree *
gowl_client_get_scene(GowlClient *self)
{
	g_return_val_if_fail(GOWL_IS_CLIENT(self), NULL);
	return self->scene;
}

/**
 * gowl_client_get_border_width:
 * @self: a #GowlClient
 *
 * Returns the border width in pixels.
 *
 * Returns: the border width
 */
guint
gowl_client_get_border_width(GowlClient *self)
{
	g_return_val_if_fail(GOWL_IS_CLIENT(self), 0);

	return self->bw;
}

/**
 * gowl_client_set_border_width:
 * @self: a #GowlClient
 * @width: new border width in pixels
 *
 * Sets the border width and updates the border scene rects.
 */
void
gowl_client_set_border_width(
	GowlClient *self,
	guint       width
){
	g_return_if_fail(GOWL_IS_CLIENT(self));

	self->bw = width;

	/* Update scene rect sizes to match new border width. */
	if (self->border[0] != NULL) {
		gint w = self->geom.width;
		gint h = self->geom.height;

		/* top */
		wlr_scene_rect_set_size(self->border[0], w, width);
		/* bottom */
		wlr_scene_rect_set_size(self->border[1], w, width);
		wlr_scene_node_set_position(&self->border[1]->node,
		                            0, h - width);
		/* left */
		wlr_scene_rect_set_size(self->border[2], width,
		                        h - 2 * width);
		wlr_scene_node_set_position(&self->border[2]->node,
		                            0, width);
		/* right */
		wlr_scene_rect_set_size(self->border[3], width,
		                        h - 2 * width);
		wlr_scene_node_set_position(&self->border[3]->node,
		                            w - width, width);
	}
}

/**
 * gowl_client_set_visible:
 * @self: a #GowlClient
 * @visible: %TRUE to show, %FALSE to hide
 *
 * Shows or hides the client's scene node without destroying it.
 */
void
gowl_client_set_visible(
	GowlClient *self,
	gboolean    visible
){
	g_return_if_fail(GOWL_IS_CLIENT(self));

	if (self->scene != NULL)
		wlr_scene_node_set_enabled(&self->scene->node, visible);
}

/**
 * gowl_client_get_embedded:
 */
gboolean
gowl_client_get_embedded(GowlClient *self)
{
	g_return_val_if_fail(GOWL_IS_CLIENT(self), FALSE);
	return self->isembedded;
}

/**
 * gowl_client_set_embedded:
 */
void
gowl_client_set_embedded(
	GowlClient *self,
	gboolean    embedded
){
	g_return_if_fail(GOWL_IS_CLIENT(self));
	self->isembedded = embedded;
}

/* --- Alpha / opacity --- */

static void
set_opacity_iter(struct wlr_scene_buffer *buffer,
                 int sx, int sy, void *data)
{
	gfloat *alpha = (gfloat *)data;
	(void)sx;
	(void)sy;
	wlr_scene_buffer_set_opacity(buffer, *alpha);
}

/**
 * gowl_client_get_alpha:
 */
gfloat
gowl_client_get_alpha(GowlClient *self)
{
	g_return_val_if_fail(GOWL_IS_CLIENT(self), 1.0f);
	return self->alpha;
}

/**
 * gowl_client_set_alpha:
 */
void
gowl_client_set_alpha(
	GowlClient *self,
	gfloat      alpha
){
	g_return_if_fail(GOWL_IS_CLIENT(self));

	if (alpha < 0.0f) alpha = 0.0f;
	if (alpha > 1.0f) alpha = 1.0f;

	self->alpha = alpha;

	/* Walk only the client's own surface tree, not the full scene
	 * container.  The container (self->scene) may include reparented
	 * child clients (embedded apps) whose opacity is managed
	 * separately — walking the container would overwrite their alpha. */
	if (self->scene_surface != NULL)
		wlr_scene_node_for_each_buffer(&self->scene_surface->node,
		                                set_opacity_iter, &self->alpha);
	else if (self->scene != NULL)
		wlr_scene_node_for_each_buffer(&self->scene->node,
		                                set_opacity_iter, &self->alpha);
}

/**
 * gowl_client_set_rule_overrides:
 *
 * Stashes initial-placement overrides for on_client_map() to
 * consume after the client-pre-map signal handlers run.  The
 * compositor clears these fields after reading them, so this
 * setter is only meaningful when called from a client-pre-map
 * handler on a client that has not yet been placed.
 */
void
gowl_client_set_rule_overrides(
	GowlClient *self,
	guint32     tags,
	gint        monitor,
	gboolean    geom_set
){
	g_return_if_fail(GOWL_IS_CLIENT(self));

	self->pending_rule_tags     = tags;
	self->pending_rule_monitor  = monitor;
	self->pending_rule_geom_set = geom_set;
}

/* -------------------------------------------------------------------
 * Mirror views.  Implementation delegates the wlroots plumbing to
 * gowl-mirror.c via two private helpers:
 *   _gowl_mirror_new_for_client()      — constructs a GowlMirror
 *                                        bound to the given scene
 *                                        tree and surface.
 *   _gowl_mirror_update_geometry()     — move/resize.
 *   _gowl_mirror_detach_from_client()  — drop listeners before the
 *                                        client's surface is freed.
 * ------------------------------------------------------------------- */

extern GowlMirror *_gowl_mirror_new_for_client(GowlClient            *client,
                                                struct wlr_scene_tree *parent,
                                                struct wlr_surface    *surface,
                                                guint64                view_id,
                                                gint                   x,
                                                gint                   y,
                                                gint                   w,
                                                gint                   h);

extern void _gowl_mirror_update_geometry(GowlMirror *self,
                                          gint        x,
                                          gint        y,
                                          gint        w,
                                          gint        h);

GowlMirror *
gowl_client_add_mirror(GowlClient *self,
                        gint        x,
                        gint        y,
                        gint        w,
                        gint        h)
{
	GowlMirror            *mirror;
	struct wlr_scene_tree *parent;
	struct wlr_surface    *surface;

	g_return_val_if_fail(GOWL_IS_CLIENT(self), NULL);
	g_return_val_if_fail(self->compositor != NULL, NULL);
	g_return_val_if_fail(self->xdg_toplevel != NULL, NULL);

	/* Mirror scene node lives on the overlay layer so it floats
	 * above tiled clients, matching emskin's behaviour when the
	 * same client is shown in multiple Emacs windows. */
	parent = self->compositor->layers[GOWL_SCENE_LAYER_OVERLAY];
	surface = self->xdg_toplevel->base->surface;

	mirror = _gowl_mirror_new_for_client(self,
	                                      parent,
	                                      surface,
	                                      self->next_mirror_id++,
	                                      x, y, w, h);
	if (mirror == NULL)
		return NULL;

	self->mirrors = g_list_append(self->mirrors, g_object_ref(mirror));

	g_signal_emit(self, client_signals[SIGNAL_MIRROR_ADDED], 0, mirror);

	/* Drop the factory reference — the client's GList now holds
	 * the strong ref, and callers get a borrowed pointer. */
	g_object_unref(mirror);
	return mirror;
}

void
gowl_client_update_mirror(GowlClient *self,
                           GowlMirror *mirror,
                           gint        x,
                           gint        y,
                           gint        w,
                           gint        h)
{
	g_return_if_fail(GOWL_IS_CLIENT(self));
	g_return_if_fail(GOWL_IS_MIRROR(mirror));

	if (g_list_find(self->mirrors, mirror) == NULL)
		return;

	_gowl_mirror_update_geometry(mirror, x, y, w, h);
}

void
gowl_client_remove_mirror(GowlClient *self,
                           GowlMirror *mirror)
{
	GList *link;

	g_return_if_fail(GOWL_IS_CLIENT(self));
	g_return_if_fail(GOWL_IS_MIRROR(mirror));

	link = g_list_find(self->mirrors, mirror);
	if (link == NULL)
		return;

	/* Emit before teardown so listeners see a live GObject. */
	g_signal_emit(self, client_signals[SIGNAL_MIRROR_REMOVED],
	              0, mirror);

	_gowl_mirror_detach_from_client(mirror);

	self->mirrors = g_list_delete_link(self->mirrors, link);
	g_object_unref(mirror);
}

GList *
gowl_client_list_mirrors(GowlClient *self)
{
	g_return_val_if_fail(GOWL_IS_CLIENT(self), NULL);

	return g_list_copy(self->mirrors);
}
