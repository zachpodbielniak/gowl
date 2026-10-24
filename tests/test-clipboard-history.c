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
 * test-clipboard-history.c - The clipboard module reads on the
 * compositor's own event loop.
 *
 * A headless compositor with the real clipboard module.  Text is put on
 * the clipboard, then ONLY the compositor's wl_event_loop is dispatched
 * -- what gowl's main() and cmacs's dispatch thread both do -- and the
 * entry has to reach the history.
 *
 * The module used to drain the selection pipe with g_unix_fd_add() and
 * time it out with g_timeout_add(), both on GLib's default context.
 * That context is not the compositor's.  Standalone nothing iterates
 * it, so no copy ever reached the history.  Under cmacs --gowl Emacs's
 * thread iterates it, so the read and clip_store() ran there, beside
 * the dispatch thread and without the gowl lock.  The second assertion
 * pins that directly: setting the clipboard must leave nothing pending
 * on the default context.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>

#include "gowl.h"
#include "core/gowl-core-private.h"

typedef struct {
	gchar             *runtime;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
	gboolean           up;
} Rig;

static void
rig_setup(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *clip = NULL;
	const gchar *parent;
	GError *error = NULL;

	(void)data;
	memset(r, 0, sizeof *r);

	parent = g_getenv("XDG_RUNTIME_DIR");
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-clip-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	/* Before anything asks GLib for it: the state dir is cached. */
	g_setenv("XDG_STATE_HOME", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	r->config = gowl_config_new();
	gowl_config_set_lock_command(r->config, "");
	r->modules = gowl_module_manager_new();

	clip = g_build_filename(GOWL_TEST_MODULE_DIR, "clipboard.so", NULL);
	if (!gowl_module_manager_load_module(r->modules, clip, &error)) {
		g_test_skip("the clipboard module did not load");
		g_clear_error(&error);
		return;
	}
	gowl_module_manager_activate_all(r->modules);

	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_test_skip("no headless compositor here");
		g_clear_error(&error);
		return;
	}
	gowl_module_manager_dispatch_startup(r->modules, r->compositor);
	r->up = TRUE;
}

/* Removes the store the module made, then the runtime dir. */
static void
rig_remove_store(Rig *r)
{
	static const gchar *const dirs[] = {
		"state/gowl/clipboard", "state/gowl", "state",
		"gowl/clipboard", "gowl"
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(dirs); i++) {
		g_autofree gchar *dir = g_build_filename(r->runtime, dirs[i],
		                                         NULL);
		GDir *d = g_dir_open(dir, 0, NULL);
		const gchar *name;

		if (d == NULL)
			continue;
		while ((name = g_dir_read_name(d)) != NULL) {
			g_autofree gchar *path = g_build_filename(dir, name,
			                                          NULL);

			if (!g_file_test(path, G_FILE_TEST_IS_DIR))
				g_unlink(path);
		}
		g_dir_close(d);
		g_rmdir(dir);
	}
}

static void
rig_teardown(
	Rig           *r,
	gconstpointer  data
){
	(void)data;
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	if (r->runtime != NULL) {
		rig_remove_store(r);
		g_rmdir(r->runtime);
	}
	g_free(r->runtime);
}

/* The history index the module writes, or NULL while there is none. */
static gchar *
read_index(void)
{
	g_autofree gchar *path = NULL;
	gchar *text = NULL;

	path = g_build_filename(g_get_user_state_dir(), "gowl", "clipboard",
	                        "index", NULL);
	if (!g_file_get_contents(path, &text, NULL, NULL))
		return NULL;
	return text;
}

/*
 * Dispatch only the compositor's event loop until @needle shows up in
 * the index, or two seconds pass.  Returns the index text, or NULL.
 */
static gchar *
pump_until_indexed(
	Rig         *r,
	const gchar *needle
){
	struct wl_event_loop *loop;
	gint64 deadline;

	loop = gowl_compositor_get_event_loop(r->compositor);
	deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
	while (g_get_monotonic_time() < deadline) {
		gchar *text;

		wl_event_loop_dispatch(loop, 10);
		text = read_index();
		if (text != NULL && strstr(text, needle) != NULL)
			return text;
		g_free(text);
	}
	return NULL;
}

static void
test_copy_reaches_history_on_the_event_loop(
	Rig           *r,
	gconstpointer  data
){
	g_autofree gchar *index = NULL;
	GowlSeat *seat;

	(void)data;
	if (!r->up)
		return;

	seat = gowl_compositor_get_seat(r->compositor);
	g_assert_nonnull(seat);

	gowl_seat_set_clipboard(seat, "gowl-clipboard-history-probe");

	/* Nothing may have been put on GLib's default context: under cmacs
	 * that context is Emacs's thread, which holds no gowl lock. */
	g_assert_false(g_main_context_pending(NULL));

	index = pump_until_indexed(r, "gowl-clipboard-history-probe");
	g_assert_nonnull(index);
	g_assert_nonnull(strstr(index, "text/plain"));
}

static void
test_deactivate_with_a_read_in_flight(
	Rig           *r,
	gconstpointer  data
){
	GowlSeat *seat;

	(void)data;
	if (!r->up)
		return;

	/* Start a read and tear everything down before the event loop
	 * gets to it.  The module has to drop the read's sources and fd
	 * itself; with GLib criticals fatal, any double removal or a
	 * source left behind pointing at the module fails the run. */
	seat = gowl_compositor_get_seat(r->compositor);
	gowl_seat_set_clipboard(seat, "gowl-clipboard-inflight-probe");
	g_assert_false(g_main_context_pending(NULL));
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING);

	g_test_add("/clipboard-history/copy-reaches-history-on-the-event-loop",
	           Rig, NULL, rig_setup,
	           test_copy_reaches_history_on_the_event_loop, rig_teardown);
	g_test_add("/clipboard-history/deactivate-with-a-read-in-flight",
	           Rig, NULL, rig_setup,
	           test_deactivate_with_a_read_in_flight, rig_teardown);

	return g_test_run();
}
