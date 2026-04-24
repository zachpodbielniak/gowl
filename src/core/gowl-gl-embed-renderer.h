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

#ifndef GOWL_GL_EMBED_RENDERER_H
#define GOWL_GL_EMBED_RENDERER_H

#include <glib-object.h>
#include "../interfaces/gowl-embed-renderer.h"

G_BEGIN_DECLS

#define GOWL_TYPE_GL_EMBED_RENDERER (gowl_gl_embed_renderer_get_type())

G_DECLARE_DERIVABLE_TYPE(GowlGlEmbedRenderer, gowl_gl_embed_renderer,
                          GOWL, GL_EMBED_RENDERER, GowlEmbedRenderer)

/**
 * GowlGlEmbedRendererClass:
 * @parent_class: parent #GowlEmbedRendererClass
 *
 * DMA-BUF-based concrete renderer.  On every commit it calls
 * `wlr_buffer_get_dmabuf()` on the client's current buffer,
 * imports the DMA-BUF as an `EGLImageKHR` via
 * `EGL_EXT_image_dma_buf_import`, and binds the image to a GL
 * texture with `glEGLImageTargetTexture2DOES`.  The caller (e.g. a
 * `GtkGLArea` render callback) reads `gowl_gl_embed_renderer_get_texture`
 * to paint a quad.
 *
 * Construction path:
 *   renderer = gowl_gl_embed_renderer_new(egl_display);
 *   gowl_embed_renderer_attach(renderer, client);
 *   // on each frame callback...
 *   gowl_embed_renderer_commit(renderer);
 *   texture = gowl_gl_embed_renderer_get_texture(renderer);
 *
 * The renderer does NOT create its own GL context or framebuffer;
 * it expects the caller to have a current GL context when commit
 * runs.  GtkGLArea makes its context current around `render` and
 * `realize`, so driving the renderer from those callbacks is
 * natural.
 *
 * If the host EGL does not advertise
 * `EGL_EXT_image_dma_buf_import`, `supports_dmabuf` returns %FALSE
 * and the owner should fall back to #GowlCairoEmbedRenderer.
 * This keeps the GL renderer's behaviour monotone: either it
 * works end-to-end or it stays dormant.
 */
struct _GowlGlEmbedRendererClass {
	GowlEmbedRendererClass parent_class;
};

/**
 * gowl_gl_embed_renderer_new:
 * @egl_display: the `EGLDisplay` to use.  Must be connected to
 *   the same GPU as the client's DMA-BUFs; typically obtained
 *   from the host compositor via
 *   `gdk_wayland_display_get_egl_display()` on pgtk.
 *
 * Probes the display for `EGL_EXT_image_dma_buf_import` and caches
 * the function pointers.  On failure returns the renderer with
 * `supports_dmabuf` reporting %FALSE so the caller can downgrade
 * to Cairo.
 *
 * Returns: (transfer full) (nullable): a new renderer, or %NULL
 *          on allocation failure.
 */
GowlGlEmbedRenderer *
gowl_gl_embed_renderer_new(gpointer egl_display);

/**
 * gowl_gl_embed_renderer_get_texture:
 * @self: a #GowlGlEmbedRenderer
 *
 * Returns: the GL texture name the most recent commit produced,
 *          or 0 if no frame has been imported yet.  The texture
 *          is `GL_TEXTURE_2D`; bind it against the active context
 *          to draw.  Lifetime: owned by the renderer; callers
 *          must not glDeleteTextures() it.
 */
guint
gowl_gl_embed_renderer_get_texture(GowlGlEmbedRenderer *self);

/**
 * gowl_gl_embed_renderer_get_texture_size:
 * @self: a #GowlGlEmbedRenderer
 * @width: (out): the texture width in pixels
 * @height: (out): the texture height in pixels
 *
 * Reads the dimensions of the most recently imported frame.
 * Both outputs are set to 0 if no frame has been imported yet.
 */
void
gowl_gl_embed_renderer_get_texture_size(GowlGlEmbedRenderer *self,
                                         gint                *width,
                                         gint                *height);

G_END_DECLS

#endif /* GOWL_GL_EMBED_RENDERER_H */
