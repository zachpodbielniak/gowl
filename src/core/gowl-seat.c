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
 * GowlSeat:
 *
 * Represents the Wayland seat (input device group).  Holds a reference
 * to the focused client, the keyboard group, and cursor state.
 * The struct definition lives in gowl-core-private.h.
 */

G_DEFINE_FINAL_TYPE(GowlSeat, gowl_seat, G_TYPE_OBJECT)

/* Signal identifiers */
enum {
	SIGNAL_FOCUS_CHANGED,
	N_SIGNALS
};

static guint seat_signals[N_SIGNALS] = { 0, };

/* --- GObject lifecycle --- */

static void
gowl_seat_dispose(GObject *object)
{
	GowlSeat *self;

	self = GOWL_SEAT(object);

	self->focused_client  = NULL;
	self->keyboard_group  = NULL;
	self->cursor          = NULL;

	G_OBJECT_CLASS(gowl_seat_parent_class)->dispose(object);
}

static void
gowl_seat_finalize(GObject *object)
{
	G_OBJECT_CLASS(gowl_seat_parent_class)->finalize(object);
}

/* --- class / instance init --- */

static void
gowl_seat_class_init(GowlSeatClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->dispose  = gowl_seat_dispose;
	object_class->finalize = gowl_seat_finalize;

	/**
	 * GowlSeat::focus-changed:
	 * @seat: the #GowlSeat that emitted the signal
	 *
	 * Emitted when the focused client changes.
	 */
	seat_signals[SIGNAL_FOCUS_CHANGED] =
		g_signal_new("focus-changed",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);
}

static void
gowl_seat_init(GowlSeat *self)
{
	self->wlr_seat        = NULL;
	self->focused_client  = NULL;
	self->keyboard_group  = NULL;
	self->cursor          = NULL;
}

/* --- Public API --- */

/**
 * gowl_seat_new:
 *
 * Creates a new #GowlSeat.
 *
 * Returns: (transfer full): a newly allocated #GowlSeat
 */
GowlSeat *
gowl_seat_new(void)
{
	return (GowlSeat *)g_object_new(GOWL_TYPE_SEAT, NULL);
}

/**
 * gowl_seat_get_focused_client:
 * @self: a #GowlSeat
 *
 * Returns the client that currently holds keyboard focus.
 *
 * Returns: (transfer none) (nullable): the focused #GowlClient, or %NULL
 */
gpointer
gowl_seat_get_focused_client(GowlSeat *self)
{
	g_return_val_if_fail(GOWL_IS_SEAT(self), NULL);

	return self->focused_client;
}

/**
 * gowl_seat_set_focused_client:
 * @self: a #GowlSeat
 * @client: (nullable): the #GowlClient to focus, or %NULL to unfocus
 *
 * Sets the focused client and emits "focus-changed" if it changed.
 */
void
gowl_seat_set_focused_client(
	GowlSeat *self,
	gpointer  client
){
	g_return_if_fail(GOWL_IS_SEAT(self));

	if (self->focused_client != client) {
		self->focused_client = client;
		g_signal_emit(self, seat_signals[SIGNAL_FOCUS_CHANGED], 0);
	}
}
