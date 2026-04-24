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

#ifndef GOWL_FOCUS_TOKEN_H
#define GOWL_FOCUS_TOKEN_H

#include "gowl-types.h"
#include "gowl-enums.h"

G_BEGIN_DECLS

#define GOWL_TYPE_FOCUS_TOKEN (gowl_focus_token_get_type())

/**
 * GowlFocusToken:
 *
 * Opaque handle returned by #gowl_seat_push_focus_redirect.  Holds
 * the saved focus target so a matching #gowl_seat_pop_focus_redirect
 * call can restore it.  The token also carries a #GowlFocusReason
 * so introspection consumers can report *why* the redirect was
 * started.  The internal representation is deliberately private;
 * code using this type should only pass tokens between push/pop.
 */
typedef struct _GowlFocusToken GowlFocusToken;

GType gowl_focus_token_get_type(void) G_GNUC_CONST;

/**
 * gowl_focus_token_new:
 * @saved_client: (nullable): the client that held focus before the
 *   redirect (or %NULL if none).  The token holds this as a raw
 *   pointer; callers must guarantee the client outlives the token
 *   (standard GowlSeat lifetime rule: tokens live only between
 *   push/pop within a single dispatch tick in typical cmacs use).
 * @reason: why the redirect was initiated
 *
 * Allocates a new token.  Most callers should go through
 * #gowl_seat_push_focus_redirect instead of constructing tokens
 * directly; this exists for introspection and for bindings that
 * want to synthesise tokens for testing.
 *
 * Returns: (transfer full): a new #GowlFocusToken
 */
GowlFocusToken *
gowl_focus_token_new(gpointer         saved_client,
                      GowlFocusReason  reason);

/**
 * gowl_focus_token_copy:
 * @self: (nullable): a token to copy
 *
 * Shallow-copies the token.  Safe with %NULL.
 *
 * Returns: (transfer full) (nullable): a new token with the same
 *          saved_client pointer and reason, or %NULL when @self is %NULL.
 */
GowlFocusToken *
gowl_focus_token_copy(const GowlFocusToken *self);

/**
 * gowl_focus_token_free:
 * @self: (nullable): a token to free
 *
 * Releases the token.  Safe with %NULL.  Does not touch the saved
 * client pointer.
 */
void
gowl_focus_token_free(GowlFocusToken *self);

/**
 * gowl_focus_token_get_saved_client:
 * @self: a #GowlFocusToken
 *
 * Returns: (transfer none) (nullable): the client pointer the token
 *          recorded, or %NULL
 */
gpointer
gowl_focus_token_get_saved_client(const GowlFocusToken *self);

/**
 * gowl_focus_token_get_reason:
 * @self: a #GowlFocusToken
 *
 * Returns: the #GowlFocusReason recorded at push time
 */
GowlFocusReason
gowl_focus_token_get_reason(const GowlFocusToken *self);

G_END_DECLS

#endif /* GOWL_FOCUS_TOKEN_H */
