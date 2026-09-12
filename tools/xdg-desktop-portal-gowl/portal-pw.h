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

#ifndef PORTAL_PW_H
#define PORTAL_PW_H

#include <glib.h>
#include <gio/gio.h>

#include "portal-capture.h"

G_BEGIN_DECLS

/*
 * PortalPw -- the PipeWire half of the ScreenCast backend.
 *
 * One PortalPwStream marries a PortalCaptureStream to a pw_stream: the
 * compositor writes frames into buffers PipeWire allocated, and the
 * consuming application reads them out of the same memory.  The memfd
 * PipeWire hands out is wrapped as a wl_buffer, so a frame is copied
 * once, by the compositor, straight into the place the application
 * will read it.
 */
typedef struct _PortalPw PortalPw;
typedef struct _PortalPwStream PortalPwStream;

PortalPw *portal_pw_new  (GError **error);
void      portal_pw_free (PortalPw *self);

/**
 * portal_pw_stream_new:
 * @self: a #PortalPw
 * @capture: the Wayland capture context
 * @source: the chosen source, borrowed for the call
 * @with_cursor: draw the pointer into the frames
 * @error: return location for an error
 *
 * Opens the capture session and the PipeWire stream it feeds, in that
 * order: the stream cannot announce a format until the compositor has
 * said what size and pixel layout the source has, so this waits for
 * the session's first constraints before connecting.
 *
 * Returns: (transfer full): the stream, or %NULL
 */
PortalPwStream *portal_pw_stream_new (PortalPw                  *self,
                                      PortalCapture             *capture,
                                      const PortalCaptureSource *source,
                                      gboolean                   with_cursor,
                                      GError                   **error);

/**
 * portal_pw_stream_node_id:
 * @stream: a #PortalPwStream
 *
 * The PipeWire node the application should connect to.  Valid once the
 * stream has reached at least the CONNECTING state, which it has by the
 * time portal_pw_stream_new() returns.
 *
 * Returns: the node id, or %SPA_ID_INVALID
 */
guint32 portal_pw_stream_node_id (PortalPwStream *stream);

/**
 * portal_pw_stream_get_size:
 * @stream: a #PortalPwStream
 * @width: (out): the stream's width in pixels
 * @height: (out): its height
 *
 * What the compositor said the source measures, which is what the
 * cast's consumer should expect its frames to be.
 */
void portal_pw_stream_get_size (PortalPwStream *stream,
                                guint32        *width,
                                guint32        *height);

void portal_pw_stream_free (PortalPwStream *stream);

G_END_DECLS

#endif /* PORTAL_PW_H */
