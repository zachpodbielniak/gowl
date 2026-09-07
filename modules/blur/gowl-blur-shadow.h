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

/*
 * gowl-blur-shadow.h -- drawing a soft shadow, analytically.
 *
 * A drop shadow is a blurred rectangle, and the obvious way to make one
 * is to draw a rectangle and blur it.  This does not: it evaluates what
 * the blurred rectangle WOULD look like, per pixel, in closed form.  That
 * is not cleverness for its own sake --- it means no blur passes, no
 * intermediate buffers, and no GPU involvement at all for something that
 * only changes when a window is resized.
 *
 * Pure arithmetic, so the falloff and the premultiplication can be
 * checked by a test rather than by squinting at a screen.
 */

#ifndef GOWL_BLUR_SHADOW_H
#define GOWL_BLUR_SHADOW_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * gowl_blur_shadow_alpha:
 * @x: pixel centre x, relative to the shadow image's top left
 * @y: pixel centre y
 * @rect_x: the casting rectangle's left edge in the same space
 * @rect_y: its top edge
 * @rect_w: its width
 * @rect_h: its height
 * @radius: blur radius in pixels; 0 gives a hard edge
 * @corner: corner rounding of the casting rectangle, in pixels
 *
 * Coverage at one pixel, 0.0 to 1.0.
 *
 * The falloff is a smoothstep across twice the radius, centred on the
 * rectangle's edge.  A true gaussian would need an error function; a
 * smoothstep is within a few per cent of one over the range that matters
 * and is the difference between a shadow that costs a multiply and a
 * shadow that costs a library.
 *
 * Returns: coverage, 0.0 to 1.0.
 */
gdouble gowl_blur_shadow_alpha (gdouble x, gdouble y,
                                gdouble rect_x, gdouble rect_y,
                                gdouble rect_w, gdouble rect_h,
                                gdouble radius, gdouble corner);

/**
 * gowl_blur_shadow_render:
 * @width: image width
 * @height: image height
 * @rect_x: the casting rectangle's left edge within the image
 * @rect_y: its top edge
 * @rect_w: its width
 * @rect_h: its height
 * @radius: blur radius in pixels
 * @corner: corner rounding in pixels
 * @opacity: peak opacity, 0.0 to 1.0
 * @rgb: (array fixed-size=3): the shadow's colour, 0.0 to 1.0 per channel
 *
 * Renders a whole shadow as ARGB8888, PREMULTIPLIED --- which is what
 * wlroots composites and what a scene buffer must contain.  Getting that
 * wrong does not produce an error, it produces a shadow with a bright
 * halo where the alpha is low.
 *
 * Returns: (transfer full): the pixels, @width * @height * 4 bytes.
 */
guint8 *gowl_blur_shadow_render (gint          width,
                                 gint          height,
                                 gdouble       rect_x,
                                 gdouble       rect_y,
                                 gdouble       rect_w,
                                 gdouble       rect_h,
                                 gdouble       radius,
                                 gdouble       corner,
                                 gdouble       opacity,
                                 const gdouble *rgb);

G_END_DECLS

#endif /* GOWL_BLUR_SHADOW_H */
