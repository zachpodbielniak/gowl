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

#ifndef GOWL_BAR_ICON_H
#define GOWL_BAR_ICON_H

#include <glib.h>
#include <cairo.h>

G_BEGIN_DECLS

/*
 * A themed icon name, turned into pixels.
 *
 * This was the tray widget's, and it is here because it was never about
 * trays: a name, a size and the icon theme specification's directory
 * layout.  The menu's application rows want exactly the same answer, and
 * the alternative to sharing it is a second copy of the search, which
 * would drift the first time either one gained a theme.
 *
 * WHY THE LOOKUP IS DONE BY HAND.  Resolving a themed name properly is
 * GtkIconTheme's job, and linking GTK into a compositor to find a PNG is
 * a lot of process for the question.  What is here is the specification's
 * layout walked directly, which is all that is needed: one name at one
 * size, cached for as long as the caller keeps the cache.
 */

/**
 * gowl_bar_icon_cache_new:
 *
 * A cache of rendered icons, keyed by name and size.
 *
 * Returns: (transfer full): the cache; free with g_hash_table_unref()
 */
GHashTable *gowl_bar_icon_cache_new (void);

/**
 * gowl_bar_icon_lookup:
 * @cache: (nullable): a cache from gowl_bar_icon_cache_new()
 * @name: a themed icon name, or an absolute path
 * @theme_path: (nullable): extra directories to search first, `:'
 *   separated --- where an application that ships its own icon puts it
 * @size: the square to draw it in, in pixels
 *
 * The icon at @size, rendered once and kept.
 *
 * A name that resolves to nothing is remembered as nothing, so a missing
 * icon is searched for once rather than on every repaint.
 *
 * Returns: (transfer none) (nullable): the surface, owned by @cache
 */
cairo_surface_t *gowl_bar_icon_lookup (GHashTable  *cache,
                                        const gchar *name,
                                        const gchar *theme_path,
                                        gint         size);

/**
 * gowl_bar_icon_from_file:
 * @path: (nullable): an image file
 * @size: the square to draw it in
 *
 * Returns: (transfer full) (nullable): a new surface, or %NULL
 */
cairo_surface_t *gowl_bar_icon_from_file (const gchar *path, gint size);

/**
 * gowl_bar_icon_scale:
 * @src: (nullable): an image surface
 * @size: the square to fit it into
 *
 * Scales onto a square, keeping the aspect ratio and centring.
 *
 * Returns: (transfer full) (nullable): a new surface, or a reference to
 *   @src when it is already the right size
 */
cairo_surface_t *gowl_bar_icon_scale (cairo_surface_t *src, gint size);

/**
 * gowl_bar_icon_is_mask:
 * @surface: an ARGB32 image surface, or %NULL
 *
 * Is this image a mask rather than a picture?
 *
 * A symbolic icon carries its shape in the alpha channel and nothing
 * in the colour one: every pixel that is there at all is the same flat
 * black (or the same flat white), and whoever draws it is expected to
 * supply the colour.  Qt hands one over as a pixmap like any other, so
 * an application with a symbolic tray icon ships a black glyph --- and
 * a black glyph on a dark bar is an invisible one.
 *
 * The test is on the pixels, not on the icon's name: an application
 * that calls its icon `-symbolic' is telling the truth, but so is one
 * that hands over a monochrome glyph under any other name, and it is
 * the pixels that decide whether it can be seen.  An icon with any
 * colour in it --- even one flat colour, like a red dot --- is a
 * picture and is left alone.
 *
 * Returns: %TRUE if every visible pixel is the same achromatic tone.
 */
gboolean gowl_bar_icon_is_mask (cairo_surface_t *surface);

/**
 * gowl_bar_icon_tint:
 * @surface: an ARGB32 image surface
 * @r: red, 0..1
 * @g: green, 0..1
 * @b: blue, 0..1
 *
 * Paint a mask in one colour, keeping its alpha.  The shape is the
 * alpha channel, so the result is the same glyph in a colour that can
 * be seen against the bar it is drawn on.
 *
 * Returns: (transfer full) (nullable): a new surface; @surface is not
 *   modified.
 */
cairo_surface_t *gowl_bar_icon_tint (cairo_surface_t *surface,
                                     gdouble r, gdouble g, gdouble b);

/**
 * gowl_bar_icon_find_file:
 * @name: a themed icon name, or an absolute path
 * @theme_path: (nullable): extra directories to search first
 * @size: the size wanted, which decides which directory wins
 *
 * Returns: (transfer full) (nullable): the file found, or %NULL
 */
gchar *gowl_bar_icon_find_file (const gchar *name,
                                 const gchar *theme_path,
                                 gint         size);

G_END_DECLS

#endif /* GOWL_BAR_ICON_H */
