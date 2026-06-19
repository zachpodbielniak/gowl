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
 * Pure decision logic for laptop-lid output management.  Deliberately
 * free of wlroots / GObject types so it can be unit-tested directly
 * (see tests/test-lid.c).  The wl_listener glue that drives these lives
 * in gowl-compositor.c.
 */

#include "core/gowl-lid-policy.h"

#include <string.h>

gboolean
gowl_name_is_internal_panel(const char *name)
{
	if (name == NULL)
		return FALSE;

	return g_str_has_prefix(name, "eDP")
	    || g_str_has_prefix(name, "LVDS")
	    || g_str_has_prefix(name, "DSI");
}

gboolean
gowl_lid_internal_should_enable(gboolean lid_closed, gint ext_enabled_count)
{
	/* Lid open: always on.  Lid shut: off only if some external
	 * display remains to fall back to (never black out the last
	 * screen). */
	return (!lid_closed) || (ext_enabled_count <= 0);
}

gboolean
gowl_lid_parse_proc_state(const char *proc_contents)
{
	if (proc_contents == NULL)
		return FALSE;

	/* The ACPI file reads "state:      open" or "state:      closed".
	 * A substring test for "closed" is whitespace- and layout-robust;
	 * "open" never contains it. */
	return strstr(proc_contents, "closed") != NULL;
}
