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
 * gowl-cube-plan.h -- what turns, how far, and for how long.
 *
 * This is the whole decision-making half of the cube and it deliberately
 * knows nothing about wlroots, GL or the compositor: given "you were on
 * tag 1, you are now on tag 4", it answers "three face-steps, this long,
 * these tags in this order, and at time T the solid has turned this far".
 * Everything with a chance of being subtly wrong lives here, where a test
 * can call it without a GPU.
 */

#ifndef GOWL_CUBE_PLAN_H
#define GOWL_CUBE_PLAN_H

#include <glib.h>

G_BEGIN_DECLS

/* gowl has nine tags, so the furthest jump (1 -> 9) is eight steps. */
#define GOWL_CUBE_MAX_TAGS  9
#define GOWL_CUBE_MAX_STEPS (GOWL_CUBE_MAX_TAGS - 1)

/* Sides of the prism.  Three is the least that encloses a volume; past a
 * dozen the faces are too narrow to read on the way past. */
#define GOWL_CUBE_MIN_FACES 3
#define GOWL_CUBE_MAX_FACES 12

/**
 * GowlCubePlan:
 * @faces: sides of the prism, GOWL_CUBE_MIN_FACES..GOWL_CUBE_MAX_FACES
 * @steps: how many face-steps to travel; always >= 1 for a valid plan
 * @dir: +1 when tag numbers increase, -1 when they decrease
 * @tag: tag index shown at each slot, @steps + 1 entries, slot 0 first
 * @start_us: monotonic microseconds at which the rotation began
 * @dur_us: total run time in microseconds
 *
 * One rotation, fully described.  "Slot j" is the jth desktop in the
 * journey, not the jth side of the prism: with four sides and an eight
 * step journey the same side is reused, which is exactly why slots and
 * sides are separate ideas here.
 */
typedef struct {
	gint     faces;
	gint     steps;
	gint     dir;
	gint     tag[GOWL_CUBE_MAX_STEPS + 1];
	gint64   start_us;
	gint64   dur_us;

	/* Set when the journey has been abandoned and is running backwards
	 * to where it started; see gowl_cube_plan_reverse(). */
	gboolean rewind;
	gdouble  rewind_from;
} GowlCubePlan;

/**
 * gowl_cube_tag_index:
 * @tagmask: a gowl tag bitmask
 *
 * Reduces a tag bitmask to the ordinal the cube turns to.
 *
 * A tag view can hold several bits at once (Super+Ctrl adds a tag to the
 * view rather than replacing it).  There is no honest ordinal for such a
 * set, so the lowest set bit wins: viewing {1,2} and then {2} still reads
 * as one step forward, while merely ADDING tag 2 to a view of tag 1 keeps
 * the same index and so does not spin --- which is right, because nothing
 * was replaced.
 *
 * Returns: a 0-based tag index, or -1 when @tagmask selects nothing.
 */
gint gowl_cube_tag_index (guint32 tagmask);

/**
 * gowl_cube_plan_init:
 * @plan: (out): the plan to fill in
 * @from_tag: 0-based tag index being left
 * @to_tag: 0-based tag index being entered
 * @faces: configured prism sides (clamped)
 * @base_ms: duration of a single-face rotation
 * @step_ms: added per EXTRA face beyond the first
 * @now_us: monotonic microseconds, the start of the rotation
 *
 * Plans a rotation.  The journey is every tag between the two, in order,
 * so tag 1 to tag 4 turns three times and shows tags 2 and 3 on the way
 * rather than cross-fading past them.  There is no wrap-around: 9 to 1 is
 * the eight-step trip, not a two-step shortcut, because the tags are a
 * row in the user's head and in the bar, not a ring.
 *
 * Total duration is @base_ms + (steps - 1) * @step_ms, capped at three
 * times @base_ms.  A longer journey should feel longer, but an eight-step
 * jump at full price would be a loading screen.
 *
 * Returns: %TRUE when there is something to animate; %FALSE when the two
 *   tags are the same, either is out of range, or the duration is zero.
 */
gboolean gowl_cube_plan_init (GowlCubePlan *plan,
                              gint          from_tag,
                              gint          to_tag,
                              gint          faces,
                              gint          base_ms,
                              gint          step_ms,
                              gint64        now_us);

/**
 * gowl_cube_plan_reverse:
 * @plan: (inout): the plan to turn around
 * @from_progress: where the journey got to, 0.0..1.0
 * @now_us: monotonic microseconds
 *
 * Abandons the journey and sends the solid back to where it started.
 *
 * This is what letting go of a half-finished swipe does.  It is not a
 * second plan: the same tags, sides, direction and captures carry the
 * rotation home, so the picture is continuous across the moment the
 * fingers lift.  The trip back is shortened in proportion to how far it
 * got --- a rotation abandoned after a tenth of a turn should not take
 * as long to undo as one abandoned just short of the end --- with a
 * floor, because an instant snap back reads as a glitch rather than as
 * a decision.
 */
void gowl_cube_plan_reverse (GowlCubePlan *plan,
                             gdouble       from_progress,
                             gint64        now_us);

/**
 * gowl_cube_plan_finished:
 * @plan: a plan
 * @now_us: monotonic microseconds
 *
 * Whether the rotation is over and its resources can go.
 *
 * Not the same as progress reaching 1.0: a reversed plan finishes at 0.0,
 * and asking about progress alone would leave a rewound rotation on
 * screen for ever.
 *
 * Returns: %TRUE when the plan has run its course.
 */
gboolean gowl_cube_plan_finished (const GowlCubePlan *plan, gint64 now_us);

/**
 * gowl_cube_plan_progress:
 * @plan: a plan
 * @now_us: monotonic microseconds
 *
 * Returns: linear progress clamped to 0.0..1.0.
 */
gdouble gowl_cube_plan_progress (const GowlCubePlan *plan, gint64 now_us);

/**
 * gowl_cube_plan_face_angle:
 * @plan: a plan
 *
 * Returns: the angle subtended by one side, in radians.
 */
gdouble gowl_cube_plan_face_angle (const GowlCubePlan *plan);

/**
 * gowl_cube_plan_rotation:
 * @plan: a plan
 * @eased: eased progress, normally 0.0..1.0 but may overshoot
 *
 * Returns: how far the solid has turned, in radians, signed by direction.
 *   Slot j sits at this angle subtracted from j face-angles, so at the
 *   end slot @steps is dead ahead.
 */
gdouble gowl_cube_plan_rotation (const GowlCubePlan *plan, gdouble eased);

/**
 * gowl_cube_plan_slot_window:
 * @plan: a plan
 * @eased: eased progress
 * @first: (out): lowest slot worth drawing
 * @last: (out): highest slot worth drawing
 *
 * Narrows drawing to the slots near the front.
 *
 * Slots repeat every @faces sides, so slot j and slot j+faces occupy the
 * same plane; drawing both would z-fight on a renderer with no depth
 * buffer (and this one has none, by design --- a convex solid with
 * back-face culling does not need one).  The window is small enough that
 * two slots can never land on the same side.
 *
 * The range may fall outside 0..@steps at the ends of the journey; those
 * slots have no desktop and are drawn as blank sides so the solid stays
 * closed.
 */
void gowl_cube_plan_slot_window (const GowlCubePlan *plan,
                                 gdouble             eased,
                                 gint               *first,
                                 gint               *last);

/**
 * gowl_cube_plan_bump:
 * @t: linear progress 0.0..1.0
 *
 * The in-and-out envelope for everything that must not be visible at
 * either end: camera pull-back, pitch, backdrop, reflection, caps.
 *
 * At t = 0 and t = 1 this is exactly 0, which is what makes the rotation
 * start and finish pixel-identical to the flat desktop --- with no
 * pull-back and no pitch, the leading face projects to precisely the
 * viewport, so there is no cut into or out of the effect.
 *
 * Returns: 0.0 at both ends, 1.0 in the middle.
 */
gdouble gowl_cube_plan_bump (gdouble t);

G_END_DECLS

#endif /* GOWL_CUBE_PLAN_H */
