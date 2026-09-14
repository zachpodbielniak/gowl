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

#include "gowl-core-private.h"
#include "boxed/gowl-output-mode.h"
#include "boxed/gowl-geometry.h"
#include <wlr/render/color.h>
#include <drm_fourcc.h>
#include <string.h>

/**
 * GowlMonitor:
 *
 * Represents a physical output / monitor.  Holds per-output tag state,
 * layout configuration, and wlroots output/scene objects.
 * The struct definition lives in gowl-core-private.h.
 */

G_DEFINE_FINAL_TYPE(GowlMonitor, gowl_monitor, G_TYPE_OBJECT)

/* Signal identifiers */
enum {
	SIGNAL_TAG_CHANGED,
	SIGNAL_LAYOUT_CHANGED,
	SIGNAL_FRAME,
	SIGNAL_DESTROY,
	SIGNAL_USABLE_AREA_CHANGED,
	N_SIGNALS
};

static guint monitor_signals[N_SIGNALS] = { 0, };

/* --- GObject lifecycle --- */

static void
gowl_monitor_dispose(GObject *object)
{
	G_OBJECT_CLASS(gowl_monitor_parent_class)->dispose(object);
}

static void
gowl_monitor_finalize(GObject *object)
{
	GowlMonitor *self;

	self = GOWL_MONITOR(object);

	g_free(self->layout_symbol);
	g_free(self->layout_name);

	G_OBJECT_CLASS(gowl_monitor_parent_class)->finalize(object);
}

/* --- class / instance init --- */

static void
gowl_monitor_class_init(GowlMonitorClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->dispose  = gowl_monitor_dispose;
	object_class->finalize = gowl_monitor_finalize;

	/**
	 * GowlMonitor::tag-changed:
	 * @monitor: the #GowlMonitor that emitted the signal
	 * @old_tags: the previous tag bitmask
	 * @new_tags: the new tag bitmask
	 *
	 * Emitted when the active tag set changes.
	 */
	monitor_signals[SIGNAL_TAG_CHANGED] =
		g_signal_new("tag-changed",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             2,
		             G_TYPE_UINT,
		             G_TYPE_UINT);

	/**
	 * GowlMonitor::layout-changed:
	 * @monitor: the #GowlMonitor that emitted the signal
	 *
	 * Emitted when the active layout changes on this monitor.
	 */
	monitor_signals[SIGNAL_LAYOUT_CHANGED] =
		g_signal_new("layout-changed",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlMonitor::frame:
	 * @monitor: the #GowlMonitor that emitted the signal
	 *
	 * Emitted on each output frame callback.
	 */
	monitor_signals[SIGNAL_FRAME] =
		g_signal_new("frame",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlMonitor::destroy:
	 * @monitor: the #GowlMonitor that emitted the signal
	 *
	 * Emitted when the underlying output is destroyed (disconnected).
	 */
	monitor_signals[SIGNAL_DESTROY] =
		g_signal_new("destroy",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlMonitor::usable-area-changed:
	 * @monitor: the #GowlMonitor whose canvas shrank or grew
	 * @old_area: (transfer none): the previous usable rectangle,
	 *   monitor-local coordinates
	 * @new_area: (transfer none): the new usable rectangle
	 *
	 * Emitted whenever the monitor's "window area" (the rectangle
	 * inside which tiled clients are arranged, i.e. the monitor
	 * rectangle minus any wlr_layer_surface exclusive_zones such
	 * as a bar or waybar) changes.  Integration layers — notably
	 * cmacs `--gowl` — subscribe to this signal to reflow embedded
	 * app buffer geometry when a layer surface maps or unmaps,
	 * replacing emskin's racy "compute offset from
	 * surface-height - frame-height" approach.
	 *
	 * Both #GowlGeometry arguments are owned by the emitter; do not
	 * free them.  Copy them via gowl_geometry_copy() if you need to
	 * keep them past the signal callback.
	 */
	monitor_signals[SIGNAL_USABLE_AREA_CHANGED] =
		g_signal_new("usable-area-changed",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             2,
		             GOWL_TYPE_GEOMETRY,
		             GOWL_TYPE_GEOMETRY);
}

/**
 * gowl_monitor_emit_usable_area_changed:
 * @self: a #GowlMonitor
 * @old_x: previous usable area x (monitor-local)
 * @old_y: previous usable area y
 * @old_w: previous usable area width
 * @old_h: previous usable area height
 * @new_x: new usable area x
 * @new_y: new usable area y
 * @new_w: new usable area width
 * @new_h: new usable area height
 *
 * Convenience wrapper that allocates temporary #GowlGeometry boxed
 * values and emits the `usable-area-changed` signal.  Intended to be
 * called by gowl_compositor_arrangelayers after the window-area box
 * actually changes.
 */
void
gowl_monitor_emit_usable_area_changed(GowlMonitor *self,
                                       gint old_x, gint old_y,
                                       gint old_w, gint old_h,
                                       gint new_x, gint new_y,
                                       gint new_w, gint new_h)
{
	GowlGeometry *old_geom;
	GowlGeometry *new_geom;

	g_return_if_fail(GOWL_IS_MONITOR(self));

	old_geom = gowl_geometry_new(old_x, old_y, old_w, old_h);
	new_geom = gowl_geometry_new(new_x, new_y, new_w, new_h);

	g_signal_emit(self, monitor_signals[SIGNAL_USABLE_AREA_CHANGED],
	              0, old_geom, new_geom);

	gowl_geometry_free(old_geom);
	gowl_geometry_free(new_geom);
}

static void
gowl_monitor_init(GowlMonitor *self)
{
	self->wlr_output    = NULL;
	self->scene_output  = NULL;
	self->fullscreen_bg = NULL;
	memset(&self->m, 0, sizeof(self->m));
	memset(&self->w, 0, sizeof(self->w));
	self->tagset[0]     = 1;
	self->tagset[1]     = 1;
	self->seltags       = 0;
	self->sellt         = 0;
	self->nmaster       = 1;
	self->mfact         = 0.55;
	self->vsplit        = FALSE;
	self->layout_symbol = g_strdup("[]=");
	self->compositor    = NULL;
}

/* --- Public API --- */

/**
 * gowl_monitor_new:
 *
 * Creates a new #GowlMonitor with default tag and layout state.
 *
 * Returns: (transfer full): a newly allocated #GowlMonitor
 */
GowlMonitor *
gowl_monitor_new(void)
{
	return (GowlMonitor *)g_object_new(GOWL_TYPE_MONITOR, NULL);
}

/**
 * gowl_monitor_get_tags:
 * @self: a #GowlMonitor
 *
 * Returns the currently active tag bitmask from the selected tag set.
 *
 * Returns: the active tag bitmask
 */
guint32
gowl_monitor_get_tags(GowlMonitor *self)
{
	g_return_val_if_fail(GOWL_IS_MONITOR(self), 0);

	return self->tagset[self->seltags];
}

/**
 * gowl_monitor_set_tags:
 * @self: a #GowlMonitor
 * @tags: the new tag bitmask
 *
 * Replaces the active tag set with @tags and emits "tag-changed".
 */
void
gowl_monitor_set_tags(
	GowlMonitor *self,
	guint32      tags
){
	guint32 old_tags;

	g_return_if_fail(GOWL_IS_MONITOR(self));

	old_tags = self->tagset[self->seltags];
	self->tagset[self->seltags] = tags;

	if (old_tags != tags)
		g_signal_emit(self, monitor_signals[SIGNAL_TAG_CHANGED], 0,
		              old_tags, tags);
}

/**
 * gowl_monitor_toggle_tag:
 * @self: a #GowlMonitor
 * @tag: the tag bit to toggle
 *
 * Toggles a single tag bit in the active tag set and emits
 * "tag-changed" if the result differs.
 */
void
gowl_monitor_toggle_tag(
	GowlMonitor *self,
	guint32      tag
){
	guint32 old_tags;
	guint32 new_tags;

	g_return_if_fail(GOWL_IS_MONITOR(self));

	old_tags = self->tagset[self->seltags];
	new_tags = old_tags ^ tag;

	/* refuse to leave no tags visible */
	if (new_tags == 0)
		return;

	self->tagset[self->seltags] = new_tags;
	g_signal_emit(self, monitor_signals[SIGNAL_TAG_CHANGED], 0,
	              old_tags, new_tags);
}

/**
 * gowl_monitor_get_mfact:
 * @self: a #GowlMonitor
 *
 * Returns the master area factor for this monitor.
 *
 * Returns: the master factor (0.0 - 1.0)
 */
gdouble
gowl_monitor_get_mfact(GowlMonitor *self)
{
	g_return_val_if_fail(GOWL_IS_MONITOR(self), 0.55);

	return self->mfact;
}

/**
 * gowl_monitor_set_mfact:
 * @self: a #GowlMonitor
 * @mfact: the new master factor (clamped to 0.05 - 0.95)
 *
 * Sets the master area factor, clamping to a sane range.
 */
void
gowl_monitor_set_mfact(
	GowlMonitor *self,
	gdouble      mfact
){
	g_return_if_fail(GOWL_IS_MONITOR(self));

	if (mfact < 0.05)
		mfact = 0.05;
	if (mfact > 0.95)
		mfact = 0.95;

	self->mfact = mfact;
}

/**
 * gowl_monitor_get_nmaster:
 * @self: a #GowlMonitor
 *
 * Returns the number of windows in the master area.
 *
 * Returns: the master count
 */
gint
gowl_monitor_get_nmaster(GowlMonitor *self)
{
	g_return_val_if_fail(GOWL_IS_MONITOR(self), 1);

	return self->nmaster;
}

gpointer
gowl_monitor_get_compositor(GowlMonitor *self)
{
	g_return_val_if_fail(GOWL_IS_MONITOR(self), NULL);

	return self->compositor;
}

/**
 * gowl_monitor_set_nmaster:
 * @self: a #GowlMonitor
 * @nmaster: the new master count (minimum 0)
 *
 * Sets the number of windows in the master area.
 */
void
gowl_monitor_set_nmaster(
	GowlMonitor *self,
	gint         nmaster
){
	g_return_if_fail(GOWL_IS_MONITOR(self));

	if (nmaster < 0)
		nmaster = 0;

	self->nmaster = nmaster;
}

/**
 * gowl_monitor_get_vsplit:
 * @self: a #GowlMonitor
 *
 * Returns whether the tile layout uses the vsplit (master row on top,
 * stack row on bottom) orientation on this monitor.
 *
 * Returns: TRUE if vsplit is enabled
 */
gboolean
gowl_monitor_get_vsplit(GowlMonitor *self)
{
	g_return_val_if_fail(GOWL_IS_MONITOR(self), FALSE);

	return self->vsplit;
}

/**
 * gowl_monitor_set_vsplit:
 * @self: a #GowlMonitor
 * @vsplit: TRUE for vsplit (master on top), FALSE for normal (master left)
 *
 * Sets the tile layout split orientation.  Does not re-arrange; the caller
 * is responsible for calling gowl_compositor_arrange() afterwards.
 */
void
gowl_monitor_set_vsplit(
	GowlMonitor *self,
	gboolean     vsplit
){
	g_return_if_fail(GOWL_IS_MONITOR(self));

	self->vsplit = vsplit;
}

/**
 * gowl_monitor_get_layout_symbol:
 * @self: a #GowlMonitor
 *
 * Returns the display symbol for the currently active layout.
 *
 * Returns: (transfer none) (nullable): the layout symbol string
 */
const gchar *
gowl_monitor_get_layout_symbol(GowlMonitor *self)
{
	g_return_val_if_fail(GOWL_IS_MONITOR(self), NULL);

	return self->layout_symbol;
}

/**
 * gowl_monitor_get_name:
 * @self: a #GowlMonitor
 *
 * Returns the output name from the underlying wlr_output
 * (e.g. "eDP-1", "HDMI-A-1").
 *
 * Returns: (transfer none) (nullable): the output name string
 */
const gchar *
gowl_monitor_get_name(GowlMonitor *self)
{
	g_return_val_if_fail(GOWL_IS_MONITOR(self), NULL);

	if (self->wlr_output == NULL)
		return NULL;

	return self->wlr_output->name;
}

/**
 * gowl_monitor_get_geometry:
 * @self: a #GowlMonitor
 * @x: (out) (nullable): return location for x
 * @y: (out) (nullable): return location for y
 * @width: (out) (nullable): return location for width
 * @height: (out) (nullable): return location for height
 *
 * Returns the monitor's layout-relative geometry (the full
 * output area, not the window area).
 */
void
gowl_monitor_get_geometry(
	GowlMonitor *self,
	gint        *x,
	gint        *y,
	gint        *width,
	gint        *height
){
	g_return_if_fail(GOWL_IS_MONITOR(self));

	if (x != NULL)      *x      = self->m.x;
	if (y != NULL)      *y      = self->m.y;
	if (width != NULL)  *width  = self->m.width;
	if (height != NULL) *height = self->m.height;
}

/**
 * gowl_monitor_get_window_area:
 * @self: a #GowlMonitor
 * @x: (out) (nullable): return location for x
 * @y: (out) (nullable): return location for y
 * @width: (out) (nullable): return location for width
 * @height: (out) (nullable): return location for height
 *
 * Returns the usable window area after subtracting exclusive
 * zones (layer-shell surfaces, bar height, etc.).
 */
void
gowl_monitor_get_window_area(
	GowlMonitor *self,
	gint        *x,
	gint        *y,
	gint        *width,
	gint        *height
){
	g_return_if_fail(GOWL_IS_MONITOR(self));

	if (x != NULL)      *x      = self->w.x;
	if (y != NULL)      *y      = self->w.y;
	if (width != NULL)  *width  = self->w.width;
	if (height != NULL) *height = self->w.height;
}

/**
 * gowl_monitor_get_wlr_output:
 * @self: a #GowlMonitor
 *
 * Returns the underlying wlr_output for this monitor.
 *
 * Returns: (transfer none) (nullable): the wlr_output, or %NULL
 */
struct wlr_output *
gowl_monitor_get_wlr_output(GowlMonitor *self)
{
	g_return_val_if_fail(GOWL_IS_MONITOR(self), NULL);

	return self->wlr_output;
}

/**
 * gowl_monitor_get_scene_output:
 * @self: a #GowlMonitor
 *
 * Returns the wlr_scene_output for this monitor.
 *
 * Returns: (transfer none) (nullable): the wlr_scene_output, or %NULL
 */
struct wlr_scene_output *
gowl_monitor_get_scene_output(GowlMonitor *self)
{
	g_return_val_if_fail(GOWL_IS_MONITOR(self), NULL);

	return self->scene_output;
}

/* ── Output mode / configuration API ──────────────────────────────── */

/**
 * gowl_monitor_get_modes:
 * @self: a #GowlMonitor
 *
 * Returns a list of available output modes.
 *
 * Returns: (transfer full) (element-type GowlOutputMode): available modes
 */
GList *
gowl_monitor_get_modes(GowlMonitor *self)
{
	GList *result = NULL;
	struct wlr_output_mode *mode;

	g_return_val_if_fail(GOWL_IS_MONITOR(self), NULL);
	g_return_val_if_fail(self->wlr_output != NULL, NULL);

	wl_list_for_each(mode, &self->wlr_output->modes, link) {
		result = g_list_prepend(result,
			gowl_output_mode_new(mode->width, mode->height,
			                     mode->refresh));
	}

	return g_list_reverse(result);
}

/**
 * gowl_monitor_get_current_mode:
 * @self: a #GowlMonitor
 *
 * Returns the currently active output mode.
 *
 * Returns: (transfer full) (nullable): the current mode, or %NULL
 */
GowlOutputMode *
gowl_monitor_get_current_mode(GowlMonitor *self)
{
	struct wlr_output_mode *mode;

	g_return_val_if_fail(GOWL_IS_MONITOR(self), NULL);
	g_return_val_if_fail(self->wlr_output != NULL, NULL);

	mode = self->wlr_output->current_mode;
	if (mode == NULL)
		return NULL;

	return gowl_output_mode_new(mode->width, mode->height,
	                            mode->refresh);
}

/**
 * gowl_monitor_set_mode:
 * @self: a #GowlMonitor
 * @width: horizontal resolution
 * @height: vertical resolution
 * @refresh_mhz: refresh rate in millihertz
 *
 * Sets the output mode.  Finds a matching advertised mode first,
 * falls back to a custom mode if no exact match.
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_monitor_set_mode(
	GowlMonitor *self,
	gint         width,
	gint         height,
	gint         refresh_mhz
){
	struct wlr_output_state state;
	struct wlr_output_mode *mode;
	struct wlr_output_mode *match = NULL;
	gboolean ok;

	g_return_val_if_fail(GOWL_IS_MONITOR(self), FALSE);
	g_return_val_if_fail(self->wlr_output != NULL, FALSE);

	/* Find an advertised mode that matches the request.
	 * When refresh_mhz is 0, match any refresh rate. */
	wl_list_for_each(mode, &self->wlr_output->modes, link) {
		if (mode->width == width && mode->height == height
		    && (refresh_mhz == 0 || mode->refresh == refresh_mhz)) {
			match = mode;
			break;
		}
	}

	wlr_output_state_init(&state);

	if (match != NULL)
		wlr_output_state_set_mode(&state, match);
	else
		wlr_output_state_set_custom_mode(&state, width, height,
		                                 refresh_mhz);

	/* The Wayland backend (nested compositors) requires enabled
	 * to be set alongside mode changes for the commit to succeed. */
	wlr_output_state_set_enabled(&state, TRUE);

	ok = wlr_output_commit_state(self->wlr_output, &state);
	wlr_output_state_finish(&state);

	/* Safety net: if on_layout_change() did not fire synchronously,
	 * query the layout box and update geometry ourselves. */
	if (ok && self->compositor != NULL) {
		struct wlr_box box;
		wlr_output_layout_get_box(
			self->compositor->output_layout,
			self->wlr_output, &box);
		if (!wlr_box_empty(&box)) {
			self->m = box;
			self->w = self->m;
		}
		gowl_compositor_arrange(self->compositor, self);
	}

	return ok;
}

/**
 * gowl_monitor_get_position:
 * @self: a #GowlMonitor
 * @x: (out) (nullable): return location for x
 * @y: (out) (nullable): return location for y
 *
 * Returns the layout-relative position.
 */
void
gowl_monitor_get_position(
	GowlMonitor *self,
	gint        *x,
	gint        *y
){
	g_return_if_fail(GOWL_IS_MONITOR(self));

	if (x != NULL) *x = self->m.x;
	if (y != NULL) *y = self->m.y;
}

/**
 * gowl_monitor_set_position:
 * @self: a #GowlMonitor
 * @x: x coordinate in layout space
 * @y: y coordinate in layout space
 *
 * Sets the monitor position.  Switches from auto to manual layout.
 * The on_layout_change callback updates m.x/m.y automatically.
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_monitor_set_position(
	GowlMonitor *self,
	gint         x,
	gint         y
){
	g_return_val_if_fail(GOWL_IS_MONITOR(self), FALSE);
	g_return_val_if_fail(self->wlr_output != NULL, FALSE);
	g_return_val_if_fail(self->compositor != NULL, FALSE);

	wlr_output_layout_add(self->compositor->output_layout,
	                       self->wlr_output, x, y);

	/* Update geometry and re-tile immediately.  on_layout_change
	 * may not fire synchronously in all backends, and even when
	 * it does, arrangelayers may skip arrange() if only the
	 * position changed (same dimensions).  Mirror the pattern
	 * used by gowl_monitor_set_mode(). */
	{
		struct wlr_box box;
		wlr_output_layout_get_box(self->compositor->output_layout,
		                          self->wlr_output, &box);
		if (!wlr_box_empty(&box)) {
			self->m = box;
			self->w = self->m;
		}
	}
	gowl_compositor_arrangelayers(self->compositor, self);

	return TRUE;
}

/**
 * gowl_monitor_get_enabled:
 * @self: a #GowlMonitor
 *
 * Returns: %TRUE if the output is enabled
 */
gboolean
gowl_monitor_get_enabled(GowlMonitor *self)
{
	g_return_val_if_fail(GOWL_IS_MONITOR(self), FALSE);
	g_return_val_if_fail(self->wlr_output != NULL, FALSE);

	return self->wlr_output->enabled;
}

/**
 * gowl_monitor_set_enabled:
 * @self: a #GowlMonitor
 * @enabled: whether to enable the output
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_monitor_set_enabled(
	GowlMonitor *self,
	gboolean     enabled
){
	struct wlr_output_state state;
	gboolean ok;

	g_return_val_if_fail(GOWL_IS_MONITOR(self), FALSE);
	g_return_val_if_fail(self->wlr_output != NULL, FALSE);

	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, enabled);

	/* Powering an output back on needs a mode: the DRM backend rejects
	 * an enable commit on a connector with no active mode (the case
	 * after it was disabled, e.g. for a closed laptop lid).  Re-apply
	 * the preferred mode; outputs with no mode list (nested Wayland)
	 * keep the size from their first configure. */
	if (enabled) {
		struct wlr_output_mode *mode;

		mode = wlr_output_preferred_mode(self->wlr_output);
		if (mode != NULL)
			wlr_output_state_set_mode(&state, mode);
	}

	ok = wlr_output_commit_state(self->wlr_output, &state);
	wlr_output_state_finish(&state);

	return ok;
}

/**
 * gowl_monitor_get_scale:
 * @self: a #GowlMonitor
 *
 * Returns: the output scale factor
 */
gdouble
gowl_monitor_get_scale(GowlMonitor *self)
{
	g_return_val_if_fail(GOWL_IS_MONITOR(self), 1.0);
	g_return_val_if_fail(self->wlr_output != NULL, 1.0);

	return (gdouble)self->wlr_output->scale;
}

/* ── HDR ─────────────────────────────────────────────────────────────
 *
 * An HDR output is three things at once: BT.2020 primaries, the ST.2084
 * PQ transfer function, and ten bits per channel.  The first two are
 * the image description the backend hands to KMS (the Colorspace and
 * HDR_OUTPUT_METADATA properties); the third is the format the
 * compositor renders into.  Committing the description without the
 * format gives a picture that is technically HDR and visibly banded in
 * every dark gradient, so they go in one atomic commit and fail
 * together.
 *
 * The luminance numbers below are the defaults a display uses when the
 * metadata says nothing useful: 0.005 to 1000 cd/m² is the range a
 * mid-range HDR10 panel actually reaches.  A player that knows its
 * content's mastering display describes it through
 * wp-color-management-v1 and that wins for its own surface.
 */

/* Rec. ITU-R BT.2020 primaries and the D65 white point. */
static const struct wlr_color_primaries gowl_bt2020_primaries = {
	.red   = { .x = 0.708f, .y = 0.292f },
	.green = { .x = 0.170f, .y = 0.797f },
	.blue  = { .x = 0.131f, .y = 0.046f },
	.white = { .x = 0.3127f, .y = 0.3290f },
};

/**
 * gowl_monitor_hdr_display_capable:
 * @self: a #GowlMonitor
 *
 * Whether the DISPLAY end of the chain can do HDR -- that is, whether it
 * advertises BT.2020 and the PQ transfer function.
 *
 * Half of gowl_monitor_supports_hdr(), and the half a person can do
 * something about: it is a property of the whole chain rather than of
 * the panel, so an HDR monitor on a cable or at a refresh rate that
 * cannot carry ten bits reports neither.
 *
 * It exists separately so that a refusal can say WHICH end refused.
 * Reporting "this output does not offer BT.2020 and PQ" when the output
 * offers both and the renderer is the problem sends somebody to check
 * their cable for an afternoon.
 *
 * Returns: %TRUE if the display advertises BT.2020 and PQ
 */
gboolean
gowl_monitor_hdr_display_capable(GowlMonitor *self)
{
	g_return_val_if_fail(GOWL_IS_MONITOR(self), FALSE);

	if (self->wlr_output == NULL)
		return FALSE;
	return (self->wlr_output->supported_primaries
	        & WLR_COLOR_NAMED_PRIMARIES_BT2020) != 0
	    && (self->wlr_output->supported_transfer_functions
	        & WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ) != 0;
}

/**
 * gowl_monitor_supports_hdr:
 * @self: a #GowlMonitor
 *
 * Whether HDR can be driven on this output correctly.
 *
 * Two questions, and the second is the one that surprises people.  The
 * DISPLAY has to advertise BT.2020 and PQ -- that is a property of the
 * whole chain, and a cable or a refresh rate that cannot carry ten bits
 * reports neither.  And the RENDERER has to be able to convert colour,
 * because a PQ signal carries absolute luminance and every SDR surface
 * on the screen has to be re-encoded into it.
 *
 * wlroots implements that conversion in its Vulkan renderer and nowhere
 * else.  Under the GLES2 renderer gowl uses for its visual effects, the
 * scene passes sRGB code values straight into the PQ signal: ordinary
 * white becomes a request for 10,000 cd/m2, the panel runs at its peak
 * -- which is most of what HDR costs in battery -- and a client that
 * DOES honour wp-color-management-v1 encodes itself correctly at the
 * 203 cd/m2 reference white and so appears dim beside everything that
 * did not.  All three of those read as separate bugs and are one.
 *
 * `hdr-unmanaged: true' says "give it to me anyway".
 *
 * Returns: %TRUE if HDR can be switched on for this output
 */
gboolean
gowl_monitor_supports_hdr(GowlMonitor *self)
{
	GowlConfig *config;

	g_return_val_if_fail(GOWL_IS_MONITOR(self), FALSE);

	if (!gowl_monitor_hdr_display_capable(self))
		return FALSE;

	if (self->compositor == NULL)
		return TRUE;
	config = gowl_compositor_get_config(self->compositor);
	if (config != NULL && gowl_config_get_hdr_unmanaged(config))
		return TRUE;
	if (gowl_renderer_can_color_manage(self->compositor->renderer))
		return TRUE;

	if (!self->hdr_renderer_warned) {
		self->hdr_renderer_warned = TRUE;
		g_message("%s advertises BT.2020 and PQ, but this renderer "
		          "cannot convert colour -- HDR is not offered, because "
		          "SDR windows would reach the panel unconverted inside "
		          "a PQ signal.  WLR_RENDERER=vulkan can convert it (and "
		          "turns off every visual effect); `hdr-unmanaged: true' "
		          "takes it uncorrected.",
		          gowl_monitor_get_name(self));
	}
	return FALSE;
}

/**
 * gowl_monitor_get_hdr:
 * @self: a #GowlMonitor
 *
 * Returns: %TRUE if the output is in HDR
 */
gboolean
gowl_monitor_get_hdr(GowlMonitor *self)
{
	g_return_val_if_fail(GOWL_IS_MONITOR(self), FALSE);
	return self->hdr_enabled;
}

/**
 * gowl_monitor_get_edid_hdr:
 * @self: a #GowlMonitor
 *
 * What the display said about its own HDR range, read out of its EDID
 * the first time it is asked for and kept.
 *
 * Returns: (transfer none) (nullable): the parsed metadata, or %NULL
 *          where there is no EDID to read (nested, headless)
 */
const GowlEdidHdr *
gowl_monitor_get_edid_hdr(GowlMonitor *self)
{
	g_return_val_if_fail(GOWL_IS_MONITOR(self), NULL);

	if (!self->edid_read) {
		self->edid_read = TRUE;
		if (!gowl_edid_read_connector(gowl_monitor_get_name(self),
		                              &self->edid_hdr))
			memset(&self->edid_hdr, 0, sizeof self->edid_hdr);
	}
	/* An EDID with nothing to say about HDR still counts as read; the
	 * zeroed luminances are what tell the caller to keep its own
	 * defaults. */
	return &self->edid_hdr;
}

/**
 * gowl_monitor_set_hdr:
 * @self: a #GowlMonitor
 * @enable: %TRUE for BT.2020 + PQ at 10 bits
 *
 * Returns: %TRUE if the output is now in the requested state
 */
gboolean
gowl_monitor_set_hdr(
	GowlMonitor *self,
	gboolean     enable
){
	struct wlr_output_state state;
	struct wlr_output_image_description desc;
	guint32 chosen_format = 0;
	gsize tested = 0;
	gboolean ok;

	g_return_val_if_fail(GOWL_IS_MONITOR(self), FALSE);
	g_return_val_if_fail(self->wlr_output != NULL, FALSE);

	enable = enable ? TRUE : FALSE;
	if (self->hdr_enabled == enable)
		return TRUE;
	if (enable && !gowl_monitor_supports_hdr(self)) {
		g_message("%s cannot do HDR: the output advertises no "
		          "BT.2020 + PQ", gowl_monitor_get_name(self));
		return FALSE;
	}

	wlr_output_state_init(&state);
	if (enable) {
		const GowlEdidHdr *edid = gowl_monitor_get_edid_hdr(self);

		memset(&desc, 0, sizeof desc);
		desc.primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020;
		desc.transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ;

		/*
		 * The mastering display: what the content is declared to have
		 * been graded on.  A compositor has no single honest answer
		 * for a desktop, so it describes THIS display -- which says
		 * to the sink "what I am sending already fits you, leave it
		 * alone".  Claiming a brighter mastering display than the
		 * panel has is what makes it tone-map, and tone-mapping a
		 * desktop that was never brighter than the panel only dims
		 * it.
		 *
		 * The panel's own primaries come from wlroots (out of the
		 * EDID's chromaticity); its luminance range does not, so it
		 * is parsed here.  A display that states neither keeps the
		 * conventional 1000 cd/m² grade, which is what most HDR
		 * content is actually mastered at.
		 */
		desc.mastering_display_primaries =
			self->wlr_output->default_primaries != NULL
			? *self->wlr_output->default_primaries
			: gowl_bt2020_primaries;

		desc.mastering_luminance.min = 0.005;
		desc.mastering_luminance.max = 1000.0;
		desc.max_cll = 1000.0;
		desc.max_fall = 400.0;
		if (edid != NULL) {
			if (edid->max_luminance > 0.0) {
				desc.mastering_luminance.max = edid->max_luminance;
				desc.max_cll = edid->max_luminance;
			}
			if (edid->min_luminance > 0.0)
				desc.mastering_luminance.min = edid->min_luminance;
			if (edid->max_frame_average > 0.0)
				desc.max_fall = edid->max_frame_average;
			g_debug("%s: mastering %.0f cd/m² peak, %.3f min, "
			        "%.0f frame-average (%s)",
			        gowl_monitor_get_name(self),
			        desc.mastering_luminance.max,
			        desc.mastering_luminance.min, desc.max_fall,
			        edid->max_luminance > 0.0
			        ? "from the display's EDID"
			        : "the EDID states no luminance");
		}
		if (!wlr_output_state_set_image_description(&state, &desc)) {
			g_warning("%s refused the HDR image description",
			          gowl_monitor_get_name(self));
			wlr_output_state_finish(&state);
			return FALSE;
		}
		/* Remember what it was rendering at, so turning HDR off does
		 * not leave a 10-bit format behind on a display that only
		 * wanted it for HDR. */
		self->hdr_prev_render_format = self->wlr_output->render_format;

		/*
		 * Which 10-bit format, asked rather than assumed.
		 *
		 * This used to name DRM_FORMAT_XRGB2101010 and commit.  When
		 * a driver would not take that one the whole commit failed,
		 * the image description went with it, and the only thing
		 * anybody saw was "the output refused the change" -- with no
		 * way to tell a display that cannot do HDR from one that
		 * simply wanted the other byte order.  Channel order is a
		 * property of the plane, not of HDR.
		 *
		 * The other half is that ten bits per channel is a change of
		 * pipe depth, and on a DisplayPort link that means retraining
		 * it -- which wlroots will not do unless the state says
		 * disruption is acceptable.  Without allow_reconfiguration
		 * the atomic test simply fails, which is what
		 * "Swapchain for output 'eDP-1' failed test" was: not a
		 * display that cannot do HDR, a link that was never allowed
		 * to be retrained for it.  A brief black flash while it
		 * retrains is what every television does when it switches
		 * into HDR.
		 *
		 * So: test each candidate against the backend, in order, and
		 * take the first that would be accepted.
		 * wlr_output_test_state() is a test-only atomic commit, so
		 * the ones that fail cost nothing and are not seen.
		 *
		 * The last candidate is 0, meaning "leave the format alone":
		 * PQ at 8 bits per channel bands visibly in gradients, but it
		 * is HDR, and a panel that will not give 10 bits at its
		 * current mode -- no DSC and not enough link for it -- can
		 * still show it.  That is a worse picture offered knowingly,
		 * with a warning, rather than a feature that silently does
		 * nothing.
		 */
		{
			/*
			 * Ordered by preference, most desirable first: ten bits
			 * without disturbing the link, ten bits with a retrain,
			 * and finally whatever the output is already using.
			 */
			static const struct {
				guint32  format;
				gboolean reconfigure;
			} attempts[] = {
				{ DRM_FORMAT_XRGB2101010, FALSE },
				{ DRM_FORMAT_XBGR2101010, FALSE },
				{ DRM_FORMAT_XRGB2101010, TRUE  },
				{ DRM_FORMAT_XBGR2101010, TRUE  },
				{ 0,                      FALSE },
				{ 0,                      TRUE  }
			};
			gsize i;
			gboolean found = FALSE;
			GString *refused = g_string_new(NULL);

			for (i = 0; i < G_N_ELEMENTS(attempts); i++) {
				const gchar *what = attempts[i].format == 0
					? "the current format (8-bit)"
					: (attempts[i].format == DRM_FORMAT_XRGB2101010
					   ? "XRGB2101010" : "XBGR2101010");

				if (attempts[i].format != 0)
					wlr_output_state_set_render_format(&state,
						attempts[i].format);
				else
					/* `committed' is a public field; the bit is
					 * one this loop set itself through the
					 * setter, so clearing it is well defined and
					 * is the only way to say "do not touch the
					 * format" once it has been said once. */
					state.committed &=
						~(guint32)WLR_OUTPUT_STATE_RENDER_FORMAT;
				state.allow_reconfiguration =
					attempts[i].reconfigure ? true : false;

				if (wlr_output_test_state(self->wlr_output,
				                          &state)) {
					chosen_format = attempts[i].format;
					found = TRUE;
					break;
				}
				tested++;
				g_string_append_printf(refused, "%s%s%s",
					refused->len > 0 ? ", " : "", what,
					attempts[i].reconfigure
					? " (with a retrain)" : "");
			}
			if (!found) {
				/* At warning level with the whole list, because
				 * this is the message somebody has to act on:
				 * "the output refused the change" on its own says
				 * nothing about which part was refused. */
				g_warning("%s: the display advertises BT.2020 + PQ "
				          "but the driver refused every way of "
				          "sending it -- tried %s",
				          gowl_monitor_get_name(self), refused->str);
				g_string_free(refused, TRUE);
				wlr_output_state_finish(&state);
				return FALSE;
			}
			if (tested > 0)
				g_message("%s: HDR accepted after %s was refused",
				          gowl_monitor_get_name(self),
				          refused->str);
			g_string_free(refused, TRUE);
		}
	} else {
		wlr_output_state_set_image_description(&state, NULL);
		wlr_output_state_set_render_format(&state,
			self->hdr_prev_render_format != 0
			? self->hdr_prev_render_format : DRM_FORMAT_XRGB8888);
		/*
		 * Going back to eight bits is a change of pipe depth as much
		 * as going up was, so it needs the same permission to retrain
		 * the link -- and being unable to leave HDR is worse than
		 * being unable to enter it.  Asked for only if the
		 * undisturbed version is refused.
		 */
		if (!wlr_output_test_state(self->wlr_output, &state))
			state.allow_reconfiguration = true;
	}

	ok = wlr_output_commit_state(self->wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		g_warning("%s refused to switch HDR %s",
		          gowl_monitor_get_name(self), enable ? "on" : "off");
		return FALSE;
	}

	self->hdr_enabled = enable;
	self->hdr_format = enable ? chosen_format : 0;
	if (enable && chosen_format == 0)
		g_warning("%s: HDR on, but only at 8 bits per channel -- the "
		          "driver refused every 10-bit format, so gradients "
		          "will band", gowl_monitor_get_name(self));
	else
		g_message("%s: HDR %s", gowl_monitor_get_name(self),
		          enable ? "on (BT.2020, PQ, 10-bit)" : "off");
	if (self->compositor != NULL)
		g_signal_emit_by_name(self->compositor, "monitor-hdr-changed",
		                      self, enable);
	/* The whole output has to be redrawn in the new colour space. */
	wlr_output_schedule_frame(self->wlr_output);
	return TRUE;
}

/**
 * gowl_monitor_set_scale:
 * @self: a #GowlMonitor
 * @scale: the scale factor
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_monitor_set_scale(
	GowlMonitor *self,
	gdouble      scale
){
	struct wlr_output_state state;
	gboolean ok;

	g_return_val_if_fail(GOWL_IS_MONITOR(self), FALSE);
	g_return_val_if_fail(self->wlr_output != NULL, FALSE);

	wlr_output_state_init(&state);
	wlr_output_state_set_scale(&state, (float)scale);
	ok = wlr_output_commit_state(self->wlr_output, &state);
	wlr_output_state_finish(&state);

	/* Safety net: if on_layout_change() did not fire synchronously,
	 * query the layout box and update geometry ourselves. */
	if (ok && self->compositor != NULL) {
		struct wlr_box box;
		wlr_output_layout_get_box(
			self->compositor->output_layout,
			self->wlr_output, &box);
		if (!wlr_box_empty(&box)) {
			self->m = box;
			self->w = self->m;
		}
		gowl_compositor_arrange(self->compositor, self);
	}

	return ok;
}

/**
 * gowl_monitor_get_transform:
 * @self: a #GowlMonitor
 *
 * Returns: the transform value (matches enum wl_output_transform)
 */
gint
gowl_monitor_get_transform(GowlMonitor *self)
{
	g_return_val_if_fail(GOWL_IS_MONITOR(self), 0);
	g_return_val_if_fail(self->wlr_output != NULL, 0);

	return (gint)self->wlr_output->transform;
}

/**
 * gowl_monitor_set_transform:
 * @self: a #GowlMonitor
 * @transform: transform value (0-7)
 *
 * Returns: %TRUE on success
 */
gboolean
gowl_monitor_set_transform(
	GowlMonitor *self,
	gint         transform
){
	struct wlr_output_state state;
	gboolean ok;

	g_return_val_if_fail(GOWL_IS_MONITOR(self), FALSE);
	g_return_val_if_fail(self->wlr_output != NULL, FALSE);
	g_return_val_if_fail(transform >= 0 && transform <= 7, FALSE);

	wlr_output_state_init(&state);
	wlr_output_state_set_transform(&state,
	                               (enum wl_output_transform)transform);
	ok = wlr_output_commit_state(self->wlr_output, &state);
	wlr_output_state_finish(&state);

	/* Safety net: if on_layout_change() did not fire synchronously,
	 * query the layout box and update geometry ourselves. */
	if (ok && self->compositor != NULL) {
		struct wlr_box box;
		wlr_output_layout_get_box(
			self->compositor->output_layout,
			self->wlr_output, &box);
		if (!wlr_box_empty(&box)) {
			self->m = box;
			self->w = self->m;
		}
		gowl_compositor_arrange(self->compositor, self);
	}

	return ok;
}

/**
 * gowl_monitor_get_layer_surfaces:
 * @self: a #GowlMonitor
 *
 * Returns the list of layer surfaces on this monitor.
 *
 * Returns: (transfer none) (element-type GowlLayerSurface): the list
 */
GList *
gowl_monitor_get_layer_surfaces(GowlMonitor *self)
{
	g_return_val_if_fail(GOWL_IS_MONITOR(self), NULL);

	return self->layer_surfaces;
}
