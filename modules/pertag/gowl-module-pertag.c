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
#define G_LOG_DOMAIN "gowl-pertag"

#include <glib-object.h>
#include <gmodule.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-tag-manager.h"

/**
 * GOWL_PERTAG_NTAGS:
 *
 * Maximum number of tags supported.  Matches the 9-tag
 * convention from dwm/dwl (tags 1-9).
 */
#define GOWL_PERTAG_NTAGS (9)

/**
 * PertagState:
 *
 * Per-tag saved state.  Each tag independently tracks its
 * layout selection, master factor, and master count.
 */
typedef struct {
	gint    sellt;    /* selected layout index (0=tile, 1=monocle, ...) */
	gdouble mfact;    /* master area factor (0.0 - 1.0) */
	gint    nmaster;  /* number of master windows */
} PertagState;

/**
 * GowlModulePertag:
 *
 * Per-tag settings module.  Stores independent layout, mfact,
 * and nmaster per tag so that switching tags restores the
 * last-used layout configuration for each.
 */

#define GOWL_TYPE_MODULE_PERTAG (gowl_module_pertag_get_type())
G_DECLARE_FINAL_TYPE(GowlModulePertag, gowl_module_pertag,
                     GOWL, MODULE_PERTAG, GowlModule)

struct _GowlModulePertag {
	GowlModule  parent_instance;
	PertagState tags[GOWL_PERTAG_NTAGS];
	gint        current_tag;
	gpointer    compositor;
};

static void pertag_startup_init(GowlStartupHandlerInterface *iface);
static void pertag_tag_manager_init(GowlTagManagerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModulePertag, gowl_module_pertag,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		pertag_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_TAG_MANAGER,
		pertag_tag_manager_init))

/* --- GowlModule virtual methods --- */

static gboolean
pertag_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static const gchar *
pertag_get_name(GowlModule *mod)
{
	(void)mod;
	return "pertag";
}

static const gchar *
pertag_get_description(GowlModule *mod)
{
	(void)mod;
	return "Per-tag layout, mfact, and nmaster settings";
}

static const gchar *
pertag_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/* --- GowlStartupHandler --- */

static void
pertag_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModulePertag *self;

	self = GOWL_MODULE_PERTAG(handler);
	self->compositor = compositor;

	g_debug("pertag: startup, initialized %d tags", GOWL_PERTAG_NTAGS);
}

static void
pertag_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = pertag_on_startup;
}

/* --- GowlTagManager --- */

/**
 * pertag_get_tag_name:
 *
 * Returns the display name for a tag.  Uses 1-indexed numbering.
 */
static gchar *
pertag_get_tag_name(GowlTagManager *self, gint tag_index)
{
	(void)self;

	if (tag_index < 0 || tag_index >= GOWL_PERTAG_NTAGS)
		return g_strdup("?");

	return g_strdup_printf("%d", tag_index + 1);
}

/**
 * pertag_should_hide_vacant:
 *
 * Returns whether vacant (empty) tags should be hidden from
 * the bar display.  Defaults to FALSE (show all tags).
 */
static gboolean
pertag_should_hide_vacant(GowlTagManager *self, gint tag_index)
{
	(void)self;
	(void)tag_index;

	return FALSE;
}

static void
pertag_tag_manager_init(GowlTagManagerInterface *iface)
{
	iface->get_tag_name       = pertag_get_tag_name;
	iface->should_hide_vacant = pertag_should_hide_vacant;
}

/* --- GObject lifecycle --- */

static void
gowl_module_pertag_class_init(GowlModulePertagClass *klass)
{
	GowlModuleClass *mod_class;

	mod_class = GOWL_MODULE_CLASS(klass);

	mod_class->activate        = pertag_activate;
	mod_class->get_name        = pertag_get_name;
	mod_class->get_description = pertag_get_description;
	mod_class->get_version     = pertag_get_version;
}

static void
gowl_module_pertag_init(GowlModulePertag *self)
{
	gint i;

	/* Initialize all tags with default values */
	for (i = 0; i < GOWL_PERTAG_NTAGS; i++) {
		self->tags[i].sellt   = 0;      /* default layout (tile) */
		self->tags[i].mfact   = 0.55;   /* default master factor */
		self->tags[i].nmaster = 1;      /* default 1 master */
	}
	self->current_tag = 0;
	self->compositor = NULL;
}

/* --- Shared-object entry point --- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_PERTAG;
}
