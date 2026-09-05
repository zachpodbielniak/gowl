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

#ifndef GOWL_TABLET_H
#define GOWL_TABLET_H

#include <glib.h>
#include <wayland-server-core.h>

G_BEGIN_DECLS

typedef struct _GowlCompositor GowlCompositor;

struct wlr_input_device;
struct wlr_surface;
struct wlr_tablet_manager_v2;
struct wlr_tablet_v2_tablet;
struct wlr_tablet_v2_tablet_tool;

/**
 * GowlTablet:
 *
 * One physical tablet (a Wacom, a Huion, ...).  Holds the protocol
 * object clients bind to, and lives as long as the device does.
 */
typedef struct {
	GowlCompositor              *compositor;
	struct wlr_tablet_v2_tablet *tablet_v2;
	struct wlr_tablet           *wlr_tablet;

	struct wl_listener axis;
	struct wl_listener proximity;
	struct wl_listener tip;
	struct wl_listener button;
	struct wl_listener destroy;

	struct wl_list link;        /* GowlCompositor.tablets */
} GowlTablet;

/**
 * GowlTabletTool:
 *
 * One *tool* -- a pen, its eraser end, a puck.  A tablet may see
 * several over its life and libinput reports each as its own device,
 * which is why tools are tracked separately from tablets rather than
 * as a field on one.
 */
typedef struct {
	GowlCompositor                   *compositor;
	struct wlr_tablet_v2_tablet_tool *tool_v2;
	struct wlr_tablet_tool           *wlr_tool;

	/* TRUE while the tip is pressed, so a tool that goes out of
	 * proximity mid-stroke can be released rather than leaving the
	 * client believing the pen is still down. */
	gboolean tip_down;

	struct wl_listener destroy;
	struct wl_list     link;    /* GowlCompositor.tablet_tools */
} GowlTabletTool;

/**
 * GowlTabletPad:
 *
 * The button/ring/strip cluster on the tablet body.  Presented to
 * clients; gowl itself binds nothing to it.
 */
typedef struct {
	GowlCompositor                  *compositor;
	struct wlr_tablet_v2_tablet_pad *pad_v2;

	struct wl_listener destroy;
	struct wl_list     link;    /* GowlCompositor.tablet_pads */
} GowlTabletPad;

/**
 * gowl_tablet_manager_init:
 * @self: a #GowlCompositor
 *
 * Creates the tablet-v2 global and initialises the device lists.
 * Call once, during compositor startup, before any device arrives.
 */
void gowl_tablet_manager_init (GowlCompositor *self);

/**
 * gowl_tablet_new_device:
 * @self: a #GowlCompositor
 * @device: the new #wlr_input_device
 *
 * Adopts @device when it is a tablet or a tablet pad.  A no-op for
 * every other device type, so the caller can hand it everything.
 *
 * Returns: %TRUE when the device was adopted.
 */
gboolean gowl_tablet_new_device (GowlCompositor          *self,
                                  struct wlr_input_device *device);

/**
 * gowl_tablet_finish:
 * @self: a #GowlCompositor
 *
 * Tears down every tablet, tool and pad.  Called from compositor
 * shutdown.
 */
void gowl_tablet_finish (GowlCompositor *self);

/**
 * gowl_tablet_has_devices:
 * @self: a #GowlCompositor
 *
 * Returns: %TRUE when at least one tablet is attached.
 */
gboolean gowl_tablet_has_devices (GowlCompositor *self);

G_END_DECLS

#endif /* GOWL_TABLET_H */
