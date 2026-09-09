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

#include "barkit/gowl-bar-panel-render.h"

#include <math.h>
#include <string.h>

/**
 * SECTION:gowl-bar-panel-render
 * @title: Panel rendering
 * @short_description: turns a #GowlBarPanel into pixels and hit regions
 *
 * Measuring and drawing run through the same walk, with drawing
 * switched off for a measure pass.  Keeping them in one function is
 * what stops the two from disagreeing --- a panel whose measured
 * height does not match its drawn height either clips its last row or
 * leaves a gap under it, and that bug is invisible until some plugin
 * adds an item kind the measure pass forgot.
 */

/* Font size multipliers, relative to the theme's body font. */
#define HERO_TITLE_SCALE     1.85
#define HERO_ICON_SCALE      2.30
#define SECTION_SCALE        0.82
#define SMALL_SCALE          0.86
#define BADGE_SCALE          0.78

/* Letter spacing, in Pango units (1024ths of a point). */
#define SECTION_TRACKING     (2 * PANGO_SCALE)
#define HERO_SUBTITLE_TRACK  (2 * PANGO_SCALE)

typedef struct {
	GowlBarPanel          *panel;
	cairo_t               *cr;         /* NULL during a measure pass */
	PangoLayout           *layout;
	const GowlBarTheme    *theme;
	GowlBarPanelRenderCtx *ctx;

	PangoFontDescription  *body_font;
	PangoFontDescription  *icon_font;
	gint                   base_size;  /* body font size, Pango units */

	gint                   pad_x;
	gint                   pad_y;
	gint                   gap;
	gint                   row_h;
	gint                   radius;
	gint                   width;      /* content width */
	gint                   inner_w;    /* width minus horizontal padding */
} PanelPass;

/* ----------------------------------------------------------------
 * Text helpers
 * ---------------------------------------------------------------- */

/* Apply a scaled copy of the body font to the shared layout.  The
   layout is reused across every draw in a pass, so the font must be
   set before each measurement rather than once up front. */
static void
pass_set_font(PanelPass *p, gdouble scale, gboolean icon, gboolean bold)
{
	PangoFontDescription *desc;

	desc = pango_font_description_copy(icon ? p->icon_font : p->body_font);
	pango_font_description_set_size(desc,
		(gint)((gdouble)p->base_size * scale));
	pango_font_description_set_weight(desc,
		bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
	pango_layout_set_font_description(p->layout, desc);
	pango_font_description_free(desc);
}

/* Measure @text with whatever font is currently on the layout. */
static void
pass_measure_text(PanelPass *p, const gchar *text, gint *w, gint *h)
{
	PangoRectangle logical;

	pango_layout_set_width(p->layout, -1);
	pango_layout_set_ellipsize(p->layout, PANGO_ELLIPSIZE_NONE);
	pango_layout_set_attributes(p->layout, NULL);
	pango_layout_set_text(p->layout, (text != NULL) ? text : "", -1);
	pango_layout_get_pixel_extents(p->layout, NULL, &logical);
	if (w != NULL)
		*w = logical.width;
	if (h != NULL)
		*h = logical.height;
}

/* Draw @text at (@x, @y) in @role, vertically centred on @center_y
   when @center_y is non-negative.  @max_w of 0 means no ellipsis. */
static gint
pass_draw_text(PanelPass *p, const gchar *text, gint x, gint center_y,
               GowlBarColor role, gdouble alpha, gint max_w,
               PangoAlignment align)
{
	PangoRectangle logical;
	gint y;

	if (p->cr == NULL || text == NULL || text[0] == '\0')
		return 0;

	pango_layout_set_attributes(p->layout, NULL);
	pango_layout_set_text(p->layout, text, -1);

	if (max_w > 0) {
		pango_layout_set_width(p->layout, max_w * PANGO_SCALE);
		pango_layout_set_ellipsize(p->layout, PANGO_ELLIPSIZE_END);
		pango_layout_set_alignment(p->layout, align);
	} else {
		pango_layout_set_width(p->layout, -1);
		pango_layout_set_ellipsize(p->layout, PANGO_ELLIPSIZE_NONE);
		pango_layout_set_alignment(p->layout, PANGO_ALIGN_LEFT);
	}

	pango_layout_get_pixel_extents(p->layout, NULL, &logical);
	y = center_y - logical.height / 2;

	gowl_bar_theme_cairo_set_alpha(p->theme, p->cr, role, alpha);
	cairo_move_to(p->cr, x, y);
	pango_cairo_show_layout(p->cr, p->layout);

	return logical.width;
}

/* Draw @text with letter spacing --- the wide dim caps the panel
   headers and hero subtitles use. */
static void
pass_draw_tracked(PanelPass *p, const gchar *text, gint x, gint center_y,
                  GowlBarColor role, gdouble alpha, gint tracking,
                  gint max_w)
{
	PangoAttrList  *attrs;
	PangoAttribute *attr;
	PangoRectangle  logical;

	if (p->cr == NULL || text == NULL || text[0] == '\0')
		return;

	attrs = pango_attr_list_new();
	attr = pango_attr_letter_spacing_new(tracking);
	pango_attr_list_insert(attrs, attr);

	pango_layout_set_text(p->layout, text, -1);
	pango_layout_set_attributes(p->layout, attrs);
	if (max_w > 0) {
		pango_layout_set_width(p->layout, max_w * PANGO_SCALE);
		pango_layout_set_ellipsize(p->layout, PANGO_ELLIPSIZE_END);
	} else {
		pango_layout_set_width(p->layout, -1);
		pango_layout_set_ellipsize(p->layout, PANGO_ELLIPSIZE_NONE);
	}
	pango_layout_set_alignment(p->layout, PANGO_ALIGN_LEFT);
	pango_layout_get_pixel_extents(p->layout, NULL, &logical);

	gowl_bar_theme_cairo_set_alpha(p->theme, p->cr, role, alpha);
	cairo_move_to(p->cr, x, center_y - logical.height / 2);
	pango_cairo_show_layout(p->cr, p->layout);

	pango_layout_set_attributes(p->layout, NULL);
	pango_attr_list_unref(attrs);
}

/* Lay @text out wrapped inside @width and return the height it needs.
   Never draws: this is called from the height calculation, which runs
   in the draw pass too, so a version that painted would stamp every
   paragraph at the origin as well as in its place. */
static gint
pass_measure_wrapped(PanelPass *p, const gchar *text, gint width)
{
	PangoRectangle logical;

	if (text == NULL || text[0] == '\0' || width <= 0)
		return 0;

	pango_layout_set_attributes(p->layout, NULL);
	pango_layout_set_alignment(p->layout, PANGO_ALIGN_LEFT);
	pango_layout_set_ellipsize(p->layout, PANGO_ELLIPSIZE_NONE);
	pango_layout_set_width(p->layout, width * PANGO_SCALE);
	pango_layout_set_wrap(p->layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_text(p->layout, text, -1);
	pango_layout_get_pixel_extents(p->layout, NULL, &logical);

	/* Leave the layout as the rest of the walk expects to find it. */
	pango_layout_set_wrap(p->layout, PANGO_WRAP_WORD);
	pango_layout_set_width(p->layout, -1);

	return logical.height;
}

/* Draw @text wrapped inside @width, with its top at @y. */
static void
pass_draw_wrapped(PanelPass *p, const gchar *text, gint x, gint y,
                  gint width, GowlBarColor role, gdouble alpha)
{
	if (p->cr == NULL || text == NULL || text[0] == '\0' || width <= 0)
		return;

	pango_layout_set_attributes(p->layout, NULL);
	pango_layout_set_alignment(p->layout, PANGO_ALIGN_LEFT);
	pango_layout_set_ellipsize(p->layout, PANGO_ELLIPSIZE_NONE);
	pango_layout_set_width(p->layout, width * PANGO_SCALE);
	pango_layout_set_wrap(p->layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_text(p->layout, text, -1);

	gowl_bar_theme_cairo_set_alpha(p->theme, p->cr, role, alpha);
	cairo_move_to(p->cr, x, y);
	pango_cairo_show_layout(p->cr, p->layout);

	pango_layout_set_wrap(p->layout, PANGO_WRAP_WORD);
	pango_layout_set_width(p->layout, -1);
}

/* Right-align @text so its right edge lands on @right.
 *
 * The box is placed at @right - @box_w and Pango aligns inside it.
 * Placing it at @right minus the *measured* width while also handing
 * the layout a wider box pushes the text right by the difference --
 * which is how a two-column readout ends up with its values in the
 * next column, or outside the panel entirely. */
static void
pass_draw_text_right(PanelPass *p, const gchar *text, gint right,
                     gint center_y, GowlBarColor role, gdouble alpha,
                     gint box_w)
{
	if (p->cr == NULL || text == NULL || text[0] == '\0')
		return;
	if (box_w <= 0)
		return;

	pass_draw_text(p, text, right - box_w, center_y, role, alpha, box_w,
	               PANGO_ALIGN_RIGHT);
}

/* ----------------------------------------------------------------
 * Hit regions
 * ---------------------------------------------------------------- */

void
gowl_bar_hit_rect_clear(gpointer data)
{
	GowlBarHitRect *rect = data;

	g_clear_pointer(&rect->id, g_free);
}

static void
pass_emit_hit(PanelPass *p, gint item_index, gint child_index,
              GowlBarItemKind kind, const gchar *id,
              gint x, gint y, gint w, gint h)
{
	GowlBarHitRect rect;

	if (p->ctx->hits == NULL || id == NULL)
		return;

	rect.item_index  = item_index;
	rect.child_index = child_index;
	rect.kind        = kind;
	rect.id          = g_strdup(id);
	rect.x           = x;
	rect.y           = y;
	rect.width       = w;
	rect.height      = h;
	g_array_append_val(p->ctx->hits, rect);
}

/* ----------------------------------------------------------------
 * Item primitives
 * ---------------------------------------------------------------- */

/* Fill a row-shaped highlight behind an item.  Hover and selection use
   the same shape at different weights so a keyboard cursor and a mouse
   pointer never look like two different affordances. */
static void
pass_row_background(PanelPass *p, gint x, gint y, gint w, gint h,
                    gboolean hovered, gboolean selected, gboolean active)
{
	if (p->cr == NULL)
		return;
	if (!hovered && !selected && !active)
		return;

	gowl_bar_cairo_rounded_rect(p->cr, x, y, w, h, p->radius);
	if (active)
		gowl_bar_theme_cairo_set_alpha(p->theme, p->cr,
			GOWL_BAR_COLOR_SURFACE, 1.0);
	else if (selected)
		gowl_bar_theme_cairo_set_alpha(p->theme, p->cr,
			GOWL_BAR_COLOR_SURFACE_ALT, 0.85);
	else
		gowl_bar_theme_cairo_set_alpha(p->theme, p->cr,
			GOWL_BAR_COLOR_SURFACE, 0.55);
	cairo_fill(p->cr);

	if (selected) {
		/* A 2px accent rail on the leading edge: the keyboard cursor
		   has to be findable at a glance in a long list, and a fill
		   alone is too close to the hover wash. */
		gowl_bar_theme_cairo_set(p->theme, p->cr,
		                         GOWL_BAR_COLOR_ACCENT);
		cairo_rectangle(p->cr, x, y + 3, 2, h - 6);
		cairo_fill(p->cr);
	}
}

/* Draw a pill switch.  Returns its width. */
static gint
pass_draw_switch(PanelPass *p, gint right, gint center_y, gboolean on,
                 gboolean busy)
{
	gdouble w, h, r, knob_x;

	h = (gdouble)p->row_h * 0.55;
	w = h * 1.85;
	r = h / 2.0;

	if (p->cr == NULL)
		return (gint)w;

	gowl_bar_cairo_rounded_rect(p->cr, right - w, center_y - h / 2.0,
	                            w, h, r);
	if (on)
		gowl_bar_theme_cairo_set(p->theme, p->cr,
		                         GOWL_BAR_COLOR_ACCENT);
	else
		gowl_bar_theme_cairo_set_alpha(p->theme, p->cr,
			GOWL_BAR_COLOR_SURFACE_ALT, 1.0);
	cairo_fill(p->cr);

	knob_x = on ? (right - r - h * 0.16) : (right - w + r + h * 0.16);

	gowl_bar_theme_cairo_set_alpha(p->theme, p->cr,
		on ? GOWL_BAR_COLOR_BASE : GOWL_BAR_COLOR_SUBTEXT,
		busy ? 0.5 : 1.0);
	cairo_arc(p->cr, knob_x, center_y, r * 0.68, 0.0, 2.0 * G_PI);
	cairo_fill(p->cr);

	return (gint)w;
}

/* Draw a slider track with its filled portion and knob. */
static void
pass_draw_track(PanelPass *p, gint x, gint center_y, gint w,
                gdouble fraction, GowlBarColor fill_role, gboolean knob)
{
	gdouble th, filled;

	th = 6.0;
	filled = (gdouble)w * fraction;

	if (p->cr == NULL)
		return;

	gowl_bar_cairo_rounded_rect(p->cr, x, center_y - th / 2.0,
	                            w, th, th / 2.0);
	gowl_bar_theme_cairo_set_alpha(p->theme, p->cr,
		GOWL_BAR_COLOR_SURFACE_ALT, 1.0);
	cairo_fill(p->cr);

	if (filled > 0.5) {
		gowl_bar_cairo_rounded_rect(p->cr, x, center_y - th / 2.0,
		                            filled, th, th / 2.0);
		gowl_bar_theme_cairo_set(p->theme, p->cr, fill_role);
		cairo_fill(p->cr);
	}

	if (knob) {
		gowl_bar_theme_cairo_set(p->theme, p->cr, GOWL_BAR_COLOR_TEXT);
		cairo_arc(p->cr, x + filled, center_y, th, 0.0, 2.0 * G_PI);
		cairo_fill(p->cr);
	}
}

/* ----------------------------------------------------------------
 * Per-kind height
 * ---------------------------------------------------------------- */

static gint
pass_item_height(PanelPass *p, GowlBarPanelItem *item)
{
	gint override;

	override = gowl_bar_panel_item_get_height(item);
	if (override > 0)
		return override;

	switch (gowl_bar_panel_item_get_kind(item)) {
	case GOWL_BAR_ITEM_HERO:
		return (gint)((gdouble)p->row_h * 1.7);
	case GOWL_BAR_ITEM_SECTION:
		return (gint)((gdouble)p->row_h * 0.78);
	case GOWL_BAR_ITEM_LABEL: {
		/* A label carries prose, so its height is whatever the
		   prose wraps to.  Measured here rather than assumed,
		   which is what lets an explanation be a sentence instead
		   of a truncated line. */
		gint text_h;

		pass_set_font(p, 1.0, FALSE, FALSE);
		text_h = pass_measure_wrapped(p,
			gowl_bar_panel_item_get_title(item), p->inner_w);
		if (text_h <= 0)
			return (gint)((gdouble)p->row_h * 0.68);
		return text_h + p->gap / 2;
	}
	case GOWL_BAR_ITEM_FIELD:
	case GOWL_BAR_ITEM_FIELD_PAIR:
		return (gint)((gdouble)p->row_h * 0.68);
	case GOWL_BAR_ITEM_SEPARATOR:
		return p->gap * 2 + 1;
	case GOWL_BAR_ITEM_SLIDER:
		return (gint)((gdouble)p->row_h * 1.35);
	case GOWL_BAR_ITEM_ROW:
		/* Two stacked lines need more than one line's row. */
		if (gowl_bar_panel_item_get_subtitle(item) != NULL &&
		    gowl_bar_panel_item_get_subtitle(item)[0] != '\0')
			return (gint)((gdouble)p->row_h * 1.24);
		return p->row_h;
	case GOWL_BAR_ITEM_TOGGLE:
	case GOWL_BAR_ITEM_BUTTONS:
		return p->row_h;
	case GOWL_BAR_ITEM_PROGRESS:
		return (gint)((gdouble)p->row_h * 0.9);
	case GOWL_BAR_ITEM_GRAPH:
		return (gint)((gdouble)p->row_h * 1.6);
	case GOWL_BAR_ITEM_CALENDAR:
		/* Header row plus six week rows; a month never needs more
		   and always fitting six stops the panel resizing as the
		   user pages through months. */
		return (gint)((gdouble)p->row_h * 0.72) * 7;
	case GOWL_BAR_ITEM_SPACER:
		return p->gap;
	default:
		return p->row_h;
	}
}

/* A row kind draws its own background and so wants no extra gap
   crowding it; static text kinds read better tight. */
static gint
pass_item_gap(PanelPass *p, GowlBarPanelItem *item)
{
	switch (gowl_bar_panel_item_get_kind(item)) {
	case GOWL_BAR_ITEM_SEPARATOR:
	case GOWL_BAR_ITEM_SPACER:
		return 0;
	case GOWL_BAR_ITEM_LABEL:
	case GOWL_BAR_ITEM_FIELD:
	case GOWL_BAR_ITEM_FIELD_PAIR:
		return p->gap / 3;
	default:
		return p->gap / 2;
	}
}

/* ----------------------------------------------------------------
 * Per-kind draw
 * ---------------------------------------------------------------- */

static void
pass_draw_hero(PanelPass *p, GowlBarPanelItem *item, gint index,
               gint y, gint h)
{
	const gchar *icon;
	gint x, text_x, center, icon_w;
	gint right;

	x = p->pad_x;
	right = p->width - p->pad_x;
	center = y + h / 2;
	text_x = x;

	icon = gowl_bar_panel_item_get_icon(item);
	if (icon != NULL && icon[0] != '\0') {
		pass_set_font(p, HERO_ICON_SCALE, TRUE, FALSE);
		pass_measure_text(p, icon, &icon_w, NULL);
		pass_draw_text(p, icon, x, center,
			gowl_bar_panel_item_has_color(item)
				? gowl_bar_panel_item_get_color(item)
				: GOWL_BAR_COLOR_ACCENT,
			gowl_bar_panel_item_get_disabled(item) ? 0.45 : 1.0,
			0, PANGO_ALIGN_LEFT);
		text_x = x + icon_w + p->pad_x;
	}

	/* The trailing switch, when the hero carries one.  It is what the
	   network and tailscale panels put their power control on, so it
	   is reserved before the title measures its width. */
	if (gowl_bar_panel_item_get_id(item) != NULL) {
		gint sw;

		sw = pass_draw_switch(p, right, center,
		                      gowl_bar_panel_item_get_active(item),
		                      gowl_bar_panel_item_get_busy(item));
		pass_emit_hit(p, index, -1, GOWL_BAR_ITEM_HERO,
		              gowl_bar_panel_item_get_id(item),
		              right - sw - 6, center - p->row_h / 2,
		              sw + 12, p->row_h);
		right -= sw + p->pad_x;
	}

	{
		g_autofree gchar *upper = NULL;
		const gchar *sub;
		gint title_h, sub_h, top;

		sub = gowl_bar_panel_item_get_subtitle(item);
		if (sub != NULL && sub[0] != '\0')
			upper = g_utf8_strup(sub, -1);

		pass_set_font(p, HERO_TITLE_SCALE, FALSE, TRUE);
		pass_measure_text(p, gowl_bar_panel_item_get_title(item),
		                  NULL, &title_h);
		sub_h = 0;
		if (upper != NULL) {
			pass_set_font(p, SECTION_SCALE, FALSE, FALSE);
			pass_measure_text(p, upper, NULL, &sub_h);
		}

		top = center - (title_h + sub_h) / 2;

		pass_set_font(p, HERO_TITLE_SCALE, FALSE, TRUE);
		pass_draw_text(p, gowl_bar_panel_item_get_title(item), text_x,
		               top + title_h / 2, GOWL_BAR_COLOR_TEXT, 1.0,
		               right - text_x, PANGO_ALIGN_LEFT);

		if (upper != NULL) {
			pass_set_font(p, SECTION_SCALE, FALSE, FALSE);
			pass_draw_tracked(p, upper, text_x,
				top + title_h + sub_h / 2,
				GOWL_BAR_COLOR_MUTED, 1.0,
				HERO_SUBTITLE_TRACK, right - text_x);
		}
	}
}

static void
pass_draw_section(PanelPass *p, GowlBarPanelItem *item, gint index,
                  gint y, gint h)
{
	g_autofree gchar *upper = NULL;
	const gchar *title;
	gint center;

	center = y + h / 2;
	title = gowl_bar_panel_item_get_title(item);

	pass_set_font(p, SECTION_SCALE, FALSE, FALSE);
	if (title != NULL) {
		upper = g_utf8_strup(title, -1);
		pass_draw_tracked(p, upper, p->pad_x, center,
		                  GOWL_BAR_COLOR_MUTED, 1.0,
		                  SECTION_TRACKING, p->inner_w);
	}
	if (gowl_bar_panel_item_get_value(item) != NULL) {
		pass_draw_text_right(p, gowl_bar_panel_item_get_value(item),
		                     p->width - p->pad_x, center,
		                     GOWL_BAR_COLOR_MUTED, 1.0, p->inner_w / 2);
	}
	(void)index;
}

static void
pass_draw_field(PanelPass *p, GowlBarPanelItem *item, gint y, gint h,
                gboolean pair)
{
	gint center, half, col_w;

	center = y + h / 2;
	pass_set_font(p, SMALL_SCALE, FALSE, FALSE);

	if (!pair) {
		pass_draw_text(p, gowl_bar_panel_item_get_title(item),
		               p->pad_x, center, GOWL_BAR_COLOR_MUTED, 1.0,
		               p->inner_w / 2, PANGO_ALIGN_LEFT);
		pass_draw_text_right(p, gowl_bar_panel_item_get_value(item),
			p->width - p->pad_x, center,
			gowl_bar_panel_item_get_value_color(item), 1.0,
			p->inner_w / 2);
		return;
	}

	half  = p->inner_w / 2;
	col_w = half - p->gap;

	/* Label left, value right, each in half the column: a label and a
	   value that both run long would otherwise meet in the middle. */
	pass_draw_text(p, gowl_bar_panel_item_get_title(item), p->pad_x,
	               center, GOWL_BAR_COLOR_MUTED, 1.0, col_w / 2,
	               PANGO_ALIGN_LEFT);
	pass_draw_text_right(p, gowl_bar_panel_item_get_value(item),
		p->pad_x + col_w, center,
		gowl_bar_panel_item_get_value_color(item), 1.0, col_w / 2);

	pass_draw_text(p, gowl_bar_panel_item_get_title2(item),
	               p->pad_x + half + p->gap, center,
	               GOWL_BAR_COLOR_MUTED, 1.0, col_w / 2,
	               PANGO_ALIGN_LEFT);
	pass_draw_text_right(p, gowl_bar_panel_item_get_value2(item),
		p->width - p->pad_x, center,
		gowl_bar_panel_item_get_value_color(item), 1.0, col_w / 2);
}

static void
pass_draw_row(PanelPass *p, GowlBarPanelItem *item, gint index,
              gint y, gint h, gboolean hovered)
{
	const gchar *icon, *subtitle, *value, *badge;
	gint x, center, icon_w, right;
	gdouble alpha;

	alpha = gowl_bar_panel_item_get_disabled(item) ? 0.45 : 1.0;
	center = y + h / 2;
	x = p->pad_x;
	right = p->width - p->pad_x;

	pass_row_background(p, p->pad_x / 2, y, p->width - p->pad_x, h,
	                    hovered, gowl_bar_panel_item_get_selected(item),
	                    gowl_bar_panel_item_get_active(item));

	icon = gowl_bar_panel_item_get_icon(item);
	if (icon != NULL && icon[0] != '\0') {
		pass_set_font(p, 1.05, TRUE, FALSE);
		pass_measure_text(p, icon, &icon_w, NULL);
		pass_draw_text(p, icon, x, center,
			gowl_bar_panel_item_has_color(item)
				? gowl_bar_panel_item_get_color(item)
				: GOWL_BAR_COLOR_SUBTEXT,
			alpha, 0, PANGO_ALIGN_LEFT);
		x += icon_w + p->gap + 2;
	}

	badge = gowl_bar_panel_item_get_badge(item);
	if (badge != NULL && badge[0] != '\0') {
		gint bw;

		pass_set_font(p, BADGE_SCALE, TRUE, FALSE);
		pass_measure_text(p, badge, &bw, NULL);
		pass_draw_text(p, badge, right - bw, center,
		               GOWL_BAR_COLOR_MUTED, alpha, 0,
		               PANGO_ALIGN_LEFT);
		right -= bw + p->gap;
	}

	value = gowl_bar_panel_item_get_value(item);
	if (value != NULL && value[0] != '\0') {
		gint vw;

		pass_set_font(p, SMALL_SCALE, FALSE, FALSE);
		pass_measure_text(p, value, &vw, NULL);
		if (vw > p->inner_w / 2)
			vw = p->inner_w / 2;
		pass_draw_text_right(p, value, right, center,
		                     gowl_bar_panel_item_get_value_color(item),
		                     alpha, vw);
		right -= vw + p->gap;
	}

	subtitle = gowl_bar_panel_item_get_subtitle(item);
	if (subtitle != NULL && subtitle[0] != '\0') {
		gint title_h, sub_h, top;

		/* Stack the two lines against their measured heights.
		   Offsetting by a fraction of the row height instead makes
		   them collide at any font the fraction was not tuned for,
		   which is every font but one. */
		pass_set_font(p, 1.0, FALSE, FALSE);
		pass_measure_text(p, gowl_bar_panel_item_get_title(item),
		                  NULL, &title_h);
		pass_set_font(p, SMALL_SCALE, FALSE, FALSE);
		pass_measure_text(p, subtitle, NULL, &sub_h);

		top = center - (title_h + sub_h) / 2;

		pass_set_font(p, 1.0, FALSE, FALSE);
		pass_draw_text(p, gowl_bar_panel_item_get_title(item), x,
		               top + title_h / 2, GOWL_BAR_COLOR_TEXT, alpha,
		               right - x, PANGO_ALIGN_LEFT);
		pass_set_font(p, SMALL_SCALE, FALSE, FALSE);
		pass_draw_text(p, subtitle, x, top + title_h + sub_h / 2,
		               GOWL_BAR_COLOR_MUTED, alpha, right - x,
		               PANGO_ALIGN_LEFT);
	} else {
		pass_set_font(p, 1.0, FALSE, FALSE);
		pass_draw_text(p, gowl_bar_panel_item_get_title(item), x,
		               center, GOWL_BAR_COLOR_TEXT, alpha,
		               right - x, PANGO_ALIGN_LEFT);
	}

	if (!gowl_bar_panel_item_get_disabled(item)) {
		pass_emit_hit(p, index, -1, GOWL_BAR_ITEM_ROW,
		              gowl_bar_panel_item_get_id(item),
		              0, y, p->width, h);
	}
}

static void
pass_draw_toggle(PanelPass *p, GowlBarPanelItem *item, gint index,
                 gint y, gint h, gboolean hovered)
{
	gint center, sw;

	center = y + h / 2;

	pass_row_background(p, p->pad_x / 2, y, p->width - p->pad_x, h,
	                    hovered, gowl_bar_panel_item_get_selected(item),
	                    FALSE);

	sw = pass_draw_switch(p, p->width - p->pad_x, center,
	                      gowl_bar_panel_item_get_active(item),
	                      gowl_bar_panel_item_get_busy(item));

	pass_set_font(p, 1.0, FALSE, FALSE);
	pass_draw_text(p, gowl_bar_panel_item_get_title(item), p->pad_x,
	               center, GOWL_BAR_COLOR_TEXT,
	               gowl_bar_panel_item_get_disabled(item) ? 0.45 : 1.0,
	               p->inner_w - sw - p->gap, PANGO_ALIGN_LEFT);

	if (!gowl_bar_panel_item_get_disabled(item)) {
		pass_emit_hit(p, index, -1, GOWL_BAR_ITEM_TOGGLE,
		              gowl_bar_panel_item_get_id(item),
		              0, y, p->width, h);
	}
}

static void
pass_draw_slider(PanelPass *p, GowlBarPanelItem *item, gint index,
                 gint y, gint h)
{
	gint label_y, track_y;
	gchar pct[16];

	label_y = y + (gint)((gdouble)h * 0.27);
	track_y = y + (gint)((gdouble)h * 0.72);

	pass_set_font(p, SECTION_SCALE, FALSE, FALSE);
	{
		g_autofree gchar *upper = NULL;
		const gchar *title;

		title = gowl_bar_panel_item_get_title(item);
		if (title != NULL) {
			upper = g_utf8_strup(title, -1);
			pass_draw_tracked(p, upper, p->pad_x, label_y,
			                  GOWL_BAR_COLOR_MUTED, 1.0,
			                  SECTION_TRACKING, p->inner_w / 2);
		}
	}

	if (gowl_bar_panel_item_get_value(item) != NULL) {
		pass_draw_text_right(p, gowl_bar_panel_item_get_value(item),
		                     p->width - p->pad_x, label_y,
		                     GOWL_BAR_COLOR_SUBTEXT, 1.0,
		                     p->inner_w / 2);
	} else {
		g_snprintf(pct, sizeof(pct), "%d%%",
		           (gint)lround(gowl_bar_panel_item_get_fraction(item)
		                        * 100.0));
		pass_draw_text_right(p, pct, p->width - p->pad_x, label_y,
		                     GOWL_BAR_COLOR_SUBTEXT, 1.0,
		                     p->inner_w / 2);
	}

	pass_draw_track(p, p->pad_x, track_y, p->inner_w,
	                gowl_bar_panel_item_get_fraction(item),
	                gowl_bar_panel_item_has_color(item)
	                	? gowl_bar_panel_item_get_color(item)
	                	: GOWL_BAR_COLOR_ACCENT,
	                TRUE);

	/* The hit region is the track's row, widened vertically so a
	   grab does not demand pixel accuracy on a 6px track. */
	pass_emit_hit(p, index, -1, GOWL_BAR_ITEM_SLIDER,
	              gowl_bar_panel_item_get_id(item),
	              p->pad_x, track_y - p->row_h / 3, p->inner_w,
	              (p->row_h * 2) / 3);
}

static void
pass_draw_progress(PanelPass *p, GowlBarPanelItem *item, gint y, gint h)
{
	gint center;

	center = y + h / 2;

	if (gowl_bar_panel_item_get_title(item) != NULL) {
		pass_set_font(p, SMALL_SCALE, FALSE, FALSE);
		pass_draw_text(p, gowl_bar_panel_item_get_title(item),
		               p->pad_x, y + h / 4, GOWL_BAR_COLOR_MUTED,
		               1.0, p->inner_w / 2, PANGO_ALIGN_LEFT);
		if (gowl_bar_panel_item_get_value(item) != NULL)
			pass_draw_text_right(p,
				gowl_bar_panel_item_get_value(item),
				p->width - p->pad_x, y + h / 4,
				gowl_bar_panel_item_get_value_color(item),
				1.0, p->inner_w / 2);
		center = y + (gint)((gdouble)h * 0.78);
	}

	pass_draw_track(p, p->pad_x, center, p->inner_w,
	                gowl_bar_panel_item_get_fraction(item),
	                gowl_bar_panel_item_has_color(item)
	                	? gowl_bar_panel_item_get_color(item)
	                	: GOWL_BAR_COLOR_ACCENT,
	                FALSE);
}

static void
pass_draw_buttons(PanelPass *p, GowlBarPanelItem *item, gint index,
                  gint y, gint h, gint hover_child)
{
	guint n, i;
	gdouble bw, bx;
	gint bh;

	n = gowl_bar_panel_item_n_children(item);
	if (n == 0)
		return;

	bh = h - 4;
	bw = ((gdouble)p->inner_w - (gdouble)(n - 1) * (gdouble)p->gap)
	     / (gdouble)n;
	if (bw < 8.0)
		bw = 8.0;

	for (i = 0; i < n; i++) {
		GowlBarPanelItem *child;
		gboolean active, hovered;

		child = gowl_bar_panel_item_get_child(item, i);
		if (child == NULL)
			continue;

		active  = gowl_bar_panel_item_get_active(child);
		hovered = ((gint)i == hover_child);
		bx = (gdouble)p->pad_x
		     + (gdouble)i * (bw + (gdouble)p->gap);

		if (p->cr != NULL) {
			gowl_bar_cairo_rounded_rect(p->cr, bx, y + 2, bw, bh,
			                            p->radius * 0.75);
			if (active)
				gowl_bar_theme_cairo_set_alpha(p->theme, p->cr,
					GOWL_BAR_COLOR_SURFACE_ALT, 1.0);
			else if (hovered)
				gowl_bar_theme_cairo_set_alpha(p->theme, p->cr,
					GOWL_BAR_COLOR_SURFACE, 0.7);
			else
				gowl_bar_theme_cairo_set_alpha(p->theme, p->cr,
					GOWL_BAR_COLOR_SURFACE, 0.25);
			cairo_fill_preserve(p->cr);

			gowl_bar_theme_cairo_set_alpha(p->theme, p->cr,
				active ? GOWL_BAR_COLOR_ACCENT
				       : GOWL_BAR_COLOR_OVERLAY,
				active ? 0.9 : 0.45);
			cairo_set_line_width(p->cr, 1.0);
			cairo_stroke(p->cr);

			pass_set_font(p, SMALL_SCALE, FALSE, FALSE);
			{
				gint tw;

				pass_measure_text(p,
					gowl_bar_panel_item_get_title(child),
					&tw, NULL);
				if ((gdouble)tw > bw - 8.0)
					tw = (gint)(bw - 8.0);
				pass_draw_text(p,
					gowl_bar_panel_item_get_title(child),
					(gint)(bx + (bw - (gdouble)tw) / 2.0),
					y + h / 2,
					active ? GOWL_BAR_COLOR_TEXT
					       : GOWL_BAR_COLOR_SUBTEXT,
					gowl_bar_panel_item_get_disabled(child)
						? 0.4 : 1.0,
					tw, PANGO_ALIGN_CENTER);
			}
		}

		if (!gowl_bar_panel_item_get_disabled(child)) {
			pass_emit_hit(p, index, (gint)i, GOWL_BAR_ITEM_BUTTONS,
			              gowl_bar_panel_item_get_id(item),
			              (gint)bx, y + 2, (gint)bw, bh);
		}
	}
}

static void
pass_draw_graph(PanelPass *p, GowlBarPanelItem *item, gint y, gint h)
{
	const gdouble *samples;
	guint n, i;
	gint top, bottom, plot_h;
	gdouble step;
	GowlBarColor role;

	samples = gowl_bar_panel_item_get_samples(item, &n);

	if (gowl_bar_panel_item_get_title(item) != NULL) {
		pass_set_font(p, SECTION_SCALE, FALSE, FALSE);
		{
			g_autofree gchar *upper = NULL;

			upper = g_utf8_strup(
				gowl_bar_panel_item_get_title(item), -1);
			pass_draw_tracked(p, upper, p->pad_x,
			                  y + (gint)((gdouble)h * 0.16),
			                  GOWL_BAR_COLOR_MUTED, 1.0,
			                  SECTION_TRACKING, p->inner_w / 2);
		}
		if (gowl_bar_panel_item_get_value(item) != NULL)
			pass_draw_text_right(p,
				gowl_bar_panel_item_get_value(item),
				p->width - p->pad_x,
				y + (gint)((gdouble)h * 0.16),
				gowl_bar_panel_item_get_value_color(item),
				1.0, p->inner_w / 2);
	}

	if (p->cr == NULL || samples == NULL || n < 2)
		return;

	top    = y + (gint)((gdouble)h * 0.34);
	bottom = y + h - 2;
	plot_h = bottom - top;
	if (plot_h < 4)
		return;

	role = gowl_bar_panel_item_has_color(item)
		? gowl_bar_panel_item_get_color(item)
		: GOWL_BAR_COLOR_ACCENT;
	step = (gdouble)p->inner_w / (gdouble)(n - 1);

	/* Filled area first, then the stroke on top: the fill alone is
	   too faint to read a trend from, the stroke alone floats. */
	cairo_move_to(p->cr, p->pad_x, bottom);
	for (i = 0; i < n; i++) {
		gdouble v = samples[i];

		if (v < 0.0)
			v = 0.0;
		if (v > 1.0)
			v = 1.0;
		cairo_line_to(p->cr, (gdouble)p->pad_x + (gdouble)i * step,
		              (gdouble)bottom - v * (gdouble)plot_h);
	}
	cairo_line_to(p->cr, (gdouble)(p->pad_x + p->inner_w), bottom);
	cairo_close_path(p->cr);
	gowl_bar_theme_cairo_set_alpha(p->theme, p->cr, role, 0.18);
	cairo_fill(p->cr);

	for (i = 0; i < n; i++) {
		gdouble v = samples[i];

		if (v < 0.0)
			v = 0.0;
		if (v > 1.0)
			v = 1.0;
		if (i == 0)
			cairo_move_to(p->cr, p->pad_x,
			              (gdouble)bottom - v * (gdouble)plot_h);
		else
			cairo_line_to(p->cr,
				(gdouble)p->pad_x + (gdouble)i * step,
				(gdouble)bottom - v * (gdouble)plot_h);
	}
	gowl_bar_theme_cairo_set(p->theme, p->cr, role);
	cairo_set_line_width(p->cr, 1.5);
	cairo_set_line_join(p->cr, CAIRO_LINE_JOIN_ROUND);
	cairo_stroke(p->cr);
}

/* The calendar's children are its cells, in row-major order starting
   with the seven weekday headers.  A cell carries its day number as
   the title, its week number as the value (first cell of a row), and
   uses active / selected / disabled for today, the chosen day and
   days outside the month. */
static void
pass_draw_calendar(PanelPass *p, GowlBarPanelItem *item, gint index,
                   gint y, gint h, gint hover_child)
{
	guint n, i;
	gint cell_h, cell_w, cols, gutter;

	n = gowl_bar_panel_item_n_children(item);
	if (n == 0)
		return;

	/* Eight columns: a week-number gutter plus seven days. */
	cols   = 8;
	cell_h = h / 7;
	gutter = p->inner_w / 10;
	cell_w = (p->inner_w - gutter) / (cols - 1);

	for (i = 0; i < n; i++) {
		GowlBarPanelItem *cell;
		gint row, col, cx, cy, center_x, center_y;
		gboolean hovered;
		GowlBarColor role;
		gdouble alpha;

		cell = gowl_bar_panel_item_get_child(item, i);
		if (cell == NULL)
			continue;

		row = (gint)(i / (guint)cols);
		col = (gint)(i % (guint)cols);
		cx  = p->pad_x + ((col == 0) ? 0 : gutter + (col - 1) * cell_w);
		cy  = y + row * cell_h;
		center_x = cx + ((col == 0) ? gutter : cell_w) / 2;
		center_y = cy + cell_h / 2;
		hovered  = ((gint)i == hover_child);

		if (p->cr != NULL &&
		    (gowl_bar_panel_item_get_active(cell) ||
		     gowl_bar_panel_item_get_selected(cell) || hovered)) {
			gint bw = (col == 0) ? gutter : cell_w;

			gowl_bar_cairo_rounded_rect(p->cr, cx + 1, cy + 1,
			                            bw - 2, cell_h - 2,
			                            p->radius * 0.6);
			if (gowl_bar_panel_item_get_active(cell)) {
				/* Today: outlined, not filled --- a filled
				   cell reads as "selected" and the two need
				   to be distinguishable at a glance. */
				gowl_bar_theme_cairo_set(p->theme, p->cr,
					GOWL_BAR_COLOR_ACCENT);
				cairo_set_line_width(p->cr, 1.0);
				cairo_stroke(p->cr);
			} else {
				gowl_bar_theme_cairo_set_alpha(p->theme, p->cr,
					GOWL_BAR_COLOR_SURFACE,
					gowl_bar_panel_item_get_selected(cell)
						? 1.0 : 0.5);
				cairo_fill(p->cr);
			}
		}

		if (gowl_bar_panel_item_has_color(cell)) {
			role = gowl_bar_panel_item_get_color(cell);
			alpha = 1.0;
		} else if (row == 0 || col == 0) {
			role = GOWL_BAR_COLOR_MUTED;
			alpha = 1.0;
		} else {
			role = GOWL_BAR_COLOR_TEXT;
			alpha = gowl_bar_panel_item_get_disabled(cell)
				? 0.32 : 1.0;
		}

		pass_set_font(p, (row == 0 || col == 0) ? BADGE_SCALE : 1.0,
		              FALSE, gowl_bar_panel_item_get_active(cell));
		if (p->cr != NULL) {
			gint tw;

			pass_measure_text(p,
				gowl_bar_panel_item_get_title(cell), &tw, NULL);
			pass_draw_text(p,
				gowl_bar_panel_item_get_title(cell),
				center_x - tw / 2, center_y, role, alpha, 0,
				PANGO_ALIGN_LEFT);
		}

		if (row > 0 && col > 0 &&
		    !gowl_bar_panel_item_get_disabled(cell)) {
			pass_emit_hit(p, index, (gint)i,
			              GOWL_BAR_ITEM_CALENDAR,
			              gowl_bar_panel_item_get_id(item),
			              cx, cy, cell_w, cell_h);
		}
	}
}

/* ----------------------------------------------------------------
 * The walk
 * ---------------------------------------------------------------- */

static gint
pass_run(PanelPass *p)
{
	guint n, i;
	gint y;

	n = gowl_bar_panel_n_items(p->panel);
	y = p->pad_y - p->ctx->scroll;

	for (i = 0; i < n; i++) {
		GowlBarPanelItem *item;
		GowlBarItemKind kind;
		gint h, gap;
		gboolean hovered;
		gint hover_child;

		item = gowl_bar_panel_get_item(p->panel, i);
		if (item == NULL)
			continue;

		kind = gowl_bar_panel_item_get_kind(item);
		h    = pass_item_height(p, item);
		gap  = pass_item_gap(p, item);

		hovered = (p->ctx->hover_item == (gint)i);
		hover_child = hovered ? p->ctx->hover_child : -1;

		/* A keyboard cursor and a pointer hover are the same visual
		   state as far as the item kinds are concerned, so fold the
		   focus index into the item's `selected' flag for the draw
		   and put it back afterwards --- the panel model belongs to
		   the plugin and must come out of a render unchanged. */
		if (p->cr != NULL && p->ctx->focus_item == (gint)i &&
		    p->ctx->focus_child < 0) {
			gboolean saved;

			saved = gowl_bar_panel_item_get_selected(item);
			gowl_bar_panel_item_set_selected(item, TRUE);
			switch (kind) {
			case GOWL_BAR_ITEM_ROW:
				pass_draw_row(p, item, (gint)i, y, h, hovered);
				break;
			case GOWL_BAR_ITEM_TOGGLE:
				pass_draw_toggle(p, item, (gint)i, y, h,
				                 hovered);
				break;
			default:
				gowl_bar_panel_item_set_selected(item, saved);
				goto normal;
			}
			gowl_bar_panel_item_set_selected(item, saved);
			y += h + gap;
			continue;
		}

normal:
		switch (kind) {
		case GOWL_BAR_ITEM_HERO:
			pass_draw_hero(p, item, (gint)i, y, h);
			break;
		case GOWL_BAR_ITEM_SECTION:
			pass_draw_section(p, item, (gint)i, y, h);
			break;
		case GOWL_BAR_ITEM_LABEL:
			pass_set_font(p, 1.0, FALSE, FALSE);
			pass_draw_wrapped(p,
				gowl_bar_panel_item_get_title(item),
				p->pad_x, y + p->gap / 4, p->inner_w,
				gowl_bar_panel_item_has_color(item)
					? gowl_bar_panel_item_get_color(item)
					: GOWL_BAR_COLOR_SUBTEXT,
				1.0);
			break;
		case GOWL_BAR_ITEM_FIELD:
			pass_draw_field(p, item, y, h, FALSE);
			break;
		case GOWL_BAR_ITEM_FIELD_PAIR:
			pass_draw_field(p, item, y, h, TRUE);
			break;
		case GOWL_BAR_ITEM_SEPARATOR:
			if (p->cr != NULL) {
				gowl_bar_theme_cairo_set_alpha(p->theme, p->cr,
					GOWL_BAR_COLOR_OVERLAY, 0.35);
				cairo_rectangle(p->cr, p->pad_x,
				                y + h / 2, p->inner_w, 1);
				cairo_fill(p->cr);
			}
			break;
		case GOWL_BAR_ITEM_SLIDER:
			pass_draw_slider(p, item, (gint)i, y, h);
			break;
		case GOWL_BAR_ITEM_TOGGLE:
			pass_draw_toggle(p, item, (gint)i, y, h, hovered);
			break;
		case GOWL_BAR_ITEM_BUTTONS:
			pass_draw_buttons(p, item, (gint)i, y, h, hover_child);
			break;
		case GOWL_BAR_ITEM_ROW:
			pass_draw_row(p, item, (gint)i, y, h, hovered);
			break;
		case GOWL_BAR_ITEM_PROGRESS:
			pass_draw_progress(p, item, y, h);
			break;
		case GOWL_BAR_ITEM_GRAPH:
			pass_draw_graph(p, item, y, h);
			break;
		case GOWL_BAR_ITEM_CALENDAR:
			pass_draw_calendar(p, item, (gint)i, y, h, hover_child);
			break;
		case GOWL_BAR_ITEM_SPACER:
		default:
			break;
		}

		y += h + gap;
	}

	return y + p->pad_y + p->ctx->scroll;
}

/* Fill in a pass from the public arguments.  Returns FALSE when the
   theme could not produce a usable font, which would otherwise crash
   pango deep inside the walk. */
static gboolean
pass_init(PanelPass *p, GowlBarPanel *panel, cairo_t *cr,
          PangoLayout *layout, const GowlBarTheme *theme,
          GowlBarPanelRenderCtx *ctx)
{
	memset(p, 0, sizeof(*p));

	p->panel  = panel;
	p->cr     = cr;
	p->layout = layout;
	p->theme  = theme;
	p->ctx    = ctx;

	p->body_font = pango_font_description_from_string(
		gowl_bar_theme_get_font(theme));
	p->icon_font = pango_font_description_from_string(
		gowl_bar_theme_get_icon_font(theme));
	if (p->body_font == NULL || p->icon_font == NULL) {
		g_clear_pointer(&p->body_font, pango_font_description_free);
		g_clear_pointer(&p->icon_font, pango_font_description_free);
		return FALSE;
	}

	p->base_size = pango_font_description_get_size(p->body_font);
	if (p->base_size <= 0)
		p->base_size = 11 * PANGO_SCALE;

	p->pad_x  = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_PAD_X);
	p->pad_y  = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_PAD_Y);
	p->gap    = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_GAP);
	p->row_h  = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_ROW_HEIGHT);
	p->radius = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_RADIUS);
	p->width  = ctx->width;
	p->inner_w = ctx->width - 2 * p->pad_x;
	if (p->inner_w < 16)
		p->inner_w = 16;

	return TRUE;
}

static void
pass_clear(PanelPass *p)
{
	g_clear_pointer(&p->body_font, pango_font_description_free);
	g_clear_pointer(&p->icon_font, pango_font_description_free);
	if (p->layout != NULL) {
		pango_layout_set_attributes(p->layout, NULL);
		pango_layout_set_width(p->layout, -1);
		pango_layout_set_ellipsize(p->layout, PANGO_ELLIPSIZE_NONE);
		pango_layout_set_alignment(p->layout, PANGO_ALIGN_LEFT);
	}
}

/**
 * gowl_bar_panel_render_ctx_init:
 * @ctx: (out caller-allocates): the context to prepare
 * @width: the content width
 */
void
gowl_bar_panel_render_ctx_init(GowlBarPanelRenderCtx *ctx, gint width)
{
	g_return_if_fail(ctx != NULL);

	memset(ctx, 0, sizeof(*ctx));
	ctx->width       = width;
	ctx->hover_item  = -1;
	ctx->hover_child = -1;
	ctx->focus_item  = -1;
	ctx->focus_child = -1;
}

/**
 * gowl_bar_panel_measure:
 * @panel: the panel to measure
 * @layout: a #PangoLayout to measure text with
 * @theme: the active theme
 * @width: the content width in pixels
 *
 * Returns: the total content height in pixels
 */
gint
gowl_bar_panel_measure(GowlBarPanel *panel, PangoLayout *layout,
                       const GowlBarTheme *theme, gint width)
{
	GowlBarPanelRenderCtx ctx;
	PanelPass pass;
	gint height;

	g_return_val_if_fail(GOWL_IS_BAR_PANEL(panel), 0);
	g_return_val_if_fail(layout != NULL, 0);

	gowl_bar_panel_render_ctx_init(&ctx, width);
	if (!pass_init(&pass, panel, NULL, layout, theme, &ctx))
		return 0;

	height = pass_run(&pass);
	pass_clear(&pass);
	return height;
}

/**
 * gowl_bar_panel_render:
 * @panel: the panel to draw
 * @cr: the target context
 * @layout: a #PangoLayout to draw text with
 * @theme: the active theme
 * @ctx: the render context
 */
void
gowl_bar_panel_render(GowlBarPanel *panel, cairo_t *cr, PangoLayout *layout,
                      const GowlBarTheme *theme, GowlBarPanelRenderCtx *ctx)
{
	PanelPass pass;

	g_return_if_fail(GOWL_IS_BAR_PANEL(panel));
	g_return_if_fail(cr != NULL);
	g_return_if_fail(layout != NULL);
	g_return_if_fail(ctx != NULL);

	if (ctx->hits != NULL)
		g_array_set_size(ctx->hits, 0);

	if (!pass_init(&pass, panel, cr, layout, theme, ctx))
		return;

	ctx->content_height = pass_run(&pass);
	pass_clear(&pass);
}

/**
 * gowl_bar_panel_draw_frame:
 * @cr: the target context
 * @theme: the active theme
 * @x: the frame's left edge
 * @y: the frame's top edge
 * @width: the frame's width
 * @height: the frame's height
 * @accent: (nullable) (array fixed-size=4): a border colour override
 */
void
gowl_bar_panel_draw_frame(cairo_t *cr, const GowlBarTheme *theme,
                          gint x, gint y, gint width, gint height,
                          const gdouble *accent)
{
	gint radius, border, shadow, i;

	g_return_if_fail(cr != NULL);

	if (width <= 0 || height <= 0)
		return;

	radius = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_RADIUS);
	border = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_BORDER);
	shadow = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_SHADOW);

	/* A stack of progressively larger, progressively fainter rounded
	   rects.  A real gaussian blur would mean a second surface and a
	   filter pass per frame; at this radius the difference is not
	   visible and this costs a handful of fills. */
	if (shadow > 0) {
		for (i = shadow; i > 0; i -= 2) {
			gdouble a;

			a = 0.30 * (1.0 - (gdouble)i / (gdouble)(shadow + 2));
			gowl_bar_cairo_rounded_rect(cr,
				(gdouble)(x - i), (gdouble)(y - i + 2),
				(gdouble)(width + 2 * i),
				(gdouble)(height + 2 * i),
				(gdouble)(radius + i));
			cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, a * 0.5);
			cairo_fill(cr);
		}
	}

	gowl_bar_cairo_rounded_rect(cr, x, y, width, height, radius);
	gowl_bar_theme_cairo_set(theme, cr, GOWL_BAR_COLOR_BASE);
	cairo_fill_preserve(cr);

	if (border > 0) {
		if (accent != NULL) {
			cairo_set_source_rgba(cr, accent[0], accent[1],
			                      accent[2], accent[3]);
		} else {
			gowl_bar_theme_cairo_set_alpha(theme, cr,
				GOWL_BAR_COLOR_ACCENT, 0.85);
		}
		cairo_set_line_width(cr, (gdouble)border);
		cairo_stroke(cr);
	} else {
		cairo_new_path(cr);
	}
}

/**
 * gowl_bar_hit_find:
 * @hits: (element-type GowlBarHitRect): regions from a render pass
 * @x: panel-local x
 * @y: panel-local y
 *
 * Returns: the index of the topmost region containing the point, or -1
 */
gint
gowl_bar_hit_find(GArray *hits, gint x, gint y)
{
	gint i;

	if (hits == NULL)
		return -1;

	for (i = (gint)hits->len - 1; i >= 0; i--) {
		GowlBarHitRect *r;

		r = &g_array_index(hits, GowlBarHitRect, i);
		if (x >= r->x && x < r->x + r->width &&
		    y >= r->y && y < r->y + r->height)
			return i;
	}
	return -1;
}
