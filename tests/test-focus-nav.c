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
 */

#include <glib.h>
#include <glib-object.h>
#include "core/gowl-core-private.h"

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
	g_test_add("/focus-nav/urgent-and-last", Fixture, NULL,
	           fixture_setup, test_urgent_and_last, fixture_teardown);

	return g_test_run();
}
