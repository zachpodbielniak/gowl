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
 * Focus by direction, urgency and history, and the sticky flag, on a
 * compositor that was never started: the choices are geometry and list
 * order, so no seat is needed to check them.
 *
 * The sticky cases are regressions.  Element, tiled on tag 3, was
 * pinned (Super+Shift+t) and from then on was a TILE on every tag: the
 * scrolling layout on tag 4 slotted it into its strip beside Steam, and
 * on tag 2 it sat over the terminal at the box tag 4 had given it.
 * Only a floating window may be on every tag.
 */

#include <glib.h>
#include <glib-object.h>
#include "core/gowl-core-private.h"
#include <wlr/types/wlr_scene.h>

typedef struct {
	GowlCompositor *comp;
	GowlMonitor    *mon;
	GowlClient     *a;   /* left half */
	GowlClient     *b;   /* top right */
	GowlClient     *c;   /* bottom right, the taller one */
	GowlClient     *d;   /* off to the left, on tag 2 */
} Fixture;

static GowlClient *
make_client(Fixture *f, gint x, gint y, gint w, gint h, guint32 tags)
{
	GowlClient *c = gowl_client_new();

	c->mon = f->mon;
	c->tags = tags;
	c->geom.x = x;
	c->geom.y = y;
	c->geom.width = w;
	c->geom.height = h;
	f->comp->clients = g_list_append(f->comp->clients, c);
	f->comp->fstack = g_list_append(f->comp->fstack, c);
	return c;
}

static void
fixture_setup(Fixture *f, gconstpointer data)
{
	(void)data;

	f->comp = gowl_compositor_new();
	f->mon = g_object_new(GOWL_TYPE_MONITOR, NULL);
	f->mon->w.x = 0;
	f->mon->w.y = 0;
	f->mon->w.width = 1920;
	f->mon->w.height = 1080;
	f->mon->tagset[f->mon->seltags] = 1;
	f->comp->selmon = f->mon;
	f->comp->monitors = g_list_append(NULL, f->mon);

	f->a = make_client(f, 0, 0, 960, 1080, 1);
	f->b = make_client(f, 960, 0, 960, 400, 1);
	f->c = make_client(f, 960, 400, 960, 680, 1);
	f->d = make_client(f, -500, 0, 400, 1080, 2);
}

static void
fixture_teardown(Fixture *f, gconstpointer data)
{
	GList *l;
	(void)data;

	for (l = f->comp->clients; l != NULL; l = l->next)
		((GowlClient *)l->data)->mon = NULL;
	g_list_free_full(f->comp->clients, g_object_unref);
	f->comp->clients = NULL;
	g_clear_pointer(&f->comp->fstack, g_list_free);
	g_clear_pointer(&f->comp->monitors, g_list_free);
	f->comp->selmon = NULL;
	g_object_unref(f->comp);
	g_object_unref(f->mon);
}

static void
test_direction_neighbour(Fixture *f, gconstpointer data)
{
	(void)data;

	/* From the left half, right: both right-hand windows overlap it
	 * across; the taller one's centre is nearer the middle. */
	g_assert_true(gowl_compositor_direction_neighbour(f->comp, f->a,
		GOWL_DIRECTION_RIGHT) == f->c);
	g_assert_true(gowl_compositor_direction_neighbour(f->comp, f->c,
		GOWL_DIRECTION_UP) == f->b);
	g_assert_true(gowl_compositor_direction_neighbour(f->comp, f->b,
		GOWL_DIRECTION_DOWN) == f->c);
	g_assert_true(gowl_compositor_direction_neighbour(f->comp, f->b,
		GOWL_DIRECTION_LEFT) == f->a);
	/* Nothing above the top row, nothing right of the right column. */
	g_assert_null(gowl_compositor_direction_neighbour(f->comp, f->b,
		GOWL_DIRECTION_UP));
	g_assert_null(gowl_compositor_direction_neighbour(f->comp, f->c,
		GOWL_DIRECTION_RIGHT));
	/* The window on tag 2 is not there. */
	g_assert_null(gowl_compositor_direction_neighbour(f->comp, f->a,
		GOWL_DIRECTION_LEFT));
}

static void
test_sticky_is_on_every_tag(Fixture *f, gconstpointer data)
{
	(void)data;

	g_assert_false(gowl_client_get_sticky(f->d));
	gowl_client_set_sticky(f->d, TRUE);
	g_assert_true(gowl_client_get_sticky(f->d));
	/* Now it is to the left of the left half. */
	g_assert_true(gowl_compositor_direction_neighbour(f->comp, f->a,
		GOWL_DIRECTION_LEFT) == f->d);
	/* Its own tags are kept for when it is unpinned. */
	g_assert_cmpuint(f->d->tags, ==, 2);
	gowl_client_set_sticky(f->d, FALSE);
	g_assert_null(gowl_compositor_direction_neighbour(f->comp, f->a,
		GOWL_DIRECTION_LEFT));
}

/* Pinning a tile floats it: on every tag, and in no tag's layout. */
static void
test_sticky_floats_a_tile(Fixture *f, gconstpointer data)
{
	GList *tiled;
	(void)data;

	g_assert_false(f->d->isfloating);
	gowl_client_set_sticky(f->d, TRUE);
	g_assert_true(f->d->isfloating);
	g_assert_true(gowl_client_is_pinned(f->d));

	/* Tag 1 is in view; d lives on tag 2.  It shows, but is not tiled
	 * -- the scrolling strip and every other layout take their windows
	 * from this list. */
	tiled = gowl_compositor_tiling_clients(f->comp, f->mon);
	g_assert_null(g_list_find(tiled, f->d));
	g_assert_cmpuint(g_list_length(tiled), ==, 3);
	g_list_free(tiled);

	/* Unpinning leaves it floating where it is: only the flag goes. */
	gowl_client_set_sticky(f->d, FALSE);
	g_assert_true(f->d->isfloating);
	g_assert_false(gowl_client_is_pinned(f->d));
}

/*
 * A tile that carries the sticky flag anyway -- set straight on the
 * field, the way the state looked on the machine it was found on -- is
 * on its own tags only.  This is the exact failure: the scrolling
 * layout's list for another tag must not contain it.
 */
static void
test_sticky_tile_stays_on_its_tag(Fixture *f, gconstpointer data)
{
	GList *tiled;
	(void)data;

	f->d->issticky = TRUE;
	g_assert_false(f->d->isfloating);

	tiled = gowl_compositor_tiling_clients(f->comp, f->mon);
	g_assert_null(g_list_find(tiled, f->d));
	g_list_free(tiled);
	g_assert_false(gowl_client_is_pinned(f->d));
	g_assert_null(gowl_compositor_direction_neighbour(f->comp, f->a,
		GOWL_DIRECTION_LEFT));

	/* On its own tag it is tiled as usual. */
	f->mon->tagset[f->mon->seltags] = 2;
	tiled = gowl_compositor_tiling_clients(f->comp, f->mon);
	g_assert_nonnull(g_list_find(tiled, f->d));
	g_assert_cmpuint(g_list_length(tiled), ==, 1);
	g_list_free(tiled);
}

/* Tiling a pinned window again unpins it: the flag would otherwise sit
 * inert on the tile and pin it out of nowhere the next time it floats. */
static void
test_tiling_unpins(Fixture *f, gconstpointer data)
{
	struct wlr_scene *scene;
	(void)data;

	scene = wlr_scene_create();
	f->d->scene = wlr_scene_tree_create(&scene->tree);
	/* Off any monitor: setfloating() then changes state only, which is
	 * all this checks, and needs no outputs to arrange. */
	f->d->mon = NULL;

	f->d->isfloating = TRUE;
	f->d->issticky = TRUE;
	gowl_compositor_set_client_floating(f->comp, f->d, FALSE);
	g_assert_false(f->d->isfloating);
	g_assert_false(f->d->issticky);

	/* Floating it again does not bring the pin back. */
	gowl_compositor_set_client_floating(f->comp, f->d, TRUE);
	g_assert_false(gowl_client_is_pinned(f->d));

	wlr_scene_node_destroy(&scene->tree.node);
	f->d->scene = NULL;
}

static void
test_urgent_and_last(Fixture *f, gconstpointer data)
{
	(void)data;

	g_assert_null(gowl_compositor_urgent_client(f->comp));
	f->d->isurgent = TRUE;
	g_assert_true(gowl_compositor_urgent_client(f->comp) == f->d);
	f->c->isurgent = TRUE;
	/* The first urgent one in list order.  The compositor prepends on
	 * map, so there that is the most recently mapped; this fixture
	 * appends, so here it is c. */
	g_assert_true(gowl_compositor_urgent_client(f->comp) == f->c);

	/* The focus stack is a, b, c, d: a has focus, b had it before. */
	g_assert_true(gowl_compositor_last_focused(f->comp) == f->b);
	/* A hidden overlay in between is skipped. */
	f->b->isoverlay = TRUE;
	f->b->overlay_visible = FALSE;
	g_assert_true(gowl_compositor_last_focused(f->comp) == f->c);
	/* An embedded window too. */
	f->c->isembedded = TRUE;
	g_assert_true(gowl_compositor_last_focused(f->comp) == f->d);
	f->d->isembedded = TRUE;
	g_assert_null(gowl_compositor_last_focused(f->comp));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add("/focus-nav/direction-neighbour", Fixture, NULL,
	           fixture_setup, test_direction_neighbour, fixture_teardown);
	g_test_add("/focus-nav/sticky-on-every-tag", Fixture, NULL,
	           fixture_setup, test_sticky_is_on_every_tag, fixture_teardown);
	g_test_add("/focus-nav/sticky-floats-a-tile", Fixture, NULL,
	           fixture_setup, test_sticky_floats_a_tile, fixture_teardown);
	g_test_add("/focus-nav/sticky-tile-stays-on-its-tag", Fixture, NULL,
	           fixture_setup, test_sticky_tile_stays_on_its_tag,
	           fixture_teardown);
	g_test_add("/focus-nav/tiling-unpins", Fixture, NULL,
	           fixture_setup, test_tiling_unpins, fixture_teardown);
	g_test_add("/focus-nav/urgent-and-last", Fixture, NULL,
	           fixture_setup, test_urgent_and_last, fixture_teardown);

	return g_test_run();
}
