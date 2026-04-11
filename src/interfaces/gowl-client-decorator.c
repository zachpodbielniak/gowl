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

#include "gowl-client-decorator.h"

G_DEFINE_INTERFACE(GowlClientDecorator, gowl_client_decorator, G_TYPE_OBJECT)

static void
gowl_client_decorator_default_init(GowlClientDecoratorInterface *iface)
{
	/* Default implementation - no-op */
	(void)iface;
}

/**
 * gowl_client_decorator_get_border_width:
 * @self: a #GowlClientDecorator
 * @client: (nullable): the client to query border width for
 *
 * Returns the border width in pixels for the given client.
 *
 * Returns: the border width in pixels, or 0 if unset
 */
gint
gowl_client_decorator_get_border_width(
	GowlClientDecorator *self,
	gpointer             client
){
	GowlClientDecoratorInterface *iface;

	g_return_val_if_fail(GOWL_IS_CLIENT_DECORATOR(self), 0);

	iface = GOWL_CLIENT_DECORATOR_GET_IFACE(self);
	if (iface->get_border_width != NULL)
		return iface->get_border_width(self, client);
	return 0;
}

/**
 * gowl_client_decorator_should_draw_border:
 * @self: a #GowlClientDecorator
 * @client: (nullable): the client to query
 *
 * Returns whether a border should be drawn for the given client.
 *
 * Returns: %TRUE if a border should be drawn, %FALSE otherwise
 */
gboolean
gowl_client_decorator_should_draw_border(
	GowlClientDecorator *self,
	gpointer             client
){
	GowlClientDecoratorInterface *iface;

	g_return_val_if_fail(GOWL_IS_CLIENT_DECORATOR(self), FALSE);

	iface = GOWL_CLIENT_DECORATOR_GET_IFACE(self);
	if (iface->should_draw_border != NULL)
		return iface->should_draw_border(self, client);
	return FALSE;
}

/**
 * gowl_client_decorator_render_decoration:
 * @self: a #GowlClientDecorator
 * @client: the client to decorate
 * @width: total client geometry width (including borders)
 * @height: total client geometry height (including borders)
 * @bw: border width in pixels
 * @color: (array fixed-size=4): RGBA border color (pre-multiplied floats)
 *
 * Renders the decoration for @client.  The implementor creates or
 * updates scene-graph nodes (typically a cairo-rendered wlr_scene_buffer)
 * positioned relative to the client's scene tree.
 */
void
gowl_client_decorator_render_decoration(
	GowlClientDecorator *self,
	gpointer             client,
	gint                 width,
	gint                 height,
	guint                bw,
	const float         *color
){
	GowlClientDecoratorInterface *iface;

	g_return_if_fail(GOWL_IS_CLIENT_DECORATOR(self));

	iface = GOWL_CLIENT_DECORATOR_GET_IFACE(self);
	if (iface->render_decoration != NULL)
		iface->render_decoration(self, client, width, height, bw, color);
}

/**
 * gowl_client_decorator_destroy_decoration:
 * @self: a #GowlClientDecorator
 * @client: the client whose decoration should be removed
 *
 * Cleans up all decoration state for @client.
 */
void
gowl_client_decorator_destroy_decoration(
	GowlClientDecorator *self,
	gpointer             client
){
	GowlClientDecoratorInterface *iface;

	g_return_if_fail(GOWL_IS_CLIENT_DECORATOR(self));

	iface = GOWL_CLIENT_DECORATOR_GET_IFACE(self);
	if (iface->destroy_decoration != NULL)
		iface->destroy_decoration(self, client);
}

/**
 * gowl_client_decorator_get_corner_radius:
 * @self: a #GowlClientDecorator
 *
 * Returns: the corner radius in pixels, or 0 for square corners
 */
gint
gowl_client_decorator_get_corner_radius(GowlClientDecorator *self)
{
	GowlClientDecoratorInterface *iface;

	g_return_val_if_fail(GOWL_IS_CLIENT_DECORATOR(self), 0);

	iface = GOWL_CLIENT_DECORATOR_GET_IFACE(self);
	if (iface->get_corner_radius != NULL)
		return iface->get_corner_radius(self);
	return 0;
}
