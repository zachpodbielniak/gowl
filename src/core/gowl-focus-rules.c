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
 * Pure decision logic for keyboard focus and client close routing.
 * Deliberately free of wlroots / GObject types so it can be
 * unit-tested directly (see tests/test-focus-rules.c).  The wl_listener
 * and seat glue that drives these lives in gowl-compositor.c and
 * gowl-client.c.
 *
 * Both rules exist because their absence produced field crashes:
 *
 *   * gowl_close_route_for() -- the KILL_CLIENT keybind used to send
 *     the XDG close unconditionally, which NULL-derefs inside wlroots
 *     for an X11 client and SIGSEGVs the compositor.
 *
 *   * gowl_focus_decide() -- keyboard-interactive layer surfaces
 *     (wofi, on-screen keyboards) had no grab, so any later focus
 *     change silently stole their keyboard and left them visible but
 *     deaf.
 */

#include "core/gowl-focus-rules.h"

GowlCloseRoute
gowl_close_route_for(gboolean has_xwayland_surface, gboolean has_xdg_toplevel)
{
	/* X11 first: an xwayland surface is authoritative even in the
	 * impossible case where both pointers are set, because the XDG
	 * close would be the crashing path. */
	if (has_xwayland_surface)
		return GOWL_CLOSE_ROUTE_XWAYLAND;

	if (has_xdg_toplevel)
		return GOWL_CLOSE_ROUTE_XDG;

	/* Neither: the client is between map and unmap and has no
	 * shell surface left to close.  Silently do nothing. */
	return GOWL_CLOSE_ROUTE_NONE;
}

gboolean
gowl_layer_takes_keyboard(
	gboolean session_locked,
	gboolean mapped,
	guint32  keyboard_interactive,
	gint     layer
){
	/* The lock surface owns input while locked; nothing else. */
	if (session_locked)
		return FALSE;

	if (!mapped)
		return FALSE;

	/* 0 = none.  Both 1 (exclusive) and 2 (on-demand) ask for the
	 * keyboard, matching dwl's `!current.keyboard_interactive'
	 * test -- a surface that asks on-demand still needs the grab
	 * held for as long as it is up. */
	if (keyboard_interactive == 0)
		return FALSE;

	/* Background and bottom layers are wallpapers and docks; they
	 * never take the keyboard from a window. */
	return layer >= GOWL_LAYER_KEYBOARD_MIN;
}

GowlFocusDecision
gowl_focus_decide(
	gboolean session_locked,
	gboolean target_embedded,
	gboolean layer_grab_active,
	gboolean exclusive_client_active,
	gboolean target_is_exclusive_client
){
	/* Escalating authority.  Order matters: the reason reported for
	 * a refusal is the highest-authority guard that fired, which is
	 * what a bug report needs to see first. */
	if (session_locked)
		return GOWL_FOCUS_DENY_LOCKED;

	if (target_embedded)
		return GOWL_FOCUS_DENY_EMBEDDED;

	/* A launcher holding the keyboard outranks any window, including
	 * a focus clear (target NULL).  Without this a stray refocus --
	 * from an arrange, a pointer enter, or a host embedder syncing
	 * its own idea of focus -- leaves the launcher on screen and
	 * deaf, with no way to type into it or dismiss it. */
	if (layer_grab_active)
		return GOWL_FOCUS_DENY_LAYER_GRAB;

	/* An X11 override-redirect popup keeps its grab until it
	 * unmaps; focusing the popup itself just re-asserts. */
	if (exclusive_client_active && !target_is_exclusive_client)
		return GOWL_FOCUS_DENY_EXCLUSIVE_CLIENT;

	return GOWL_FOCUS_ALLOW;
}

const char *
gowl_focus_decision_to_string(GowlFocusDecision decision)
{
	switch (decision) {
	case GOWL_FOCUS_ALLOW:
		return "allow";
	case GOWL_FOCUS_DENY_LOCKED:
		return "denied: session locked";
	case GOWL_FOCUS_DENY_EMBEDDED:
		return "denied: target is an embedded client";
	case GOWL_FOCUS_DENY_LAYER_GRAB:
		return "denied: layer surface holds the keyboard";
	case GOWL_FOCUS_DENY_EXCLUSIVE_CLIENT:
		return "denied: X11 popup holds an exclusive grab";
	}

	return "denied: unknown";
}
