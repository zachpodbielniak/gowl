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

#ifndef PORTAL_CAPTURE_H
#define PORTAL_CAPTURE_H

#include <glib.h>
#include <gio/gio.h>
#include <stdint.h>

G_BEGIN_DECLS

/*
 * PortalCapture -- the Wayland client half of the ScreenCast backend.
 *
 * A second connection to the compositor, separate from the one
 * PortalWayland holds for input capture, binding the standard
 * ext-image-copy-capture-v1 and ext-image-capture-source-v1 globals.
 * Those are what let this portal do the thing portal-wlr cannot: an
 * ext_foreign_toplevel_handle is a capture source, so a single window
 * can be shared without showing the whole screen.
 *
 * Nothing here knows about PipeWire or D-Bus.  A capture hands its
 * frames to a callback, which is where they are turned into something
 * an application can consume.
 */
typedef struct _PortalCapture PortalCapture;
typedef struct _PortalCaptureStream PortalCaptureStream;

/* One thing that can be cast: an output or a mapped toplevel. */
typedef struct {
	gboolean  is_window;
	gchar    *id;        /* the output's name, or the toplevel's identifier */
	gchar    *title;     /* what to show a human choosing */
	gchar    *app_id;    /* windows only */
	gpointer  handle;    /* wl_output* or ext_foreign_toplevel_handle_v1* */
} PortalCaptureSource;

/**
 * PortalCaptureReadyFunc:
 * @stream: the stream whose frame completed
 * @user_data: as given to portal_capture_stream_new()
 *
 * A frame finished copying into the buffer most recently attached with
 * portal_capture_stream_submit().  Called on the main loop.
 */
typedef void (*PortalCaptureReadyFunc)(PortalCaptureStream *stream,
                                       gpointer             user_data);

/**
 * PortalCaptureSizeFunc:
 * @stream: the stream
 * @width: buffer width the compositor wants
 * @height: buffer height
 * @format: the DRM fourcc of the shm format offered
 * @user_data: as given to portal_capture_stream_new()
 *
 * The compositor announced (or re-announced) the buffer constraints.
 * A source that is resized announces them again; the consumer has to
 * renegotiate its buffers before the next submit.
 */
typedef void (*PortalCaptureSizeFunc)(PortalCaptureStream *stream,
                                      guint32              width,
                                      guint32              height,
                                      guint32              format,
                                      gpointer             user_data);

/**
 * PortalCaptureStoppedFunc:
 * @stream: the stream that ended
 * @user_data: as given to portal_capture_stream_new()
 *
 * The source went away -- the window closed, the output was unplugged.
 * The stream is dead; the consumer should tear its session down.
 */
typedef void (*PortalCaptureStoppedFunc)(PortalCaptureStream *stream,
                                         gpointer             user_data);

PortalCapture *portal_capture_new  (GError **error);
void           portal_capture_free (PortalCapture *self);

/**
 * portal_capture_available:
 * @self: a #PortalCapture
 * @windows: (out) (nullable): whether single windows can be cast
 *
 * Whether the compositor offers image-copy-capture at all.  A
 * compositor built against wlroots without the toplevel source manager
 * still casts outputs.
 *
 * Returns: %TRUE if anything can be cast
 */
gboolean portal_capture_available (PortalCapture *self, gboolean *windows);

/**
 * portal_capture_list_sources:
 * @self: a #PortalCapture
 * @want_windows: include mapped toplevels
 * @want_outputs: include outputs
 *
 * Everything that can be cast right now, for the chooser to show.
 *
 * Returns: (transfer full) (element-type PortalCaptureSource): the list
 */
GPtrArray *portal_capture_list_sources (PortalCapture *self,
                                        gboolean       want_windows,
                                        gboolean       want_outputs);

/**
 * portal_capture_stream_new:
 * @self: a #PortalCapture
 * @source: the chosen source, borrowed for the call
 * @with_cursor: draw the pointer into the frames
 * @error: return location for an error
 *
 * Opens a capture session on @source.  The size callback fires before
 * any frame; nothing is captured until portal_capture_stream_submit()
 * is called with a buffer.
 *
 * Returns: (transfer full): the stream, or %NULL
 */
PortalCaptureStream *portal_capture_stream_new (PortalCapture             *self,
                                                const PortalCaptureSource *source,
                                                gboolean                   with_cursor,
                                                PortalCaptureSizeFunc      on_size,
                                                PortalCaptureReadyFunc     on_ready,
                                                PortalCaptureStoppedFunc   on_stopped,
                                                gpointer                   user_data,
                                                GError                   **error);

/**
 * portal_capture_stream_submit:
 * @stream: a #PortalCaptureStream
 * @buffer: a wl_buffer to copy the next frame into, borrowed
 *
 * Asks for one frame into @buffer.  The ready callback fires when it
 * has been written.  One frame is in flight at a time.
 */
void portal_capture_stream_submit (PortalCaptureStream *stream,
                                   gpointer             buffer);

/**
 * portal_capture_stream_wait_ready:
 * @stream: a #PortalCaptureStream
 * @timeout_ms: how long to wait
 *
 * Blocks until the compositor has announced the source's size and shm
 * format, which it does once shortly after the session opens.  A
 * consumer cannot describe its stream before this, and there is
 * nothing useful to do meanwhile.
 *
 * Returns: %TRUE if the constraints arrived
 */
gboolean portal_capture_stream_wait_ready (PortalCaptureStream *stream,
                                           guint                timeout_ms);

/**
 * portal_capture_stream_get_format:
 * @stream: a #PortalCaptureStream
 *
 * Returns: the DRM fourcc the session offered, or 0
 */
guint32 portal_capture_stream_get_format (PortalCaptureStream *stream);

/**
 * portal_capture_stream_get_geometry:
 * @stream: a #PortalCaptureStream
 * @width: (out): the source's width in pixels
 * @height: (out): its height
 */
void portal_capture_stream_get_geometry (PortalCaptureStream *stream,
                                         guint32             *width,
                                         guint32             *height);

/**
 * portal_capture_stream_shm_pool:
 * @self: a #PortalCapture
 * @fd: a memfd holding the buffer
 * @size: its size in bytes
 * @width: buffer width
 * @height: buffer height
 * @stride: bytes per row
 * @format: DRM fourcc, as the size callback reported
 *
 * Wraps a consumer-owned memfd as a wl_buffer the compositor can copy
 * into, so a frame lands in the consumer's memory with no intermediate
 * copy.
 *
 * Returns: (transfer full) (nullable): a wl_buffer, destroyed with
 *          portal_capture_buffer_destroy()
 */
gpointer portal_capture_shm_buffer (PortalCapture *self,
                                    gint           fd,
                                    gsize          size,
                                    guint32        width,
                                    guint32        height,
                                    guint32        stride,
                                    guint32        format);

void portal_capture_buffer_destroy (gpointer buffer);

void portal_capture_stream_free (PortalCaptureStream *stream);

void portal_capture_source_free (PortalCaptureSource *source);

G_END_DECLS

#endif /* PORTAL_CAPTURE_H */
