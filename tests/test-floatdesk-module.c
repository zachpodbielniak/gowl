/* test-floatdesk-module.c -- the floating desktop against a compositor
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * tests/test-floatdesk-state.c proves the decisions are right; this
 * proves the module carries them out, with the real floatdesk.so and
 * tile.so loaded into a started headless compositor.  The mode is
 * entered and left the ways a user does it -- the toggle command, the
 * layout cycle, the default switch -- and windows are moved in and out
 * of a floating workspace the ways the compositor moves them.
 *
 * The rig is tests/test-window-below.c's: fabricated windows with no
 * client behind them, and gowl_compositor_place_client() interposed so
 * a layout's (or the module's) placement records a box instead of
 * configuring an xdg_toplevel that does not exist.  The executable is
 * linked with --export-dynamic, so the module's calls resolve here too.
 * The two seat calls focus makes are interposed for the same reason.
 *
 * What it asserts is what the user would see go wrong: a window left
 * floating after the mode ended, a dialog tiled by it, a window moved to
 * a floating workspace that stays a tile, a mode forgotten across a
 * restart, a new window opening as a tile.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>

#include "gowl.h"
#include "core/gowl-core-private.h"
#include "core/gowl-layout-registry.h"
/* Compiled in to read back the state file the module saved.  The same
 * source the module links, so the module's calls resolving to these
 * copies (the executable exports its symbols) changes nothing. */
#include "../modules/floatdesk/gowl-floatdesk-state.c"

#ifndef GOWL_TEST_MODULE_DIR
#define GOWL_TEST_MODULE_DIR "build/release/modules"
#endif

#define RIG_MAX_CLIENTS 8
#define TAG(n) ((guint32)1u << ((n) - 1))

/* Records the box instead of configuring a toplevel (see above). */
void
gowl_compositor_place_client(GowlCompositor *self, GowlClient *client,
                             gint x, gint y, gint width, gint height)
{
	(void)self;
	client->geom = (struct wlr_box){ x, y, width, height };
	client->frame = client->geom;
}

void
wlr_seat_keyboard_notify_enter(struct wlr_seat *seat,
                               struct wlr_surface *surface,
                               const uint32_t *keycodes, size_t num_keycodes,
                               const struct wlr_keyboard_modifiers *modifiers)
{
	(void)seat; (void)surface; (void)keycodes;
	(void)num_keycodes; (void)modifiers;
}

uint32_t
wlr_xdg_toplevel_set_activated(struct wlr_xdg_toplevel *toplevel,
                               bool activated)
{
	(void)toplevel; (void)activated;
	return 0;
}

typedef struct {
	gchar             *parent;
	gchar             *runtime;
	gchar             *state_path;
	GowlConfig        *config;
	GowlModuleManager *modules;
	GowlModule        *floatdesk;
	GowlCompositor    *compositor;
	gboolean           started;
	GowlClient        *c[RIG_MAX_CLIENTS];
	gint               count;
} Rig;

static const gchar *const rig_modules[] = { "tile", "floatdesk", NULL };

/*
 * rig_up_with_state:
 * @r: the rig
 * @state_yaml: (nullable): a state file to have been saved by a previous
 *   session, or %NULL for none
 *
 * The state file lives in the rig's own runtime directory
 * ($GOWL_FLOATDESK_STATE), never in the developer's $XDG_STATE_HOME.
 */
static gboolean
rig_up_with_state(Rig *r, const gchar *state_yaml)
{
	const gchar *parent;
	GError      *error = NULL;
	guint        i;

	memset(r, 0, sizeof(*r));
	g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_FATAL_MASK);

	r->parent = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
	parent = r->parent;
	if (parent == NULL || !g_file_test(parent, G_FILE_TEST_IS_DIR))
		parent = g_get_tmp_dir();
	r->runtime = g_build_filename(parent, "gowl-floatdesk-XXXXXX", NULL);
	g_assert_nonnull(g_mkdtemp_full(r->runtime, 0700));
	r->state_path = g_build_filename(r->runtime, "floatdesk.yaml", NULL);
	if (state_yaml != NULL)
		g_assert_true(g_file_set_contents(r->state_path, state_yaml, -1,
		                                  NULL));
	g_setenv("GOWL_FLOATDESK_STATE", r->state_path, TRUE);
	g_setenv("GOWL_DISABLE_SYSTEMD", "1", TRUE);
	g_setenv("XDG_RUNTIME_DIR", r->runtime, TRUE);
	g_setenv("WLR_BACKENDS", "headless", TRUE);
	g_setenv("WLR_HEADLESS_OUTPUTS", "1", TRUE);
	g_setenv("WLR_RENDERER", "pixman", TRUE);

	r->modules = gowl_module_manager_new();
	for (i = 0; rig_modules[i] != NULL; i++) {
		gchar *file = g_strconcat(rig_modules[i], ".so", NULL);
		gchar *path = g_build_filename(GOWL_TEST_MODULE_DIR, file, NULL);

		if (!gowl_module_manager_load_module(r->modules, path, &error))
			g_error("could not load %s: %s", path, error->message);
		g_free(path);
		g_free(file);
	}
	gowl_module_manager_activate_all(r->modules);
	r->floatdesk = gowl_module_manager_find_module(r->modules, "floatdesk");
	g_assert_nonnull(r->floatdesk);
	{
		/* No dock in the rig: it would scan the installed desktop
		 * entries, which is the developer's machine and not the
		 * test's.  The dock commands are asserted separately. */
		GHashTable *settings = g_hash_table_new(g_str_hash, g_str_equal);

		g_hash_table_insert(settings, "dock", "false");
		gowl_module_configure(r->floatdesk, settings);
		g_hash_table_unref(settings);
	}

	r->config = gowl_config_new();
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

static gboolean
rig_up(Rig *r)
{
	return rig_up_with_state(r, NULL);
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
		r->compositor->fstack = g_list_remove(r->compositor->fstack,
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
	g_unsetenv("GOWL_FLOATDESK_STATE");
	g_unlink(r->state_path);
	g_rmdir(r->runtime);
	g_free(r->state_path);
	g_free(r->runtime);
	g_free(r->parent);
	memset(r, 0, sizeof(*r));
}

/* A tiled window on @tags of the selected monitor, in the focus stack
 * as well as the client list. */
static GowlClient *
add_client(Rig *r, guint32 tags)
{
	GowlMonitor *m = r->compositor->selmon;
	GowlClient  *c;
	gint         i;
	static const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };

	g_assert_cmpint(r->count, <, RIG_MAX_CLIENTS);
	c = gowl_client_new();
	c->compositor = r->compositor;
	c->mon = m;
	c->bw = 0;
	c->tags = tags;
	c->geom = (struct wlr_box){ m->w.x + 10, m->w.y + 10, 300, 200 };
	c->frame = c->geom;
	c->scene = wlr_scene_tree_create(
		r->compositor->layers[GOWL_SCENE_LAYER_TILE]);
	c->scene->node.data = c;
	for (i = 0; i < 4; i++)
		c->border[i] = wlr_scene_rect_create(c->scene, 0, 0, grey);

	r->compositor->clients = g_list_append(r->compositor->clients, c);
	r->compositor->fstack = g_list_prepend(r->compositor->fstack, c);
	r->c[r->count++] = c;
	return c;
}

/* A window that maps the way on_client_map announces one: the
 * pre-map signal, then an arrange of its output. */
static GowlClient *
map_client(Rig *r, guint32 tags)
{
	GowlClient *c = add_client(r, tags);

	g_signal_emit_by_name(r->compositor, "client-pre-map", c);
	gowl_compositor_arrange(r->compositor, c->mon);
	return c;
}

static void
arrange(Rig *r)
{
	gowl_compositor_arrange(r->compositor, r->compositor->selmon);
}

/* Runs a command; returns the reply, which the caller frees. */
static gchar *
run(Rig *r, const gchar *line)
{
	return gowl_compositor_run_command(r->compositor, line);
}

#define ASSERT_REPLY_PREFIX(r, line, prefix) \
	G_STMT_START { \
		g_autofree gchar *reply_ = run((r), (line)); \
		g_assert_nonnull(reply_); \
		if (!g_str_has_prefix(reply_, (prefix))) \
			g_error("%s -> \"%s\", expected \"%s...\"", (line), \
			        reply_, (prefix)); \
	} G_STMT_END

static const gchar *
layout_name(Rig *r)
{
	GowlLayoutEntry *e = gowl_layout_get(r->compositor,
	                                     r->compositor->selmon);

	return e != NULL ? e->name : NULL;
}

static void
view(Rig *r, guint32 tags)
{
	gowl_compositor_view_tags(r->compositor, r->compositor->selmon, tags);
}

/* The state file as saved, re-read. */
static GowlFloatdeskState *
saved_state(Rig *r)
{
	GowlFloatdeskState *s = gowl_floatdesk_state_new();
	GError             *error = NULL;

	g_assert_true(gowl_floatdesk_state_load(s, r->state_path, &error));
	g_assert_no_error(error);
	return s;
}

/* ── Entering and leaving ────────────────────────────────────────── */

/*
 * The toggle floats every tile on the view and claims it; the second
 * toggle tiles exactly those again.  A window the user floated is
 * floating before, during and after, and is never claimed.
 */
static void
test_toggle_floats_then_gives_back_only_its_own(void)
{
	Rig         r;
	GowlClient *a, *b, *mine;

	if (!rig_up(&r)) { g_test_skip("no headless compositor"); return; }
	a = add_client(&r, TAG(1));
	b = add_client(&r, TAG(1));
	mine = add_client(&r, TAG(1));
	gowl_compositor_set_client_floating(r.compositor, mine, TRUE);
	arrange(&r);
	g_assert_cmpstr(layout_name(&r), ==, "tile");

	ASSERT_REPLY_PREFIX(&r, "floatdesk-toggle", "OK tag 1 floating");
	g_assert_cmpstr(layout_name(&r), ==, GOWL_FLOATDESK_LAYOUT);
	g_assert_true(a->isfloating && a->mode_floated);
	g_assert_true(b->isfloating && b->mode_floated);
	g_assert_true(mine->isfloating);
	g_assert_false(mine->mode_floated);
	/* Floating on a floating desktop: title bars, the user's window
	 * included. */
	g_assert_cmpint(a->deco_top, >, 0);
	g_assert_cmpint(mine->deco_top, >, 0);

	ASSERT_REPLY_PREFIX(&r, "floatdesk-toggle", "OK tag 1 tiled");
	g_assert_cmpstr(layout_name(&r), ==, "tile");
	g_assert_false(a->isfloating || a->mode_floated);
	g_assert_false(b->isfloating || b->mode_floated);
	g_assert_true(mine->isfloating);
	g_assert_false(mine->mode_floated);
	g_assert_cmpint(a->deco_top, ==, 0);
	g_assert_cmpint(mine->deco_top, ==, 0);
	rig_down(&r);
}

/* The layout cycle is the same transition: picking `desktop' enters
 * the mode (and saves it), picking another layout leaves it. */
static void
test_layout_cycle_enters_and_leaves(void)
{
	Rig                 r;
	GowlClient         *a;
	GowlFloatdeskState *s;
	const gchar        *next;

	if (!rig_up(&r)) { g_test_skip("no headless compositor"); return; }
	a = add_client(&r, TAG(1));
	arrange(&r);

	next = gowl_layout_cycle(r.compositor, r.compositor->selmon, +1);
	g_assert_cmpstr(next, ==, GOWL_FLOATDESK_LAYOUT);
	g_assert_true(a->isfloating && a->mode_floated);
	s = saved_state(&r);
	g_assert_true(gowl_floatdesk_state_tag_floating(s, 0));
	/* Where it came from, for the way back. */
	g_assert_cmpstr(gowl_floatdesk_state_get_previous_layout(s, 0), ==,
	                "tile");
	gowl_floatdesk_state_free(s);

	g_assert_true(gowl_layout_set(r.compositor, r.compositor->selmon,
	                              "tile"));
	g_assert_false(a->isfloating || a->mode_floated);
	s = saved_state(&r);
	g_assert_false(gowl_floatdesk_state_tag_floating(s, 0));
	gowl_floatdesk_state_free(s);
	rig_down(&r);
}

/* ── Windows moving between workspaces ──────────────────────────── */

static void
test_window_moved_in_and_out(void)
{
	Rig         r;
	GowlClient *c;

	if (!rig_up(&r)) { g_test_skip("no headless compositor"); return; }
	ASSERT_REPLY_PREFIX(&r, "floatdesk-on", "OK tag 1 floating");
	c = add_client(&r, TAG(2));
	arrange(&r);
	g_assert_false(c->isfloating);

	/* Into the floating workspace: floated and claimed. */
	gowl_compositor_move_client(r.compositor, c, NULL, TAG(1));
	g_assert_true(c->isfloating && c->mode_floated);

	/* Back out to one that tiles, which is not even in view: tiled
	 * and unclaimed all the same. */
	gowl_compositor_move_client(r.compositor, c, NULL, TAG(2));
	g_assert_false(c->isfloating || c->mode_floated);
	rig_down(&r);
}

/* The claim is on the window and goes where it goes. */
static void
test_claim_follows_the_window(void)
{
	Rig         r;
	GowlClient *c;

	if (!rig_up(&r)) { g_test_skip("no headless compositor"); return; }
	ASSERT_REPLY_PREFIX(&r, "floatdesk-on 1", "OK tag 1 floating");
	ASSERT_REPLY_PREFIX(&r, "floatdesk-on 3", "OK tag 3 floating");
	c = add_client(&r, TAG(1));
	arrange(&r);
	g_assert_true(c->mode_floated);

	gowl_compositor_move_client(r.compositor, c, NULL, TAG(3));
	g_assert_true(c->isfloating && c->mode_floated);
	g_assert_true(gowl_client_get_mode_floated(c));

	/* Tag 3 leaves the mode while not in view: the window there is
	 * given back on the next arrange of its output. */
	ASSERT_REPLY_PREFIX(&r, "floatdesk-off 3", "OK tag 3 tiled");
	g_assert_false(c->isfloating || c->mode_floated);
	rig_down(&r);
}

/* ── Persistence ────────────────────────────────────────────────── */

/* A mode saved by the last session is back before the first key press. */
static void
test_saved_mode_is_restored_at_startup(void)
{
	Rig         r;
	GowlClient *c;

	if (!rig_up_with_state(&r, "version: 1\ndefault-floating: false\n"
	                           "tags:\n  - tag: 1\n    mode: floating\n"
	                           "    previous-layout: tile\n")) {
		g_test_skip("no headless compositor");
		return;
	}
	c = add_client(&r, TAG(1));
	arrange(&r);
	g_assert_cmpstr(layout_name(&r), ==, GOWL_FLOATDESK_LAYOUT);
	g_assert_true(c->isfloating && c->mode_floated);

	/* The other tags were never set and follow the (tiling) default. */
	view(&r, TAG(2));
	g_assert_cmpstr(layout_name(&r), ==, "tile");
	view(&r, TAG(1));
	g_assert_cmpstr(layout_name(&r), ==, GOWL_FLOATDESK_LAYOUT);
	rig_down(&r);
}

/* ── New windows ────────────────────────────────────────────────── */

/* Floating from the first arrange, at the default size, centred, the
 * next one cascaded from it -- and a maximized window gives way. */
static void
test_new_windows_open_floating_centred_and_cascade(void)
{
	Rig             r;
	GowlClient     *first, *second;
	struct wlr_box  w;

	if (!rig_up(&r)) { g_test_skip("no headless compositor"); return; }
	ASSERT_REPLY_PREFIX(&r, "floatdesk-on", "OK tag 1 floating");
	w = r.compositor->selmon->w;

	first = map_client(&r, TAG(1));
	g_assert_true(first->isfloating && first->mode_floated);
	g_assert_cmpint(first->geom.width, ==, (gint)(w.width * 0.6));
	g_assert_cmpint(first->geom.height, ==, (gint)(w.height * 0.7));
	g_assert_cmpint(first->geom.x, ==, w.x + (w.width - first->geom.width) / 2);
	g_assert_cmpint(first->geom.y, ==,
	                w.y + (w.height - first->geom.height) / 2);

	{
		g_autofree gchar *reply = NULL;
		g_autofree gchar *want = NULL;
		g_autofree gchar *line = g_strdup_printf("floatdesk-maximize %u",
		                                         first->id);

		reply = run(&r, line);
		want = g_strdup_printf("OK maximized %u", first->id);
		g_assert_cmpstr(reply, ==, want);
	}
	g_assert_cmpint(first->geom.width, ==, w.width);

	second = map_client(&r, TAG(1));
	g_assert_cmpint(second->geom.x, ==, first->geom.x + 32);
	g_assert_cmpint(second->geom.y, ==, first->geom.y + 32);
	/* The maximized window was restored to make room. */
	g_assert_cmpint(first->geom.width, ==, (gint)(w.width * 0.6));
	rig_down(&r);
}

/* A window a rule (or its parent) floated keeps its own size: the
 * mode does not claim it. */
static void
test_a_window_floating_for_its_own_reasons_is_not_placed(void)
{
	Rig         r;
	GowlClient *dialog;

	if (!rig_up(&r)) { g_test_skip("no headless compositor"); return; }
	ASSERT_REPLY_PREFIX(&r, "floatdesk-on", "OK tag 1 floating");
	dialog = add_client(&r, TAG(1));
	dialog->isfloating = TRUE;
	g_signal_emit_by_name(r.compositor, "client-pre-map", dialog);
	arrange(&r);
	g_assert_false(dialog->mode_floated);
	g_assert_cmpint(dialog->geom.width, ==, 300);
	g_assert_cmpint(dialog->geom.height, ==, 200);
	rig_down(&r);
}

/* ── Minimize ───────────────────────────────────────────────────── */

static void
test_minimize_only_on_a_floating_desktop(void)
{
	Rig              r;
	GowlClient      *c;
	g_autofree gchar *line = NULL;

	if (!rig_up(&r)) { g_test_skip("no headless compositor"); return; }
	c = add_client(&r, TAG(1));
	arrange(&r);
	line = g_strdup_printf("floatdesk-minimize %u", c->id);

	/* A tiling workspace: refused, nothing changes. */
	ASSERT_REPLY_PREFIX(&r, line, "ERROR");
	g_assert_false(c->isminimized);
	g_assert_cmpuint(c->tags, ==, TAG(1));

	ASSERT_REPLY_PREFIX(&r, "floatdesk-on", "OK tag 1 floating");
	ASSERT_REPLY_PREFIX(&r, line, "OK minimized");
	g_assert_true(c->isminimized);
	g_assert_cmpuint(c->tags, ==, 0);
	g_assert_cmpuint(gowl_client_get_minimized_tags(c), ==, TAG(1));
	/* Its claim waits for the restore. */
	g_assert_true(c->mode_floated);
	{
		g_autofree gchar *status = run(&r, "floatdesk-status");
		g_autofree gchar *want = g_strdup_printf("minimized=%u ", c->id);

		g_assert_nonnull(strstr(status, want));
	}

	/* Restored onto the workspace in view, still floating there. */
	ASSERT_REPLY_PREFIX(&r, "floatdesk-restore", "OK restored");
	g_assert_false(c->isminimized);
	g_assert_cmpuint(c->tags, ==, TAG(1));
	g_assert_true(c->isfloating && c->mode_floated);
	ASSERT_REPLY_PREFIX(&r, "floatdesk-restore", "ERROR nothing");
	rig_down(&r);
}

/* Restored onto a workspace that tiles: given back to the layout. */
static void
test_restore_onto_a_tiling_workspace_tiles(void)
{
	Rig              r;
	GowlClient      *c;
	g_autofree gchar *line = NULL;

	if (!rig_up(&r)) { g_test_skip("no headless compositor"); return; }
	ASSERT_REPLY_PREFIX(&r, "floatdesk-on", "OK tag 1 floating");
	c = add_client(&r, TAG(1));
	arrange(&r);
	line = g_strdup_printf("floatdesk-minimize %u", c->id);
	ASSERT_REPLY_PREFIX(&r, line, "OK minimized");
	view(&r, TAG(2));
	ASSERT_REPLY_PREFIX(&r, "floatdesk-restore", "OK restored");
	g_assert_cmpuint(c->tags, ==, TAG(2));
	g_assert_false(c->isfloating || c->mode_floated);
	rig_down(&r);
}

/* ── The default ────────────────────────────────────────────────── */

static void
test_default_floats_every_unset_workspace(void)
{
	Rig         r;
	GowlClient *one, *two;

	if (!rig_up(&r)) { g_test_skip("no headless compositor"); return; }
	ASSERT_REPLY_PREFIX(&r, "floatdesk-off 2", "OK tag 2 tiled");
	one = add_client(&r, TAG(1));
	two = add_client(&r, TAG(2));
	arrange(&r);

	ASSERT_REPLY_PREFIX(&r, "floatdesk-default on", "OK default floating");
	g_assert_cmpstr(layout_name(&r), ==, GOWL_FLOATDESK_LAYOUT);
	g_assert_true(one->isfloating && one->mode_floated);
	/* Tag 2 was tiled on purpose; the default does not override it. */
	g_assert_false(two->isfloating);
	view(&r, TAG(2));
	g_assert_cmpstr(layout_name(&r), ==, "tile");

	view(&r, TAG(1));
	ASSERT_REPLY_PREFIX(&r, "floatdesk-default off", "OK default tiled");
	g_assert_cmpstr(layout_name(&r), ==, "tile");
	g_assert_false(one->isfloating || one->mode_floated);
	rig_down(&r);
}

/* ── Maximize and snap ──────────────────────────────────────────── */

static void
test_maximize_and_snap_remember_the_size(void)
{
	Rig              r;
	GowlClient      *c;
	struct wlr_box   before, w;
	g_autofree gchar *max = NULL, *left = NULL;

	if (!rig_up(&r)) { g_test_skip("no headless compositor"); return; }
	ASSERT_REPLY_PREFIX(&r, "floatdesk-on", "OK tag 1 floating");
	c = add_client(&r, TAG(1));
	arrange(&r);
	before = c->geom;
	w = r.compositor->selmon->w;
	max = g_strdup_printf("floatdesk-maximize %u", c->id);
	left = g_strdup_printf("floatdesk-snap left %u", c->id);

	ASSERT_REPLY_PREFIX(&r, max, "OK maximized");
	g_assert_true(wlr_box_equal(&c->geom, &w));
	ASSERT_REPLY_PREFIX(&r, max, "OK restored");
	g_assert_true(wlr_box_equal(&c->geom, &before));

	ASSERT_REPLY_PREFIX(&r, left, "OK snapped");
	g_assert_cmpint(c->geom.x, ==, w.x);
	g_assert_cmpint(c->geom.width, ==, w.width / 2);
	/* Maximize from a snap, then back: the ORIGINAL size. */
	ASSERT_REPLY_PREFIX(&r, max, "OK maximized");
	ASSERT_REPLY_PREFIX(&r, max, "OK restored");
	g_assert_true(wlr_box_equal(&c->geom, &before));
	rig_down(&r);
}

/* ── Switching the module off ───────────────────────────────────── */

static void
test_switching_off_gives_every_window_back(void)
{
	Rig         r;
	GowlClient *c;

	if (!rig_up(&r)) { g_test_skip("no headless compositor"); return; }
	ASSERT_REPLY_PREFIX(&r, "floatdesk-on", "OK tag 1 floating");
	c = add_client(&r, TAG(1));
	arrange(&r);
	g_assert_true(c->mode_floated);

	gowl_module_deactivate(r.floatdesk);
	g_assert_false(c->isfloating || c->mode_floated);
	g_assert_cmpint(c->deco_top, ==, 0);
	/* The layout went with it. */
	g_assert_null(gowl_layout_lookup(r.compositor, GOWL_FLOATDESK_LAYOUT));
	g_assert_cmpstr(layout_name(&r), ==, "tile");
	rig_down(&r);
}

/* ── Commands ───────────────────────────────────────────────────── */

static void
test_commands_report_errors(void)
{
	Rig r;

	if (!rig_up(&r)) { g_test_skip("no headless compositor"); return; }
	ASSERT_REPLY_PREFIX(&r, "floatdesk-frobnicate", "ERROR unknown command");
	ASSERT_REPLY_PREFIX(&r, "floatdesk-toggle 40", "ERROR");
	ASSERT_REPLY_PREFIX(&r, "floatdesk-toggle x", "ERROR");
	ASSERT_REPLY_PREFIX(&r, "floatdesk-default sideways", "ERROR");
	ASSERT_REPLY_PREFIX(&r, "floatdesk-minimize 99999", "ERROR no client");
	ASSERT_REPLY_PREFIX(&r, "floatdesk-snap up", "ERROR unknown snap");
	ASSERT_REPLY_PREFIX(&r, "floatdesk-dock-position middle", "ERROR");
	ASSERT_REPLY_PREFIX(&r, "floatdesk-status", "OK view=1 view-floating=0");
	rig_down(&r);
}

/* Dock settings set at run time land in the state file. */
static void
test_dock_settings_are_saved(void)
{
	Rig                 r;
	GowlFloatdeskState *s;
	const gchar *const *pins;

	if (!rig_up(&r)) { g_test_skip("no headless compositor"); return; }
	ASSERT_REPLY_PREFIX(&r, "floatdesk-dock-position left", "OK dock left");
	ASSERT_REPLY_PREFIX(&r, "floatdesk-dock-autohide on", "OK autohide on");
	ASSERT_REPLY_PREFIX(&r, "floatdesk-dock-pin com.example.one", "OK pinned");
	ASSERT_REPLY_PREFIX(&r, "floatdesk-dock-pin com.example.two 0", "OK pinned");
	ASSERT_REPLY_PREFIX(&r, "floatdesk-dock-unpin com.example.three", "ERROR");

	s = saved_state(&r);
	g_assert_cmpstr(gowl_floatdesk_state_get_dock_position(s), ==, "left");
	g_assert_cmpint(gowl_floatdesk_state_get_dock_autohide(s), ==, 1);
	pins = gowl_floatdesk_state_get_dock_pins(s);
	g_assert_nonnull(pins);
	g_assert_cmpstr(pins[0], ==, "com.example.two");
	g_assert_cmpstr(pins[1], ==, "com.example.one");
	g_assert_null(pins[2]);
	gowl_floatdesk_state_free(s);

	ASSERT_REPLY_PREFIX(&r, "floatdesk-dock-unpin com.example.two", "OK unpinned");
	s = saved_state(&r);
	pins = gowl_floatdesk_state_get_dock_pins(s);
	g_assert_cmpstr(pins[0], ==, "com.example.one");
	g_assert_null(pins[1]);
	gowl_floatdesk_state_free(s);
	rig_down(&r);
}

/* Sloppy focus is refused on a floating desktop, allowed elsewhere. */
static void
test_focus_follows_click_on_a_floating_desktop(void)
{
	Rig         r;
	GowlClient *on, *off;

	if (!rig_up(&r)) { g_test_skip("no headless compositor"); return; }
	ASSERT_REPLY_PREFIX(&r, "floatdesk-on 1", "OK tag 1 floating");
	on = add_client(&r, TAG(1));
	off = add_client(&r, TAG(2));
	g_assert_false(gowl_module_manager_focus_on_hover(r.modules, on));
	g_assert_true(gowl_module_manager_focus_on_hover(r.modules, off));
	rig_down(&r);
}

/* The dock: on a floating view only, every running app in it, a
 * minimized window included, and a click restores it. */
static void
test_dock_only_on_a_floating_desktop(void)
{
	Rig              r;
	GowlClient      *c;
	GHashTable      *settings;
	g_autofree gchar *line = NULL;

	if (!rig_up(&r)) { g_test_skip("no headless compositor"); return; }
	settings = g_hash_table_new(g_str_hash, g_str_equal);
	g_hash_table_insert(settings, "dock", "true");
	gowl_module_configure(r.floatdesk, settings);
	g_hash_table_unref(settings);

	c = add_client(&r, TAG(1));
	gowl_client_set_app_id(c, "com.example.NoSuchApp");
	arrange(&r);
	ASSERT_REPLY_PREFIX(&r, "floatdesk-dock-status", "OK position=bottom "
	                    "autohide=0 shown=0");

	ASSERT_REPLY_PREFIX(&r, "floatdesk-on", "OK tag 1 floating");
	{
		g_autofree gchar *status = run(&r, "floatdesk-dock-status");

		g_assert_nonnull(strstr(status, "shown=1"));
		/* No desktop entry: still its own app, one window. */
		g_assert_nonnull(strstr(status, "com.example.nosuchapp:0:1:0:0"));
	}

	line = g_strdup_printf("floatdesk-minimize %u", c->id);
	ASSERT_REPLY_PREFIX(&r, line, "OK minimized");
	{
		g_autofree gchar *status = run(&r, "floatdesk-dock-status");

		g_assert_nonnull(strstr(status, "com.example.nosuchapp:0:1:1:0"));
	}
	ASSERT_REPLY_PREFIX(&r, "floatdesk-dock-activate com.example.nosuchapp",
	                    "OK focused");
	g_assert_false(c->isminimized);

	/* (Hidden under a fullscreen window too, which this rig cannot
	 * show: the core configures a fullscreen window's toplevel, and a
	 * fabricated window has none.) */

	/* A tiling view: hidden. */
	ASSERT_REPLY_PREFIX(&r, "floatdesk-off", "OK tag 1 tiled");
	ASSERT_REPLY_PREFIX(&r, "floatdesk-dock-status", "OK position=bottom "
	                    "autohide=0 shown=0");
	rig_down(&r);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/floatdesk-module/toggle-gives-back-only-its-own",
	                test_toggle_floats_then_gives_back_only_its_own);
	g_test_add_func("/floatdesk-module/layout-cycle",
	                test_layout_cycle_enters_and_leaves);
	g_test_add_func("/floatdesk-module/moved-in-and-out",
	                test_window_moved_in_and_out);
	g_test_add_func("/floatdesk-module/claim-follows-window",
	                test_claim_follows_the_window);
	g_test_add_func("/floatdesk-module/restored-at-startup",
	                test_saved_mode_is_restored_at_startup);
	g_test_add_func("/floatdesk-module/new-windows-cascade",
	                test_new_windows_open_floating_centred_and_cascade);
	g_test_add_func("/floatdesk-module/own-reasons-not-placed",
	                test_a_window_floating_for_its_own_reasons_is_not_placed);
	g_test_add_func("/floatdesk-module/minimize",
	                test_minimize_only_on_a_floating_desktop);
	g_test_add_func("/floatdesk-module/restore-onto-tiling",
	                test_restore_onto_a_tiling_workspace_tiles);
	g_test_add_func("/floatdesk-module/default",
	                test_default_floats_every_unset_workspace);
	g_test_add_func("/floatdesk-module/maximize-and-snap",
	                test_maximize_and_snap_remember_the_size);
	g_test_add_func("/floatdesk-module/switch-off",
	                test_switching_off_gives_every_window_back);
	g_test_add_func("/floatdesk-module/command-errors",
	                test_commands_report_errors);
	g_test_add_func("/floatdesk-module/dock-settings-saved",
	                test_dock_settings_are_saved);
	g_test_add_func("/floatdesk-module/focus-follows-click",
	                test_focus_follows_click_on_a_floating_desktop);

	g_test_add_func("/floatdesk-module/dock-only-on-floating",
	                test_dock_only_on_a_floating_desktop);

	return g_test_run();
}
