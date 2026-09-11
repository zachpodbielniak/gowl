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

#ifndef GOWL_SCRATCHPAD_HANDLER_H
#define GOWL_SCRATCHPAD_HANDLER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GOWL_TYPE_SCRATCHPAD_HANDLER (gowl_scratchpad_handler_get_type())

G_DECLARE_INTERFACE(GowlScratchpadHandler, gowl_scratchpad_handler, GOWL, SCRATCHPAD_HANDLER, GObject)

/**
 * GowlScratchpadHandlerInterface:
 * @parent_iface: the parent interface
 * @is_scratchpad: whether @client is one of the scratchpad's windows
 * @toggle_scratchpad: show the scratchpad if it is hidden, hide it if it
 *   is shown.  @name is kept for compatibility with named scratchpads;
 *   there is one scratchpad, and the name is ignored.
 *
 * Implemented by the scratchpad module: a panel that slides up from the
 * bottom of the focused output holding any windows sent to it.  Windows
 * join and leave through its IPC commands (`scratchpad-add',
 * `scratchpad-remove'); this interface is the typed entry point an
 * embedder uses to ask and to toggle.
 */
struct _GowlScratchpadHandlerInterface {
	GTypeInterface parent_iface;

	gboolean (*is_scratchpad)     (GowlScratchpadHandler *self, gpointer client);
	void     (*toggle_scratchpad) (GowlScratchpadHandler *self, const gchar *name);
};

/* Public dispatch functions */
gboolean gowl_scratchpad_handler_is_scratchpad     (GowlScratchpadHandler *self, gpointer client);
void     gowl_scratchpad_handler_toggle_scratchpad (GowlScratchpadHandler *self, const gchar *name);

G_END_DECLS

#endif /* GOWL_SCRATCHPAD_HANDLER_H */
