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

#include "barkit/gowl-bar-plugin.h"

#include <string.h>

/**
 * SECTION:gowl-bar-plugin
 * @title: GowlBarPlugin
 * @short_description: the base class every bar widget derives from
 *
 * A plugin owns three things: a label it puts in the bar, an optional
 * panel it puts under the bar, and whatever it does when either is
 * clicked.  Everything else --- measurement, layout, hover, hit
 * testing, the panel's rendering and scrolling, the fault guard --- is
 * the host's, so a plugin is usually a poll function and a panel
 * builder.
 *
 * The label, icon, colour and tooltip are guarded by a mutex because
 * the whole point of @poll_async is that it runs off the compositor
 * thread; a subclass sets them from either thread with the setters
 * here and never touches the fields directly.
 */

typedef struct {
	gchar        *id;
	GowlBarHost  *host;        /* weak */

	GHashTable   *settings;    /* gchar* -> gchar* */

	/* Presentation state.  Written from worker threads, read from the
	   compositor thread during a paint, so everything under here is
	   behind `lock'. */
	GMutex        lock;
	gchar        *label;
	gchar        *icon;
	gchar        *tooltip;
	GowlBarColor  color;
	gboolean      has_color;

	gboolean      visible;
	gboolean      active;
	volatile gint dirty;
} GowlBarPluginPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(GowlBarPlugin, gowl_bar_plugin,
                                    G_TYPE_OBJECT)

enum {
	PROP_0,
	PROP_ID,
	PROP_VISIBLE,
	PROP_LAST
};

static GParamSpec *plugin_props[PROP_LAST];

enum {
	SIGNAL_CHANGED,
	SIGNAL_PANEL_CHANGED,
	SIGNAL_LAST
};

static guint plugin_signals[SIGNAL_LAST];

#define PRIV(o) ((GowlBarPluginPrivate *) \
	gowl_bar_plugin_get_instance_private(GOWL_BAR_PLUGIN(o)))

/* ----------------------------------------------------------------
 * Defaults
 * ---------------------------------------------------------------- */

static const gchar *
default_get_title(GowlBarPlugin *self)
{
	return gowl_bar_plugin_get_id(self);
}

static const gchar *
default_get_description(GowlBarPlugin *self)
{
	(void)self;
	return "";
}

static gboolean
default_activate(GowlBarPlugin *self, GError **error)
{
	(void)self;
	(void)error;
	return TRUE;
}

static void
default_deactivate(GowlBarPlugin *self)
{
	(void)self;
}

static void
default_configure(GowlBarPlugin *self, GHashTable *settings)
{
	(void)self;
	(void)settings;
}

static gint
default_get_interval(GowlBarPlugin *self)
{
	(void)self;
	return 0;
}

static void
default_poll(GowlBarPlugin *self)
{
	(void)self;
}

static gboolean
default_has_panel(GowlBarPlugin *self)
{
	GowlBarPluginClass *klass;

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	/* A plugin that can build a panel has one.  Making the default
	   derive from build_panel means a subclass never has to remember
	   to say so twice. */
	return klass->build_panel != NULL;
}

static gint
default_measure(GowlBarPlugin *self, PangoLayout *layout,
                const GowlBarTheme *theme, gint height)
{
	return gowl_bar_plugin_measure_label(self, layout, theme, height);
}

static void
default_draw(GowlBarPlugin *self, cairo_t *cr, PangoLayout *layout,
             const GowlBarTheme *theme, gint x, gint y, gint width,
             gint height, gboolean hovered, gboolean panel_open)
{
	gowl_bar_plugin_draw_label(self, cr, layout, theme, x, y, width,
	                           height, hovered, panel_open);
}

/* ----------------------------------------------------------------
 * GObject
 * ---------------------------------------------------------------- */

static void
gowl_bar_plugin_finalize(GObject *object)
{
	GowlBarPluginPrivate *priv = PRIV(object);

	if (priv->host != NULL) {
		g_object_remove_weak_pointer(G_OBJECT(priv->host),
		                             (gpointer *)&priv->host);
		priv->host = NULL;
	}

	g_free(priv->id);
	g_free(priv->label);
	g_free(priv->icon);
	g_free(priv->tooltip);
	if (priv->settings != NULL)
		g_hash_table_unref(priv->settings);
	g_mutex_clear(&priv->lock);

	G_OBJECT_CLASS(gowl_bar_plugin_parent_class)->finalize(object);
}

static void
gowl_bar_plugin_get_property(GObject *object, guint prop_id,
                             GValue *value, GParamSpec *pspec)
{
	GowlBarPlugin *self = GOWL_BAR_PLUGIN(object);

	switch (prop_id) {
	case PROP_ID:
		g_value_set_string(value, gowl_bar_plugin_get_id(self));
		break;
	case PROP_VISIBLE:
		g_value_set_boolean(value, gowl_bar_plugin_get_visible(self));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
gowl_bar_plugin_set_property(GObject *object, guint prop_id,
                             const GValue *value, GParamSpec *pspec)
{
	GowlBarPlugin *self = GOWL_BAR_PLUGIN(object);

	switch (prop_id) {
	case PROP_ID:
		gowl_bar_plugin_set_id(self, g_value_get_string(value));
		break;
	case PROP_VISIBLE:
		gowl_bar_plugin_set_visible(self,
			g_value_get_boolean(value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
gowl_bar_plugin_class_init(GowlBarPluginClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize     = gowl_bar_plugin_finalize;
	object_class->get_property = gowl_bar_plugin_get_property;
	object_class->set_property = gowl_bar_plugin_set_property;

	klass->get_title       = default_get_title;
	klass->get_description = default_get_description;
	klass->activate        = default_activate;
	klass->deactivate      = default_deactivate;
	klass->configure       = default_configure;
	klass->get_interval    = default_get_interval;
	klass->poll            = default_poll;
	klass->poll_async      = NULL;
	klass->wants_async     = NULL;
	klass->measure         = default_measure;
	klass->draw            = default_draw;
	klass->on_click        = NULL;
	klass->on_scroll       = NULL;
	klass->has_panel       = default_has_panel;
	klass->build_panel     = NULL;
	klass->panel_action    = NULL;
	klass->panel_opened    = NULL;
	klass->panel_closed    = NULL;
	klass->panel_key       = NULL;

	/**
	 * GowlBarPlugin:id:
	 *
	 * The instance identifier used in configuration and IPC.
	 */
	plugin_props[PROP_ID] =
		g_param_spec_string("id", "Id", "Plugin instance identifier",
		                    NULL,
		                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	/**
	 * GowlBarPlugin:visible:
	 *
	 * Whether the plugin occupies space in the bar.
	 */
	plugin_props[PROP_VISIBLE] =
		g_param_spec_boolean("visible", "Visible",
		                     "Whether the plugin occupies bar space",
		                     TRUE,
		                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, PROP_LAST,
	                                  plugin_props);

	/**
	 * GowlBarPlugin::changed:
	 * @self: the plugin
	 *
	 * Emitted when the plugin's bar presentation changed.  The host
	 * connects to this to repaint; a plugin normally does not emit it
	 * by hand, because the label and icon setters do.
	 */
	plugin_signals[SIGNAL_CHANGED] =
		g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 0);

	/**
	 * GowlBarPlugin::panel-changed:
	 * @self: the plugin
	 *
	 * Emitted when the plugin's panel content changed while it is
	 * open, so the host rebuilds it.
	 */
	plugin_signals[SIGNAL_PANEL_CHANGED] =
		g_signal_new("panel-changed", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 0);
}

static void
gowl_bar_plugin_init(GowlBarPlugin *self)
{
	GowlBarPluginPrivate *priv = PRIV(self);

	g_mutex_init(&priv->lock);
	priv->settings = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                       g_free, g_free);
	priv->color   = GOWL_BAR_COLOR_TEXT;
	priv->visible = TRUE;
	priv->dirty   = 1;
}

/* ----------------------------------------------------------------
 * Identity
 * ---------------------------------------------------------------- */

/**
 * gowl_bar_plugin_get_id:
 * @self: a plugin
 *
 * Returns: (transfer none): the instance id
 */
const gchar *
gowl_bar_plugin_get_id(GowlBarPlugin *self)
{
	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), "");
	return (PRIV(self)->id != NULL) ? PRIV(self)->id : "";
}

/**
 * gowl_bar_plugin_set_id:
 * @self: a plugin
 * @id: the instance id
 */
void
gowl_bar_plugin_set_id(GowlBarPlugin *self, const gchar *id)
{
	GowlBarPluginPrivate *priv;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	priv = PRIV(self);
	if (g_strcmp0(priv->id, id) == 0)
		return;
	g_free(priv->id);
	priv->id = g_strdup(id);
	g_object_notify_by_pspec(G_OBJECT(self), plugin_props[PROP_ID]);
}

/**
 * gowl_bar_plugin_get_title:
 * @self: a plugin
 *
 * Returns: (transfer none): a human-readable name
 */
const gchar *
gowl_bar_plugin_get_title(GowlBarPlugin *self)
{
	GowlBarPluginClass *klass;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), "");

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->get_title == NULL)
		return gowl_bar_plugin_get_id(self);
	return klass->get_title(self);
}

/**
 * gowl_bar_plugin_get_description:
 * @self: a plugin
 *
 * Returns: (transfer none): one line describing the plugin
 */
const gchar *
gowl_bar_plugin_get_description(GowlBarPlugin *self)
{
	GowlBarPluginClass *klass;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), "");

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->get_description == NULL)
		return "";
	return klass->get_description(self);
}

/* ----------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------- */

/**
 * gowl_bar_plugin_activate:
 * @self: a plugin
 * @error: (nullable): return location for a #GError
 *
 * Returns: %TRUE when the plugin is ready to run
 */
gboolean
gowl_bar_plugin_activate(GowlBarPlugin *self, GError **error)
{
	GowlBarPluginClass *klass;
	GowlBarPluginPrivate *priv;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), FALSE);

	priv = PRIV(self);
	if (priv->active)
		return TRUE;

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->activate != NULL && !klass->activate(self, error))
		return FALSE;

	priv->active = TRUE;
	return TRUE;
}

/**
 * gowl_bar_plugin_deactivate:
 * @self: a plugin
 */
void
gowl_bar_plugin_deactivate(GowlBarPlugin *self)
{
	GowlBarPluginClass *klass;
	GowlBarPluginPrivate *priv;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	priv = PRIV(self);
	if (!priv->active)
		return;

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->deactivate != NULL)
		klass->deactivate(self);
	priv->active = FALSE;
}

/**
 * gowl_bar_plugin_is_active:
 * @self: a plugin
 *
 * Returns: %TRUE when the plugin has been activated
 */
gboolean
gowl_bar_plugin_is_active(GowlBarPlugin *self)
{
	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), FALSE);
	return PRIV(self)->active;
}

/**
 * gowl_bar_plugin_configure:
 * @self: a plugin
 * @settings: (element-type utf8 utf8) (nullable): the settings map
 */
void
gowl_bar_plugin_configure(GowlBarPlugin *self, GHashTable *settings)
{
	GowlBarPluginClass *klass;
	GowlBarPluginPrivate *priv;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	priv = PRIV(self);

	if (settings != NULL) {
		GHashTableIter iter;
		gpointer k, v;

		g_hash_table_iter_init(&iter, settings);
		while (g_hash_table_iter_next(&iter, &k, &v)) {
			g_hash_table_insert(priv->settings, g_strdup(k),
			                    g_strdup(v));
		}
	}

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->configure != NULL)
		klass->configure(self, priv->settings);

	gowl_bar_plugin_set_dirty(self);
}

/**
 * gowl_bar_plugin_get_setting:
 * @self: a plugin
 * @key: a setting name
 *
 * Returns: (transfer none) (nullable): the value
 */
const gchar *
gowl_bar_plugin_get_setting(GowlBarPlugin *self, const gchar *key)
{
	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), NULL);
	g_return_val_if_fail(key != NULL, NULL);

	return (const gchar *)g_hash_table_lookup(PRIV(self)->settings, key);
}

/**
 * gowl_bar_plugin_get_setting_int:
 * @self: a plugin
 * @key: a setting name
 * @fallback: the value to use when the setting is absent
 *
 * Returns: the setting as an integer
 */
gint
gowl_bar_plugin_get_setting_int(GowlBarPlugin *self, const gchar *key,
                                gint fallback)
{
	const gchar *value;

	value = gowl_bar_plugin_get_setting(self, key);
	if (value == NULL || value[0] == '\0')
		return fallback;
	return (gint)g_ascii_strtoll(value, NULL, 10);
}

/**
 * gowl_bar_plugin_get_setting_double:
 * @self: a plugin
 * @key: a setting name
 * @fallback: the value to use when the setting is absent
 *
 * Returns: the setting as a double
 */
gdouble
gowl_bar_plugin_get_setting_double(GowlBarPlugin *self, const gchar *key,
                                   gdouble fallback)
{
	const gchar *value;

	value = gowl_bar_plugin_get_setting(self, key);
	if (value == NULL || value[0] == '\0')
		return fallback;
	return g_ascii_strtod(value, NULL);
}

/**
 * gowl_bar_plugin_get_setting_bool:
 * @self: a plugin
 * @key: a setting name
 * @fallback: the value to use when the setting is absent
 *
 * Accepts the whole spread of truthy spellings gowl's config uses ---
 * `true', `t', `1', `yes', `on' --- because these values arrive from
 * YAML, from Elisp and from an IPC command, and each spells booleans
 * its own way.
 *
 * Returns: the setting as a boolean
 */
gboolean
gowl_bar_plugin_get_setting_bool(GowlBarPlugin *self, const gchar *key,
                                 gboolean fallback)
{
	const gchar *value;

	value = gowl_bar_plugin_get_setting(self, key);
	if (value == NULL || value[0] == '\0')
		return fallback;

	if (g_ascii_strcasecmp(value, "true") == 0 ||
	    g_ascii_strcasecmp(value, "t") == 0 ||
	    g_ascii_strcasecmp(value, "1") == 0 ||
	    g_ascii_strcasecmp(value, "yes") == 0 ||
	    g_ascii_strcasecmp(value, "on") == 0)
		return TRUE;
	if (g_ascii_strcasecmp(value, "false") == 0 ||
	    g_ascii_strcasecmp(value, "nil") == 0 ||
	    g_ascii_strcasecmp(value, "0") == 0 ||
	    g_ascii_strcasecmp(value, "no") == 0 ||
	    g_ascii_strcasecmp(value, "off") == 0)
		return FALSE;
	return fallback;
}

/**
 * gowl_bar_plugin_set_setting:
 * @self: a plugin
 * @key: a setting name
 * @value: (nullable): its value, or %NULL to unset
 */
void
gowl_bar_plugin_set_setting(GowlBarPlugin *self, const gchar *key,
                            const gchar *value)
{
	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));
	g_return_if_fail(key != NULL);

	if (value == NULL)
		g_hash_table_remove(PRIV(self)->settings, key);
	else
		g_hash_table_insert(PRIV(self)->settings, g_strdup(key),
		                    g_strdup(value));
}

/* ----------------------------------------------------------------
 * Host
 * ---------------------------------------------------------------- */

/**
 * gowl_bar_plugin_set_host:
 * @self: a plugin
 * @host: (nullable): the host
 */
void
gowl_bar_plugin_set_host(GowlBarPlugin *self, GowlBarHost *host)
{
	GowlBarPluginPrivate *priv;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	priv = PRIV(self);
	if (priv->host == host)
		return;

	if (priv->host != NULL) {
		g_object_remove_weak_pointer(G_OBJECT(priv->host),
		                             (gpointer *)&priv->host);
	}
	priv->host = host;
	if (priv->host != NULL) {
		g_object_add_weak_pointer(G_OBJECT(priv->host),
		                          (gpointer *)&priv->host);
	}
}

/**
 * gowl_bar_plugin_get_host:
 * @self: a plugin
 *
 * Returns: (transfer none) (nullable): the host
 */
GowlBarHost *
gowl_bar_plugin_get_host(GowlBarPlugin *self)
{
	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), NULL);
	return PRIV(self)->host;
}

/**
 * gowl_bar_plugin_request_redraw:
 * @self: a plugin
 */
void
gowl_bar_plugin_request_redraw(GowlBarPlugin *self)
{
	GowlBarHost *host;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	gowl_bar_plugin_set_dirty(self);
	host = PRIV(self)->host;
	if (host != NULL)
		gowl_bar_host_request_redraw(host);
}

/**
 * gowl_bar_plugin_refresh_panel:
 * @self: a plugin
 */
void
gowl_bar_plugin_refresh_panel(GowlBarPlugin *self)
{
	GowlBarHost *host;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	g_signal_emit(self, plugin_signals[SIGNAL_PANEL_CHANGED], 0);
	host = PRIV(self)->host;
	if (host != NULL)
		gowl_bar_host_request_panel_refresh(host, self);
}

/**
 * gowl_bar_plugin_spawn:
 * @self: a plugin
 * @cmdline: a shell command line
 */
void
gowl_bar_plugin_request_panel_refresh(GowlBarPlugin *self)
{
	GowlBarHost *host;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	host = PRIV(self)->host;
	if (host != NULL)
		gowl_bar_host_request_panel_refresh(host, self);
}

void
gowl_bar_plugin_spawn(GowlBarPlugin *self, const gchar *cmdline)
{
	GowlBarHost *host;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	host = PRIV(self)->host;
	if (host != NULL)
		gowl_bar_host_spawn(host, cmdline);
}

/**
 * gowl_bar_plugin_set_bar_setting:
 * @self: a plugin
 * @key: a bar configuration key, e.g. `theme-scale'
 * @value: its new value
 *
 * Changes one of the BAR's settings, not the plugin's own.
 *
 * gowl_bar_plugin_set_setting() stores state belonging to this plugin;
 * this reaches the bar's configuration, so a control in a panel can do
 * what the config file does.  Without it a panel offering, say, a text
 * size could only record a wish and tell the user to go and edit a file
 * -- which is what the display plugin did, writing a `requested-scale'
 * nothing ever read.
 *
 * Returns: %TRUE if the host recognised and applied @key
 */
gboolean
gowl_bar_plugin_set_bar_setting(GowlBarPlugin *self, const gchar *key,
                                const gchar *value)
{
	GowlBarHost *host;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), FALSE);

	host = PRIV(self)->host;
	if (host == NULL)
		return FALSE;
	return gowl_bar_host_set_bar_setting(host, key, value);
}

/**
 * gowl_bar_plugin_notify:
 * @self: a plugin
 * @urgency: the level
 * @summary: the headline
 * @body: (nullable): the detail line
 */
void
gowl_bar_plugin_notify(GowlBarPlugin *self, GowlBarToastUrgency urgency,
                       const gchar *summary, const gchar *body)
{
	GowlBarHost  *host;
	GowlBarToast *toast;
	g_autofree gchar *icon = NULL;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	host = PRIV(self)->host;
	if (host == NULL) {
		g_message("gowl-bar: %s: %s%s%s",
		          gowl_bar_plugin_get_id(self),
		          (summary != NULL) ? summary : "",
		          (body != NULL) ? " -- " : "",
		          (body != NULL) ? body : "");
		return;
	}

	toast = gowl_bar_toast_new(summary, body);
	gowl_bar_toast_set_urgency(toast, urgency);
	gowl_bar_toast_set_app(toast, gowl_bar_plugin_get_title(self));

	icon = gowl_bar_plugin_dup_icon(self);
	if (icon != NULL)
		gowl_bar_toast_set_icon(toast, icon);

	/* Wire the toast back to the plugin that raised it: a plugin's
	   notification almost always wants to hand you its own panel. */
	if (gowl_bar_plugin_has_panel(self))
		gowl_bar_toast_set_panel(toast, gowl_bar_plugin_get_id(self));

	gowl_bar_host_notify(host, toast);
}

/**
 * gowl_bar_plugin_queue_work:
 * @self: a plugin
 * @func: a blocking callback
 * @user_data: passed to @func
 * @destroy: (nullable): frees @user_data afterwards
 */
void
gowl_bar_plugin_queue_work(GowlBarPlugin *self, GowlBarWorkFunc func,
                           gpointer user_data, GDestroyNotify destroy)
{
	GowlBarHost *host;

	if (!GOWL_IS_BAR_PLUGIN(self)) {
		if (destroy != NULL && user_data != NULL)
			destroy(user_data);
		return;
	}

	host = PRIV(self)->host;
	if (host == NULL) {
		if (destroy != NULL && user_data != NULL)
			destroy(user_data);
		return;
	}
	gowl_bar_host_queue_work(host, self, func, user_data, destroy);
}

/* ----------------------------------------------------------------
 * Presentation
 * ---------------------------------------------------------------- */

/**
 * gowl_bar_plugin_set_label:
 * @self: a plugin
 * @text: (nullable): the bar text
 */
void
gowl_bar_plugin_set_label(GowlBarPlugin *self, const gchar *text)
{
	GowlBarPluginPrivate *priv;
	gboolean changed;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	priv = PRIV(self);
	g_mutex_lock(&priv->lock);
	changed = (g_strcmp0(priv->label, text) != 0);
	if (changed) {
		g_free(priv->label);
		priv->label = g_strdup(text);
	}
	g_mutex_unlock(&priv->lock);

	if (changed) {
		g_atomic_int_set(&priv->dirty, 1);
		/* The signal is emitted outside the lock: a handler that
		   calls back into a setter would otherwise deadlock, and the
		   host's handler does exactly that kind of thing. */
		g_signal_emit(self, plugin_signals[SIGNAL_CHANGED], 0);
	}
}

/**
 * gowl_bar_plugin_dup_label:
 * @self: a plugin
 *
 * Returns: (transfer full) (nullable): a copy of the label
 */
gchar *
gowl_bar_plugin_dup_label(GowlBarPlugin *self)
{
	GowlBarPluginPrivate *priv;
	gchar *copy;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), NULL);

	priv = PRIV(self);
	g_mutex_lock(&priv->lock);
	copy = g_strdup(priv->label);
	g_mutex_unlock(&priv->lock);
	return copy;
}

/**
 * gowl_bar_plugin_set_icon:
 * @self: a plugin
 * @icon: (nullable): a glyph
 */
void
gowl_bar_plugin_set_icon(GowlBarPlugin *self, const gchar *icon)
{
	GowlBarPluginPrivate *priv;
	gboolean changed;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	priv = PRIV(self);
	g_mutex_lock(&priv->lock);
	changed = (g_strcmp0(priv->icon, icon) != 0);
	if (changed) {
		g_free(priv->icon);
		priv->icon = g_strdup(icon);
	}
	g_mutex_unlock(&priv->lock);

	if (changed) {
		g_atomic_int_set(&priv->dirty, 1);
		g_signal_emit(self, plugin_signals[SIGNAL_CHANGED], 0);
	}
}

/**
 * gowl_bar_plugin_dup_icon:
 * @self: a plugin
 *
 * Returns: (transfer full) (nullable): a copy of the icon glyph
 */
gchar *
gowl_bar_plugin_dup_icon(GowlBarPlugin *self)
{
	GowlBarPluginPrivate *priv;
	gchar *copy;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), NULL);

	priv = PRIV(self);
	g_mutex_lock(&priv->lock);
	copy = g_strdup(priv->icon);
	g_mutex_unlock(&priv->lock);
	return copy;
}

/**
 * gowl_bar_plugin_set_tooltip:
 * @self: a plugin
 * @tooltip: (nullable): hover text
 */
void
gowl_bar_plugin_set_tooltip(GowlBarPlugin *self, const gchar *tooltip)
{
	GowlBarPluginPrivate *priv;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	priv = PRIV(self);
	g_mutex_lock(&priv->lock);
	g_free(priv->tooltip);
	priv->tooltip = g_strdup(tooltip);
	g_mutex_unlock(&priv->lock);
}

/**
 * gowl_bar_plugin_dup_tooltip:
 * @self: a plugin
 *
 * Returns: (transfer full) (nullable): a copy of the tooltip
 */
gchar *
gowl_bar_plugin_dup_tooltip(GowlBarPlugin *self)
{
	GowlBarPluginPrivate *priv;
	gchar *copy;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), NULL);

	priv = PRIV(self);
	g_mutex_lock(&priv->lock);
	copy = g_strdup(priv->tooltip);
	g_mutex_unlock(&priv->lock);
	return copy;
}

/**
 * gowl_bar_plugin_set_color:
 * @self: a plugin
 * @color: a theme role
 */
void
gowl_bar_plugin_set_color(GowlBarPlugin *self, GowlBarColor color)
{
	GowlBarPluginPrivate *priv;
	gboolean changed;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	priv = PRIV(self);
	g_mutex_lock(&priv->lock);
	changed = (!priv->has_color || priv->color != color);
	priv->color     = color;
	priv->has_color = TRUE;
	g_mutex_unlock(&priv->lock);

	if (changed) {
		g_atomic_int_set(&priv->dirty, 1);
		g_signal_emit(self, plugin_signals[SIGNAL_CHANGED], 0);
	}
}

/**
 * gowl_bar_plugin_get_color:
 * @self: a plugin
 *
 * Returns: the label's theme role
 */
GowlBarColor
gowl_bar_plugin_get_color(GowlBarPlugin *self)
{
	GowlBarPluginPrivate *priv;
	GowlBarColor color;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), GOWL_BAR_COLOR_TEXT);

	priv = PRIV(self);
	g_mutex_lock(&priv->lock);
	color = priv->color;
	g_mutex_unlock(&priv->lock);
	return color;
}

/**
 * gowl_bar_plugin_set_visible:
 * @self: a plugin
 * @visible: whether it occupies bar space
 */
void
gowl_bar_plugin_set_visible(GowlBarPlugin *self, gboolean visible)
{
	GowlBarPluginPrivate *priv;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	priv = PRIV(self);
	if (priv->visible == visible)
		return;
	priv->visible = visible;
	g_atomic_int_set(&priv->dirty, 1);
	g_object_notify_by_pspec(G_OBJECT(self), plugin_props[PROP_VISIBLE]);
	g_signal_emit(self, plugin_signals[SIGNAL_CHANGED], 0);
}

/**
 * gowl_bar_plugin_get_visible:
 * @self: a plugin
 *
 * Returns: %TRUE when the plugin occupies bar space
 */
gboolean
gowl_bar_plugin_get_visible(GowlBarPlugin *self)
{
	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), FALSE);
	return PRIV(self)->visible;
}

/**
 * gowl_bar_plugin_set_dirty:
 * @self: a plugin
 */
void
gowl_bar_plugin_set_dirty(GowlBarPlugin *self)
{
	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));
	g_atomic_int_set(&PRIV(self)->dirty, 1);
}

/**
 * gowl_bar_plugin_take_dirty:
 * @self: a plugin
 *
 * Reads and clears the dirty flag.
 *
 * Returns: %TRUE when the plugin had been marked dirty
 */
gboolean
gowl_bar_plugin_take_dirty(GowlBarPlugin *self)
{
	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), FALSE);
	return g_atomic_int_compare_and_exchange(&PRIV(self)->dirty, 1, 0);
}

/* ----------------------------------------------------------------
 * Dispatch
 * ---------------------------------------------------------------- */

/**
 * gowl_bar_plugin_get_interval:
 * @self: a plugin
 *
 * Returns: the poll interval in seconds, or 0 for every tick
 */
gint
gowl_bar_plugin_get_interval(GowlBarPlugin *self)
{
	GowlBarPluginClass *klass;
	gint configured;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), 0);

	/* An explicit `interval' setting always wins: the per-widget `@N'
	   suffix in a bar config lands here, and the user overriding a
	   plugin's own idea of how often it should run is the entire
	   point of that syntax. */
	configured = gowl_bar_plugin_get_setting_int(self, "interval", -1);
	if (configured >= 0)
		return configured;

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->get_interval == NULL)
		return 0;
	return klass->get_interval(self);
}

/**
 * gowl_bar_plugin_poll:
 * @self: a plugin
 */
void
gowl_bar_plugin_poll(GowlBarPlugin *self)
{
	GowlBarPluginClass *klass;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->poll != NULL)
		klass->poll(self);
}

/**
 * gowl_bar_plugin_poll_async:
 * @self: a plugin
 */
void
gowl_bar_plugin_poll_async(GowlBarPlugin *self)
{
	GowlBarPluginClass *klass;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->poll_async != NULL)
		klass->poll_async(self);
}

/**
 * gowl_bar_plugin_wants_async:
 * @self: a plugin
 *
 * Returns: %TRUE when the plugin has an async poll
 */
gboolean
gowl_bar_plugin_wants_async(GowlBarPlugin *self)
{
	GowlBarPluginClass *klass;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), FALSE);

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->wants_async != NULL)
		return klass->wants_async(self);
	return klass->poll_async != NULL;
}

/**
 * gowl_bar_plugin_measure:
 * @self: a plugin
 * @layout: a #PangoLayout
 * @theme: the active theme
 * @height: the bar height
 *
 * Returns: the plugin's natural width, or 0 when it should be skipped
 */
gint
gowl_bar_plugin_measure(GowlBarPlugin *self, PangoLayout *layout,
                        const GowlBarTheme *theme, gint height)
{
	GowlBarPluginClass *klass;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), 0);

	if (!PRIV(self)->visible)
		return 0;

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->measure == NULL)
		return 0;
	return klass->measure(self, layout, theme, height);
}

/**
 * gowl_bar_plugin_draw:
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
 */
void
gowl_bar_plugin_draw(GowlBarPlugin *self, cairo_t *cr, PangoLayout *layout,
                     const GowlBarTheme *theme, gint x, gint y,
                     gint width, gint height, gboolean hovered,
                     gboolean panel_open)
{
	GowlBarPluginClass *klass;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	if (!PRIV(self)->visible)
		return;

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->draw != NULL)
		klass->draw(self, cr, layout, theme, x, y, width, height,
		            hovered, panel_open);
}

/**
 * gowl_bar_plugin_on_click:
 * @self: a plugin
 * @button: the pointer button
 * @x: x within the plugin's slot
 * @y: y within the plugin's slot
 * @modifiers: keyboard modifiers
 *
 * Returns: %TRUE when the plugin consumed the click
 */
gboolean
gowl_bar_plugin_on_click(GowlBarPlugin *self, guint button, gint x, gint y,
                         guint modifiers)
{
	GowlBarPluginClass *klass;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), FALSE);

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->on_click == NULL)
		return FALSE;
	return klass->on_click(self, button, x, y, modifiers);
}

/**
 * gowl_bar_plugin_on_scroll:
 * @self: a plugin
 * @delta: the scroll distance in surface units
 * @discrete: the scroll distance in wheel clicks
 * @modifiers: keyboard modifiers
 *
 * Returns: %TRUE when the plugin consumed the scroll
 */
gboolean
gowl_bar_plugin_on_scroll(GowlBarPlugin *self, gdouble delta, gint discrete,
                          guint modifiers)
{
	GowlBarPluginClass *klass;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), FALSE);

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->on_scroll == NULL)
		return FALSE;
	return klass->on_scroll(self, delta, discrete, modifiers);
}

/**
 * gowl_bar_plugin_has_panel:
 * @self: a plugin
 *
 * Returns: %TRUE when clicking should open a dropdown
 */
gboolean
gowl_bar_plugin_has_panel(GowlBarPlugin *self)
{
	GowlBarPluginClass *klass;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), FALSE);

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->has_panel == NULL)
		return FALSE;
	return klass->has_panel(self);
}

/**
 * gowl_bar_plugin_build_panel:
 * @self: a plugin
 *
 * Returns: (transfer full) (nullable): the panel to show
 */
GowlBarPanel *
gowl_bar_plugin_build_panel(GowlBarPlugin *self)
{
	GowlBarPluginClass *klass;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), NULL);

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->build_panel == NULL)
		return NULL;
	return klass->build_panel(self);
}

/**
 * gowl_bar_plugin_panel_action:
 * @self: a plugin
 * @item_id: the activated item's id
 * @index: a child index, or -1
 * @value: a slider's new fraction, or 0
 * @button: the pointer button
 */
void
gowl_bar_plugin_panel_action(GowlBarPlugin *self, const gchar *item_id,
                             gint index, gdouble value, guint button)
{
	GowlBarPluginClass *klass;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->panel_action != NULL)
		klass->panel_action(self, item_id, index, value, button);
}

/**
 * gowl_bar_plugin_panel_opened:
 * @self: a plugin
 */
void
gowl_bar_plugin_panel_opened(GowlBarPlugin *self)
{
	GowlBarPluginClass *klass;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->panel_opened != NULL)
		klass->panel_opened(self);
}

/**
 * gowl_bar_plugin_panel_closed:
 * @self: a plugin
 */
gboolean
gowl_bar_plugin_panel_key(GowlBarPlugin *self, guint keysym, guint modifiers,
                          gint focused_item)
{
	GowlBarPluginClass *klass;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), FALSE);

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->panel_key != NULL)
		return klass->panel_key(self, keysym, modifiers, focused_item);
	return FALSE;
}

void
gowl_bar_plugin_panel_closed(GowlBarPlugin *self)
{
	GowlBarPluginClass *klass;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));

	klass = GOWL_BAR_PLUGIN_GET_CLASS(self);
	if (klass->panel_closed != NULL)
		klass->panel_closed(self);
}

/* ----------------------------------------------------------------
 * Default label drawing
 * ---------------------------------------------------------------- */

/* Set the layout to the theme's body or icon font.  Returns the
   description so the caller can free it. */
static PangoFontDescription *
plugin_apply_font(PangoLayout *layout, const GowlBarTheme *theme,
                  gboolean icon)
{
	PangoFontDescription *desc;

	desc = pango_font_description_from_string(
		icon ? gowl_bar_theme_get_icon_font(theme)
		     : gowl_bar_theme_get_font(theme));
	pango_layout_set_font_description(layout, desc);
	return desc;
}

/**
 * gowl_bar_plugin_measure_label:
 * @self: a plugin
 * @layout: a #PangoLayout
 * @theme: the active theme
 * @height: the bar height
 *
 * Returns: the default drawing's natural width
 */
gint
gowl_bar_plugin_measure_label(GowlBarPlugin *self, PangoLayout *layout,
                              const GowlBarTheme *theme, gint height)
{
	g_autofree gchar *label = NULL;
	g_autofree gchar *icon = NULL;
	PangoFontDescription *desc;
	PangoRectangle logical;
	gint width, pad;

	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(self), 0);
	g_return_val_if_fail(layout != NULL, 0);

	(void)height;

	label = gowl_bar_plugin_dup_label(self);
	icon  = gowl_bar_plugin_dup_icon(self);

	if ((label == NULL || label[0] == '\0') &&
	    (icon == NULL || icon[0] == '\0'))
		return 0;

	pad   = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_ITEM_PAD);
	width = 2 * pad;

	pango_layout_set_width(layout, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
	pango_layout_set_attributes(layout, NULL);

	if (icon != NULL && icon[0] != '\0') {
		desc = plugin_apply_font(layout, theme, TRUE);
		pango_layout_set_text(layout, icon, -1);
		pango_layout_get_pixel_extents(layout, NULL, &logical);
		width += logical.width;
		pango_font_description_free(desc);
		if (label != NULL && label[0] != '\0')
			width += pad / 2 + 1;
	}

	if (label != NULL && label[0] != '\0') {
		desc = plugin_apply_font(layout, theme, FALSE);
		pango_layout_set_text(layout, label, -1);
		pango_layout_get_pixel_extents(layout, NULL, &logical);
		width += logical.width;
		pango_font_description_free(desc);
	}

	return width;
}

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
 */
void
gowl_bar_plugin_draw_label(GowlBarPlugin *self, cairo_t *cr,
                           PangoLayout *layout, const GowlBarTheme *theme,
                           gint x, gint y, gint width, gint height,
                           gboolean hovered, gboolean panel_open)
{
	g_autofree gchar *label = NULL;
	g_autofree gchar *icon = NULL;
	PangoFontDescription *desc;
	PangoRectangle logical;
	GowlBarColor color;
	gint pad, cur_x, center_y, radius;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));
	g_return_if_fail(cr != NULL);
	g_return_if_fail(layout != NULL);

	label = gowl_bar_plugin_dup_label(self);
	icon  = gowl_bar_plugin_dup_icon(self);
	color = gowl_bar_plugin_get_color(self);

	pad      = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_ITEM_PAD);
	radius   = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_RADIUS);
	center_y = y + height / 2;

	if (hovered || panel_open) {
		gowl_bar_cairo_rounded_rect(cr, x + 1, y + 2, width - 2,
		                            height - 4, radius * 0.7);
		gowl_bar_theme_cairo_set_alpha(theme, cr,
			GOWL_BAR_COLOR_SURFACE, panel_open ? 0.95 : 0.55);
		cairo_fill(cr);
	}

	if (panel_open) {
		/* A 2px rule under the open item, the way the shipped shell
		   marks which dropdown you are looking at. */
		gowl_bar_theme_cairo_set(theme, cr, GOWL_BAR_COLOR_ACCENT);
		cairo_rectangle(cr, x + pad / 2, y + height - 3,
		                width - pad, 2);
		cairo_fill(cr);
	}

	cur_x = x + pad;

	pango_layout_set_width(layout, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
	pango_layout_set_attributes(layout, NULL);

	if (icon != NULL && icon[0] != '\0') {
		desc = plugin_apply_font(layout, theme, TRUE);
		pango_layout_set_text(layout, icon, -1);
		pango_layout_get_pixel_extents(layout, NULL, &logical);
		gowl_bar_theme_cairo_set(theme, cr, color);
		cairo_move_to(cr, cur_x, center_y - logical.height / 2);
		pango_cairo_show_layout(cr, layout);
		cur_x += logical.width;
		pango_font_description_free(desc);
		if (label != NULL && label[0] != '\0')
			cur_x += pad / 2 + 1;
	}

	if (label != NULL && label[0] != '\0') {
		desc = plugin_apply_font(layout, theme, FALSE);
		pango_layout_set_text(layout, label, -1);
		pango_layout_get_pixel_extents(layout, NULL, &logical);
		gowl_bar_theme_cairo_set(theme, cr, color);
		cairo_move_to(cr, cur_x, center_y - logical.height / 2);
		pango_cairo_show_layout(cr, layout);
		pango_font_description_free(desc);
	}
}

/**
 * gowl_bar_plugin_signature:
 * @self: a plugin
 * @out: a #GString to append to
 */
void
gowl_bar_plugin_signature(GowlBarPlugin *self, GString *out)
{
	g_autofree gchar *label = NULL;
	g_autofree gchar *icon = NULL;

	g_return_if_fail(GOWL_IS_BAR_PLUGIN(self));
	g_return_if_fail(out != NULL);

	label = gowl_bar_plugin_dup_label(self);
	icon  = gowl_bar_plugin_dup_icon(self);

	g_string_append_printf(out, "%s|%d|%d|%s|%s;",
	                       gowl_bar_plugin_get_id(self),
	                       PRIV(self)->visible ? 1 : 0,
	                       (gint)gowl_bar_plugin_get_color(self),
	                       (icon != NULL) ? icon : "",
	                       (label != NULL) ? label : "");
}
