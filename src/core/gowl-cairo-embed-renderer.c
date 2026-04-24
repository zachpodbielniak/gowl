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

#include "gowl-cairo-embed-renderer.h"

typedef struct {
	gpointer client;      /* GowlClient pointer, unowned */
	gint     target_w;
	gint     target_h;
} GowlCairoEmbedRendererPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(GowlCairoEmbedRenderer,
                           gowl_cairo_embed_renderer,
                           GOWL_TYPE_EMBED_RENDERER)

#define GET_PRIV(self) \
	((GowlCairoEmbedRendererPrivate *) \
	 gowl_cairo_embed_renderer_get_instance_private(self))

static void
cairo_attach(GowlEmbedRenderer *base, gpointer client)
{
	GowlCairoEmbedRenderer        *self = GOWL_CAIRO_EMBED_RENDERER(base);
	GowlCairoEmbedRendererPrivate *priv = GET_PRIV(self);

	priv->client = client;
}

static void
cairo_commit(GowlEmbedRenderer *base)
{
	/* The actual pixel pull happens in cmacs/gowl/cmacs-gowl.c's
	 * `gowl_embed_view_on_commit' path today.  This method exists
	 * so the abstract contract has an attachable vfunc the
	 * integration layer can drive once the pixel code migrates
	 * over.  Until then this is a no-op placeholder. */
	(void)base;
}

static void
cairo_resize(GowlEmbedRenderer *base, gint width, gint height)
{
	GowlCairoEmbedRenderer        *self = GOWL_CAIRO_EMBED_RENDERER(base);
	GowlCairoEmbedRendererPrivate *priv = GET_PRIV(self);

	priv->target_w = width;
	priv->target_h = height;
}

static void
cairo_detach(GowlEmbedRenderer *base)
{
	GowlCairoEmbedRenderer        *self = GOWL_CAIRO_EMBED_RENDERER(base);
	GowlCairoEmbedRendererPrivate *priv = GET_PRIV(self);

	priv->client = NULL;
}

static gboolean
cairo_supports_dmabuf(GowlEmbedRenderer *base)
{
	(void)base;
	return FALSE;  /* CPU readback path; DMA-BUF lives in the GL
	                * subclass once it ships. */
}

static void
gowl_cairo_embed_renderer_class_init(GowlCairoEmbedRendererClass *klass)
{
	GowlEmbedRendererClass *renderer_class =
		GOWL_EMBED_RENDERER_CLASS(klass);

	renderer_class->attach          = cairo_attach;
	renderer_class->commit          = cairo_commit;
	renderer_class->resize          = cairo_resize;
	renderer_class->detach          = cairo_detach;
	renderer_class->supports_dmabuf = cairo_supports_dmabuf;
}

static void
gowl_cairo_embed_renderer_init(GowlCairoEmbedRenderer *self)
{
	GowlCairoEmbedRendererPrivate *priv = GET_PRIV(self);

	priv->client   = NULL;
	priv->target_w = 0;
	priv->target_h = 0;
}

GowlCairoEmbedRenderer *
gowl_cairo_embed_renderer_new(void)
{
	return g_object_new(GOWL_TYPE_CAIRO_EMBED_RENDERER, NULL);
}
