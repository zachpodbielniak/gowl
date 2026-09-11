/* test-compositor-teardown.c -- a compositor that was started can be
 * finalized
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
 */

#include <glib.h>
#include <glib/gstdio.h>

#include "core/gowl-compositor.h"
#include "config/gowl-config.h"
#include "module/gowl-module-manager.h"

/* The child: start one, finalize it, and say nothing if that went well. */
static void
start_and_finalize(void)
{
	g_autofree gchar  *runtime = NULL;
	GowlCompositor    *compositor;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GError            *error;
	const gchar       *parent;

	/* A critical is what this is looking for.  A warning about the
	 * environment -- no Xwayland binary, say -- is not a teardown
	 * failure, so it does not abort here. */
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	/* Nothing may reach the session this runs in: no systemd user
	 * targets, and a runtime directory of its own, made inside the real
	 * one so its path stays under the 108 bytes a socket path gets. */
	parent = g_getenv("XDG_RUNTIME_DIR");
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	runtime = g_build_filename(parent, "gowl-teardown-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	error = NULL;
	config = gowl_config_new();
	modules = gowl_module_manager_new();
	compositor = gowl_compositor_new();
	gowl_compositor_set_config(compositor, config);
	gowl_compositor_set_module_manager(compositor, modules);
	if (!gowl_compositor_start(compositor, &error))
		g_error("could not start a headless compositor: %s",
		        error->message);

	/* What is under test. */
	g_object_unref(compositor);

	g_object_unref(modules);
	g_object_unref(config);

	/* The display took its socket and lock file with it. */
	g_assert_cmpint(g_rmdir(runtime), ==, 0);
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

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/compositor-teardown/start-then-finalize",
	                test_start_then_finalize);

	return g_test_run();
}
