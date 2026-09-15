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

#ifndef GOWL_TRAY_H
#define GOWL_TRAY_H

#include <glib-object.h>
#include <cairo.h>

#include <wayland-server-core.h>

G_BEGIN_DECLS

/*
 * The system tray, which is three D-Bus interfaces and no pixels.
 *
 * Solaar, Zoom, Deskflow, Syncthing, Steam, the Proton Mail bridge and a
 * long tail of others put their only always-available interface in a
 * tray icon.  There has been nowhere to put one under gowl, so those
 * applications have been running with their interface simply absent.
 *
 * It is not an X11 thing any more.  A WATCHER owns a well-known name and
 * keeps the register; a HOST says somebody is willing to display items;
 * each ITEM is an object on the application's own bus name carrying a
 * title, a status, an icon and a menu.  gowl is all three.
 *
 * WHAT IS AND IS NOT IN HERE.  This file is the protocol and the state:
 * who is registered, what each one currently says about itself, and the
 * methods that act on one.  It does not know what an icon looks like --
 * an item hands over either a themed NAME or raw ARGB pixmaps, and
 * turning either into something on a screen needs an icon theme and a
 * renderer, which is the bar widget's business (modules/bar).  Keeping
 * the split here is what lets the same register serve a bar widget, a
 * buffer in cmacs and anything else later.
 *
 * THREADING.  Everything D-Bus happens on a thread of its own with its
 * own #GMainContext, exactly as modules/notifyd does it, because a tray
 * application that stops answering must not be able to stall the
 * compositor.  What the compositor thread sees is a snapshot taken under
 * a lock, and it is told to look again through the Wayland event loop.
 * Every function here says which thread it belongs to.
 */

/**
 * GowlTrayItem:
 * @key: the identity this item is addressed by: its bus name and object
 *   path joined.  Stable for as long as the item is registered
 * @service: the application's bus name
 * @path: the object path of the item on it
 * @id: the item's own `Id' --- an application name, not unique
 * @title: `Title', the human-readable name
 * @status: `Passive', `Active' or `NeedsAttention'
 * @category: `ApplicationStatus', `Communications', `SystemServices' or
 *   `Hardware'
 * @icon_name: the themed icon name, or %NULL
 * @attention_icon_name: the themed name to use while @status is
 *   `NeedsAttention', or %NULL
 * @icon_theme_path: a directory to search before the icon theme, which
 *   is how an application ships an icon the theme has never heard of
 * @tooltip: the tooltip's title, flattened out of the `ToolTip' struct
 * @menu_path: the object path of the item's `com.canonical.dbusmenu',
 *   or %NULL when it has none
 * @item_is_menu: the application says a left click should show the menu
 *   rather than activate it.  Items that set this often have no
 *   `Activate' at all
 * @pixmap: (nullable): raw `IconPixmap' data for the chosen size, ARGB
 *   with the bytes in NETWORK order, exactly as it came off the bus
 * @pixmap_width: its width
 * @pixmap_height: its height
 * @serial: bumped whenever anything above changes.  A renderer that
 *   caches by this can skip everything else
 *
 * One registered item, as the compositor thread sees it.
 */
typedef struct {
	gchar    *key;
	gchar    *service;
	gchar    *path;
	gchar    *id;
	gchar    *title;
	gchar    *status;
	gchar    *category;
	gchar    *icon_name;
	gchar    *attention_icon_name;
	gchar    *icon_theme_path;
	gchar    *tooltip;
	gchar    *menu_path;
	gboolean  item_is_menu;
	guint8   *pixmap;
	gint      pixmap_width;
	gint      pixmap_height;
	guint     serial;
} GowlTrayItem;

#define GOWL_TYPE_TRAY_ITEM (gowl_tray_item_get_type())
GType         gowl_tray_item_get_type (void) G_GNUC_CONST;
GowlTrayItem *gowl_tray_item_copy (const GowlTrayItem *self);
void          gowl_tray_item_free (GowlTrayItem *self);

/**
 * GowlTrayMenuItem:
 * @id: the dbusmenu id, which is what an activation reports back
 * @label: the text, with the underscore mnemonics already removed
 * @icon_name: a themed icon name for the row, or %NULL
 * @is_separator: draw a rule rather than a row
 * @enabled: %FALSE greys it out and makes it inert
 * @visible: %FALSE drops it entirely
 * @toggle_type: `checkmark', `radio' or %NULL
 * @toggle_state: 1 on, 0 off, -1 indeterminate
 * @children: (element-type GowlTrayMenuItem) (nullable): a submenu
 *
 * One row of an item's menu.  A tree, because dbusmenu menus nest ---
 * the Proton bridge's is three deep.
 */
typedef struct _GowlTrayMenuItem GowlTrayMenuItem;

struct _GowlTrayMenuItem {
	gint       id;
	gchar     *label;
	gchar     *icon_name;
	gboolean   is_separator;
	gboolean   enabled;
	gboolean   visible;
	gchar     *toggle_type;
	gint       toggle_state;
	GPtrArray *children;
};

#define GOWL_TYPE_TRAY_MENU_ITEM (gowl_tray_menu_item_get_type())
GType             gowl_tray_menu_item_get_type (void) G_GNUC_CONST;
GowlTrayMenuItem *gowl_tray_menu_item_copy (const GowlTrayMenuItem *self);
void              gowl_tray_menu_item_free (GowlTrayMenuItem *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlTrayItem, gowl_tray_item_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlTrayMenuItem, gowl_tray_menu_item_free)

/**
 * gowl_tray_item_argb32:
 * @item: an item carrying a pixmap
 *
 * The item's raw `IconPixmap' as a cairo image surface.
 *
 * TWO CONVERSIONS, and getting either wrong produces a picture rather
 * than an error.  The wire format is ARGB32 in NETWORK byte order --- A,
 * R, G, B in memory, in that order, on every machine --- while cairo's
 * ARGB32 is a native-endian 32-bit word, which on a little-endian
 * machine is B, G, R, A in memory.  And the wire format is straight
 * alpha while cairo's is PREMULTIPLIED.  Byte order alone turns every
 * icon into a blue-tinted negative; premultiplication alone puts a halo
 * around everything with a soft edge.
 *
 * Here rather than in the bar widget so that it can be tested without a
 * screen: it is pure arithmetic over bytes, and it is the one part of
 * drawing a tray icon that is wrong in a way nobody can see is wrong.
 *
 * Returns: (transfer full) (nullable): the surface, or %NULL when the
 *   item has no pixmap
 */
cairo_surface_t *gowl_tray_item_argb32 (const GowlTrayItem *item);

#define GOWL_TYPE_TRAY (gowl_tray_get_type())
G_DECLARE_FINAL_TYPE(GowlTray, gowl_tray, GOWL, TRAY, GObject)

/**
 * gowl_tray_new:
 *
 * Returns: (transfer full): a tray that is not serving yet
 */
GowlTray *gowl_tray_new (void);

/**
 * gowl_tray_get_default:
 *
 * The one tray in this process.
 *
 * A singleton because the protocol is: one process can own
 * `org.kde.StatusNotifierWatcher' once, so a second #GowlTray could only
 * ever stand down.  It is also what lets the register be built by the
 * compositor and read by a bar plugin, which are linked together but
 * deliberately do not share headers.
 *
 * Created on first use and never freed; starting it is a separate step.
 *
 * Returns: (transfer none): the tray
 */
GowlTray *gowl_tray_get_default (void);

/**
 * gowl_tray_start:
 * @self: a tray
 * @loop: the compositor's Wayland event loop, which is how the bus
 *   thread gets the compositor thread's attention
 * @error: (nullable): return location for a #GError
 *
 * Starts the bus thread and tries to become the watcher.
 *
 * FAILING TO GET THE NAME IS NOT AN ERROR.  Another tray may already own
 * it --- a GNOME shell extension, a second compositor, a cmacs that was
 * started first --- and the right behaviour then is to sit there owning
 * nothing rather than to fight over it: two watchers on one bus is how
 * applications end up registered with the one nobody is displaying.
 * gowl_tray_is_serving() is how a caller finds out which happened.
 *
 * Compositor thread.
 *
 * Returns: %FALSE only when the thread or the bus could not be had.
 */
gboolean gowl_tray_start (GowlTray              *self,
                          struct wl_event_loop  *loop,
                          GError               **error);

/**
 * gowl_tray_stop:
 * @self: a tray
 *
 * Gives up the names, drops every item and stops the bus thread.  Safe
 * to call more than once and safe on a tray that never started.
 *
 * Compositor thread.
 */
void gowl_tray_stop (GowlTray *self);

/**
 * gowl_tray_is_serving:
 * @self: a tray
 *
 * Returns: %TRUE while this process owns the watcher name.
 */
gboolean gowl_tray_is_serving (GowlTray *self);

/**
 * gowl_tray_dup_items:
 * @self: a tray
 *
 * The register, newest registration last.
 *
 * A COPY, taken under the lock: the bus thread rewrites these whenever
 * an application changes its mind, so a renderer holding a borrowed
 * pointer across a redraw would be reading freed memory on the tick a
 * tray application quit.
 *
 * Compositor thread.
 *
 * Returns: (transfer full) (element-type GowlTrayItem): the items
 */
GPtrArray *gowl_tray_dup_items (GowlTray *self);

/**
 * gowl_tray_get_serial:
 * @self: a tray
 *
 * Bumped whenever anything about the register changes: an item arriving
 * or leaving, or any one of them changing its icon, title or status.
 *
 * This is what a bar widget puts in its signature.  The bar skips a
 * repaint when the signature has not moved, and a widget that draws
 * something other than its own label has to say so or it will not
 * redraw at all.
 *
 * Compositor thread.
 */
guint gowl_tray_get_serial (GowlTray *self);

/* --- Acting on an item -------------------------------------------- */

/*
 * @key is a #GowlTrayItem.key.  @x and @y are where on the screen the
 * click happened, which applications use to place their own menus.
 *
 * All of these return immediately: the call is handed to the bus thread
 * and made there, because an application that has stopped answering must
 * not be able to stall a click.  Nothing reports failure for the same
 * reason --- there is nothing a caller could usefully do about it.
 *
 * Compositor thread.
 */
void gowl_tray_activate           (GowlTray *self, const gchar *key,
                                   gint x, gint y);
void gowl_tray_secondary_activate (GowlTray *self, const gchar *key,
                                   gint x, gint y);
void gowl_tray_context_menu       (GowlTray *self, const gchar *key,
                                   gint x, gint y);
void gowl_tray_scroll             (GowlTray *self, const gchar *key,
                                   gint delta, const gchar *orientation);

/* --- The menu ------------------------------------------------------ */

/**
 * gowl_tray_dup_menu:
 * @self: a tray
 * @key: a #GowlTrayItem.key
 *
 * The item's menu as it was last read, or %NULL when it has none or it
 * has not been read yet.  gowl_tray_menu_refresh() asks for it.
 *
 * Compositor thread.
 *
 * Returns: (transfer full) (nullable): the root, whose children are the
 *   top-level rows
 */
GowlTrayMenuItem *gowl_tray_dup_menu (GowlTray *self, const gchar *key);

/**
 * gowl_tray_menu_refresh:
 * @self: a tray
 * @key: a #GowlTrayItem.key
 *
 * Asks the application for its menu, and for permission to show it:
 * dbusmenu's contract is that `AboutToShow' comes first and is what
 * gives an application the chance to build a menu it generates on
 * demand.  The answer arrives later and bumps the serial.
 *
 * Compositor thread.
 */
void gowl_tray_menu_refresh (GowlTray *self, const gchar *key);

/**
 * gowl_tray_menu_clicked:
 * @self: a tray
 * @key: a #GowlTrayItem.key
 * @id: a #GowlTrayMenuItem.id
 *
 * Tells the application one of its menu rows was chosen.
 *
 * Compositor thread.
 */
void gowl_tray_menu_clicked (GowlTray *self, const gchar *key, gint id);

G_END_DECLS

#endif /* GOWL_TRAY_H */
