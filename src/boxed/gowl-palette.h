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

#ifndef GOWL_PALETTE_H
#define GOWL_PALETTE_H

#include "gowl-types.h"

G_BEGIN_DECLS

#define GOWL_TYPE_PALETTE (gowl_palette_get_type())


GType gowl_palette_get_type (void) G_GNUC_CONST;

/**
 * gowl_palette_new:
 *
 * Creates an empty palette.  A colour spec that names no entry in it
 * resolves to itself, so an empty palette is the same as no palette.
 *
 * Returns: (transfer full): a new #GowlPalette.  Free with
 *   gowl_palette_free().
 */
GowlPalette * gowl_palette_new (void);

/**
 * gowl_palette_new_builtin:
 * @name: (nullable): a built-in palette name, or %NULL for the default
 *
 * Creates a palette pre-filled with one of the built-in colour sets.
 * Known names are `mocha', `macchiato', `frappe' and `latte' (the four
 * Catppuccin flavours), plus `dwm' for the upstream dwm colours gowl
 * used before palettes existed.  An unknown name gives `mocha'.
 *
 * Returns: (transfer full): a new #GowlPalette.
 */
GowlPalette * gowl_palette_new_builtin (const gchar *name);

/**
 * gowl_palette_builtin_names:
 *
 * Returns: (transfer none) (array zero-terminated=1): the built-in
 *   palette names, %NULL-terminated.
 */
const gchar * const * gowl_palette_builtin_names (void);

GowlPalette * gowl_palette_copy (const GowlPalette *self);
void          gowl_palette_free (GowlPalette       *self);

/**
 * gowl_palette_set:
 * @self: a #GowlPalette
 * @name: the entry name, e.g. `accent'
 * @hex: (nullable): a literal `#rrggbb' or `#rrggbbaa', or %NULL to
 *   remove the entry
 *
 * Sets one entry.  @hex is stored as given and is not itself resolved,
 * so an entry cannot alias another entry --- a palette is a flat set of
 * literals, which keeps resolution a single lookup with no cycles to
 * detect.
 */
void gowl_palette_set (GowlPalette *self,
                        const gchar *name,
                        const gchar *hex);

/**
 * gowl_palette_lookup:
 * @self: a #GowlPalette
 * @name: an entry name
 *
 * Returns: (transfer none) (nullable): the entry's hex string, or %NULL.
 */
const gchar * gowl_palette_lookup (const GowlPalette *self,
                                    const gchar       *name);

/**
 * gowl_palette_size:
 * @self: a #GowlPalette
 *
 * Returns: how many entries the palette holds.
 */
guint gowl_palette_size (const GowlPalette *self);

/**
 * gowl_palette_names:
 * @self: a #GowlPalette
 *
 * Returns: (transfer full) (array zero-terminated=1): the entry names in
 *   sorted order, %NULL-terminated.  Free with g_strfreev().
 */
gchar ** gowl_palette_names (const GowlPalette *self);

/**
 * gowl_palette_merge:
 * @self: a #GowlPalette
 * @other: (nullable): entries to copy in
 *
 * Copies every entry of @other over @self, replacing collisions.  This
 * is how a user's `palette:' block layers onto a built-in flavour, and
 * how a palette pushed in at runtime layers onto the config file.
 */
void gowl_palette_merge (GowlPalette       *self,
                          const GowlPalette *other);

/**
 * gowl_palette_resolve:
 * @self: (nullable): a #GowlPalette, or %NULL
 * @spec: (nullable): a colour spec
 *
 * Resolves a colour spec to a literal hex string.
 *
 * A spec is one of:
 *
 * - `#rrggbb' or `#rrggbbaa' --- a literal, returned as given.
 * - `name' --- a palette entry.
 * - `name/aa' --- a palette entry with its alpha replaced by the two
 *   hex digits after the slash, which is how a bar gets a translucent
 *   version of the same background the borders use without the palette
 *   having to carry both.
 *
 * A name with no matching entry resolves to itself.  That is
 * deliberate: an unknown name is far more likely to be a colour format
 * this function does not know about (`red', `rgb(...)') that the
 * consumer does, than a typo worth failing the whole config over.
 *
 * Returns: (transfer full): a newly allocated string, or %NULL if @spec
 *   was %NULL.
 */
gchar * gowl_palette_resolve (const GowlPalette *self,
                               const gchar       *spec);

/**
 * gowl_palette_key_is_color:
 * @key: a configuration key name
 *
 * Whether a module setting under @key holds a colour, and so should be
 * resolved against the palette.
 *
 * Module settings are an untyped string-to-string map --- a module
 * declares no schema --- so the key name is the only thing available to
 * decide this.  The rule is that the key contains `color' or `colour',
 * which is what every colour setting in every shipped module is already
 * called, and it means a new module gets palette support without
 * writing any code.
 *
 * Returns: %TRUE when @key names a colour.
 */
gboolean gowl_palette_key_is_color (const gchar *key);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlPalette, gowl_palette_free)

G_END_DECLS

#endif /* GOWL_PALETTE_H */
