/* test-effects.c -- how scene-effect hooks are shared out
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * gowl runs several effect modules at once -- animation, cube, expo,
 * switcher, blur, magnifier -- over one interface.  Which of them gets a
 * given hook is decided in gowl-effects.c, per event, and getting it
 * wrong does not crash:
 *
 *   - a CONSUMABLE hook that failed to stop at the first claimant would
 *     let two modules place the same window, and the last to run wins by
 *     accident;
 *   - a BROADCAST hook that stopped at the first provider would silently
 *     switch off every module after it in priority order --- exactly the
 *     failure the old single-owner dispatch had.
 *
 * So the semantics are pinned here with counting providers, and the two
 * real plugins are loaded at the end to check they agree with them.
 */

#include <glib.h>

#include "core/gowl-core-private.h"
#include "core/gowl-effects.h"
#include "interfaces/gowl-scene-effect.h"
#include "module/gowl-module-manager.h"

/* ── A provider that counts, and can be told to claim ─────────────── */

typedef struct {
	gint client_event;
	gint get_geometry;
	gint alpha_changed;
	gint surface_at;
	gint frame;
	gint frame_done;
	gint monitor_removed;
	gint finish;
} Calls;

G_DECLARE_FINAL_TYPE(TestEffect, test_effect, TEST, EFFECT, GowlModule)
struct _TestEffect {
	GowlModule parent_instance;
	Calls      calls;
	gboolean   claims;       /* answer consumable hooks affirmatively */
	gboolean   frame_live;   /* answer "still animating" */
	gchar     *name;
};

static gboolean
te_client_event(GowlSceneEffect *e, GowlCompositor *c, GowlClient *cl,
                GowlSceneEffectEvent ev, const struct wlr_box *prev, gboolean it)
{
	TestEffect *self = TEST_EFFECT(e);

	self->calls.client_event++;
	return self->claims;
}

static gboolean
te_get_geometry(GowlSceneEffect *e, GowlClient *c, struct wlr_box *box)
{
	TestEffect *self = TEST_EFFECT(e);

	self->calls.get_geometry++;
	if (!self->claims)
		return FALSE;
	box->x = 42;
	return TRUE;
}

static void
te_alpha_changed(GowlSceneEffect *e, GowlClient *c, gfloat a)
{
	TEST_EFFECT(e)->calls.alpha_changed++;
}

static struct wlr_surface *
te_surface_at(GowlSceneEffect *e, GowlClient *c, gdouble x, gdouble y,
              gdouble *sx, gdouble *sy)
{
	TestEffect *self = TEST_EFFECT(e);

	self->calls.surface_at++;
	/* Any non-NULL value; the dispatcher only looks at nullness. */
	return self->claims ? (struct wlr_surface *)self : NULL;
}

static gboolean
te_frame(GowlSceneEffect *e, GowlCompositor *c, GowlMonitor *m, gint64 now)
{
	TestEffect *self = TEST_EFFECT(e);

	self->calls.frame++;
	return self->frame_live;
}

static void
te_frame_done(GowlSceneEffect *e, GowlCompositor *c, GowlMonitor *m,
              const struct timespec *now)
{
	TEST_EFFECT(e)->calls.frame_done++;
}

static void
te_monitor_removed(GowlSceneEffect *e, GowlCompositor *c, GowlMonitor *m)
{
	TEST_EFFECT(e)->calls.monitor_removed++;
}

static void
te_finish(GowlSceneEffect *e, GowlCompositor *c)
{
	TEST_EFFECT(e)->calls.finish++;
}

static void
te_effect_init(GowlSceneEffectInterface *iface)
{
	iface->client_event    = te_client_event;
	iface->get_geometry    = te_get_geometry;
	iface->alpha_changed   = te_alpha_changed;
	iface->surface_at      = te_surface_at;
	iface->frame           = te_frame;
	iface->frame_done      = te_frame_done;
	iface->monitor_removed = te_monitor_removed;
	iface->finish          = te_finish;
}

G_DEFINE_TYPE_WITH_CODE(TestEffect, test_effect, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SCENE_EFFECT, te_effect_init))

/*
 * The manager instantiates modules itself, from a GType, and sorts the
 * dispatch arrays as it registers them -- so a priority set afterwards
 * would come too late.  These two statics are how a test hands the
 * identity and priority in through that constructor.
 */
static gint         next_priority = 0;
static const gchar *next_name     = "test-effect";

static const gchar *te_name(GowlModule *m) { return TEST_EFFECT(m)->name; }
static gboolean te_activate(GowlModule *m) { return TRUE; }
static void
test_effect_init(TestEffect *self)
{
	self->name = g_strdup(next_name);
	gowl_module_set_priority(GOWL_MODULE(self), next_priority);
}
static void
te_finalize(GObject *o)
{
	g_free(TEST_EFFECT(o)->name);
	G_OBJECT_CLASS(test_effect_parent_class)->finalize(o);
}
static void
test_effect_class_init(TestEffectClass *klass)
{
	GOWL_MODULE_CLASS(klass)->get_name = te_name;
	GOWL_MODULE_CLASS(klass)->activate = te_activate;
	G_OBJECT_CLASS(klass)->finalize = te_finalize;
}

/* ── Fixture ─────────────────────────────────────────────────────── */

typedef struct {
	GowlCompositor *compositor;
	GowlClient     *client;
	TestEffect     *first;    /* priority -10 */
	TestEffect     *second;   /* priority   0 */
} Fixture;

static TestEffect *
add_provider(Fixture *f, gint priority, const gchar *name)
{
	TestEffect *e;

	next_priority = priority;
	next_name = name;
	g_assert_true(gowl_module_manager_register(f->compositor->module_mgr,
	                                            test_effect_get_type(),
	                                            NULL));
	e = TEST_EFFECT(gowl_module_manager_find_module(
		f->compositor->module_mgr, name));
	g_assert_nonnull(e);
	g_assert_true(gowl_module_activate(GOWL_MODULE(e)));
	return e;
}

static void
setup(Fixture *f, gconstpointer data)
{
	f->compositor = gowl_compositor_new();
	f->compositor->module_mgr = gowl_module_manager_new();
	f->client = gowl_client_new();
	f->client->compositor = f->compositor;
	f->client->geom.x = 7;

	f->first  = add_provider(f, -10, "first");
	f->second = add_provider(f, 0, "second");
}

static void
teardown(Fixture *f, gconstpointer data)
{
	g_clear_object(&f->client);
	g_clear_object(&f->compositor->module_mgr);
	g_clear_object(&f->compositor);
}

/* ── Ordering ────────────────────────────────────────────────────── */

/* Lower number first, whatever order they were registered in. */
static void
test_priority_orders_providers(Fixture *f, gconstpointer data)
{
	GPtrArray *set =
		gowl_module_manager_get_scene_effects(f->compositor->module_mgr);

	g_assert_cmpuint(set->len, ==, 2);
	g_assert_true(g_ptr_array_index(set, 0) == f->first);
	g_assert_true(g_ptr_array_index(set, 1) == f->second);
	g_ptr_array_unref(set);

	g_assert_true(gowl_module_manager_get_scene_effect(f->compositor->module_mgr)
	              == (gpointer)f->first);
}

/* A deactivated provider is not in the set at all --- that is how
 * `(gowl-disable-module "cube")' takes effect. */
static void
test_deactivated_providers_drop_out(Fixture *f, gconstpointer data)
{
	GPtrArray *set;

	gowl_module_deactivate(GOWL_MODULE(f->first));

	set = gowl_module_manager_get_scene_effects(f->compositor->module_mgr);
	g_assert_cmpuint(set->len, ==, 1);
	g_assert_true(g_ptr_array_index(set, 0) == f->second);
	g_ptr_array_unref(set);

	gowl_effects_frame(f->compositor, NULL, 0);
	g_assert_cmpint(f->first->calls.frame, ==, 0);
	g_assert_cmpint(f->second->calls.frame, ==, 1);
}

/* ── Consumable hooks stop at the first claimant ──────────────────── */

static void
test_client_event_stops_at_the_claimant(Fixture *f, gconstpointer data)
{
	f->first->claims = TRUE;

	g_assert_true(gowl_effects_client_event(f->compositor, f->client,
	                                        GOWL_SCENE_EFFECT_GEOMETRY,
	                                        NULL, FALSE));
	g_assert_cmpint(f->first->calls.client_event, ==, 1);
	g_assert_cmpint(f->second->calls.client_event, ==, 0);
}

/* And when it declines, the event carries on down rather than being
 * eaten.  This is the case that matters: the cube declines almost
 * everything, and the animation module is what is behind it. */
static void
test_client_event_falls_through_a_decline(Fixture *f, gconstpointer data)
{
	f->first->claims  = FALSE;
	f->second->claims = TRUE;

	g_assert_true(gowl_effects_client_event(f->compositor, f->client,
	                                        GOWL_SCENE_EFFECT_GEOMETRY,
	                                        NULL, FALSE));
	g_assert_cmpint(f->first->calls.client_event, ==, 1);
	g_assert_cmpint(f->second->calls.client_event, ==, 1);
}

/* Nobody claiming is not an error: the core applies the geometry itself. */
static void
test_client_event_unclaimed_is_false(Fixture *f, gconstpointer data)
{
	g_assert_false(gowl_effects_client_event(f->compositor, f->client,
	                                         GOWL_SCENE_EFFECT_GEOMETRY,
	                                         NULL, FALSE));
	g_assert_cmpint(f->first->calls.client_event, ==, 1);
	g_assert_cmpint(f->second->calls.client_event, ==, 1);
}

static void
test_geometry_stops_at_the_claimant(Fixture *f, gconstpointer data)
{
	struct wlr_box box;

	f->second->claims = TRUE;
	box = gowl_effects_geometry(f->client);

	g_assert_cmpint(box.x, ==, 42);
	g_assert_cmpint(f->first->calls.get_geometry, ==, 1);
	g_assert_cmpint(f->second->calls.get_geometry, ==, 1);

	/* With nobody claiming, the client's own rectangle comes back. */
	f->second->claims = FALSE;
	box = gowl_effects_geometry(f->client);
	g_assert_cmpint(box.x, ==, 7);
}

static void
test_surface_at_stops_at_the_first_hit(Fixture *f, gconstpointer data)
{
	gdouble sx = 0, sy = 0;

	f->first->claims = TRUE;
	g_assert_nonnull(gowl_effects_surface_at(f->client, 1, 1, &sx, &sy));
	g_assert_cmpint(f->first->calls.surface_at, ==, 1);
	g_assert_cmpint(f->second->calls.surface_at, ==, 0);
}

/* ── Broadcast hooks reach everyone ───────────────────────────────── */

static void
test_frame_reaches_every_provider(Fixture *f, gconstpointer data)
{
	g_assert_false(gowl_effects_frame(f->compositor, NULL, 0));
	g_assert_cmpint(f->first->calls.frame, ==, 1);
	g_assert_cmpint(f->second->calls.frame, ==, 1);
}

/*
 * "Still animating" is an OR across providers, and the earlier one saying
 * yes must not short-circuit the later one's tick.  A short-circuit here
 * would freeze whichever module happens to sort second, but only while
 * the first one was busy -- an intermittent bug of the worst kind.
 */
static void
test_frame_is_or_and_never_short_circuits(Fixture *f, gconstpointer data)
{
	f->first->frame_live = TRUE;

	g_assert_true(gowl_effects_frame(f->compositor, NULL, 0));
	g_assert_cmpint(f->first->calls.frame, ==, 1);
	g_assert_cmpint(f->second->calls.frame, ==, 1);

	f->first->frame_live  = FALSE;
	f->second->frame_live = TRUE;
	g_assert_true(gowl_effects_frame(f->compositor, NULL, 0));
}

static void
test_teardown_hooks_reach_every_provider(Fixture *f, gconstpointer data)
{
	struct timespec now = { 0, 0 };

	gowl_effects_alpha_changed(f->client, 0.5f);
	gowl_effects_frame_done(f->compositor, NULL, &now);
	gowl_effects_monitor_removed(f->compositor, NULL);
	gowl_effects_finish(f->compositor);

	g_assert_cmpint(f->first->calls.alpha_changed, ==, 1);
	g_assert_cmpint(f->second->calls.alpha_changed, ==, 1);
	g_assert_cmpint(f->first->calls.frame_done, ==, 1);
	g_assert_cmpint(f->second->calls.frame_done, ==, 1);
	g_assert_cmpint(f->first->calls.monitor_removed, ==, 1);
	g_assert_cmpint(f->second->calls.monitor_removed, ==, 1);
	/* finish especially: a provider that misses it holds client buffers
	 * and GL objects into renderer teardown. */
	g_assert_cmpint(f->first->calls.finish, ==, 1);
	g_assert_cmpint(f->second->calls.finish, ==, 1);
}

/* A claiming provider must not swallow a broadcast hook either --- the
 * two kinds are independent, and conflating them is the easy mistake. */
static void
test_claiming_does_not_affect_broadcast(Fixture *f, gconstpointer data)
{
	f->first->claims = TRUE;

	gowl_effects_finish(f->compositor);
	g_assert_cmpint(f->second->calls.finish, ==, 1);
}

/* ── No providers at all ──────────────────────────────────────────── */

static void
test_no_providers_is_quiet(void)
{
	GowlCompositor *c = gowl_compositor_new();
	GowlClient *cl = gowl_client_new();
	struct timespec now = { 0, 0 };
	struct wlr_box box;
	gdouble sx = 0, sy = 0;

	cl->compositor = c;
	cl->geom.x = 11;

	/* Without a module manager at all, which is what a bare compositor
	 * and half the tests look like. */
	g_assert_false(gowl_effects_client_event(c, cl, GOWL_SCENE_EFFECT_GEOMETRY,
	                                         NULL, FALSE));
	box = gowl_effects_geometry(cl);
	g_assert_cmpint(box.x, ==, 11);
	g_assert_false(gowl_effects_has_geometry(cl));
	g_assert_null(gowl_effects_surface_at(cl, 0, 0, &sx, &sy));
	g_assert_false(gowl_effects_frame(c, NULL, 0));
	gowl_effects_alpha_changed(cl, 1.0f);
	gowl_effects_frame_done(c, NULL, &now);
	gowl_effects_monitor_removed(c, NULL);
	gowl_effects_finish(c);

	g_object_unref(cl);
	g_object_unref(c);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add("/effects/priority-order", Fixture, NULL,
	           setup, test_priority_orders_providers, teardown);
	g_test_add("/effects/deactivated-drop-out", Fixture, NULL,
	           setup, test_deactivated_providers_drop_out, teardown);
	g_test_add("/effects/client-event/claimed", Fixture, NULL,
	           setup, test_client_event_stops_at_the_claimant, teardown);
	g_test_add("/effects/client-event/declined", Fixture, NULL,
	           setup, test_client_event_falls_through_a_decline, teardown);
	g_test_add("/effects/client-event/unclaimed", Fixture, NULL,
	           setup, test_client_event_unclaimed_is_false, teardown);
	g_test_add("/effects/geometry/claimed", Fixture, NULL,
	           setup, test_geometry_stops_at_the_claimant, teardown);
	g_test_add("/effects/surface-at/first-hit", Fixture, NULL,
	           setup, test_surface_at_stops_at_the_first_hit, teardown);
	g_test_add("/effects/frame/broadcast", Fixture, NULL,
	           setup, test_frame_reaches_every_provider, teardown);
	g_test_add("/effects/frame/or-semantics", Fixture, NULL,
	           setup, test_frame_is_or_and_never_short_circuits, teardown);
	g_test_add("/effects/teardown/broadcast", Fixture, NULL,
	           setup, test_teardown_hooks_reach_every_provider, teardown);
	g_test_add("/effects/claim-does-not-block-broadcast", Fixture, NULL,
	           setup, test_claiming_does_not_affect_broadcast, teardown);
	g_test_add_func("/effects/no-providers", test_no_providers_is_quiet);

	return g_test_run();
}
