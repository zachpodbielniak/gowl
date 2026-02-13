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

#include "gowlbar-widget.h"

/**
 * GowlbarWidget:
 *
 * Abstract base class for bar widgets.  Each widget knows how to
 * render itself, report its desired width, and handle click events.
 */
G_DEFINE_ABSTRACT_TYPE(GowlbarWidget, gowlbar_widget, G_TYPE_OBJECT)

/* --- Default vfunc implementations --- */

static void
gowlbar_widget_default_render(
	GowlbarWidget *self,
	cairo_t       *cr,
	PangoLayout   *layout,
	gint           x,
	gint           y,
	gint           width,
	gint           height
){
	/* default: do nothing */
	(void)self; (void)cr; (void)layout;
	(void)x; (void)y; (void)width; (void)height;
}

static gint
gowlbar_widget_default_get_width(
	GowlbarWidget *self,
	cairo_t       *cr,
	PangoLayout   *layout
){
	(void)self; (void)cr; (void)layout;
	return 0;
}

static gboolean
gowlbar_widget_default_on_click(
	GowlbarWidget *self,
	gint           x,
	gint           y,
	guint          button
){
	(void)self; (void)x; (void)y; (void)button;
	return FALSE;
}

static void
gowlbar_widget_default_update(GowlbarWidget *self)
{
	(void)self;
}

/* --- Class / instance init --- */

static void
gowlbar_widget_class_init(GowlbarWidgetClass *klass)
{
	klass->render    = gowlbar_widget_default_render;
	klass->get_width = gowlbar_widget_default_get_width;
	klass->on_click  = gowlbar_widget_default_on_click;
	klass->update    = gowlbar_widget_default_update;
}

static void
gowlbar_widget_init(GowlbarWidget *self)
{
	(void)self;
}

/* --- Public API (dispatch to vfuncs) --- */

void
gowlbar_widget_render(
	GowlbarWidget *self,
	cairo_t       *cr,
	PangoLayout   *layout,
	gint           x,
	gint           y,
	gint           width,
	gint           height
){
	GowlbarWidgetClass *klass;

	g_return_if_fail(GOWLBAR_IS_WIDGET(self));

	klass = GOWLBAR_WIDGET_GET_CLASS(self);
	if (klass->render != NULL)
		klass->render(self, cr, layout, x, y, width, height);
}

gint
gowlbar_widget_get_width(
	GowlbarWidget *self,
	cairo_t       *cr,
	PangoLayout   *layout
){
	GowlbarWidgetClass *klass;

	g_return_val_if_fail(GOWLBAR_IS_WIDGET(self), 0);

	klass = GOWLBAR_WIDGET_GET_CLASS(self);
	if (klass->get_width != NULL)
		return klass->get_width(self, cr, layout);
	return 0;
}

gboolean
gowlbar_widget_on_click(
	GowlbarWidget *self,
	gint           x,
	gint           y,
	guint          button
){
	GowlbarWidgetClass *klass;

	g_return_val_if_fail(GOWLBAR_IS_WIDGET(self), FALSE);

	klass = GOWLBAR_WIDGET_GET_CLASS(self);
	if (klass->on_click != NULL)
		return klass->on_click(self, x, y, button);
	return FALSE;
}

void
gowlbar_widget_update(GowlbarWidget *self)
{
	GowlbarWidgetClass *klass;

	g_return_if_fail(GOWLBAR_IS_WIDGET(self));

	klass = GOWLBAR_WIDGET_GET_CLASS(self);
	if (klass->update != NULL)
		klass->update(self);
}
