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

#ifndef GOWL_CAPTURE_RESULT_H
#define GOWL_CAPTURE_RESULT_H

#include "gowl-types.h"

G_BEGIN_DECLS

#define GOWL_TYPE_CAPTURE_RESULT (gowl_capture_result_get_type())

/**
 * GowlCaptureResult:
 * @data: (nullable): raw RGBA pixel data (4 bytes per pixel, row-major).
 * @width: image width in pixels.
 * @height: image height in pixels.
 * @stride: bytes per row (may include padding).
 * @path: (nullable): saved file path, or %NULL if not saved to disk.
 * @cancelled: %TRUE if the user cancelled an interactive selection.
 *
 * Boxed type carrying the result of a screen capture operation.
 * Emitted by #GowlScreenshotProvider via the "capture-complete" signal
 * and returned by compositor screenshot functions.
 */
struct _GowlCaptureResult {
	GBytes   *data;
	gint      width;
	gint      height;
	gint      stride;
	gchar    *path;
	gboolean  cancelled;
};

GType               gowl_capture_result_get_type   (void) G_GNUC_CONST;

/**
 * gowl_capture_result_new:
 * @data: (transfer none) (nullable): RGBA pixel data
 * @width: image width
 * @height: image height
 * @stride: bytes per row
 * @path: (nullable): file path
 * @cancelled: whether the capture was cancelled
 *
 * Allocates a new #GowlCaptureResult.  The @data reference is
 * copied (ref'd); the @path string is duplicated.
 *
 * Returns: (transfer full): a newly allocated #GowlCaptureResult.
 *          Free with gowl_capture_result_free().
 */
GowlCaptureResult * gowl_capture_result_new        (GBytes            *data,
                                                     gint               width,
                                                     gint               height,
                                                     gint               stride,
                                                     const gchar       *path,
                                                     gboolean           cancelled);

/**
 * gowl_capture_result_copy:
 * @self: (not nullable): a #GowlCaptureResult
 *
 * Creates a deep copy.
 *
 * Returns: (transfer full): a copy of @self
 */
GowlCaptureResult * gowl_capture_result_copy       (const GowlCaptureResult *self);

/**
 * gowl_capture_result_free:
 * @self: (transfer full): a #GowlCaptureResult
 *
 * Frees @self and its contents.
 */
void                gowl_capture_result_free        (GowlCaptureResult *self);

/**
 * gowl_capture_result_get_data:
 * @self: a #GowlCaptureResult
 *
 * Returns: (transfer none) (nullable): the raw pixel data
 */
GBytes *            gowl_capture_result_get_data    (const GowlCaptureResult *self);

/**
 * gowl_capture_result_get_width:
 * @self: a #GowlCaptureResult
 *
 * Returns: the image width in pixels
 */
gint                gowl_capture_result_get_width   (const GowlCaptureResult *self);

/**
 * gowl_capture_result_get_height:
 * @self: a #GowlCaptureResult
 *
 * Returns: the image height in pixels
 */
gint                gowl_capture_result_get_height  (const GowlCaptureResult *self);

/**
 * gowl_capture_result_get_path:
 * @self: a #GowlCaptureResult
 *
 * Returns: (transfer none) (nullable): the saved file path, or %NULL
 */
const gchar *       gowl_capture_result_get_path    (const GowlCaptureResult *self);

/**
 * gowl_capture_result_is_cancelled:
 * @self: a #GowlCaptureResult
 *
 * Returns: %TRUE if the capture was cancelled by the user
 */
gboolean            gowl_capture_result_is_cancelled(const GowlCaptureResult *self);

G_END_DECLS

#endif /* GOWL_CAPTURE_RESULT_H */
