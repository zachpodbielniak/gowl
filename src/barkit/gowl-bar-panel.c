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

#include "barkit/gowl-bar-panel.h"

#include <string.h>

/**
 * SECTION:gowl-bar-panel
 * @title: GowlBarPanel
 * @short_description: the declarative model a bar dropdown is built from
 *
 * A plugin describes its dropdown rather than drawing it.  The host owns
 * rendering, hit-testing, scrolling and keyboard navigation, so every
 * panel in the bar looks and behaves the same no matter who wrote it,
 * and improvements to the panel layer reach third-party plugins with no
 * change on their side.
 */

struct _GowlBarPanelItem {
	GowlBarItemKind kind;

	gchar   *id;
	gchar   *icon;
	gchar   *title;
	gchar   *subtitle;
	gchar   *value;
	gchar   *badge;
	gchar   *title2;
	gchar   *value2;

	GowlBarColor color;
	gboolean     has_color;
	GowlBarColor value_color;
	gboolean     has_value_color;

	gboolean active;
	gboolean selected;
	gboolean disabled;
	gboolean busy;

	gdouble fraction;
	gdouble step;
	gint    height;

	GPtrArray  *children;  /* GowlBarPanelItem*, owned */
	GArray     *samples;   /* gdouble */
	GHashTable *props;     /* gchar* -> gchar*, created on demand */
};

G_DEFINE_BOXED_TYPE(GowlBarPanelItem, gowl_bar_panel_item,
                    gowl_bar_panel_item_copy, gowl_bar_panel_item_free)

/**
 * gowl_bar_panel_item_new:
 * @kind: the item kind
 *
 * Returns: (transfer full): a new item with neutral defaults
 */
GowlBarPanelItem *
gowl_bar_panel_item_new(GowlBarItemKind kind)
{
	GowlBarPanelItem *self;

	self = g_new0(GowlBarPanelItem, 1);
	self->kind        = kind;
	self->color       = GOWL_BAR_COLOR_TEXT;
	self->value_color = GOWL_BAR_COLOR_SUBTEXT;
	self->fraction    = 0.0;
	self->step        = 0.0;
	self->height      = 0;
	return self;
}

/**
 * gowl_bar_panel_item_copy:
 * @self: (nullable): an item
 *
 * Returns: (transfer full) (nullable): a deep copy of @self
 */
GowlBarPanelItem *
gowl_bar_panel_item_copy(const GowlBarPanelItem *self)
{
	GowlBarPanelItem *copy;
	guint i;

	if (self == NULL)
		return NULL;

	copy = gowl_bar_panel_item_new(self->kind);
	copy->id       = g_strdup(self->id);
	copy->icon     = g_strdup(self->icon);
	copy->title    = g_strdup(self->title);
	copy->subtitle = g_strdup(self->subtitle);
	copy->value    = g_strdup(self->value);
	copy->badge    = g_strdup(self->badge);
	copy->title2   = g_strdup(self->title2);
	copy->value2   = g_strdup(self->value2);

	copy->color           = self->color;
	copy->has_color       = self->has_color;
	copy->value_color     = self->value_color;
	copy->has_value_color = self->has_value_color;
	copy->active          = self->active;
	copy->selected        = self->selected;
	copy->disabled        = self->disabled;
	copy->busy            = self->busy;
	copy->fraction        = self->fraction;
	copy->step            = self->step;
	copy->height          = self->height;

	if (self->children != NULL) {
		for (i = 0; i < self->children->len; i++) {
			gowl_bar_panel_item_add_child(copy,
				gowl_bar_panel_item_copy(
					g_ptr_array_index(self->children, i)));
		}
	}
	if (self->samples != NULL) {
		gowl_bar_panel_item_set_samples(copy,
			&g_array_index(self->samples, gdouble, 0),
			self->samples->len);
	}
	if (self->props != NULL) {
		GHashTableIter iter;
		gpointer k, v;

		g_hash_table_iter_init(&iter, self->props);
		while (g_hash_table_iter_next(&iter, &k, &v))
			gowl_bar_panel_item_set_prop(copy, (const gchar *)k,
			                             (const gchar *)v);
	}
	return copy;
}

/**
 * gowl_bar_panel_item_free:
 * @self: (nullable) (transfer full): an item
 *
 * Frees @self and every child it owns.
 */
void
gowl_bar_panel_item_free(GowlBarPanelItem *self)
{
	if (self == NULL)
		return;

	g_free(self->id);
	g_free(self->icon);
	g_free(self->title);
	g_free(self->subtitle);
	g_free(self->value);
	g_free(self->badge);
	g_free(self->title2);
	g_free(self->value2);
	if (self->children != NULL)
		g_ptr_array_unref(self->children);
	if (self->samples != NULL)
		g_array_unref(self->samples);
	if (self->props != NULL)
		g_hash_table_unref(self->props);
	g_free(self);
}

/**
 * gowl_bar_panel_item_get_kind:
 * @self: an item
 *
 * Returns: the item's kind
 */
GowlBarItemKind
gowl_bar_panel_item_get_kind(const GowlBarPanelItem *self)
{
	return (self != NULL) ? self->kind : GOWL_BAR_ITEM_LABEL;
}

/* Generate the repetitive string accessors.  Written as a macro rather
   than by hand because there are eight of them and a typo in one is
   the kind of bug that only shows up in whichever panel happens to use
   that field. */
#define GOWL_BAR_ITEM_STRING_ACCESSOR(field)                                \
	void                                                                \
	gowl_bar_panel_item_set_##field(GowlBarPanelItem *self,             \
	                                const gchar *value)                 \
	{                                                                   \
		g_return_if_fail(self != NULL);                             \
		g_free(self->field);                                        \
		self->field = g_strdup(value);                              \
	}                                                                   \
	const gchar *                                                       \
	gowl_bar_panel_item_get_##field(const GowlBarPanelItem *self)       \
	{                                                                   \
		return (self != NULL) ? self->field : NULL;                 \
	}

/**
 * gowl_bar_panel_item_set_id:
 * @self: an item
 * @value: (nullable): the activation identifier
 */
GOWL_BAR_ITEM_STRING_ACCESSOR(id)

/**
 * gowl_bar_panel_item_set_icon:
 * @self: an item
 * @value: (nullable): a glyph
 */
GOWL_BAR_ITEM_STRING_ACCESSOR(icon)

/**
 * gowl_bar_panel_item_set_title:
 * @self: an item
 * @value: (nullable): the primary text
 */
GOWL_BAR_ITEM_STRING_ACCESSOR(title)

/**
 * gowl_bar_panel_item_set_subtitle:
 * @self: an item
 * @value: (nullable): the secondary text
 */
GOWL_BAR_ITEM_STRING_ACCESSOR(subtitle)

/**
 * gowl_bar_panel_item_set_value:
 * @self: an item
 * @value: (nullable): the trailing text
 */
GOWL_BAR_ITEM_STRING_ACCESSOR(value)

/**
 * gowl_bar_panel_item_set_badge:
 * @self: an item
 * @value: (nullable): a trailing glyph
 */
GOWL_BAR_ITEM_STRING_ACCESSOR(badge)

/**
 * gowl_bar_panel_item_set_title2:
 * @self: an item
 * @value: (nullable): the second column's label
 */
GOWL_BAR_ITEM_STRING_ACCESSOR(title2)

/**
 * gowl_bar_panel_item_set_value2:
 * @self: an item
 * @value: (nullable): the second column's value
 */
GOWL_BAR_ITEM_STRING_ACCESSOR(value2)

#undef GOWL_BAR_ITEM_STRING_ACCESSOR

/**
 * gowl_bar_panel_item_set_color:
 * @self: an item
 * @color: a theme role
 */
void
gowl_bar_panel_item_set_color(GowlBarPanelItem *self, GowlBarColor color)
{
	g_return_if_fail(self != NULL);
	self->color     = color;
	self->has_color = TRUE;
}

/**
 * gowl_bar_panel_item_get_color:
 * @self: an item
 *
 * Returns: the item's role, or %GOWL_BAR_COLOR_TEXT
 */
GowlBarColor
gowl_bar_panel_item_get_color(const GowlBarPanelItem *self)
{
	return (self != NULL) ? self->color : GOWL_BAR_COLOR_TEXT;
}

/**
 * gowl_bar_panel_item_has_color:
 * @self: an item
 *
 * Returns: %TRUE when a role was set explicitly
 */
gboolean
gowl_bar_panel_item_has_color(const GowlBarPanelItem *self)
{
	return (self != NULL) ? self->has_color : FALSE;
}

/**
 * gowl_bar_panel_item_set_value_color:
 * @self: an item
 * @color: a theme role for the trailing value
 */
void
gowl_bar_panel_item_set_value_color(GowlBarPanelItem *self, GowlBarColor color)
{
	g_return_if_fail(self != NULL);
	self->value_color     = color;
	self->has_value_color = TRUE;
}

/**
 * gowl_bar_panel_item_get_value_color:
 * @self: an item
 *
 * Returns: the trailing value's role
 */
GowlBarColor
gowl_bar_panel_item_get_value_color(const GowlBarPanelItem *self)
{
	return (self != NULL) ? self->value_color : GOWL_BAR_COLOR_SUBTEXT;
}

/**
 * gowl_bar_panel_item_has_value_color:
 * @self: an item
 *
 * Returns: %TRUE when a trailing-value role was set explicitly
 */
gboolean
gowl_bar_panel_item_has_value_color(const GowlBarPanelItem *self)
{
	return (self != NULL) ? self->has_value_color : FALSE;
}

#define GOWL_BAR_ITEM_BOOL_ACCESSOR(field)                                  \
	void                                                                \
	gowl_bar_panel_item_set_##field(GowlBarPanelItem *self,             \
	                                gboolean value)                     \
	{                                                                   \
		g_return_if_fail(self != NULL);                             \
		self->field = value;                                        \
	}                                                                   \
	gboolean                                                            \
	gowl_bar_panel_item_get_##field(const GowlBarPanelItem *self)       \
	{                                                                   \
		return (self != NULL) ? self->field : FALSE;                \
	}

/**
 * gowl_bar_panel_item_set_active:
 * @self: an item
 * @value: whether the item reads as on
 */
GOWL_BAR_ITEM_BOOL_ACCESSOR(active)

/**
 * gowl_bar_panel_item_set_selected:
 * @self: an item
 * @value: whether the item is the keyboard cursor's row
 */
GOWL_BAR_ITEM_BOOL_ACCESSOR(selected)

/**
 * gowl_bar_panel_item_set_disabled:
 * @self: an item
 * @value: whether the item is inert and dimmed
 */
GOWL_BAR_ITEM_BOOL_ACCESSOR(disabled)

/**
 * gowl_bar_panel_item_set_busy:
 * @self: an item
 * @value: whether an action on it is still running
 */
GOWL_BAR_ITEM_BOOL_ACCESSOR(busy)

#undef GOWL_BAR_ITEM_BOOL_ACCESSOR

/**
 * gowl_bar_panel_item_set_fraction:
 * @self: an item
 * @fraction: 0.0--1.0, clamped
 */
void
gowl_bar_panel_item_set_fraction(GowlBarPanelItem *self, gdouble fraction)
{
	g_return_if_fail(self != NULL);

	if (fraction < 0.0)
		fraction = 0.0;
	if (fraction > 1.0)
		fraction = 1.0;
	self->fraction = fraction;
}

/**
 * gowl_bar_panel_item_get_fraction:
 * @self: an item
 *
 * Returns: the filled proportion
 */
gdouble
gowl_bar_panel_item_get_fraction(const GowlBarPanelItem *self)
{
	return (self != NULL) ? self->fraction : 0.0;
}

/**
 * gowl_bar_panel_item_set_step:
 * @self: an item
 * @step: the snap quantum, as a fraction
 */
void
gowl_bar_panel_item_set_step(GowlBarPanelItem *self, gdouble step)
{
	g_return_if_fail(self != NULL);
	self->step = (step > 0.0 && step <= 1.0) ? step : 0.0;
}

/**
 * gowl_bar_panel_item_get_step:
 * @self: an item
 *
 * Returns: the snap quantum, or 0 when continuous
 */
gdouble
gowl_bar_panel_item_get_step(const GowlBarPanelItem *self)
{
	return (self != NULL) ? self->step : 0.0;
}

/**
 * gowl_bar_panel_item_set_height:
 * @self: an item
 * @height: a height override in pixels, or 0 for natural
 */
void
gowl_bar_panel_item_set_height(GowlBarPanelItem *self, gint height)
{
	g_return_if_fail(self != NULL);
	self->height = (height > 0) ? height : 0;
}

/**
 * gowl_bar_panel_item_get_height:
 * @self: an item
 *
 * Returns: the height override, or 0
 */
gint
gowl_bar_panel_item_get_height(const GowlBarPanelItem *self)
{
	return (self != NULL) ? self->height : 0;
}

/**
 * gowl_bar_panel_item_add_child:
 * @self: an item
 * @child: (transfer full): the child to append
 */
void
gowl_bar_panel_item_add_child(GowlBarPanelItem *self, GowlBarPanelItem *child)
{
	g_return_if_fail(self != NULL);

	if (child == NULL)
		return;
	if (self->children == NULL) {
		self->children = g_ptr_array_new_with_free_func(
			(GDestroyNotify)gowl_bar_panel_item_free);
	}
	g_ptr_array_add(self->children, child);
}

/**
 * gowl_bar_panel_item_n_children:
 * @self: an item
 *
 * Returns: how many children @self owns
 */
guint
gowl_bar_panel_item_n_children(const GowlBarPanelItem *self)
{
	if (self == NULL || self->children == NULL)
		return 0;
	return self->children->len;
}

/**
 * gowl_bar_panel_item_get_child:
 * @self: an item
 * @index: a child index
 *
 * Returns: (transfer none) (nullable): the child, or %NULL
 */
GowlBarPanelItem *
gowl_bar_panel_item_get_child(const GowlBarPanelItem *self, guint index)
{
	if (self == NULL || self->children == NULL ||
	    index >= self->children->len)
		return NULL;
	return (GowlBarPanelItem *)g_ptr_array_index(self->children, index);
}

/**
 * gowl_bar_panel_item_set_samples:
 * @self: an item
 * @samples: (array length=n_samples) (nullable): the sample window
 * @n_samples: how many
 */
void
gowl_bar_panel_item_set_samples(GowlBarPanelItem *self,
                                const gdouble *samples, guint n_samples)
{
	g_return_if_fail(self != NULL);

	if (self->samples != NULL) {
		g_array_unref(self->samples);
		self->samples = NULL;
	}
	if (samples == NULL || n_samples == 0)
		return;

	self->samples = g_array_sized_new(FALSE, FALSE, sizeof(gdouble),
	                                  n_samples);
	g_array_append_vals(self->samples, samples, n_samples);
}

/**
 * gowl_bar_panel_item_get_samples:
 * @self: an item
 * @n_samples: (out) (optional): how many samples
 *
 * Returns: (array length=n_samples) (transfer none) (nullable): the samples
 */
const gdouble *
gowl_bar_panel_item_get_samples(const GowlBarPanelItem *self, guint *n_samples)
{
	if (n_samples != NULL)
		*n_samples = 0;
	if (self == NULL || self->samples == NULL || self->samples->len == 0)
		return NULL;
	if (n_samples != NULL)
		*n_samples = self->samples->len;
	return &g_array_index(self->samples, gdouble, 0);
}

/**
 * gowl_bar_panel_item_set_prop:
 * @self: an item
 * @key: a property name
 * @value: (nullable): its value, or %NULL to unset
 */
void
gowl_bar_panel_item_set_prop(GowlBarPanelItem *self, const gchar *key,
                             const gchar *value)
{
	g_return_if_fail(self != NULL);
	g_return_if_fail(key != NULL);

	if (value == NULL) {
		if (self->props != NULL)
			g_hash_table_remove(self->props, key);
		return;
	}
	if (self->props == NULL) {
		self->props = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                    g_free, g_free);
	}
	g_hash_table_insert(self->props, g_strdup(key), g_strdup(value));
}

/**
 * gowl_bar_panel_item_get_prop:
 * @self: an item
 * @key: a property name
 *
 * Returns: (transfer none) (nullable): the value, or %NULL
 */
const gchar *
gowl_bar_panel_item_get_prop(const GowlBarPanelItem *self, const gchar *key)
{
	if (self == NULL || self->props == NULL || key == NULL)
		return NULL;
	return (const gchar *)g_hash_table_lookup(self->props, key);
}

/* ----------------------------------------------------------------
 * GowlBarPanel
 * ---------------------------------------------------------------- */

struct _GowlBarPanel {
	GObject parent_instance;

	GPtrArray *items;      /* GowlBarPanelItem*, owned */
	gint       width;      /* 0 = the theme's default */
	gint       max_height; /* 0 = no cap beyond the monitor */
};

G_DEFINE_FINAL_TYPE(GowlBarPanel, gowl_bar_panel, G_TYPE_OBJECT)

static void
gowl_bar_panel_finalize(GObject *object)
{
	GowlBarPanel *self = GOWL_BAR_PANEL(object);

	g_clear_pointer(&self->items, g_ptr_array_unref);

	G_OBJECT_CLASS(gowl_bar_panel_parent_class)->finalize(object);
}

static void
gowl_bar_panel_class_init(GowlBarPanelClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = gowl_bar_panel_finalize;
}

static void
gowl_bar_panel_init(GowlBarPanel *self)
{
	self->items = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_bar_panel_item_free);
	self->width      = 0;
	self->max_height = 0;
}

/**
 * gowl_bar_panel_new:
 *
 * Returns: (transfer full): a new empty panel
 */
GowlBarPanel *
gowl_bar_panel_new(void)
{
	return (GowlBarPanel *)g_object_new(GOWL_TYPE_BAR_PANEL, NULL);
}

/**
 * gowl_bar_panel_set_width:
 * @self: a panel
 * @width: a width in pixels, or 0 for the theme default
 */
void
gowl_bar_panel_set_width(GowlBarPanel *self, gint width)
{
	g_return_if_fail(GOWL_IS_BAR_PANEL(self));
	self->width = (width > 0) ? width : 0;
}

/**
 * gowl_bar_panel_get_width:
 * @self: a panel
 *
 * Returns: the requested width, or 0
 */
gint
gowl_bar_panel_get_width(GowlBarPanel *self)
{
	g_return_val_if_fail(GOWL_IS_BAR_PANEL(self), 0);
	return self->width;
}

/**
 * gowl_bar_panel_set_max_height:
 * @self: a panel
 * @height: a cap in pixels, or 0 for none
 */
void
gowl_bar_panel_set_max_height(GowlBarPanel *self, gint height)
{
	g_return_if_fail(GOWL_IS_BAR_PANEL(self));
	self->max_height = (height > 0) ? height : 0;
}

/**
 * gowl_bar_panel_get_max_height:
 * @self: a panel
 *
 * Returns: the height cap, or 0
 */
gint
gowl_bar_panel_get_max_height(GowlBarPanel *self)
{
	g_return_val_if_fail(GOWL_IS_BAR_PANEL(self), 0);
	return self->max_height;
}

/**
 * gowl_bar_panel_add:
 * @self: a panel
 * @item: (transfer full): the item to append
 *
 * Returns: (transfer none): @item
 */
GowlBarPanelItem *
gowl_bar_panel_add(GowlBarPanel *self, GowlBarPanelItem *item)
{
	g_return_val_if_fail(GOWL_IS_BAR_PANEL(self), NULL);
	g_return_val_if_fail(item != NULL, NULL);

	g_ptr_array_add(self->items, item);
	return item;
}

/**
 * gowl_bar_panel_n_items:
 * @self: a panel
 *
 * Returns: how many items the panel holds
 */
guint
gowl_bar_panel_n_items(GowlBarPanel *self)
{
	g_return_val_if_fail(GOWL_IS_BAR_PANEL(self), 0);
	return self->items->len;
}

/**
 * gowl_bar_panel_get_item:
 * @self: a panel
 * @index: an item index
 *
 * Returns: (transfer none) (nullable): the item, or %NULL
 */
GowlBarPanelItem *
gowl_bar_panel_get_item(GowlBarPanel *self, guint index)
{
	g_return_val_if_fail(GOWL_IS_BAR_PANEL(self), NULL);

	if (index >= self->items->len)
		return NULL;
	return (GowlBarPanelItem *)g_ptr_array_index(self->items, index);
}

/**
 * gowl_bar_panel_find:
 * @self: a panel
 * @id: an item id
 *
 * Returns: (transfer none) (nullable): the first item with that id
 */
GowlBarPanelItem *
gowl_bar_panel_find(GowlBarPanel *self, const gchar *id)
{
	guint i;

	g_return_val_if_fail(GOWL_IS_BAR_PANEL(self), NULL);

	if (id == NULL)
		return NULL;

	for (i = 0; i < self->items->len; i++) {
		GowlBarPanelItem *item;

		item = (GowlBarPanelItem *)g_ptr_array_index(self->items, i);
		if (item->id != NULL && strcmp(item->id, id) == 0)
			return item;
	}
	return NULL;
}

/* --- Convenience builders ----------------------------------------- */

/**
 * gowl_bar_panel_add_hero:
 * @self: a panel
 * @icon: (nullable): a glyph
 * @title: (nullable): the headline
 * @subtitle: (nullable): the line under it
 *
 * Returns: (transfer none): the new item
 */
GowlBarPanelItem *
gowl_bar_panel_add_hero(GowlBarPanel *self, const gchar *icon,
                        const gchar *title, const gchar *subtitle)
{
	GowlBarPanelItem *item;

	item = gowl_bar_panel_item_new(GOWL_BAR_ITEM_HERO);
	gowl_bar_panel_item_set_icon(item, icon);
	gowl_bar_panel_item_set_title(item, title);
	gowl_bar_panel_item_set_subtitle(item, subtitle);
	return gowl_bar_panel_add(self, item);
}

/**
 * gowl_bar_panel_add_section:
 * @self: a panel
 * @title: the header text
 *
 * Returns: (transfer none): the new item
 */
GowlBarPanelItem *
gowl_bar_panel_add_section(GowlBarPanel *self, const gchar *title)
{
	GowlBarPanelItem *item;

	item = gowl_bar_panel_item_new(GOWL_BAR_ITEM_SECTION);
	gowl_bar_panel_item_set_title(item, title);
	return gowl_bar_panel_add(self, item);
}

/**
 * gowl_bar_panel_add_label:
 * @self: a panel
 * @text: the text
 *
 * Returns: (transfer none): the new item
 */
GowlBarPanelItem *
gowl_bar_panel_add_label(GowlBarPanel *self, const gchar *text)
{
	GowlBarPanelItem *item;

	item = gowl_bar_panel_item_new(GOWL_BAR_ITEM_LABEL);
	gowl_bar_panel_item_set_title(item, text);
	return gowl_bar_panel_add(self, item);
}

/**
 * gowl_bar_panel_add_field:
 * @self: a panel
 * @label: the label
 * @value: the reading
 *
 * Returns: (transfer none): the new item
 */
GowlBarPanelItem *
gowl_bar_panel_add_field(GowlBarPanel *self, const gchar *label,
                         const gchar *value)
{
	GowlBarPanelItem *item;

	item = gowl_bar_panel_item_new(GOWL_BAR_ITEM_FIELD);
	gowl_bar_panel_item_set_title(item, label);
	gowl_bar_panel_item_set_value(item, value);
	return gowl_bar_panel_add(self, item);
}

/**
 * gowl_bar_panel_add_field_pair:
 * @self: a panel
 * @label_a: the left label
 * @value_a: the left reading
 * @label_b: the right label
 * @value_b: the right reading
 *
 * Returns: (transfer none): the new item
 */
GowlBarPanelItem *
gowl_bar_panel_add_field_pair(GowlBarPanel *self,
                              const gchar *label_a, const gchar *value_a,
                              const gchar *label_b, const gchar *value_b)
{
	GowlBarPanelItem *item;

	item = gowl_bar_panel_item_new(GOWL_BAR_ITEM_FIELD_PAIR);
	gowl_bar_panel_item_set_title(item, label_a);
	gowl_bar_panel_item_set_value(item, value_a);
	gowl_bar_panel_item_set_title2(item, label_b);
	gowl_bar_panel_item_set_value2(item, value_b);
	return gowl_bar_panel_add(self, item);
}

/**
 * gowl_bar_panel_add_separator:
 * @self: a panel
 *
 * Returns: (transfer none): the new item
 */
GowlBarPanelItem *
gowl_bar_panel_add_separator(GowlBarPanel *self)
{
	return gowl_bar_panel_add(self,
		gowl_bar_panel_item_new(GOWL_BAR_ITEM_SEPARATOR));
}

/**
 * gowl_bar_panel_add_spacer:
 * @self: a panel
 * @height: the gap in pixels
 *
 * Returns: (transfer none): the new item
 */
GowlBarPanelItem *
gowl_bar_panel_add_spacer(GowlBarPanel *self, gint height)
{
	GowlBarPanelItem *item;

	item = gowl_bar_panel_item_new(GOWL_BAR_ITEM_SPACER);
	gowl_bar_panel_item_set_height(item, height);
	return gowl_bar_panel_add(self, item);
}

/**
 * gowl_bar_panel_add_slider:
 * @self: a panel
 * @id: the activation id
 * @label: the label
 * @fraction: the initial position, 0.0--1.0
 *
 * Returns: (transfer none): the new item
 */
GowlBarPanelItem *
gowl_bar_panel_add_slider(GowlBarPanel *self, const gchar *id,
                          const gchar *label, gdouble fraction)
{
	GowlBarPanelItem *item;

	item = gowl_bar_panel_item_new(GOWL_BAR_ITEM_SLIDER);
	gowl_bar_panel_item_set_id(item, id);
	gowl_bar_panel_item_set_title(item, label);
	gowl_bar_panel_item_set_fraction(item, fraction);
	return gowl_bar_panel_add(self, item);
}

/**
 * gowl_bar_panel_add_toggle:
 * @self: a panel
 * @id: the activation id
 * @label: the label
 * @active: the initial state
 *
 * Returns: (transfer none): the new item
 */
GowlBarPanelItem *
gowl_bar_panel_add_toggle(GowlBarPanel *self, const gchar *id,
                          const gchar *label, gboolean active)
{
	GowlBarPanelItem *item;

	item = gowl_bar_panel_item_new(GOWL_BAR_ITEM_TOGGLE);
	gowl_bar_panel_item_set_id(item, id);
	gowl_bar_panel_item_set_title(item, label);
	gowl_bar_panel_item_set_active(item, active);
	return gowl_bar_panel_add(self, item);
}

/**
 * gowl_bar_panel_add_progress:
 * @self: a panel
 * @label: the label
 * @fraction: the fill, 0.0--1.0
 *
 * Returns: (transfer none): the new item
 */
GowlBarPanelItem *
gowl_bar_panel_add_progress(GowlBarPanel *self, const gchar *label,
                            gdouble fraction)
{
	GowlBarPanelItem *item;

	item = gowl_bar_panel_item_new(GOWL_BAR_ITEM_PROGRESS);
	gowl_bar_panel_item_set_title(item, label);
	gowl_bar_panel_item_set_fraction(item, fraction);
	return gowl_bar_panel_add(self, item);
}

/**
 * gowl_bar_panel_add_row:
 * @self: a panel
 * @id: (nullable): the activation id
 * @icon: (nullable): a leading glyph
 * @title: the primary text
 * @subtitle: (nullable): the secondary text
 *
 * Returns: (transfer none): the new item
 */
GowlBarPanelItem *
gowl_bar_panel_add_row(GowlBarPanel *self, const gchar *id,
                       const gchar *icon, const gchar *title,
                       const gchar *subtitle)
{
	GowlBarPanelItem *item;

	item = gowl_bar_panel_item_new(GOWL_BAR_ITEM_ROW);
	gowl_bar_panel_item_set_id(item, id);
	gowl_bar_panel_item_set_icon(item, icon);
	gowl_bar_panel_item_set_title(item, title);
	gowl_bar_panel_item_set_subtitle(item, subtitle);
	return gowl_bar_panel_add(self, item);
}

/**
 * gowl_bar_panel_add_graph:
 * @self: a panel
 * @label: (nullable): a caption
 * @samples: (array length=n_samples) (nullable): values, each 0.0--1.0
 * @n_samples: how many
 *
 * Returns: (transfer none): the new item
 */
GowlBarPanelItem *
gowl_bar_panel_add_graph(GowlBarPanel *self, const gchar *label,
                         const gdouble *samples, guint n_samples)
{
	GowlBarPanelItem *item;

	item = gowl_bar_panel_item_new(GOWL_BAR_ITEM_GRAPH);
	gowl_bar_panel_item_set_title(item, label);
	gowl_bar_panel_item_set_samples(item, samples, n_samples);
	return gowl_bar_panel_add(self, item);
}

/**
 * gowl_bar_panel_add_buttons:
 * @self: a panel
 * @id: the id reported for every button in the row
 *
 * Returns: (transfer none): the new item
 */
GowlBarPanelItem *
gowl_bar_panel_add_buttons(GowlBarPanel *self, const gchar *id)
{
	GowlBarPanelItem *item;

	item = gowl_bar_panel_item_new(GOWL_BAR_ITEM_BUTTONS);
	gowl_bar_panel_item_set_id(item, id);
	return gowl_bar_panel_add(self, item);
}

/**
 * gowl_bar_panel_add_button:
 * @row: a %GOWL_BAR_ITEM_BUTTONS item
 * @label: the button's text
 * @active: whether it reads as the current choice
 *
 * Returns: (transfer none): the new button
 */
GowlBarPanelItem *
gowl_bar_panel_add_button(GowlBarPanelItem *row, const gchar *label,
                          gboolean active)
{
	GowlBarPanelItem *button;

	g_return_val_if_fail(row != NULL, NULL);

	button = gowl_bar_panel_item_new(GOWL_BAR_ITEM_LABEL);
	gowl_bar_panel_item_set_title(button, label);
	gowl_bar_panel_item_set_active(button, active);
	gowl_bar_panel_item_add_child(row, button);
	return button;
}
