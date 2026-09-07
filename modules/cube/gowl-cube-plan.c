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

#include "gowl-cube-plan.h"

#include <math.h>

gint
gowl_cube_tag_index(guint32 tagmask)
{
	gint i;

	for (i = 0; i < GOWL_CUBE_MAX_TAGS; i++) {
		if ((tagmask & (1u << i)) != 0)
			return i;
	}
	return -1;
}

gboolean
gowl_cube_plan_init(
	GowlCubePlan *plan,
	gint          from_tag,
	gint          to_tag,
	gint          faces,
	gint          base_ms,
	gint          step_ms,
	gint64        now_us
){
	gint steps;
	gint total_ms;
	gint cap_ms;
	gint j;

	if (plan == NULL)
		return FALSE;

	if (from_tag < 0 || to_tag < 0
	    || from_tag >= GOWL_CUBE_MAX_TAGS || to_tag >= GOWL_CUBE_MAX_TAGS
	    || from_tag == to_tag)
		return FALSE;

	if (base_ms <= 0)
		return FALSE;

	steps = ABS(to_tag - from_tag);

	/* A longer journey should feel longer without becoming a wait.  The
	 * cap is what keeps 1 -> 9 from running for the better part of two
	 * seconds while the user's keystroke sits unanswered. */
	total_ms = base_ms + (steps - 1) * MAX(step_ms, 0);
	cap_ms   = base_ms * 3;
	if (total_ms > cap_ms)
		total_ms = cap_ms;

	plan->faces    = CLAMP(faces, GOWL_CUBE_MIN_FACES, GOWL_CUBE_MAX_FACES);
	plan->steps    = steps;
	plan->dir      = to_tag > from_tag ? 1 : -1;
	plan->start_us = now_us;
	plan->dur_us   = (gint64)total_ms * 1000;

	/* Explicitly, not by trusting the caller's storage: a plan is often a
	 * local, and an uninitialised rewind flag would run the very first
	 * rotation of a session backwards. */
	plan->rewind      = FALSE;
	plan->rewind_from = 0.0;

	for (j = 0; j <= steps; j++)
		plan->tag[j] = from_tag + plan->dir * j;

	return TRUE;
}

gdouble
gowl_cube_plan_progress(const GowlCubePlan *plan, gint64 now_us)
{
	gdouble t;

	if (plan == NULL || plan->dur_us <= 0)
		return 1.0;

	t = (gdouble)(now_us - plan->start_us) / (gdouble)plan->dur_us;
	t = CLAMP(t, 0.0, 1.0);

	/* A reversed plan runs the same clock the other way, from where the
	 * journey was abandoned back to nothing. */
	return plan->rewind ? plan->rewind_from * (1.0 - t) : t;
}

/* Shortest trip back that still reads as a decision rather than a
 * glitch, in milliseconds. */
#define GOWL_CUBE_REWIND_MIN_MS 90

void
gowl_cube_plan_reverse(GowlCubePlan *plan, gdouble from_progress, gint64 now_us)
{
	gint64 full;

	if (plan == NULL)
		return;

	full = plan->dur_us;
	if (plan->rewind) {
		/* Already going back; do not compound the shortening. */
		plan->rewind_from = CLAMP(from_progress, 0.0, 1.0);
		plan->start_us = now_us;
		return;
	}

	plan->rewind      = TRUE;
	plan->rewind_from = CLAMP(from_progress, 0.0, 1.0);
	plan->start_us    = now_us;
	plan->dur_us      = (gint64)((gdouble)full * plan->rewind_from);
	if (plan->dur_us < GOWL_CUBE_REWIND_MIN_MS * 1000)
		plan->dur_us = GOWL_CUBE_REWIND_MIN_MS * 1000;
}

gboolean
gowl_cube_plan_finished(const GowlCubePlan *plan, gint64 now_us)
{
	if (plan == NULL || plan->dur_us <= 0)
		return TRUE;

	/* The clock, not the progress: a reversed plan ends at zero, and
	 * asking "is progress 1.0 yet" would never let one go. */
	return now_us - plan->start_us >= plan->dur_us;
}

gdouble
gowl_cube_plan_face_angle(const GowlCubePlan *plan)
{
	if (plan == NULL || plan->faces <= 0)
		return G_PI_2;
	return 2.0 * G_PI / (gdouble)plan->faces;
}

gdouble
gowl_cube_plan_rotation(const GowlCubePlan *plan, gdouble eased)
{
	if (plan == NULL)
		return 0.0;
	return eased * (gdouble)plan->steps * gowl_cube_plan_face_angle(plan);
}

void
gowl_cube_plan_slot_window(
	const GowlCubePlan *plan,
	gdouble             eased,
	gint               *first,
	gint               *last
){
	gdouble travelled;
	gint    front;

	if (plan == NULL) {
		if (first != NULL) *first = 0;
		if (last  != NULL) *last  = 0;
		return;
	}

	/* How many whole sides have gone by.  floor(), not round(), so the
	 * pair on screen is always {front, front + 1}: those are the two
	 * whose shared edge is the corner the user is looking at. */
	travelled = eased * (gdouble)plan->steps;
	front = (gint)floor(travelled);

	/*
	 * One side of slack each way covers the ends of an overshooting
	 * curve, which can carry `eased' outside 0..1 and briefly bring the
	 * side beyond the destination into view.
	 *
	 * The span is capped at one less than the face count because a slot
	 * and its own repeat are exactly `faces' apart: on a three-sided
	 * solid, front-1 and front+2 are the SAME plane, and drawing both
	 * would put two desktops on one side of the prism.
	 */
	if (first != NULL) *first = front - 1;
	if (last  != NULL) *last  = front + MIN(2, plan->faces - 2);
}

gdouble
gowl_cube_plan_bump(gdouble t)
{
	gdouble s;

	if (t <= 0.0 || t >= 1.0)
		return 0.0;

	/*
	 * A sine, smoothstepped.
	 *
	 * "Exactly zero at the ends" is not enough on its own, because no
	 * frame lands exactly on t = 1: at sixty frames a second the last one
	 * drawn is around t = 0.97, and an envelope that leaves zero steeply
	 * --- sin(pi t) raised to a power below one, which was the first
	 * thing tried here --- is still a fifth of the way pulled back at
	 * that point.  The rotation then ends with a visible jolt as the
	 * sheet comes off.  Smoothstep is quadratically flat at both ends, so
	 * the same frame is under three per cent in, which is invisible,
	 * while the middle stays broad enough that the solid is fully formed
	 * for most of the turn.
	 */
	s = sin(G_PI * t);
	return s * s * (3.0 - 2.0 * s);
}
