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

#include "gowl-mirror.h"
#include "gowl-core-private.h"

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_compositor.h>

/* Forward declaration: gowl-client.c notifies a mirror when its
 * owning client is destroyed so it can detach cleanly. */
void _gowl_mirror_detach_from_client(GowlMirror *self);

struct _GowlMirrorPrivate {
	GowlClient              *client;       /* weak, cleared by client  */
	guint64                  view_id;
	struct wlr_scene_buffer *scene_buffer; /* owned until removed      */
	struct wl_listener       surface_commit;
	gint                     x, y, w, h;
};

typedef struct _GowlMirrorPrivate GowlMirrorPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(GowlMirror, gowl_mirror, G_TYPE_OBJECT)

#define GET_PRIV(self) \
	((GowlMirrorPrivate *)gowl_mirror_get_instance_private(self))

/* ---------------------------------------------------------------
 * wlroots commit bridge: when the client surface commits a new
 * buffer, update the mirror's scene_buffer so it reflects the
 * latest frame.  Sub-surfaces are not walked — only the root
 * surface buffer is mirrored (see header Scope note).
 * --------------------------------------------------------------- */

static void
mirror_on_surface_commit(struct wl_listener *listener, void *data)
{
	GowlMirrorPrivate *priv = wl_container_of(listener, priv,
	                                           surface_commit);
	struct wlr_surface *surface = data;
	struct wlr_buffer  *buf;

	if (priv->scene_buffer == NULL)
		return;

	buf = (surface != NULL && surface->buffer != NULL)
		? &surface->buffer->base
		: NULL;
	wlr_scene_buffer_set_buffer(priv->scene_buffer, buf);
}

/* ---------------------------------------------------------------
 * GObject lifecycle
 * --------------------------------------------------------------- */

static void
gowl_mirror_dispose(GObject *object)
{
	GowlMirror        *self = GOWL_MIRROR(object);
	GowlMirrorPrivate *priv = GET_PRIV(self);

	/* Detach from wlroots if we still have a live subscription.
	 * gowl-client.c calls _gowl_mirror_detach_from_client() to
	 * pre-empt this when the client is destroyed first. */
	if (priv->surface_commit.notify != NULL) {
		wl_list_remove(&priv->surface_commit.link);
		priv->surface_commit.notify = NULL;
	}

	if (priv->scene_buffer != NULL) {
		wlr_scene_node_destroy(&priv->scene_buffer->node);
		priv->scene_buffer = NULL;
	}

	priv->client = NULL;

	G_OBJECT_CLASS(gowl_mirror_parent_class)->dispose(object);
}

static void
gowl_mirror_class_init(GowlMirrorClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = gowl_mirror_dispose;
}

static void
gowl_mirror_init(GowlMirror *self)
{
	GowlMirrorPrivate *priv = GET_PRIV(self);

	priv->client                 = NULL;
	priv->view_id                = 0;
	priv->scene_buffer           = NULL;
	priv->surface_commit.notify  = NULL;
	wl_list_init(&priv->surface_commit.link);
	priv->x = priv->y = priv->w = priv->h = 0;
}

/* ---------------------------------------------------------------
 * Private API — called from gowl-client.c where the wlroots scene
 * and surface are accessible.  Must live in this file so the
 * GowlMirrorPrivate struct stays private to the mirror module.
 * --------------------------------------------------------------- */

/**
 * _gowl_mirror_new_for_client: (skip)
 *
 * Internal helper used by #gowl_client_add_mirror.  Constructs a
 * mirror, attaches it to the given parent scene tree rendering
 * @initial_buffer, registers a commit listener on @surface, and
 * sets the initial geometry.
 *
 * The caller (gowl-client.c) tracks the returned mirror in the
 * client's mirror list and emits the `mirror-added' signal.
 */
GowlMirror *
_gowl_mirror_new_for_client(GowlClient             *client,
                             struct wlr_scene_tree  *parent,
                             struct wlr_surface     *surface,
                             guint64                 view_id,
                             gint                    x,
                             gint                    y,
                             gint                    w,
                             gint                    h)
{
	GowlMirror        *self;
	GowlMirrorPrivate *priv;
	struct wlr_buffer *initial = NULL;

	self = g_object_new(GOWL_TYPE_MIRROR, NULL);
	priv = GET_PRIV(self);

	priv->client  = client;
	priv->view_id = view_id;
	priv->x = x;
	priv->y = y;
	priv->w = w;
	priv->h = h;

	if (surface != NULL && surface->buffer != NULL)
		initial = &surface->buffer->base;

	priv->scene_buffer = wlr_scene_buffer_create(parent, initial);
	if (priv->scene_buffer != NULL) {
		wlr_scene_node_set_position(&priv->scene_buffer->node,
		                             x, y);
		if (w > 0 && h > 0)
			wlr_scene_buffer_set_dest_size(priv->scene_buffer,
			                                w, h);
	}

	if (surface != NULL) {
		priv->surface_commit.notify = mirror_on_surface_commit;
		wl_signal_add(&surface->events.commit,
		              &priv->surface_commit);
	}

	return self;
}

/**
 * _gowl_mirror_update_geometry: (skip)
 */
void
_gowl_mirror_update_geometry(GowlMirror *self,
                              gint        x,
                              gint        y,
                              gint        w,
                              gint        h)
{
	GowlMirrorPrivate *priv;

	g_return_if_fail(GOWL_IS_MIRROR(self));

	priv = GET_PRIV(self);
	priv->x = x;
	priv->y = y;
	priv->w = w;
	priv->h = h;

	if (priv->scene_buffer != NULL) {
		wlr_scene_node_set_position(&priv->scene_buffer->node,
		                             x, y);
		if (w > 0 && h > 0)
			wlr_scene_buffer_set_dest_size(priv->scene_buffer,
			                                w, h);
	}
}

/**
 * _gowl_mirror_detach_from_client: (skip)
 *
 * Called by the client's destroy path BEFORE the client's own
 * surface is freed, so the mirror can drop its commit listener
 * cleanly.  After this the mirror is "dead" — no scene node, no
 * listener — but the GObject itself can still be referenced
 * until the owner drops it.
 */
void
_gowl_mirror_detach_from_client(GowlMirror *self)
{
	GowlMirrorPrivate *priv;

	g_return_if_fail(GOWL_IS_MIRROR(self));

	priv = GET_PRIV(self);

	if (priv->surface_commit.notify != NULL) {
		wl_list_remove(&priv->surface_commit.link);
		priv->surface_commit.notify = NULL;
	}

	if (priv->scene_buffer != NULL) {
		wlr_scene_node_destroy(&priv->scene_buffer->node);
		priv->scene_buffer = NULL;
	}

	priv->client = NULL;
}

/* ---------------------------------------------------------------
 * Public accessors
 * --------------------------------------------------------------- */

guint64
gowl_mirror_get_view_id(GowlMirror *self)
{
	g_return_val_if_fail(GOWL_IS_MIRROR(self), 0);
	return GET_PRIV(self)->view_id;
}

gpointer
gowl_mirror_get_client(GowlMirror *self)
{
	g_return_val_if_fail(GOWL_IS_MIRROR(self), NULL);
	return GET_PRIV(self)->client;
}

void
gowl_mirror_get_geometry(GowlMirror *self,
                          gint       *x,
                          gint       *y,
                          gint       *w,
                          gint       *h)
{
	GowlMirrorPrivate *priv;

	g_return_if_fail(GOWL_IS_MIRROR(self));
	priv = GET_PRIV(self);

	if (x != NULL) *x = priv->x;
	if (y != NULL) *y = priv->y;
	if (w != NULL) *w = priv->w;
	if (h != NULL) *h = priv->h;
}
