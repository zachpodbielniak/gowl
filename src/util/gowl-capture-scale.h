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

#ifndef GOWL_CAPTURE_SCALE_H
#define GOWL_CAPTURE_SCALE_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * Pure integer geometry for screen capture on scaled outputs.  No
 * wlroots or cairo types, so the maths can be unit-tested directly
 * (see tests/test-capture-scale.c).
 *
 * TWO COORDINATE SPACES MEET HERE, AND CONFUSING THEM IS THE BUG THIS
 * FILE EXISTS TO PREVENT.
 *
 *   Layout coordinates are what the rest of the compositor speaks: the
 *   cursor, a client's geometry, a monitor's position in the output
 *   layout, the rubber band drawn during an area selection.  They are
 *   logical, so a 3840x2160 panel at scale 2 is 1920x1080 of them.
 *
 *   Device pixels are what a captured image is made of.  Rendering an
 *   output produces its framebuffer, which is the panel's real
 *   resolution --- 3840x2160 for that same monitor.
 *
 * A selection is made in the first and cropped out of the second, so
 * every capture of less than a whole output has to cross between them.
 * Skipping the conversion does not fail loudly; it quietly photographs
 * the wrong part of the screen, at scale 2 the top-left quarter of what
 * was asked for.
 */

/**
 * gowl_capture_scale_factor:
 * @layout_w: the monitor's width in layout coordinates
 * @layout_h: the monitor's height in layout coordinates
 * @image_w: the captured image's width in device pixels
 * @image_h: the captured image's height in device pixels
 *
 * The effective scale of a captured output: how many device pixels
 * one layout unit is worth.  Measured from the image rather than read
 * from the output's scale property, so fractional scaling --- where
 * the layout size is a rounded quotient and not an exact one --- maps
 * back exactly onto the pixels that were actually rendered.
 *
 * Returns: the scale, or 1.0 if any dimension is non-positive.
 */
gdouble gowl_capture_scale_factor(gint layout_w, gint layout_h,
                                  gint image_w, gint image_h);

/**
 * gowl_capture_scale_crop:
 * @mon_x: the monitor's x position in layout coordinates
 * @mon_y: the monitor's y position in layout coordinates
 * @mon_w: the monitor's width in layout coordinates
 * @mon_h: the monitor's height in layout coordinates
 * @image_w: the captured image's width in device pixels
 * @image_h: the captured image's height in device pixels
 * @rx: the wanted region's x, in layout coordinates
 * @ry: the wanted region's y, in layout coordinates
 * @rw: the wanted region's width, in layout coordinates
 * @rh: the wanted region's height, in layout coordinates
 * @crop_x: (out): the crop's x within the image, in device pixels
 * @crop_y: (out): the crop's y within the image, in device pixels
 * @crop_w: (out): the crop's width, in device pixels
 * @crop_h: (out): the crop's height, in device pixels
 *
 * Maps a region given in layout coordinates onto the device pixels of
 * an image captured from one output.  The region is made relative to
 * the monitor's own origin, clamped to the monitor, and then scaled.
 *
 * The edges are taken outwards --- the near one floored, the far one
 * ceiled --- so a selection never loses the pixel row it was drawn
 * around.  The result is always inside the image.
 *
 * Returns: %TRUE if anything is left to crop, %FALSE if the region
 *   misses the monitor entirely or any input dimension is unusable.
 */
gboolean gowl_capture_scale_crop(gint mon_x, gint mon_y,
                                 gint mon_w, gint mon_h,
                                 gint image_w, gint image_h,
                                 gint rx, gint ry, gint rw, gint rh,
                                 gint *crop_x, gint *crop_y,
                                 gint *crop_w, gint *crop_h);

/**
 * gowl_capture_scale_place:
 * @mon_x: the monitor's x position in layout coordinates
 * @mon_y: the monitor's y position in layout coordinates
 * @mon_w: the monitor's width in layout coordinates
 * @mon_h: the monitor's height in layout coordinates
 * @origin_x: the x of the whole layout's bounding box
 * @origin_y: the y of the whole layout's bounding box
 * @scale: the canvas's device pixels per layout unit
 * @dest_x: (out): where this monitor starts on the canvas
 * @dest_y: (out): where this monitor starts on the canvas
 * @dest_w: (out): how wide this monitor is on the canvas
 * @dest_h: (out): how tall this monitor is on the canvas
 *
 * Where one monitor's image belongs on a canvas that holds every
 * monitor.  The canvas is in device pixels at @scale, so a monitor
 * whose own scale is @scale lands at its image's exact size and needs
 * no resampling --- the case every single-scale desktop is in.
 *
 * Returns: %TRUE if the monitor occupies any of the canvas.
 */
gboolean gowl_capture_scale_place(gint mon_x, gint mon_y,
                                  gint mon_w, gint mon_h,
                                  gint origin_x, gint origin_y,
                                  gdouble scale,
                                  gint *dest_x, gint *dest_y,
                                  gint *dest_w, gint *dest_h);

G_END_DECLS

#endif /* GOWL_CAPTURE_SCALE_H */
