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
