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

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-vanitygaps"

#include <glib-object.h>
#include <gmodule.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-gap-provider.h"
#include "interfaces/gowl-startup-handler.h"

/**
 * GOWL_VANITYGAPS_DEFAULT_GAP:
 *
 * Default gap size in pixels when no configuration is provided.
 */
#define GOWL_VANITYGAPS_DEFAULT_GAP (5)

/**
 * GowlModuleVanitygaps:
 *
 * Vanity gaps module.  Provides configurable inner and outer gaps
 * for tiling layouts.  Gap values are returned through the
 * GowlGapProvider interface when the compositor queries gap sizes
 * during layout arrangement.
 *
 * Four independent gap values are supported:
 *   - inner_h: horizontal gap between tiled windows
 *   - inner_v: vertical gap between tiled windows
 *   - outer_h: horizontal gap at screen edges
 *   - outer_v: vertical gap at screen edges
 */

#define GOWL_TYPE_MODULE_VANITYGAPS (gowl_module_vanitygaps_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleVanitygaps, gowl_module_vanitygaps,
                     GOWL, MODULE_VANITYGAPS, GowlModule)

struct _GowlModuleVanitygaps {
	GowlModule parent_instance;
	gpointer   compositor;
	gint       inner_h;   /* inner horizontal gap (pixels) */
	gint       inner_v;   /* inner vertical gap (pixels) */
	gint       outer_h;   /* outer horizontal gap (pixels) */
	gint       outer_v;   /* outer vertical gap (pixels) */
};

static void vanitygaps_gap_init(GowlGapProviderInterface *iface);
static void vanitygaps_startup_init(GowlStartupHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleVanitygaps, gowl_module_vanitygaps,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_GAP_PROVIDER,
		vanitygaps_gap_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		vanitygaps_startup_init))

/* --- GowlModule virtual methods --- */

static gboolean
vanitygaps_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static const gchar *
vanitygaps_get_name(GowlModule *mod)
{
	(void)mod;
	return "vanitygaps";
}

static const gchar *
vanitygaps_get_description(GowlModule *mod)
{
	(void)mod;
	return "Configurable inner and outer gaps for tiling layouts";
}

static const gchar *
vanitygaps_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/**
 * vanitygaps_get_int_setting:
 * @settings: a #GHashTable of string key-value pairs
 * @key: the setting key to look up
 * @fallback: value to return if the key is missing
 *
 * Helper to read an integer from the module config hash table.
 *
 * Returns: the parsed integer, or @fallback on missing/invalid key
 */
static gint
vanitygaps_get_int_setting(
	GHashTable  *settings,
	const gchar *key,
	gint         fallback
){
	const gchar *val;

	val = (const gchar *)g_hash_table_lookup(settings, key);
	if (val == NULL)
		return fallback;

	return (gint)g_ascii_strtoll(val, NULL, 10);
}

/**
 * vanitygaps_configure:
 * @mod: the module
 * @config: a #GHashTable of string key-value settings from the YAML
 *          config.  Recognised keys: "inner-h", "inner-v", "outer-h",
 *          "outer-v" (integers in pixels).  Also accepts the shorthand
 *          keys "inner-gap" (sets both inner-h and inner-v) and
 *          "outer-gap" (sets both outer-h and outer-v).
 *
 * Applies configuration from the YAML modules section.
 */
static void
vanitygaps_configure(GowlModule *mod, gpointer config)
{
	GowlModuleVanitygaps *self;
	GHashTable *settings;

	self = GOWL_MODULE_VANITYGAPS(mod);

	if (config == NULL)
		return;

	settings = (GHashTable *)config;

	/* Support shorthand: "inner-gap" sets both h and v,
	 * "outer-gap" sets both h and v.  Specific keys override. */
	if (g_hash_table_contains(settings, "inner-gap")) {
		gint ig = vanitygaps_get_int_setting(settings, "inner-gap",
		                                     GOWL_VANITYGAPS_DEFAULT_GAP);
		self->inner_h = ig;
		self->inner_v = ig;
	}
	if (g_hash_table_contains(settings, "outer-gap")) {
		gint og = vanitygaps_get_int_setting(settings, "outer-gap",
		                                     GOWL_VANITYGAPS_DEFAULT_GAP);
		self->outer_h = og;
		self->outer_v = og;
	}

	/* Specific per-axis keys override the shorthand */
	if (g_hash_table_contains(settings, "inner-h"))
		self->inner_h = vanitygaps_get_int_setting(
			settings, "inner-h", self->inner_h);
	if (g_hash_table_contains(settings, "inner-v"))
		self->inner_v = vanitygaps_get_int_setting(
			settings, "inner-v", self->inner_v);
	if (g_hash_table_contains(settings, "outer-h"))
		self->outer_h = vanitygaps_get_int_setting(
			settings, "outer-h", self->outer_h);
	if (g_hash_table_contains(settings, "outer-v"))
		self->outer_v = vanitygaps_get_int_setting(
			settings, "outer-v", self->outer_v);

	g_message("vanitygaps: configured gaps ih=%d iv=%d oh=%d ov=%d",
	          self->inner_h, self->inner_v,
	          self->outer_h, self->outer_v);
}

/* --- GowlStartupHandler --- */

static void
vanitygaps_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleVanitygaps *self;

	self = GOWL_MODULE_VANITYGAPS(handler);
	self->compositor = compositor;

	g_debug("vanitygaps: startup, gaps ih=%d iv=%d oh=%d ov=%d",
	        self->inner_h, self->inner_v,
	        self->outer_h, self->outer_v);
}

static void
vanitygaps_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = vanitygaps_on_startup;
}

/* --- GowlGapProvider --- */

/**
 * vanitygaps_get_gaps:
 * @self: the gap provider
 * @monitor: the monitor being laid out (currently unused)
 * @inner_h: (out): horizontal gap between windows
 * @inner_v: (out): vertical gap between windows
 * @outer_h: (out): horizontal gap at screen edges
 * @outer_v: (out): vertical gap at screen edges
 *
 * Returns the configured gap values for the given monitor.
 * Currently all monitors share the same gap values.
 */
static void
vanitygaps_get_gaps(
	GowlGapProvider *provider,
	gpointer         monitor,
	gint            *inner_h,
	gint            *inner_v,
	gint            *outer_h,
	gint            *outer_v
){
	GowlModuleVanitygaps *self;

	(void)monitor;

	self = GOWL_MODULE_VANITYGAPS(provider);

	if (inner_h != NULL)
		*inner_h = self->inner_h;
	if (inner_v != NULL)
		*inner_v = self->inner_v;
	if (outer_h != NULL)
		*outer_h = self->outer_h;
	if (outer_v != NULL)
		*outer_v = self->outer_v;
}

static void
vanitygaps_gap_init(GowlGapProviderInterface *iface)
{
	iface->get_gaps = vanitygaps_get_gaps;
}

/* --- GObject lifecycle --- */

static void
gowl_module_vanitygaps_class_init(GowlModuleVanitygapsClass *klass)
{
	GowlModuleClass *mod_class;

	mod_class = GOWL_MODULE_CLASS(klass);

	mod_class->activate        = vanitygaps_activate;
	mod_class->get_name        = vanitygaps_get_name;
	mod_class->get_description = vanitygaps_get_description;
	mod_class->get_version     = vanitygaps_get_version;
	mod_class->configure       = vanitygaps_configure;
}

static void
gowl_module_vanitygaps_init(GowlModuleVanitygaps *self)
{
	self->compositor = NULL;
	self->inner_h    = GOWL_VANITYGAPS_DEFAULT_GAP;
	self->inner_v    = GOWL_VANITYGAPS_DEFAULT_GAP;
	self->outer_h    = GOWL_VANITYGAPS_DEFAULT_GAP;
	self->outer_v    = GOWL_VANITYGAPS_DEFAULT_GAP;
}

/* --- Shared-object entry point --- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_VANITYGAPS;
}
