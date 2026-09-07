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

#ifndef GOWL_BAR_PANEL_RENDER_H
#define GOWL_BAR_PANEL_RENDER_H

#include <glib-object.h>
#include <cairo.h>
#include <pango/pangocairo.h>

#include "barkit/gowl-bar-panel.h"
#include "barkit/gowl-bar-theme.h"

G_BEGIN_DECLS

/**
 * GowlBarHitRect:
 * @item_index: index of the owning item in the panel
 * @child_index: index within a %GOWL_BAR_ITEM_BUTTONS or
 *   %GOWL_BAR_ITEM_CALENDAR item, or -1
 * @kind: the owning item's kind, so a caller can tell a slider drag
 *   from a row click without looking the item back up
 * @id: (nullable): the owning item's activation id, borrowed from the
 *   panel and valid only while that panel is alive
 * @x: left edge in panel-local coordinates
 * @y: top edge, already offset by the panel's scroll position
 * @width: width in pixels
 * @height: height in pixels
 *
 * One interactive region produced by a render pass.  The renderer
 * emits these instead of the caller re-deriving geometry, so hit-
 * testing can never drift from what was actually drawn.
 */
typedef struct {
	gint             item_index;
	gint             child_index;
	GowlBarItemKind  kind;
	const gchar     *id;
	gint             x;
	gint             y;
	gint             width;
	gint             height;
} GowlBarHitRect;

/**
 * GowlBarPanelRenderCtx:
 * @width: the panel's content width in pixels
 * @max_height: the tallest the panel may draw, or 0 for unlimited
 * @scroll: how far the content is scrolled up, in pixels
 * @hover_item: the item index under the pointer, or -1
 * @hover_child: the child index under the pointer, or -1
 * @focus_item: the item index the keyboard cursor is on, or -1
 * @focus_child: the child index the keyboard cursor is on, or -1
 * @hits: (element-type GowlBarHitRect) (nullable): filled with the
 *   interactive regions when non-%NULL
 * @content_height: (out): the height the content wanted, before the cap
 *
 * Everything a render pass needs beyond the panel and the theme.
 * Zero-initialise it and set what you need; -1 is the "nothing" value
 * for the index fields.
 */
typedef struct {
	gint     width;
	gint     max_height;
	gint     scroll;
	gint     hover_item;
	gint     hover_child;
	gint     focus_item;
	gint     focus_child;
	GArray  *hits;
	gint     content_height;
} GowlBarPanelRenderCtx;

/**
 * gowl_bar_panel_render_ctx_init:
 * @ctx: (out caller-allocates): the context to prepare
 * @width: the content width
 *
 * Zeroes @ctx and sets every index field to -1.  Always use this
 * rather than a bare memset: a zeroed @hover_item would mean "item 0
 * is hovered", which lights up the first row of every panel.
 */
void gowl_bar_panel_render_ctx_init (GowlBarPanelRenderCtx *ctx, gint width);

/**
 * gowl_bar_panel_measure:
 * @panel: the panel to measure
 * @layout: a #PangoLayout to measure text with
 * @theme: the active theme
 * @width: the content width in pixels
 *
 * Returns: the total content height in pixels
 */
gint gowl_bar_panel_measure (GowlBarPanel       *panel,
                              PangoLayout        *layout,
                              const GowlBarTheme *theme,
                              gint                width);

/**
 * gowl_bar_panel_render:
 * @panel: the panel to draw
 * @cr: the target context, with the panel's content origin at (0, 0)
 * @layout: a #PangoLayout to draw text with
 * @theme: the active theme
 * @ctx: the render context; @ctx->hits and @ctx->content_height are
 *   filled in
 *
 * Draws the panel's items.  The caller is responsible for the panel's
 * own background, border and shadow --- see
 * gowl_bar_panel_draw_frame() --- because those belong to the surface,
 * not the content, and a scrolled panel must clip the content without
 * clipping its own chrome.
 */
void gowl_bar_panel_render (GowlBarPanel          *panel,
                             cairo_t               *cr,
                             PangoLayout           *layout,
                             const GowlBarTheme    *theme,
                             GowlBarPanelRenderCtx *ctx);

/**
 * gowl_bar_panel_draw_frame:
 * @cr: the target context, with the surface origin at (0, 0)
 * @theme: the active theme
 * @x: the frame's left edge
 * @y: the frame's top edge
 * @width: the frame's width
 * @height: the frame's height
 * @accent: (nullable) (array fixed-size=4): a border colour override,
 *   or %NULL for the theme's accent
 *
 * Paints the shadow, body and border shared by panels and toasts.
 */
void gowl_bar_panel_draw_frame (cairo_t            *cr,
                                 const GowlBarTheme *theme,
                                 gint                x,
                                 gint                y,
                                 gint                width,
                                 gint                height,
                                 const gdouble      *accent);

/**
 * gowl_bar_hit_find:
 * @hits: (element-type GowlBarHitRect): regions from a render pass
 * @x: panel-local x
 * @y: panel-local y
 *
 * Returns: the index into @hits of the topmost region containing the
 *   point, or -1.  Later regions win, matching draw order.
 */
gint gowl_bar_hit_find (GArray *hits, gint x, gint y);

G_END_DECLS

#endif /* GOWL_BAR_PANEL_RENDER_H */
