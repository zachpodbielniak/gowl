/* test-overlay-adopt.c -- adopting a window as an overlay, and giving it back
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * gowl_compositor_adopt_overlay() and gowl_compositor_release_overlay()
 * are how a module such as the scratchpad takes a window out of the
 * layout and gives it back.  They run here against a compositor that was
 * never started -- no backend, no scene, no seat -- which is enough for
 * everything they decide: which windows they accept, the state a window is
 * left in, and where a floating one lands on the way back.
 *
 * Also here, because each is the same contract seen from somewhere else:
 * the focus-stack rule that keeps a shown scratchpad's windows cycling
 * among themselves, the jump-to-window path that must not read a hidden
 * overlay's empty tag set as a view to switch to, and the session file,
 * which must not save overlays at all.
 */

#include <glib-object.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <unistd.h>

#include "core/gowl-core-private.h"
#include "core/gowl-session-default.h"
#include "interfaces/gowl-session-provider.h"

typedef struct {
	GowlCompositor *comp;
	GowlMonitor    *mon;
	GowlMonitor    *mon2;
	GowlClient     *c[4];
} Fixture;

static void
set_box(
	struct wlr_box *box,
	gint            x,
	gint            y,
	gint            width,
	gint            height
){
	box->x = x;
	box->y = y;
	box->width = width;
	box->height = height;
}

/* Two outputs: a 1920x1080 one under a 30px bar, viewing tag 1, and one
 * to its right viewing tag 3.  Four windows on the first, on tag 1, in
 * focus order c[0] first. */
static void
fixture_setup(Fixture *f, gconstpointer data)
{
	guint i;

	(void)data;
	f->comp = gowl_compositor_new();

	f->mon = g_object_new(GOWL_TYPE_MONITOR, NULL);
	set_box(&f->mon->w, 0, 30, 1920, 1050);
	f->mon->tagset[f->mon->seltags] = 1;
	f->mon2 = g_object_new(GOWL_TYPE_MONITOR, NULL);
	set_box(&f->mon2->w, 1920, 0, 1920, 1080);
	f->mon2->tagset[f->mon2->seltags] = 4;
	f->comp->selmon = f->mon;
	f->comp->monitors = g_list_append(NULL, f->mon);
	f->comp->monitors = g_list_append(f->comp->monitors, f->mon2);

	for (i = 0; i < G_N_ELEMENTS(f->c); i++) {
		f->c[i] = gowl_client_new();
		f->c[i]->mon = f->mon;
		f->c[i]->tags = 1;
		f->comp->clients = g_list_append(f->comp->clients, f->c[i]);
		f->comp->fstack = g_list_append(f->comp->fstack, f->c[i]);
	}
}

static void
fixture_teardown(Fixture *f, gconstpointer data)
{
	guint i;

	(void)data;
	g_clear_pointer(&f->comp->fstack, g_list_free);
	g_clear_pointer(&f->comp->clients, g_list_free);
	g_clear_pointer(&f->comp->monitors, g_list_free);
	f->comp->selmon = NULL;
	for (i = 0; i < G_N_ELEMENTS(f->c); i++) {
		f->c[i]->mon = NULL;
		g_object_unref(f->c[i]);
	}
	g_object_unref(f->comp);
	g_object_unref(f->mon);
	g_object_unref(f->mon2);
}

/* An ordinary window becomes a hidden overlay on no tag, and the window
 * that counts as focused is the next one. */
static void
test_adopt_takes_the_window(Fixture *f, gconstpointer data)
{
	GowlClient *t = f->c[0];

	(void)data;
	g_assert_true(gowl_compositor_get_focused_client(f->comp) == t);
	g_assert_true(gowl_compositor_adopt_overlay(f->comp, t, 1));
	g_assert_true(t->isoverlay);
	g_assert_false(t->overlay_visible);
	g_assert_cmpuint(t->overlay_group, ==, 1);
	g_assert_true(t->isfloating);
	g_assert_cmpuint(t->tags, ==, 0);
	g_assert_true(t->mon == f->mon);
	g_assert_true(gowl_compositor_get_focused_client(f->comp) == f->c[1]);
}

/* What cannot be adopted, and that a refusal changes nothing. */
static void
test_adopt_refuses(Fixture *f, gconstpointer data)
{
	(void)data;
	/* Already somebody's overlay: the dropdown's shell, say. */
	f->c[0]->isoverlay = TRUE;
	f->c[0]->overlay_visible = TRUE;
	g_assert_false(gowl_compositor_adopt_overlay(f->comp, f->c[0], 1));
	g_assert_cmpuint(f->c[0]->overlay_group, ==, 0);
	g_assert_cmpuint(f->c[0]->tags, ==, 1);

	/* Embedded: it belongs to its host. */
	f->c[1]->isembedded = TRUE;
	g_assert_false(gowl_compositor_adopt_overlay(f->comp, f->c[1], 1));
	g_assert_false(f->c[1]->isoverlay);

	/* Not in the client list: not mapped. */
	f->comp->clients = g_list_remove(f->comp->clients, f->c[2]);
	g_assert_false(gowl_compositor_adopt_overlay(f->comp, f->c[2], 1));
	g_assert_false(f->c[2]->isoverlay);

	/* Group 0 is for windows nobody adopted. */
	g_test_expect_message("gowl", G_LOG_LEVEL_CRITICAL, "*group != 0*");
	g_assert_false(gowl_compositor_adopt_overlay(f->comp, f->c[3], 0));
	g_test_assert_expected_messages();
	g_assert_false(f->c[3]->isoverlay);
}

/* A tiled window goes back tiled, on the tags the output is viewing. */
static void
test_release_tiled(Fixture *f, gconstpointer data)
{
	GowlClient *t = f->c[0];

	(void)data;
	g_assert_true(gowl_compositor_adopt_overlay(f->comp, t, 1));
	g_assert_true(gowl_compositor_release_overlay(f->comp, t, NULL));
	g_assert_false(t->isoverlay);
	g_assert_false(t->overlay_visible);
	g_assert_cmpuint(t->overlay_group, ==, 0);
	g_assert_false(t->isfloating);
	/* NULL means the selected output. */
	g_assert_true(t->mon == f->mon);
	g_assert_cmpuint(t->tags, ==, 1);

	/* An ordinary window is no overlay to release. */
	g_assert_false(gowl_compositor_release_overlay(f->comp, f->c[1], NULL));
	g_assert_cmpuint(f->c[1]->tags, ==, 1);
}

/* A floating window goes back floating where it floated, measured from
 * the window area -- so on another output it lands in the same place. */
static void
test_release_floating_where_it_floated(Fixture *f, gconstpointer data)
{
	GowlClient *w = f->c[0];

	(void)data;
	w->isfloating = TRUE;
	set_box(&w->geom, 100, 200, 300, 400);  /* 170 below the bar */
	g_assert_true(gowl_compositor_adopt_overlay(f->comp, w, 1));

	/* Shown in a panel it has another geometry entirely. */
	set_box(&w->geom, 0, 380, 1920, 700);
	g_assert_true(gowl_compositor_release_overlay(f->comp, w, f->mon2));
	g_assert_true(w->isfloating);
	g_assert_true(w->mon == f->mon2);
	g_assert_cmpuint(w->tags, ==, 4);
	g_assert_cmpint(w->geom.x, ==, 1920 + 100);
	g_assert_cmpint(w->geom.y, ==, 0 + 170);
	g_assert_cmpint(w->geom.width, ==, 300);
	g_assert_cmpint(w->geom.height, ==, 400);
}

/* Unmapped, there is nothing to place: only the overlay state goes.  This
 * is the call on_client_unmap() makes, so a toplevel that maps again comes
 * back as an ordinary window. */
static void
test_release_unmapped(Fixture *f, gconstpointer data)
{
	GowlClient *t = f->c[0];

	(void)data;
	g_assert_true(gowl_compositor_adopt_overlay(f->comp, t, 1));
	/* What on_client_unmap() has done by the time it releases. */
	f->comp->clients = g_list_remove(f->comp->clients, t);
	f->comp->fstack = g_list_remove(f->comp->fstack, t);
	t->mon = NULL;

	g_assert_true(gowl_compositor_release_overlay(f->comp, t, f->mon));
	g_assert_false(t->isoverlay);
	g_assert_cmpuint(t->overlay_group, ==, 0);
	g_assert_false(t->isfloating);
	g_assert_null(t->mon);
	g_assert_cmpuint(t->tags, ==, 0);
}

/* Focus-stack steps stay in their overlay group: tiles step over a shown
 * panel, and the panel's windows cycle among themselves. */
static void
test_stack_neighbour_stays_in_its_group(Fixture *f, gconstpointer data)
{
	GowlClient *t1 = f->c[0];
	GowlClient *s1 = f->c[1];
	GowlClient *t2 = f->c[2];
	GowlClient *s2 = f->c[3];
	GowlClient *hidden;
	GowlClient *away;

	(void)data;
	/* In list order: t1 s1 t2 s2, then a hidden member and a window on
	 * tag 2, neither of them visible. */
	s1->isoverlay = TRUE;
	s1->overlay_visible = TRUE;
	s1->overlay_group = 1;
	s2->isoverlay = TRUE;
	s2->overlay_visible = TRUE;
	s2->overlay_group = 1;
	hidden = gowl_client_new();
	hidden->mon = f->mon;
	hidden->isoverlay = TRUE;
	hidden->overlay_group = 1;
	away = gowl_client_new();
	away->mon = f->mon;
	away->tags = 2;
	f->comp->clients = g_list_append(f->comp->clients, hidden);
	f->comp->clients = g_list_append(f->comp->clients, away);

	g_assert_true(gowl_compositor_stack_neighbour(f->comp, t1, 1) == t2);
	g_assert_true(gowl_compositor_stack_neighbour(f->comp, t2, 1) == t1);
	g_assert_true(gowl_compositor_stack_neighbour(f->comp, t1, -1) == t2);
	g_assert_true(gowl_compositor_stack_neighbour(f->comp, s1, 1) == s2);
	g_assert_true(gowl_compositor_stack_neighbour(f->comp, s2, 1) == s1);
	g_assert_true(gowl_compositor_stack_neighbour(f->comp, s1, -1) == s2);

	/* Alone in its group, a window steps to itself. */
	s2->overlay_visible = FALSE;
	g_assert_true(gowl_compositor_stack_neighbour(f->comp, s1, 1) == s1);

	/* Not in the list: no step at all. */
	f->comp->clients = g_list_remove(f->comp->clients, away);
	g_assert_null(gowl_compositor_stack_neighbour(f->comp, away, 1));

	f->comp->clients = g_list_remove(f->comp->clients, hidden);
	hidden->mon = NULL;
	away->mon = NULL;
	g_object_unref(hidden);
	g_object_unref(away);
}

/* Jumping to a hidden overlay has no tag to switch to, and must not
 * switch the output to viewing none -- that empties the screen. */
static void
test_show_client_keeps_the_view(Fixture *f, gconstpointer data)
{
	GowlClient *t = f->c[0];

	(void)data;
	g_assert_true(gowl_compositor_adopt_overlay(f->comp, t, 1));
	g_assert_cmpuint(t->tags, ==, 0);
	gowl_compositor_show_client(f->comp, t);
	g_assert_cmpuint(f->mon->tagset[f->mon->seltags], ==, 1);

	/* An ordinary window on another tag still switches the view. */
	f->c[1]->tags = 2;
	gowl_compositor_show_client(f->comp, f->c[1]);
	g_assert_cmpuint(f->mon->tagset[f->mon->seltags], ==, 2);
}

/* A session file keeps ordinary windows and leaves overlays out: their
 * modules put them back, and saved they would come back as rules that
 * float every window of that app at the overlay's size. */
static void
test_session_leaves_overlays_out(Fixture *f, gconstpointer data)
{
	g_autoptr(GowlSessionDefault) provider = NULL;
	g_autoptr(GFile)              file = NULL;
	g_autoptr(GError)             error = NULL;
	g_autofree gchar             *path = NULL;
	g_autofree gchar             *text = NULL;
	gint                          fd;

	(void)data;
	gowl_client_set_app_id(f->c[0], "org.example.Tiled");
	gowl_client_set_app_id(f->c[1], "org.example.InThePanel");
	g_assert_true(gowl_compositor_adopt_overlay(f->comp, f->c[1], 1));

	fd = g_file_open_tmp("gowl-session-XXXXXX.yaml", &path, &error);
	g_assert_no_error(error);
	close(fd);
	file = g_file_new_for_path(path);
	provider = gowl_session_default_new();
	g_assert_true(gowl_session_provider_save(
		GOWL_SESSION_PROVIDER(provider), f->comp, file, &error));
	g_assert_no_error(error);

	g_assert_true(g_file_get_contents(path, &text, NULL, &error));
	g_assert_no_error(error);
	g_assert_nonnull(strstr(text, "org.example.Tiled"));
	g_assert_null(strstr(text, "org.example.InThePanel"));
	g_unlink(path);
}

#define ADD(path, fn) \
	g_test_add("/overlay-adopt/" path, Fixture, NULL, \
	           fixture_setup, fn, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("adopt/takes-the-window", test_adopt_takes_the_window);
	ADD("adopt/refuses", test_adopt_refuses);
	ADD("release/tiled", test_release_tiled);
	ADD("release/floating-where-it-floated",
	    test_release_floating_where_it_floated);
	ADD("release/unmapped", test_release_unmapped);
	ADD("stack-neighbour/stays-in-its-group",
	    test_stack_neighbour_stays_in_its_group);
	ADD("show-client/keeps-the-view", test_show_client_keeps_the_view);
	ADD("session/leaves-overlays-out", test_session_leaves_overlays_out);

	return g_test_run();
}
