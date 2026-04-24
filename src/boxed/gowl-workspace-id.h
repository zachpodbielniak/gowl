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

#ifndef GOWL_WORKSPACE_ID_H
#define GOWL_WORKSPACE_ID_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_WORKSPACE_ID (gowl_workspace_id_get_type())

/**
 * GowlWorkspaceId:
 * @value: the monotonically-allocated identifier.
 *
 * Opaque 64-bit identifier for a #GowlWorkspace.  Exists as a boxed
 * type rather than a raw `guint64' so it is distinguishable in GIR
 * signatures and carried on signals without losing its semantic
 * meaning.  Zero (%GOWL_WORKSPACE_ID_INVALID) is reserved as a
 * sentinel for "no workspace".
 */
typedef struct _GowlWorkspaceId GowlWorkspaceId;

struct _GowlWorkspaceId {
	guint64 value;
};

/**
 * GOWL_WORKSPACE_ID_INVALID:
 *
 * Sentinel value used to mean "no workspace" (returned by
 * #gowl_workspace_provider_get_current before any workspace exists,
 * for example).  Never a valid identifier — the allocator starts
 * at 1.
 */
#define GOWL_WORKSPACE_ID_INVALID (0)

GType gowl_workspace_id_get_type(void) G_GNUC_CONST;

/**
 * gowl_workspace_id_new:
 * @value: the numeric identifier
 *
 * Boxes @value as a #GowlWorkspaceId.
 *
 * Returns: (transfer full): a new boxed id
 */
GowlWorkspaceId *
gowl_workspace_id_new(guint64 value);

/**
 * gowl_workspace_id_copy:
 * @self: (nullable): a boxed id to copy
 *
 * Returns: (transfer full) (nullable): a fresh copy of @self
 */
GowlWorkspaceId *
gowl_workspace_id_copy(const GowlWorkspaceId *self);

/**
 * gowl_workspace_id_free:
 * @self: (nullable): an id to free
 *
 * Releases the id.  Safe with %NULL.
 */
void
gowl_workspace_id_free(GowlWorkspaceId *self);

/**
 * gowl_workspace_id_get_value:
 * @self: a #GowlWorkspaceId
 *
 * Returns: the underlying `guint64' value, or
 *          %GOWL_WORKSPACE_ID_INVALID when @self is %NULL.
 */
guint64
gowl_workspace_id_get_value(const GowlWorkspaceId *self);

/**
 * gowl_workspace_id_equals:
 * @a: (nullable): first id
 * @b: (nullable): second id
 *
 * Two %NULL ids are considered equal; a %NULL versus a boxed id
 * with value %GOWL_WORKSPACE_ID_INVALID are NOT — %NULL means "no
 * id was even allocated", the invalid value means "allocator bug
 * or sentinel return".
 *
 * Returns: %TRUE when both ids carry the same value.
 */
gboolean
gowl_workspace_id_equals(const GowlWorkspaceId *a,
                          const GowlWorkspaceId *b);

G_END_DECLS

#endif /* GOWL_WORKSPACE_ID_H */
