/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * test-seat-keyboard.c - When the seat advertises wl_keyboard.
 *
 * The capability used to mean "a physical keyboard is plugged in".  On
 * a machine with none -- a tablet, a headset, a headless session -- the
 * seat therefore never offered wl_keyboard at all: keys typed on an
 * on-screen keyboard or by wtype, which arrive through a virtual
 * keyboard, reached no client, because no client had a wl_keyboard to
 * get them on; and a GTK window, which learns that it has focus from
 * wl_keyboard.enter, never enabled its text input.
 *
 * A headless compositor, with software devices plugged into its backend
 * the way libinput plugs real ones in, and virtual keyboards announced
 * on the virtual-keyboard manager's own signal -- the same call a
 * client's create_virtual_keyboard request ends in, so the handlers
 * under test are the real ones.
 *
 * Covers: no keyboard, no capability; a virtual keyboard is one, for as
 * long as it lives, and the last of several is the one that takes the
 * capability away; hardware still counts, and plugging a mouse in does
 * not take a virtual keyboard's capability away (two copies of the rule
 * once could); the `force-seat-keyboard' setting at startup and at run
 * time; GOWL_SEAT_KEYBOARD in the environment over the config, both
 * ways; the pointer capability from the start.
 *
 * Not covered here: a real client binding wl_keyboard and receiving
 * wl_keyboard.enter.  test-macro-targeted-key.c does that with the
 * capability present.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>

#include "gowl.h"
#include "core/gowl-core-private.h"

/* ── Fake devices ───────────────────────────────────────────────── */

static struct wlr_keyboard_impl fake_keyboard_impl;
static struct wlr_pointer_impl  fake_pointer_impl;

/* ── The rig ────────────────────────────────────────────────────── */

typedef struct {
	gchar             *runtime;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
	GPtrArray         *keyboards;   /* struct wlr_keyboard*, owned */
	GPtrArray         *pointers;    /* struct wlr_pointer*, owned */
	GPtrArray         *virtuals;    /* struct wlr_virtual_keyboard_v1* */
	gboolean           up;
} Rig;

/* What a test asks of the rig before the compositor starts. */
typedef struct {
	gboolean     force;         /* the config's force-seat-keyboard */
	const gchar *environment;   /* GOWL_SEAT_KEYBOARD, or NULL to unset */
} Setup;

static void
rig_setup(
	Rig           *r,
	gconstpointer  data
){
	const Setup *setup = data;
	const gchar *parent;
	GError *error = NULL;

	memset(r, 0, sizeof *r);
	r->keyboards = g_ptr_array_new();
	r->pointers = g_ptr_array_new();
	r->virtuals = g_ptr_array_new();
	fake_keyboard_impl.name = "gowl-test-keyboard";
	fake_pointer_impl.name = "gowl-test-pointer";

	parent = g_getenv("XDG_RUNTIME_DIR");
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-seatkb-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);
	if (setup->environment != NULL)
		g_setenv("GOWL_SEAT_KEYBOARD", setup->environment, TRUE);
	else
		g_unsetenv("GOWL_SEAT_KEYBOARD");

	r->config = gowl_config_new();
	gowl_config_set_lock_command(r->config, "");
	gowl_config_set_force_seat_keyboard(r->config, setup->force);
	r->modules = gowl_module_manager_new();
	gowl_module_manager_activate_all(r->modules);

	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_test_skip("no headless compositor here");
		g_clear_error(&error);
		return;
	}
	gowl_module_manager_dispatch_startup(r->modules, r->compositor);
	r->up = TRUE;
}

static void
rig_teardown(
	Rig           *r,
	gconstpointer  data
){
	guint i;

	(void)data;
	/* Devices first, while the compositor they were handed to exists */
	for (i = 0; i < r->virtuals->len; i++) {
		struct wlr_virtual_keyboard_v1 *vkb;

		vkb = g_ptr_array_index(r->virtuals, i);
		wlr_keyboard_finish(&vkb->keyboard);
		g_free(vkb);
	}
	for (i = 0; i < r->keyboards->len; i++) {
		struct wlr_keyboard *kb = g_ptr_array_index(r->keyboards, i);

		wlr_keyboard_finish(kb);
		g_free(kb);
	}
	for (i = 0; i < r->pointers->len; i++) {
		struct wlr_pointer *p = g_ptr_array_index(r->pointers, i);

		wlr_pointer_finish(p);
		g_free(p);
	}
	g_ptr_array_unref(r->virtuals);
	g_ptr_array_unref(r->keyboards);
	g_ptr_array_unref(r->pointers);
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	if (r->runtime != NULL)
		g_rmdir(r->runtime);
	g_free(r->runtime);
	g_unsetenv("GOWL_SEAT_KEYBOARD");
}

#define RIG_UP(r) do { if (!(r)->up) return; } while (0)

static guint32
caps(Rig *r)
{
	return r->compositor->wlr_seat->capabilities;
}

#define ASSERT_KEYBOARD(r) \
	g_assert_true(caps(r) & WL_SEAT_CAPABILITY_KEYBOARD)
#define ASSERT_NO_KEYBOARD(r) \
	g_assert_false(caps(r) & WL_SEAT_CAPABILITY_KEYBOARD)

/* Plugs a keyboard in, as the backend's new_input would. */
static struct wlr_keyboard *
plug_keyboard(
	Rig         *r,
	const gchar *name
){
	struct wlr_keyboard *kb;

	kb = g_new0(struct wlr_keyboard, 1);
	wlr_keyboard_init(kb, &fake_keyboard_impl, name);
	g_ptr_array_add(r->keyboards, kb);
	wl_signal_emit_mutable(&r->compositor->backend->events.new_input,
	                       &kb->base);
	return kb;
}

static void
plug_pointer(
	Rig         *r,
	const gchar *name
){
	struct wlr_pointer *p;

	p = g_new0(struct wlr_pointer, 1);
	wlr_pointer_init(p, &fake_pointer_impl, name);
	g_ptr_array_add(r->pointers, p);
	wl_signal_emit_mutable(&r->compositor->backend->events.new_input,
	                       &p->base);
}

/*
 * A client created a virtual keyboard.  wlroots builds the object and
 * emits the manager's new_virtual_keyboard with it; this does the same
 * with an object of the test's own, which is all the compositor's
 * handler is given either way.  No client stands behind it (resource
 * is NULL), which the handlers only look at while an input method is
 * running.
 */
static struct wlr_virtual_keyboard_v1 *
create_virtual_keyboard(
	Rig         *r,
	const gchar *name
){
	struct wlr_virtual_keyboard_manager_v1 *mgr;
	struct wlr_virtual_keyboard_v1 *vkb;

	mgr = gowl_text_input_virtual_keyboard_manager(r->compositor);
	g_assert_nonnull(mgr);

	vkb = g_new0(struct wlr_virtual_keyboard_v1, 1);
	wlr_keyboard_init(&vkb->keyboard, &fake_keyboard_impl, name);
	vkb->seat = r->compositor->wlr_seat;
	wl_list_init(&vkb->link);
	g_ptr_array_add(r->virtuals, vkb);
	wl_signal_emit_mutable(&mgr->events.new_virtual_keyboard, vkb);
	return vkb;
}

/* The client went away: the keyboard is destroyed. */
static void
destroy_virtual_keyboard(
	Rig                            *r,
	struct wlr_virtual_keyboard_v1 *vkb
){
	g_assert_true(g_ptr_array_remove(r->virtuals, vkb));
	wlr_keyboard_finish(&vkb->keyboard);
	g_free(vkb);
}

/* ── Tests ──────────────────────────────────────────────────────── */

/* Nothing plugged in, nothing forced: a pointer and no keyboard.  The
 * pointer is there from the start, not from the first hotplug. */
static void
test_no_keyboard_no_capability(
	Rig           *r,
	gconstpointer  data
){
	(void)data;
	RIG_UP(r);
	g_assert_true(caps(r) & WL_SEAT_CAPABILITY_POINTER);
	ASSERT_NO_KEYBOARD(r);
	g_assert_false(gowl_compositor_seat_has_keyboard(r->compositor));
	g_assert_false(gowl_compositor_get_force_seat_keyboard(r->compositor));
}

/* THE BUG: a virtual keyboard on a keyboardless seat is a keyboard. */
static void
test_virtual_keyboard_counts(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_virtual_keyboard_v1 *vkb;

	(void)data;
	RIG_UP(r);
	ASSERT_NO_KEYBOARD(r);
	g_assert_false(gowl_text_input_has_virtual_keyboard(r->compositor));

	vkb = create_virtual_keyboard(r, "on-screen keyboard");
	g_assert_true(gowl_text_input_has_virtual_keyboard(r->compositor));
	ASSERT_KEYBOARD(r);
	/* ...and it did not join the hardware group, which is what keeps
	 * the input recorder from mistaking it for hardware */
	g_assert_null(vkb->keyboard.group);
	g_assert_true(wl_list_empty(&r->compositor->wlr_kb_group->devices));

	destroy_virtual_keyboard(r, vkb);
	g_assert_false(gowl_text_input_has_virtual_keyboard(r->compositor));
	ASSERT_NO_KEYBOARD(r);
}

/* Two of them: the capability goes with the last, not the first. */
static void
test_last_virtual_keyboard_takes_it(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_virtual_keyboard_v1 *a;
	struct wlr_virtual_keyboard_v1 *b;

	(void)data;
	RIG_UP(r);
	a = create_virtual_keyboard(r, "a");
	b = create_virtual_keyboard(r, "b");
	ASSERT_KEYBOARD(r);
	destroy_virtual_keyboard(r, a);
	ASSERT_KEYBOARD(r);
	destroy_virtual_keyboard(r, b);
	ASSERT_NO_KEYBOARD(r);
}

/* Hardware counts as it always did, and outlives a virtual keyboard. */
static void
test_hardware_keyboard_counts(
	Rig           *r,
	gconstpointer  data
){
	struct wlr_virtual_keyboard_v1 *vkb;

	(void)data;
	RIG_UP(r);
	plug_keyboard(r, "Test Keyboard");
	ASSERT_KEYBOARD(r);
	vkb = create_virtual_keyboard(r, "wtype");
	ASSERT_KEYBOARD(r);
	destroy_virtual_keyboard(r, vkb);
	ASSERT_KEYBOARD(r);
}

/*
 * Every path that decides the capabilities uses the one rule.  The
 * hotplug path and the remap path each used to compute them for
 * themselves, from hardware alone: with that, plugging a mouse in --
 * or reloading the config -- took wl_keyboard away from under an
 * on-screen keyboard.
 */
static void
test_other_paths_keep_a_virtual_keyboard(
	Rig           *r,
	gconstpointer  data
){
	(void)data;
	RIG_UP(r);
	create_virtual_keyboard(r, "on-screen keyboard");
	ASSERT_KEYBOARD(r);

	plug_pointer(r, "Test Mouse");
	ASSERT_KEYBOARD(r);
	gowl_compositor_input_remap_reevaluate(r->compositor);
	ASSERT_KEYBOARD(r);
	gowl_compositor_set_config(r->compositor, r->config);
	ASSERT_KEYBOARD(r);
}

/* force-seat-keyboard in the config: there before any client connects. */
static void
test_forced_by_config(
	Rig           *r,
	gconstpointer  data
){
	(void)data;
	RIG_UP(r);
	g_assert_true(gowl_compositor_get_force_seat_keyboard(r->compositor));
	g_assert_true(gowl_compositor_seat_has_keyboard(r->compositor));
	ASSERT_KEYBOARD(r);
	g_assert_true(caps(r) & WL_SEAT_CAPABILITY_POINTER);
	/* no keyboard was invented to get there */
	g_assert_true(wl_list_empty(&r->compositor->wlr_kb_group->devices));
	g_assert_false(gowl_text_input_has_virtual_keyboard(r->compositor));

	/* A mouse arriving changes nothing */
	plug_pointer(r, "Test Mouse");
	ASSERT_KEYBOARD(r);
}

/* ...and switched at run time, both ways. */
static void
test_forced_at_run_time(
	Rig           *r,
	gconstpointer  data
){
	(void)data;
	RIG_UP(r);
	ASSERT_NO_KEYBOARD(r);

	gowl_compositor_set_force_seat_keyboard(r->compositor, TRUE);
	g_assert_true(gowl_config_get_force_seat_keyboard(r->config));
	ASSERT_KEYBOARD(r);

	gowl_compositor_set_force_seat_keyboard(r->compositor, FALSE);
	g_assert_false(gowl_config_get_force_seat_keyboard(r->config));
	ASSERT_NO_KEYBOARD(r);

	/* Automatic again means automatic: a keyboard still brings it */
	plug_keyboard(r, "Test Keyboard");
	ASSERT_KEYBOARD(r);
}

/* GOWL_SEAT_KEYBOARD=always over a config that says automatic. */
static void
test_environment_forces(
	Rig           *r,
	gconstpointer  data
){
	(void)data;
	RIG_UP(r);
	g_assert_false(gowl_config_get_force_seat_keyboard(r->config));
	g_assert_true(gowl_compositor_get_force_seat_keyboard(r->compositor));
	ASSERT_KEYBOARD(r);
}

/* GOWL_SEAT_KEYBOARD=auto over a config that forces. */
static void
test_environment_unforces(
	Rig           *r,
	gconstpointer  data
){
	(void)data;
	RIG_UP(r);
	g_assert_true(gowl_config_get_force_seat_keyboard(r->config));
	g_assert_false(gowl_compositor_get_force_seat_keyboard(r->compositor));
	ASSERT_NO_KEYBOARD(r);
	/* automatic, so a virtual keyboard still counts */
	create_virtual_keyboard(r, "wtype");
	ASSERT_KEYBOARD(r);
}

int
main(
	int    argc,
	char **argv
){
	static const Setup automatic    = { FALSE, NULL };
	static const Setup forced       = { TRUE,  NULL };
	static const Setup env_always   = { FALSE, "always" };
	static const Setup env_auto     = { TRUE,  "auto" };

	g_test_init(&argc, &argv, NULL);

#define ADD(path, setup, fn) \
	g_test_add("/seat-keyboard/" path, Rig, setup, \
	           rig_setup, fn, rig_teardown)

	ADD("no-keyboard-no-capability", &automatic,
	    test_no_keyboard_no_capability);
	ADD("virtual-keyboard-counts", &automatic,
	    test_virtual_keyboard_counts);
	ADD("last-virtual-keyboard-takes-it", &automatic,
	    test_last_virtual_keyboard_takes_it);
	ADD("hardware-keyboard-counts", &automatic,
	    test_hardware_keyboard_counts);
	ADD("other-paths-keep-a-virtual-keyboard", &automatic,
	    test_other_paths_keep_a_virtual_keyboard);
	ADD("forced-by-config", &forced, test_forced_by_config);
	ADD("forced-at-run-time", &automatic, test_forced_at_run_time);
	ADD("environment-forces", &env_always, test_environment_forces);
	ADD("environment-unforces", &env_auto, test_environment_unforces);

#undef ADD
	return g_test_run();
}
