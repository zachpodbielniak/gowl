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

#include "gowl-embed-renderer.h"

typedef struct {
	int _unused;
} GowlEmbedRendererPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(GowlEmbedRenderer, gowl_embed_renderer,
                           G_TYPE_OBJECT)

static gboolean
default_supports_dmabuf(GowlEmbedRenderer *self)
{
	(void)self;
	return FALSE;
}

static void
gowl_embed_renderer_class_init(GowlEmbedRendererClass *klass)
{
	klass->attach          = NULL;
	klass->commit          = NULL;
	klass->resize          = NULL;
	klass->detach          = NULL;
	klass->supports_dmabuf = default_supports_dmabuf;
}

static void
gowl_embed_renderer_init(GowlEmbedRenderer *self)
{
	(void)self;
}

void
gowl_embed_renderer_attach(GowlEmbedRenderer *self, gpointer client)
{
	GowlEmbedRendererClass *klass;

	g_return_if_fail(GOWL_IS_EMBED_RENDERER(self));

	klass = GOWL_EMBED_RENDERER_GET_CLASS(self);
	if (klass->attach != NULL)
		klass->attach(self, client);
}

void
gowl_embed_renderer_commit(GowlEmbedRenderer *self)
{
	GowlEmbedRendererClass *klass;

	g_return_if_fail(GOWL_IS_EMBED_RENDERER(self));

	klass = GOWL_EMBED_RENDERER_GET_CLASS(self);
	if (klass->commit != NULL)
		klass->commit(self);
}

void
gowl_embed_renderer_resize(GowlEmbedRenderer *self,
                            gint               width,
                            gint               height)
{
	GowlEmbedRendererClass *klass;

	g_return_if_fail(GOWL_IS_EMBED_RENDERER(self));

	klass = GOWL_EMBED_RENDERER_GET_CLASS(self);
	if (klass->resize != NULL)
		klass->resize(self, width, height);
}

void
gowl_embed_renderer_detach(GowlEmbedRenderer *self)
{
	GowlEmbedRendererClass *klass;

	g_return_if_fail(GOWL_IS_EMBED_RENDERER(self));

	klass = GOWL_EMBED_RENDERER_GET_CLASS(self);
	if (klass->detach != NULL)
		klass->detach(self);
}

gboolean
gowl_embed_renderer_supports_dmabuf(GowlEmbedRenderer *self)
{
	GowlEmbedRendererClass *klass;

	g_return_val_if_fail(GOWL_IS_EMBED_RENDERER(self), FALSE);

	klass = GOWL_EMBED_RENDERER_GET_CLASS(self);
	if (klass->supports_dmabuf != NULL)
		return klass->supports_dmabuf(self);
	return FALSE;
}
