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

#ifndef GOWL_CAIRO_EMBED_RENDERER_H
#define GOWL_CAIRO_EMBED_RENDERER_H

#include <glib-object.h>
#include "../interfaces/gowl-embed-renderer.h"

G_BEGIN_DECLS

#define GOWL_TYPE_CAIRO_EMBED_RENDERER \
	(gowl_cairo_embed_renderer_get_type())

G_DECLARE_DERIVABLE_TYPE(GowlCairoEmbedRenderer,
                          gowl_cairo_embed_renderer,
                          GOWL, CAIRO_EMBED_RENDERER, GowlEmbedRenderer)

/**
 * GowlCairoEmbedRendererClass:
 * @parent_class: parent #GowlEmbedRendererClass
 *
 * Concrete #GowlEmbedRenderer implementation covering the CPU
 * readback + Cairo draw path.  This is the fallback renderer that
 * works with any client, including SHM-only terminals.  It is the
 * renderer the integration layer selects when
 * #gowl_embed_renderer_supports_dmabuf returns %FALSE for the
 * active subclass.
 *
 * The actual pixel capture logic still lives in
 * cmacs/gowl/cmacs-gowl.c for now (the `gowl_embed_view' family).
 * This class exists to:
 *   - expose the path as a discoverable GType in GIR,
 *   - give the abstract #GowlEmbedRenderer contract a working
 *     reference implementation,
 *   - provide a stable vtable for the integration layer to swap
 *     between Cairo and future GL renderers when a client signals
 *     that it has a usable DMA-BUF.
 *
 * Standalone gowl never instantiates this class; the xwidget
 * pathway it targets only exists in the cmacs integration.
 */
struct _GowlCairoEmbedRendererClass {
	GowlEmbedRendererClass parent_class;
};

/**
 * gowl_cairo_embed_renderer_new:
 *
 * Returns: (transfer full): a new renderer instance.  No client
 *   is attached; call #gowl_embed_renderer_attach to bind one.
 */
GowlCairoEmbedRenderer *
gowl_cairo_embed_renderer_new(void);

G_END_DECLS

#endif /* GOWL_CAIRO_EMBED_RENDERER_H */
