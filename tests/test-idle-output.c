/* test-idle-output.c -- an output with nothing to draw stops drawing
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * on_monitor_frame() used to build and commit a frame on every frame
 * event.  wlroots renders and attaches a buffer whether or not anything
 * changed, and on DRM every committed buffer is a page flip that ends in
 * another frame event, so an idle output redrew at full refresh forever:
 * wakeups and power for nothing.  The one-shot wlr_scene_output_commit()
 * it replaced had skipped a frame with nothing to draw; the explicit
 * commit the gamma ramp needed dropped that check.
 *
 * The headless backend loops the same way -- every commit arms its frame
 * timer -- so each case starts a real compositor (headless, pixman, a
 * private runtime directory, systemd off, in a subprocess so an abort is
 * a failed test) and watches the output's commit count:
 *
 *  - left alone it stops moving: nothing changed, nothing is drawn;
 *  - a frame event with nothing to draw still runs the effect hooks and
 *    frame callbacks, but commits nothing and emits no ::frame-rendered;
 *  - it moves again for everything that should draw: a scheduled frame,
 *    a bare needs_frame (how a screencopy asks), damage, a gamma ramp;
 *  - an effect still animating keeps it drawing, and so does whatever
 *    asks for its next frame from ::frame-rendered -- how the recording
 *    module gets a steady stream from an unchanging screen.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "interfaces/gowl-scene-effect.h"
#include "module/gowl-module-manager.h"

/* ── A scene effect that counts its frames ─────────────────────── */

G_DECLARE_FINAL_TYPE(IdleEffect, idle_effect, IDLE, EFFECT, GowlModule)
struct _IdleEffect {
	GowlModule parent_instance;
	gint       frame;        /* frame hooks run */
	gint       frame_done;   /* frame_done hooks run */
	gboolean   live;         /* answer "still animating" */
};

static gboolean
ie_frame(GowlSceneEffect *e, GowlCompositor *c, GowlMonitor *m, gint64 now)
{
	IdleEffect *self = IDLE_EFFECT(e);

	self->frame++;
	return self->live;
}

static void
ie_frame_done(GowlSceneEffect *e, GowlCompositor *c, GowlMonitor *m,
              const struct timespec *now)
{
	IDLE_EFFECT(e)->frame_done++;
}

static void
ie_effect_init(GowlSceneEffectInterface *iface)
{
	iface->frame      = ie_frame;
	iface->frame_done = ie_frame_done;
}

G_DEFINE_TYPE_WITH_CODE(IdleEffect, idle_effect, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, ie_effect_init))

static const gchar *ie_name(GowlModule *m) { return "idle-effect"; }
static gboolean ie_activate(GowlModule *m) { return TRUE; }
static void idle_effect_init(IdleEffect *self) { }
static void
idle_effect_class_init(IdleEffectClass *klass)
{
	GOWL_MODULE_CLASS(klass)->get_name = ie_name;
	GOWL_MODULE_CLASS(klass)->activate = ie_activate;
}

/* ── A compositor to watch ───────────────────────────────────────── */

typedef struct {
	gchar             *runtime;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
	GowlMonitor       *monitor;
	struct wlr_output *output;
	IdleEffect        *effect;
	guint              rendered;   /* ::frame-rendered emissions */
} Idle;

static void
on_rendered(GowlCompositor *compositor, GObject *monitor, gpointer data)
{
	((Idle *)data)->rendered++;
}

/* Nothing may reach the session this runs in: no systemd user targets,
 * and a runtime directory of its own, made inside the real one so its
 * path stays under the 108 bytes a socket path gets. */
static void
idle_start(Idle *t)
{
	const gchar *parent;
	GError      *error;

	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);
	memset(t, 0, sizeof(*t));

	parent = g_getenv("XDG_RUNTIME_DIR");
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	t->runtime = g_build_filename(parent, "gowl-idle-output-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(t->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", t->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	t->config = gowl_config_new();
	t->modules = gowl_module_manager_new();
	g_assert_true(gowl_module_manager_register(t->modules,
	                                            idle_effect_get_type(),
	                                            NULL));
	t->effect = IDLE_EFFECT(gowl_module_manager_find_module(t->modules,
	                                                        "idle-effect"));
	g_assert_nonnull(t->effect);
	g_assert_true(gowl_module_activate(GOWL_MODULE(t->effect)));

	error = NULL;
	t->compositor = gowl_compositor_new();
	gowl_compositor_set_config(t->compositor, t->config);
	gowl_compositor_set_module_manager(t->compositor, t->modules);
	if (!gowl_compositor_start(t->compositor, &error))
		g_error("could not start a headless compositor: %s",
		        error->message);
	g_assert_cmpuint(g_list_length(t->compositor->monitors), ==, 1);
	t->monitor = (GowlMonitor *)t->compositor->monitors->data;
	t->output = t->monitor->wlr_output;
	g_signal_connect(t->compositor, "frame-rendered",
	                 G_CALLBACK(on_rendered), t);
}

static void
idle_stop(Idle *t)
{
	g_object_unref(t->compositor);
	g_object_unref(t->modules);
	g_object_unref(t->config);
	g_assert_cmpint(g_rmdir(t->runtime), ==, 0);
	g_free(t->runtime);
}

/* Run the loop until the output has gone 200 ms without a commit.  An
 * idle output that never gets there is the bug this file is about. */
static void
assert_settles(Idle *t)
{
	gint64  deadline;
	gint64  last_change;
	guint32 seq;

	deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
	last_change = g_get_monotonic_time();
	seq = t->output->commit_seq;
	while (g_get_monotonic_time() < deadline) {
		wl_event_loop_dispatch(t->compositor->event_loop, 5);
		if (t->output->commit_seq != seq) {
			seq = t->output->commit_seq;
			last_change = g_get_monotonic_time();
		} else if (g_get_monotonic_time() - last_change
		           >= 200 * 1000) {
			return;
		}
	}
	g_error("the output was still committing after 3 s with nothing "
	        "to draw: an idle output is being redrawn every frame");
}

/* Run the loop until the output commits, for at most a second. */
static gboolean
await_commit(Idle *t)
{
	gint64  deadline;
	guint32 seq;

	deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
	seq = t->output->commit_seq;
	while (g_get_monotonic_time() < deadline) {
		wl_event_loop_dispatch(t->compositor->event_loop, 5);
		if (t->output->commit_seq != seq)
			return TRUE;
	}
	return FALSE;
}

/* Run the loop for @ms, whatever happens. */
static void
run_for(Idle *t, gint ms)
{
	gint64 deadline;

	deadline = g_get_monotonic_time() + (gint64)ms * 1000;
	while (g_get_monotonic_time() < deadline)
		wl_event_loop_dispatch(t->compositor->event_loop, 5);
}

/* ── The cases ───────────────────────────────────────────────────── */

/* Startup draws; then nothing changes, and nothing more is drawn. */
static void
stops_drawing(void)
{
	Idle    t;
	guint32 seq;

	idle_start(&t);
	assert_settles(&t);
	seq = t.output->commit_seq;
	run_for(&t, 300);
	g_assert_cmpuint(t.output->commit_seq, ==, seq);
	g_assert_false(t.output->frame_pending);
	idle_stop(&t);
}

/* A frame event on an output with nothing to draw -- what DRM sends after
 * the page flip of the last frame that changed anything -- still runs
 * the effect hooks and sends frame callbacks, but draws nothing. */
static void
frame_with_nothing_to_draw(void)
{
	Idle    t;
	guint32 seq;
	gint    frame;
	gint    frame_done;
	guint   rendered;

	idle_start(&t);
	assert_settles(&t);
	seq = t.output->commit_seq;
	frame = t.effect->frame;
	frame_done = t.effect->frame_done;
	rendered = t.rendered;

	wlr_output_send_frame(t.output);
	g_assert_cmpint(t.effect->frame, ==, frame + 1);
	g_assert_cmpint(t.effect->frame_done, ==, frame_done + 1);
	g_assert_cmpuint(t.output->commit_seq, ==, seq);
	g_assert_cmpuint(t.rendered, ==, rendered);

	/* And having drawn nothing, it asks for nothing more. */
	run_for(&t, 100);
	g_assert_cmpuint(t.output->commit_seq, ==, seq);
	idle_stop(&t);
}

/* Everything that should draw still does. */
static void
draws_on_demand(void)
{
	static const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
	struct wlr_scene_rect *rect;
	Idle                   t;
	guint                  rendered;

	idle_start(&t);
	assert_settles(&t);

	/* A frame asked for, and ::frame-rendered with it. */
	rendered = t.rendered;
	wlr_output_schedule_frame(t.output);
	g_assert_true(await_commit(&t));
	g_assert_cmpuint(t.rendered, >, rendered);
	assert_settles(&t);

	/* A bare needs_frame: all a screencopy, an export-dmabuf or a
	 * software cursor does.  The scene turns it into a scheduled frame,
	 * which is the only reason grim still works on an idle screen. */
	wlr_output_update_needs_frame(t.output);
	g_assert_true(await_commit(&t));
	assert_settles(&t);

	/* Damage, both ways. */
	rect = wlr_scene_rect_create(t.compositor->layers[GOWL_SCENE_LAYER_TOP],
	                             64, 64, red);
	g_assert_true(await_commit(&t));
	assert_settles(&t);
	wlr_scene_node_destroy(&rect->node);
	g_assert_true(await_commit(&t));
	assert_settles(&t);

	/* A gamma ramp rides a frame of its own, nothing else changed or
	 * not.  Whether the headless backend accepts the colour transform
	 * does not matter here: the frame is built for it either way. */
	t.monitor->gamma_dirty = TRUE;
	wlr_output_send_frame(t.output);
	g_assert_false(t.monitor->gamma_dirty);
	idle_stop(&t);
}

static void
ask_again(GowlCompositor *compositor, GObject *monitor, gpointer data)
{
	wlr_output_schedule_frame(
		gowl_monitor_get_wlr_output(GOWL_MONITOR(monitor)));
}

/* An effect still animating keeps its output drawing, and so does
 * whatever asks for the next frame from each ::frame-rendered. */
static void
keeps_drawing_when_asked(void)
{
	Idle    t;
	guint32 seq;
	gint    frame;
	guint   rendered;
	gulong  id;

	idle_start(&t);
	assert_settles(&t);

	t.effect->live = TRUE;
	seq = t.output->commit_seq;
	frame = t.effect->frame;
	wlr_output_schedule_frame(t.output);
	run_for(&t, 250);
	g_assert_cmpuint(t.output->commit_seq - seq, >=, 5);
	g_assert_cmpint(t.effect->frame - frame, >=, 5);
	t.effect->live = FALSE;
	assert_settles(&t);

	id = g_signal_connect(t.compositor, "frame-rendered",
	                      G_CALLBACK(ask_again), NULL);
	rendered = t.rendered;
	wlr_output_schedule_frame(t.output);
	run_for(&t, 250);
	g_assert_cmpuint(t.rendered - rendered, >=, 5);
	g_signal_handler_disconnect(t.compositor, id);
	assert_settles(&t);
	idle_stop(&t);
}

/* ── Each case in a subprocess of its own ────────────────────────── */

static void
in_subprocess(void (*body)(void))
{
	if (g_test_subprocess()) {
		body();
		return;
	}
	g_test_trap_subprocess(NULL, 60 * G_USEC_PER_SEC,
	                       G_TEST_SUBPROCESS_INHERIT_STDERR);
	g_test_trap_assert_passed();
}

static void test_stops_drawing(void) { in_subprocess(stops_drawing); }
static void test_nothing_to_draw(void)
{
	in_subprocess(frame_with_nothing_to_draw);
}
static void test_draws_on_demand(void) { in_subprocess(draws_on_demand); }
static void test_keeps_drawing(void)
{
	in_subprocess(keeps_drawing_when_asked);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/idle-output/stops-drawing", test_stops_drawing);
	g_test_add_func("/idle-output/frame-with-nothing-to-draw",
	                test_nothing_to_draw);
	g_test_add_func("/idle-output/draws-on-demand", test_draws_on_demand);
	g_test_add_func("/idle-output/keeps-drawing-when-asked",
	                test_keeps_drawing);

	return g_test_run();
}
