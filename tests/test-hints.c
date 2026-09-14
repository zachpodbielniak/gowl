/* test-hints.c -- the window-hint overlay
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * THE LABELS NOT MOVING IS THE WHOLE FEATURE, so it is most of what is
 * asserted here.  A hint overlay whose letters are assigned in whatever
 * order the client list happens to hold is not a faster way to reach a
 * window -- it is a lookup table you have to READ every time, which is
 * slower than the relative motion it replaces.  It only pays once the
 * letters are muscle memory, and that needs them to depend on WHERE a
 * window is and on nothing else.
 *
 * That property is invisible in a screenshot and invisible in a diff:
 * the overlay looks correct either way, and the regression is that it
 * stops being worth using.  So the list is shuffled between two opens
 * and the labels are required to be identical.
 *
 * The rest:
 *
 *   IT COULD LABEL WINDOWS NOBODY CAN SEE.  A badge on a window that is
 *   on another tag is a key that appears to do nothing.
 *
 *   IT COULD LET A KEY THROUGH.  The overlay is modal; an unmatched
 *   keystroke reaching the window underneath is a feature that
 *   occasionally types into your editor, which is worse than not having
 *   it at all.
 *
 *   IT COULD RUN OUT OF ALPHABET SILENTLY.  Past one character per
 *   window the labels have to become pairs, and a version that instead
 *   labelled the first twenty-six and left the rest bare would look like
 *   it was working.
 *
 *   IT COULD OUTLIVE ITS WINDOWS.  The overlay waits indefinitely by
 *   default, so a window closing under it is ordinary, and a badge left
 *   pointing at a window that is gone would focus the wrong one.
 *
 * WHAT IS NOT DRIVEN HERE, and deliberately: the keyboard actually
 * moving.  A client a test can fabricate has no wlr_surface, and the
 * module refuses to focus one that has none -- which is a guard worth
 * having in its own right, since the overlay can wait indefinitely and a
 * window can lose its surface while it does.  So a selection here
 * resolves the label, names the window it resolved to, finds it
 * unfocusable and says so, which covers every step this module owns;
 * handing a real window the keyboard is one call into the compositor,
 * which its own tests cover.
 *
 * Needs no GPU.  The overlay is cairo into a raw buffer, so this runs
 * under the pixman renderer -- which is itself worth asserting, since it
 * is the reason this module works on machines where the overview and the
 * switcher do not.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "core/gowl-effects.h"

#ifndef GOWL_TEST_MODULE_DIR
#define GOWL_TEST_MODULE_DIR "build/release/modules"
#endif

#define RIG_MAX_CLIENTS 40

typedef struct {
	gchar             *parent;
	gchar             *runtime;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlCompositor    *compositor;
	gboolean           started;
	GowlClient        *c[RIG_MAX_CLIENTS];
	gint               count;
} Rig;

/* ── The rig ─────────────────────────────────────────────────────── */

static gboolean
rig_up(Rig *r, const gchar *extra_config)
{
	const gchar *parent;
	GError      *error = NULL;
	gchar       *path;

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	/* Nothing may reach the session this runs in, and the runtime
	 * directory goes inside the real one so the socket path stays under
	 * the 108 bytes it gets. */
	r->parent = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	parent = r->parent;
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-hints-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	/* Two outputs: reaching a window on the OTHER screen in one
	 * keystroke is most of what the overlay is for, and the ordering
	 * across outputs cannot be checked with one. */
	g_setenv("WLR_HEADLESS_OUTPUTS", "2", TRUE);
	/* Pixman, not GLES2.  The overlay is cairo into a raw buffer, and
	 * asserting that it works without a GPU is asserting the thing that
	 * makes it available where the overview is not. */
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	r->modules = gowl_module_manager_new();
	path = g_build_filename(GOWL_TEST_MODULE_DIR, "hints.so", NULL);
	if (!gowl_module_manager_load_module(r->modules, path, &error))
		g_error("could not load %s: %s", path, error->message);
	g_free(path);
	gowl_module_manager_activate_all(r->modules);

	r->config = gowl_config_new();
	if (extra_config != NULL) {
		gchar *file = g_build_filename(r->runtime, "gowl.yaml", NULL);

		g_assert_true(g_file_set_contents(file, extra_config, -1, NULL));
		if (!gowl_config_load_yaml(r->config, file, &error))
			g_error("config: %s", error->message);
		g_unlink(file);
		g_free(file);
	}

	r->compositor = gowl_compositor_new();
	gowl_compositor_set_config(r->compositor, r->config);
	gowl_compositor_set_module_manager(r->compositor, r->modules);
	if (!gowl_compositor_start(r->compositor, &error)) {
		g_clear_error(&error);
		return FALSE;
	}
	r->started = TRUE;
	gowl_module_manager_dispatch_startup(r->modules, r->compositor);
	g_assert_nonnull(r->compositor->selmon);
	return TRUE;
}

static void
rig_down(Rig *r)
{
	gint i;

	if (r->started)
		gowl_module_manager_dispatch_shutdown(r->modules, r->compositor);
	for (i = 0; i < r->count; i++) {
		r->compositor->clients = g_list_remove(r->compositor->clients,
		                                       r->c[i]);
		if (r->c[i]->scene != NULL)
			wlr_scene_node_destroy(&r->c[i]->scene->node);
		g_object_unref(r->c[i]);
	}
	g_clear_object(&r->compositor);
	g_clear_object(&r->modules);
	g_clear_object(&r->config);
	if (r->parent != NULL)
		g_setenv("XDG_RUNTIME_DIR", r->parent, TRUE);
	else
		g_unsetenv("XDG_RUNTIME_DIR");
	g_rmdir(r->runtime);
	g_free(r->runtime);
	g_free(r->parent);
	memset(r, 0, sizeof(*r));
}

/* The nth output, in the order the compositor lists them. */
static GowlMonitor *
monitor_at(Rig *r, gint n)
{
	GList *l = g_list_nth(r->compositor->monitors, n);

	return l != NULL ? l->data : NULL;
}

/*
 * A window at @x,@y on @mon, appended to the compositor's list.
 *
 * Deliberately APPENDED, so the list order is creation order -- which is
 * exactly the order the labels must NOT come out in.
 */
static GowlClient *
add_client(Rig *r, GowlMonitor *mon, gint x, gint y, gint w, gint h,
           guint32 tags)
{
	GowlClient *c;

	g_assert_cmpint(r->count, <, RIG_MAX_CLIENTS);
	c = gowl_client_new();
	c->compositor = r->compositor;
	c->mon = mon;
	c->bw = 2;
	c->tags = tags;
	c->geom.x = mon->m.x + x;
	c->geom.y = mon->m.y + y;
	c->geom.width  = w;
	c->geom.height = h;
	c->frame = c->geom;
	c->scene = wlr_scene_tree_create(
		r->compositor->layers[GOWL_SCENE_LAYER_TILE]);
	wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);

	r->compositor->clients = g_list_append(r->compositor->clients, c);
	r->c[r->count++] = c;
	return c;
}

static gchar *
ipc(Rig *r, const gchar *command, const gchar *args)
{
	return gowl_module_manager_dispatch_command(r->modules, command, args);
}

static gboolean
key(Rig *r, guint keysym)
{
	return gowl_module_manager_dispatch_key(r->modules, 0, keysym, TRUE);
}

/* Every buffer the overlay put in the overlay layer. */
static guint
overlay_buffers(Rig *r)
{
	struct wlr_scene_node *child;
	guint                  n = 0;

	wl_list_for_each(child,
	                 &r->compositor->layers[GOWL_SCENE_LAYER_OVERLAY]->children,
	                 link) {
		if (child->type == WLR_SCENE_NODE_BUFFER)
			n++;
	}
	return n;
}

/* The label `hints-list' gave the window with @id, or NULL. */
static gchar *
label_of(const gchar *listing, guint id)
{
	g_auto(GStrv) lines = g_strsplit(listing, "\n", -1);
	guint i;

	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) f = g_strsplit(lines[i], " ", 3);

		if (f[0] == NULL || f[1] == NULL)
			continue;
		if ((guint)g_ascii_strtoull(f[1], NULL, 10) == id)
			return g_strdup(f[0]);
	}
	return NULL;
}

/* ── The alphabet ────────────────────────────────────────────────── */

/*
 * Pure arithmetic, no compositor: whether a candidate alphabet can label
 * windows at all.
 *
 * A REPEATED character is the one that matters.  It gives two windows
 * the same label, so one of them can never be reached -- and it is a
 * plausible typo in a config rather than an exotic case.  Refusing it
 * and keeping the default is the difference between a config with a typo
 * in it and an overlay that appears to have a window missing.
 */
static void
test_the_alphabet_is_checked(void)
{
	g_assert_true(gowl_config_hints_keys_valid("asdf"));
	g_assert_true(gowl_config_hints_keys_valid("ab"));
	/* Case-insensitive, because the labels are matched that way: `aA'
	 * is the same character twice. */
	g_assert_false(gowl_config_hints_keys_valid("aA"));
	g_assert_false(gowl_config_hints_keys_valid("asdfa"));
	/* One character cannot even make a pair, so it would cap the
	 * overlay at a single window. */
	g_assert_false(gowl_config_hints_keys_valid("a"));
	g_assert_false(gowl_config_hints_keys_valid(""));
	g_assert_false(gowl_config_hints_keys_valid(NULL));
	/* A label has to be a key somebody can press. */
	g_assert_false(gowl_config_hints_keys_valid("as df"));
	g_assert_false(gowl_config_hints_keys_valid("as\tdf"));
}

/*
 * And a bad one is refused rather than taken, so the overlay keeps
 * working.  A config that silently accepted `aa' would leave one window
 * permanently unreachable, which is a far worse outcome than a warning.
 */
static void
test_a_bad_alphabet_is_not_taken(void)
{
	g_autoptr(GowlConfig) config = gowl_config_new();
	const gchar *before = gowl_config_get_hints_keys(config);
	g_autofree gchar *kept = g_strdup(before);

	gowl_config_set_hints_keys(config, "aa");
	g_assert_cmpstr(gowl_config_get_hints_keys(config), ==, kept);
	gowl_config_set_hints_keys(config, "z");
	g_assert_cmpstr(gowl_config_get_hints_keys(config), ==, kept);

	/* And a good one is, lowercased, since that is how keys arrive. */
	gowl_config_set_hints_keys(config, "QWER");
	g_assert_cmpstr(gowl_config_get_hints_keys(config), ==, "qwer");
}

/* ── The overlay ─────────────────────────────────────────────────── */

static void
test_every_visible_window_gets_a_badge(void)
{
	Rig r;
	g_autofree gchar *reply = NULL;
	g_autofree gchar *listing = NULL;

	if (!rig_up(&r, NULL)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}
	g_assert_nonnull(monitor_at(&r, 1));

	add_client(&r, monitor_at(&r, 0), 0, 0, 400, 600, 1);
	add_client(&r, monitor_at(&r, 0), 400, 0, 400, 300, 1);
	add_client(&r, monitor_at(&r, 1), 0, 0, 400, 600, 1);

	g_assert_cmpuint(overlay_buffers(&r), ==, 0);
	reply = ipc(&r, "hints", NULL);
	g_assert_cmpstr(reply, ==, "OK hints shown");

	/* One badge per window, and on BOTH outputs: the whole point is
	 * reaching the other screen without aiming at it. */
	g_assert_cmpuint(overlay_buffers(&r), ==, 3);

	listing = ipc(&r, "hints-list", NULL);
	g_assert_nonnull(listing);
	{
		g_autofree gchar *a = label_of(listing, gowl_client_get_id(r.c[0]));
		g_autofree gchar *b = label_of(listing, gowl_client_get_id(r.c[1]));
		g_autofree gchar *c = label_of(listing, gowl_client_get_id(r.c[2]));

		g_assert_nonnull(a);
		g_assert_nonnull(b);
		g_assert_nonnull(c);
		/* Home row first, so the first three windows are under resting
		 * fingers.  This is the part a user forms muscle memory on. */
		g_assert_cmpstr(a, ==, "a");
		g_assert_cmpstr(b, ==, "s");
		g_assert_cmpstr(c, ==, "d");
	}

	rig_down(&r);
}

/*
 * A window on a tag nobody is looking at gets no badge.
 *
 * A key that appears to do nothing is worse than a key that is not
 * offered: the letters after it would shift as soon as the tag came
 * back, which is exactly the instability the ordering exists to avoid.
 */
static void
test_hidden_windows_are_not_labelled(void)
{
	Rig r;
	g_autofree gchar *reply = NULL;

	if (!rig_up(&r, NULL)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	add_client(&r, monitor_at(&r, 0), 0, 0, 400, 600, 1);
	/* Tag 2, while the monitor is showing tag 1. */
	add_client(&r, monitor_at(&r, 0), 400, 0, 400, 600, 2);

	reply = ipc(&r, "hints", NULL);
	g_assert_cmpstr(reply, ==, "OK hints shown");
	g_assert_cmpuint(overlay_buffers(&r), ==, 1);

	rig_down(&r);
}

/*
 * THE ONE THAT MATTERS: a window's letter depends on where it IS.
 *
 * The overlay is opened, the compositor's client list is then reversed
 * -- which is what closing and reopening windows does to it over a
 * session -- and the overlay is opened again.  Every window has to come
 * back wearing the letter it had.
 *
 * Assigned from the list order instead, every letter would change, and
 * nothing else in this file would notice: the badges would still be
 * drawn, still be one per window, still focus what they point at.  The
 * feature would simply have stopped being faster than counting hops.
 */
static void
test_the_labels_do_not_move(void)
{
	Rig r;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *close1 = NULL;
	g_autofree gchar *close2 = NULL;
	guint i;

	if (!rig_up(&r, NULL)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	/* Two columns on the first screen and two rows on the second, added
	 * in an order that has nothing to do with either. */
	add_client(&r, monitor_at(&r, 1), 0, 0,   800, 300, 1);
	add_client(&r, monitor_at(&r, 0), 400, 0, 400, 600, 1);
	add_client(&r, monitor_at(&r, 1), 0, 300, 800, 300, 1);
	add_client(&r, monitor_at(&r, 0), 0, 0,   400, 600, 1);

	g_free(ipc(&r, "hints-show", NULL));
	first = ipc(&r, "hints-list", NULL);
	g_assert_nonnull(first);
	close1 = ipc(&r, "hints-hide", NULL);

	/*
	 * The order the labels SHOULD come out in, which is neither the
	 * order the windows were added in nor the order the list holds
	 * them: leftmost output first, and within an output the left column
	 * before the right.
	 */
	{
		g_autofree gchar *l0 = label_of(first, gowl_client_get_id(r.c[0]));
		g_autofree gchar *l1 = label_of(first, gowl_client_get_id(r.c[1]));
		g_autofree gchar *l2 = label_of(first, gowl_client_get_id(r.c[2]));
		g_autofree gchar *l3 = label_of(first, gowl_client_get_id(r.c[3]));

		/* Screen one: the left column, then the right. */
		g_assert_cmpstr(l3, ==, "a");
		g_assert_cmpstr(l1, ==, "s");
		/* Screen two: the top row, then the bottom. */
		g_assert_cmpstr(l0, ==, "d");
		g_assert_cmpstr(l2, ==, "f");
	}

	/* Now shuffle the list under it. */
	r.compositor->clients = g_list_reverse(r.compositor->clients);

	g_free(ipc(&r, "hints-show", NULL));
	second = ipc(&r, "hints-list", NULL);
	g_assert_nonnull(second);
	close2 = ipc(&r, "hints-hide", NULL);

	for (i = 0; i < (guint)r.count; i++) {
		guint id = gowl_client_get_id(r.c[i]);
		g_autofree gchar *was = label_of(first, id);
		g_autofree gchar *now = label_of(second, id);

		g_assert_nonnull(was);
		g_assert_nonnull(now);
		if (g_strcmp0(was, now) != 0) {
			g_error("window %u wore '%s' and now wears '%s': the labels "
			        "are following the client list, not the layout",
			        id, was, now);
		}
	}

	rig_down(&r);
}

/*
 * Pressing a letter picks the window wearing it, and only that window.
 *
 * The focus itself is not driven -- see the note at the top of the file
 * -- so this asserts through `hints-select', which names the window it
 * resolved to before handing it over.
 */
static void
test_a_letter_picks_its_window(void)
{
	Rig r;
	g_autofree gchar *listing = NULL;
	g_autofree gchar *bad = NULL;
	g_autofree gchar *reply = NULL;

	if (!rig_up(&r, NULL)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	add_client(&r, monitor_at(&r, 0), 0, 0,   400, 600, 1);
	add_client(&r, monitor_at(&r, 0), 400, 0, 400, 600, 1);

	g_free(ipc(&r, "hints-show", NULL));
	listing = ipc(&r, "hints-list", NULL);

	/* A label nothing wears is refused, and the overlay stays up: a
	 * script that asked for the wrong thing should not also have taken
	 * the overlay down. */
	bad = ipc(&r, "hints-select", "zz");
	g_assert_cmpstr(bad, ==, "ERROR no such hint");
	g_assert_cmpuint(overlay_buffers(&r), ==, 2);

	{
		g_autofree gchar *want =
			label_of(listing, gowl_client_get_id(r.c[1]));

		/* Which window `s' resolves to is the part this module
		 * decides, and it is the RIGHT window: the second column, not
		 * the second one in the list. */
		g_assert_cmpstr(want, ==, "s");
		reply = ipc(&r, "hints-select", want);
		/* A fabricated client has no surface, so the selection refuses
		 * it at the last step -- see the note at the top.  Everything
		 * before that step has run. */
		g_assert_cmpstr(reply, ==, "ERROR that window went away");
	}
	/* And the overlay is gone either way.  The badges come down BEFORE
	 * the window is raised, or the chosen window comes forward under a
	 * rectangle. */
	g_assert_cmpuint(overlay_buffers(&r), ==, 0);

	rig_down(&r);
}

/*
 * NOTHING gets past the overlay.
 *
 * It is modal, and a keystroke meant for it that also reaches the
 * surface underneath is the failure that makes a hint mode unusable:
 * every miss types a character into whatever was focused.  The module
 * has to claim the key whether or not it means anything to it.
 */
static void
test_every_key_is_swallowed(void)
{
	Rig r;

	if (!rig_up(&r, NULL)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	add_client(&r, monitor_at(&r, 0), 0, 0, 400, 600, 1);
	add_client(&r, monitor_at(&r, 0), 400, 0, 400, 600, 1);

	/* Closed, the module claims nothing: a compositor that swallowed
	 * keys while the overlay was down would be unusable in a different
	 * way. */
	g_assert_false(key(&r, XKB_KEY_x));
	g_assert_false(key(&r, XKB_KEY_Escape));

	g_free(ipc(&r, "hints-show", NULL));

	/* A letter no window wears, a function key, a modifier, a digit. */
	g_assert_true(gowl_module_manager_dispatch_key(r.modules, 0,
	                                                XKB_KEY_F5, TRUE));
	g_assert_true(gowl_module_manager_dispatch_key(r.modules, 0,
	                                                XKB_KEY_Shift_L, TRUE));
	/* The release of whatever opened it, which must not be read as a
	 * label -- the first thing to reach a module after the key that
	 * opened the overlay is that key coming back up. */
	g_assert_true(gowl_module_manager_dispatch_key(r.modules, 0,
	                                                XKB_KEY_a, FALSE));
	g_assert_cmpuint(overlay_buffers(&r), ==, 2);

	/* A letter outside the alphabet cancels rather than sitting there,
	 * which is what tells somebody they hit the wrong key. */
	g_assert_true(key(&r, XKB_KEY_1));
	g_assert_cmpuint(overlay_buffers(&r), ==, 0);
	g_assert_false(key(&r, XKB_KEY_a));

	/* Escape cancels too. */
	g_free(ipc(&r, "hints-show", NULL));
	g_assert_cmpuint(overlay_buffers(&r), ==, 2);
	g_assert_true(key(&r, XKB_KEY_Escape));
	g_assert_cmpuint(overlay_buffers(&r), ==, 0);

	rig_down(&r);
}

/*
 * More windows than letters: the labels become PAIRS rather than running
 * out.
 *
 * A version that labelled the first twenty-six and left the rest bare
 * would look like it was working, which is why this is worth a case of
 * its own.  The alphabet is cut to two characters so three windows are
 * enough to force it.
 */
static void
test_pairs_when_the_alphabet_runs_out(void)
{
	Rig r;
	g_autofree gchar *listing = NULL;
	guint i;

	if (!rig_up(&r, "hints-keys: \"ab\"\n")) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}
	g_assert_cmpstr(gowl_config_get_hints_keys(r.config), ==, "ab");

	add_client(&r, monitor_at(&r, 0), 0, 0,   300, 600, 1);
	add_client(&r, monitor_at(&r, 0), 300, 0, 300, 600, 1);
	add_client(&r, monitor_at(&r, 0), 600, 0, 300, 600, 1);

	g_free(ipc(&r, "hints-show", NULL));
	listing = ipc(&r, "hints-list", NULL);
	g_assert_nonnull(listing);

	for (i = 0; i < 3; i++) {
		g_autofree gchar *l =
			label_of(listing, gowl_client_get_id(r.c[i]));

		g_assert_nonnull(l);
		/* Every label the same length, which is what makes no label a
		 * prefix of another and lets a pair be matched without a
		 * terminator. */
		g_assert_cmpuint((guint)strlen(l), ==, 2);
	}

	/*
	 * The first key NARROWS and does not pick.  A two-character overlay
	 * that selected on the first keystroke would focus a window nobody
	 * asked for.
	 */
	g_assert_true(key(&r, XKB_KEY_a));
	g_assert_cmpuint(overlay_buffers(&r), ==, 3);

	/* Backspace takes it back rather than cancelling: a mistyped first
	 * key should not cost the whole overlay. */
	g_assert_true(key(&r, XKB_KEY_BackSpace));
	g_assert_cmpuint(overlay_buffers(&r), ==, 3);

	/* And the pair completes it. */
	g_assert_true(key(&r, XKB_KEY_a));
	g_assert_true(key(&r, XKB_KEY_a));
	g_assert_cmpuint(overlay_buffers(&r), ==, 0);

	rig_down(&r);
}

/*
 * A window closing under the overlay takes the whole thing down.
 *
 * Not tidiness: every label after the missing one would shift, which is
 * the one thing the ordering exists to prevent -- and doing it under
 * somebody's fingers mid-keystroke is how a hint mode focuses the wrong
 * window.
 */
static void
test_a_window_going_away_closes_it(void)
{
	Rig r;

	if (!rig_up(&r, NULL)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	add_client(&r, monitor_at(&r, 0), 0, 0,   400, 600, 1);
	add_client(&r, monitor_at(&r, 0), 400, 0, 400, 600, 1);

	g_free(ipc(&r, "hints-show", NULL));
	g_assert_cmpuint(overlay_buffers(&r), ==, 2);

	gowl_effects_client_event(r.compositor, r.c[1],
	                          GOWL_SCENE_EFFECT_DESTROY, NULL, FALSE);
	g_assert_cmpuint(overlay_buffers(&r), ==, 0);

	rig_down(&r);
}

/*
 * A badge follows its window, and sits exactly on it.
 *
 * The overlay can wait indefinitely, so a window resizing under it is
 * ordinary -- and a badge left at the old place is worse than no badge,
 * because it points at the wrong window.
 */
static void
test_a_badge_sits_on_its_window(void)
{
	Rig r;
	struct wlr_scene_node *child;
	gboolean found = FALSE;

	if (!rig_up(&r, NULL)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	add_client(&r, monitor_at(&r, 0), 120, 90, 400, 300, 1);
	g_free(ipc(&r, "hints-show", NULL));
	g_assert_cmpuint(overlay_buffers(&r), ==, 1);

	/* Where the window is, not where its unclipped geometry says: a
	 * scrolling layout keeps `geom' hanging off the screen on purpose. */
	r.c[0]->geom.x += 260;
	r.c[0]->geom.width = 260;
	r.c[0]->frame = r.c[0]->geom;
	gowl_effects_client_placed(r.compositor, r.c[0]);

	wl_list_for_each(child,
	                 &r.compositor->layers[GOWL_SCENE_LAYER_OVERLAY]->children,
	                 link) {
		struct wlr_scene_buffer *buf;

		if (child->type != WLR_SCENE_NODE_BUFFER)
			continue;
		buf = wlr_scene_buffer_from_node(child);
		g_assert_cmpint(child->x, ==, r.c[0]->frame.x);
		g_assert_cmpint(child->y, ==, r.c[0]->frame.y);
		g_assert_cmpint(buf->dst_width, ==, r.c[0]->frame.width);
		g_assert_cmpint(buf->dst_height, ==, r.c[0]->frame.height);
		/* And it takes no pointer input: a click during the overlay
		 * belongs to whatever is underneath. */
		g_assert_nonnull(buf->point_accepts_input);
		found = TRUE;
	}
	g_assert_true(found);

	rig_down(&r);
}

/* Only the focused output, when asked.  Off by default, because
 * reaching the other screen is most of what the overlay is for. */
static void
test_one_output_only_when_asked(void)
{
	Rig r;

	if (!rig_up(&r, "hints-current-output: true\n")) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}
	g_assert_true(gowl_config_get_hints_current_output(r.config));

	add_client(&r, r.compositor->selmon, 0, 0, 400, 600, 1);
	add_client(&r, monitor_at(&r, 0) == r.compositor->selmon
	           ? monitor_at(&r, 1) : monitor_at(&r, 0),
	           0, 0, 400, 600, 1);

	g_free(ipc(&r, "hints-show", NULL));
	g_assert_cmpuint(overlay_buffers(&r), ==, 1);

	rig_down(&r);
}

/* Nothing to label is not an error worth drawing an empty overlay for. */
static void
test_an_empty_desktop_shows_nothing(void)
{
	Rig r;
	g_autofree gchar *reply = NULL;

	if (!rig_up(&r, NULL)) {
		rig_down(&r);
		g_test_skip("no headless compositor");
		return;
	}

	reply = ipc(&r, "hints", NULL);
	g_assert_cmpstr(reply, ==, "ERROR no windows to label");
	g_assert_cmpuint(overlay_buffers(&r), ==, 0);

	rig_down(&r);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/hints/the-alphabet-is-checked",
	                test_the_alphabet_is_checked);
	g_test_add_func("/hints/a-bad-alphabet-is-not-taken",
	                test_a_bad_alphabet_is_not_taken);
	g_test_add_func("/hints/every-visible-window-gets-a-badge",
	                test_every_visible_window_gets_a_badge);
	g_test_add_func("/hints/hidden-windows-are-not-labelled",
	                test_hidden_windows_are_not_labelled);
	g_test_add_func("/hints/the-labels-do-not-move",
	                test_the_labels_do_not_move);
	g_test_add_func("/hints/a-letter-picks-its-window",
	                test_a_letter_picks_its_window);
	g_test_add_func("/hints/every-key-is-swallowed",
	                test_every_key_is_swallowed);
	g_test_add_func("/hints/pairs-when-the-alphabet-runs-out",
	                test_pairs_when_the_alphabet_runs_out);
	g_test_add_func("/hints/a-window-going-away-closes-it",
	                test_a_window_going_away_closes_it);
	g_test_add_func("/hints/a-badge-sits-on-its-window",
	                test_a_badge_sits_on_its_window);
	g_test_add_func("/hints/one-output-only-when-asked",
	                test_one_output_only_when_asked);
	g_test_add_func("/hints/an-empty-desktop-shows-nothing",
	                test_an_empty_desktop_shows_nothing);

	return g_test_run();
}
