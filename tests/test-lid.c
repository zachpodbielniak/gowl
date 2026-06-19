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

/* Unit tests for the pure laptop-lid decision logic. */

#include "core/gowl-lid-policy.h"
#include <glib.h>

static void
test_internal_panel_names(void)
{
	/* Internal laptop panels: eDP / LVDS / DSI connectors. */
	g_assert_true(gowl_name_is_internal_panel("eDP-1"));
	g_assert_true(gowl_name_is_internal_panel("eDP-2"));
	g_assert_true(gowl_name_is_internal_panel("LVDS-1"));
	g_assert_true(gowl_name_is_internal_panel("LVDS-2"));
	g_assert_true(gowl_name_is_internal_panel("DSI-1"));
	g_assert_true(gowl_name_is_internal_panel("DSI-2"));

	/* External / virtual / nested outputs are NOT internal. */
	g_assert_false(gowl_name_is_internal_panel("DP-1"));
	g_assert_false(gowl_name_is_internal_panel("DP-2"));
	g_assert_false(gowl_name_is_internal_panel("HDMI-A-1"));
	g_assert_false(gowl_name_is_internal_panel("HDMI-A-2"));
	g_assert_false(gowl_name_is_internal_panel("VGA-1"));
	g_assert_false(gowl_name_is_internal_panel("DVI-D-1"));
	g_assert_false(gowl_name_is_internal_panel("Virtual-1"));
	g_assert_false(gowl_name_is_internal_panel("WL-1"));
	g_assert_false(gowl_name_is_internal_panel("HEADLESS-1"));

	/* Edge cases. */
	g_assert_false(gowl_name_is_internal_panel(""));
	g_assert_false(gowl_name_is_internal_panel(NULL));

	/* Match is case-sensitive (DRM names are exact case). */
	g_assert_false(gowl_name_is_internal_panel("edp-1"));
	g_assert_false(gowl_name_is_internal_panel("EDP-1"));
	g_assert_false(gowl_name_is_internal_panel("Lvds-1"));
}

static void
test_should_enable_truth_table(void)
{
	/* Lid open: the internal panel is always on. */
	g_assert_true(gowl_lid_internal_should_enable(FALSE, 0));
	g_assert_true(gowl_lid_internal_should_enable(FALSE, 1));
	g_assert_true(gowl_lid_internal_should_enable(FALSE, 3));

	/* Lid shut, no external display: stay on (single-display safety). */
	g_assert_true(gowl_lid_internal_should_enable(TRUE, 0));

	/* Lid shut, at least one external: power the internal panel off. */
	g_assert_false(gowl_lid_internal_should_enable(TRUE, 1));
	g_assert_false(gowl_lid_internal_should_enable(TRUE, 3));

	/* Defensive: a negative count behaves like zero (stay on). */
	g_assert_true(gowl_lid_internal_should_enable(TRUE, -1));
}

static void
test_parse_proc_state(void)
{
	/* Real ACPI file layouts. */
	g_assert_false(gowl_lid_parse_proc_state("state:      open\n"));
	g_assert_true(gowl_lid_parse_proc_state("state:      closed\n"));

	/* Whitespace / newline robustness. */
	g_assert_true(gowl_lid_parse_proc_state("state:      closed"));
	g_assert_true(gowl_lid_parse_proc_state("closed"));
	g_assert_false(gowl_lid_parse_proc_state("open"));

	/* Multi-line content. */
	g_assert_true(gowl_lid_parse_proc_state("foo\nstate:      closed\n"));

	/* Unknown / empty / NULL all read as "open" (safe default). */
	g_assert_false(gowl_lid_parse_proc_state("state:      unknown\n"));
	g_assert_false(gowl_lid_parse_proc_state(""));
	g_assert_false(gowl_lid_parse_proc_state(NULL));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/lid/internal-panel-names",
	                test_internal_panel_names);
	g_test_add_func("/lid/should-enable-truth-table",
	                test_should_enable_truth_table);
	g_test_add_func("/lid/parse-proc-state", test_parse_proc_state);

	return g_test_run();
}
