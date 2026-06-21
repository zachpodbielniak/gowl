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

#include "gowl-capture-source.h"

#include <string.h>

G_DEFINE_BOXED_TYPE(GowlCaptureSource, gowl_capture_source,
                    gowl_capture_source_copy, gowl_capture_source_free)

/**
 * gowl_capture_source_new:
 * @kind: monitor or window
 * @id: (nullable): stable identifier (output name or toplevel id)
 * @title: (nullable): human-readable title
 * @app_id: (nullable): application id (window sources only)
 *
 * Allocates a new #GowlCaptureSource.  The string fields are copied;
 * %NULL is stored as %NULL (not the empty string) so callers can
 * distinguish "unset" from "empty".
 *
 * Returns: (transfer full): a new #GowlCaptureSource. Free with
 *          gowl_capture_source_free().
 */
GowlCaptureSource *
gowl_capture_source_new(
	GowlCaptureSourceKind  kind,
	const gchar           *id,
	const gchar           *title,
	const gchar           *app_id
){
	GowlCaptureSource *self;

	self = g_slice_new0(GowlCaptureSource);
	self->kind   = kind;
	self->id     = g_strdup(id);
	self->title  = g_strdup(title);
	self->app_id = g_strdup(app_id);

	return self;
}

/**
 * gowl_capture_source_copy:
 * @self: (not nullable): a #GowlCaptureSource to copy
 *
 * Creates a deep copy of @self, duplicating its strings.
 *
 * Returns: (transfer full): a newly allocated copy. Free with
 *          gowl_capture_source_free().
 */
GowlCaptureSource *
gowl_capture_source_copy(const GowlCaptureSource *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return gowl_capture_source_new(self->kind, self->id,
	                               self->title, self->app_id);
}

/**
 * gowl_capture_source_free:
 * @self: (nullable): a #GowlCaptureSource to free
 *
 * Releases @self and its strings. Safe to call with %NULL.
 */
void
gowl_capture_source_free(GowlCaptureSource *self)
{
	if (self != NULL) {
		g_free(self->id);
		g_free(self->title);
		g_free(self->app_id);
		g_slice_free(GowlCaptureSource, self);
	}
}

/* g_strcmp0-style equality that treats two NULLs as equal. */
static gboolean
str_equal0(const gchar *a, const gchar *b)
{
	return g_strcmp0(a, b) == 0;
}

/**
 * gowl_capture_source_equals:
 * @a: (not nullable): first source
 * @b: (not nullable): second source
 *
 * Tests whether two sources describe the same capturable entity: same
 * kind and same id/title/app_id (NULL-safe, two NULLs compare equal).
 *
 * Returns: %TRUE if equal, %FALSE otherwise.
 */
gboolean
gowl_capture_source_equals(
	const GowlCaptureSource *a,
	const GowlCaptureSource *b
){
	g_return_val_if_fail(a != NULL, FALSE);
	g_return_val_if_fail(b != NULL, FALSE);

	return a->kind == b->kind
	       && str_equal0(a->id, b->id)
	       && str_equal0(a->title, b->title)
	       && str_equal0(a->app_id, b->app_id);
}
