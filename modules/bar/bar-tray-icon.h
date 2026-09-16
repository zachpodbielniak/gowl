/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef GOWL_BAR_TRAY_ICON_H
#define GOWL_BAR_TRAY_ICON_H

#include <cairo.h>

#include "barkit/gowl-bar-icon.h"
#include <glib.h>

#include "tray/gowl-tray.h"

G_BEGIN_DECLS

/**
 * BarTrayIconCache:
 *
 * Two tables, because there are two questions.  @items is keyed on an
 * item's identity AND its serial, so an application that changes its
 * icon --- which is the entire point of a tray icon for something that
 * syncs or connects --- is not served its old one for ever.  @names is
 * barkit's, keyed on the name alone, and is shared across every item
 * because a name resolves to the same file whoever asked.
 */
typedef struct {
	GHashTable *items;
	GHashTable *names;
} BarTrayIconCache;

/**
 * bar_tray_icon_cache_new:
 *
 * Returns: (transfer full): a cache of rendered tray icons
 */
BarTrayIconCache *bar_tray_icon_cache_new (void);

/**
 * bar_tray_icon_cache_free:
 * @cache: (transfer full) (nullable): a cache
 */
void bar_tray_icon_cache_free (BarTrayIconCache *cache);

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
 * A monochrome icon --- a symbolic one, which carries its shape in the
 * alpha channel and hands over a flat black glyph --- is painted in
 * @fg instead of its own colour.  Black on a dark bar is a slot that
 * looks empty, and the application looks like it never registered at
 * all.  An icon with any colour of its own keeps it.
 *
 * Returns: (transfer none) (nullable): the surface, owned by @cache, or
 *   %NULL when nothing could be found --- which the caller draws as a
 *   lettered placeholder rather than as a gap.
 */
cairo_surface_t *bar_tray_icon_for (BarTrayIconCache   *cache,
                                    const GowlTrayItem *item,
                                    gint                size,
                                    const gdouble      *fg);

G_END_DECLS

#endif /* GOWL_BAR_TRAY_ICON_H */
