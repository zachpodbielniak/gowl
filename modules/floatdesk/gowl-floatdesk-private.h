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

/*
 * gowl-floatdesk-private.h - what the floating desktop's three parts
 * share: the module (mode, placement, commands), the title bars, and
 * the dock.
 */

#ifndef GOWL_FLOATDESK_PRIVATE_H
#define GOWL_FLOATDESK_PRIVATE_H

#include <glib-object.h>
#include <wayland-server-core.h>

#include "module/gowl-module.h"
#include "core/gowl-core-private.h"
#include "barkit/gowl-bar-theme.h"

#include "gowl-floatdesk-state.h"
#include "gowl-floatdesk-apps.h"

G_BEGIN_DECLS

/* Object-data keys on a GowlClient.  Transient bookkeeping only; the
 * ownership flag itself is the client's own mode_floated field. */

/* Set at client-pre-map, consumed by the first pre-arrange that sees
 * the window on a workspace: the window has not been placed yet. */
#define FLOATDESK_FRESH_KEY "gowl-floatdesk-fresh"
/* A FloatdeskRestore: maximized or snapped, and where it came from. */
#define FLOATDESK_RESTORE_KEY "gowl-floatdesk-restore"

/* Every command shares this prefix. */
#define FLOATDESK_PREFIX "floatdesk-"

/**
 * FloatdeskRestore:
 * @kind: how the window was put where it is (a #GowlFloatdeskSnap;
 *   MAXIMIZE for maximized)
 * @box: the frame to go back to
 *
 * Kept as object data on the client: it dies with the window.
 */
typedef struct {
	GowlFloatdeskSnap kind;
	struct wlr_box    box;
} FloatdeskRestore;

/**
 * FloatdeskMonitor:
 * @mask: the view the last pre-arrange saw
 * @layout: (nullable): the layout it saw on that view
 * @valid: whether @mask and @layout have been seen yet
 * @last_place: the last window opened here, for the cascade
 * @have_place: whether @last_place is set
 *
 * What the module remembers per output between arranges.  Comparing the
 * view and layout an arrange finds with what the previous one found is
 * how a layout change made by the cycle is told apart from a tag switch
 * landing on a workspace whose remembered layout disagrees with the
 * mode.
 */
typedef struct {
	guint32         mask;
	gchar          *layout;
	gboolean        valid;
	struct wlr_box  last_place;
	gboolean        have_place;
} FloatdeskMonitor;

typedef struct _FloatdeskDock FloatdeskDock;
typedef struct _FloatdeskTitlebar FloatdeskTitlebar;

#define GOWL_TYPE_MODULE_FLOATDESK (gowl_module_floatdesk_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleFloatdesk, gowl_module_floatdesk,
                     GOWL, MODULE_FLOATDESK, GowlModule)

struct _GowlModuleFloatdesk {
	GowlModule          parent_instance;

	GowlCompositor     *compositor;    /* weak */
	GowlFloatdeskState *state;
	gchar              *state_path;

	/* --- settings (the `modules: floatdesk:' section) --- */
	gboolean  config_default_set;      /* default-floating was given */
	gboolean  config_default_floating;
	gdouble   width_pct;               /* a new window's default size */
	gdouble   height_pct;
	gint      cascade_step;
	gboolean  focus_follows_click;
	gboolean  titlebars;
	gint      titlebar_height;
	gint      snap_threshold;
	gboolean  dock_enabled;
	gint      dock_icon_size;
	gchar    *config_dock_position;
	gboolean  config_dock_autohide;
	gchar   **config_pins;
	gchar    *terminal;

	/* --- run time --- */
	GHashTable *monitors;              /* GowlMonitor* -> FloatdeskMonitor* */
	GowlBarTheme *theme;

	/* title bars: GowlClient* -> FloatdeskTitlebar* */
	GHashTable *titlebar_nodes;
	GowlClient *last_click_client;     /* double-click detection */
	gint64      last_click_us;

	/* a drop at an output edge, applied once the grab has ended */
	struct wl_event_source *snap_idle;
	GowlClient             *snap_client;
	GowlFloatdeskSnap       snap_kind;
	GowlMonitor            *snap_monitor;

	/* the dock */
	GowlFloatdeskCatalog *catalog;
	GHashTable *docks;                 /* GowlMonitor* -> FloatdeskDock* */
	GHashTable *launching;             /* app key -> gint64 deadline (us) */
	GHashTable *icons;                 /* gowl_bar_icon cache */
	struct wl_event_source *dock_timer;

	/* compositor signal handlers */
	gulong pre_arrange_id;
	gulong arranged_id;
	gulong pre_map_id;
	gulong added_id;
	gulong removed_id;
	gulong focus_id;
	gulong title_id;
	gulong minimized_id;
	gulong monitor_removed_id;
};

/* --- gowl-module-floatdesk.c ------------------------------------- */

gboolean floatdesk_client_on_floating (GowlModuleFloatdesk *self,
                                       GowlClient          *c);
gboolean floatdesk_view_floating      (GowlModuleFloatdesk *self,
                                       GowlMonitor         *m);
void     floatdesk_toast              (GowlModuleFloatdesk *self,
                                       GowlMonitor         *m,
                                       const gchar         *text);
gboolean floatdesk_save               (GowlModuleFloatdesk *self);
gchar   *floatdesk_minimize           (GowlModuleFloatdesk *self,
                                       GowlClient          *c);
void     floatdesk_restore            (GowlModuleFloatdesk *self,
                                       GowlClient          *c);
void     floatdesk_maximize_toggle    (GowlModuleFloatdesk *self,
                                       GowlClient          *c);
void     floatdesk_snap               (GowlModuleFloatdesk *self,
                                       GowlClient          *c,
                                       GowlFloatdeskSnap    kind);
void     floatdesk_unsnap             (GowlModuleFloatdesk *self,
                                       GowlClient          *c);
gboolean floatdesk_is_maximized       (GowlClient          *c);
const gchar *floatdesk_dock_position  (GowlModuleFloatdesk *self);
gboolean floatdesk_dock_autohide      (GowlModuleFloatdesk *self);
gchar  **floatdesk_dock_pins          (GowlModuleFloatdesk *self);

/* --- gowl-floatdesk-titlebar.c ----------------------------------- */

gboolean floatdesk_titlebar_wanted    (GowlModuleFloatdesk *self,
                                       GowlClient          *c);
void     floatdesk_titlebar_sync      (GowlModuleFloatdesk *self,
                                       GowlClient          *c);
void     floatdesk_titlebar_drop      (GowlModuleFloatdesk *self,
                                       GowlClient          *c);
void     floatdesk_titlebar_drop_all  (GowlModuleFloatdesk *self);
gboolean floatdesk_titlebar_button    (GowlModuleFloatdesk *self,
                                       guint                button,
                                       gboolean             pressed);
void     floatdesk_titlebar_release   (GowlModuleFloatdesk *self,
                                       guint                button);

/* --- gowl-floatdesk-dock.c --------------------------------------- */

void     floatdesk_dock_refresh       (GowlModuleFloatdesk *self,
                                       GowlMonitor         *m);
void     floatdesk_dock_refresh_all   (GowlModuleFloatdesk *self);
void     floatdesk_dock_destroy_all   (GowlModuleFloatdesk *self);
void     floatdesk_dock_monitor_gone  (GowlModuleFloatdesk *self,
                                       GowlMonitor         *m);
gboolean floatdesk_dock_button        (GowlModuleFloatdesk *self,
                                       guint                button,
                                       gboolean             pressed);
gboolean floatdesk_dock_motion        (GowlModuleFloatdesk *self,
                                       gdouble              x,
                                       gdouble              y);
gboolean floatdesk_dock_axis          (GowlModuleFloatdesk *self,
                                       gdouble              delta);
gboolean floatdesk_dock_key           (GowlModuleFloatdesk *self,
                                       guint                keysym,
                                       gboolean             pressed);
void     floatdesk_dock_client_mapped (GowlModuleFloatdesk *self,
                                       GowlClient          *c);
gchar   *floatdesk_dock_launch        (GowlModuleFloatdesk *self,
                                       const gchar         *key);
gchar   *floatdesk_dock_activate      (GowlModuleFloatdesk *self,
                                       const gchar         *key);
gchar   *floatdesk_dock_status        (GowlModuleFloatdesk *self);
gboolean floatdesk_dock_pin           (GowlModuleFloatdesk *self,
                                       const gchar         *key,
                                       gint                 index);
gboolean floatdesk_dock_unpin         (GowlModuleFloatdesk *self,
                                       const gchar         *key);
void     floatdesk_dock_toggle_grid   (GowlModuleFloatdesk *self);

G_END_DECLS

#endif /* GOWL_FLOATDESK_PRIVATE_H */
