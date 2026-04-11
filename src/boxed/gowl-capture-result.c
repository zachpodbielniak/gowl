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

#include "gowl-capture-result.h"

G_DEFINE_BOXED_TYPE(GowlCaptureResult, gowl_capture_result,
                    gowl_capture_result_copy, gowl_capture_result_free)

GowlCaptureResult *
gowl_capture_result_new(
	GBytes      *data,
	gint         width,
	gint         height,
	gint         stride,
	const gchar *path,
	gboolean     cancelled
){
	GowlCaptureResult *self;

	self = g_slice_new(GowlCaptureResult);
	self->data      = data != NULL ? g_bytes_ref(data) : NULL;
	self->width     = width;
	self->height    = height;
	self->stride    = stride;
	self->path      = g_strdup(path);
	self->cancelled = cancelled;

	return self;
}

GowlCaptureResult *
gowl_capture_result_copy(const GowlCaptureResult *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return gowl_capture_result_new(self->data, self->width, self->height,
	                               self->stride, self->path, self->cancelled);
}

void
gowl_capture_result_free(GowlCaptureResult *self)
{
	if (self == NULL)
		return;

	g_clear_pointer(&self->data, g_bytes_unref);
	g_free(self->path);
	g_slice_free(GowlCaptureResult, self);
}

GBytes *
gowl_capture_result_get_data(const GowlCaptureResult *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->data;
}

gint
gowl_capture_result_get_width(const GowlCaptureResult *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->width;
}

gint
gowl_capture_result_get_height(const GowlCaptureResult *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->height;
}

const gchar *
gowl_capture_result_get_path(const GowlCaptureResult *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->path;
}

gboolean
gowl_capture_result_is_cancelled(const GowlCaptureResult *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	return self->cancelled;
}
