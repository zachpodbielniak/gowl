/* test-crt-frames.c -- the tube must not stop the desktop drawing
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * modules/crt parks one opaque, monitor-sized sheet over everything and
 * re-captures the screen into it every frame.  Both halves of that are
 * hostile to the machinery that decides WHEN to draw, and each has a
 * failure mode that looks nothing like a graphics bug.
 *
 *   A SHEET THAT OCCLUDES STOPS THE DESKTOP.  wlr_scene culls damage
 *   from nodes it can prove are hidden and sends frame callbacks only to
 *   surfaces that are visible -- both keyed on the opaque regions of the
 *   nodes above.  A full-screen opaque sheet parked permanently
 *   therefore tells every window on the machine that it cannot be seen:
 *   their damage stops scheduling frames and their frame callbacks stop
 *   arriving, so they stop drawing.  What is on screen is then a
 *   PHOTOGRAPH of the desktop taken at the moment the tube came on, and
 *   the compositor is idle -- no CPU, no GPU, nothing in any log.  It
 *   reads as "Emacs has hung", and Emacs is fine: it is drawing into a
 *   scene nobody is compositing.
 *
 *   The other sheets in this tree get away with it because they are up
 *   for a second.  This one is up for as long as somebody likes it.
 *
 *   A SHEET THAT REDRAWS UNCONDITIONALLY NEVER SLEEPS.  The capture
 *   forces whole damage on the output so that the frame after it draws
 *   everything, which means an effect that asks for another frame every
 *   time is asking forever: two full scene renders, a wide blur and a
 *   shader pass at the refresh rate, on a desktop where nothing is
 *   moving.  On a laptop that is the battery.
 *
 * Both are asserted through wlr_scene_output_needs_frame(), which is the
 * compositor's own question -- "is there anything to draw?" -- and is
 * exactly what goes wrong in each case.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include <wayland-server-core.h>

#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-effects.h"
#include "config/gowl-config.h"
#include "module/gowl-module-manager.h"

typedef struct {
	gchar                 *parent;
	gchar                 *runtime;
	GowlModuleManager     *modules;
	GowlConfig            *config;
	GowlCompositor        *compositor;
	gboolean               started;
	struct wlr_scene_rect *below;   /* a node under the sheet */
} Rig;

static gboolean
rig_up(Rig *r)
{
	const gchar *parent;
	GError      *error = NULL;
	gchar       *path;
	gfloat       colour[4] = { 0.2f, 0.4f, 0.8f, 1.0f };

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	/* Nothing may reach the session this runs in. */
	r->parent = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	parent = r->parent;
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-crt-frames-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "gles2", TRUE);

	r->modules = gowl_module_manager_new();
	path = g_build_filename(GOWL_TEST_MODULE_DIR, "crt.so", NULL);
	if (!gowl_module_manager_load_module(r->modules, path, &error))
		g_error("could not load %s: %s", path, error->message);
	g_free(path);
	gowl_module_manager_activate_all(r->modules);

	r->config = gowl_config_new();
	gowl_config_set_crt(r->config, TRUE);
	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_clear_error(&error);
		return FALSE;
	}
	r->started = TRUE;
	gowl_module_manager_dispatch_startup(r->modules, r->compositor);
	if (!wlr_renderer_is_gles2(r->compositor->renderer))
		return FALSE;
	g_assert_nonnull(r->compositor->selmon);

	/*
	 * Something under the sheet that can be damaged.  A plain rect
	 * rather than a client, because the question is not about clients:
	 * it is about whether the scene still believes anything below the
	 * tube is worth drawing.
	 */
	r->below = wlr_scene_rect_create(
		r->compositor->layers[GOWL_SCENE_LAYER_TILE], 400, 300, colour);
	g_assert_nonnull(r->below);
	wlr_scene_node_set_position(&r->below->node, 100, 100);
	return TRUE;
}

static void
rig_down(Rig *r)
{
	if (r->below != NULL)
		wlr_scene_node_destroy(&r->below->node);
	if (r->started)
		gowl_module_manager_dispatch_shutdown(r->modules, r->compositor);
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	if (r->started)
		g_assert_cmpint(g_rmdir(r->runtime), ==, 0);
	else
		g_rmdir(r->runtime);
	g_free(r->runtime);
	if (r->parent != NULL)
		g_setenv("XDG_RUNTIME_DIR", r->parent, TRUE);
	else
		g_unsetenv("XDG_RUNTIME_DIR");
	g_free(r->parent);
}

/* One turn of the compositor's own frame loop: the effects, then the
 * commit that consumes whatever damage they left. */
static gboolean
turn(Rig *r, gint64 now)
{
	GowlMonitor            *m = r->compositor->selmon;
	struct wlr_output_state state;
	gboolean                live;

	live = gowl_effects_frame(r->compositor, m, now);

	if (wlr_scene_output_needs_frame(m->scene_output)) {
		wlr_output_state_init(&state);
		if (wlr_scene_output_build_state(m->scene_output, &state, NULL))
			wlr_output_commit_state(m->wlr_output, &state);
		wlr_output_state_finish(&state);
	}
	return live;
}

/* ── The desktop must keep drawing ───────────────────────────────── */

static void
test_the_tube_does_not_hide_the_desktop_from_the_scene(void)
{
	Rig     r;
	gint64  now = g_get_monotonic_time();
	gfloat  other[4] = { 0.9f, 0.1f, 0.1f, 1.0f };

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no GLES2 renderer to draw with here");
		return;
	}

	/* Two turns: the first builds the sheet, the second settles it. */
	turn(&r, now);
	turn(&r, now + 16000);

	/* Quiet: whatever the tube left has been committed. */
	while (wlr_scene_output_needs_frame(r.compositor->selmon->scene_output))
		turn(&r, (now += 16000));

	/*
	 * Now something under the sheet changes, which is a window
	 * redrawing.  The scene MUST want a frame for it.
	 *
	 * If the sheet is opaque and covers the output, wlr_scene knows the
	 * rect cannot be seen and throws the damage away -- and the screen
	 * stays as it was, for ever, at no CPU cost.  That is the whole bug:
	 * the desktop is still running and nothing it draws reaches the
	 * glass.
	 */
	wlr_scene_rect_set_color(r.below, other);
	g_assert_true(wlr_scene_output_needs_frame(
		r.compositor->selmon->scene_output));

	rig_down(&r);
}

/* ── ...and the tube must not spin ───────────────────────────────── */

static void
test_a_still_desktop_stops_asking_for_frames(void)
{
	Rig    r;
	gint64 now = g_get_monotonic_time();
	gint   i;
	gboolean live = TRUE;

	if (!rig_up(&r)) {
		rig_down(&r);
		g_test_skip("no GLES2 renderer to draw with here");
		return;
	}

	/*
	 * Turn the loop until it settles.  Twenty is a long way past what a
	 * tube needs to reach a steady picture and a long way short of
	 * "forever", which is what the failure looks like.
	 */
	for (i = 0; i < 20 && live; i++)
		live = turn(&r, now + (gint64)i * 16000);

	/*
	 * Nothing is moving, so the module must stop asking.  Returning TRUE
	 * from the frame hook is what keeps an output awake, and an output
	 * kept awake by the tube renders the whole scene twice, blurs it and
	 * runs a shader over it sixty times a second to show a picture that
	 * has not changed.
	 */
	g_assert_false(live);
	g_assert_false(wlr_scene_output_needs_frame(
		r.compositor->selmon->scene_output));

	rig_down(&r);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/crt-frames/the-desktop-still-draws",
	                test_the_tube_does_not_hide_the_desktop_from_the_scene);
	g_test_add_func("/crt-frames/a-still-desktop-sleeps",
	                test_a_still_desktop_stops_asking_for_frames);

	return g_test_run();
}
