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

#include "barkit/gowl-bar-toast.h"

/**
 * SECTION:gowl-bar-toast
 * @title: GowlBarToast
 * @short_description: the on-screen notification cards the bar owns
 *
 * cmacs already keeps notification history in an Org buffer; what a
 * gowl session lacked was the transient card.  A toast here is
 * deliberately thin --- summary, body, urgency, and a target --- and
 * its whole reason to exist beyond "show text" is
 * gowl_bar_toast_set_panel(): a notification that can hand you the
 * dropdown that fixes it.
 */

#define GOWL_BAR_TOAST_DEFAULT_LIMIT 5

struct _GowlBarToast {
	guint                id;
	gchar               *summary;
	gchar               *body;
	gchar               *icon;
	gchar               *app;
	gchar               *panel;
	gchar               *command;
	gchar               *hint;
	GowlBarToastUrgency  urgency;
	gint                 timeout_ms;
	gint64               created_us;
};

G_DEFINE_BOXED_TYPE(GowlBarToast, gowl_bar_toast,
                    gowl_bar_toast_copy, gowl_bar_toast_free)

/**
 * gowl_bar_toast_new:
 * @summary: the headline
 * @body: (nullable): the detail line
 *
 * Returns: (transfer full): a new toast
 */
GowlBarToast *
gowl_bar_toast_new(const gchar *summary, const gchar *body)
{
	GowlBarToast *self;

	self = g_new0(GowlBarToast, 1);
	self->summary    = g_strdup(summary != NULL ? summary : "");
	self->body       = g_strdup(body);
	self->urgency    = GOWL_BAR_TOAST_NORMAL;
	self->timeout_ms = 5000;
	self->created_us = 0;
	return self;
}

/**
 * gowl_bar_toast_copy:
 * @self: (nullable): a toast
 *
 * Returns: (transfer full) (nullable): a deep copy
 */
GowlBarToast *
gowl_bar_toast_copy(const GowlBarToast *self)
{
	GowlBarToast *copy;

	if (self == NULL)
		return NULL;

	copy = gowl_bar_toast_new(self->summary, self->body);
	copy->id         = self->id;
	copy->icon       = g_strdup(self->icon);
	copy->app        = g_strdup(self->app);
	copy->panel      = g_strdup(self->panel);
	copy->command    = g_strdup(self->command);
	copy->hint       = g_strdup(self->hint);
	copy->urgency    = self->urgency;
	copy->timeout_ms = self->timeout_ms;
	copy->created_us = self->created_us;
	return copy;
}

/**
 * gowl_bar_toast_free:
 * @self: (nullable) (transfer full): a toast
 */
void
gowl_bar_toast_free(GowlBarToast *self)
{
	if (self == NULL)
		return;
	g_free(self->summary);
	g_free(self->body);
	g_free(self->icon);
	g_free(self->app);
	g_free(self->panel);
	g_free(self->command);
	g_free(self->hint);
	g_free(self);
}

/**
 * gowl_bar_toast_get_id:
 * @self: a toast
 *
 * Returns: the toast's id
 */
guint
gowl_bar_toast_get_id(const GowlBarToast *self)
{
	return (self != NULL) ? self->id : 0;
}

/**
 * gowl_bar_toast_set_id:
 * @self: a toast
 * @id: the identifier the stack assigned
 */
void
gowl_bar_toast_set_id(GowlBarToast *self, guint id)
{
	g_return_if_fail(self != NULL);
	self->id = id;
}

#define GOWL_BAR_TOAST_STRING_GETTER(field)                                \
	const gchar *                                                      \
	gowl_bar_toast_get_##field(const GowlBarToast *self)               \
	{                                                                  \
		return (self != NULL) ? self->field : NULL;                \
	}

#define GOWL_BAR_TOAST_STRING_ACCESSOR(field)                              \
	GOWL_BAR_TOAST_STRING_GETTER(field)                                \
	void                                                               \
	gowl_bar_toast_set_##field(GowlBarToast *self, const gchar *value) \
	{                                                                  \
		g_return_if_fail(self != NULL);                            \
		g_free(self->field);                                       \
		self->field = g_strdup(value);                             \
	}

/**
 * gowl_bar_toast_get_summary:
 * @self: a toast
 *
 * Returns: (transfer none) (nullable): the headline
 */
GOWL_BAR_TOAST_STRING_GETTER(summary)

/**
 * gowl_bar_toast_get_body:
 * @self: a toast
 *
 * Returns: (transfer none) (nullable): the detail line
 */
GOWL_BAR_TOAST_STRING_GETTER(body)

/**
 * gowl_bar_toast_set_icon:
 * @self: a toast
 * @value: (nullable): a glyph
 */
GOWL_BAR_TOAST_STRING_ACCESSOR(icon)

/**
 * gowl_bar_toast_set_app:
 * @self: a toast
 * @value: (nullable): the originating application's name
 */
GOWL_BAR_TOAST_STRING_ACCESSOR(app)

/**
 * gowl_bar_toast_set_panel:
 * @self: a toast
 * @value: (nullable): a bar plugin id to open on click
 */
GOWL_BAR_TOAST_STRING_ACCESSOR(panel)

/**
 * gowl_bar_toast_set_command:
 * @self: a toast
 * @value: (nullable): a shell command to run on click
 */
GOWL_BAR_TOAST_STRING_ACCESSOR(command)

/**
 * gowl_bar_toast_set_hint:
 * @self: a toast
 * @value: (nullable): a short line of instruction
 */
GOWL_BAR_TOAST_STRING_ACCESSOR(hint)

#undef GOWL_BAR_TOAST_STRING_ACCESSOR
#undef GOWL_BAR_TOAST_STRING_GETTER

/**
 * gowl_bar_toast_get_urgency:
 * @self: a toast
 *
 * Returns: the urgency level
 */
GowlBarToastUrgency
gowl_bar_toast_get_urgency(const GowlBarToast *self)
{
	return (self != NULL) ? self->urgency : GOWL_BAR_TOAST_NORMAL;
}

/**
 * gowl_bar_toast_set_urgency:
 * @self: a toast
 * @urgency: the level
 *
 * A critical toast also loses its timeout: a plugin crash notice that
 * scrolls away before you look at the screen is the same as no notice.
 */
void
gowl_bar_toast_set_urgency(GowlBarToast *self, GowlBarToastUrgency urgency)
{
	g_return_if_fail(self != NULL);

	self->urgency = urgency;
	if (urgency == GOWL_BAR_TOAST_CRITICAL)
		self->timeout_ms = 0;
}

/**
 * gowl_bar_toast_set_timeout:
 * @self: a toast
 * @ms: milliseconds on screen, or 0 for no expiry
 */
void
gowl_bar_toast_set_timeout(GowlBarToast *self, gint ms)
{
	g_return_if_fail(self != NULL);
	self->timeout_ms = (ms > 0) ? ms : 0;
}

/**
 * gowl_bar_toast_get_timeout:
 * @self: a toast
 *
 * Returns: the timeout in milliseconds, or 0
 */
gint
gowl_bar_toast_get_timeout(const GowlBarToast *self)
{
	return (self != NULL) ? self->timeout_ms : 0;
}

/**
 * gowl_bar_toast_set_created:
 * @self: a toast
 * @monotonic_us: the stamp the stack applied
 */
void
gowl_bar_toast_set_created(GowlBarToast *self, gint64 monotonic_us)
{
	g_return_if_fail(self != NULL);
	self->created_us = monotonic_us;
}

/**
 * gowl_bar_toast_get_created:
 * @self: a toast
 *
 * Returns: the creation stamp in monotonic microseconds
 */
gint64
gowl_bar_toast_get_created(const GowlBarToast *self)
{
	return (self != NULL) ? self->created_us : 0;
}

/**
 * gowl_bar_toast_is_expired:
 * @self: a toast
 * @now_us: the current monotonic time
 *
 * Returns: %TRUE when the toast has outlived its timeout
 */
gboolean
gowl_bar_toast_is_expired(const GowlBarToast *self, gint64 now_us)
{
	if (self == NULL || self->timeout_ms <= 0)
		return FALSE;
	return (now_us - self->created_us) >= ((gint64)self->timeout_ms * 1000);
}

/**
 * gowl_bar_toast_urgency_color:
 * @urgency: a level
 *
 * Returns: the theme role for that level
 */
GowlBarColor
gowl_bar_toast_urgency_color(GowlBarToastUrgency urgency)
{
	switch (urgency) {
	case GOWL_BAR_TOAST_CRITICAL:
		return GOWL_BAR_COLOR_RED;
	case GOWL_BAR_TOAST_LOW:
		return GOWL_BAR_COLOR_OVERLAY;
	case GOWL_BAR_TOAST_NORMAL:
	default:
		return GOWL_BAR_COLOR_ACCENT;
	}
}

/* ----------------------------------------------------------------
 * GowlBarToastStack
 * ---------------------------------------------------------------- */

struct _GowlBarToastStack {
	GObject parent_instance;

	GPtrArray *toasts;    /* GowlBarToast*, owned, oldest first */
	guint      next_id;
	guint      limit;
};

G_DEFINE_FINAL_TYPE(GowlBarToastStack, gowl_bar_toast_stack, G_TYPE_OBJECT)

enum {
	STACK_SIGNAL_CHANGED,
	STACK_SIGNAL_LAST
};

static guint stack_signals[STACK_SIGNAL_LAST];

static void
gowl_bar_toast_stack_finalize(GObject *object)
{
	GowlBarToastStack *self = GOWL_BAR_TOAST_STACK(object);

	g_clear_pointer(&self->toasts, g_ptr_array_unref);

	G_OBJECT_CLASS(gowl_bar_toast_stack_parent_class)->finalize(object);
}

static void
gowl_bar_toast_stack_class_init(GowlBarToastStackClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = gowl_bar_toast_stack_finalize;

	/**
	 * GowlBarToastStack::changed:
	 * @self: the stack
	 *
	 * Emitted whenever the visible set of toasts changed, so the host
	 * knows to rebuild its overlay surface.
	 */
	stack_signals[STACK_SIGNAL_CHANGED] =
		g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 0);
}

static void
gowl_bar_toast_stack_init(GowlBarToastStack *self)
{
	self->toasts = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_bar_toast_free);
	self->next_id = 1;
	self->limit   = GOWL_BAR_TOAST_DEFAULT_LIMIT;
}

/**
 * gowl_bar_toast_stack_new:
 *
 * Returns: (transfer full): a new empty stack
 */
GowlBarToastStack *
gowl_bar_toast_stack_new(void)
{
	return (GowlBarToastStack *)g_object_new(GOWL_TYPE_BAR_TOAST_STACK,
	                                         NULL);
}

/* Drop the oldest non-critical toast.  Critical ones are only removed
   by an explicit dismiss, so a burst of chatter cannot push a crash
   report off the screen. */
static gboolean
stack_evict_one(GowlBarToastStack *self)
{
	guint i;

	for (i = 0; i < self->toasts->len; i++) {
		GowlBarToast *t;

		t = (GowlBarToast *)g_ptr_array_index(self->toasts, i);
		if (t->urgency != GOWL_BAR_TOAST_CRITICAL) {
			g_ptr_array_remove_index(self->toasts, i);
			return TRUE;
		}
	}
	if (self->toasts->len > 0) {
		g_ptr_array_remove_index(self->toasts, 0);
		return TRUE;
	}
	return FALSE;
}

/**
 * gowl_bar_toast_stack_push:
 * @self: a stack
 * @toast: (transfer full): the toast to show
 *
 * Returns: the toast's id
 */
guint
gowl_bar_toast_stack_push(GowlBarToastStack *self, GowlBarToast *toast)
{
	guint id;

	g_return_val_if_fail(GOWL_IS_BAR_TOAST_STACK(self), 0);
	g_return_val_if_fail(toast != NULL, 0);

	if (toast->id == 0)
		toast->id = self->next_id++;
	id = toast->id;

	if (toast->created_us == 0)
		toast->created_us = g_get_monotonic_time();

	while (self->toasts->len >= self->limit) {
		if (!stack_evict_one(self))
			break;
	}

	g_ptr_array_add(self->toasts, toast);
	g_signal_emit(self, stack_signals[STACK_SIGNAL_CHANGED], 0);
	return id;
}

/**
 * gowl_bar_toast_stack_set_limit:
 * @self: a stack
 * @limit: the most toasts to keep on screen; clamped to at least 1
 */
void
gowl_bar_toast_stack_set_limit(GowlBarToastStack *self, guint limit)
{
	g_return_if_fail(GOWL_IS_BAR_TOAST_STACK(self));

	self->limit = (limit > 0) ? limit : 1;
	while (self->toasts->len > self->limit) {
		if (!stack_evict_one(self))
			break;
	}
}

/**
 * gowl_bar_toast_stack_expire:
 * @self: a stack
 * @now_us: the current monotonic time
 *
 * Returns: %TRUE when anything was dropped
 */
gboolean
gowl_bar_toast_stack_expire(GowlBarToastStack *self, gint64 now_us)
{
	gboolean changed;
	guint i;

	g_return_val_if_fail(GOWL_IS_BAR_TOAST_STACK(self), FALSE);

	changed = FALSE;
	i = 0;
	while (i < self->toasts->len) {
		GowlBarToast *t;

		t = (GowlBarToast *)g_ptr_array_index(self->toasts, i);
		if (gowl_bar_toast_is_expired(t, now_us)) {
			g_ptr_array_remove_index(self->toasts, i);
			changed = TRUE;
			continue;
		}
		i++;
	}

	if (changed)
		g_signal_emit(self, stack_signals[STACK_SIGNAL_CHANGED], 0);
	return changed;
}

/**
 * gowl_bar_toast_stack_dismiss:
 * @self: a stack
 * @id: a toast id
 *
 * Returns: %TRUE when a toast with that id was removed
 */
gboolean
gowl_bar_toast_stack_dismiss(GowlBarToastStack *self, guint id)
{
	guint i;

	g_return_val_if_fail(GOWL_IS_BAR_TOAST_STACK(self), FALSE);

	for (i = 0; i < self->toasts->len; i++) {
		GowlBarToast *t;

		t = (GowlBarToast *)g_ptr_array_index(self->toasts, i);
		if (t->id == id) {
			g_ptr_array_remove_index(self->toasts, i);
			g_signal_emit(self,
				stack_signals[STACK_SIGNAL_CHANGED], 0);
			return TRUE;
		}
	}
	return FALSE;
}

/**
 * gowl_bar_toast_stack_clear:
 * @self: a stack
 *
 * Removes every toast, critical ones included.
 */
void
gowl_bar_toast_stack_clear(GowlBarToastStack *self)
{
	g_return_if_fail(GOWL_IS_BAR_TOAST_STACK(self));

	if (self->toasts->len == 0)
		return;
	g_ptr_array_set_size(self->toasts, 0);
	g_signal_emit(self, stack_signals[STACK_SIGNAL_CHANGED], 0);
}

/**
 * gowl_bar_toast_stack_size:
 * @self: a stack
 *
 * Returns: how many toasts are showing
 */
guint
gowl_bar_toast_stack_size(GowlBarToastStack *self)
{
	g_return_val_if_fail(GOWL_IS_BAR_TOAST_STACK(self), 0);
	return self->toasts->len;
}

/**
 * gowl_bar_toast_stack_get:
 * @self: a stack
 * @index: a position, oldest first
 *
 * Returns: (transfer none) (nullable): the toast, or %NULL
 */
GowlBarToast *
gowl_bar_toast_stack_get(GowlBarToastStack *self, guint index)
{
	g_return_val_if_fail(GOWL_IS_BAR_TOAST_STACK(self), NULL);

	if (index >= self->toasts->len)
		return NULL;
	return (GowlBarToast *)g_ptr_array_index(self->toasts, index);
}

/**
 * gowl_bar_toast_stack_next_timeout:
 * @self: a stack
 * @now_us: the current monotonic time
 *
 * Returns: milliseconds until the next expiry, or -1
 */
gint
gowl_bar_toast_stack_next_timeout(GowlBarToastStack *self, gint64 now_us)
{
	gint64 best;
	guint i;

	g_return_val_if_fail(GOWL_IS_BAR_TOAST_STACK(self), -1);

	best = -1;
	for (i = 0; i < self->toasts->len; i++) {
		GowlBarToast *t;
		gint64 remain;

		t = (GowlBarToast *)g_ptr_array_index(self->toasts, i);
		if (t->timeout_ms <= 0)
			continue;
		remain = ((gint64)t->timeout_ms * 1000)
		         - (now_us - t->created_us);
		if (remain < 0)
			remain = 0;
		if (best < 0 || remain < best)
			best = remain;
	}
	if (best < 0)
		return -1;
	return (gint)(best / 1000) + 1;
}
