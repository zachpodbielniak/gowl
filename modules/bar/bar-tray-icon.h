/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef GOWL_BAR_TRAY_ICON_H
#define GOWL_BAR_TRAY_ICON_H

#include <cairo.h>
#include <glib.h>

#include "tray/gowl-tray.h"

G_BEGIN_DECLS

/**
 * bar_tray_icon_cache_new:
 *
 * Returns: (transfer full): a cache of rendered tray icons
 */
GHashTable *bar_tray_icon_cache_new (void);

/**
 * bar_tray_icon_for:
 * @cache: a cache from bar_tray_icon_cache_new()
 * @item: the item to draw
 * @size: the square to draw it in, in pixels
 *
 * The item's icon at @size, rendered once and kept.
 *
 * Tries, in the order applications actually manage to be useful in: the
 * raw pixmap the item handed over, then its themed name looked up in its
 * own `IconThemePath', then that name in the icon theme, then the
 * item's `Id' as a name --- which is a guess, and a good one, because an
 * application's tray icon is very often installed under its own name.
 *
 * Returns: (transfer none) (nullable): the surface, owned by @cache, or
 *   %NULL when nothing could be found --- which the caller draws as a
 *   lettered placeholder rather than as a gap.
 */
cairo_surface_t *bar_tray_icon_for (GHashTable         *cache,
                                    const GowlTrayItem *item,
                                    gint                size);

G_END_DECLS

#endif /* GOWL_BAR_TRAY_ICON_H */
