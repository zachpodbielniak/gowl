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

#ifndef PORTAL_RESTORE_H
#define PORTAL_RESTORE_H

#include <glib.h>

#include "portal-capture.h"

G_BEGIN_DECLS

/*
 * PortalRestore -- the screen-cast grants a restore token stands for.
 *
 * ScreenCast v4 lets an application skip the chooser next time.  The
 * application holds a restore TOKEN, which the xdg-desktop-portal
 * frontend mints and maps to the backend's opaque `restore_data'
 * (vendor, version, variant).  Ours is ("gowl", 1, <token>): a token
 * of our own naming one entry here, and the entry says WHAT was
 * shared -- an output by connector, make and model, a window by
 * application and title -- so the next session can find the same
 * thing again, or find that it is gone and ask.
 *
 * Why a store and not the description itself in restore_data: the
 * grants are then visible and revocable from this side
 * ($XDG_STATE_HOME/gowl/screencast-grants.yaml; delete an entry and
 * the application is asked again), and a grant is bound to the
 * application it was given to, so a token cannot be replayed by
 * another app.
 *
 * persist_mode 2 (until revoked) is written to that file, mode 0600.
 * persist_mode 1 (while the application runs) lives in memory only and
 * ends with this process, which runs exactly as long as the session.
 * Grants are single use, as the spec says restore tokens are: a
 * restored session consumes its grant and is issued a fresh one.
 */
typedef struct _PortalRestore PortalRestore;

/**
 * PortalGrant:
 * @token: the grant's name, carried in restore_data
 * @app_id: the application it was given to ("" for an unsandboxed one)
 * @persist_mode: 1 or 2
 * @is_window: whether a window was shared, else an output
 * @output_name: outputs: the connector
 * @make: outputs: the monitor's make
 * @model: outputs: its model
 * @window_app_id: windows: the shared window's application
 * @window_title: windows: its title when shared
 * @stream_id: the stream `id' the application was told, kept so a
 *   restored session reports the same one
 * @created: when it was granted, Unix seconds
 * @last_used: when it was last restored, Unix seconds
 */
typedef struct {
	gchar    *token;
	gchar    *app_id;
	guint32   persist_mode;
	gboolean  is_window;
	gchar    *output_name;
	gchar    *make;
	gchar    *model;
	gchar    *window_app_id;
	gchar    *window_title;
	gchar    *stream_id;
	gint64    created;
	gint64    last_used;
} PortalGrant;

/**
 * portal_restore_new:
 * @path: (nullable): the store, or %NULL for
 *   $XDG_STATE_HOME/gowl/screencast-grants.yaml
 *
 * Loads the persistent grants.  A missing or unreadable file is an
 * empty store, never an error: the worst it costs is a chooser.
 *
 * Returns: (transfer full): the store
 */
PortalRestore *portal_restore_new (const gchar *path);

void portal_restore_free (PortalRestore *self);

/**
 * portal_restore_lookup:
 * @self: a #PortalRestore
 * @token: a token out of restore_data
 * @app_id: the application presenting it
 *
 * Returns: (transfer none) (nullable): the grant, if @token names one
 *          given to @app_id
 */
const PortalGrant *portal_restore_lookup (PortalRestore *self,
                                          const gchar   *token,
                                          const gchar   *app_id);

/**
 * portal_restore_match:
 * @grant: a grant
 * @sources: (element-type PortalCaptureSource): what can be cast now
 *
 * The source @grant names, if it still exists.  An output matches by
 * connector (and make and model when recorded), else by make and model
 * alone when exactly one output has them -- a dock renumbers
 * connectors.  A window matches by application and title, else by
 * application alone when exactly one window has it -- a browser's
 * title follows its tab.  Anything ambiguous is no match: guessing
 * wrong shares the wrong thing.
 *
 * Returns: (transfer none) (nullable): the source, out of @sources
 */
const PortalCaptureSource *portal_restore_match (const PortalGrant *grant,
                                                 GPtrArray         *sources);

/**
 * portal_restore_add:
 * @self: a #PortalRestore
 * @app_id: the application
 * @persist_mode: 1 (in memory) or 2 (on disk)
 * @source: what it was given
 * @stream_id: (nullable): the stream id to keep, or %NULL for a new one
 *
 * Records a grant and saves the store when it is persistent.
 *
 * Returns: (transfer none): the new grant
 */
const PortalGrant *portal_restore_add (PortalRestore             *self,
                                       const gchar               *app_id,
                                       guint32                    persist_mode,
                                       const PortalCaptureSource *source,
                                       const gchar               *stream_id);

/**
 * portal_restore_remove:
 * @self: a #PortalRestore
 * @token: a grant's token
 *
 * Forgets a grant, saving the store when it was persistent.
 *
 * Returns: %TRUE if there was one
 */
gboolean portal_restore_remove (PortalRestore *self,
                                const gchar   *token);

/**
 * portal_restore_get_path:
 * @self: a #PortalRestore
 *
 * Returns: (transfer none): the file persistent grants live in
 */
const gchar *portal_restore_get_path (PortalRestore *self);

G_END_DECLS

#endif /* PORTAL_RESTORE_H */
