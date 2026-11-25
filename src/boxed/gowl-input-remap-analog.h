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
 * gowl-input-remap-analog.h - The arithmetic of analog remapping.
 *
 * Pure functions, no wlroots and no evdev headers, so every piece the
 * gamepad support depends on -- normalising a raw axis, the deadzone,
 * the press/release hysteresis that turns a stick direction into one
 * press, combining two held keys into one stick position, the pointer
 * speed curve -- is unit-tested as plain C (tests/test-input-remap-
 * analog.c).  The core (reading controllers) and the inputremap module
 * (driving a virtual pad) both call these, so they agree by
 * construction.
 *
 * Not part of the introspected API: every function is (skip).
 */

#ifndef GOWL_INPUT_REMAP_ANALOG_H
#define GOWL_INPUT_REMAP_ANALOG_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GowlRemapAbsInfo: (skip)
 * @min: the smallest raw value the axis reports
 * @max: the largest raw value the axis reports
 * @flat: the kernel's "flat" (resting noise) half-width, raw units
 * @fuzz: the kernel's noise filter width, raw units
 *
 * The calibration of one absolute axis, as EVIOCGABS reports it.
 */
typedef struct {
	gint32 min;
	gint32 max;
	gint32 flat;
	gint32 fuzz;
} GowlRemapAbsInfo;

gdouble  gowl_remap_analog_normalize        (const GowlRemapAbsInfo *abs,
                                             gint32                  raw,
                                             gboolean                centred);
gdouble  gowl_remap_analog_default_deadzone (const GowlRemapAbsInfo *abs,
                                             gboolean                centred);
gdouble  gowl_remap_analog_deadzone         (gdouble                 value,
                                             gdouble                 deadzone);
gboolean gowl_remap_analog_hysteresis       (gboolean                was_pressed,
                                             gdouble                 amount,
                                             gdouble                 press,
                                             gdouble                 release);
void     gowl_remap_analog_slant            (gdouble                 x,
                                             gdouble                 y,
                                             gdouble                 degrees,
                                             gdouble                *out_x,
                                             gdouble                *out_y);
void     gowl_remap_analog_gate             (gdouble                 x,
                                             gdouble                 y,
                                             gdouble                 gate,
                                             gdouble                *out_x,
                                             gdouble                *out_y);
void     gowl_remap_analog_combine          (gdouble                 pos_x,
                                             gdouble                 neg_x,
                                             gdouble                 pos_y,
                                             gdouble                 neg_y,
                                             gdouble                *out_x,
                                             gdouble                *out_y);
gdouble  gowl_remap_analog_curve            (gdouble                 value,
                                             gdouble                 exponent);
gdouble  gowl_remap_analog_speed            (gdouble                 value,
                                             gdouble                 exponent,
                                             gdouble                 max_speed,
                                             gdouble                 acceleration,
                                             gdouble                 held_secs);
gint32   gowl_remap_analog_to_raw           (gdouble                 value,
                                             gint32                  min,
                                             gint32                  max);

G_END_DECLS

#endif /* GOWL_INPUT_REMAP_ANALOG_H */
