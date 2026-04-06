/*
 * gowl-decor.h — Window decoration for nested Wayland sessions
 *
 * When gowl runs inside another Wayland compositor, this module uses
 * libdecor to add a title bar and window controls.  On tiling WMs
 * (sway, Hyprland) the decorations are server-side and auto-hidden
 * when tiled; on GNOME they are client-side GTK-style header bars.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef GOWL_DECOR_H
#define GOWL_DECOR_H

#include <glib.h>

struct wlr_backend;
struct wlr_output;

typedef struct _GowlCompositor GowlCompositor;
typedef struct _GowlDecor      GowlDecor;

/**
 * gowl_decor_new:
 * @compositor: the owning compositor
 * @wl_backend: the Wayland sub-backend (from wlr_output->backend)
 *
 * Allocates a GowlDecor and binds to the parent compositor's globals
 * (wl_compositor from the parent display).  Does not create the
 * decorated surface yet — call gowl_decor_setup() after the backend
 * has started.
 *
 * Returns: a new GowlDecor, or %NULL on failure.
 */
GowlDecor *gowl_decor_new(GowlCompositor *compositor,
                            struct wlr_backend *wl_backend);

/**
 * gowl_decor_setup:
 * @decor: a GowlDecor from gowl_decor_new()
 *
 * Creates the libdecor-managed surface, decorates it, maps the frame,
 * and creates a wlr_output from the surface.  Call this after
 * wlr_backend_start() returns.
 *
 * Returns: %TRUE on success.
 */
gboolean gowl_decor_setup(GowlDecor *decor);

/**
 * gowl_decor_destroy:
 * @decor: (nullable): a GowlDecor
 *
 * Tears down the libdecor frame, context, and event source.
 */
void gowl_decor_destroy(GowlDecor *decor);

/**
 * gowl_decor_owns_output:
 * @decor: a GowlDecor
 * @output: a wlr_output to test
 *
 * Returns: %TRUE if @output is the decorated output created by this
 *          GowlDecor.
 */
gboolean gowl_decor_owns_output(GowlDecor *decor,
                                 struct wlr_output *output);

#endif /* GOWL_DECOR_H */
