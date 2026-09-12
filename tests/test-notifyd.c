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
 * test-notifyd.c - The notification daemon module on a private session
 * bus.
 *
 * A GTestDBus stands in for the session; the module, loaded into a
 * headless compositor, must take org.freedesktop.Notifications there.
 * A Notify from another connection is answered with an id straight
 * away (the bus thread never waits on the compositor), and once the
 * compositor's loop turns, the notification is counted -- which is the
 * whole cross-thread path: bus thread, eventfd, wl_event_loop.  DND
 * counts without showing; clear resets; the name goes back when the
 * module stops.
 */

#include <glib.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include "core/gowl-compositor.h"
#include "core/gowl-core-private.h"
#include "config/gowl-config.h"
#include "module/gowl-module-manager.h"

#ifndef GOWL_TEST_MODULE_DIR
#define GOWL_TEST_MODULE_DIR "build/release/modules"
#endif

typedef struct {
	gchar             *runtime;
	GTestDBus         *bus;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
} Fixture;

static void
pump(Fixture *f, gint ms)
{
	struct wl_event_loop *loop = gowl_compositor_get_event_loop(f->compositor);
	gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < end) {
		wl_event_loop_dispatch(loop, 10);
		wl_display_flush_clients(f->compositor->wl_display);
	}
}

static void
fixture_setup(Fixture *f, gconstpointer data)
{
	const gchar *parent;
	gchar *state;
	g_autofree gchar *path = NULL;
	GError *error = NULL;
	(void)data;

	parent = g_getenv("XDG_RUNTIME_DIR");
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	f->runtime = g_build_filename(parent, "gowl-notifyd-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(f->runtime, 0700));
	state = g_build_filename(f->runtime, "state", NULL);
	/* A bus of our own; g_test_dbus_up sets DBUS_SESSION_BUS_ADDRESS --
	 * and unsets XDG_RUNTIME_DIR, so ours goes in after it. */
	f->bus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(f->bus);

	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", f->runtime, TRUE);
	g_setenv("XDG_STATE_HOME", state, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);
	g_free(state);


	f->modules = gowl_module_manager_new();
	path = g_strdup_printf("%s/notifyd.so", GOWL_TEST_MODULE_DIR);
	if (!gowl_module_manager_load_module(f->modules, path, &error))
		g_error("%s", error->message);
	gowl_module_manager_activate_all(f->modules);

	f->config = gowl_config_new();
	f->compositor = gowl_compositor_new();
	gowl_compositor_set_config(f->compositor, f->config);
	gowl_compositor_set_module_manager(f->compositor, f->modules);
	if (!gowl_compositor_start(f->compositor, &error))
		g_error("could not start a headless compositor: %s",
		        error->message);
	gowl_module_manager_dispatch_startup(f->modules, f->compositor);
}

static void
fixture_teardown(Fixture *f, gconstpointer data)
{
	(void)data;

	gowl_module_manager_dispatch_shutdown(f->modules, f->compositor);
	g_object_unref(f->compositor);
	g_object_unref(f->modules);
	g_object_unref(f->config);
	g_test_dbus_down(f->bus);
	g_object_unref(f->bus);
	g_rmdir(f->runtime);
	g_free(f->runtime);
}

/* Waits, on this thread's context, until the name has an owner. */
static gboolean
wait_for_owner(GDBusConnection *conn, gboolean want)
{
	gint tries;

	for (tries = 0; tries < 200; tries++) {
		GVariant *r = g_dbus_connection_call_sync(conn,
			"org.freedesktop.DBus", "/org/freedesktop/DBus",
			"org.freedesktop.DBus", "NameHasOwner",
			g_variant_new("(s)", "org.freedesktop.Notifications"),
			G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL,
			NULL);
		gboolean has = FALSE;

		if (r != NULL) {
			g_variant_get(r, "(b)", &has);
			g_variant_unref(r);
		}
		if (has == want)
			return TRUE;
		g_usleep(10 * 1000);
	}
	return FALSE;
}

static guint32
notify(GDBusConnection *conn, const gchar *summary, const gchar *body,
       guint8 urgency)
{
	GVariantBuilder hints;
	GVariant *r;
	guint32 id = 0;
	GError *error = NULL;

	g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&hints, "{sv}", "urgency",
	                      g_variant_new_byte(urgency));
	r = g_dbus_connection_call_sync(conn, "org.freedesktop.Notifications",
		"/org/freedesktop/Notifications", "org.freedesktop.Notifications",
		"Notify",
		g_variant_new("(susssasa{sv}i)", "test", 0, "", summary, body,
		              NULL, &hints, -1),
		G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
	g_assert_no_error(error);
	g_variant_get(r, "(u)", &id);
	g_variant_unref(r);
	return id;
}

static gchar *
command(Fixture *f, const gchar *line)
{
	gchar *r = gowl_compositor_run_command(f->compositor, line);

	g_assert_nonnull(r);
	g_strchomp(r);
	return r;
}

static void
test_notify_counts_and_dnd(Fixture *f, gconstpointer data)
{
	GDBusConnection *conn;
	GError *error = NULL;
	GVariant *r;
	const gchar *name;
	gchar *reply;
	(void)data;

	conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(wait_for_owner(conn, TRUE));

	r = g_dbus_connection_call_sync(conn, "org.freedesktop.Notifications",
		"/org/freedesktop/Notifications", "org.freedesktop.Notifications",
		"GetServerInformation", NULL, G_VARIANT_TYPE("(ssss)"),
		G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
	g_assert_no_error(error);
	g_variant_get(r, "(&s&s&s&s)", &name, NULL, NULL, NULL);
	g_assert_cmpstr(name, ==, "gowl");
	g_variant_unref(r);

	/* Ids are handed out in order, without waiting on the compositor. */
	g_assert_cmpuint(notify(conn, "one", "first", 1), ==, 1);
	g_assert_cmpuint(notify(conn, "two", "", 1), ==, 2);
	reply = command(f, "notify-count");
	g_assert_cmpstr(reply, ==, "0");   /* not yet: the loop has not turned */
	g_free(reply);
	pump(f, 100);
	reply = command(f, "notify-count");
	g_assert_cmpstr(reply, ==, "2");
	g_free(reply);

	/* DND still counts. */
	reply = command(f, "notify-dnd on");
	g_assert_cmpstr(reply, ==, "on");
	g_free(reply);
	notify(conn, "three", "", 2);
	pump(f, 100);
	reply = command(f, "notify-count");
	g_assert_cmpstr(reply, ==, "3");
	g_free(reply);

	reply = command(f, "notify-clear");
	g_free(reply);
	reply = command(f, "notify-count");
	g_assert_cmpstr(reply, ==, "0");
	g_free(reply);
	reply = command(f, "notify-dnd toggle");
	g_assert_cmpstr(reply, ==, "off");
	g_free(reply);

	/* Stopping the module gives the name back. */
	gowl_module_manager_dispatch_shutdown(f->modules, f->compositor);
	g_assert_true(wait_for_owner(conn, FALSE));
	g_object_unref(conn);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/notifyd/counts-and-dnd", Fixture, NULL,
	           fixture_setup, test_notify_counts_and_dnd, fixture_teardown);
	return g_test_run();
}
