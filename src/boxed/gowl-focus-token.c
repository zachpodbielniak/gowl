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

#include "gowl-focus-token.h"

/**
 * struct _GowlFocusToken:
 * @saved_client: raw pointer to the previously focused #GowlClient
 *   (may be %NULL if there was no prior focus).  Not refcounted —
 *   tokens are expected to have a short lifetime bounded by the
 *   owning seat.
 * @reason: why the redirect was initiated (recorded for signals and
 *   introspection)
 *
 * Private representation of a #GowlFocusToken.
 */
struct _GowlFocusToken {
	gpointer        saved_client;
	GowlFocusReason reason;
};

G_DEFINE_BOXED_TYPE(GowlFocusToken, gowl_focus_token,
                    gowl_focus_token_copy, gowl_focus_token_free)

GowlFocusToken *
gowl_focus_token_new(gpointer        saved_client,
                      GowlFocusReason reason)
{
	GowlFocusToken *self;

	self = g_slice_new(GowlFocusToken);
	self->saved_client = saved_client;
	self->reason       = reason;

	return self;
}

GowlFocusToken *
gowl_focus_token_copy(const GowlFocusToken *self)
{
	if (self == NULL)
		return NULL;

	return gowl_focus_token_new(self->saved_client, self->reason);
}

void
gowl_focus_token_free(GowlFocusToken *self)
{
	if (self == NULL)
		return;
	g_slice_free(GowlFocusToken, self);
}

gpointer
gowl_focus_token_get_saved_client(const GowlFocusToken *self)
{
	if (self == NULL)
		return NULL;
	return self->saved_client;
}

GowlFocusReason
gowl_focus_token_get_reason(const GowlFocusToken *self)
{
	if (self == NULL)
		return GOWL_FOCUS_REASON_EXPLICIT;
	return self->reason;
}
