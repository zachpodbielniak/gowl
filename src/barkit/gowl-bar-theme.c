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

#include "barkit/gowl-bar-theme.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/**
 * SECTION:gowl-bar-theme
 * @title: GowlBarTheme
 * @short_description: the colour and metric vocabulary bar plugins draw with
 *
 * A plugin names a #GowlBarColor role; the theme decides what the role
 * looks like.  Roles resolve out of the session #GowlPalette, so the
 * bar, its panels, its toasts and every third-party plugin follow one
 * `palette:' setting.  Catppuccin Mocha is the default because it is
 * what the shipped configuration uses, but nothing in the drawing code
 * knows that.
 */

/* ----------------------------------------------------------------
 * Role table
 *
 * Each role names the palette entry it prefers plus the literal to
 * fall back on when the palette does not carry that entry.  The
 * fallbacks are Catppuccin Mocha, so a NULL palette still produces the
 * shipped look rather than black-on-black.
 * ---------------------------------------------------------------- */

typedef struct {
	const gchar *name;
	const gchar *palette_key;
	const gchar *fallback;
} GowlBarRoleDef;

static const GowlBarRoleDef role_defs[GOWL_BAR_COLOR_COUNT] = {
	{ "base",        "base",      "#1e1e2e" },
	{ "mantle",      "mantle",    "#181825" },
	{ "crust",       "crust",     "#11111b" },
	{ "surface",     "surface",   "#313244" },
	{ "surface-alt", "surface",   "#45475a" },
	{ "overlay",     "overlay",   "#6c7086" },
	{ "text",        "text",      "#cdd6f4" },
	{ "subtext",     "subtext",   "#a6adc8" },
	{ "muted",       "overlay",   "#7f849c" },
	{ "accent",      "accent",    "#89b4fa" },
	{ "red",         "red",       "#f38ba8" },
	{ "green",       "green",     "#a6e3a1" },
	{ "yellow",      "yellow",    "#f9e2af" },
	{ "blue",        "blue",      "#89b4fa" },
	{ "mauve",       "mauve",     "#cba6f7" },
	{ "teal",        "teal",      "#94e2d5" },
	{ "peach",       "peach",     "#fab387" },
	{ "pink",        "pink",      "#f5c2e7" },
	{ "sapphire",    "sapphire",  "#74c7ec" },
	{ "sky",         "sky",       "#89dceb" },
	{ "lavender",    "lavender",  "#b4befe" },
	{ "flamingo",    "flamingo",  "#f2cdcd" },
	{ "rosewater",   "rosewater", "#f5e0dc" },
	{ "maroon",      "maroon",    "#eba0ac" }
};

/* Base metric values, before the scale multiplier. */
static const gint metric_defaults[GOWL_BAR_METRIC_COUNT] = {
	8,    /* RADIUS */
	16,   /* PAD_X */
	14,   /* PAD_Y */
	8,    /* GAP */
	34,   /* ROW_HEIGHT */
	9,    /* ITEM_PAD */
	6,    /* ITEM_GAP */
	1,    /* BORDER */
	400,  /* PANEL_WIDTH */
	6,    /* PANEL_OFFSET */
	18,   /* SHADOW */
	380   /* TOAST_WIDTH */
};

/* Metric names as they appear in `theme-metric-<name>' settings. */
static const gchar *const metric_names[GOWL_BAR_METRIC_COUNT] = {
	"radius", "pad-x", "pad-y", "gap", "row-height", "item-pad",
	"item-gap", "border", "panel-width", "panel-offset", "shadow",
	"toast-width"
};

struct _GowlBarTheme {
	gdouble  colors[GOWL_BAR_COLOR_COUNT][4];
	/* A role the user pinned with set_color(); apply_palette() skips
	   these so a palette switch cannot silently undo an explicit
	   choice made in the config. */
	gboolean pinned[GOWL_BAR_COLOR_COUNT];
	gint     metrics[GOWL_BAR_METRIC_COUNT];
	gdouble  scale;
	gchar   *font;
	gchar   *icon_font;
};

G_DEFINE_BOXED_TYPE(GowlBarTheme, gowl_bar_theme,
                    gowl_bar_theme_copy, gowl_bar_theme_free)

/**
 * gowl_bar_color_parse:
 * @spec: `#rrggbb' or `#rrggbbaa'
 * @rgba: (out caller-allocates) (array fixed-size=4): the components
 *
 * Parses a hex colour into normalised components.
 *
 * Returns: %TRUE when @spec parsed.
 */
gboolean
gowl_bar_color_parse(const gchar *spec, gdouble rgba[4])
{
	gsize   len;
	guint32 val;
	gchar  *end;

	if (spec == NULL || spec[0] != '#')
		return FALSE;

	len = strlen(spec);
	if (len != 7 && len != 9)
		return FALSE;

	end = NULL;
	val = (guint32)g_ascii_strtoull(spec + 1, &end, 16);
	if (end == NULL || *end != '\0')
		return FALSE;

	if (len == 9) {
		rgba[0] = (gdouble)((val >> 24) & 0xFF) / 255.0;
		rgba[1] = (gdouble)((val >> 16) & 0xFF) / 255.0;
		rgba[2] = (gdouble)((val >>  8) & 0xFF) / 255.0;
		rgba[3] = (gdouble)((val      ) & 0xFF) / 255.0;
	} else {
		rgba[0] = (gdouble)((val >> 16) & 0xFF) / 255.0;
		rgba[1] = (gdouble)((val >>  8) & 0xFF) / 255.0;
		rgba[2] = (gdouble)((val      ) & 0xFF) / 255.0;
		rgba[3] = 1.0;
	}
	return TRUE;
}

/* Resolve one role against @palette, writing the result into @self.
   Pinned roles are left alone. */
static void
theme_resolve_role(GowlBarTheme *self, GowlBarColor role,
                   const GowlPalette *palette)
{
	g_autofree gchar *resolved = NULL;
	const GowlBarRoleDef *def;

	if (self->pinned[role])
		return;

	def = &role_defs[role];

	if (palette != NULL) {
		resolved = gowl_palette_resolve(palette, def->palette_key);
		if (resolved != NULL &&
		    gowl_bar_color_parse(resolved, self->colors[role]))
			return;
	}

	(void)gowl_bar_color_parse(def->fallback, self->colors[role]);
}

/**
 * gowl_bar_theme_new:
 *
 * Creates a theme with the Catppuccin Mocha defaults.
 *
 * Returns: (transfer full): a new #GowlBarTheme
 */
GowlBarTheme *
gowl_bar_theme_new(void)
{
	return gowl_bar_theme_new_for_palette(NULL);
}

/**
 * gowl_bar_theme_new_for_palette:
 * @palette: (nullable): the session palette, or %NULL for Mocha
 *
 * Creates a theme whose colour roles are resolved out of @palette.
 *
 * Returns: (transfer full): a new #GowlBarTheme
 */
GowlBarTheme *
gowl_bar_theme_new_for_palette(const GowlPalette *palette)
{
	GowlBarTheme *self;
	gint i;

	self = g_new0(GowlBarTheme, 1);
	self->scale     = 1.0;
	self->font      = g_strdup("monospace 11");
	self->icon_font = NULL;

	for (i = 0; i < GOWL_BAR_METRIC_COUNT; i++)
		self->metrics[i] = metric_defaults[i];

	for (i = 0; i < GOWL_BAR_COLOR_COUNT; i++)
		theme_resolve_role(self, (GowlBarColor)i, palette);

	return self;
}

/**
 * gowl_bar_theme_apply_palette:
 * @self: a #GowlBarTheme
 * @palette: (nullable): the session palette
 *
 * Re-resolves every unpinned role against @palette.
 */
void
gowl_bar_theme_apply_palette(GowlBarTheme *self, const GowlPalette *palette)
{
	gint i;

	g_return_if_fail(self != NULL);

	for (i = 0; i < GOWL_BAR_COLOR_COUNT; i++)
		theme_resolve_role(self, (GowlBarColor)i, palette);
}

/**
 * gowl_bar_theme_copy:
 * @self: (nullable): a #GowlBarTheme
 *
 * Returns: (transfer full) (nullable): a deep copy of @self
 */
GowlBarTheme *
gowl_bar_theme_copy(const GowlBarTheme *self)
{
	GowlBarTheme *copy;

	if (self == NULL)
		return NULL;

	copy = g_new0(GowlBarTheme, 1);
	memcpy(copy, self, sizeof(GowlBarTheme));
	copy->font      = g_strdup(self->font);
	copy->icon_font = g_strdup(self->icon_font);
	return copy;
}

/**
 * gowl_bar_theme_free:
 * @self: (nullable) (transfer full): a #GowlBarTheme
 *
 * Frees @self.
 */
void
gowl_bar_theme_free(GowlBarTheme *self)
{
	if (self == NULL)
		return;
	g_free(self->font);
	g_free(self->icon_font);
	g_free(self);
}

/**
 * gowl_bar_theme_color:
 * @self: a #GowlBarTheme
 * @role: the role to look up
 *
 * Returns: (array fixed-size=4) (transfer none): the role's RGBA
 */
const gdouble *
gowl_bar_theme_color(const GowlBarTheme *self, GowlBarColor role)
{
	static const gdouble white[4] = { 1.0, 1.0, 1.0, 1.0 };

	if (self == NULL)
		return white;
	if ((guint)role >= GOWL_BAR_COLOR_COUNT)
		role = GOWL_BAR_COLOR_TEXT;
	return self->colors[role];
}

/**
 * gowl_bar_theme_set_color:
 * @self: a #GowlBarTheme
 * @role: the role to override
 * @spec: a colour spec or palette name
 *
 * Pins @role to @spec.
 */
void
gowl_bar_theme_set_color(GowlBarTheme *self, GowlBarColor role,
                         const gchar *spec)
{
	g_return_if_fail(self != NULL);
	g_return_if_fail((guint)role < GOWL_BAR_COLOR_COUNT);

	if (spec == NULL)
		return;

	if (gowl_bar_color_parse(spec, self->colors[role])) {
		self->pinned[role] = TRUE;
		return;
	}

	/* Not a literal: treat it as another role's name, which lets a
	   config say `theme-accent: mauve' without repeating hex. */
	{
		GowlBarColor alias;

		if (gowl_bar_theme_color_from_name(spec, &alias) &&
		    alias != role) {
			memcpy(self->colors[role], self->colors[alias],
			       sizeof(self->colors[role]));
			self->pinned[role] = TRUE;
		}
	}
}

/**
 * gowl_bar_theme_color_from_name:
 * @name: a role name
 * @out: (out): the matching role
 *
 * Returns: %TRUE when @name is a known role
 */
gboolean
gowl_bar_theme_color_from_name(const gchar *name, GowlBarColor *out)
{
	gint i;

	if (name == NULL)
		return FALSE;

	for (i = 0; i < GOWL_BAR_COLOR_COUNT; i++) {
		if (g_ascii_strcasecmp(name, role_defs[i].name) == 0) {
			if (out != NULL)
				*out = (GowlBarColor)i;
			return TRUE;
		}
	}
	return FALSE;
}

/**
 * gowl_bar_theme_color_name:
 * @role: a role
 *
 * Returns: (transfer none): the role's canonical name
 */
const gchar *
gowl_bar_theme_color_name(GowlBarColor role)
{
	if ((guint)role >= GOWL_BAR_COLOR_COUNT)
		return "text";
	return role_defs[role].name;
}

/**
 * gowl_bar_theme_metric:
 * @self: a #GowlBarTheme
 * @metric: the metric to read
 *
 * Returns: the metric in pixels, after the scale multiplier
 */
gint
gowl_bar_theme_metric(const GowlBarTheme *self, GowlBarMetric metric)
{
	gint value;

	if (self == NULL || (guint)metric >= GOWL_BAR_METRIC_COUNT)
		return 0;

	value = (gint)lround((gdouble)self->metrics[metric] * self->scale);
	/* A metric that rounds to zero makes borders and gaps vanish
	   entirely at small scales; keep anything non-zero visible. */
	if (value < 1 && self->metrics[metric] > 0)
		value = 1;
	return value;
}

/**
 * gowl_bar_theme_set_metric:
 * @self: a #GowlBarTheme
 * @metric: the metric to set
 * @value: the new base value in pixels
 */
void
gowl_bar_theme_set_metric(GowlBarTheme *self, GowlBarMetric metric,
                          gint value)
{
	g_return_if_fail(self != NULL);
	g_return_if_fail((guint)metric < GOWL_BAR_METRIC_COUNT);

	if (value < 0)
		value = 0;
	self->metrics[metric] = value;
}

/**
 * gowl_bar_theme_set_scale:
 * @self: a #GowlBarTheme
 * @scale: a multiplier, clamped to 0.5--4.0
 */
void
gowl_bar_theme_set_scale(GowlBarTheme *self, gdouble scale)
{
	g_return_if_fail(self != NULL);

	if (scale < 0.5)
		scale = 0.5;
	if (scale > 4.0)
		scale = 4.0;
	self->scale = scale;
}

/**
 * gowl_bar_theme_get_scale:
 * @self: a #GowlBarTheme
 *
 * Returns: the metric multiplier
 */
gdouble
gowl_bar_theme_get_scale(const GowlBarTheme *self)
{
	return (self != NULL) ? self->scale : 1.0;
}

/**
 * gowl_bar_theme_get_font:
 * @self: a #GowlBarTheme
 *
 * Returns: (transfer none): the Pango font description for body text
 */
const gchar *
gowl_bar_theme_get_font(const GowlBarTheme *self)
{
	if (self == NULL || self->font == NULL)
		return "monospace 11";
	return self->font;
}

/**
 * gowl_bar_theme_set_font:
 * @self: a #GowlBarTheme
 * @font: a Pango font description string
 */
void
gowl_bar_theme_set_font(GowlBarTheme *self, const gchar *font)
{
	g_return_if_fail(self != NULL);

	if (font == NULL || font[0] == '\0')
		return;
	g_free(self->font);
	self->font = g_strdup(font);
}

/**
 * gowl_bar_theme_get_icon_font:
 * @self: a #GowlBarTheme
 *
 * The font used for glyph icons.  Falls back to the body font, which
 * is the right answer when the body font is already a Nerd Font.
 *
 * Returns: (transfer none): a Pango font description string
 */
const gchar *
gowl_bar_theme_get_icon_font(const GowlBarTheme *self)
{
	if (self == NULL)
		return "monospace 11";
	if (self->icon_font != NULL && self->icon_font[0] != '\0')
		return self->icon_font;
	return gowl_bar_theme_get_font(self);
}

/**
 * gowl_bar_theme_set_icon_font:
 * @self: a #GowlBarTheme
 * @font: (nullable): a Pango font description string, or %NULL to
 *   follow the body font
 */
void
gowl_bar_theme_set_icon_font(GowlBarTheme *self, const gchar *font)
{
	g_return_if_fail(self != NULL);

	g_free(self->icon_font);
	self->icon_font = (font != NULL && font[0] != '\0')
		? g_strdup(font) : NULL;
}

/**
 * gowl_bar_theme_apply_setting:
 * @self: a #GowlBarTheme
 * @key: a configuration key
 * @value: its value
 *
 * Applies one `theme-*' configuration key.
 *
 * Returns: %TRUE when @key was recognised
 */
gboolean
gowl_bar_theme_apply_setting(GowlBarTheme *self, const gchar *key,
                             const gchar *value)
{
	GowlBarColor role;
	gint i;

	g_return_val_if_fail(self != NULL, FALSE);

	if (key == NULL || value == NULL)
		return FALSE;
	if (strncmp(key, "theme-", 6) != 0)
		return FALSE;

	key += 6;

	if (strcmp(key, "font") == 0) {
		gowl_bar_theme_set_font(self, value);
		return TRUE;
	}
	if (strcmp(key, "icon-font") == 0) {
		gowl_bar_theme_set_icon_font(self, value);
		return TRUE;
	}
	if (strcmp(key, "scale") == 0) {
		gowl_bar_theme_set_scale(self, g_ascii_strtod(value, NULL));
		return TRUE;
	}

	if (strncmp(key, "metric-", 7) == 0) {
		const gchar *mname = key + 7;

		for (i = 0; i < GOWL_BAR_METRIC_COUNT; i++) {
			if (strcmp(mname, metric_names[i]) == 0) {
				gowl_bar_theme_set_metric(self,
					(GowlBarMetric)i,
					(gint)g_ascii_strtoll(value, NULL, 10));
				return TRUE;
			}
		}
		return FALSE;
	}

	if (gowl_bar_theme_color_from_name(key, &role)) {
		gowl_bar_theme_set_color(self, role, value);
		return TRUE;
	}

	return FALSE;
}

/**
 * gowl_bar_theme_cairo_set:
 * @self: a #GowlBarTheme
 * @cr: a cairo context
 * @role: the role to select as the source
 */
void
gowl_bar_theme_cairo_set(const GowlBarTheme *self, cairo_t *cr,
                         GowlBarColor role)
{
	const gdouble *c;

	if (cr == NULL)
		return;
	c = gowl_bar_theme_color(self, role);
	cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);
}

/**
 * gowl_bar_theme_cairo_set_alpha:
 * @self: a #GowlBarTheme
 * @cr: a cairo context
 * @role: the role to select
 * @alpha: an extra multiplier on the role's alpha
 */
void
gowl_bar_theme_cairo_set_alpha(const GowlBarTheme *self, cairo_t *cr,
                               GowlBarColor role, gdouble alpha)
{
	const gdouble *c;

	if (cr == NULL)
		return;
	c = gowl_bar_theme_color(self, role);
	cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3] * alpha);
}

/**
 * gowl_bar_cairo_rounded_rect:
 * @cr: a cairo context
 * @x: left edge
 * @y: top edge
 * @w: width
 * @h: height
 * @radius: corner radius
 *
 * Appends a rounded rectangle to the current path.
 */
void
gowl_bar_cairo_rounded_rect(cairo_t *cr, gdouble x, gdouble y,
                            gdouble w, gdouble h, gdouble radius)
{
	gdouble r;

	if (cr == NULL || w <= 0.0 || h <= 0.0)
		return;

	r = radius;
	if (r > w / 2.0)
		r = w / 2.0;
	if (r > h / 2.0)
		r = h / 2.0;
	if (r < 0.0)
		r = 0.0;

	if (r < 0.5) {
		cairo_rectangle(cr, x, y, w, h);
		return;
	}

	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r,     r, -G_PI / 2.0, 0.0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0.0, G_PI / 2.0);
	cairo_arc(cr, x + r,     y + h - r, r, G_PI / 2.0, G_PI);
	cairo_arc(cr, x + r,     y + r,     r, G_PI, 3.0 * G_PI / 2.0);
	cairo_close_path(cr);
}
