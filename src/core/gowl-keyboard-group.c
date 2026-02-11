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
 * GowlKeyboardGroup:
 *
 * Groups all keyboards into a single logical device so they share
 * the same XKB state.  Holds repeat rate/delay configuration.
 * The struct definition lives in gowl-core-private.h.
 */

G_DEFINE_FINAL_TYPE(GowlKeyboardGroup, gowl_keyboard_group, G_TYPE_OBJECT)

/* Signal identifiers */
enum {
	SIGNAL_KEY,
	SIGNAL_MODIFIERS,
	N_SIGNALS
};

static guint kb_signals[N_SIGNALS] = { 0, };

/* --- GObject lifecycle --- */

static void
gowl_keyboard_group_dispose(GObject *object)
{
	G_OBJECT_CLASS(gowl_keyboard_group_parent_class)->dispose(object);
}

static void
gowl_keyboard_group_finalize(GObject *object)
{
	GowlKeyboardGroup *self;

	self = GOWL_KEYBOARD_GROUP(object);

	/* Release the XKB context if this object owns one */
	if (self->xkb_context != NULL)
		xkb_context_unref((struct xkb_context *)self->xkb_context);

	G_OBJECT_CLASS(gowl_keyboard_group_parent_class)->finalize(object);
}

/* --- class / instance init --- */

static void
gowl_keyboard_group_class_init(GowlKeyboardGroupClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->dispose  = gowl_keyboard_group_dispose;
	object_class->finalize = gowl_keyboard_group_finalize;

	/**
	 * GowlKeyboardGroup::key:
	 * @group: the #GowlKeyboardGroup that emitted the signal
	 * @keycode: the raw hardware keycode
	 * @state: key state (pressed / released)
	 * @mods: active modifier bitmask
	 *
	 * Emitted when a key event is received from any grouped keyboard.
	 */
	kb_signals[SIGNAL_KEY] =
		g_signal_new("key",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             3,
		             G_TYPE_UINT,
		             G_TYPE_UINT,
		             G_TYPE_UINT);

	/**
	 * GowlKeyboardGroup::modifiers:
	 * @group: the #GowlKeyboardGroup that emitted the signal
	 *
	 * Emitted when the modifier state changes.
	 */
	kb_signals[SIGNAL_MODIFIERS] =
		g_signal_new("modifiers",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);
}

static void
gowl_keyboard_group_init(GowlKeyboardGroup *self)
{
	self->wlr_group    = NULL;
	self->xkb_context  = NULL;
	self->repeat_rate  = 25;
	self->repeat_delay = 600;
}

/* --- Public API --- */

/**
 * gowl_keyboard_group_new:
 *
 * Creates a new #GowlKeyboardGroup with default repeat settings.
 *
 * Returns: (transfer full): a newly allocated #GowlKeyboardGroup
 */
GowlKeyboardGroup *
gowl_keyboard_group_new(void)
{
	return (GowlKeyboardGroup *)g_object_new(GOWL_TYPE_KEYBOARD_GROUP, NULL);
}

/**
 * gowl_keyboard_group_get_repeat_rate:
 * @self: a #GowlKeyboardGroup
 *
 * Returns the key repeat rate in characters per second.
 *
 * Returns: the repeat rate
 */
gint
gowl_keyboard_group_get_repeat_rate(GowlKeyboardGroup *self)
{
	g_return_val_if_fail(GOWL_IS_KEYBOARD_GROUP(self), 25);

	return self->repeat_rate;
}

/**
 * gowl_keyboard_group_set_repeat_rate:
 * @self: a #GowlKeyboardGroup
 * @rate: the new repeat rate (characters per second)
 *
 * Sets the key repeat rate.
 */
void
gowl_keyboard_group_set_repeat_rate(
	GowlKeyboardGroup *self,
	gint                rate
){
	g_return_if_fail(GOWL_IS_KEYBOARD_GROUP(self));

	self->repeat_rate = rate;
}

/**
 * gowl_keyboard_group_get_repeat_delay:
 * @self: a #GowlKeyboardGroup
 *
 * Returns the delay before key repeat begins, in milliseconds.
 *
 * Returns: the repeat delay in ms
 */
gint
gowl_keyboard_group_get_repeat_delay(GowlKeyboardGroup *self)
{
	g_return_val_if_fail(GOWL_IS_KEYBOARD_GROUP(self), 600);

	return self->repeat_delay;
}

/**
 * gowl_keyboard_group_set_repeat_delay:
 * @self: a #GowlKeyboardGroup
 * @delay: the new repeat delay in milliseconds
 *
 * Sets the delay before key repeat begins.
 */
void
gowl_keyboard_group_set_repeat_delay(
	GowlKeyboardGroup *self,
	gint                delay
){
	g_return_if_fail(GOWL_IS_KEYBOARD_GROUP(self));

	self->repeat_delay = delay;
}
