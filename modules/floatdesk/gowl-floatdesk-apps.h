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
 * gowl-floatdesk-apps.h - which application a window belongs to.
 *
 * The dock groups windows by application and draws each application's
 * icon, so it has to get from a window's app_id (Wayland) or WM_CLASS
 * (X11) to a desktop entry.  Those rarely match exactly: Nautilus is
 * `org.gnome.Nautilus' on Wayland, Steam is `steam' with an entry
 * called `steam.desktop' but a StartupWMClass of `Steam', a flatpak is
 * its reverse-DNS id.  The catalog resolves all of those, and gives a
 * terminal program its own identity: a TUI the dock launches runs in a
 * terminal whose app_id is `tui.<entry id>', so btop is btop in the
 * dock, with btop's icon, rather than one more terminal window.
 *
 * No wlroots and no compositor here; the catalog is filled from the
 * system's desktop entries in the module and from literals in the
 * tests.
 */

#ifndef GOWL_FLOATDESK_APPS_H
#define GOWL_FLOATDESK_APPS_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GOWL_FLOATDESK_TUI_PREFIX:
 *
 * The app_id prefix that marks a terminal running one program.  Matched
 * case-insensitively, so Omarchy's `TUI.btop' resolves too.
 */
#define GOWL_FLOATDESK_TUI_PREFIX "tui."

/**
 * GowlFloatdeskApp:
 * @id: the desktop entry id without `.desktop' -- the app's key
 * @name: the display name
 * @icon: (nullable): a themed icon name or an absolute path
 * @exec: (nullable): the command line, field codes removed
 * @wm_class: (nullable): StartupWMClass
 * @terminal: Terminal=true: a program run inside a terminal
 * @hidden: NoDisplay or not for this desktop: resolvable, never listed
 *
 * One application.
 */
typedef struct {
	gchar    *id;
	gchar    *name;
	gchar    *icon;
	gchar    *exec;
	gchar    *wm_class;
	gboolean  terminal;
	gboolean  hidden;
} GowlFloatdeskApp;

/**
 * GowlFloatdeskCatalog:
 *
 * Opaque.  Every known application, by id.
 */
typedef struct _GowlFloatdeskCatalog GowlFloatdeskCatalog;

/**
 * gowl_floatdesk_catalog_new:
 *
 * Returns: (transfer full): an empty catalog
 */
GowlFloatdeskCatalog *gowl_floatdesk_catalog_new (void);

/**
 * gowl_floatdesk_catalog_free:
 * @self: (nullable) (transfer full): a catalog
 */
void gowl_floatdesk_catalog_free (GowlFloatdeskCatalog *self);

/**
 * gowl_floatdesk_catalog_add:
 * @self: a catalog
 * @id: the entry id, with or without `.desktop'
 * @name: (nullable): display name; @id when %NULL
 * @icon: (nullable): icon name or path
 * @exec: (nullable): command line; field codes (`%U' and friends) are
 *   removed here
 * @wm_class: (nullable): StartupWMClass
 * @terminal: Terminal=true
 * @hidden: not to be listed
 *
 * Adds an application, replacing one with the same id.
 */
void gowl_floatdesk_catalog_add (GowlFloatdeskCatalog *self,
                                 const gchar          *id,
                                 const gchar          *name,
                                 const gchar          *icon,
                                 const gchar          *exec,
                                 const gchar          *wm_class,
                                 gboolean              terminal,
                                 gboolean              hidden);

/**
 * gowl_floatdesk_catalog_load_system:
 * @self: a catalog
 *
 * Adds every installed desktop entry (g_app_info_get_all(), so flatpak
 * exports and ~/.local/share/applications included).
 *
 * Returns: how many entries were added
 */
guint gowl_floatdesk_catalog_load_system (GowlFloatdeskCatalog *self);

/**
 * gowl_floatdesk_catalog_lookup:
 * @self: a catalog
 * @id: an entry id, with or without `.desktop'
 *
 * Returns: (transfer none) (nullable): the application
 */
const GowlFloatdeskApp *gowl_floatdesk_catalog_lookup (const GowlFloatdeskCatalog *self,
                                                       const gchar                *id);

/**
 * gowl_floatdesk_catalog_resolve:
 * @self: a catalog
 * @app_id: (nullable): a window's app_id or X11 class
 *
 * In order: a `tui.' prefix names the entry after it; the exact id;
 * the id ignoring case; StartupWMClass ignoring case; then the last
 * dotted part of either side (`org.gnome.Nautilus' and `nautilus').
 *
 * Returns: (transfer none) (nullable): the application
 */
const GowlFloatdeskApp *gowl_floatdesk_catalog_resolve (const GowlFloatdeskCatalog *self,
                                                        const gchar                *app_id);

/**
 * gowl_floatdesk_catalog_key:
 * @self: a catalog
 * @app_id: (nullable): a window's app_id
 *
 * The dock's grouping key for a window: the entry id when one resolves,
 * else the app_id itself lower-cased (a window with no entry is still
 * its own application), else "unknown".
 *
 * Returns: (transfer full): the key
 */
gchar *gowl_floatdesk_catalog_key (const GowlFloatdeskCatalog *self,
                                   const gchar                *app_id);

/**
 * gowl_floatdesk_catalog_list:
 * @self: a catalog
 *
 * Returns: (transfer container) (element-type GowlFloatdeskApp): every
 *   listed application by name -- the "all apps" grid
 */
GPtrArray *gowl_floatdesk_catalog_list (const GowlFloatdeskCatalog *self);

/**
 * gowl_floatdesk_strip_field_codes:
 * @exec: (nullable): a desktop entry Exec line
 *
 * Returns: (transfer full) (nullable): @exec with `%f %F %u %U %i %c %k'
 *   and friends removed, and `%%' turned into `%'
 */
gchar *gowl_floatdesk_strip_field_codes (const gchar *exec);

/**
 * gowl_floatdesk_terminal_argv:
 * @terminal: the terminal program, e.g. "foot"
 * @app_id: the app_id/class the window should get
 * @command: the command line to run in it
 * @error: return location for a #GError (an unparseable @command)
 *
 * How to start @command in @terminal so that the window comes up with
 * @app_id: `--app-id=' for foot, `--class' for kitty, alacritty,
 * ghostty and wezterm, `-c' for st and gst.  A terminal this does not
 * know gets `-e' and no class -- the program still runs, it just
 * groups with the terminal.
 *
 * Returns: (transfer full) (nullable): the argv
 */
gchar **gowl_floatdesk_terminal_argv (const gchar *terminal,
                                      const gchar *app_id,
                                      const gchar *command,
                                      GError     **error);

/**
 * gowl_floatdesk_cycle_next:
 * @count: how many windows the application has, in cycle order
 * @focused: the index of the focused one among them, -1 if none is
 *
 * The window a click on the application's dock icon goes to: the first
 * when none of them has focus, else the one after the focused one,
 * wrapping.  The caller orders the windows -- current workspace, then
 * minimized, then the rest -- so repeated clicks walk that order.
 *
 * Returns: the index, -1 when @count is 0
 */
gint gowl_floatdesk_cycle_next (guint count,
                                gint  focused);

G_END_DECLS

#endif /* GOWL_FLOATDESK_APPS_H */
