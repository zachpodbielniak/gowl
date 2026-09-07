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

#ifndef GOWL_EASING_H
#define GOWL_EASING_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * gowl_easing_eval:
 * @name: (nullable): a curve name, or %NULL for the default
 * @t: linear progress, 0.0 to 1.0
 *
 * Evaluates a named easing curve.
 *
 * The curves are cubic Béziers with the endpoints fixed at (0,0) and
 * (1,1), which is what both CSS and Hyprland use, so the two control
 * points are the whole definition and a Hyprland `bezier' line
 * translates directly.  Known names: `linear', `ease-out-quint',
 * `ease-in-out-cubic', `almost-linear', `quick', `ease-out-expo',
 * `ease-out-back' and `spring'.  An unknown name falls back to
 * `ease-out-expo'.
 *
 * The last two OVERSHOOT --- they return values above 1.0 partway
 * through.  That is deliberate; see the note in gowl-easing.c for when
 * it is and is not wanted.
 *
 * This lives in the core rather than in one module because more than
 * one presentation module needs it: the `animation' module eases window
 * geometry with it and the `cube' module eases the tag rotation with
 * it, and they must agree on what `ease-out-expo' means.
 *
 * Returns: eased progress, 0.0 to 1.0 (or beyond, for overshoot curves).
 */
gdouble gowl_easing_eval (const gchar *name, gdouble t);

/**
 * gowl_easing_name_is_known:
 * @name: (nullable): a curve name
 *
 * Returns: %TRUE when @name matches a curve, so a caller can warn about
 *   a typo in a config file rather than silently easing differently.
 */
gboolean gowl_easing_name_is_known (const gchar *name);

G_END_DECLS

#endif /* GOWL_EASING_H */
