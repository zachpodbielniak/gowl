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

#ifndef PORTAL_CHOOSER_H
#define PORTAL_CHOOSER_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * The chooser: a human picks one line of a list.
 *
 * Shared by ScreenCast (what to share) and Screenshot (what to shoot).
 * In order:
 *
 *   1. $GOWL_PORTAL_CHOOSER, a dmenu-style command: lines on stdin, the
 *      chosen one on stdout.  An explicit setting always wins.
 *   2. gowl's own menu card (Super+space), when the portal runs inside
 *      a gowl session whose menu module is loaded: `menu-choose' over
 *      the IPC socket, the answer read back as an event.
 *   3. the first of bemenu, fuzzel, wofi, rofi, dmenu on $PATH.
 *
 * None of them available is not an error; the caller decides what a
 * missing chooser means.
 */

/**
 * PortalChooserResult:
 * @PORTAL_CHOOSER_CHOSEN: a line was chosen
 * @PORTAL_CHOOSER_CANCELLED: the human dismissed the chooser
 * @PORTAL_CHOOSER_UNAVAILABLE: no chooser could be run
 */
typedef enum {
	PORTAL_CHOOSER_CHOSEN,
	PORTAL_CHOOSER_CANCELLED,
	PORTAL_CHOOSER_UNAVAILABLE
} PortalChooserResult;

/**
 * portal_chooser_run:
 * @prompt: what the chooser is asking, e.g. `Share'
 * @lines: (array zero-terminated=1): the choices; a tab or newline in
 *   one is replaced by a space
 * @index: (out): the chosen line's index, when %PORTAL_CHOOSER_CHOSEN
 *
 * Puts @lines in front of a human and waits for the answer.
 *
 * Returns: what happened
 */
PortalChooserResult portal_chooser_run (const gchar        *prompt,
                                        const gchar *const *lines,
                                        guint              *index);

G_END_DECLS

#endif /* PORTAL_CHOOSER_H */
