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
