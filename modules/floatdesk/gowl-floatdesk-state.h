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
 * gowl-floatdesk-state.h - the floating desktop's decisions, without a
 * compositor.
 *
 * Which workspaces are in floating mode, what each one was before, what
 * a given window should have done to it on a given arrange, and the
 * state file that carries all of it across a restart.  Nothing in here
 * touches wlroots or a GowlClient: a window is described by a
 * #GowlFloatdeskClientView, a plain struct of the flags that matter, so
 * every transition the mode makes can be tested by filling one in.
 */

#ifndef GOWL_FLOATDESK_STATE_H
#define GOWL_FLOATDESK_STATE_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GOWL_FLOATDESK_MAX_TAGS:
 *
 * Tags a mask can name.  Tag N is bit N-1 of a 32-bit mask.
 */
#define GOWL_FLOATDESK_MAX_TAGS (32)

/**
 * GOWL_FLOATDESK_LAYOUT:
 *
 * The name the mode registers in the layout cycle.  Picking it there
 * (Super+Shift+> on to it, or `set-layout desktop') enters the mode;
 * picking anything else leaves it.
 */
#define GOWL_FLOATDESK_LAYOUT "desktop"

/**
 * GOWL_FLOATDESK_STATE_VERSION:
 *
 * The state file's `version:'.  A file from a newer version is refused
 * rather than half-read.
 */
#define GOWL_FLOATDESK_STATE_VERSION (1)

/**
 * GowlFloatdeskMode:
 * @GOWL_FLOATDESK_MODE_UNSET: never set for this tag: it follows the
 *   global default
 * @GOWL_FLOATDESK_MODE_TILED: set to tile, whatever the default says
 * @GOWL_FLOATDESK_MODE_FLOATING: set to float, whatever the default says
 *
 * A tag's own setting.  Explicit settings always beat the default, so
 * turning on "float every workspace" never overrides a tag the user
 * tiled on purpose.
 */
typedef enum {
	GOWL_FLOATDESK_MODE_UNSET    = -1,
	GOWL_FLOATDESK_MODE_TILED    = 0,
	GOWL_FLOATDESK_MODE_FLOATING = 1
} GowlFloatdeskMode;

/**
 * GowlFloatdeskAction:
 * @GOWL_FLOATDESK_ACTION_NONE: leave the window alone
 * @GOWL_FLOATDESK_ACTION_FLOAT: mark it mode-floated and float it
 * @GOWL_FLOATDESK_ACTION_UNFLOAT: clear the mark and tile it again
 * @GOWL_FLOATDESK_ACTION_CLEAR: clear the mark only -- the window is
 *   already tiled (the user tiled it by hand), so there is nothing to
 *   undo but the claim
 *
 * What one arrange should do to one window.
 */
typedef enum {
	GOWL_FLOATDESK_ACTION_NONE = 0,
	GOWL_FLOATDESK_ACTION_FLOAT,
	GOWL_FLOATDESK_ACTION_UNFLOAT,
	GOWL_FLOATDESK_ACTION_CLEAR
} GowlFloatdeskAction;

/**
 * GowlFloatdeskClientView:
 * @tags: the window's tags; 0 for a minimized window or hidden overlay
 * @floating: it floats now, for whatever reason
 * @fullscreen: it is fullscreen
 * @mode_floated: the mode floated it (the flag on the client)
 * @overlay: a module-owned overlay (dropdown, scratchpad)
 * @unmanaged: an X11 override-redirect popup
 * @embedded: managed by the embedder (cmacs), not by layouts
 * @minimized: minimized
 *
 * The flags of a window that decide what the mode does to it.
 */
typedef struct {
	guint32  tags;
	gboolean floating;
	gboolean fullscreen;
	gboolean mode_floated;
	gboolean overlay;
	gboolean unmanaged;
	gboolean embedded;
	gboolean minimized;
} GowlFloatdeskClientView;

/**
 * GowlFloatdeskState:
 *
 * Opaque.  Per-tag modes, the global default, the layout each tag had
 * before it floated, and the dock settings the user changed at run
 * time -- everything the state file holds.
 */
typedef struct _GowlFloatdeskState GowlFloatdeskState;

/**
 * gowl_floatdesk_state_new:
 *
 * Every tag unset, the default tiled, no dock settings.
 *
 * Returns: (transfer full): a new state
 */
GowlFloatdeskState *gowl_floatdesk_state_new (void);

/**
 * gowl_floatdesk_state_free:
 * @self: (nullable) (transfer full): a state
 */
void gowl_floatdesk_state_free (GowlFloatdeskState *self);

/**
 * gowl_floatdesk_state_get_default_floating:
 * @self: a state
 *
 * Returns: %TRUE when tags with no setting of their own float
 */
gboolean gowl_floatdesk_state_get_default_floating (const GowlFloatdeskState *self);

/**
 * gowl_floatdesk_state_set_default_floating:
 * @self: a state
 * @floating: whether tags with no setting of their own float
 *
 * The "float every workspace" switch.  Tags set explicitly keep their
 * setting.
 */
void gowl_floatdesk_state_set_default_floating (GowlFloatdeskState *self,
                                                gboolean            floating);

/**
 * gowl_floatdesk_state_get_mode:
 * @self: a state
 * @tag: a tag index, 0-based
 *
 * Returns: the tag's own setting, %GOWL_FLOATDESK_MODE_UNSET if none or
 *   if @tag is out of range
 */
GowlFloatdeskMode gowl_floatdesk_state_get_mode (const GowlFloatdeskState *self,
                                                 guint                     tag);

/**
 * gowl_floatdesk_state_set_mode:
 * @self: a state
 * @tag: a tag index, 0-based
 * @mode: the tag's setting; %GOWL_FLOATDESK_MODE_UNSET returns it to
 *   the default
 */
void gowl_floatdesk_state_set_mode (GowlFloatdeskState *self,
                                    guint               tag,
                                    GowlFloatdeskMode   mode);

/**
 * gowl_floatdesk_state_tag_floating:
 * @self: a state
 * @tag: a tag index, 0-based
 *
 * Returns: %TRUE if @tag floats: its own setting, else the default
 */
gboolean gowl_floatdesk_state_tag_floating (const GowlFloatdeskState *self,
                                            guint                     tag);

/**
 * gowl_floatdesk_state_mask_floating:
 * @self: a state
 * @mask: a tag mask
 *
 * A mask floats when it is not empty and EVERY tag in it floats.  A
 * view of a floating tag and a tiled one tiles: the tiled tag's windows
 * have to go somewhere, and floating them would be the mode reaching
 * into a workspace it was never turned on for.  Likewise a window on
 * two tags floats only if both do.
 *
 * Returns: %TRUE if @mask is a floating workspace
 */
gboolean gowl_floatdesk_state_mask_floating (const GowlFloatdeskState *self,
                                             guint32                   mask);

/**
 * gowl_floatdesk_state_set_mask:
 * @self: a state
 * @mask: the tags to set
 * @floating: whether they float
 *
 * Sets every tag in @mask explicitly.
 */
void gowl_floatdesk_state_set_mask (GowlFloatdeskState *self,
                                    guint32             mask,
                                    gboolean            floating);

/**
 * gowl_floatdesk_state_get_previous_layout:
 * @self: a state
 * @tag: a tag index, 0-based
 *
 * Returns: (nullable): the layout @tag had before it was floated
 */
const gchar *gowl_floatdesk_state_get_previous_layout (const GowlFloatdeskState *self,
                                                       guint                     tag);

/**
 * gowl_floatdesk_state_set_previous_layout:
 * @self: a state
 * @tag: a tag index, 0-based
 * @layout: (nullable): the layout to go back to; the mode's own layout
 *   name is ignored, since going back to it is not leaving
 */
void gowl_floatdesk_state_set_previous_layout (GowlFloatdeskState *self,
                                               guint               tag,
                                               const gchar        *layout);

/**
 * gowl_floatdesk_state_previous_layout_for_mask:
 * @self: a state
 * @mask: a tag mask
 *
 * Returns: (nullable): the remembered layout of the lowest tag in @mask
 *   that has one
 */
const gchar *gowl_floatdesk_state_previous_layout_for_mask (const GowlFloatdeskState *self,
                                                            guint32                   mask);

/**
 * gowl_floatdesk_state_decide:
 * @self: a state
 * @client: the window
 *
 * The whole ownership rule, in one place:
 *
 * - a window on a floating workspace that does not float (and is not
 *   fullscreen) is floated and marked;
 * - a marked window on a workspace that tiles is tiled again and
 *   unmarked -- a fullscreen one too, so it comes back tiled;
 * - a marked window that already tiles (the user tiled it) loses the
 *   mark only;
 * - a window floating for its own reasons is never touched: no mark, no
 *   action, on any workspace;
 * - minimized windows, overlays, popups and embedded windows are never
 *   touched either, and a minimized one keeps its mark for the restore.
 *
 * Returns: what to do
 */
GowlFloatdeskAction gowl_floatdesk_state_decide (const GowlFloatdeskState      *self,
                                                 const GowlFloatdeskClientView *client);

/**
 * gowl_floatdesk_state_get_dock_position:
 * @self: a state
 *
 * Returns: (nullable): "bottom", "left", "right" or "top" if the user
 *   moved the dock at run time, else %NULL (the config decides)
 */
const gchar *gowl_floatdesk_state_get_dock_position (const GowlFloatdeskState *self);

/**
 * gowl_floatdesk_state_set_dock_position:
 * @self: a state
 * @position: (nullable): "bottom", "left", "right" or "top"; anything
 *   else is refused
 *
 * Returns: %TRUE if @position was valid (or %NULL)
 */
gboolean gowl_floatdesk_state_set_dock_position (GowlFloatdeskState *self,
                                                 const gchar        *position);

/**
 * gowl_floatdesk_state_get_dock_autohide:
 * @self: a state
 *
 * Returns: -1 when not set at run time, else 0 or 1
 */
gint gowl_floatdesk_state_get_dock_autohide (const GowlFloatdeskState *self);

/**
 * gowl_floatdesk_state_set_dock_autohide:
 * @self: a state
 * @autohide: -1 to forget, 0 or 1
 */
void gowl_floatdesk_state_set_dock_autohide (GowlFloatdeskState *self,
                                             gint                autohide);

/**
 * gowl_floatdesk_state_get_dock_pins:
 * @self: a state
 *
 * Returns: (transfer none) (nullable): the pinned app ids in dock order
 *   if the user changed them at run time, else %NULL
 */
const gchar *const *gowl_floatdesk_state_get_dock_pins (const GowlFloatdeskState *self);

/**
 * gowl_floatdesk_state_set_dock_pins:
 * @self: a state
 * @pins: (nullable) (array zero-terminated=1): app ids in order; %NULL
 *   forgets the run-time list
 */
void gowl_floatdesk_state_set_dock_pins (GowlFloatdeskState *self,
                                         const gchar *const *pins);

/**
 * gowl_floatdesk_state_to_yaml:
 * @self: a state
 *
 * Only what was set explicitly is written: an unset tag is omitted, so
 * changing the default later still reaches it.
 *
 * Returns: (transfer full): the state file's text
 */
gchar *gowl_floatdesk_state_to_yaml (const GowlFloatdeskState *self);

/**
 * gowl_floatdesk_state_load_yaml:
 * @self: a state, replaced on success and untouched on failure
 * @text: the state file's text
 * @error: return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean gowl_floatdesk_state_load_yaml (GowlFloatdeskState *self,
                                         const gchar        *text,
                                         GError            **error);

/**
 * gowl_floatdesk_state_save:
 * @self: a state
 * @path: the file
 * @error: return location for a #GError
 *
 * Writes atomically (g_file_set_contents), creating the directory.
 *
 * Returns: %TRUE on success
 */
gboolean gowl_floatdesk_state_save (const GowlFloatdeskState *self,
                                    const gchar              *path,
                                    GError                  **error);

/**
 * gowl_floatdesk_state_load:
 * @self: a state, replaced on success and untouched on failure
 * @path: the file
 * @error: return location for a #GError
 *
 * A missing file is not an error: there is simply nothing saved yet,
 * and @self is left as it is.
 *
 * Returns: %TRUE on success or a missing file
 */
gboolean gowl_floatdesk_state_load (GowlFloatdeskState *self,
                                    const gchar        *path,
                                    GError            **error);

/**
 * gowl_floatdesk_state_default_path:
 *
 * `$GOWL_FLOATDESK_STATE' if set, else
 * `$XDG_STATE_HOME/gowl/floatdesk.yaml'.
 *
 * Returns: (transfer full): the path
 */
gchar *gowl_floatdesk_state_default_path (void);

/**
 * gowl_floatdesk_place_cascade:
 * @area_x: the usable area
 * @area_y: the usable area
 * @area_w: the usable area
 * @area_h: the usable area
 * @width: the new window's frame width
 * @height: the new window's frame height
 * @have_last: %TRUE when @last_x/@last_y name the last window opened
 * @last_x: where the last window opened
 * @last_y: where the last window opened
 * @step: the cascade offset in pixels
 * @out_x: (out): where to put the new window
 * @out_y: (out): where to put the new window
 *
 * Centered for the first window; each next one @step further down and
 * right from the last, back to the centre once that would run off the
 * area.  Pure arithmetic, so it is tested without windows.
 */
void gowl_floatdesk_place_cascade (gint      area_x,
                                   gint      area_y,
                                   gint      area_w,
                                   gint      area_h,
                                   gint      width,
                                   gint      height,
                                   gboolean  have_last,
                                   gint      last_x,
                                   gint      last_y,
                                   gint      step,
                                   gint     *out_x,
                                   gint     *out_y);

/**
 * GowlFloatdeskSnap:
 * @GOWL_FLOATDESK_SNAP_NONE: dropped away from every edge
 * @GOWL_FLOATDESK_SNAP_LEFT: the left half
 * @GOWL_FLOATDESK_SNAP_RIGHT: the right half
 * @GOWL_FLOATDESK_SNAP_MAXIMIZE: the whole area
 *
 * Where a window dropped at a point snaps to.
 */
typedef enum {
	GOWL_FLOATDESK_SNAP_NONE = 0,
	GOWL_FLOATDESK_SNAP_LEFT,
	GOWL_FLOATDESK_SNAP_RIGHT,
	GOWL_FLOATDESK_SNAP_MAXIMIZE
} GowlFloatdeskSnap;

/**
 * gowl_floatdesk_snap_at:
 * @mon_x: the output, in layout coordinates
 * @mon_y: the output
 * @mon_w: the output
 * @mon_h: the output
 * @px: the pointer when the window was dropped
 * @py: the pointer
 * @threshold: how close to an edge counts as at it, in pixels
 *
 * Top edge first (a drop in a top corner maximizes), then left, then
 * right.  The bottom edge does nothing: that is where the dock is.
 *
 * Returns: the snap
 */
GowlFloatdeskSnap gowl_floatdesk_snap_at (gint    mon_x,
                                          gint    mon_y,
                                          gint    mon_w,
                                          gint    mon_h,
                                          gdouble px,
                                          gdouble py,
                                          gint    threshold);

G_END_DECLS

#endif /* GOWL_FLOATDESK_STATE_H */
