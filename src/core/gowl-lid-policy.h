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

#ifndef GOWL_LID_POLICY_H
#define GOWL_LID_POLICY_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * gowl_name_is_internal_panel:
 * @name: (nullable): a DRM connector / output name (e.g. "eDP-1", "DP-1")
 *
 * Classify an output by its connector name as a laptop's built-in
 * panel.  Internal panels use the DRM connector types eDP, LVDS and
 * DSI; everything else (DP, HDMI-A, VGA, DVI, headless, nested
 * Wayland, …) is treated as external.  The match is a case-sensitive
 * prefix test, because DRM connector names are spelled exactly
 * "eDP-N" / "LVDS-N" / "DSI-N".
 *
 * Returns: %TRUE if @name names an internal laptop panel, %FALSE
 *          otherwise (including for %NULL).
 */
gboolean gowl_name_is_internal_panel(const char *name);

/**
 * gowl_lid_internal_should_enable:
 * @lid_closed: %TRUE when the laptop lid is currently shut
 * @ext_enabled_count: number of enabled external (non-internal) outputs
 *
 * Decide whether an internal laptop panel should be powered on given
 * the lid state and how many external displays are lit.  An internal
 * panel stays on whenever the lid is open, and is turned off only when
 * the lid is shut AND at least one external display is available — the
 * single-display safety net that prevents blacking out the only
 * usable screen.
 *
 * Returns: %TRUE if the internal panel should be enabled.
 */
gboolean gowl_lid_internal_should_enable(gboolean lid_closed,
                                         gint     ext_enabled_count);

/**
 * gowl_lid_parse_proc_state:
 * @proc_contents: (nullable): the text read from
 *   /proc/acpi/button/lid/<dev>/state (e.g. "state:      closed\n")
 *
 * Parse the ACPI lid state file contents into a boolean.  The file
 * reports either "open" or "closed"; this returns %TRUE when the text
 * contains "closed".  Robust to surrounding whitespace and extra
 * lines.  A %NULL or empty string is treated as "open" (%FALSE), the
 * safe default for machines without an ACPI lid button.
 *
 * Returns: %TRUE if the contents indicate the lid is closed.
 */
gboolean gowl_lid_parse_proc_state(const char *proc_contents);

G_END_DECLS

#endif /* GOWL_LID_POLICY_H */
