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
 * GowlKeyRoute:
 * @GOWL_KEY_ROUTE_LOCAL: this machine's business -- configured binds,
 *   module binds, the embedder intercept, then the focused client
 * @GOWL_KEY_ROUTE_CAPTURE: the OTHER machine's business: an input
 *   capture is active, so the key goes to the sink and nothing here
 *   runs on it
 * @GOWL_KEY_ROUTE_BREAK_CAPTURE: Super+Escape while captured -- the
 *   escape hatch, consumed here and sent nowhere
 *
 * Where a key belongs while a software KVM (deskflow) may be holding
 * the input.
 *
 * This is an ordering question and it was wrong for as long as the
 * order lived only in the sequence of `if' statements: the capture
 * diversion sat at the BOTTOM behind `!handled', so a key matching a
 * local bind was claimed locally and never sent.  With both machines
 * running gowl -- which share every shortcut -- Super+2 switched a tag
 * HERE while the pointer was on the remote screen, and the remote
 * host could not be driven by the keyboard at all.
 */
typedef enum {
	GOWL_KEY_ROUTE_LOCAL = 0,
	GOWL_KEY_ROUTE_CAPTURE,
	GOWL_KEY_ROUTE_BREAK_CAPTURE
} GowlKeyRoute;

/**
 * gowl_key_route:
 * @capture_active: an input-capture session currently holds the input
 * @synthetic: the key was injected rather than typed
 * @pressed: a press rather than a release
 * @logo: the Super modifier is held
 * @escape: one of the key's keysyms is Escape
 *
 * Where this key goes.  A captured keyboard belongs to the machine the
 * pointer is on, with exactly one exception: Super+Escape breaks the
 * capture, which is the only guaranteed way back when a KVM client
 * wedges it.  The exception is tested FIRST for that reason.
 *
 * An injected key is never diverted: it arrived from the sink, and
 * handing it back is a loop.
 *
 * Returns: the route
 */
GowlKeyRoute gowl_key_route (gboolean capture_active,
                             gboolean synthetic,
                             gboolean pressed,
                             gboolean logo,
                             gboolean escape);

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
 * @GOWL_FOCUS_DENY_PASSIVE_POPUP: the target is an X11
 *   override-redirect surface that does not want the keyboard -- a
 *   menu, a tooltip, a combo dropdown, a drag icon.  Focusing one
 *   deactivates the window it belongs to, and that window dismisses
 *   the menu.
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
	GOWL_FOCUS_DENY_PASSIVE_POPUP,
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
 * @target_passive_popup: %TRUE when the target is an X11
 *   override-redirect surface that does not want focus -- see
 *   wlr_xwayland_surface_override_redirect_wants_focus(); %FALSE for
 *   every Wayland client, every managed X11 window, a popup that does
 *   want it, and a %NULL target
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
 * a passive X11 popup is never focusable, a layer grab outranks an
 * ordinary window, and an X11 popup grab outranks the window beneath
 * it.  Note that a focus *clear*
 * (target %NULL) is refused by the grab guards too -- clearing focus
 * out from under a launcher leaves it visible, on top, and deaf.
 *
 * Returns: %GOWL_FOCUS_ALLOW, or the #GowlFocusDecision naming the
 *          guard that refused.
 */
GowlFocusDecision gowl_focus_decide(gboolean session_locked,
                                    gboolean target_embedded,
                                    gboolean target_passive_popup,
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

/**
 * gowl_focus_stack_accepts:
 * @focused_group: overlay group of the client a focus-stack step starts
 *   from, 0 for none
 * @candidate_group: overlay group of a client the step reaches
 *
 * Whether a focus-stack step (Super+j / Super+k) may land on the
 * candidate.  Windows cycle only among windows of their own overlay
 * group.  While the scratchpad is up its windows cycle among themselves
 * instead of reaching the tiles underneath -- which would roll it away,
 * since focus leaving the scratchpad hides it -- and ordinary windows
 * never step up into it.  Group 0 holds every ordinary window and the
 * ungrouped overlays such as the dropdown, so their behaviour does not
 * change.
 *
 * Returns: %TRUE if the step may focus the candidate
 */
gboolean gowl_focus_stack_accepts(guint focused_group,
                                  guint candidate_group);

G_END_DECLS

#endif /* GOWL_FOCUS_RULES_H */
