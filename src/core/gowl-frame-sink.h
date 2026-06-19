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

/*
 * GowlFrameSink — push externally-produced raw frames into the scene graph.
 *
 * A #GowlFrameSink owns one wlr_scene_buffer per monitor on a chosen scene
 * layer and lets an OUTSIDE producer hand gowl finished ARGB8888 pixels to
 * display (animated wallpaper, animated lock-screen background, ...).
 *
 * Only a plain pixel span — (pixels, width, height, stride) — crosses the
 * boundary.  gowl never renders these frames and pulls in NO rendering engine:
 * this header includes only wlroots/glib/stdint, never libregnum, graylib,
 * raylib, or GL.  Producers (e.g. cmacs rendering libregnum screensavers) own
 * all of that on their side and simply call gowl_frame_sink_push().
 *
 * The pixel format is DRM_FORMAT_ARGB8888 (little-endian byte order B,G,R,A —
 * exactly what glReadPixels(GL_BGRA) produces, so no channel swizzle is
 * needed).
 */

#ifndef GOWL_FRAME_SINK_H
#define GOWL_FRAME_SINK_H

#include <glib.h>
#include <stdint.h>
#include <wlr/types/wlr_scene.h>

G_BEGIN_DECLS

/* ---------------------------------------------------------------------------
 * Pure helpers (no wlroots state — directly unit-testable).
 * ------------------------------------------------------------------------- */

/* Default ARGB8888 row stride in bytes for WIDTH pixels (0 if WIDTH <= 0). */
gint     gowl_frame_default_stride (gint width);

/* TRUE iff (width, height, stride) describe a usable ARGB8888 image:
 * width > 0, height > 0, and stride >= width * 4. */
gboolean gowl_frame_dims_valid     (gint width, gint height, gint stride);

/* Total byte count for HEIGHT rows of STRIDE bytes (0 if either <= 0). */
gsize    gowl_frame_buffer_size    (gint height, gint stride);

/* ---------------------------------------------------------------------------
 * GowlRawBuffer — a wlr_buffer over a COPY of caller-supplied pixels.
 * ------------------------------------------------------------------------- */

/* Wrap a freshly-allocated ARGB8888 copy of PIXELS (WIDTH x HEIGHT, STRIDE
 * bytes/row) in a struct wlr_buffer.  Returns NULL on NULL pixels or invalid
 * dims.  The returned buffer carries the creator's reference: hand it to
 * wlr_scene_buffer_create()/_set_buffer() (which take their own lock) and then
 * wlr_buffer_drop() it.  Pixels are copied, so the caller's buffer may be
 * reused/freed immediately. */
struct wlr_buffer *gowl_raw_buffer_create (const guint8 *pixels,
                                           gint          width,
                                           gint          height,
                                           gint          stride);

/* ---------------------------------------------------------------------------
 * GowlFrameSink — per-monitor scene buffers on one layer.
 * ------------------------------------------------------------------------- */

typedef struct _GowlFrameSink GowlFrameSink;

/* Create a sink attaching buffers to LAYER (borrowed; must outlive the sink).
 * When KEEP_AT_BOTTOM is TRUE every pushed node is lowered to the bottom of
 * LAYER on each push — used for the lock background so password UI drawn on the
 * same layer stays above it. */
GowlFrameSink *gowl_frame_sink_new (struct wlr_scene_tree *layer,
                                    gboolean               keep_at_bottom);

/* Create-or-replace the buffer for monitor MON_NAME, positioned at (X, Y).
 * Returns TRUE on success, FALSE (no-op) on NULL args, invalid dims, or NULL
 * pixels. */
gboolean gowl_frame_sink_push (GowlFrameSink *self,
                               const gchar   *mon_name,
                               gint           x,
                               gint           y,
                               const guint8  *pixels,
                               gint           width,
                               gint           height,
                               gint           stride);

/* Remove the buffer for MON_NAME (no-op if absent). */
void     gowl_frame_sink_clear (GowlFrameSink *self, const gchar *mon_name);

/* Remove every buffer. */
void     gowl_frame_sink_clear_all (GowlFrameSink *self);

/* TRUE if the sink currently holds no buffers. */
gboolean gowl_frame_sink_is_empty (GowlFrameSink *self);

/* Destroy the sink and all its scene buffers. */
void     gowl_frame_sink_free (GowlFrameSink *self);

G_END_DECLS

#endif /* GOWL_FRAME_SINK_H */
