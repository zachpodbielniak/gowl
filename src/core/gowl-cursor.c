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
 * GowlCursor:
 *
 * Wraps the wlroots cursor and xcursor manager.  Tracks the current
 * interaction mode (normal, move, resize), the grabbed client, and
 * the grab origin so the compositor can compute deltas during
 * interactive move/resize operations.
 * The struct definition lives in gowl-core-private.h.
 */

G_DEFINE_FINAL_TYPE(GowlCursor, gowl_cursor, G_TYPE_OBJECT)

/* Signal identifiers */
enum {
	SIGNAL_MOTION,
	SIGNAL_BUTTON,
	SIGNAL_AXIS,
	N_SIGNALS
};

static guint cursor_signals[N_SIGNALS] = { 0, };

/* --- GObject lifecycle --- */

static void
gowl_cursor_dispose(GObject *object)
{
	GowlCursor *self;

	self = GOWL_CURSOR(object);
	self->grabbed_client = NULL;

	G_OBJECT_CLASS(gowl_cursor_parent_class)->dispose(object);
}

static void
gowl_cursor_finalize(GObject *object)
{
	G_OBJECT_CLASS(gowl_cursor_parent_class)->finalize(object);
}

/* --- class / instance init --- */

static void
gowl_cursor_class_init(GowlCursorClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->dispose  = gowl_cursor_dispose;
	object_class->finalize = gowl_cursor_finalize;

	/**
	 * GowlCursor::motion:
	 * @cursor: the #GowlCursor that emitted the signal
	 * @x: the absolute x coordinate
	 * @y: the absolute y coordinate
	 *
	 * Emitted when the cursor moves.
	 */
	cursor_signals[SIGNAL_MOTION] =
		g_signal_new("motion",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             2,
		             G_TYPE_DOUBLE,
		             G_TYPE_DOUBLE);

	/**
	 * GowlCursor::button:
	 * @cursor: the #GowlCursor that emitted the signal
	 * @button: the button code
	 * @state: the button state (pressed / released)
	 *
	 * Emitted when a mouse button event occurs.
	 */
	cursor_signals[SIGNAL_BUTTON] =
		g_signal_new("button",
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
	 * GowlCursor::axis:
	 * @cursor: the #GowlCursor that emitted the signal
	 * @orientation: the scroll axis (vertical / horizontal)
	 * @delta: the scroll amount
	 *
	 * Emitted when a scroll / axis event occurs.
	 */
	cursor_signals[SIGNAL_AXIS] =
		g_signal_new("axis",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             2,
		             G_TYPE_UINT,
		             G_TYPE_DOUBLE);
}

static void
gowl_cursor_init(GowlCursor *self)
{
	self->wlr_cursor       = NULL;
	self->xcursor_manager  = NULL;
	self->mode             = 0;
	self->grabbed_client   = NULL;
	self->grab_x           = 0.0;
	self->grab_y           = 0.0;
	self->grab_width       = 0;
	self->grab_height      = 0;
}

/* --- Public API --- */

/**
 * gowl_cursor_new:
 *
 * Creates a new #GowlCursor with default state.
 *
 * Returns: (transfer full): a newly allocated #GowlCursor
 */
GowlCursor *
gowl_cursor_new(void)
{
	return (GowlCursor *)g_object_new(GOWL_TYPE_CURSOR, NULL);
}

/**
 * gowl_cursor_get_mode:
 * @self: a #GowlCursor
 *
 * Returns the current cursor interaction mode.
 *
 * Returns: the mode value
 */
gint
gowl_cursor_get_mode(GowlCursor *self)
{
	g_return_val_if_fail(GOWL_IS_CURSOR(self), 0);

	return self->mode;
}

/**
 * gowl_cursor_set_mode:
 * @self: a #GowlCursor
 * @mode: the new cursor interaction mode
 *
 * Sets the cursor interaction mode (e.g. normal, move, resize).
 */
void
gowl_cursor_set_mode(
	GowlCursor *self,
	gint        mode
){
	g_return_if_fail(GOWL_IS_CURSOR(self));

	self->mode = mode;
}
