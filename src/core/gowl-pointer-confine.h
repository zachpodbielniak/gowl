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

#ifndef GOWL_POINTER_CONFINE_H
#define GOWL_POINTER_CONFINE_H

#include <glib.h>
#include <wlr/util/box.h>

G_BEGIN_DECLS

/*
 * Compositor-initiated pointer confinement: the decisions, without the
 * compositor.
 *
 * pointer-constraints-v1 confines the pointer only when a CLIENT asks.
 * A game under Proton does not ask, and a game being streamed loses the
 * stream the moment keyboard focus leaves it -- which, with sloppy
 * focus, is the moment the mouse drifts off its window.  So gowl can
 * confine the pointer to the focused window on its own say-so.
 *
 * Everything here is pure arithmetic and truth tables, so each rule can
 * be unit-tested without a running compositor.  gowl-compositor.c
 * gathers the inputs and acts on the answers.
 */

/**
 * GOWL_POINTER_CONFINE_DEFAULT_ESCAPE:
 *
 * The key that always lets go of a compositor confinement, whatever
 * else holds the keyboard.  Only consumed while a confinement is
 * active, so it does not take the key away from a bind that uses it
 * the rest of the time.
 */
#define GOWL_POINTER_CONFINE_DEFAULT_ESCAPE ("Super+Ctrl+Escape")

/**
 * GowlConfineFocusStep:
 * @GOWL_CONFINE_FOCUS_KEEP: leave the confinement as it is (or absent)
 * @GOWL_CONFINE_FOCUS_RELEASE: let go of the confinement
 * @GOWL_CONFINE_FOCUS_CONFINE: confine to the window now focused,
 *   letting go of any other first
 *
 * What a change of keyboard focus does to the confinement.  See
 * gowl_pointer_confine_focus_step().
 */
typedef enum {
	GOWL_CONFINE_FOCUS_KEEP = 0,
	GOWL_CONFINE_FOCUS_RELEASE,
	GOWL_CONFINE_FOCUS_CONFINE
} GowlConfineFocusStep;

/**
 * gowl_pointer_confine_compute_box:
 * @geom: the window's layout geometry, border included
 * @bw: the border width, which the pointer is kept off
 * @frame: (nullable): the frame as drawn, or %NULL or an empty box to
 *   ignore it.  A scrolling layout leaves @geom hanging off the screen
 *   on purpose and draws only the part that is on it; this is that part.
 * @output: (nullable): the box of the window's output, or %NULL or an
 *   empty box to ignore it
 * @out: (out caller-allocates): the box the pointer is kept in, in
 *   layout coordinates
 *
 * The region a confined pointer may move in: the window's surface (its
 * geometry inside the border), cut down to what is actually drawn and to
 * its own output.  Either cut is skipped when it would leave nothing: a
 * frame that does not overlap the geometry is one an animation has not
 * caught up with yet, and a window entirely off its output keeps its
 * own box rather than none.
 *
 * Returns: %TRUE if there is a non-empty box to confine to
 */
gboolean
gowl_pointer_confine_compute_box(
	const struct wlr_box *geom,
	guint                 bw,
	const struct wlr_box *frame,
	const struct wlr_box *output,
	struct wlr_box       *out
);

/**
 * gowl_pointer_confine_clamp:
 * @box: the confinement box, layout coordinates
 * @scale: the scale of the output the box is on; 0 or less means 1
 * @x: (inout): a proposed layout X, clamped in place
 * @y: (inout): a proposed layout Y, clamped in place
 *
 * Moves a proposed cursor position to the nearest point inside @box.
 *
 * The far edges are exclusive, and how far inside them the pointer may
 * go depends on @scale: the last reachable position is one PHYSICAL
 * pixel short of the edge, which is one logical pixel at scale 1 and
 * half of one at scale 2.  At HiDPI the right-most physical column of
 * the window is then still reachable -- an edge-scrolling game looks for
 * exactly that column -- and the pointer can still never land on the
 * first pixel of whatever is next to the window.
 *
 * Returns: %TRUE if the position was moved
 */
gboolean
gowl_pointer_confine_clamp(
	const struct wlr_box *box,
	gdouble               scale,
	gdouble              *x,
	gdouble              *y
);

/**
 * gowl_pointer_confine_contains:
 * @box: the confinement box, layout coordinates
 * @scale: the output scale, as for gowl_pointer_confine_clamp()
 * @x: a layout X
 * @y: a layout Y
 *
 * Whether a position is one gowl_pointer_confine_clamp() would leave
 * alone.
 *
 * Returns: %TRUE if (@x, @y) is inside @box
 */
gboolean
gowl_pointer_confine_contains(
	const struct wlr_box *box,
	gdouble               scale,
	gdouble               x,
	gdouble               y
);

/**
 * gowl_pointer_confine_focus_step:
 * @confined: a compositor confinement is active right now
 * @target_null: focus is being cleared rather than given
 * @same_client: the window gaining focus is the one confined to
 * @target_unmanaged: the window gaining focus is an X11
 *   override-redirect surface (a menu, a tooltip, a game's own popup)
 * @target_ruled: the window gaining focus matched a `confine-pointer'
 *   window rule
 * @target_suppressed: the user let go of that window's confinement by
 *   hand, and focus has not left it since
 *
 * What a change of keyboard focus does to the confinement.
 *
 * An override-redirect surface never moves it: such a window belongs to
 * whatever opened it, and a game's own popup taking the keyboard is not
 * the user leaving the game.  A ruled window is confined to as it takes
 * focus -- unless the user has just released it, in which case the rule
 * waits until focus has been somewhere else.  Any other focus change
 * releases, because a cursor held inside a window that no longer has
 * the keyboard is a trap rather than a feature.
 *
 * Returns: the #GowlConfineFocusStep to take
 */
GowlConfineFocusStep
gowl_pointer_confine_focus_step(
	gboolean confined,
	gboolean target_null,
	gboolean same_client,
	gboolean target_unmanaged,
	gboolean target_ruled,
	gboolean target_suppressed
);

/**
 * gowl_pointer_confine_pin_blocks:
 * @pinned: a confinement with focus pinning is active
 * @target_is_confined: the window that would take focus is the
 *   confined one
 * @same_process: the window that would take focus belongs to the
 *   confined window's process
 *
 * Whether focus pinning refuses an automatic focus change: sloppy
 * focus, a window mapping, an activation request.  A focus change the
 * user asks for -- a keybind, a click, IPC -- is never refused; it
 * releases the confinement instead.
 *
 * The confined program's own windows are let through.  A game's dialog
 * or launcher window taking the keyboard is the game talking to the
 * user, not another program stealing the stream.
 *
 * Returns: %TRUE if the focus change must not happen
 */
gboolean
gowl_pointer_confine_pin_blocks(
	gboolean pinned,
	gboolean target_is_confined,
	gboolean same_process
);

/**
 * gowl_pointer_confine_view_lost:
 * @tagset_then: the tags its output was viewing when the confinement
 *   began
 * @tagset_now: the tags that output is viewing now
 * @visible: the confined window is still visible on its output
 *
 * Whether a workspace change ends the confinement: the window was
 * hidden, or the output moved to other tags at all -- even with the
 * window still showing, as a sticky one would be, because switching
 * workspace is the user saying they are doing something else.
 *
 * Returns: %TRUE if the confinement must be released
 */
gboolean
gowl_pointer_confine_view_lost(
	guint32  tagset_then,
	guint32  tagset_now,
	gboolean visible
);

/**
 * gowl_pointer_confine_escape_matches:
 * @want_mods: the escape key's modifiers (#GowlKeyMod flags)
 * @want_sym: the escape key's keysym; 0 matches nothing
 * @mods: the modifiers held, NumLock and CapsLock already stripped
 * @sym: the keysym pressed
 *
 * Whether a key press is the confinement escape.  The modifiers must
 * match exactly, as for every other bind, so Super+Ctrl+Shift+Escape is
 * not Super+Ctrl+Escape.
 *
 * Returns: %TRUE if it is
 */
gboolean
gowl_pointer_confine_escape_matches(
	guint want_mods,
	guint want_sym,
	guint mods,
	guint sym
);

G_END_DECLS

#endif /* GOWL_POINTER_CONFINE_H */
