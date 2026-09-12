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
 * Output power: wlr-output-power-management-v1, the `output-power'
 * keybind action, and the idle manager's dpms-timeout.
 *
 * All three end in gowl_compositor_set_monitor_powered().  Powering
 * off is NOT the lid policy's disable: the output keeps its place in
 * the layout and its clients, nothing migrates, and the next input
 * (or a client's "on") brings the picture straight back.  That is
 * what a screen blanking after twenty minutes should do; what the lid
 * policy does -- leave the layout, move the windows -- is for a panel
 * that is genuinely gone.
 *
 * Two things power an output back on: a client asking through the
 * protocol, or input arriving after the IDLE TIMER turned it off.  An
 * explicit `output-power off' does not wake on the very next input,
 * because the very next input is the release of the key that ran it;
 * it waits out a short grace period first.
 */

#include "gowl-core-private.h"

#define OUTPUT_POWER_GRACE_USEC (G_USEC_PER_SEC / 2)

/**
 * gowl_compositor_set_monitor_powered:
 * @self: the compositor
 * @m: the monitor
 * @on: %TRUE to power on, %FALSE to power off
 *
 * Commits the enable bit alone.  On, wlr-output-power-management
 * objects on this output are told by wlroots itself (it watches the
 * output's commits), so there is nothing to send here.
 */
void
gowl_compositor_set_monitor_powered(
	GowlCompositor *self,
	GowlMonitor    *m,
	gboolean        on
){
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(GOWL_IS_MONITOR(m));

	if (m->wlr_output == NULL)
		return;
	/* A panel the lid policy took out of the layout is not ours to
	 * light: it comes back when the lid opens. */
	if (wlr_output_layout_get(self->output_layout, m->wlr_output) == NULL)
		return;

	if (on == !m->powered_off && on == (gboolean)m->wlr_output->enabled)
		return;

	if (on) {
		struct wlr_output_state state;
		struct wlr_output_mode *mode;
		gboolean ok;

		/* Enabling needs a mode.  A real output has a preferred one;
		 * a headless or nested output has none and keeps the size it
		 * was running at, which is what it gets back. */
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, true);
		mode = wlr_output_preferred_mode(m->wlr_output);
		if (mode != NULL)
			wlr_output_state_set_mode(&state, mode);
		else if (m->wlr_output->width > 0 && m->wlr_output->height > 0)
			wlr_output_state_set_custom_mode(&state,
				m->wlr_output->width, m->wlr_output->height,
				m->wlr_output->refresh);
		ok = wlr_output_commit_state(m->wlr_output, &state);
		wlr_output_state_finish(&state);
		if (!ok) {
			g_warning("output-power: could not power '%s' on",
			          m->wlr_output->name);
			return;
		}
		m->powered_off = FALSE;
		/* Everything on it is stale: re-place, and ask for a frame
		 * outright since nothing has been damaged. */
		gowl_compositor_arrange(self, m);
		if (m->scene_output != NULL)
			wlr_output_schedule_frame(m->wlr_output);
	} else {
		struct wlr_output_state state;

		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, false);
		if (!wlr_output_commit_state(m->wlr_output, &state)) {
			wlr_output_state_finish(&state);
			g_warning("output-power: could not power '%s' off",
			          m->wlr_output->name);
			return;
		}
		wlr_output_state_finish(&state);
		m->powered_off = TRUE;
	}

	g_debug("output-power: '%s' %s", m->wlr_output->name,
	        on ? "on" : "off");
	g_signal_emit_by_name(self, "output-power-changed", m, on);
}

/**
 * gowl_compositor_wake_outputs:
 * @self: the compositor
 *
 * Input arrived.  Powers on every output the idle timer turned off,
 * or that an explicit `off' turned off longer ago than the grace
 * period.
 *
 * Returns: %TRUE if any output was powered on
 */
gboolean
gowl_compositor_wake_outputs(GowlCompositor *self)
{
	GList *l;
	gboolean woke = FALSE;
	gint64 now;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);

	now = g_get_monotonic_time();
	if (!self->outputs_off_by_idle && now < self->output_power_grace)
		return FALSE;

	for (l = self->monitors; l != NULL; l = l->next) {
		GowlMonitor *m = (GowlMonitor *)l->data;

		if (!m->powered_off)
			continue;
		gowl_compositor_set_monitor_powered(self, m, TRUE);
		woke = TRUE;
	}
	self->outputs_off_by_idle = FALSE;
	return woke;
}

/**
 * gowl_compositor_set_outputs_powered:
 * @self: a #GowlCompositor
 * @on: %TRUE to power every output on, %FALSE to power every one off
 *
 * The `output-power' keybind action, and what an embedder calls for a
 * "screen off" command.  Powering off starts the grace period during
 * which input does not wake the outputs, so that the release of the
 * key that ran it does not undo it.
 */
void
gowl_compositor_set_outputs_powered(
	GowlCompositor *self,
	gboolean        on
){
	GList *l;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	if (!on) {
		self->output_power_grace =
			g_get_monotonic_time() + OUTPUT_POWER_GRACE_USEC;
		self->outputs_off_by_idle = FALSE;
	}
	for (l = self->monitors; l != NULL; l = l->next)
		gowl_compositor_set_monitor_powered(self,
			(GowlMonitor *)l->data, on);
}

/**
 * gowl_compositor_any_output_powered_off:
 * @self: a #GowlCompositor
 *
 * Returns: %TRUE if at least one output is powered off through this
 *   path (not the lid policy)
 */
gboolean
gowl_compositor_any_output_powered_off(GowlCompositor *self)
{
	GList *l;

	g_return_val_if_fail(GOWL_IS_COMPOSITOR(self), FALSE);

	for (l = self->monitors; l != NULL; l = l->next)
		if (((GowlMonitor *)l->data)->powered_off)
			return TRUE;
	return FALSE;
}

/* --- the protocol --- */

static void
on_output_power_set_mode(struct wl_listener *listener, void *data)
{
	GowlCompositor *self =
		wl_container_of(listener, self, output_power_set_mode);
	struct wlr_output_power_v1_set_mode_event *ev = data;
	GList *l;

	for (l = self->monitors; l != NULL; l = l->next) {
		GowlMonitor *m = (GowlMonitor *)l->data;

		if (m->wlr_output != ev->output)
			continue;
		if (ev->mode == ZWLR_OUTPUT_POWER_V1_MODE_OFF) {
			/* A client's off is like the keybind's: the input that
			 * follows it (swayidle's own resume hook, a key) must
			 * not undo it at once. */
			self->output_power_grace = g_get_monotonic_time()
			                           + OUTPUT_POWER_GRACE_USEC;
			self->outputs_off_by_idle = FALSE;
		}
		gowl_compositor_set_monitor_powered(self, m,
			ev->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
		return;
	}
}

/**
 * gowl_output_power_init:
 * @self: the compositor, during gowl_compositor_start()
 *
 * Creates the wlr-output-power-management global.  Without it wlopm
 * exits with "compositor doesn't support" and swayidle's
 * `timeout N 'wlopm --off *'` does nothing.
 */
void
gowl_output_power_init(GowlCompositor *self)
{
	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	self->output_power_mgr =
		wlr_output_power_manager_v1_create(self->wl_display);
	if (self->output_power_mgr == NULL) {
		g_warning("output-power: manager not created");
		return;
	}
	self->output_power_set_mode.notify = on_output_power_set_mode;
	wl_signal_add(&self->output_power_mgr->events.set_mode,
	              &self->output_power_set_mode);
}

/**
 * gowl_output_power_finish:
 * @self: the compositor, during teardown
 *
 * Takes the listener off before the display destroys the global.
 */
void
gowl_output_power_finish(GowlCompositor *self)
{
	if (self->output_power_mgr == NULL)
		return;
	wl_list_remove(&self->output_power_set_mode.link);
	wl_list_init(&self->output_power_set_mode.link);
	self->output_power_mgr = NULL;
}
