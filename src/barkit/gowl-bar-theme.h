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

#ifndef GOWL_BAR_THEME_H
#define GOWL_BAR_THEME_H

#include <glib-object.h>
#include <cairo.h>

#include "boxed/gowl-palette.h"

G_BEGIN_DECLS

/**
 * GowlBarColor:
 * @GOWL_BAR_COLOR_BASE: the bar / panel body background.
 * @GOWL_BAR_COLOR_MANTLE: one step behind @GOWL_BAR_COLOR_BASE; panel
 *   footers and inset areas.
 * @GOWL_BAR_COLOR_CRUST: the darkest ground; panel shadow.
 * @GOWL_BAR_COLOR_SURFACE: a raised row or an inactive button face.
 * @GOWL_BAR_COLOR_SURFACE_ALT: a hovered row or a pressed button face.
 * @GOWL_BAR_COLOR_OVERLAY: hairlines, dividers and slider troughs.
 * @GOWL_BAR_COLOR_TEXT: primary text.
 * @GOWL_BAR_COLOR_SUBTEXT: secondary text (subtitles, values).
 * @GOWL_BAR_COLOR_MUTED: tertiary text (section headers, disabled).
 * @GOWL_BAR_COLOR_ACCENT: the one colour that says "this is selected".
 * @GOWL_BAR_COLOR_RED: semantic red.
 * @GOWL_BAR_COLOR_GREEN: semantic green.
 * @GOWL_BAR_COLOR_YELLOW: semantic yellow.
 * @GOWL_BAR_COLOR_BLUE: semantic blue.
 * @GOWL_BAR_COLOR_MAUVE: semantic mauve.
 * @GOWL_BAR_COLOR_TEAL: semantic teal.
 * @GOWL_BAR_COLOR_PEACH: semantic peach.
 * @GOWL_BAR_COLOR_PINK: semantic pink.
 * @GOWL_BAR_COLOR_SAPPHIRE: semantic sapphire.
 * @GOWL_BAR_COLOR_SKY: semantic sky.
 * @GOWL_BAR_COLOR_LAVENDER: semantic lavender.
 * @GOWL_BAR_COLOR_FLAMINGO: semantic flamingo.
 * @GOWL_BAR_COLOR_ROSEWATER: semantic rosewater.
 * @GOWL_BAR_COLOR_MAROON: semantic maroon.
 * @GOWL_BAR_COLOR_COUNT: sentinel; number of roles.
 *
 * The colour roles a bar plugin may name.  A plugin never writes a hex
 * literal: it names a role, and the theme decides what that role looks
 * like under the session's palette.  That is what makes one
 * `palette: {name: latte}' restyle every plugin, shipped or third
 * party, without any of them knowing latte exists.
 */
typedef enum {
	GOWL_BAR_COLOR_BASE = 0,
	GOWL_BAR_COLOR_MANTLE,
	GOWL_BAR_COLOR_CRUST,
	GOWL_BAR_COLOR_SURFACE,
	GOWL_BAR_COLOR_SURFACE_ALT,
	GOWL_BAR_COLOR_OVERLAY,
	GOWL_BAR_COLOR_TEXT,
	GOWL_BAR_COLOR_SUBTEXT,
	GOWL_BAR_COLOR_MUTED,
	GOWL_BAR_COLOR_ACCENT,
	GOWL_BAR_COLOR_RED,
	GOWL_BAR_COLOR_GREEN,
	GOWL_BAR_COLOR_YELLOW,
	GOWL_BAR_COLOR_BLUE,
	GOWL_BAR_COLOR_MAUVE,
	GOWL_BAR_COLOR_TEAL,
	GOWL_BAR_COLOR_PEACH,
	GOWL_BAR_COLOR_PINK,
	GOWL_BAR_COLOR_SAPPHIRE,
	GOWL_BAR_COLOR_SKY,
	GOWL_BAR_COLOR_LAVENDER,
	GOWL_BAR_COLOR_FLAMINGO,
	GOWL_BAR_COLOR_ROSEWATER,
	GOWL_BAR_COLOR_MAROON,
	GOWL_BAR_COLOR_COUNT
} GowlBarColor;

/**
 * GowlBarMetric:
 * @GOWL_BAR_METRIC_RADIUS: corner radius of panels and rows.
 * @GOWL_BAR_METRIC_PAD_X: horizontal padding inside a panel.
 * @GOWL_BAR_METRIC_PAD_Y: vertical padding inside a panel.
 * @GOWL_BAR_METRIC_GAP: vertical gap between panel items.
 * @GOWL_BAR_METRIC_ROW_HEIGHT: height of a list row.
 * @GOWL_BAR_METRIC_ITEM_PAD: horizontal padding inside a bar item.
 * @GOWL_BAR_METRIC_ITEM_GAP: gap between adjacent bar items.
 * @GOWL_BAR_METRIC_BORDER: panel border width.
 * @GOWL_BAR_METRIC_PANEL_WIDTH: default panel width.
 * @GOWL_BAR_METRIC_PANEL_OFFSET: gap between the bar and its panel.
 * @GOWL_BAR_METRIC_SHADOW: drop-shadow spread; 0 disables it.
 * @GOWL_BAR_METRIC_TOAST_WIDTH: width of a toast card.
 * @GOWL_BAR_METRIC_COUNT: sentinel; number of metrics.
 *
 * Pixel metrics, all scalable together with
 * gowl_bar_theme_set_scale().
 */
typedef enum {
	GOWL_BAR_METRIC_RADIUS = 0,
	GOWL_BAR_METRIC_PAD_X,
	GOWL_BAR_METRIC_PAD_Y,
	GOWL_BAR_METRIC_GAP,
	GOWL_BAR_METRIC_ROW_HEIGHT,
	GOWL_BAR_METRIC_ITEM_PAD,
	GOWL_BAR_METRIC_ITEM_GAP,
	GOWL_BAR_METRIC_BORDER,
	GOWL_BAR_METRIC_PANEL_WIDTH,
	GOWL_BAR_METRIC_PANEL_OFFSET,
	GOWL_BAR_METRIC_SHADOW,
	GOWL_BAR_METRIC_TOAST_WIDTH,
	GOWL_BAR_METRIC_COUNT
} GowlBarMetric;

#define GOWL_TYPE_BAR_THEME (gowl_bar_theme_get_type())

typedef struct _GowlBarTheme GowlBarTheme;

GType gowl_bar_theme_get_type (void) G_GNUC_CONST;

/**
 * gowl_bar_theme_new:
 *
 * Creates a theme with the Catppuccin Mocha defaults.
 *
 * Returns: (transfer full): a new #GowlBarTheme.  Free with
 *   gowl_bar_theme_free().
 */
GowlBarTheme *gowl_bar_theme_new (void);

/**
 * gowl_bar_theme_new_for_palette:
 * @palette: (nullable): the session palette, or %NULL for Mocha
 *
 * Creates a theme whose colour roles are resolved out of @palette.  A
 * role the palette does not name keeps the Mocha default, so a partial
 * palette is always safe.
 *
 * Returns: (transfer full): a new #GowlBarTheme.
 */
GowlBarTheme *gowl_bar_theme_new_for_palette (const GowlPalette *palette);

/**
 * gowl_bar_theme_apply_palette:
 * @self: a #GowlBarTheme
 * @palette: (nullable): the session palette
 *
 * Re-resolves every role that has not been explicitly overridden by
 * gowl_bar_theme_set_color() against @palette.  An explicit override
 * survives a palette change --- the user asked for that exact colour.
 */
void gowl_bar_theme_apply_palette (GowlBarTheme      *self,
                                    const GowlPalette *palette);

GowlBarTheme *gowl_bar_theme_copy (const GowlBarTheme *self);
void          gowl_bar_theme_free (GowlBarTheme       *self);

/**
 * gowl_bar_theme_color:
 * @self: a #GowlBarTheme
 * @role: the role to look up
 *
 * Returns: (array fixed-size=4) (transfer none): the role's premultiplied-
 *   ready RGBA components, each 0.0--1.0.  Never %NULL; an out-of-range
 *   @role gives text.
 */
const gdouble *gowl_bar_theme_color (const GowlBarTheme *self,
                                      GowlBarColor        role);

/**
 * gowl_bar_theme_set_color:
 * @self: a #GowlBarTheme
 * @role: the role to override
 * @spec: a colour spec: `#rrggbb', `#rrggbbaa', or a palette name
 *
 * Pins @role to @spec.  Marks the role as user-set, so a later
 * gowl_bar_theme_apply_palette() leaves it alone.
 */
void gowl_bar_theme_set_color (GowlBarTheme *self,
                                GowlBarColor  role,
                                const gchar  *spec);

/**
 * gowl_bar_theme_color_from_name:
 * @name: a role name such as `accent' or `red'
 * @out: (out): the matching role
 *
 * Returns: %TRUE when @name is a known role.
 */
gboolean gowl_bar_theme_color_from_name (const gchar  *name,
                                          GowlBarColor *out);

/**
 * gowl_bar_theme_color_name:
 * @role: a role
 *
 * Returns: (transfer none): the role's canonical name.
 */
const gchar *gowl_bar_theme_color_name (GowlBarColor role);

gint gowl_bar_theme_metric     (const GowlBarTheme *self,
                                 GowlBarMetric       metric);
void gowl_bar_theme_set_metric (GowlBarTheme *self,
                                 GowlBarMetric metric,
                                 gint          value);

/**
 * gowl_bar_theme_set_scale:
 * @self: a #GowlBarTheme
 * @scale: a multiplier, clamped to 0.5--4.0
 *
 * Scales every metric from its base value at once, so a HiDPI or
 * large-text session does not need twelve separate settings.
 */
void    gowl_bar_theme_set_scale (GowlBarTheme *self, gdouble scale);
gdouble gowl_bar_theme_get_scale (const GowlBarTheme *self);

const gchar *gowl_bar_theme_get_font      (const GowlBarTheme *self);
void         gowl_bar_theme_set_font      (GowlBarTheme *self,
                                            const gchar  *font);
const gchar *gowl_bar_theme_get_icon_font (const GowlBarTheme *self);
void         gowl_bar_theme_set_icon_font (GowlBarTheme *self,
                                            const gchar  *font);

/**
 * gowl_bar_theme_apply_setting:
 * @self: a #GowlBarTheme
 * @key: a configuration key
 * @value: its value
 *
 * Applies one `theme-*' configuration key.  Recognised forms:
 *
 * - `theme-<role>' --- a colour role, e.g. `theme-accent'.
 * - `theme-metric-<metric>' --- a pixel metric, e.g.
 *   `theme-metric-radius'.
 * - `theme-font', `theme-icon-font', `theme-scale'.
 *
 * Returns: %TRUE when @key was recognised and applied.
 */
gboolean gowl_bar_theme_apply_setting (GowlBarTheme *self,
                                        const gchar  *key,
                                        const gchar  *value);

/* --- cairo helpers ------------------------------------------------ */

void gowl_bar_theme_cairo_set (const GowlBarTheme *self,
                                cairo_t            *cr,
                                GowlBarColor        role);

/**
 * gowl_bar_theme_cairo_set_alpha:
 * @self: a #GowlBarTheme
 * @cr: a cairo context
 * @role: the role to select
 * @alpha: an extra multiplier applied to the role's own alpha
 *
 * Selects @role as the source, scaled to @alpha.  Used for hover
 * washes and disabled text without adding a role for each.
 */
void gowl_bar_theme_cairo_set_alpha (const GowlBarTheme *self,
                                      cairo_t            *cr,
                                      GowlBarColor        role,
                                      gdouble             alpha);

/**
 * gowl_bar_cairo_rounded_rect:
 * @cr: a cairo context
 * @x: left edge
 * @y: top edge
 * @w: width
 * @h: height
 * @radius: corner radius; clamped to half the shorter side
 *
 * Appends a rounded rectangle to the current path.
 */
void gowl_bar_cairo_rounded_rect (cairo_t *cr,
                                   gdouble  x,
                                   gdouble  y,
                                   gdouble  w,
                                   gdouble  h,
                                   gdouble  radius);

/**
 * gowl_bar_color_parse:
 * @spec: `#rrggbb' or `#rrggbbaa'
 * @rgba: (out caller-allocates) (array fixed-size=4): the components
 *
 * Parses a hex colour into normalised components.  An unparseable spec
 * leaves @rgba untouched.
 *
 * Returns: %TRUE when @spec parsed.
 */
gboolean gowl_bar_color_parse (const gchar *spec, gdouble rgba[4]);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlBarTheme, gowl_bar_theme_free)

G_END_DECLS

#endif /* GOWL_BAR_THEME_H */
