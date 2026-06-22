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

#ifndef GOWL_INPUT_CAPTURE_H
#define GOWL_INPUT_CAPTURE_H

#include <glib-object.h>

#include "boxed/gowl-input-zone.h"
#include "boxed/gowl-input-barrier.h"

G_BEGIN_DECLS

/**
 * GowlInputCaptureCapability:
 * @GOWL_INPUT_CAPTURE_CAP_NONE: nothing capturable.
 * @GOWL_INPUT_CAPTURE_CAP_KEYBOARD: keyboard key + modifier capture.
 * @GOWL_INPUT_CAPTURE_CAP_POINTER: pointer motion + button + axis capture.
 *
 * Bitmask of input kinds the capture machine can divert.  The numeric
 * values match the InputCapture portal capability flags (1 = KEYBOARD,
 * 2 = POINTER); TOUCHSCREEN (4) is intentionally not supported.
 */
typedef enum /*< flags >*/ {
	GOWL_INPUT_CAPTURE_CAP_NONE     = 0,
	GOWL_INPUT_CAPTURE_CAP_KEYBOARD = 1 << 0,
	GOWL_INPUT_CAPTURE_CAP_POINTER  = 1 << 1
} GowlInputCaptureCapability;

/**
 * GowlInputEventType:
 * @GOWL_INPUT_EVENT_REL_MOTION: relative pointer motion (dx, dy).
 * @GOWL_INPUT_EVENT_BUTTON: pointer button (code in @button, pressed in
 *   @state).
 * @GOWL_INPUT_EVENT_AXIS: scroll (axis 0 = vertical, 1 = horizontal, in
 *   @axis; value in @value; discrete steps in @discrete).
 * @GOWL_INPUT_EVENT_KEY: keyboard key (keycode in @keycode, pressed in
 *   @state).
 * @GOWL_INPUT_EVENT_MODIFIERS: modifier state changed (depressed/latched/
 *   locked/group in the four modifier fields).
 *
 * Discriminator for #GowlInputEvent.
 */
typedef enum {
	GOWL_INPUT_EVENT_REL_MOTION = 0,
	GOWL_INPUT_EVENT_BUTTON,
	GOWL_INPUT_EVENT_AXIS,
	GOWL_INPUT_EVENT_KEY,
	GOWL_INPUT_EVENT_MODIFIERS
} GowlInputEventType;

/**
 * GowlInputEvent:
 * @type: which kind of event this is.
 * @time_msec: event timestamp in milliseconds (compositor clock).
 * @dx: relative motion delta x (REL_MOTION).
 * @dy: relative motion delta y (REL_MOTION).
 * @button: button code (BUTTON).
 * @axis: scroll axis, 0 = vertical / 1 = horizontal (AXIS).
 * @value: scroll value (AXIS).
 * @discrete: discrete scroll steps, 0 if continuous (AXIS).
 * @keycode: key code (KEY).
 * @state: 1 = pressed/down, 0 = released/up (BUTTON, KEY).
 * @mods_depressed: depressed modifiers (MODIFIERS).
 * @mods_latched: latched modifiers (MODIFIERS).
 * @mods_locked: locked modifiers (MODIFIERS).
 * @mods_group: effective layout group (MODIFIERS).
 *
 * A single diverted input event handed to the #GowlInputSinkFunc.  This
 * is a plain POD value (no allocation) so it can be copied across a
 * thread boundary lock-free by downstream consumers.
 */
typedef struct _GowlInputEvent GowlInputEvent;

struct _GowlInputEvent {
	GowlInputEventType type;
	guint32            time_msec;

	gdouble            dx;
	gdouble            dy;

	guint32            button;

	guint32            axis;
	gdouble            value;
	gint32             discrete;

	guint32            keycode;
	guint32            state;

	guint32            mods_depressed;
	guint32            mods_latched;
	guint32            mods_locked;
	guint32            mods_group;
};

#define GOWL_TYPE_INPUT_CAPTURE (gowl_input_capture_get_type())

G_DECLARE_FINAL_TYPE(GowlInputCapture, gowl_input_capture,
                     GOWL, INPUT_CAPTURE, GObject)

/**
 * GowlInputSinkFunc:
 * @capture: the #GowlInputCapture
 * @event: (not nullable): the diverted event (valid only for the call)
 * @user_data: the data passed to gowl_input_capture_set_sink()
 *
 * Receives an input event diverted away from the focused client while
 * capture is active.  The @event pointer is owned by the caller and only
 * valid for the duration of the call; copy the value if it must outlive
 * it.  Called from whatever thread drives the compositor input hooks.
 */
typedef void (*GowlInputSinkFunc)(GowlInputCapture     *capture,
                                  const GowlInputEvent *event,
                                  gpointer              user_data);

/**
 * GowlInputActivationFunc:
 * @capture: the #GowlInputCapture
 * @activation_id: monotonically increasing id for this activation
 * @x: cursor x at the crossing point, layout-relative
 * @y: cursor y at the crossing point, layout-relative
 * @barrier_id: the id of the barrier that was crossed
 * @activated: %TRUE on activation (barrier crossed), %FALSE on
 *   deactivation (capture released back to local)
 * @user_data: the data passed to gowl_input_capture_set_activation_callback()
 *
 * Fired when capture activates (a barrier was crossed) or deactivates.
 * Maps to the portal Activated/Deactivated signals.
 */
typedef void (*GowlInputActivationFunc)(GowlInputCapture *capture,
                                        guint32           activation_id,
                                        gdouble           x,
                                        gdouble           y,
                                        guint32           barrier_id,
                                        gboolean          activated,
                                        gpointer          user_data);

GowlInputCapture * gowl_input_capture_new (void);

GowlInputCaptureCapability
                   gowl_input_capture_get_capabilities
                                          (GowlInputCapture *self);

void               gowl_input_capture_set_zones
                                          (GowlInputCapture *self,
                                           GList            *zones);

GList *            gowl_input_capture_get_zones
                                          (GowlInputCapture *self);

guint              gowl_input_capture_get_zone_set
                                          (GowlInputCapture *self);

gboolean           gowl_input_capture_set_barriers
                                          (GowlInputCapture *self,
                                           GList            *barriers,
                                           GArray           *accepted_ids,
                                           GError          **error);

guint              gowl_input_capture_n_barriers
                                          (GowlInputCapture *self);

void               gowl_input_capture_enable   (GowlInputCapture *self);

void               gowl_input_capture_disable  (GowlInputCapture *self);

void               gowl_input_capture_release  (GowlInputCapture *self);

gboolean           gowl_input_capture_is_enabled
                                          (GowlInputCapture *self);

gboolean           gowl_input_capture_is_active
                                          (GowlInputCapture *self);

void               gowl_input_capture_set_sink
                                          (GowlInputCapture  *self,
                                           GowlInputSinkFunc  sink,
                                           gpointer           user_data);

void               gowl_input_capture_set_activation_callback
                                          (GowlInputCapture        *self,
                                           GowlInputActivationFunc  cb,
                                           gpointer                 user_data);

gboolean           gowl_input_capture_check_crossing
                                          (GowlInputCapture *self,
                                           gdouble           prev_x,
                                           gdouble           prev_y,
                                           gdouble           cur_x,
                                           gdouble           cur_y);

void               gowl_input_capture_deactivate
                                          (GowlInputCapture *self);

void               gowl_input_capture_emit (GowlInputCapture     *self,
                                            const GowlInputEvent *event);

G_END_DECLS

#endif /* GOWL_INPUT_CAPTURE_H */
