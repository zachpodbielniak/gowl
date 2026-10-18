/* test-portal-eis-zones.c -- the portal notices when the screens change
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * xdg-desktop-portal-gowl turns the compositor's zones into libei
 * regions, and deskflow takes the screen's shape from those.  A
 * region is fixed once its device is added, so new zones only reach a
 * connected client as a new device -- and the portal used to just swap
 * the list it would use for the NEXT device, leaving the connected one
 * describing the screen as it was.
 *
 * portal_eis_commit_zones() decides when to rebuild: whenever the zones
 * differ from the ones the devices were built from, and only then.
 * This drives it against a real libeis context.  The rebuild of a live
 * device needs a libei client on the other end, and libei's client
 * library (Fedora: libei-devel) is not something this suite can count
 * on, so what is pinned here is the decision -- including that a
 * rotation, which keeps the area and only swaps the sides, counts.
 *
 * portal-eis.c is included rather than linked: the portal is a
 * standalone binary with no library, and the test reads its state.
 */

#include "../tools/xdg-desktop-portal-gowl/portal-eis.c"

static void
give_zones(PortalEis *eis, const struct portal_eis_zone *zones, guint n)
{
	guint i;

	portal_eis_clear_zones(eis);
	for (i = 0; i < n; i++)
		portal_eis_add_zone(eis, zones[i].x, zones[i].y,
		                    zones[i].w, zones[i].h);
}

static void
test_commit_tracks_changes(void)
{
	static const struct portal_eis_zone laptop[] = {
		{ 0, 0, 1920, 1200 },
	};
	static const struct portal_eis_zone rotated[] = {
		{ 0, 0, 1200, 1920 },
	};
	static const struct portal_eis_zone docked[] = {
		{ 0, 0, 1920, 1200 },
		{ 1920, 0, 2560, 2880 },
	};
	PortalEis *eis;

	eis = portal_eis_new();
	if (eis == NULL) {
		g_test_skip("libeis could not set up a backend here");
		return;
	}

	/* The first zones are a change from nothing. */
	give_zones(eis, laptop, G_N_ELEMENTS(laptop));
	g_assert_true(portal_eis_commit_zones(eis));
	/* The same zones again -- a layout change elsewhere -- are not. */
	give_zones(eis, laptop, G_N_ELEMENTS(laptop));
	g_assert_false(portal_eis_commit_zones(eis));
	/* A quarter turn: same area, sides swapped. */
	give_zones(eis, rotated, G_N_ELEMENTS(rotated));
	g_assert_true(portal_eis_commit_zones(eis));
	g_assert_cmpuint(g_array_index(eis->committed,
	                               struct portal_eis_zone, 0).w, ==, 1200);
	/* A monitor plugged in, and pulled out again. */
	give_zones(eis, docked, G_N_ELEMENTS(docked));
	g_assert_true(portal_eis_commit_zones(eis));
	g_assert_cmpuint(eis->committed->len, ==, 2);
	give_zones(eis, laptop, G_N_ELEMENTS(laptop));
	g_assert_true(portal_eis_commit_zones(eis));
	g_assert_cmpuint(eis->committed->len, ==, 1);

	/* With no client connected there is no device to rebuild, and
	 * committing must not invent one. */
	g_assert_null(eis->device);
	g_assert_null(eis->tx_device);

	portal_eis_free(eis);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/portal/eis/commit-tracks-zone-changes",
	                test_commit_tracks_changes);
	return g_test_run();
}
