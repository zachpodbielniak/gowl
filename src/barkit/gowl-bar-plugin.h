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

#ifndef GOWL_BAR_PLUGIN_H
#define GOWL_BAR_PLUGIN_H

#include <glib-object.h>
#include <cairo.h>
#include <pango/pangocairo.h>

#include "barkit/gowl-barkit-types.h"
#include "barkit/gowl-bar-host.h"
#include "barkit/gowl-bar-panel.h"
#include "barkit/gowl-bar-theme.h"

G_BEGIN_DECLS

/**
 * GOWL_BAR_PLUGIN_ABI:
 *
 * The plugin ABI version.  A loadable plugin stamps this into its
 * #GowlBarPluginDesc and the registry refuses anything that does not
 * match: a plugin built against an older barkit and loaded into a
 * newer one is a segfault in the compositor, and a clear refusal at
 * load time is the only acceptable failure mode for that.
 */
#define GOWL_BAR_PLUGIN_ABI 1

#define GOWL_TYPE_BAR_PLUGIN (gowl_bar_plugin_get_type())

G_DECLARE_DERIVABLE_TYPE(GowlBarPlugin, gowl_bar_plugin,
                         GOWL, BAR_PLUGIN, GObject)

/**
 * GowlBarPluginClass:
 * @parent_class: the parent class
 * @get_title: a human-readable name for menus and diagnostics
 * @get_description: one line describing what the plugin shows
 * @activate: called once the plugin has a host and its settings;
 *   returns %FALSE with @error set to refuse to run
 * @deactivate: called before the plugin is dropped
 * @configure: applies the plugin's settings, and is called again on
 *   every reconfigure, so it must be idempotent
 * @get_interval: how often, in seconds, @poll and @poll_async want to
 *   run; 0 means every bar tick
 * @poll: refresh cheap state; runs on the compositor thread, so it may
 *   read /proc and nothing slower
 * @poll_async: refresh expensive state; runs on a worker thread and may
 *   spawn processes or block on the network
 * @measure: the plugin's natural width in pixels; the default measures
 *   the icon and label
 * @draw: paint the plugin into its slot; the default draws the icon and
 *   label, which is all most plugins ever need
 * @on_click: handle a pointer button; return %TRUE to consume it and
 *   suppress the default panel toggle
 * @on_scroll: handle a scroll over the plugin
 * @has_panel: whether clicking should open a dropdown
 * @build_panel: build that dropdown; called each time it is shown or
 *   refreshed, so it must be cheap and must not block
 * @panel_action: an item in the open panel was activated
 * @panel_opened: the panel just became visible
 * @panel_closed: the panel was dismissed
 * @padding: reserved for ABI-compatible expansion
 *
 * The contract a bar plugin implements.
 *
 * Almost every vfunc has a working default: the minimum viable plugin
 * overrides @poll and calls gowl_bar_plugin_set_label(), and gets
 * measurement, drawing, hover and the panel machinery for free.
 */
struct _GowlBarPluginClass {
	GObjectClass parent_class;

	const gchar *(*get_title)       (GowlBarPlugin *self);
	const gchar *(*get_description) (GowlBarPlugin *self);

	gboolean (*activate)   (GowlBarPlugin *self, GError **error);
	void     (*deactivate) (GowlBarPlugin *self);
	void     (*configure)  (GowlBarPlugin *self, GHashTable *settings);

	gint     (*get_interval) (GowlBarPlugin *self);
	void     (*poll)         (GowlBarPlugin *self);
	void     (*poll_async)   (GowlBarPlugin *self);

	gint     (*measure) (GowlBarPlugin      *self,
	                     PangoLayout        *layout,
	                     const GowlBarTheme *theme,
	                     gint                height);
	void     (*draw)    (GowlBarPlugin      *self,
	                     cairo_t            *cr,
	                     PangoLayout        *layout,
	                     const GowlBarTheme *theme,
	                     gint x, gint y, gint width, gint height,
	                     gboolean hovered, gboolean panel_open);

	gboolean (*on_click)  (GowlBarPlugin *self, guint button,
	                       gint x, gint y, guint modifiers);
	gboolean (*on_scroll) (GowlBarPlugin *self, gdouble delta,
	                       gint discrete, guint modifiers);

	gboolean      (*has_panel)    (GowlBarPlugin *self);
	GowlBarPanel *(*build_panel)  (GowlBarPlugin *self);
	void          (*panel_action) (GowlBarPlugin *self,
	                               const gchar   *item_id,
	                               gint           index,
	                               gdouble        value,
	                               guint          button);
	void          (*panel_opened) (GowlBarPlugin *self);
	void          (*panel_closed) (GowlBarPlugin *self);

	gpointer padding[8];
};

/* --- Identity ----------------------------------------------------- */

/**
 * gowl_bar_plugin_get_id:
 * @self: a plugin
 *
 * The instance's id: the name it is addressed by in configuration, in
 * IPC and in a toast's panel target.  Two instances of the same plugin
 * type must have distinct ids, which is why this is an instance
 * property rather than a class method.
 *
 * Returns: (transfer none): the id
 */
const gchar *gowl_bar_plugin_get_id (GowlBarPlugin *self);
void         gowl_bar_plugin_set_id (GowlBarPlugin *self, const gchar *id);

const gchar *gowl_bar_plugin_get_title       (GowlBarPlugin *self);
const gchar *gowl_bar_plugin_get_description (GowlBarPlugin *self);

/* --- Lifecycle ---------------------------------------------------- */

gboolean gowl_bar_plugin_activate   (GowlBarPlugin *self, GError **error);
void     gowl_bar_plugin_deactivate (GowlBarPlugin *self);
gboolean gowl_bar_plugin_is_active  (GowlBarPlugin *self);

/**
 * gowl_bar_plugin_configure:
 * @self: a plugin
 * @settings: (element-type utf8 utf8) (nullable): the settings map
 *
 * Merges @settings into the plugin's own map and calls the subclass
 * hook.  Keys are the flat `string -> string' pairs the rest of gowl's
 * module configuration uses.
 */
void gowl_bar_plugin_configure (GowlBarPlugin *self, GHashTable *settings);

/**
 * gowl_bar_plugin_get_setting:
 * @self: a plugin
 * @key: a setting name
 *
 * Returns: (transfer none) (nullable): the value, or %NULL
 */
const gchar *gowl_bar_plugin_get_setting (GowlBarPlugin *self,
                                           const gchar   *key);
gint         gowl_bar_plugin_get_setting_int (GowlBarPlugin *self,
                                               const gchar   *key,
                                               gint           fallback);
gdouble      gowl_bar_plugin_get_setting_double (GowlBarPlugin *self,
                                                  const gchar   *key,
                                                  gdouble        fallback);
gboolean     gowl_bar_plugin_get_setting_bool (GowlBarPlugin *self,
                                                const gchar   *key,
                                                gboolean       fallback);

/**
 * gowl_bar_plugin_set_setting:
 * @self: a plugin
 * @key: a setting name
 * @value: (nullable): its value, or %NULL to unset
 *
 * Sets one setting directly.  Used by the host when instantiating a
 * plugin from a spec such as `disk:/var', which becomes the `param'
 * setting.
 */
void gowl_bar_plugin_set_setting (GowlBarPlugin *self,
                                   const gchar   *key,
                                   const gchar   *value);

/* --- Host --------------------------------------------------------- */

/**
 * gowl_bar_plugin_set_host:
 * @self: a plugin
 * @host: (nullable): the host, or %NULL to detach
 *
 * The host is held weakly: a plugin outliving its host is a bug, but a
 * bug that should log rather than crash.
 */
void         gowl_bar_plugin_set_host (GowlBarPlugin *self,
                                        GowlBarHost   *host);
GowlBarHost *gowl_bar_plugin_get_host (GowlBarPlugin *self);

/* Shorthands over the host, safe to call with no host attached. */
void gowl_bar_plugin_request_redraw (GowlBarPlugin *self);
void gowl_bar_plugin_refresh_panel  (GowlBarPlugin *self);
void gowl_bar_plugin_spawn          (GowlBarPlugin *self,
                                      const gchar   *cmdline);

/**
 * gowl_bar_plugin_notify:
 * @self: a plugin
 * @urgency: how loudly to say it
 * @summary: the headline
 * @body: (nullable): the detail line
 *
 * Raises a toast attributed to this plugin, with its panel wired to
 * the toast so a click lands on the plugin that raised it.
 */
void gowl_bar_plugin_notify (GowlBarPlugin       *self,
                              GowlBarToastUrgency  urgency,
                              const gchar         *summary,
                              const gchar         *body);

/**
 * gowl_bar_plugin_queue_work:
 * @self: a plugin
 * @func: a blocking callback
 * @user_data: passed to @func
 * @destroy: (nullable): frees @user_data afterwards
 */
void gowl_bar_plugin_queue_work (GowlBarPlugin   *self,
                                  GowlBarWorkFunc  func,
                                  gpointer         user_data,
                                  GDestroyNotify   destroy);

/* --- Presentation ------------------------------------------------- */

/**
 * gowl_bar_plugin_set_label:
 * @self: a plugin
 * @text: (nullable): the text shown in the bar, or %NULL to show none
 *
 * Thread-safe.  This is the call a @poll_async implementation makes
 * from its worker thread once it has a fresh reading; the bar picks it
 * up on the next repaint.
 */
void   gowl_bar_plugin_set_label (GowlBarPlugin *self, const gchar *text);

/**
 * gowl_bar_plugin_dup_label:
 * @self: a plugin
 *
 * Returns: (transfer full) (nullable): a copy of the current label
 */
gchar *gowl_bar_plugin_dup_label (GowlBarPlugin *self);

/**
 * gowl_bar_plugin_set_icon:
 * @self: a plugin
 * @icon: (nullable): a glyph drawn before the label
 *
 * Thread-safe, like gowl_bar_plugin_set_label().
 */
void   gowl_bar_plugin_set_icon (GowlBarPlugin *self, const gchar *icon);
gchar *gowl_bar_plugin_dup_icon (GowlBarPlugin *self);

/**
 * gowl_bar_plugin_set_tooltip:
 * @self: a plugin
 * @tooltip: (nullable): text shown on hover
 */
void   gowl_bar_plugin_set_tooltip (GowlBarPlugin *self,
                                     const gchar   *tooltip);
gchar *gowl_bar_plugin_dup_tooltip (GowlBarPlugin *self);

/**
 * gowl_bar_plugin_set_color:
 * @self: a plugin
 * @color: a theme role for the label
 *
 * Thread-safe.  A plugin that turns red when a reading is bad sets
 * this from its poll, not from its draw.
 */
void         gowl_bar_plugin_set_color (GowlBarPlugin *self,
                                         GowlBarColor   color);
GowlBarColor gowl_bar_plugin_get_color (GowlBarPlugin *self);

/**
 * gowl_bar_plugin_set_visible:
 * @self: a plugin
 * @visible: whether it occupies space in the bar
 *
 * An invisible plugin keeps polling.  That is deliberate: a battery
 * plugin on a desktop hides itself but must still notice when a UPS
 * appears.
 */
void     gowl_bar_plugin_set_visible (GowlBarPlugin *self, gboolean visible);
gboolean gowl_bar_plugin_get_visible (GowlBarPlugin *self);

/**
 * gowl_bar_plugin_set_dirty:
 * @self: a plugin
 *
 * Marks the plugin as needing a repaint without going through the
 * host.  The bar folds this into its own change detection.
 */
void     gowl_bar_plugin_set_dirty  (GowlBarPlugin *self);
gboolean gowl_bar_plugin_take_dirty (GowlBarPlugin *self);

/* --- Dispatch ----------------------------------------------------- */

gint gowl_bar_plugin_get_interval (GowlBarPlugin *self);
void gowl_bar_plugin_poll         (GowlBarPlugin *self);
void gowl_bar_plugin_poll_async   (GowlBarPlugin *self);

/**
 * gowl_bar_plugin_wants_async:
 * @self: a plugin
 *
 * Returns: %TRUE when the plugin overrides @poll_async and so needs a
 *   worker thread
 */
gboolean gowl_bar_plugin_wants_async (GowlBarPlugin *self);

gint gowl_bar_plugin_measure (GowlBarPlugin      *self,
                               PangoLayout        *layout,
                               const GowlBarTheme *theme,
                               gint                height);
void gowl_bar_plugin_draw    (GowlBarPlugin      *self,
                               cairo_t            *cr,
                               PangoLayout        *layout,
                               const GowlBarTheme *theme,
                               gint x, gint y, gint width, gint height,
                               gboolean hovered, gboolean panel_open);

gboolean gowl_bar_plugin_on_click  (GowlBarPlugin *self, guint button,
                                     gint x, gint y, guint modifiers);
gboolean gowl_bar_plugin_on_scroll (GowlBarPlugin *self, gdouble delta,
                                     gint discrete, guint modifiers);

gboolean      gowl_bar_plugin_has_panel    (GowlBarPlugin *self);
GowlBarPanel *gowl_bar_plugin_build_panel  (GowlBarPlugin *self);
void          gowl_bar_plugin_panel_action (GowlBarPlugin *self,
                                             const gchar   *item_id,
                                             gint           index,
                                             gdouble        value,
                                             guint          button);
void          gowl_bar_plugin_panel_opened (GowlBarPlugin *self);
void          gowl_bar_plugin_panel_closed (GowlBarPlugin *self);

/* --- Helpers for subclasses --------------------------------------- */

/**
 * gowl_bar_plugin_draw_label:
 * @self: a plugin
 * @cr: the target context
 * @layout: a #PangoLayout
 * @theme: the active theme
 * @x: the slot's left edge
 * @y: the slot's top edge
 * @width: the slot's width
 * @height: the slot's height
 * @hovered: whether the pointer is over the slot
 * @panel_open: whether this plugin's panel is showing
 *
 * The default drawing: an optional glyph, the label, a hover wash and
 * an open-panel underline.  A subclass that only wants to add
 * something on top of the standard look calls this and then draws.
 */
void gowl_bar_plugin_draw_label (GowlBarPlugin      *self,
                                  cairo_t            *cr,
                                  PangoLayout        *layout,
                                  const GowlBarTheme *theme,
                                  gint x, gint y, gint width, gint height,
                                  gboolean hovered, gboolean panel_open);

/**
 * gowl_bar_plugin_measure_label:
 * @self: a plugin
 * @layout: a #PangoLayout
 * @theme: the active theme
 * @height: the bar height
 *
 * Returns: the width the default drawing would need, or 0 when the
 *   plugin currently has nothing to show
 */
gint gowl_bar_plugin_measure_label (GowlBarPlugin      *self,
                                     PangoLayout        *layout,
                                     const GowlBarTheme *theme,
                                     gint                height);

/**
 * gowl_bar_plugin_signature:
 * @self: a plugin
 * @out: a #GString the signature is appended to
 *
 * Appends everything about the plugin that affects its appearance.
 * The bar skips a repaint when no plugin's signature changed, so a
 * plugin whose look depends on state beyond its label, icon and colour
 * must add that state here or it will not redraw.
 */
void gowl_bar_plugin_signature (GowlBarPlugin *self, GString *out);

G_END_DECLS

#endif /* GOWL_BAR_PLUGIN_H */
