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
 * Which tag transition runs, and that only one does.
 *
 * Sixteen transition modules can be loaded at once and each watches every
 * tag switch on its own.  What keeps them from all drawing at once is the
 * arbitration in src/core/gowl-tag-transition.c, and its failure modes
 * are all silent -- two sheets fighting over an output, a "random" that
 * picks differently for each module that asks, a tour of Super+: that
 * differs with module load order -- so each is pinned here.
 *
 * The first half needs only a headless compositor.  The second drives two
 * GowlFxTransitionHosts with fake kinds against a real GLES2 renderer
 * (skipped without a render node), the way two loaded modules would.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include <wlr/render/gles2.h>

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "config/gowl-config.h"
#include "fx/gowl-fx-transition.h"

typedef struct {
	gchar          *runtime;
	gchar          *parent;
	GowlConfig     *config;
	GowlCompositor *compositor;
	gint            toasts;
	gchar          *last_toast;
} Fx;

static void
on_toast(GowlCompositor *self, GowlMonitor *m, const gchar *text, gpointer data)
{
	Fx *f = data;

	(void)self;
	(void)m;
	f->toasts++;
	g_free(f->last_toast);
	f->last_toast = g_strdup(text);
}

static gboolean
fx_up(Fx *f, const gchar *renderer)
{
	const gchar *parent;
	GError *error = NULL;

	memset(f, 0, sizeof *f);
	f->parent = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	parent = f->parent;
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	f->runtime = g_build_filename(parent, "gowl-tag-transition-XXXXXX",
	                              NULL);
	g_assert_nonnull(g_mkdtemp_full(f->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", f->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", renderer, TRUE);

	f->config = gowl_config_new();
	f->compositor = gowl_compositor_new();
	gowl_compositor_set_config(f->compositor, f->config);
	if (!gowl_compositor_start(f->compositor, &error)) {
		g_clear_error(&error);
		return FALSE;
	}
	g_signal_connect(f->compositor, "toast-requested",
	                 G_CALLBACK(on_toast), f);
	return f->compositor->selmon != NULL;
}

static void
fx_down(Fx *f)
{
	g_clear_object(&f->compositor);
	g_clear_object(&f->config);
	if (f->runtime != NULL)
		g_rmdir(f->runtime);
	if (f->parent != NULL)
		g_setenv("XDG_RUNTIME_DIR", f->parent, TRUE);
	g_free(f->parent);
	g_free(f->runtime);
	g_free(f->last_toast);
}

/* Switch @m to @tags the way a keybind does. */
static void
view(Fx *f, guint32 tags)
{
	gowl_compositor_view_tags(f->compositor, f->compositor->selmon, tags);
}

/* ── The selector ────────────────────────────────────────────────── */

/* The tour is a fixed order, whatever order the modules loaded in. */
static void
test_order_is_canonical(void)
{
	Fx f;
	g_auto(GStrv) names = NULL;

	if (!fx_up(&f, "pixman")) {
		fx_down(&f);
		g_test_skip("no headless compositor here");
		return;
	}
	gowl_compositor_register_tag_transition(f.compositor, "sphere", "Sphere");
	gowl_compositor_register_tag_transition(f.compositor, "thirdparty", "X");
	gowl_compositor_register_tag_transition(f.compositor, "cube", "Cube");
	gowl_compositor_register_tag_transition(f.compositor, "slide", "Slide");
	/* Idempotent: a second registration is not a second entry. */
	gowl_compositor_register_tag_transition(f.compositor, "slide", "Slide");

	names = gowl_compositor_list_tag_transitions(f.compositor);
	g_assert_cmpuint(g_strv_length(names), ==, 4);
	g_assert_cmpstr(names[0], ==, "cube");
	g_assert_cmpstr(names[1], ==, "slide");
	g_assert_cmpstr(names[2], ==, "sphere");
	/* Not in the canonical list: after it. */
	g_assert_cmpstr(names[3], ==, "thirdparty");
	fx_down(&f);
}

/* Super+: steps round the loaded ones and "random", and says so. */
static void
test_tour_and_toast(void)
{
	Fx f;

	if (!fx_up(&f, "pixman")) {
		fx_down(&f);
		g_test_skip("no headless compositor here");
		return;
	}
	gowl_compositor_register_tag_transition(f.compositor, "cube", "Cube");
	gowl_compositor_register_tag_transition(f.compositor, "fade", "Fade");
	g_assert_cmpstr(gowl_compositor_get_tag_transition(f.compositor), ==,
	                "cube");

	gowl_compositor_cycle_tag_transition(f.compositor, 1);
	g_assert_cmpstr(gowl_compositor_get_tag_transition(f.compositor), ==,
	                "fade");
	g_assert_cmpint(f.toasts, ==, 1);
	g_assert_cmpstr(f.last_toast, ==, "Transition: Fade");

	gowl_compositor_cycle_tag_transition(f.compositor, 1);
	g_assert_cmpstr(gowl_compositor_get_tag_transition(f.compositor), ==,
	                "random");
	g_assert_cmpstr(f.last_toast, ==, "Transition: Random");

	/* And round again to the start. */
	gowl_compositor_cycle_tag_transition(f.compositor, 1);
	g_assert_cmpstr(gowl_compositor_get_tag_transition(f.compositor), ==,
	                "cube");

	/* Backwards from the first stop is "random". */
	gowl_compositor_cycle_tag_transition(f.compositor, -1);
	g_assert_cmpstr(gowl_compositor_get_tag_transition(f.compositor), ==,
	                "random");

	/* Selecting what is already in force is quiet: no toast. */
	{
		gint before = f.toasts;

		g_assert_true(gowl_compositor_set_tag_transition(f.compositor,
		                                                 "random"));
		g_assert_cmpint(f.toasts, ==, before);
	}

	/* A name nothing provides is refused, not silently instant. */
	g_assert_false(gowl_compositor_set_tag_transition(f.compositor, "nope"));
	g_assert_cmpstr(gowl_compositor_get_tag_transition(f.compositor), ==,
	                "random");
	g_assert_true(gowl_compositor_set_tag_transition(f.compositor, "none"));
	g_assert_null(gowl_compositor_tag_transition_pick(f.compositor,
	                                                  f.compositor->selmon));

	/* The action, as Super+: runs it. */
	{
		GowlKeybindEntry kb;

		memset(&kb, 0, sizeof kb);
		kb.action = GOWL_ACTION_CYCLE_TAG_TRANSITION;
		g_assert_true(gowl_compositor_run_keybind_entry(f.compositor, &kb));
		g_assert_cmpstr(gowl_compositor_get_tag_transition(f.compositor),
		                ==, "cube");
	}
	fx_down(&f);
}

/*
 * "random" is decided once per switch: every module asking about the
 * same switch hears the same name, and the next switch is a new draw --
 * never the same one twice running when there is a choice.
 */
static void
test_random_is_one_decision_per_switch(void)
{
	Fx f;
	GowlMonitor *m;
	gchar *prev = NULL;
	gint i;

	if (!fx_up(&f, "pixman")) {
		fx_down(&f);
		g_test_skip("no headless compositor here");
		return;
	}
	m = f.compositor->selmon;
	gowl_compositor_register_tag_transition(f.compositor, "cube", "Cube");
	gowl_compositor_register_tag_transition(f.compositor, "fade", "Fade");
	gowl_compositor_register_tag_transition(f.compositor, "slide", "Slide");
	g_assert_true(gowl_compositor_set_tag_transition(f.compositor, "random"));

	for (i = 0; i < 60; i++) {
		const gchar *a, *b;

		view(&f, (i % 2 == 0) ? 2 : 1);
		a = gowl_compositor_tag_transition_pick(f.compositor, m);
		b = gowl_compositor_tag_transition_pick(f.compositor, m);
		g_assert_nonnull(a);
		/* The same switch, asked twice: one answer. */
		g_assert_cmpstr(a, ==, b);
		g_assert_true(gowl_compositor_tag_transition_is(f.compositor, m, a));
		if (prev != NULL)
			g_assert_cmpstr(a, !=, prev);
		g_free(prev);
		prev = g_strdup(a);
	}
	g_free(prev);

	/* A selection no loaded module provides animates nothing. */
	gowl_compositor_unregister_tag_transition(f.compositor, "fade");
	gowl_config_set_tag_transition(f.config, "fade");
	view(&f, 4);
	g_assert_null(gowl_compositor_tag_transition_pick(f.compositor, m));
	fx_down(&f);
}

/* The socket, both dialects. */
static void
test_ipc_words(void)
{
	Fx f;
	g_autofree gchar *r = NULL;

	if (!fx_up(&f, "pixman")) {
		fx_down(&f);
		g_test_skip("no headless compositor here");
		return;
	}
	gowl_compositor_register_tag_transition(f.compositor, "cube", "Cube");
	gowl_compositor_register_tag_transition(f.compositor, "melt", "Melt");

	r = gowl_compositor_ipc_command(f.compositor, "tag-transition");
	g_assert_cmpstr(r, ==, "OK cube");
	g_clear_pointer(&r, g_free);
	r = gowl_compositor_ipc_command(f.compositor, "tag-transition next");
	g_assert_cmpstr(r, ==, "OK melt");
	g_clear_pointer(&r, g_free);
	r = gowl_compositor_ipc_command(f.compositor, "tag-transition list");
	g_assert_cmpstr(r, ==, "[\"cube\",\"melt\",\"random\"]");
	g_clear_pointer(&r, g_free);
	r = gowl_compositor_ipc_command(f.compositor, "tag-transition bogus");
	g_assert_true(g_str_has_prefix(r, "ERROR"));
	fx_down(&f);
}

/* ── Two hosts, one switch ───────────────────────────────────────── */

static gint draws_a, draws_b, setups_a;

static gboolean
setup_a(GowlFxTransitionRun *run)
{
	(void)run;
	setups_a++;
	return TRUE;
}

static gboolean
draw_a(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
       gdouble linear)
{
	(void)linear;
	draws_a++;
	g_assert_cmpfloat(eased, >=, 0.0);
	g_assert_cmpfloat(eased, <=, 1.0);
	gowl_fx_transition_pass_desktop(pass, &run->to, NULL, NULL, 1.0f, 1.0f);
	return TRUE;
}

static gboolean
draw_b(GowlFxPass *pass, GowlFxTransitionRun *run, gdouble eased,
       gdouble linear)
{
	(void)eased;
	(void)linear;
	draws_b++;
	gowl_fx_transition_pass_desktop(pass, &run->from, NULL, NULL, 1.0f, 1.0f);
	return TRUE;
}

static const GowlFxTransitionKind kind_a = {
	"fade", "Fade", 1.0, 0.0, FALSE, setup_a, NULL, draw_a
};
static const GowlFxTransitionKind kind_b = {
	"slide", "Slide", 1.0, 0.0, FALSE, NULL, NULL, draw_b
};

static void
frame(Fx *f, GowlFxTransitionHost *a, GowlFxTransitionHost *b, gint64 now)
{
	gowl_fx_transition_host_frame(a, f->compositor, f->compositor->selmon,
	                              now);
	gowl_fx_transition_host_frame(b, f->compositor, f->compositor->selmon,
	                              now);
}

static void
test_only_the_picked_one_runs(void)
{
	Fx f;
	GowlFxTransitionHost *a, *b;
	GowlMonitor *m;
	gint64 now;
	GowlClient *c;

	if (!fx_up(&f, "gles2")
	    || !wlr_renderer_is_gles2(f.compositor->renderer)) {
		fx_down(&f);
		g_test_skip("no GLES2 renderer here (no DRM render node?)");
		return;
	}
	m = f.compositor->selmon;
	a = gowl_fx_transition_host_new(&kind_a);
	b = gowl_fx_transition_host_new(&kind_b);
	gowl_fx_transition_host_attach(a, f.compositor);
	gowl_fx_transition_host_attach(b, f.compositor);
	g_assert_true(gowl_compositor_set_tag_transition(f.compositor, "fade"));
	draws_a = draws_b = setups_a = 0;

	/* First sight of the output: remembered, not animated. */
	now = g_get_monotonic_time();
	frame(&f, a, b, now);
	g_assert_null(gowl_fx_transition_host_running(a, m));

	/* A switch: the picked one runs, the other stands down. */
	view(&f, 2);
	/* Before the frame that starts it, the reveal is already the
	 * picked transition's -- and only its. */
	c = gowl_client_new();
	c->compositor = f.compositor;
	c->mon = m;
	g_assert_true(gowl_fx_transition_host_client_event(
		a, f.compositor, c, GOWL_SCENE_EFFECT_REVEAL));
	g_assert_false(gowl_fx_transition_host_client_event(
		b, f.compositor, c, GOWL_SCENE_EFFECT_REVEAL));
	/* Nothing but the reveal is ever claimed. */
	g_assert_false(gowl_fx_transition_host_client_event(
		a, f.compositor, c, GOWL_SCENE_EFFECT_GEOMETRY));

	frame(&f, a, b, now + 1000);
	g_assert_nonnull(gowl_fx_transition_host_running(a, m));
	g_assert_null(gowl_fx_transition_host_running(b, m));
	g_assert_cmpint(setups_a, ==, 1);
	g_assert_cmpint(draws_a, ==, 1);
	g_assert_cmpint(draws_b, ==, 0);
	{
		GowlFxTransitionRun *run = gowl_fx_transition_host_running(a, m);

		g_assert_cmpint(run->from_tags, ==, 1);
		g_assert_cmpint(run->to_tags, ==, 2);
		g_assert_cmpint(run->dir, ==, 1);
		g_assert_cmpint(run->from.tex, !=, 0);
		g_assert_cmpint(run->to.tex, !=, 0);
		g_assert_cmpint(run->dur_us, ==,
			(gint64)gowl_config_get_tag_transition_duration(f.config) * 1000);
	}

	/* It ends on its clock, and the output is the desktop again. */
	frame(&f, a, b, now + 1000 + 30 * G_USEC_PER_SEC);
	g_assert_null(gowl_fx_transition_host_running(a, m));

	/* Select the other: now it is b's turn, a's not. */
	g_assert_true(gowl_compositor_set_tag_transition(f.compositor, "slide"));
	view(&f, 1);
	frame(&f, a, b, now + 60 * G_USEC_PER_SEC);
	g_assert_null(gowl_fx_transition_host_running(a, m));
	g_assert_nonnull(gowl_fx_transition_host_running(b, m));
	g_assert_cmpint(gowl_fx_transition_host_running(b, m)->dir, ==, -1);
	g_assert_cmpint(draws_b, ==, 1);

	/* "none": nobody runs. */
	frame(&f, a, b, now + 90 * G_USEC_PER_SEC);
	g_assert_true(gowl_compositor_set_tag_transition(f.compositor, "none"));
	view(&f, 4);
	frame(&f, a, b, now + 91 * G_USEC_PER_SEC);
	g_assert_null(gowl_fx_transition_host_running(a, m));
	g_assert_null(gowl_fx_transition_host_running(b, m));

	/* Detaching withdraws the kind from the tour. */
	gowl_fx_transition_host_detach(a);
	{
		g_auto(GStrv) names = gowl_compositor_list_tag_transitions(
			f.compositor);

		g_assert_cmpuint(g_strv_length(names), ==, 1);
		g_assert_cmpstr(names[0], ==, "slide");
	}

	g_object_unref(c);
	gowl_fx_transition_host_free(a);
	gowl_fx_transition_host_free(b);
	fx_down(&f);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/tag-transition/order-is-canonical",
	                test_order_is_canonical);
	g_test_add_func("/tag-transition/tour-and-toast", test_tour_and_toast);
	g_test_add_func("/tag-transition/random-is-one-decision-per-switch",
	                test_random_is_one_decision_per_switch);
	g_test_add_func("/tag-transition/ipc-words", test_ipc_words);
	g_test_add_func("/tag-transition/only-the-picked-one-runs",
	                test_only_the_picked_one_runs);

	return g_test_run();
}
