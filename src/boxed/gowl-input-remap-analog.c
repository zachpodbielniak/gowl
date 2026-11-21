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
 * gowl-input-remap-analog.c - The arithmetic of analog remapping.
 *
 * See the header.  Everything here is a pure function of its arguments;
 * the state (whether a direction is currently pressed, how long a stick
 * has been held) belongs to the caller.
 */

#include "gowl-input-remap-analog.h"

#include <math.h>

/* Acceleration never makes the pointer faster than this multiple of its
 * configured top speed, however long the stick is held. */
#define GOWL_REMAP_MAX_ACCEL_FACTOR (4.0)

/**
 * gowl_remap_analog_normalize: (skip)
 * @abs: the axis calibration
 * @raw: a raw value from the device
 * @centred: %TRUE for a stick or hat (rests in the middle, result in
 *   [-1, 1]); %FALSE for a trigger (rests at @abs->min, result in
 *   [0, 1])
 *
 * Maps a raw reading onto the unit range.  A centred axis is split at
 * the midpoint of its range, and each half is scaled on its own, so an
 * asymmetric range (-32768..32767, or 0..255 resting at 128) still
 * reaches exactly -1 and +1 at its ends.  Out-of-range readings clamp.
 *
 * Returns: the normalised value
 */
gdouble
gowl_remap_analog_normalize(
	const GowlRemapAbsInfo *abs,
	gint32                  raw,
	gboolean                centred
){
	gdouble lo;
	gdouble hi;
	gdouble v;
	gdouble mid;

	if (abs == NULL || abs->max <= abs->min)
		return 0.0;
	lo = (gdouble)abs->min;
	hi = (gdouble)abs->max;
	v = (gdouble)raw;
	if (v < lo)
		v = lo;
	if (v > hi)
		v = hi;

	if (!centred)
		return (v - lo) / (hi - lo);

	mid = (lo + hi) / 2.0;
	if (v >= mid)
		return hi > mid ? (v - mid) / (hi - mid) : 0.0;
	return mid > lo ? (v - mid) / (mid - lo) : 0.0;
}

/**
 * gowl_remap_analog_default_deadzone: (skip)
 * @abs: the axis calibration
 * @centred: whether the axis is centred (see
 *   gowl_remap_analog_normalize())
 *
 * The deadzone a rule gets when it does not set one: the kernel's
 * `flat' as a fraction of the axis's half range (full range for a
 * trigger), floored at 0.05 because many drivers report a flat of 0
 * for sticks that still drift a few percent.
 *
 * Returns: the deadzone, in [0.05, 0.95]
 */
gdouble
gowl_remap_analog_default_deadzone(
	const GowlRemapAbsInfo *abs,
	gboolean                centred
){
	gdouble span;
	gdouble dz;

	if (abs == NULL || abs->max <= abs->min)
		return 0.05;
	span = (gdouble)abs->max - (gdouble)abs->min;
	if (centred)
		span /= 2.0;
	dz = span > 0.0 ? (gdouble)abs->flat / span : 0.0;
	if (dz < 0.05)
		dz = 0.05;
	if (dz > 0.95)
		dz = 0.95;
	return dz;
}

/**
 * gowl_remap_analog_deadzone: (skip)
 * @value: a normalised value, [-1, 1]
 * @deadzone: the deadzone half-width, [0, 1)
 *
 * Inside the deadzone the value is 0; outside, the remaining travel is
 * rescaled so the output still runs continuously from 0 to 1 -- a stick
 * pushed just past the deadzone moves the pointer slowly, not at once at
 * the deadzone's speed.
 *
 * Returns: the value with the deadzone applied
 */
gdouble
gowl_remap_analog_deadzone(
	gdouble value,
	gdouble deadzone
){
	gdouble mag;

	if (deadzone < 0.0)
		deadzone = 0.0;
	if (deadzone >= 1.0)
		return 0.0;
	mag = fabs(value);
	if (mag <= deadzone)
		return 0.0;
	if (mag > 1.0)
		mag = 1.0;
	mag = (mag - deadzone) / (1.0 - deadzone);
	return value < 0.0 ? -mag : mag;
}

/**
 * gowl_remap_analog_hysteresis: (skip)
 * @was_pressed: whether the direction is currently pressed
 * @amount: how far the axis is pushed IN THIS DIRECTION (negative or 0
 *   when it is pushed the other way or resting)
 * @press: the threshold that presses it
 * @release: the threshold below which it releases; lower than @press
 *
 * A stick direction used as a key: crossing @press presses it once, and
 * it stays pressed until the stick comes back below @release.  The gap
 * between the two is what stops a stick resting near the threshold from
 * chattering a key on and off.
 *
 * Returns: whether the direction is pressed after @amount
 */
gboolean
gowl_remap_analog_hysteresis(
	gboolean was_pressed,
	gdouble  amount,
	gdouble  press,
	gdouble  release
){
	if (release > press)
		release = press;
	if (was_pressed)
		return amount >= release && amount > 0.0;
	return amount >= press && amount > 0.0;
}

/**
 * gowl_remap_analog_combine: (skip)
 * @pos_x: how far keys hold the stick right, [0, 1]
 * @neg_x: how far keys hold it left, [0, 1]
 * @pos_y: how far keys hold it down (evdev's +Y), [0, 1]
 * @neg_y: how far keys hold it up, [0, 1]
 * @out_x: (out): the stick's x, [-1, 1]
 * @out_y: (out): the stick's y, [-1, 1]
 *
 * Keys on a virtual stick: opposite directions cancel (both held is
 * centre), and a diagonal is normalised onto the unit circle, so W+D is
 * up-right at full deflection rather than a corner a real stick cannot
 * reach -- a game reading the magnitude would otherwise run 41% faster
 * diagonally.
 */
void
gowl_remap_analog_combine(
	gdouble  pos_x,
	gdouble  neg_x,
	gdouble  pos_y,
	gdouble  neg_y,
	gdouble *out_x,
	gdouble *out_y
){
	gdouble x;
	gdouble y;
	gdouble mag;

	x = CLAMP(pos_x, 0.0, 1.0) - CLAMP(neg_x, 0.0, 1.0);
	y = CLAMP(pos_y, 0.0, 1.0) - CLAMP(neg_y, 0.0, 1.0);
	mag = sqrt(x * x + y * y);
	if (mag > 1.0) {
		x /= mag;
		y /= mag;
	}
	if (out_x != NULL)
		*out_x = x;
	if (out_y != NULL)
		*out_y = y;
}

/**
 * gowl_remap_analog_curve: (skip)
 * @value: a value in [-1, 1], deadzone already applied
 * @exponent: the curve; 1 is linear, 2 (the default) gives fine control
 *   near the centre and full speed at the edge
 *
 * Returns: sign(@value) * |@value|^@exponent
 */
gdouble
gowl_remap_analog_curve(
	gdouble value,
	gdouble exponent
){
	gdouble mag;

	if (exponent <= 0.0)
		exponent = 1.0;
	mag = pow(fabs(value) > 1.0 ? 1.0 : fabs(value), exponent);
	return value < 0.0 ? -mag : mag;
}

/**
 * gowl_remap_analog_speed: (skip)
 * @value: the axis value, deadzone applied, [-1, 1]
 * @exponent: the speed curve (see gowl_remap_analog_curve())
 * @max_speed: speed at full deflection, units per second
 * @acceleration: extra speed per second the stick has been held outside
 *   its deadzone, as a fraction of @max_speed (0 = none)
 * @held_secs: how long the stick has been held outside its deadzone
 *
 * The pointer (or scroll) speed one axis asks for.  Acceleration lets a
 * stick cross a large screen without making small adjustments twitchy;
 * it is capped at four times @max_speed.
 *
 * Returns: the signed speed, units per second
 */
gdouble
gowl_remap_analog_speed(
	gdouble value,
	gdouble exponent,
	gdouble max_speed,
	gdouble acceleration,
	gdouble held_secs
){
	gdouble factor;

	factor = 1.0;
	if (acceleration > 0.0 && held_secs > 0.0)
		factor += acceleration * held_secs;
	if (factor > GOWL_REMAP_MAX_ACCEL_FACTOR)
		factor = GOWL_REMAP_MAX_ACCEL_FACTOR;
	return gowl_remap_analog_curve(value, exponent) * max_speed * factor;
}

/**
 * gowl_remap_analog_to_raw: (skip)
 * @value: [-1, 1] for a centred axis whose @min is negative or whose
 *   range has a middle; [0, 1] maps onto the whole range when @min is 0
 * @min: the output axis's minimum
 * @max: the output axis's maximum
 *
 * The inverse of normalising, for a virtual pad's output axis: -1 is
 * @min, 0 the middle, +1 @max.  For a trigger (@min 0) a value in [0, 1]
 * covers the whole range and a negative value is 0.
 *
 * Returns: the raw value to report
 */
gint32
gowl_remap_analog_to_raw(
	gdouble value,
	gint32  min,
	gint32  max
){
	gdouble lo;
	gdouble hi;
	gdouble mid;
	gdouble v;

	lo = (gdouble)min;
	hi = (gdouble)max;
	if (hi <= lo)
		return min;
	v = CLAMP(value, -1.0, 1.0);
	if (min == 0) {
		if (v < 0.0)
			v = 0.0;
		return (gint32)lround(lo + v * (hi - lo));
	}
	mid = (lo + hi) / 2.0;
	if (v >= 0.0)
		return (gint32)lround(mid + v * (hi - mid));
	return (gint32)lround(mid + v * (mid - lo));
}
