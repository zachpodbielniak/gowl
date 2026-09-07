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

#ifndef GOWL_BAR_HOST_H
#define GOWL_BAR_HOST_H

#include <glib-object.h>

#include "barkit/gowl-barkit-types.h"
#include "barkit/gowl-bar-theme.h"
#include "barkit/gowl-bar-toast.h"

G_BEGIN_DECLS

#define GOWL_TYPE_BAR_HOST (gowl_bar_host_get_type())

G_DECLARE_INTERFACE(GowlBarHost, gowl_bar_host, GOWL, BAR_HOST, GObject)

/**
 * GowlBarWorkFunc:
 * @plugin: the plugin the work belongs to
 * @user_data: the pointer handed to gowl_bar_host_queue_work()
 *
 * A unit of blocking work.  Runs on a worker thread, so it must touch
 * only its own data and the plugin's thread-safe setters --- notably
 * gowl_bar_plugin_set_label(), which is the whole point of the
 * mechanism.
 */
typedef void (*GowlBarWorkFunc) (GowlBarPlugin *plugin, gpointer user_data);

/**
 * GowlBarHostInterface:
 * @parent_iface: the parent interface
 * @get_theme: returns the active theme
 * @request_redraw: asks for a bar repaint on the next tick
 * @request_panel_refresh: asks for the plugin's open panel to be
 *   rebuilt, if it is the one showing
 * @open_panel: opens a plugin's panel by id
 * @close_panel: closes whatever panel is open
 * @notify: raises a toast
 * @queue_work: runs a blocking callback off the compositor thread
 * @spawn: launches a detached shell command
 * @get_state_dir: returns a writable per-user directory for plugin state
 *
 * The services a bar plugin may ask of whatever is hosting it.
 *
 * This exists as an interface rather than a concrete type so the same
 * plugins run unchanged in the compositor's in-process bar, in a test
 * harness with a stub host, and in any future out-of-process bar --- a
 * plugin never links against the bar module, only against this
 * contract.
 */
struct _GowlBarHostInterface {
	GTypeInterface parent_iface;

	const GowlBarTheme *(*get_theme)            (GowlBarHost *self);
	void                (*request_redraw)       (GowlBarHost *self);
	void                (*request_panel_refresh)(GowlBarHost *self,
	                                             GowlBarPlugin *plugin);
	void                (*open_panel)           (GowlBarHost *self,
	                                             const gchar *plugin_id);
	void                (*close_panel)          (GowlBarHost *self);
	void                (*notify)               (GowlBarHost *self,
	                                             GowlBarToast *toast);
	void                (*queue_work)           (GowlBarHost *self,
	                                             GowlBarPlugin *plugin,
	                                             GowlBarWorkFunc func,
	                                             gpointer user_data,
	                                             GDestroyNotify destroy);
	void                (*spawn)                (GowlBarHost *self,
	                                             const gchar *cmdline);
	const gchar        *(*get_state_dir)        (GowlBarHost *self);

	gpointer padding[8];
};

const GowlBarTheme *gowl_bar_host_get_theme (GowlBarHost *self);

/**
 * gowl_bar_host_request_redraw:
 * @self: a host
 *
 * Asks for a repaint.  Coalesced by the host: a plugin may call this
 * as often as it likes.
 */
void gowl_bar_host_request_redraw (GowlBarHost *self);

void gowl_bar_host_request_panel_refresh (GowlBarHost   *self,
                                           GowlBarPlugin *plugin);
void gowl_bar_host_open_panel  (GowlBarHost *self, const gchar *plugin_id);
void gowl_bar_host_close_panel (GowlBarHost *self);

/**
 * gowl_bar_host_notify:
 * @self: a host
 * @toast: (transfer full): the toast to show
 */
void gowl_bar_host_notify (GowlBarHost *self, GowlBarToast *toast);

/**
 * gowl_bar_host_queue_work:
 * @self: a host
 * @plugin: the plugin the work belongs to
 * @func: the blocking callback
 * @user_data: passed to @func
 * @destroy: (nullable): frees @user_data once @func has run
 *
 * Runs @func on a worker thread.  Anything that spawns a subprocess or
 * touches the network must go through here: the compositor's dispatch
 * thread holds the lock every main-thread editor primitive needs, so
 * blocking it deadlocks the editor, not just the bar.
 */
void gowl_bar_host_queue_work (GowlBarHost     *self,
                                GowlBarPlugin   *plugin,
                                GowlBarWorkFunc  func,
                                gpointer         user_data,
                                GDestroyNotify   destroy);

void         gowl_bar_host_spawn         (GowlBarHost *self,
                                           const gchar *cmdline);
const gchar *gowl_bar_host_get_state_dir (GowlBarHost *self);

G_END_DECLS

#endif /* GOWL_BAR_HOST_H */
