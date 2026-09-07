/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Turning nmcli's output into a list of networks.
 *
 * Both bugs this covers only appear with real output.  nmcli lists one
 * row per BSS, so a dual-band router repeats its SSID and a mesh
 * repeats it once per node -- the panel filled with duplicates and real
 * networks fell off the end of the list.  And the truncation that
 * claimed to keep "the strongest twenty" kept whichever twenty nmcli
 * emitted first.
 */

#include <glib.h>
#include <string.h>

#include "../modules/bar/bar-wifi-scan.c"

static const gchar *
row_ssid(GPtrArray *rows, guint i, gchar **rest)
{
	static gchar buf[256];
	const gchar *r = g_ptr_array_index(rows, i);
	const gchar *tab = strchr(r, '\t');

	g_snprintf(buf, sizeof(buf), "%.*s", (int)(tab - r), r);
	if (rest != NULL)
		*rest = (gchar *)(tab + 1);
	return buf;
}

/* The reported bug: one network, several radios, several rows. */
static void
test_duplicate_ssids_collapse(void)
{
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_free);
	const gchar *out =
		"yes:Podbielniak:71:WPA2\n"
		":Podbielniak:47:WPA2\n"          /* the 2.4 GHz radio */
		":Podbielniak:39:WPA2\n"          /* a mesh node */
		":Neighbour:55:WPA2\n";

	bar_wifi_scan_parse(rows, out, 20);
	g_assert_cmpuint(rows->len, ==, 2);
	/* The strongest sighting is the one kept, and it ranks first. */
	g_assert_cmpstr(row_ssid(rows, 0, NULL), ==, "Podbielniak");
	g_assert_cmpstr(row_ssid(rows, 1, NULL), ==, "Neighbour");
}

/* The kept row must be the STRONGEST sighting, whichever order the
   duplicates arrived in. */
static void
test_keeps_the_strongest_sighting(void)
{
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_free);
	gchar *rest = NULL;
	const gchar *out =
		":Home:20:WPA2\n"
		":Home:88:WPA2\n"
		":Home:44:WPA2\n";

	bar_wifi_scan_parse(rows, out, 20);
	g_assert_cmpuint(rows->len, ==, 1);
	row_ssid(rows, 0, &rest);
	g_assert_true(g_str_has_prefix(rest, "88\t"));
}

/* Rank, then truncate.  Truncating first keeps whatever came earliest,
   which is what the old code did while claiming otherwise. */
static void
test_limit_keeps_the_strongest(void)
{
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_free);
	GString *out = g_string_new(NULL);
	gint i;

	/* Weak ones first, so a truncate-before-sort keeps the wrong set. */
	for (i = 0; i < 30; i++)
		g_string_append_printf(out, ":net%02d:%d:WPA2\n", i, i);

	bar_wifi_scan_parse(rows, out->str, 5);
	g_assert_cmpuint(rows->len, ==, 5);
	g_assert_cmpstr(row_ssid(rows, 0, NULL), ==, "net29");
	g_assert_cmpstr(row_ssid(rows, 4, NULL), ==, "net25");
	g_string_free(out, TRUE);
}

/* An SSID may contain a colon, which nmcli escapes.  Splitting naively
   turns one network into two fields and loses it. */
static void
test_escaped_colon_in_ssid(void)
{
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_free);
	const gchar *out = ":cafe\\:wifi:60:WPA2\n";

	bar_wifi_scan_parse(rows, out, 20);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_cmpstr(row_ssid(rows, 0, NULL), ==, "cafe:wifi");
}

/* A hidden network reports an empty SSID; it cannot be shown or picked,
   so it must not take a slot. */
static void
test_hidden_networks_dropped(void)
{
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_free);
	const gchar *out = "::90:WPA2\n:Real:10:WPA2\n";

	bar_wifi_scan_parse(rows, out, 20);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_cmpstr(row_ssid(rows, 0, NULL), ==, "Real");
}

/* Degenerate input must clear the list rather than leave stale results
   on screen. */
static void
test_degenerate(void)
{
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_free);

	g_ptr_array_add(rows, g_strdup("stale\t99\tWPA2\t"));
	bar_wifi_scan_parse(rows, NULL, 20);
	g_assert_cmpuint(rows->len, ==, 0);

	bar_wifi_scan_parse(rows, "", 20);
	g_assert_cmpuint(rows->len, ==, 0);

	bar_wifi_scan_parse(rows, "garbage without colons\n", 20);
	g_assert_cmpuint(rows->len, ==, 0);
}

/* Against real output when there is some to hand. */
static void
test_against_real_nmcli(void)
{
	const gchar *path = g_getenv("GOWL_TEST_NMCLI_WIFI");
	g_autofree gchar *text = NULL;
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_free);
	guint i;

	if (path == NULL) {
		g_test_skip("set GOWL_TEST_NMCLI_WIFI to real nmcli output");
		return;
	}
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	bar_wifi_scan_parse(rows, text, 20);

	/* Whatever the input, no SSID may appear twice. */
	for (i = 0; i < rows->len; i++) {
		guint j;
		g_autofree gchar *a = g_strdup(row_ssid(rows, i, NULL));

		for (j = i + 1; j < rows->len; j++) {
			if (g_strcmp0(a, row_ssid(rows, j, NULL)) == 0)
				g_error("SSID '%s' listed twice", a);
		}
	}
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/bar-wifi-scan/duplicates-collapse",
	                test_duplicate_ssids_collapse);
	g_test_add_func("/bar-wifi-scan/keeps-strongest",
	                test_keeps_the_strongest_sighting);
	g_test_add_func("/bar-wifi-scan/limit-keeps-strongest",
	                test_limit_keeps_the_strongest);
	g_test_add_func("/bar-wifi-scan/escaped-colon",
	                test_escaped_colon_in_ssid);
	g_test_add_func("/bar-wifi-scan/hidden-dropped",
	                test_hidden_networks_dropped);
	g_test_add_func("/bar-wifi-scan/degenerate", test_degenerate);
	g_test_add_func("/bar-wifi-scan/real-nmcli", test_against_real_nmcli);
	return g_test_run();
}
