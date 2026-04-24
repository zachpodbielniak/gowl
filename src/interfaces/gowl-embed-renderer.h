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

#ifndef GOWL_EMBED_RENDERER_H
#define GOWL_EMBED_RENDERER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_EMBED_RENDERER (gowl_embed_renderer_get_type())

G_DECLARE_DERIVABLE_TYPE(GowlEmbedRenderer, gowl_embed_renderer,
                          GOWL, EMBED_RENDERER, GObject)

/**
 * GowlEmbedRendererClass:
 * @parent_class: parent GObjectClass
 * @attach: vfunc — take ownership of the client's surface buffer
 *   stream; must return quickly.
 * @commit: vfunc — a new frame is available; render it.  Called
 *   from the compositor dispatch tick.
 * @resize: vfunc — widget dimensions changed; update internal
 *   target.  Dimensions are in logical pixels.
 * @detach: vfunc — release the surface; after this the renderer
 *   must not touch the client or its buffer.
 * @supports_dmabuf: vfunc — introspection hint: does this
 *   implementation consume `wlr_buffer_get_dmabuf`?  The
 *   #GowlAppBuffer uses this to decide whether to prefer a GL
 *   renderer over the SHM/Cairo fallback.
 *
 * Abstract contract for embed-path renderers.  The default
 * installed by cmacs `--gowl` is #GowlCairoEmbedRenderer (SHM
 * readback + Cairo, works with any client).  When a client
 * exposes a DMA-BUF via `wlr_buffer_get_dmabuf`, the app buffer
 * may swap in #GowlGlEmbedRenderer (GL texture import, zero
 * memcpy).  The swap is transparent to callers because the
 * contract — attach / commit / resize / detach — is the same.
 *
 * Standalone gowl never instantiates these; the xwidget-based
 * rendering path only exists under the cmacs integration.
 */
struct _GowlEmbedRendererClass {
	GObjectClass parent_class;

	void     (*attach)          (GowlEmbedRenderer *self,
	                              gpointer           client);
	void     (*commit)          (GowlEmbedRenderer *self);
	void     (*resize)          (GowlEmbedRenderer *self,
	                              gint               width,
	                              gint               height);
	void     (*detach)          (GowlEmbedRenderer *self);
	gboolean (*supports_dmabuf) (GowlEmbedRenderer *self);
};

/**
 * gowl_embed_renderer_attach:
 * @self: a #GowlEmbedRenderer
 * @client: (nullable): a #GowlClient to render, or %NULL to clear
 *
 * Binds @client to the renderer.  Passing %NULL detaches without
 * destroying the renderer instance.
 */
void
gowl_embed_renderer_attach(GowlEmbedRenderer *self,
                            gpointer           client);

/**
 * gowl_embed_renderer_commit:
 * @self: a #GowlEmbedRenderer
 *
 * A new frame is available on the attached client's surface.
 * Implementations pull the latest buffer and present it.
 */
void
gowl_embed_renderer_commit(GowlEmbedRenderer *self);

/**
 * gowl_embed_renderer_resize:
 * @self: a #GowlEmbedRenderer
 * @width: target width in logical pixels
 * @height: target height in logical pixels
 *
 * Hint: the display widget size changed; adjust the target.
 */
void
gowl_embed_renderer_resize(GowlEmbedRenderer *self,
                            gint               width,
                            gint               height);

/**
 * gowl_embed_renderer_detach:
 * @self: a #GowlEmbedRenderer
 *
 * Releases the attached client.  Safe when nothing is attached.
 */
void
gowl_embed_renderer_detach(GowlEmbedRenderer *self);

/**
 * gowl_embed_renderer_supports_dmabuf:
 * @self: a #GowlEmbedRenderer
 *
 * Returns: %TRUE if the renderer consumes client DMA-BUFs
 *          directly (zero-copy GL path).  Subclasses default to
 *          %FALSE unless they override the vfunc.
 */
gboolean
gowl_embed_renderer_supports_dmabuf(GowlEmbedRenderer *self);

G_END_DECLS

#endif /* GOWL_EMBED_RENDERER_H */
