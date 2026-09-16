/* test-scroll-portrait.c -- a tall scrolling strip fills from the bottom
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The scrolling layout lays its windows along one axis, newest first,
 * and transposes that axis on an output that is taller than it is wide.
 * Transposing alone puts the newest window at the TOP, which is not how
 * a column of anything fills: a terminal scrolls up, a chat scrolls up,
 * and the thing you just opened belongs at the bottom with the older
 * ones above it.
 *
 * So on a tall output the axis is mirrored end for end.  What is
 * asserted:
 *
 *   THE NEWEST WINDOW IS THE LOWEST, and each older one sits above it.
 *
 *   IT IS FLUSH WITH THE BOTTOM.  Ordering alone would be satisfied by
 *   a strip that started halfway up the screen and left a gap under it.
 *
 *   LANDSCAPE IS UNTOUCHED.  The mirror is for tall outputs only; a
 *   wide one still reads left to right from the newest.
 *
 *   SCROLLING AND FOCUS STILL AGREE WITH IT.  Only the placement is
 *   mirrored -- the offset and the extent are measured along the same
 *   virtual axis as before -- so bringing the oldest window into view
 *   has to land it fully on the screen and not off the far end.
 */

#include "core/gowl-core-private.h"
#include "core/gowl-layout-registry.h"
#include "interfaces/gowl-layout-provider.h"

#ifndef GOWL_TEST_LAYOUT_MODULE_DIR
#define GOWL_TEST_LAYOUT_MODULE_DIR "build/release/modules"
#endif

#define N_CLIENTS (4)

/* Exercise the real plugin without a renderer: only the final placement
 * and arrange scheduling are intercepted. */
void
gowl_compositor_place_client(GowlCompositor *self, GowlClient *client,
                             gint x, gint y, gint width, gint height)
{
	client->geom = (struct wlr_box){x, y, width, height};
}

void
gowl_compositor_arrange(GowlCompositor *self, GowlMonitor *monitor)
{
	gowl_layout_apply(self, monitor);
}

typedef struct {
	GowlCompositor *comp;
	GowlMonitor    *mon;
	GowlClient      clients[N_CLIENTS];
} Rig;

/*
 * @tall picks the shape of the one monitor.  Clients are appended to the
 * compositor's list in order, and the compositor prepends a newly mapped
 * window, so clients[0] is the NEWEST -- the one just opened.
 */
static void
rig_up(Rig *r, gboolean tall)
{
	guint i;
	gchar *path;

	memset(r, 0, sizeof(*r));
	r->comp = gowl_compositor_new();
	r->comp->module_mgr = gowl_module_manager_new();
	r->mon = g_object_new(GOWL_TYPE_MONITOR, NULL);
	r->mon->compositor = r->comp;
	r->comp->selmon = r->mon;
	r->mon->m = r->mon->w = tall
		? (struct wlr_box){0, 50, 800, 1600}
		: (struct wlr_box){0, 50, 1600, 800};
	r->mon->tagset[0] = 1;
	r->mon->nmaster = 1;
	r->mon->mfact = 0.5;

	for (i = 0; i < N_CLIENTS; i++) {
		r->clients[i].mon = r->mon;
		r->clients[i].tags = 1;
		r->clients[i].compositor = r->comp;
		r->comp->clients = g_list_append(r->comp->clients,
		                                 &r->clients[i]);
	}

	gowl_layout_registry_init(r->comp);
	path = g_strdup_printf("%s/scrolling.so", GOWL_TEST_LAYOUT_MODULE_DIR);
	g_assert_true(gowl_module_manager_load_module(r->comp->module_mgr,
	                                              path, NULL));
	g_free(path);
	gowl_module_manager_activate_all(r->comp->module_mgr);
	gowl_layout_adopt_providers(r->comp);
	g_assert_true(gowl_layout_set(r->comp, r->mon, "scrolling"));
	r->mon->scroll_x = 0;
	gowl_layout_apply(r->comp, r->mon);
}

static void
rig_down(Rig *r)
{
	g_list_free(r->comp->clients);
	r->comp->clients = NULL;
	g_clear_object(&r->mon);
	g_clear_object(&r->comp);
}

/* ------------------------------------------------------------------ */

static void
test_the_newest_window_is_the_lowest(void)
{
	Rig r;
	guint i;

	rig_up(&r, TRUE);

	/*
	 * clients[0] is the one just opened.  It is at the bottom, and
	 * every older one is strictly above the one after it.
	 */
	for (i = 1; i < N_CLIENTS; i++) {
		g_assert_cmpint(r.clients[i].geom.y, <,
		                r.clients[i - 1].geom.y);
	}

	rig_down(&r);
}

static void
test_the_newest_window_is_flush_with_the_bottom(void)
{
	Rig r;
	gint bottom;

	rig_up(&r, TRUE);

	/*
	 * Order alone would be satisfied by a strip floating halfway up
	 * the screen.  At rest the newest window's lower edge is the
	 * usable area's lower edge.
	 */
	bottom = r.mon->w.y + r.mon->w.height;
	g_assert_cmpint(r.clients[0].geom.y + r.clients[0].geom.height,
	                ==, bottom);

	/* And it spans the width, which is what makes it a strip of rows
	 * rather than a grid. */
	g_assert_cmpint(r.clients[0].geom.width, ==, r.mon->w.width);

	rig_down(&r);
}

static void
test_a_wide_output_is_untouched(void)
{
	Rig r;
	guint i;

	rig_up(&r, FALSE);

	/* Left to right from the newest, and flush with the left edge:
	 * the mirror is for tall outputs only. */
	for (i = 1; i < N_CLIENTS; i++) {
		g_assert_cmpint(r.clients[i].geom.x, >,
		                r.clients[i - 1].geom.x);
	}
	g_assert_cmpint(r.clients[0].geom.x, ==, r.mon->w.x);
	g_assert_cmpint(r.clients[0].geom.height, ==, r.mon->w.height);

	rig_down(&r);
}

static void
test_focusing_the_oldest_brings_it_fully_on_screen(void)
{
	Rig r;
	GowlLayoutProvider *provider;
	GowlClient *oldest;

	rig_up(&r, TRUE);
	provider = GOWL_LAYOUT_PROVIDER(gowl_module_manager_find_module(
		r.comp->module_mgr, "scrolling"));
	g_assert_nonnull(provider);
	oldest = &r.clients[N_CLIENTS - 1];

	/*
	 * The oldest window is at the far end of a strip longer than the
	 * screen, so it starts off it.  Focus scrolls it in -- and since
	 * only the placement is mirrored, the offset that does the
	 * scrolling still has to land it inside the output rather than
	 * past the opposite edge.
	 */
	GOWL_LAYOUT_PROVIDER_GET_IFACE(provider)->focus_client(provider,
	                                                       oldest);
	g_assert_cmpint(r.mon->scroll_x, >, 0);
	g_assert_cmpint(oldest->geom.y, >=, r.mon->w.y);
	g_assert_cmpint(oldest->geom.y + oldest->geom.height, <=,
	                r.mon->w.y + r.mon->w.height);
	g_assert_cmpint(oldest->geom.x, >=, r.mon->w.x);
	g_assert_cmpint(oldest->geom.x + oldest->geom.width, <=,
	                r.mon->w.x + r.mon->w.width);

	/* Scrolled or not, the order it was asked for holds. */
	g_assert_cmpint(oldest->geom.y, <, r.clients[0].geom.y);

	rig_down(&r);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/scroll-portrait/the-newest-window-is-the-lowest",
	                test_the_newest_window_is_the_lowest);
	g_test_add_func("/scroll-portrait/flush-with-the-bottom",
	                test_the_newest_window_is_flush_with_the_bottom);
	g_test_add_func("/scroll-portrait/a-wide-output-is-untouched",
	                test_a_wide_output_is_untouched);
	g_test_add_func("/scroll-portrait/focusing-the-oldest-scrolls-it-in",
	                test_focusing_the_oldest_brings_it_fully_on_screen);

	return g_test_run();
}
