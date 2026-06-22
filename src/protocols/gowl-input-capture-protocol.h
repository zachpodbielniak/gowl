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

#ifndef GOWL_INPUT_CAPTURE_PROTOCOL_H
#define GOWL_INPUT_CAPTURE_PROTOCOL_H

#include <glib-object.h>

struct wl_display;

G_BEGIN_DECLS

/**
 * GowlInputCaptureProtocol:
 *
 * Opaque manager that exposes the gowl-private
 * `zgowl_input_capture_manager_v1` Wayland global and bridges it to the
 * compositor's #GowlInputCapture state machine (for capture/egress) and
 * the compositor's input-injection API (for RemoteDesktop/ingress).  One
 * is created per compositor at startup by the embedder
 * (cmacs --gowl) and by standalone gowl's main().
 */
typedef struct _GowlInputCaptureProtocol GowlInputCaptureProtocol;

GowlInputCaptureProtocol *
gowl_input_capture_protocol_register   (gpointer            compositor,
                                        struct wl_display  *display);

void
gowl_input_capture_protocol_unregister (GowlInputCaptureProtocol *self);

G_END_DECLS

#endif /* GOWL_INPUT_CAPTURE_PROTOCOL_H */
