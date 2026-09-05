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
 * gowl-tablet.c -- graphics tablets (tablet-v2)
 *
 * Before this, gowl's new-input handler knew about keyboards, pointers
 * and switches, and everything else fell into `default: break;'.  A pen
 * therefore did nothing at all: not a stroke, not a click, not even
 * cursor motion.
 *
 * Two audiences, and both have to be served from the same events:
 *
 *   - clients that speak tablet-v2 (GTK3 and GTK4 both do) get real
 *     tool events, with pressure, tilt, distance and rotation.  That is
 *     what makes a pen a pen -- cmacs's org-ex ink already reads
 *     GDK_AXIS_PRESSURE and scales stroke width by it, and has simply
 *     never received a non-1.0 value under gowl.
 *   - everything else gets pointer emulation, because a pen that cannot
 *     click a GTK2 dialog or an X11 app is not much better than a pen
 *     that does nothing.
 *
 * Which one a given event takes is decided per motion, by asking
 * whether the surface under the tool accepts tablet-v2.  A tool can
 * therefore cross from an inking buffer onto a menu and keep working.
 */

#include "gowl-tablet.h"
#include "gowl-core-private.h"
#include "gowl-compositor.h"
#include "gowl-seat.h"

#include <math.h>
#include <linux/input-event-codes.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_tablet_pad.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_v2.h>
#include <wlr/types/wlr_input_device.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

/*
 * The surface under the tool, and whether it speaks tablet-v2.
 *
 * Note this asks about the surface under the CURSOR, which the caller
 * has already warped for this event.  Doing it in that order is what
 * lets a tool decide per-event: the answer changes the moment the pen
 * crosses from an inking canvas onto a plain widget.
 */
static struct wlr_surface *
tablet_surface_under(GowlCompositor *self, gdouble *sx, gdouble *sy)
{
	struct wlr_surface *surface = NULL;

	gowl_compositor_surface_at(self, self->wlr_cursor->x,
	                            self->wlr_cursor->y, &surface, sx, sy);
	return surface;
}

/*
 * Map a stylus button onto a pointer button for the emulation path.
 * The tip is handled separately; these are the barrel buttons.
 */
static guint32
tablet_button_to_pointer(guint32 button)
{
	switch (button) {
	case BTN_STYLUS:  return BTN_RIGHT;
	case BTN_STYLUS2: return BTN_MIDDLE;
	case BTN_STYLUS3: return BTN_SIDE;
	default:          return BTN_LEFT;
	}
}

static void on_tool_destroy (struct wl_listener *listener, void *data);

/*
 * Find or create the per-tool state.  libinput reports a tool the
 * first time it is seen and again after it leaves and returns, so the
 * lookup is lazy and cached on the wlroots object rather than searched
 * for on every event.
 */
static GowlTabletTool *
tablet_tool_get(GowlCompositor         *self,
                 struct wlr_tablet_tool *wlr_tool)
{
	GowlTabletTool *tool;

	if (wlr_tool->data != NULL)
		return (GowlTabletTool *)wlr_tool->data;

	tool = g_new0(GowlTabletTool, 1);
	tool->compositor = self;
	tool->wlr_tool = wlr_tool;
	tool->tool_v2 = wlr_tablet_tool_create(self->tablet_manager,
	                                        self->wlr_seat, wlr_tool);
	if (tool->tool_v2 == NULL) {
		g_warning("gowl_tablet: could not create tablet tool");
		g_free(tool);
		return NULL;
	}

	wlr_tool->data = tool;
	tool->destroy.notify = on_tool_destroy;
	wl_signal_add(&wlr_tool->events.destroy, &tool->destroy);
	wl_list_insert(&self->tablet_tools, &tool->link);

	g_debug("gowl_tablet: tool added (pressure=%s tilt=%s)",
	        wlr_tool->pressure ? "yes" : "no",
	        wlr_tool->tilt ? "yes" : "no");
	return tool;
}

/* ── Event handlers ──────────────────────────────────────────────── */

/*
 * Axis: the pen moved, or changed pressure/tilt/rotation without
 * moving.  Only the axes flagged in updated_axes carry new values --
 * passing a stale one back to the client would jitter the stroke, and
 * NAN is how wlr_cursor spells "this axis did not change".
 */
static void
on_tablet_axis(struct wl_listener *listener, void *data)
{
	GowlTablet *tablet = wl_container_of(listener, tablet, axis);
	struct wlr_tablet_tool_axis_event *ev = data;
	GowlCompositor *self = tablet->compositor;
	GowlTabletTool *tool;
	struct wlr_surface *surface;
	gdouble sx = 0, sy = 0;

	tool = tablet_tool_get(self, ev->tool);
	if (tool == NULL)
		return;

	if (ev->updated_axes & (WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y)) {
		wlr_cursor_warp_absolute(
			self->wlr_cursor, &tablet->wlr_tablet->base,
			(ev->updated_axes & WLR_TABLET_TOOL_AXIS_X) ? ev->x : NAN,
			(ev->updated_axes & WLR_TABLET_TOOL_AXIS_Y) ? ev->y : NAN);
	}

	surface = tablet_surface_under(self, &sx, &sy);

	if (surface != NULL
	    && wlr_surface_accepts_tablet_v2(surface, tablet->tablet_v2)) {
		wlr_tablet_v2_tablet_tool_notify_proximity_in(
			tool->tool_v2, tablet->tablet_v2, surface);
		wlr_tablet_v2_tablet_tool_notify_motion(tool->tool_v2, sx, sy);

		if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE)
			wlr_tablet_v2_tablet_tool_notify_pressure(
				tool->tool_v2, ev->pressure);
		if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE)
			wlr_tablet_v2_tablet_tool_notify_distance(
				tool->tool_v2, ev->distance);
		if (ev->updated_axes & (WLR_TABLET_TOOL_AXIS_TILT_X
		                        | WLR_TABLET_TOOL_AXIS_TILT_Y))
			wlr_tablet_v2_tablet_tool_notify_tilt(
				tool->tool_v2, ev->tilt_x, ev->tilt_y);
		if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_ROTATION)
			wlr_tablet_v2_tablet_tool_notify_rotation(
				tool->tool_v2, ev->rotation);
		if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_SLIDER)
			wlr_tablet_v2_tablet_tool_notify_slider(
				tool->tool_v2, ev->slider);
		if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_WHEEL)
			wlr_tablet_v2_tablet_tool_notify_wheel(
				tool->tool_v2, ev->wheel_delta, 0);
		return;
	}

	/* Not a tablet-aware surface: the pen is a mouse.  The full
	 * motionnotify path, so sloppy focus, pointer enter/leave and the
	 * cursor image all behave exactly as they do for a real pointer. */
	wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->tool_v2);
	gowl_compositor_motionnotify(self, ev->time_msec);
}

/*
 * Proximity: the pen entered or left the tablet's hover range.  This is
 * what bounds a stroke session -- a client that never sees proximity_out
 * keeps drawing a cursor for a pen that is in a drawer.
 */
static void
on_tablet_proximity(struct wl_listener *listener, void *data)
{
	GowlTablet *tablet = wl_container_of(listener, tablet, proximity);
	struct wlr_tablet_tool_proximity_event *ev = data;
	GowlCompositor *self = tablet->compositor;
	GowlTabletTool *tool;

	tool = tablet_tool_get(self, ev->tool);
	if (tool == NULL)
		return;

	if (ev->state == WLR_TABLET_TOOL_PROXIMITY_OUT) {
		/* A pen lifted mid-stroke must not leave the client believing
		 * the tip is still down; nothing else will ever tell it. */
		if (tool->tip_down) {
			wlr_tablet_v2_tablet_tool_notify_up(tool->tool_v2);
			tool->tip_down = FALSE;
		}
		wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->tool_v2);
		return;
	}

	wlr_cursor_warp_absolute(self->wlr_cursor,
	                          &tablet->wlr_tablet->base, ev->x, ev->y);
	gowl_compositor_motionnotify(self, ev->time_msec);
}

/*
 * Tip: the pen touched down or lifted.  On a tablet-aware surface this
 * is a tool down/up; everywhere else it is a left click, which is what
 * makes the pen usable as a pointing device.
 */
static void
on_tablet_tip(struct wl_listener *listener, void *data)
{
	GowlTablet *tablet = wl_container_of(listener, tablet, tip);
	struct wlr_tablet_tool_tip_event *ev = data;
	GowlCompositor *self = tablet->compositor;
	GowlTabletTool *tool;
	struct wlr_surface *surface;
	gdouble sx = 0, sy = 0;

	tool = tablet_tool_get(self, ev->tool);
	if (tool == NULL)
		return;

	surface = tablet_surface_under(self, &sx, &sy);

	if (surface != NULL
	    && wlr_surface_accepts_tablet_v2(surface, tablet->tablet_v2)) {
		if (ev->state == WLR_TABLET_TOOL_TIP_DOWN) {
			wlr_tablet_v2_tablet_tool_notify_down(tool->tool_v2);
			tool->tip_down = TRUE;
		} else {
			wlr_tablet_v2_tablet_tool_notify_up(tool->tool_v2);
			tool->tip_down = FALSE;
		}
		return;
	}

	wlr_seat_pointer_notify_button(
		self->wlr_seat, ev->time_msec, BTN_LEFT,
		ev->state == WLR_TABLET_TOOL_TIP_DOWN
		? WL_POINTER_BUTTON_STATE_PRESSED
		: WL_POINTER_BUTTON_STATE_RELEASED);
	tool->tip_down = (ev->state == WLR_TABLET_TOOL_TIP_DOWN);
}

/* Barrel buttons, routed the same way as the tip. */
static void
on_tablet_button(struct wl_listener *listener, void *data)
{
	GowlTablet *tablet = wl_container_of(listener, tablet, button);
	struct wlr_tablet_tool_button_event *ev = data;
	GowlCompositor *self = tablet->compositor;
	GowlTabletTool *tool;
	struct wlr_surface *surface;
	gdouble sx = 0, sy = 0;

	tool = tablet_tool_get(self, ev->tool);
	if (tool == NULL)
		return;

	surface = tablet_surface_under(self, &sx, &sy);

	if (surface != NULL
	    && wlr_surface_accepts_tablet_v2(surface, tablet->tablet_v2)) {
		/* The tool notifier takes the PAD button-state enum -- that
		 * is wlroots' own signature, not a mistake here. */
		wlr_tablet_v2_tablet_tool_notify_button(
			tool->tool_v2, ev->button,
			ev->state == WLR_BUTTON_PRESSED
			? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED
			: ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
		return;
	}

	wlr_seat_pointer_notify_button(
		self->wlr_seat, ev->time_msec,
		tablet_button_to_pointer(ev->button),
		ev->state == WLR_BUTTON_PRESSED
		? WL_POINTER_BUTTON_STATE_PRESSED
		: WL_POINTER_BUTTON_STATE_RELEASED);
}

/* ── Lifetime ────────────────────────────────────────────────────── */

static void
on_tool_destroy(struct wl_listener *listener, void *data)
{
	GowlTabletTool *tool = wl_container_of(listener, tool, destroy);

	(void)data;
	wl_list_remove(&tool->destroy.link);
	wl_list_remove(&tool->link);
	if (tool->wlr_tool != NULL)
		tool->wlr_tool->data = NULL;
	g_free(tool);
}

static void
on_tablet_destroy(struct wl_listener *listener, void *data)
{
	GowlTablet *tablet = wl_container_of(listener, tablet, destroy);

	(void)data;
	wl_list_remove(&tablet->axis.link);
	wl_list_remove(&tablet->proximity.link);
	wl_list_remove(&tablet->tip.link);
	wl_list_remove(&tablet->button.link);
	wl_list_remove(&tablet->destroy.link);
	wl_list_remove(&tablet->link);
	g_free(tablet);
}

static void
on_pad_destroy(struct wl_listener *listener, void *data)
{
	GowlTabletPad *pad = wl_container_of(listener, pad, destroy);

	(void)data;
	wl_list_remove(&pad->destroy.link);
	wl_list_remove(&pad->link);
	g_free(pad);
}

/* ── Public API ──────────────────────────────────────────────────── */

void
gowl_tablet_manager_init(GowlCompositor *self)
{
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	wl_list_init(&self->tablets);
	wl_list_init(&self->tablet_tools);
	wl_list_init(&self->tablet_pads);

	self->tablet_manager = wlr_tablet_v2_create(self->wl_display);
	if (self->tablet_manager == NULL)
		g_warning("gowl_tablet: tablet-v2 global not created; "
		          "pens will not work");
}

gboolean
gowl_tablet_new_device(GowlCompositor *self, struct wlr_input_device *device)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);
	g_return_val_if_fail(device != NULL, FALSE);

	if (self->tablet_manager == NULL)
		return FALSE;

	if (device->type == WLR_INPUT_DEVICE_TABLET) {
		GowlTablet *tablet = g_new0(GowlTablet, 1);

		tablet->compositor = self;
		tablet->wlr_tablet = wlr_tablet_from_input_device(device);
		tablet->tablet_v2 = wlr_tablet_create(self->tablet_manager,
		                                       self->wlr_seat, device);
		if (tablet->tablet_v2 == NULL) {
			g_warning("gowl_tablet: could not create tablet for '%s'",
			          device->name ? device->name : "(unnamed)");
			g_free(tablet);
			return FALSE;
		}

		/* Attached to the cursor so wlroots maps the tablet's absolute
		 * 0..1 coordinates onto the output layout for us -- including
		 * any per-device output mapping libinput reports. */
		wlr_cursor_attach_input_device(self->wlr_cursor, device);

		tablet->axis.notify = on_tablet_axis;
		wl_signal_add(&tablet->wlr_tablet->events.axis, &tablet->axis);
		tablet->proximity.notify = on_tablet_proximity;
		wl_signal_add(&tablet->wlr_tablet->events.proximity,
		              &tablet->proximity);
		tablet->tip.notify = on_tablet_tip;
		wl_signal_add(&tablet->wlr_tablet->events.tip, &tablet->tip);
		tablet->button.notify = on_tablet_button;
		wl_signal_add(&tablet->wlr_tablet->events.button, &tablet->button);
		tablet->destroy.notify = on_tablet_destroy;
		wl_signal_add(&device->events.destroy, &tablet->destroy);

		wl_list_insert(&self->tablets, &tablet->link);

		g_message("gowl: tablet '%s' attached (%.0fx%.0f mm)",
		          device->name ? device->name : "(unnamed)",
		          tablet->wlr_tablet->width_mm,
		          tablet->wlr_tablet->height_mm);
		return TRUE;
	}

	if (device->type == WLR_INPUT_DEVICE_TABLET_PAD) {
		GowlTabletPad *pad = g_new0(GowlTabletPad, 1);

		pad->compositor = self;
		pad->pad_v2 = wlr_tablet_pad_create(self->tablet_manager,
		                                     self->wlr_seat, device);
		if (pad->pad_v2 == NULL) {
			g_free(pad);
			return FALSE;
		}

		/* Presented to clients, but gowl binds nothing to the pad's own
		 * buttons: what they should do is a user decision, and the
		 * keybind table is where user decisions live. */
		pad->destroy.notify = on_pad_destroy;
		wl_signal_add(&device->events.destroy, &pad->destroy);
		wl_list_insert(&self->tablet_pads, &pad->link);

		g_debug("gowl_tablet: pad '%s' attached",
		        device->name ? device->name : "(unnamed)");
		return TRUE;
	}

	return FALSE;
}

void
gowl_tablet_finish(GowlCompositor *self)
{
	GowlTablet *tablet, *tablet_tmp;
	GowlTabletTool *tool, *tool_tmp;
	GowlTabletPad *pad, *pad_tmp;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	if (self->tablets.next == NULL)   /* never initialised */
		return;

	wl_list_for_each_safe(tablet, tablet_tmp, &self->tablets, link)
		on_tablet_destroy(&tablet->destroy, NULL);
	wl_list_for_each_safe(tool, tool_tmp, &self->tablet_tools, link)
		on_tool_destroy(&tool->destroy, NULL);
	wl_list_for_each_safe(pad, pad_tmp, &self->tablet_pads, link)
		on_pad_destroy(&pad->destroy, NULL);
}

gboolean
gowl_tablet_has_devices(GowlCompositor *self)
{
	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);

	if (self->tablets.next == NULL)
		return FALSE;
	return !wl_list_empty(&self->tablets);
}
