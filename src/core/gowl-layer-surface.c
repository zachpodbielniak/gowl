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
 * GowlLayerSurface:
 *
 * Represents a wlr-layer-shell surface.  Layer surfaces are used for
 * panels, wallpapers, overlays, and other shell UI elements.  Each
 * surface belongs to a specific layer and is associated with a monitor.
 * The struct definition lives in gowl-core-private.h.
 */

G_DEFINE_FINAL_TYPE(GowlLayerSurface, gowl_layer_surface, G_TYPE_OBJECT)

/* Signal identifiers */
enum {
	SIGNAL_MAP,
	SIGNAL_UNMAP,
	SIGNAL_DESTROY,
	N_SIGNALS
};

static guint layer_signals[N_SIGNALS] = { 0, };

/* --- GObject lifecycle --- */

static void
gowl_layer_surface_dispose(GObject *object)
{
	GowlLayerSurface *self;

	self = GOWL_LAYER_SURFACE(object);
	self->mon = NULL;
	self->compositor = NULL;

	G_OBJECT_CLASS(gowl_layer_surface_parent_class)->dispose(object);
}

static void
gowl_layer_surface_finalize(GObject *object)
{
	G_OBJECT_CLASS(gowl_layer_surface_parent_class)->finalize(object);
}

/* --- class / instance init --- */

static void
gowl_layer_surface_class_init(GowlLayerSurfaceClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	object_class->dispose  = gowl_layer_surface_dispose;
	object_class->finalize = gowl_layer_surface_finalize;

	/**
	 * GowlLayerSurface::map:
	 * @surface: the #GowlLayerSurface that emitted the signal
	 *
	 * Emitted when the layer surface is mapped (becomes visible).
	 */
	layer_signals[SIGNAL_MAP] =
		g_signal_new("map",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlLayerSurface::unmap:
	 * @surface: the #GowlLayerSurface that emitted the signal
	 *
	 * Emitted when the layer surface is unmapped (hidden).
	 */
	layer_signals[SIGNAL_UNMAP] =
		g_signal_new("unmap",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0,
		             NULL, NULL,
		             NULL,
		             G_TYPE_NONE,
		             0);

	/**
	 * GowlLayerSurface::destroy:
	 * @surface: the #GowlLayerSurface that emitted the signal
	 *
	 * Emitted when the layer surface is being destroyed.
	 */
	layer_signals[SIGNAL_DESTROY] =
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
gowl_layer_surface_init(GowlLayerSurface *self)
{
	self->wlr_layer_surface  = NULL;
	self->scene_layer_surface = NULL;
	self->scene              = NULL;
	self->layer              = 0;
	self->mapped             = FALSE;
	self->mon                = NULL;
	self->compositor         = NULL;
}

/* --- Public API --- */

/**
 * gowl_layer_surface_new:
 *
 * Creates a new #GowlLayerSurface with default state.
 *
 * Returns: (transfer full): a newly allocated #GowlLayerSurface
 */
GowlLayerSurface *
gowl_layer_surface_new(void)
{
	return (GowlLayerSurface *)g_object_new(GOWL_TYPE_LAYER_SURFACE, NULL);
}

/**
 * gowl_layer_surface_get_layer:
 * @self: a #GowlLayerSurface
 *
 * Returns which layer this surface belongs to (background, bottom,
 * top, or overlay).
 *
 * Returns: the layer index
 */
gint
gowl_layer_surface_get_layer(GowlLayerSurface *self)
{
	g_return_val_if_fail(GOWL_IS_LAYER_SURFACE(self), 0);

	return self->layer;
}

/**
 * gowl_layer_surface_is_mapped:
 * @self: a #GowlLayerSurface
 *
 * Returns whether the layer surface is currently mapped.
 *
 * Returns: %TRUE if the surface is mapped
 */
gboolean
gowl_layer_surface_is_mapped(GowlLayerSurface *self)
{
	g_return_val_if_fail(GOWL_IS_LAYER_SURFACE(self), FALSE);

	return self->mapped;
}
