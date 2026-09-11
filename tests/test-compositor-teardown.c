/* test-compositor-teardown.c -- a compositor that was started can be
 * finalized, and the modules it ran can be released after it
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Standalone gowl and cmacs --gowl exit with the compositor alive, and
 * the unit tests never start one, so gowl_compositor_finalize() after a
 * real start ran nowhere -- until something did, and it aborted.
 * wlroots asserts that a signal has no listeners left when the object
 * owning it is destroyed, and the compositor left every one of its own
 * on: wlr_xwayland_destroy() was the first to notice, in a harness that
 * started a headless compositor and unreffed it.  cmacs finalizes one
 * too, in `gowl-stop' and when a start fails half way, where the abort
 * takes Emacs down with it.
 *
 * So this starts a real compositor -- headless backend, pixman renderer,
 * a private runtime directory, systemd off -- and finalizes it, in a
 * subprocess, so that an abort is a failed test rather than a dead suite.
 *
 * Then the modules.  The compositor only borrows its module manager, so
 * the manager is released after the compositor is gone -- main() does
 * it in that order -- and its dispose deactivates every module that is
 * still active.  A module that disconnects from the compositor in
 * deactivate therefore needs a pointer that GObject clears when the
 * compositor goes.  windowrules kept a plain one, and every teardown
 * with it loaded ended in two GLib criticals: g_signal_handler_disconnect()
 * on freed memory.  alpha did the same whenever shutdown had not been
 * dispatched first, because only its shutdown handler cleared the
 * pointer, and nothing makes an embedder dispatch shutdown -- cmacs's
 * `gowl-stop' does not.  So the module cases load everything cmacs
 * --gowl loads, start a compositor under it, and tear it down both ways;
 * a critical anywhere fails them.  No client is mapped, so what a module
 * keeps per client is out of their reach.
 */

#include <glib.h>
#include <glib/gstdio.h>

#include "core/gowl-compositor.h"
#include "core/gowl-layout-registry.h"
#include "config/gowl-config.h"
#include "module/gowl-module.h"
#include "module/gowl-module-manager.h"

#ifndef GOWL_TEST_MODULE_DIR
#define GOWL_TEST_MODULE_DIR "build/release/modules"
#endif

/* What cmacs --gowl loads: the wallpaper, then names[] in
 * cmacs/gowl/cmacs-gowl.c, in that order.  The bar, which cmacs enables
 * on its own, cannot be loaded in a test: its shipped widgets poll the
 * machine the test runs on.  The Makefile's TEARDOWN_MODULES names the
 * same modules, so that they are built before this runs. */
static const gchar *const cmacs_modules[] = {
	"wallpaper",
	"tile", "monocle", "float", "scrolling", "animation", "cube",
	"expo", "switcher", "magnifier", "blur", "layout-indicator",
	"alpha", "vanitygaps", "roundcorners", "windowrules", "dropdown",
	"scratchpad", "screenshot", "osd", "clipboard"
};

/* How a module case tears down. */
typedef struct {
	gboolean dispatch_shutdown;
} Teardown;

/* The order main() uses: shutdown while the compositor is alive, then
 * the compositor, then the manager. */
static const Teardown shutdown_first = { TRUE };

/* The same without the shutdown dispatch: cmacs's `gowl-stop' finalizes
 * the compositor without one. */
static const Teardown without_shutdown = { FALSE };

/* Nothing may reach the session this runs in: no systemd user targets,
 * and a runtime directory of its own, made inside the real one so its
 * path stays under the 108 bytes a socket path gets.  The user's state
 * directory moves in there too, because the clipboard module makes its
 * store under it at startup.  Returns the runtime directory. */
static gchar *
isolate(void)
{
	const gchar *parent;
	gchar       *runtime;
	gchar       *state;

	/* A critical is what this is looking for.  A warning about the
	 * environment -- no Xwayland binary, say -- is not a teardown
	 * failure, so it does not abort here. */
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	parent = g_getenv("XDG_RUNTIME_DIR");
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	runtime = g_build_filename(parent, "gowl-teardown-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(runtime, 0700));
	state = g_build_filename(runtime, "state", NULL);

	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", runtime, TRUE);
	g_setenv("XDG_STATE_HOME", state, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);
	g_free(state);
	return runtime;
}

/* Removes @runtime, which is empty by now but for the clipboard's store:
 * the display took its socket and lock file with it, and nothing was
 * copied, so the store is an empty directory. */
static void
clean_up(const gchar *runtime)
{
	static const gchar *const store[] = {
		"state/gowl/clipboard", "state/gowl", "state"
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(store); i++) {
		gchar *path;

		path = g_build_filename(runtime, store[i], NULL);
		if (g_file_test(path, G_FILE_TEST_IS_DIR))
			g_assert_cmpint(g_rmdir(path), ==, 0);
		g_free(path);
	}
	g_assert_cmpint(g_rmdir(runtime), ==, 0);
}

static GowlCompositor *
start(GowlConfig *config, GowlModuleManager *modules)
{
	GowlCompositor *compositor;
	GError         *error;

	error = NULL;
	compositor = gowl_compositor_new();
	gowl_compositor_set_config(compositor, config);
	gowl_compositor_set_module_manager(compositor, modules);
	if (!gowl_compositor_start(compositor, &error))
		g_error("could not start a headless compositor: %s",
		        error->message);
	return compositor;
}

/* The first child: start one, finalize it, and say nothing if that went
 * well. */
static void
start_and_finalize(void)
{
	g_autofree gchar  *runtime = NULL;
	GowlCompositor    *compositor;
	GowlConfig        *config;
	GowlModuleManager *modules;

	runtime = isolate();
	config = gowl_config_new();
	modules = gowl_module_manager_new();
	compositor = start(config, modules);

	/* What is under test. */
	g_object_unref(compositor);

	g_object_unref(modules);
	g_object_unref(config);
	clean_up(runtime);
}

/* The module children: everything cmacs --gowl loads, a compositor
 * started under it, and a teardown that releases the manager last,
 * dispatching shutdown first when @how says so. */
static void
modules_then_release(const Teardown *how)
{
	g_autofree gchar  *runtime = NULL;
	GowlCompositor    *compositor;
	GowlModuleManager *modules;
	GowlConfig        *config;
	gsize              i;

	runtime = isolate();

	/* Loaded and activated before there is a compositor, as main() and
	 * cmacs both do it. */
	modules = gowl_module_manager_new();
	for (i = 0; i < G_N_ELEMENTS(cmacs_modules); i++) {
		g_autofree gchar *path = NULL;
		GError           *error;

		path = g_strdup_printf("%s/%s.so", GOWL_TEST_MODULE_DIR,
		                       cmacs_modules[i]);
		error = NULL;
		if (!gowl_module_manager_load_module(modules, path, &error))
			g_error("%s", error->message);
	}
	gowl_module_manager_activate_all(modules);

	config = gowl_config_new();
	compositor = start(config, modules);
	gowl_module_manager_dispatch_startup(modules, compositor);
	gowl_layout_adopt_providers(compositor);

	/* Every one of them is up, so every one of them is deactivated
	 * below: a module that never came up would pass by never being
	 * torn down. */
	for (i = 0; i < G_N_ELEMENTS(cmacs_modules); i++) {
		GowlModule *module;

		module = gowl_module_manager_find_module(modules,
		                                         cmacs_modules[i]);
		g_assert_nonnull(module);
		g_assert_true(gowl_module_get_is_active(module));
	}

	/* What is under test: once the compositor is finalized, the
	 * manager's dispose deactivates every module with it gone. */
	if (how->dispatch_shutdown)
		gowl_module_manager_dispatch_shutdown(modules, compositor);
	g_object_unref(compositor);
	g_object_unref(modules);

	g_object_unref(config);
	clean_up(runtime);
}

static void
test_start_then_finalize(void)
{
	if (g_test_subprocess()) {
		start_and_finalize();
		return;
	}
	g_test_trap_subprocess(NULL, 60 * G_USEC_PER_SEC,
	                       G_TEST_SUBPROCESS_INHERIT_STDERR);
	g_test_trap_assert_passed();
}

static void
test_modules_then_release(gconstpointer data)
{
	gsize i;

	if (g_test_subprocess()) {
		modules_then_release((const Teardown *)data);
		return;
	}
	for (i = 0; i < G_N_ELEMENTS(cmacs_modules); i++) {
		gchar    *path;
		gboolean  built;

		path = g_strdup_printf("%s/%s.so", GOWL_TEST_MODULE_DIR,
		                       cmacs_modules[i]);
		built = g_file_test(path, G_FILE_TEST_EXISTS);
		g_free(path);
		if (!built) {
			g_test_skip("the modules are not built; run `make' first");
			return;
		}
	}
	g_test_trap_subprocess(NULL, 60 * G_USEC_PER_SEC,
	                       G_TEST_SUBPROCESS_INHERIT_STDERR);
	g_test_trap_assert_passed();
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/compositor-teardown/start-then-finalize",
	                test_start_then_finalize);
	g_test_add_data_func("/compositor-teardown/modules/shutdown-first",
	                     &shutdown_first, test_modules_then_release);
	g_test_add_data_func("/compositor-teardown/modules/without-shutdown",
	                     &without_shutdown, test_modules_then_release);

	return g_test_run();
}
