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
 * xdg-desktop-portal-gowl -- the InputCapture + RemoteDesktop portal
 * backend for gowl / cmacs --gowl.
 *
 *   normal mode:  own org.freedesktop.impl.portal.desktop.gowl and serve
 *                 the InputCapture/RemoteDesktop impl-portal interfaces.
 *   --self-test:  connect to the compositor, query zones, install a
 *                 left-edge barrier, enable capture, and print what comes
 *                 back -- no D-Bus.  Used by the Phase-2 integration test.
 */

#include <glib.h>
#include <glib-unix.h>
#include <stdio.h>

#include "portal-eis.h"
#include "portal-wayland.h"
#include "portal-dbus.h"

static GMainLoop *main_loop;

static void
on_activation(guint32 activation_id, double x, double y, guint32 barrier_id,
              gboolean activated, gpointer user_data)
{
	(void)user_data;
	if (activated)
		g_print("ACTIVATED id=%u barrier=%u at (%.1f, %.1f)\n",
			activation_id, barrier_id, x, y);
	else
		g_print("DEACTIVATED id=%u\n", activation_id);
}

static gboolean
on_signal(gpointer data)
{
	(void)data;
	g_main_loop_quit(main_loop);
	return G_SOURCE_REMOVE;
}

static int
run_self_test(void)
{
	PortalEis     *eis;
	PortalWayland *wl;
	PortalZone    *zones = NULL;
	guint          n_zones = 0;
	guint32        zone_set = 0;
	GError        *error = NULL;
	int            client_fd;

	eis = portal_eis_new();
	if (eis == NULL) {
		g_printerr("self-test: failed to create EIS server\n");
		return 1;
	}

	wl = portal_wayland_new(eis, &error);
	if (wl == NULL) {
		g_printerr("self-test: %s\n", error->message);
		g_clear_error(&error);
		portal_eis_free(eis);
		return 1;
	}
	portal_wayland_set_activation_callback(wl, on_activation, NULL);

	if (!portal_wayland_get_zones(wl, &zones, &n_zones, &zone_set)) {
		g_printerr("self-test: get_zones failed\n");
		portal_wayland_free(wl);
		portal_eis_free(eis);
		return 1;
	}

	g_print("zones: %u (zone_set=%u)\n", n_zones, zone_set);
	if (n_zones > 0) {
		PortalZone *z = &zones[0];
		g_print("zone[0] = %ux%u @ (%d,%d)\n",
			z->width, z->height, z->x, z->y);
		/* Install a left-edge vertical barrier on the first zone. */
		portal_wayland_add_barrier(wl, 1, z->x, z->y,
			z->x, z->y + (int32_t)z->height);
		portal_wayland_set_barriers(wl, zone_set);
		portal_wayland_enable(wl);
		g_print("installed left-edge barrier id 1; capture armed.\n");
	}

	client_fd = portal_eis_connect_fd(eis);
	g_print("EIS client fd = %d (pass to a libei client to receive)\n",
		client_fd);

	g_print("self-test running; move the cursor across the left edge. "
		"Ctrl-C to quit.\n");

	main_loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGINT, on_signal, NULL);
	g_unix_signal_add(SIGTERM, on_signal, NULL);
	g_main_loop_run(main_loop);
	g_main_loop_unref(main_loop);

	portal_wayland_free(wl);
	portal_eis_free(eis);
	return 0;
}

int
main(int argc, char *argv[])
{
	PortalEis     *eis;
	PortalWayland *wl;
	PortalDbus    *dbus;
	GError        *error = NULL;

	if (argc > 1 && g_strcmp0(argv[1], "--self-test") == 0)
		return run_self_test();

	eis = portal_eis_new();
	if (eis == NULL) {
		g_printerr("xdg-desktop-portal-gowl: failed to create EIS "
			"server\n");
		return 1;
	}

	wl = portal_wayland_new(eis, &error);
	if (wl == NULL) {
		g_printerr("xdg-desktop-portal-gowl: %s\n", error->message);
		g_clear_error(&error);
		portal_eis_free(eis);
		return 1;
	}

	dbus = portal_dbus_new(wl, eis);

	main_loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGINT, on_signal, NULL);
	g_unix_signal_add(SIGTERM, on_signal, NULL);
	g_main_loop_run(main_loop);
	g_main_loop_unref(main_loop);

	portal_dbus_free(dbus);
	portal_wayland_free(wl);
	portal_eis_free(eis);
	return 0;
}
