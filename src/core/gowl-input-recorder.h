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

#ifndef GOWL_INPUT_RECORDER_H
#define GOWL_INPUT_RECORDER_H

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * GOWL_INPUT_RECORDER_MAX_EVENTS_CEILING:
 *
 * Hard ceiling on the ring size a caller may ask for.  The ring is
 * allocated up front and lives inside the compositor process, so the
 * ceiling is what stops a careless caller from asking for a buffer that
 * takes the session down with it.
 */
#define GOWL_INPUT_RECORDER_MAX_EVENTS_CEILING (100000)

/**
 * GOWL_INPUT_RECORDER_MAX_SECONDS_CEILING:
 *
 * Hard ceiling on the self-stop deadline, in seconds (one hour).
 */
#define GOWL_INPUT_RECORDER_MAX_SECONDS_CEILING (3600)

/**
 * GOWL_INPUT_RECORDER_DEFAULT_MAX_EVENTS:
 *
 * Ring size used when a caller passes 0.
 */
#define GOWL_INPUT_RECORDER_DEFAULT_MAX_EVENTS (4096)

/**
 * GOWL_INPUT_RECORDER_DEFAULT_MAX_SECONDS:
 *
 * Self-stop deadline used when a caller passes 0, in seconds.
 */
#define GOWL_INPUT_RECORDER_DEFAULT_MAX_SECONDS (120)

/**
 * GOWL_INPUT_RECORDER_MOTION_COALESCE_US:
 *
 * Two consecutive pointer-motion events closer together than this are
 * merged into one ring entry, accumulating the delta and keeping the
 * later position.  A single drag is hundreds of motion events, and
 * without this the ring fills with them and drops the clicks and keys a
 * demonstration is actually about.  A merge is not a drop: nothing is
 * lost except the sampling rate, and the count is reported separately.
 */
#define GOWL_INPUT_RECORDER_MOTION_COALESCE_US (8000)

/**
 * GowlRecordedEventType:
 * @GOWL_RECORDED_EVENT_KEY: a keyboard key press or release.
 * @GOWL_RECORDED_EVENT_MODIFIERS: the modifier state changed.
 * @GOWL_RECORDED_EVENT_POINTER_MOTION: the pointer moved.
 * @GOWL_RECORDED_EVENT_POINTER_BUTTON: a pointer button press or release.
 * @GOWL_RECORDED_EVENT_POINTER_AXIS: a scroll event.
 *
 * Discriminator for #GowlRecordedEvent.
 */
typedef enum {
	GOWL_RECORDED_EVENT_KEY = 0,
	GOWL_RECORDED_EVENT_MODIFIERS,
	GOWL_RECORDED_EVENT_POINTER_MOTION,
	GOWL_RECORDED_EVENT_POINTER_BUTTON,
	GOWL_RECORDED_EVENT_POINTER_AXIS
} GowlRecordedEventType;

/**
 * GowlRecordedEvent:
 * @type: which kind of event this is.
 * @wall_us: wall-clock time of capture, microseconds since the Unix
 *   epoch (%CLOCK_REALTIME, from g_get_real_time()).
 * @offset_us: microseconds since the recording started, from the
 *   monotonic clock.
 * @keycode: evdev keycode (KEY).
 * @keysym: resolved xkb keysym, 0 when unknown (KEY).
 * @state: 1 = pressed/down, 0 = released/up (KEY, POINTER_BUTTON).
 * @mods: effective modifier mask at capture (KEY, POINTER_BUTTON).
 * @button: evdev button code (POINTER_BUTTON).
 * @axis: 0 = vertical, 1 = horizontal (POINTER_AXIS).
 * @value: scroll delta (POINTER_AXIS).
 * @discrete: discrete scroll steps, 0 if continuous (POINTER_AXIS).
 * @x: layout-absolute cursor x (POINTER_*).
 * @y: layout-absolute cursor y (POINTER_*).
 * @dx: accumulated relative motion x (POINTER_MOTION).
 * @dy: accumulated relative motion y (POINTER_MOTION).
 * @mods_depressed: depressed modifiers (MODIFIERS).
 * @mods_latched: latched modifiers (MODIFIERS).
 * @mods_locked: locked modifiers (MODIFIERS).
 * @mods_group: effective layout group (MODIFIERS).
 * @merged: how many further motion events were coalesced into this one.
 *
 * One entry in the recorder's ring.  A plain POD value so the ring can
 * be a flat array with no per-event allocation.
 *
 * There is deliberately **no compositor event timestamp** here.  The
 * compositor's own clock is what
 * gowl_compositor_inject_pointer_button() and friends stamp synthetic
 * events with, and a captured value that can be handed straight back to
 * an injection call is how a replay ends up asserting that a human
 * pressed a key at a time in the past.  The two timebases in this
 * struct -- wall time and an offset from the start of *this* recording
 * -- are both useless as synthetic timestamps, which is the point.
 */
typedef struct _GowlRecordedEvent GowlRecordedEvent;

struct _GowlRecordedEvent {
	GowlRecordedEventType type;

	gint64                wall_us;
	gint64                offset_us;

	guint32               keycode;
	guint32               keysym;
	guint32               state;
	guint32               mods;

	guint32               button;

	guint32               axis;
	gdouble               value;
	gint32                discrete;

	gdouble               x;
	gdouble               y;
	gdouble               dx;
	gdouble               dy;

	guint32               mods_depressed;
	guint32               mods_latched;
	guint32               mods_locked;
	guint32               mods_group;

	guint32               merged;
};

#define GOWL_TYPE_INPUT_RECORDER (gowl_input_recorder_get_type())

G_DECLARE_FINAL_TYPE(GowlInputRecorder, gowl_input_recorder,
                     GOWL, INPUT_RECORDER, GObject)

GowlInputRecorder * gowl_input_recorder_new       (void);

/* --- consent -------------------------------------------------------- */

gboolean     gowl_input_recorder_get_consent      (GowlInputRecorder *self);

void         gowl_input_recorder_set_consent      (GowlInputRecorder *self,
                                                   gboolean           consent);

/* --- secret suppression --------------------------------------------- */

void         gowl_input_recorder_set_deny_patterns
                                                  (GowlInputRecorder  *self,
                                                   const gchar *const *patterns);

void         gowl_input_recorder_add_deny_patterns
                                                  (GowlInputRecorder  *self,
                                                   const gchar *const *patterns);

gchar **     gowl_input_recorder_get_deny_patterns
                                                  (GowlInputRecorder *self);

const gchar *gowl_input_recorder_suppress_reason  (GowlInputRecorder *self,
                                                   gboolean           locked,
                                                   const gchar       *app_id,
                                                   const gchar       *title);

/* --- lifecycle ------------------------------------------------------- */

gchar *      gowl_input_recorder_start            (GowlInputRecorder *self,
                                                   guint              max_seconds,
                                                   guint              max_events,
                                                   GError           **error);

gchar *      gowl_input_recorder_drain            (GowlInputRecorder *self,
                                                   const gchar       *token,
                                                   GError           **error);

gchar *      gowl_input_recorder_stop             (GowlInputRecorder *self,
                                                   const gchar       *token,
                                                   GError           **error);

void         gowl_input_recorder_force_stop       (GowlInputRecorder *self,
                                                   const gchar       *reason);

gchar *      gowl_input_recorder_status           (GowlInputRecorder *self);

gboolean     gowl_input_recorder_is_active        (GowlInputRecorder *self);

const gchar *gowl_input_recorder_get_token        (GowlInputRecorder *self);

gint64       gowl_input_recorder_get_deadline_us  (GowlInputRecorder *self);

gboolean     gowl_input_recorder_check_expiry     (GowlInputRecorder *self);

/* --- the tap --------------------------------------------------------- */

void         gowl_input_recorder_note             (GowlInputRecorder       *self,
                                                   const GowlRecordedEvent *event,
                                                   const gchar             *suppress_reason);

G_END_DECLS

#endif /* GOWL_INPUT_RECORDER_H */
