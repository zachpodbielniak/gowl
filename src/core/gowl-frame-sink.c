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

#include "gowl-frame-sink.h"

#include <string.h>
#include <drm_fourcc.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_buffer.h>

/* ---------------------------------------------------------------------------
 * Pure helpers
 * ------------------------------------------------------------------------- */

gint
gowl_frame_default_stride(gint width)
{
	if (width <= 0)
		return 0;
	return width * 4;
}

gboolean
gowl_frame_dims_valid(gint width, gint height, gint stride)
{
	if (width <= 0 || height <= 0)
		return FALSE;
	if (stride < width * 4)
		return FALSE;
	return TRUE;
}

gsize
gowl_frame_buffer_size(gint height, gint stride)
{
	if (height <= 0 || stride <= 0)
		return 0;
	return (gsize)height * (gsize)stride;
}

/* ---------------------------------------------------------------------------
 * GowlRawBuffer — struct wlr_buffer over a g_malloc'd ARGB8888 copy
 * ------------------------------------------------------------------------- */

typedef struct {
	struct wlr_buffer base;
	guint8           *pixels;   /* owned ARGB8888 copy */
	gsize             size;
	gint              stride;
} GowlRawBuffer;

static void
raw_buffer_destroy(struct wlr_buffer *buf)
{
	GowlRawBuffer *self;

	self = wl_container_of(buf, self, base);
	g_free(self->pixels);
	g_free(self);
}

static bool
raw_buffer_begin_data_ptr_access(
	struct wlr_buffer *buf,
	uint32_t           flags,
	void             **data,
	uint32_t          *format,
	size_t            *stride
){
	GowlRawBuffer *self;

	(void)flags;
	self = wl_container_of(buf, self, base);
	*data   = (void *)self->pixels;
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)self->stride;
	return true;
}

static void
raw_buffer_end_data_ptr_access(struct wlr_buffer *buf)
{
	(void)buf;
}

static const struct wlr_buffer_impl raw_buffer_impl = {
	.destroy               = raw_buffer_destroy,
	.begin_data_ptr_access = raw_buffer_begin_data_ptr_access,
	.end_data_ptr_access   = raw_buffer_end_data_ptr_access,
};

struct wlr_buffer *
gowl_raw_buffer_create(
	const guint8 *pixels,
	gint          width,
	gint          height,
	gint          stride
){
	GowlRawBuffer *buf;
	gsize          size;

	if (pixels == NULL)
		return NULL;
	if (!gowl_frame_dims_valid(width, height, stride))
		return NULL;

	size = gowl_frame_buffer_size(height, stride);
	buf = (GowlRawBuffer *)g_new0(GowlRawBuffer, 1);
	buf->pixels = (guint8 *)g_malloc(size);
	memcpy(buf->pixels, pixels, size);
	buf->size   = size;
	buf->stride = stride;
	wlr_buffer_init(&buf->base, &raw_buffer_impl, width, height);
	return &buf->base;
}

/* ---------------------------------------------------------------------------
 * GowlFrameSink
 * ------------------------------------------------------------------------- */

struct _GowlFrameSink {
	struct wlr_scene_tree *layer;          /* borrowed */
	GHashTable            *buffers;         /* name -> struct wlr_scene_buffer* */
	gboolean               keep_at_bottom;
};

static void
sink_value_destroy(gpointer data)
{
	struct wlr_scene_buffer *sb;

	sb = (struct wlr_scene_buffer *)data;
	if (sb != NULL)
		wlr_scene_node_destroy(&sb->node);
}

GowlFrameSink *
gowl_frame_sink_new(struct wlr_scene_tree *layer, gboolean keep_at_bottom)
{
	GowlFrameSink *self;

	g_return_val_if_fail(layer != NULL, NULL);

	self = g_new0(GowlFrameSink, 1);
	self->layer = layer;
	self->keep_at_bottom = keep_at_bottom;
	self->buffers = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                       g_free, sink_value_destroy);
	return self;
}

gboolean
gowl_frame_sink_push(
	GowlFrameSink *self,
	const gchar   *mon_name,
	gint           x,
	gint           y,
	const guint8  *pixels,
	gint           width,
	gint           height,
	gint           stride
){
	struct wlr_buffer       *buf;
	struct wlr_scene_buffer *sb;

	if (self == NULL || mon_name == NULL)
		return FALSE;

	buf = gowl_raw_buffer_create(pixels, width, height, stride);
	if (buf == NULL)
		return FALSE;

	sb = (struct wlr_scene_buffer *)g_hash_table_lookup(self->buffers,
	                                                     mon_name);
	if (sb != NULL) {
		/* Cheap per-frame swap: the node stays, only its buffer changes. */
		wlr_scene_buffer_set_buffer(sb, buf);
		wlr_buffer_drop(buf);
	} else {
		sb = wlr_scene_buffer_create(self->layer, buf);
		wlr_buffer_drop(buf);
		if (sb == NULL)
			return FALSE;
		g_hash_table_insert(self->buffers, g_strdup(mon_name), sb);
	}

	wlr_scene_node_set_position(&sb->node, x, y);
	if (self->keep_at_bottom)
		wlr_scene_node_lower_to_bottom(&sb->node);
	return TRUE;
}

void
gowl_frame_sink_clear(GowlFrameSink *self, const gchar *mon_name)
{
	if (self == NULL || mon_name == NULL)
		return;
	/* The value-destroy func tears down the scene node. */
	g_hash_table_remove(self->buffers, mon_name);
}

void
gowl_frame_sink_clear_all(GowlFrameSink *self)
{
	if (self == NULL)
		return;
	g_hash_table_remove_all(self->buffers);
}

gboolean
gowl_frame_sink_is_empty(GowlFrameSink *self)
{
	if (self == NULL)
		return TRUE;
	return g_hash_table_size(self->buffers) == 0;
}

void
gowl_frame_sink_free(GowlFrameSink *self)
{
	if (self == NULL)
		return;
	g_hash_table_destroy(self->buffers);
	g_free(self);
}
