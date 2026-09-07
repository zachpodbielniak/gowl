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

#ifndef GOWL_BAR_TOAST_H
#define GOWL_BAR_TOAST_H

#include <glib-object.h>

#include "barkit/gowl-bar-theme.h"

G_BEGIN_DECLS

/**
 * GowlBarToastUrgency:
 * @GOWL_BAR_TOAST_LOW: background chatter; short timeout, muted colour.
 * @GOWL_BAR_TOAST_NORMAL: the default.
 * @GOWL_BAR_TOAST_CRITICAL: never expires on its own and is drawn in
 *   the theme's red.  A plugin that failed to load raises one of these.
 *
 * Matches the freedesktop notification urgency levels so a toast can be
 * fed straight from an incoming notification.
 */
typedef enum {
	GOWL_BAR_TOAST_LOW = 0,
	GOWL_BAR_TOAST_NORMAL,
	GOWL_BAR_TOAST_CRITICAL
} GowlBarToastUrgency;

/**
 * GowlBarToast:
 *
 * One on-screen notification card.
 */
typedef struct _GowlBarToast GowlBarToast;

#define GOWL_TYPE_BAR_TOAST (gowl_bar_toast_get_type())

GType gowl_bar_toast_get_type (void) G_GNUC_CONST;

/**
 * gowl_bar_toast_new:
 * @summary: the headline
 * @body: (nullable): the detail line
 *
 * Returns: (transfer full): a new toast at normal urgency
 */
GowlBarToast *gowl_bar_toast_new  (const gchar *summary, const gchar *body);
GowlBarToast *gowl_bar_toast_copy (const GowlBarToast *self);
void          gowl_bar_toast_free (GowlBarToast *self);

guint        gowl_bar_toast_get_id      (const GowlBarToast *self);
void         gowl_bar_toast_set_id      (GowlBarToast *self, guint id);
const gchar *gowl_bar_toast_get_summary (const GowlBarToast *self);
const gchar *gowl_bar_toast_get_body    (const GowlBarToast *self);
const gchar *gowl_bar_toast_get_icon    (const GowlBarToast *self);
void         gowl_bar_toast_set_icon    (GowlBarToast *self,
                                          const gchar  *icon);
const gchar *gowl_bar_toast_get_app     (const GowlBarToast *self);
void         gowl_bar_toast_set_app     (GowlBarToast *self,
                                          const gchar  *app);

GowlBarToastUrgency gowl_bar_toast_get_urgency (const GowlBarToast *self);
void                gowl_bar_toast_set_urgency (GowlBarToast *self,
                                                 GowlBarToastUrgency urgency);

/**
 * gowl_bar_toast_set_timeout:
 * @self: a toast
 * @ms: milliseconds on screen, or 0 to stay until dismissed
 */
void  gowl_bar_toast_set_timeout (GowlBarToast *self, gint ms);
gint  gowl_bar_toast_get_timeout (const GowlBarToast *self);

/**
 * gowl_bar_toast_set_panel:
 * @self: a toast
 * @plugin_id: (nullable): a bar plugin id
 *
 * Clicking the toast opens that plugin's panel.  This is what turns a
 * "Wi-Fi is not configured" notification into a one-click route to the
 * network dropdown, instead of a dead end that tells you to go find it.
 */
void         gowl_bar_toast_set_panel (GowlBarToast *self,
                                        const gchar  *plugin_id);
const gchar *gowl_bar_toast_get_panel (const GowlBarToast *self);

/**
 * gowl_bar_toast_set_command:
 * @self: a toast
 * @cmdline: (nullable): a shell command run when the toast is clicked
 *
 * Used when no panel is set.  A toast with neither is informational and
 * a click only dismisses it.
 */
void         gowl_bar_toast_set_command (GowlBarToast *self,
                                          const gchar  *cmdline);
const gchar *gowl_bar_toast_get_command (const GowlBarToast *self);

/**
 * gowl_bar_toast_set_hint:
 * @self: a toast
 * @hint: (nullable): a short line of instruction, e.g. `Click to configure'
 */
void         gowl_bar_toast_set_hint (GowlBarToast *self, const gchar *hint);
const gchar *gowl_bar_toast_get_hint (const GowlBarToast *self);

void    gowl_bar_toast_set_created (GowlBarToast *self, gint64 monotonic_us);
gint64  gowl_bar_toast_get_created (const GowlBarToast *self);

/**
 * gowl_bar_toast_is_expired:
 * @self: a toast
 * @now_us: the current monotonic time in microseconds
 *
 * Returns: %TRUE when the toast has outlived its timeout
 */
gboolean gowl_bar_toast_is_expired (const GowlBarToast *self, gint64 now_us);

/**
 * gowl_bar_toast_urgency_color:
 * @urgency: a level
 *
 * Returns: the theme role a toast at that level is accented with
 */
GowlBarColor gowl_bar_toast_urgency_color (GowlBarToastUrgency urgency);

/* --- The stack ---------------------------------------------------- */

#define GOWL_TYPE_BAR_TOAST_STACK (gowl_bar_toast_stack_get_type())

G_DECLARE_FINAL_TYPE(GowlBarToastStack, gowl_bar_toast_stack,
                     GOWL, BAR_TOAST_STACK, GObject)

/**
 * gowl_bar_toast_stack_new:
 *
 * Creates an empty toast stack.
 *
 * Returns: (transfer full): a new #GowlBarToastStack
 */
GowlBarToastStack *gowl_bar_toast_stack_new (void);

/**
 * gowl_bar_toast_stack_push:
 * @self: a stack
 * @toast: (transfer full): the toast to show
 *
 * Adds @toast, assigning it an id if it has none and stamping its
 * creation time.  Oldest non-critical toasts are dropped once the
 * stack is at its limit.
 *
 * Returns: the toast's id
 */
guint gowl_bar_toast_stack_push (GowlBarToastStack *self,
                                  GowlBarToast      *toast);

void  gowl_bar_toast_stack_set_limit (GowlBarToastStack *self, guint limit);

/**
 * gowl_bar_toast_stack_expire:
 * @self: a stack
 * @now_us: the current monotonic time
 *
 * Drops every timed-out toast.
 *
 * Returns: %TRUE when anything was dropped
 */
gboolean gowl_bar_toast_stack_expire (GowlBarToastStack *self, gint64 now_us);

gboolean gowl_bar_toast_stack_dismiss (GowlBarToastStack *self, guint id);
void     gowl_bar_toast_stack_clear   (GowlBarToastStack *self);

guint          gowl_bar_toast_stack_size (GowlBarToastStack *self);
GowlBarToast  *gowl_bar_toast_stack_get  (GowlBarToastStack *self,
                                           guint index);

/**
 * gowl_bar_toast_stack_next_timeout:
 * @self: a stack
 * @now_us: the current monotonic time
 *
 * Returns: milliseconds until the next toast expires, or -1 when
 *   nothing is on a timer
 */
gint gowl_bar_toast_stack_next_timeout (GowlBarToastStack *self,
                                         gint64             now_us);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlBarToast, gowl_bar_toast_free)

G_END_DECLS

#endif /* GOWL_BAR_TOAST_H */
