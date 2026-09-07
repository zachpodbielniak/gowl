/* test-cube-chain.c -- two scene-effect modules, one owner
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * gowl dispatches scene presentation to ONE provider: the first active
 * one in priority order.  The cube takes that slot so it can decline a
 * rotation it cannot do, and hands everything else down to the provider
 * it outranks.  If that chain breaks, loading the cube quietly switches
 * window animations off -- no crash, no warning, just a compositor that
 * stopped popping windows.
 *
 * tests/test-cube-guard.sh checks the source still contains the
 * delegation.  This checks the runtime half: that the two real plugins,
 * loaded together, actually order and chain the way they are supposed to,
 * and that the cube implements every hook the animation module does --
 * because a hook the cube leaves NULL is a hook the core skips entirely
 * rather than one it passes on.
 */

#include <glib.h>

#include "core/gowl-core-private.h"
#include "interfaces/gowl-scene-effect.h"
#include "module/gowl-module-manager.h"

typedef struct {
	GowlModuleManager *mgr;
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	f->mgr = gowl_module_manager_new();
	g_assert_nonnull(f->mgr);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	g_clear_object(&f->mgr);
}

static GowlModule *
load(Fixture *f, const gchar *path, const gchar *name)
{
	GError *error = NULL;
	GowlModule *mod;

	g_assert_true(gowl_module_manager_load_module(f->mgr, path, &error));
	g_assert_no_error(error);

	mod = gowl_module_manager_find_module(f->mgr, name);
	g_assert_nonnull(mod);
	g_assert_true(gowl_module_activate(mod));
	return mod;
}

/* Alone, the cube is the owner and has nobody to delegate to. */
static void
test_cube_alone(Fixture *f, gconstpointer data)
{
	GowlModule *cube = load(f, GOWL_TEST_CUBE_MODULE, "cube");

	g_assert_true(gowl_module_manager_get_scene_effect(f->mgr)
	              == (gpointer)cube);
	g_assert_null(gowl_module_manager_get_scene_effect_after(f->mgr, cube));
}

/*
 * Together, the cube is asked first and the animation module is next.
 * Both load orders, because dispatch order must come from the priority
 * property and not from which plugin happened to be opened first.
 */
static void
test_chain_both_orders(Fixture *f, gconstpointer data)
{
	GowlModule *cube, *anim;

	if (data == GINT_TO_POINTER(1)) {
		anim = load(f, GOWL_TEST_ANIMATION_MODULE, "animation");
		cube = load(f, GOWL_TEST_CUBE_MODULE, "cube");
	} else {
		cube = load(f, GOWL_TEST_CUBE_MODULE, "cube");
		anim = load(f, GOWL_TEST_ANIMATION_MODULE, "animation");
	}

	g_assert_cmpint(gowl_module_get_priority(cube), <,
	                gowl_module_get_priority(anim));

	g_assert_true(gowl_module_manager_get_scene_effect(f->mgr)
	              == (gpointer)cube);
	g_assert_true(gowl_module_manager_get_scene_effect_after(f->mgr, cube)
	              == (gpointer)anim);

	/* The chain terminates rather than wrapping back to the top: a loop
	 * here would be an infinite delegation on every frame. */
	g_assert_null(gowl_module_manager_get_scene_effect_after(f->mgr, anim));

	/* No module named means "start at the top", so a caller with nothing
	 * to skip gets the ordinary owner. */
	g_assert_true(gowl_module_manager_get_scene_effect_after(f->mgr, NULL)
	              == (gpointer)cube);
}

/* Deactivating the cube hands ownership straight to the animation module,
 * which is what `(gowl-disable-module "cube")' has to do. */
static void
test_deactivating_cube_promotes_animation(Fixture *f, gconstpointer data)
{
	GowlModule *cube = load(f, GOWL_TEST_CUBE_MODULE, "cube");
	GowlModule *anim = load(f, GOWL_TEST_ANIMATION_MODULE, "animation");

	gowl_module_deactivate(cube);

	g_assert_true(gowl_module_manager_get_scene_effect(f->mgr)
	              == (gpointer)anim);
	g_assert_null(gowl_module_manager_get_scene_effect_after(f->mgr, anim));
}

/* And the other way round: with nothing below it the cube still owns the
 * frame, and its delegation has to cope with finding nobody there. */
static void
test_deactivating_animation_empties_the_chain(Fixture *f, gconstpointer data)
{
	GowlModule *cube = load(f, GOWL_TEST_CUBE_MODULE, "cube");
	GowlModule *anim = load(f, GOWL_TEST_ANIMATION_MODULE, "animation");

	gowl_module_deactivate(anim);

	g_assert_true(gowl_module_manager_get_scene_effect(f->mgr)
	              == (gpointer)cube);
	g_assert_null(gowl_module_manager_get_scene_effect_after(f->mgr, cube));
}

/*
 * Every hook the animation module implements, the cube must implement
 * too.
 *
 * The core skips a NULL vfunc on the owning provider; it does not look
 * further down. So a hook the cube leaves unset is not delegated, it is
 * DROPPED -- and the failure mode is that one kind of window animation
 * stops happening while everything else still works, which is close to
 * undiagnosable from a bug report.  This fails the moment a hook is added
 * to one module and not the other.
 */
static void
test_cube_covers_every_hook_animation_has(Fixture *f, gconstpointer data)
{
	GowlModule *cube = load(f, GOWL_TEST_CUBE_MODULE, "cube");
	GowlModule *anim = load(f, GOWL_TEST_ANIMATION_MODULE, "animation");
	GowlSceneEffectInterface *ci =
		GOWL_SCENE_EFFECT_GET_IFACE(GOWL_SCENE_EFFECT(cube));
	GowlSceneEffectInterface *ai =
		GOWL_SCENE_EFFECT_GET_IFACE(GOWL_SCENE_EFFECT(anim));

	g_assert_nonnull(ci);
	g_assert_nonnull(ai);

	if (ai->client_event    != NULL) g_assert_nonnull(ci->client_event);
	if (ai->get_geometry    != NULL) g_assert_nonnull(ci->get_geometry);
	if (ai->alpha_changed   != NULL) g_assert_nonnull(ci->alpha_changed);
	if (ai->surface_at      != NULL) g_assert_nonnull(ci->surface_at);
	if (ai->frame           != NULL) g_assert_nonnull(ci->frame);
	if (ai->frame_done      != NULL) g_assert_nonnull(ci->frame_done);
	if (ai->monitor_removed != NULL) g_assert_nonnull(ci->monitor_removed);
	if (ai->finish          != NULL) g_assert_nonnull(ci->finish);

	/* And they are genuinely different implementations, not the cube
	 * having somehow inherited the animation module's vtable. */
	g_assert_true(ci->frame != ai->frame);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add("/cube-chain/alone", Fixture, NULL,
	           setup, test_cube_alone, teardown);
	g_test_add("/cube-chain/order/cube-first", Fixture, GINT_TO_POINTER(0),
	           setup, test_chain_both_orders, teardown);
	g_test_add("/cube-chain/order/animation-first", Fixture,
	           GINT_TO_POINTER(1), setup, test_chain_both_orders, teardown);
	g_test_add("/cube-chain/deactivate-cube", Fixture, NULL,
	           setup, test_deactivating_cube_promotes_animation, teardown);
	g_test_add("/cube-chain/deactivate-animation", Fixture, NULL,
	           setup, test_deactivating_animation_empties_the_chain,
	           teardown);
	g_test_add("/cube-chain/hook-coverage", Fixture, NULL,
	           setup, test_cube_covers_every_hook_animation_has, teardown);

	return g_test_run();
}
