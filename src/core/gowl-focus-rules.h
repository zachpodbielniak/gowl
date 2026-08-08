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

#ifndef GOWL_FOCUS_RULES_H
#define GOWL_FOCUS_RULES_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GowlCloseRoute:
 * @GOWL_CLOSE_ROUTE_NONE: the client has no closable shell surface
 *   (unmapped, or mid-teardown) -- do nothing
 * @GOWL_CLOSE_ROUTE_XDG: send wlr_xdg_toplevel_send_close()
 * @GOWL_CLOSE_ROUTE_XWAYLAND: send wlr_xwayland_surface_close()
 *
 * Which protocol a close request must travel over for a given client.
 */
typedef enum {
	GOWL_CLOSE_ROUTE_NONE = 0,
	GOWL_CLOSE_ROUTE_XDG,
	GOWL_CLOSE_ROUTE_XWAYLAND
} GowlCloseRoute;

/**
 * GowlFocusDecision:
 * @GOWL_FOCUS_ALLOW: the focus change may proceed
 * @GOWL_FOCUS_DENY_LOCKED: the session is locked; only the lock
 *   surface may hold the keyboard
 * @GOWL_FOCUS_DENY_EMBEDDED: the target is an embedded client, which
 *   is driven by the host (Emacs) and never takes keyboard focus
 * @GOWL_FOCUS_DENY_LAYER_GRAB: a keyboard-interactive layer surface
 *   (a launcher, an on-screen keyboard) holds an exclusive grab
 * @GOWL_FOCUS_DENY_EXCLUSIVE_CLIENT: an X11 override-redirect popup
 *   holds an exclusive grab
 *
 * The outcome of gowl_focus_decide(), carrying *why* a focus change
 * was refused so the refusal is greppable in a debug log rather than
 * a silent early return.
 */
typedef enum {
	GOWL_FOCUS_ALLOW = 0,
	GOWL_FOCUS_DENY_LOCKED,
	GOWL_FOCUS_DENY_EMBEDDED,
	GOWL_FOCUS_DENY_LAYER_GRAB,
	GOWL_FOCUS_DENY_EXCLUSIVE_CLIENT
} GowlFocusDecision;

/**
 * GOWL_LAYER_KEYBOARD_MIN:
 *
 * Lowest zwlr_layer_shell_v1 layer whose surfaces are allowed to take
 * keyboard focus, i.e. %ZWLR_LAYER_SHELL_V1_LAYER_TOP.  Spelled as a
 * bare integer so this header stays free of protocol headers; the
 * compositor carries a %G_STATIC_ASSERT tying the two together, so a
 * protocol renumbering breaks the build rather than the behaviour.
 */
#define GOWL_LAYER_KEYBOARD_MIN (2)

/**
 * gowl_close_route_for:
 * @has_xwayland_surface: %TRUE when the client carries a non-%NULL
 *   `struct wlr_xwayland_surface *` -- i.e. it is an X11 client
 * @has_xdg_toplevel: %TRUE when the client carries a non-%NULL
 *   `struct wlr_xdg_toplevel *` -- i.e. it is a native Wayland client
 *
 * Pick the protocol a close request must be sent over.  The two
 * pointers are mutually exclusive in practice: an X11 client has only
 * @has_xwayland_surface, a Wayland client only @has_xdg_toplevel, and
 * a client caught between map and unmap can have neither.  X11 wins
 * when (impossibly) both are set, matching gowl_client_close().
 *
 * Sending the XDG close to an X11 client dereferences a %NULL
 * `wlr_xdg_toplevel` inside wlroots and takes the compositor -- and
 * therefore, under `emacs --gowl`, the whole session -- down with it.
 *
 * Returns: the #GowlCloseRoute to use.
 */
GowlCloseRoute gowl_close_route_for(gboolean has_xwayland_surface,
                                    gboolean has_xdg_toplevel);

/**
 * gowl_layer_takes_keyboard:
 * @session_locked: %TRUE while a session-lock client owns the outputs
 * @mapped: %TRUE when the layer surface is currently mapped
 * @keyboard_interactive: the surface's committed
 *   `keyboard_interactive` value (0 = none, 1 = exclusive,
 *   2 = on-demand); any non-zero value asks for the keyboard
 * @layer: the surface's committed zwlr layer (0 = background,
 *   1 = bottom, 2 = top, 3 = overlay)
 *
 * Decide whether a layer-shell surface should be granted -- and then
 * keep -- keyboard focus.  Only mapped, keyboard-interactive surfaces
 * at %GOWL_LAYER_KEYBOARD_MIN or above qualify, so wallpapers and
 * bars can never take the keyboard away from a window.  A locked
 * session disqualifies everything: the lock surface owns input.
 *
 * Returns: %TRUE if the surface owns the keyboard.
 */
gboolean gowl_layer_takes_keyboard(gboolean session_locked,
                                   gboolean mapped,
                                   guint32  keyboard_interactive,
                                   gint     layer);

/**
 * gowl_focus_decide:
 * @session_locked: %TRUE while a session-lock client owns the outputs
 * @target_embedded: %TRUE when the focus target is an embedded client
 *   (host-driven, e.g. a cmacs `--gowl` app buffer).  Must be %FALSE
 *   when the target is %NULL (a focus *clear*)
 * @layer_grab_active: %TRUE when a keyboard-interactive layer surface
 *   currently owns the keyboard -- see gowl_layer_takes_keyboard()
 * @exclusive_client_active: %TRUE when an X11 override-redirect popup
 *   holds an exclusive grab and still wants focus
 * @target_is_exclusive_client: %TRUE when the focus target *is* that
 *   popup, which is allowed to re-assert its own grab
 *
 * The single gate every keyboard-focus change must pass, whether it
 * originates from a keybind, a pointer enter, an arrange, a client
 * map, or an out-of-tree embedder poking the seat directly.
 *
 * Guards are evaluated in escalating order of authority: a locked
 * session outranks everything, embedded clients are never focusable,
 * a layer grab outranks an ordinary window, and an X11 popup grab
 * outranks the window beneath it.  Note that a focus *clear*
 * (target %NULL) is refused by the grab guards too -- clearing focus
 * out from under a launcher leaves it visible, on top, and deaf.
 *
 * Returns: %GOWL_FOCUS_ALLOW, or the #GowlFocusDecision naming the
 *          guard that refused.
 */
GowlFocusDecision gowl_focus_decide(gboolean session_locked,
                                    gboolean target_embedded,
                                    gboolean layer_grab_active,
                                    gboolean exclusive_client_active,
                                    gboolean target_is_exclusive_client);

/**
 * gowl_focus_decision_to_string:
 * @decision: a #GowlFocusDecision
 *
 * Returns: (transfer none): a stable, human-readable name for
 *          @decision, for debug logging.  Never %NULL.
 */
const char *gowl_focus_decision_to_string(GowlFocusDecision decision);

G_END_DECLS

#endif /* GOWL_FOCUS_RULES_H */
