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
 * gowl-hotkey-hotstring.h - AutoHotkey hotstrings as gabbrev matches.
 *
 * gowl has one recognizer of typed text, gabbrev's, and the hotkey
 * module hands its hotstrings to it rather than run a second one that
 * would fight it over the same keystrokes.  This translates; the module
 * installs the result with gowl_gabbrev_add_yaml_source("hotkey", ...).
 * Pure: a test reads the YAML it makes.
 */

#ifndef GOWL_HOTKEY_HOTSTRING_H
#define GOWL_HOTKEY_HOTSTRING_H

#include <glib.h>

#include "gowl-hotkey-model.h"

G_BEGIN_DECLS

/* The gabbrev variable type an X hotstring's match runs its actions
 * through; the module registers it. */
#define HK_GABBREV_VAR_TYPE "hotkey-action"

gchar *hk_hotstrings_to_gabbrev (GPtrArray *hotstrings,
                                 HkSet     *set,
                                 GPtrArray *warnings);

G_END_DECLS

#endif /* GOWL_HOTKEY_HOTSTRING_H */
