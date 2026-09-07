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

#ifndef GOWL_BAR_PANEL_H
#define GOWL_BAR_PANEL_H

#include <glib-object.h>

#include "barkit/gowl-bar-theme.h"

G_BEGIN_DECLS

/**
 * GowlBarItemKind:
 * @GOWL_BAR_ITEM_HERO: the panel's headline --- a large icon, a title,
 *   a subtitle and an optional trailing toggle or action glyphs.
 * @GOWL_BAR_ITEM_SECTION: a small dimmed header introducing a group.
 * @GOWL_BAR_ITEM_LABEL: a line of free text.
 * @GOWL_BAR_ITEM_FIELD: a `label ....... value' pair on one line.
 * @GOWL_BAR_ITEM_FIELD_PAIR: two label/value pairs on one line, the
 *   two-column readout the network and system panels are built from.
 * @GOWL_BAR_ITEM_SEPARATOR: a hairline divider.
 * @GOWL_BAR_ITEM_SLIDER: a labelled draggable track with a percentage.
 * @GOWL_BAR_ITEM_TOGGLE: a labelled switch.
 * @GOWL_BAR_ITEM_BUTTONS: a row of buttons; one may be marked active.
 * @GOWL_BAR_ITEM_ROW: a selectable list row: icon, title, subtitle,
 *   trailing text and an optional badge glyph.
 * @GOWL_BAR_ITEM_PROGRESS: a bare progress bar.
 * @GOWL_BAR_ITEM_GRAPH: a sparkline over a sample window.
 * @GOWL_BAR_ITEM_CALENDAR: a month grid.
 * @GOWL_BAR_ITEM_SPACER: vertical whitespace.
 * @GOWL_BAR_ITEM_COUNT: sentinel; number of kinds.
 *
 * The vocabulary a panel is described in.  A plugin never draws its own
 * dropdown: it returns a #GowlBarPanel made of these, and the host
 * renders and hit-tests it.  That is what keeps a third-party plugin
 * visually identical to a shipped one, and what lets the whole panel
 * layer gain (say) keyboard navigation without touching any plugin.
 */
typedef enum {
	GOWL_BAR_ITEM_HERO = 0,
	GOWL_BAR_ITEM_SECTION,
	GOWL_BAR_ITEM_LABEL,
	GOWL_BAR_ITEM_FIELD,
	GOWL_BAR_ITEM_FIELD_PAIR,
	GOWL_BAR_ITEM_SEPARATOR,
	GOWL_BAR_ITEM_SLIDER,
	GOWL_BAR_ITEM_TOGGLE,
	GOWL_BAR_ITEM_BUTTONS,
	GOWL_BAR_ITEM_ROW,
	GOWL_BAR_ITEM_PROGRESS,
	GOWL_BAR_ITEM_GRAPH,
	GOWL_BAR_ITEM_CALENDAR,
	GOWL_BAR_ITEM_SPACER,
	GOWL_BAR_ITEM_COUNT
} GowlBarItemKind;

/**
 * GowlBarPanelItem:
 *
 * One row of a panel.  Fields that do not apply to a kind are ignored
 * rather than rejected, so a builder can set a colour or a badge on
 * anything.
 */
typedef struct _GowlBarPanelItem GowlBarPanelItem;

#define GOWL_TYPE_BAR_PANEL_ITEM (gowl_bar_panel_item_get_type())

GType gowl_bar_panel_item_get_type (void) G_GNUC_CONST;

GowlBarPanelItem *gowl_bar_panel_item_new  (GowlBarItemKind kind);
GowlBarPanelItem *gowl_bar_panel_item_copy (const GowlBarPanelItem *self);
void              gowl_bar_panel_item_free (GowlBarPanelItem *self);

GowlBarItemKind gowl_bar_panel_item_get_kind (const GowlBarPanelItem *self);

/**
 * gowl_bar_panel_item_set_id:
 * @self: a #GowlBarPanelItem
 * @id: (nullable): the identifier reported back on activation
 *
 * Sets the string the host hands to
 * gowl_bar_plugin_panel_action() when this item is clicked or
 * dragged.  An item with no id is inert.
 */
void         gowl_bar_panel_item_set_id (GowlBarPanelItem *self,
                                          const gchar      *id);
const gchar *gowl_bar_panel_item_get_id (const GowlBarPanelItem *self);

void         gowl_bar_panel_item_set_icon     (GowlBarPanelItem *self,
                                                const gchar      *icon);
const gchar *gowl_bar_panel_item_get_icon     (const GowlBarPanelItem *self);
void         gowl_bar_panel_item_set_title    (GowlBarPanelItem *self,
                                                const gchar      *title);
const gchar *gowl_bar_panel_item_get_title    (const GowlBarPanelItem *self);
void         gowl_bar_panel_item_set_subtitle (GowlBarPanelItem *self,
                                                const gchar      *subtitle);
const gchar *gowl_bar_panel_item_get_subtitle (const GowlBarPanelItem *self);
void         gowl_bar_panel_item_set_value    (GowlBarPanelItem *self,
                                                const gchar      *value);
const gchar *gowl_bar_panel_item_get_value    (const GowlBarPanelItem *self);
void         gowl_bar_panel_item_set_badge    (GowlBarPanelItem *self,
                                                const gchar      *badge);
const gchar *gowl_bar_panel_item_get_badge    (const GowlBarPanelItem *self);

/* Secondary label/value, used by %GOWL_BAR_ITEM_FIELD_PAIR. */
void         gowl_bar_panel_item_set_title2 (GowlBarPanelItem *self,
                                              const gchar      *title);
const gchar *gowl_bar_panel_item_get_title2 (const GowlBarPanelItem *self);
void         gowl_bar_panel_item_set_value2 (GowlBarPanelItem *self,
                                              const gchar      *value);
const gchar *gowl_bar_panel_item_get_value2 (const GowlBarPanelItem *self);

void         gowl_bar_panel_item_set_color (GowlBarPanelItem *self,
                                             GowlBarColor      color);
GowlBarColor gowl_bar_panel_item_get_color (const GowlBarPanelItem *self);
gboolean     gowl_bar_panel_item_has_color (const GowlBarPanelItem *self);

/* Colour of the item's trailing value; independent of @color so a row
   can be neutral while its reading is red. */
void         gowl_bar_panel_item_set_value_color (GowlBarPanelItem *self,
                                                   GowlBarColor      color);
GowlBarColor gowl_bar_panel_item_get_value_color (const GowlBarPanelItem *self);
gboolean     gowl_bar_panel_item_has_value_color (const GowlBarPanelItem *self);

void     gowl_bar_panel_item_set_active   (GowlBarPanelItem *self,
                                            gboolean          active);
gboolean gowl_bar_panel_item_get_active   (const GowlBarPanelItem *self);
void     gowl_bar_panel_item_set_selected (GowlBarPanelItem *self,
                                            gboolean          selected);
gboolean gowl_bar_panel_item_get_selected (const GowlBarPanelItem *self);
void     gowl_bar_panel_item_set_disabled (GowlBarPanelItem *self,
                                            gboolean          disabled);
gboolean gowl_bar_panel_item_get_disabled (const GowlBarPanelItem *self);
void     gowl_bar_panel_item_set_busy     (GowlBarPanelItem *self,
                                            gboolean          busy);
gboolean gowl_bar_panel_item_get_busy     (const GowlBarPanelItem *self);

/**
 * gowl_bar_panel_item_set_fraction:
 * @self: a #GowlBarPanelItem
 * @fraction: 0.0--1.0
 *
 * The filled proportion of a slider or progress bar.
 */
void    gowl_bar_panel_item_set_fraction (GowlBarPanelItem *self,
                                           gdouble           fraction);
gdouble gowl_bar_panel_item_get_fraction (const GowlBarPanelItem *self);

/**
 * gowl_bar_panel_item_set_step:
 * @self: a #GowlBarPanelItem
 * @step: the quantum a drag or scroll snaps to, as a fraction
 *
 * A step of 0 leaves the slider continuous.
 */
void    gowl_bar_panel_item_set_step (GowlBarPanelItem *self, gdouble step);
gdouble gowl_bar_panel_item_get_step (const GowlBarPanelItem *self);

/**
 * gowl_bar_panel_item_add_child:
 * @self: a #GowlBarPanelItem
 * @child: (transfer full): the child item
 *
 * Appends a child.  Children are the buttons of a
 * %GOWL_BAR_ITEM_BUTTONS row and the cells of a
 * %GOWL_BAR_ITEM_CALENDAR grid; the child's index is what the host
 * reports back alongside the parent's id.
 */
void              gowl_bar_panel_item_add_child   (GowlBarPanelItem *self,
                                                    GowlBarPanelItem *child);
guint             gowl_bar_panel_item_n_children  (const GowlBarPanelItem *self);
GowlBarPanelItem *gowl_bar_panel_item_get_child   (const GowlBarPanelItem *self,
                                                    guint index);

/**
 * gowl_bar_panel_item_set_samples:
 * @self: a #GowlBarPanelItem
 * @samples: (array length=n_samples): values, each 0.0--1.0
 * @n_samples: how many
 *
 * Supplies the sample window a %GOWL_BAR_ITEM_GRAPH plots.
 */
void           gowl_bar_panel_item_set_samples (GowlBarPanelItem *self,
                                                 const gdouble    *samples,
                                                 guint             n_samples);
const gdouble *gowl_bar_panel_item_get_samples (const GowlBarPanelItem *self,
                                                 guint            *n_samples);

/**
 * gowl_bar_panel_item_set_height:
 * @self: a #GowlBarPanelItem
 * @height: a height in pixels, or 0 for the kind's natural height
 */
void gowl_bar_panel_item_set_height (GowlBarPanelItem *self, gint height);
gint gowl_bar_panel_item_get_height (const GowlBarPanelItem *self);

/**
 * gowl_bar_panel_item_set_prop:
 * @self: a #GowlBarPanelItem
 * @key: a property name
 * @value: (nullable): its value, or %NULL to unset
 *
 * An open key/value bag carried alongside the typed fields.  A plugin
 * uses it to stash whatever it needs to act on the item later --- a
 * network SSID, a container id --- without the panel model having to
 * grow a field for it.
 */
void         gowl_bar_panel_item_set_prop (GowlBarPanelItem *self,
                                            const gchar      *key,
                                            const gchar      *value);
const gchar *gowl_bar_panel_item_get_prop (const GowlBarPanelItem *self,
                                            const gchar      *key);

/* --- The panel ---------------------------------------------------- */

#define GOWL_TYPE_BAR_PANEL (gowl_bar_panel_get_type())

G_DECLARE_FINAL_TYPE(GowlBarPanel, gowl_bar_panel, GOWL, BAR_PANEL, GObject)

/**
 * gowl_bar_panel_new:
 *
 * Creates an empty panel.
 *
 * Returns: (transfer full): a new #GowlBarPanel
 */
GowlBarPanel *gowl_bar_panel_new (void);

void         gowl_bar_panel_set_width      (GowlBarPanel *self, gint width);
gint         gowl_bar_panel_get_width      (GowlBarPanel *self);
void         gowl_bar_panel_set_max_height (GowlBarPanel *self, gint height);
gint         gowl_bar_panel_get_max_height (GowlBarPanel *self);

/**
 * gowl_bar_panel_add:
 * @self: a #GowlBarPanel
 * @item: (transfer full): the item to append
 *
 * Returns: (transfer none): @item, for chaining setters
 */
GowlBarPanelItem *gowl_bar_panel_add (GowlBarPanel     *self,
                                       GowlBarPanelItem *item);

guint             gowl_bar_panel_n_items (GowlBarPanel *self);
GowlBarPanelItem *gowl_bar_panel_get_item (GowlBarPanel *self, guint index);

/**
 * gowl_bar_panel_find:
 * @self: a #GowlBarPanel
 * @id: an item id
 *
 * Returns: (transfer none) (nullable): the first item with that id
 */
GowlBarPanelItem *gowl_bar_panel_find (GowlBarPanel *self, const gchar *id);

/* --- Convenience builders ----------------------------------------- */

/**
 * gowl_bar_panel_add_hero:
 * @self: a #GowlBarPanel
 * @icon: (nullable): a glyph
 * @title: (nullable): the headline
 * @subtitle: (nullable): the line under it
 *
 * Returns: (transfer none): the new item
 */
GowlBarPanelItem *gowl_bar_panel_add_hero (GowlBarPanel *self,
                                            const gchar  *icon,
                                            const gchar  *title,
                                            const gchar  *subtitle);
GowlBarPanelItem *gowl_bar_panel_add_section (GowlBarPanel *self,
                                               const gchar  *title);
GowlBarPanelItem *gowl_bar_panel_add_label (GowlBarPanel *self,
                                             const gchar  *text);
GowlBarPanelItem *gowl_bar_panel_add_field (GowlBarPanel *self,
                                             const gchar  *label,
                                             const gchar  *value);
GowlBarPanelItem *gowl_bar_panel_add_field_pair (GowlBarPanel *self,
                                                  const gchar  *label_a,
                                                  const gchar  *value_a,
                                                  const gchar  *label_b,
                                                  const gchar  *value_b);
GowlBarPanelItem *gowl_bar_panel_add_separator (GowlBarPanel *self);
GowlBarPanelItem *gowl_bar_panel_add_spacer (GowlBarPanel *self, gint height);
GowlBarPanelItem *gowl_bar_panel_add_slider (GowlBarPanel *self,
                                              const gchar  *id,
                                              const gchar  *label,
                                              gdouble       fraction);
GowlBarPanelItem *gowl_bar_panel_add_toggle (GowlBarPanel *self,
                                              const gchar  *id,
                                              const gchar  *label,
                                              gboolean      active);
GowlBarPanelItem *gowl_bar_panel_add_progress (GowlBarPanel *self,
                                                const gchar  *label,
                                                gdouble       fraction);
GowlBarPanelItem *gowl_bar_panel_add_row (GowlBarPanel *self,
                                           const gchar  *id,
                                           const gchar  *icon,
                                           const gchar  *title,
                                           const gchar  *subtitle);
GowlBarPanelItem *gowl_bar_panel_add_graph (GowlBarPanel  *self,
                                             const gchar   *label,
                                             const gdouble *samples,
                                             guint          n_samples);

/**
 * gowl_bar_panel_add_buttons:
 * @self: a #GowlBarPanel
 * @id: the id reported for every button in the row
 *
 * Adds an empty button row.  Append buttons with
 * gowl_bar_panel_add_button(); the host reports the clicked button's
 * index alongside @id.
 *
 * Returns: (transfer none): the new item
 */
GowlBarPanelItem *gowl_bar_panel_add_buttons (GowlBarPanel *self,
                                               const gchar  *id);

/**
 * gowl_bar_panel_add_button:
 * @row: a %GOWL_BAR_ITEM_BUTTONS item
 * @label: the button's text
 * @active: whether it reads as the current choice
 *
 * Returns: (transfer none): the new button
 */
GowlBarPanelItem *gowl_bar_panel_add_button (GowlBarPanelItem *row,
                                              const gchar      *label,
                                              gboolean          active);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlBarPanelItem, gowl_bar_panel_item_free)

G_END_DECLS

#endif /* GOWL_BAR_PANEL_H */
