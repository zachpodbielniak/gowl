/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef GOWL_MODULE_LAYOUT_AXIS_H
#define GOWL_MODULE_LAYOUT_AXIS_H

#include "core/gowl-monitor.h"
#include "core/gowl-compositor.h"
#include <wlr/util/box.h>

/* Read the full logical output each time: insets must not turn a square
 * monitor into a portrait one.  Each monitor chooses independently. */
static inline gboolean
gowl_layout_is_portrait(GowlMonitor *monitor)
{
	gint width, height;
	gowl_monitor_get_geometry(monitor, NULL, NULL, &width, &height);
	return width > 0 && height > width;
}

/* Transposing twice restores both dimensions and layout-relative origins. */
static inline struct wlr_box
gowl_layout_axis_box(struct wlr_box box, gboolean portrait)
{
	if (portrait)
		return (struct wlr_box){box.y, box.x, box.height, box.width};
	return box;
}

static inline void
gowl_layout_place_oriented(GowlCompositor *compositor, GowlClient *client,
                            gboolean portrait, gint x, gint y,
                            gint width, gint height)
{
	struct wlr_box box = gowl_layout_axis_box(
		(struct wlr_box){x, y, width, height}, portrait);
	gowl_compositor_place_client(compositor, client,
		box.x, box.y, box.width, box.height);
}
#endif
