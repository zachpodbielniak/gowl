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
 * gowl-macro-voice.h - Match what was said to a macro.
 *
 * Pure GLib.  Speech-to-text hands back a sentence -- "Sort windows.",
 * "pip corner twenty five" -- and this decides which macro it names and
 * what is left over as arguments.  Kept apart from the module so the
 * tests can feed it sentences with no microphone and no compositor.
 */

#ifndef GOWL_MACRO_VOICE_H
#define GOWL_MACRO_VOICE_H

#include <glib.h>

G_BEGIN_DECLS

gchar    *gowl_macro_voice_normalise  (const gchar         *text);
gboolean  gowl_macro_voice_match      (const gchar         *text,
                                       const gchar * const *names,
                                       GHashTable          *phrases,
                                       gchar              **out_name,
                                       gchar              **out_args);

G_END_DECLS

#endif /* GOWL_MACRO_VOICE_H */
