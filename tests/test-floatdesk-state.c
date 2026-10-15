/* test-floatdesk-state.c -- the floating desktop's decisions, without a
 * compositor
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything the floating desktop decides is decided here, from plain
 * structs: which workspaces float, what one arrange does to one window,
 * what the state file says, where a new window opens, where a dropped
 * one snaps, which application a window belongs to.  The module only
 * carries those answers out, and tests/test-floatdesk-module.c checks
 * that it does; this file checks that the answers are right.
 *
 * The cases that matter most are the OWNERSHIP ones.  The mode floats
 * every window on a workspace and must, on leaving, tile again exactly
 * those windows -- never a dialog, a rule-floated window or one the
 * user floated -- and it must keep track of that as windows move
 * between workspaces.  Each of those is a case below.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "../modules/floatdesk/gowl-floatdesk-state.c"
#include "../modules/floatdesk/gowl-floatdesk-apps.c"

#define TAG(n) ((guint32)1u << ((n) - 1))

/* A tiled window on @tags, owned by nobody. */
static GowlFloatdeskClientView
tiled_on(guint32 tags)
{
	GowlFloatdeskClientView v;

	memset(&v, 0, sizeof(v));
	v.tags = tags;
	return v;
}

/* ── Modes ───────────────────────────────────────────────────────── */

static void
test_new_state_tiles_everything(void)
{
	GowlFloatdeskState *s = gowl_floatdesk_state_new();
	guint               tag;

	for (tag = 0; tag < GOWL_FLOATDESK_MAX_TAGS; tag++) {
		g_assert_cmpint(gowl_floatdesk_state_get_mode(s, tag), ==,
		                GOWL_FLOATDESK_MODE_UNSET);
		g_assert_false(gowl_floatdesk_state_tag_floating(s, tag));
	}
	g_assert_false(gowl_floatdesk_state_mask_floating(s, TAG(1)));
	/* No tags is never a floating workspace -- it is a minimized
	 * window or a hidden overlay. */
	gowl_floatdesk_state_set_default_floating(s, TRUE);
	g_assert_false(gowl_floatdesk_state_mask_floating(s, 0));
	gowl_floatdesk_state_free(s);
}

static void
test_explicit_setting_beats_the_default(void)
{
	GowlFloatdeskState *s = gowl_floatdesk_state_new();

	gowl_floatdesk_state_set_mode(s, 2, GOWL_FLOATDESK_MODE_TILED);
	gowl_floatdesk_state_set_default_floating(s, TRUE);
	g_assert_true(gowl_floatdesk_state_tag_floating(s, 0));
	g_assert_false(gowl_floatdesk_state_tag_floating(s, 2));

	gowl_floatdesk_state_set_default_floating(s, FALSE);
	gowl_floatdesk_state_set_mode(s, 4, GOWL_FLOATDESK_MODE_FLOATING);
	g_assert_true(gowl_floatdesk_state_tag_floating(s, 4));
	g_assert_false(gowl_floatdesk_state_tag_floating(s, 0));

	/* Back to unset: follows the default again. */
	gowl_floatdesk_state_set_mode(s, 4, GOWL_FLOATDESK_MODE_UNSET);
	g_assert_false(gowl_floatdesk_state_tag_floating(s, 4));
	gowl_floatdesk_state_set_default_floating(s, TRUE);
	g_assert_true(gowl_floatdesk_state_tag_floating(s, 4));

	/* Out of range is ignored, not a crash. */
	gowl_floatdesk_state_set_mode(s, 99, GOWL_FLOATDESK_MODE_FLOATING);
	g_assert_cmpint(gowl_floatdesk_state_get_mode(s, 99), ==,
	                GOWL_FLOATDESK_MODE_UNSET);
	gowl_floatdesk_state_free(s);
}

/* A view of a floating tag and a tiled one tiles; a window on both
 * likewise. */
static void
test_a_mask_floats_only_if_every_tag_does(void)
{
	GowlFloatdeskState *s = gowl_floatdesk_state_new();

	gowl_floatdesk_state_set_mask(s, TAG(1) | TAG(3), TRUE);
	g_assert_true(gowl_floatdesk_state_mask_floating(s, TAG(1)));
	g_assert_true(gowl_floatdesk_state_mask_floating(s, TAG(1) | TAG(3)));
	g_assert_false(gowl_floatdesk_state_mask_floating(s, TAG(1) | TAG(2)));
	g_assert_false(gowl_floatdesk_state_mask_floating(s, TAG(2)));
	gowl_floatdesk_state_free(s);
}

static void
test_previous_layout_is_remembered_per_tag(void)
{
	GowlFloatdeskState *s = gowl_floatdesk_state_new();

	gowl_floatdesk_state_set_previous_layout(s, 1, "scrolling");
	gowl_floatdesk_state_set_previous_layout(s, 3, "monocle");
	g_assert_cmpstr(gowl_floatdesk_state_get_previous_layout(s, 1), ==,
	                "scrolling");
	g_assert_cmpstr(gowl_floatdesk_state_previous_layout_for_mask(
	                        s, TAG(2) | TAG(4)), ==, "scrolling");
	g_assert_null(gowl_floatdesk_state_previous_layout_for_mask(s, TAG(1)));

	/* "Back" to the mode's own layout would not be leaving it. */
	gowl_floatdesk_state_set_previous_layout(s, 1, GOWL_FLOATDESK_LAYOUT);
	g_assert_cmpstr(gowl_floatdesk_state_get_previous_layout(s, 1), ==,
	                "scrolling");
	gowl_floatdesk_state_free(s);
}

/* ── The ownership rule ─────────────────────────────────────────── */

/* Entering: a tiled window on a workspace that now floats is floated
 * and claimed. */
static void
test_enter_floats_and_claims_tiled_windows(void)
{
	GowlFloatdeskState     *s = gowl_floatdesk_state_new();
	GowlFloatdeskClientView v = tiled_on(TAG(1));

	g_assert_cmpint(gowl_floatdesk_state_decide(s, &v), ==,
	                GOWL_FLOATDESK_ACTION_NONE);
	gowl_floatdesk_state_set_mask(s, TAG(1), TRUE);
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &v), ==,
	                GOWL_FLOATDESK_ACTION_FLOAT);

	/* Once floated and claimed, nothing more to do. */
	v.floating = TRUE;
	v.mode_floated = TRUE;
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &v), ==,
	                GOWL_FLOATDESK_ACTION_NONE);
	gowl_floatdesk_state_free(s);
}

/* Exiting: only what the mode floated goes back. */
static void
test_exit_gives_back_only_what_it_took(void)
{
	GowlFloatdeskState     *s = gowl_floatdesk_state_new();
	GowlFloatdeskClientView mine = tiled_on(TAG(1));
	GowlFloatdeskClientView dialog = tiled_on(TAG(1));

	mine.floating = TRUE;
	mine.mode_floated = TRUE;
	dialog.floating = TRUE;      /* floating for its own reasons */

	/* The workspace tiles (the mode was just left). */
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &mine), ==,
	                GOWL_FLOATDESK_ACTION_UNFLOAT);
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &dialog), ==,
	                GOWL_FLOATDESK_ACTION_NONE);

	/* And while the mode is on, the dialog is never claimed. */
	gowl_floatdesk_state_set_mask(s, TAG(1), TRUE);
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &dialog), ==,
	                GOWL_FLOATDESK_ACTION_NONE);
	gowl_floatdesk_state_free(s);
}

/* A claimed window the user tiled by hand: drop the claim, nothing to
 * undo. */
static void
test_a_claim_on_a_tiled_window_is_cleared(void)
{
	GowlFloatdeskState     *s = gowl_floatdesk_state_new();
	GowlFloatdeskClientView v = tiled_on(TAG(2));

	v.mode_floated = TRUE;
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &v), ==,
	                GOWL_FLOATDESK_ACTION_CLEAR);
	gowl_floatdesk_state_free(s);
}

/* Moved in and moved out: the decision follows the window's tags. */
static void
test_moving_between_workspaces(void)
{
	GowlFloatdeskState     *s = gowl_floatdesk_state_new();
	GowlFloatdeskClientView v = tiled_on(TAG(2));

	gowl_floatdesk_state_set_mask(s, TAG(1), TRUE);
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &v), ==,
	                GOWL_FLOATDESK_ACTION_NONE);

	/* Moved in. */
	v.tags = TAG(1);
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &v), ==,
	                GOWL_FLOATDESK_ACTION_FLOAT);
	v.floating = TRUE;
	v.mode_floated = TRUE;

	/* Moved out to a workspace that tiles. */
	v.tags = TAG(2);
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &v), ==,
	                GOWL_FLOATDESK_ACTION_UNFLOAT);

	/* Moved out to ANOTHER floating workspace: stays as it is. */
	gowl_floatdesk_state_set_mask(s, TAG(3), TRUE);
	v.tags = TAG(3);
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &v), ==,
	                GOWL_FLOATDESK_ACTION_NONE);
	gowl_floatdesk_state_free(s);
}

/* Fullscreen is floated only once it leaves fullscreen; a claimed
 * fullscreen window on a workspace that tiles is given back. */
static void
test_fullscreen_waits(void)
{
	GowlFloatdeskState     *s = gowl_floatdesk_state_new();
	GowlFloatdeskClientView v = tiled_on(TAG(1));

	gowl_floatdesk_state_set_mask(s, TAG(1), TRUE);
	v.fullscreen = TRUE;
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &v), ==,
	                GOWL_FLOATDESK_ACTION_NONE);
	v.fullscreen = FALSE;
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &v), ==,
	                GOWL_FLOATDESK_ACTION_FLOAT);

	v.floating = TRUE;
	v.mode_floated = TRUE;
	v.fullscreen = TRUE;
	gowl_floatdesk_state_set_mask(s, TAG(1), FALSE);
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &v), ==,
	                GOWL_FLOATDESK_ACTION_UNFLOAT);
	gowl_floatdesk_state_free(s);
}

/* Never touched: minimized (keeping its claim), overlays, popups and
 * embedded windows. */
static void
test_windows_the_mode_never_touches(void)
{
	GowlFloatdeskState     *s = gowl_floatdesk_state_new();
	GowlFloatdeskClientView v;

	gowl_floatdesk_state_set_default_floating(s, TRUE);

	v = tiled_on(TAG(1));
	v.overlay = TRUE;
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &v), ==,
	                GOWL_FLOATDESK_ACTION_NONE);
	v = tiled_on(TAG(1));
	v.unmanaged = TRUE;
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &v), ==,
	                GOWL_FLOATDESK_ACTION_NONE);
	v = tiled_on(TAG(1));
	v.embedded = TRUE;
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &v), ==,
	                GOWL_FLOATDESK_ACTION_NONE);

	/* A minimized window is on no tag; its claim waits for the
	 * restore, even with every workspace tiling. */
	gowl_floatdesk_state_set_default_floating(s, FALSE);
	v = tiled_on(0);
	v.minimized = TRUE;
	v.floating = TRUE;
	v.mode_floated = TRUE;
	g_assert_cmpint(gowl_floatdesk_state_decide(s, &v), ==,
	                GOWL_FLOATDESK_ACTION_NONE);
	gowl_floatdesk_state_free(s);
}

/* ── The state file ─────────────────────────────────────────────── */

static void
test_round_trip_through_yaml(void)
{
	GowlFloatdeskState *a = gowl_floatdesk_state_new();
	GowlFloatdeskState *b = gowl_floatdesk_state_new();
	g_autofree gchar   *text = NULL;
	g_autofree gchar   *again = NULL;
	GError             *error = NULL;
	const gchar *pins[] = { "firefox", "org.gnome.Nautilus", "we:ird\"", NULL };
	const gchar *const *got;

	gowl_floatdesk_state_set_default_floating(a, TRUE);
	gowl_floatdesk_state_set_mode(a, 0, GOWL_FLOATDESK_MODE_FLOATING);
	gowl_floatdesk_state_set_mode(a, 8, GOWL_FLOATDESK_MODE_TILED);
	gowl_floatdesk_state_set_previous_layout(a, 0, "scrolling");
	gowl_floatdesk_state_set_previous_layout(a, 5, "monocle");
	g_assert_true(gowl_floatdesk_state_set_dock_position(a, "left"));
	gowl_floatdesk_state_set_dock_autohide(a, 1);
	gowl_floatdesk_state_set_dock_pins(a, pins);

	text = gowl_floatdesk_state_to_yaml(a);
	g_assert_true(gowl_floatdesk_state_load_yaml(b, text, &error));
	g_assert_no_error(error);

	g_assert_true(gowl_floatdesk_state_get_default_floating(b));
	g_assert_cmpint(gowl_floatdesk_state_get_mode(b, 0), ==,
	                GOWL_FLOATDESK_MODE_FLOATING);
	g_assert_cmpint(gowl_floatdesk_state_get_mode(b, 8), ==,
	                GOWL_FLOATDESK_MODE_TILED);
	/* Unset stays unset, so a later change of the default reaches it. */
	g_assert_cmpint(gowl_floatdesk_state_get_mode(b, 5), ==,
	                GOWL_FLOATDESK_MODE_UNSET);
	g_assert_cmpstr(gowl_floatdesk_state_get_previous_layout(b, 0), ==,
	                "scrolling");
	g_assert_cmpstr(gowl_floatdesk_state_get_previous_layout(b, 5), ==,
	                "monocle");
	g_assert_cmpstr(gowl_floatdesk_state_get_dock_position(b), ==, "left");
	g_assert_cmpint(gowl_floatdesk_state_get_dock_autohide(b), ==, 1);
	got = gowl_floatdesk_state_get_dock_pins(b);
	g_assert_nonnull(got);
	g_assert_cmpstr(got[0], ==, "firefox");
	g_assert_cmpstr(got[1], ==, "org.gnome.Nautilus");
	g_assert_cmpstr(got[2], ==, "we:ird\"");
	g_assert_null(got[3]);

	/* And it serialises back to exactly the same text. */
	again = gowl_floatdesk_state_to_yaml(b);
	g_assert_cmpstr(text, ==, again);

	gowl_floatdesk_state_free(a);
	gowl_floatdesk_state_free(b);
}

/* An empty pin list is a list the user emptied, not "not set". */
static void
test_empty_pins_survive(void)
{
	GowlFloatdeskState *a = gowl_floatdesk_state_new();
	GowlFloatdeskState *b = gowl_floatdesk_state_new();
	g_autofree gchar   *text = NULL;
	const gchar        *none[] = { NULL };

	gowl_floatdesk_state_set_dock_pins(a, none);
	text = gowl_floatdesk_state_to_yaml(a);
	g_assert_true(gowl_floatdesk_state_load_yaml(b, text, NULL));
	g_assert_nonnull(gowl_floatdesk_state_get_dock_pins(b));
	g_assert_null(gowl_floatdesk_state_get_dock_pins(b)[0]);
	gowl_floatdesk_state_free(a);
	gowl_floatdesk_state_free(b);
}

/* A bad file fails the whole load and leaves the running state alone:
 * a half-read file saved back loses whatever was skipped. */
static void
test_a_bad_file_changes_nothing(void)
{
	GowlFloatdeskState *s = gowl_floatdesk_state_new();
	GError             *error = NULL;

	gowl_floatdesk_state_set_mode(s, 1, GOWL_FLOATDESK_MODE_FLOATING);

	g_assert_false(gowl_floatdesk_state_load_yaml(s,
		"version: 1\ntags:\n  - tag: 2\n    mode: sideways\n", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_false(gowl_floatdesk_state_load_yaml(s,
		"tags:\n  - tag: 40\n", &error));
	g_clear_error(&error);
	g_assert_false(gowl_floatdesk_state_load_yaml(s,
		"version: 99\n", &error));
	g_clear_error(&error);
	g_assert_false(gowl_floatdesk_state_load_yaml(s,
		"dock:\n  position: middle\n", &error));
	g_clear_error(&error);

	g_assert_cmpint(gowl_floatdesk_state_get_mode(s, 1), ==,
	                GOWL_FLOATDESK_MODE_FLOATING);
	gowl_floatdesk_state_free(s);
}

static void
test_save_and_load_a_file(void)
{
	g_autofree gchar   *dir = g_dir_make_tmp("floatdesk-XXXXXX", NULL);
	g_autofree gchar   *path = NULL;
	g_autofree gchar   *missing = NULL;
	GowlFloatdeskState *a = gowl_floatdesk_state_new();
	GowlFloatdeskState *b = gowl_floatdesk_state_new();
	GError             *error = NULL;

	g_assert_nonnull(dir);
	/* A directory that does not exist yet is created. */
	path = g_build_filename(dir, "nested", "floatdesk.yaml", NULL);
	gowl_floatdesk_state_set_mode(a, 3, GOWL_FLOATDESK_MODE_FLOATING);
	g_assert_true(gowl_floatdesk_state_save(a, path, &error));
	g_assert_no_error(error);
	g_assert_true(gowl_floatdesk_state_load(b, path, &error));
	g_assert_no_error(error);
	g_assert_true(gowl_floatdesk_state_tag_floating(b, 3));

	/* Nothing saved yet is not an error. */
	missing = g_build_filename(dir, "never.yaml", NULL);
	g_assert_true(gowl_floatdesk_state_load(b, missing, &error));
	g_assert_no_error(error);
	g_assert_true(gowl_floatdesk_state_tag_floating(b, 3));

	g_unlink(path);
	{
		g_autofree gchar *nested = g_path_get_dirname(path);

		g_rmdir(nested);
	}
	g_rmdir(dir);
	gowl_floatdesk_state_free(a);
	gowl_floatdesk_state_free(b);
}

/* ── Placement ──────────────────────────────────────────────────── */

static void
test_cascade_centres_then_steps_then_wraps(void)
{
	gint x, y;

	gowl_floatdesk_place_cascade(0, 30, 1000, 800, 400, 300, FALSE, 0, 0,
	                             32, &x, &y);
	g_assert_cmpint(x, ==, 300);
	g_assert_cmpint(y, ==, 30 + 250);

	gowl_floatdesk_place_cascade(0, 30, 1000, 800, 400, 300, TRUE, x, y,
	                             32, &x, &y);
	g_assert_cmpint(x, ==, 332);
	g_assert_cmpint(y, ==, 312);

	/* Off the bottom: back to the centre. */
	gowl_floatdesk_place_cascade(0, 30, 1000, 800, 400, 300, TRUE, 500, 520,
	                             32, &x, &y);
	g_assert_cmpint(x, ==, 300);
	g_assert_cmpint(y, ==, 280);

	/* Bigger than the area: pinned to the top-left, title bar on
	 * screen. */
	gowl_floatdesk_place_cascade(10, 30, 1000, 800, 1200, 900, FALSE, 0, 0,
	                             32, &x, &y);
	g_assert_cmpint(x, ==, 10);
	g_assert_cmpint(y, ==, 30);
}

static void
test_snap_edges(void)
{
	g_assert_cmpint(gowl_floatdesk_snap_at(0, 0, 1920, 1080, 960, 2, 6), ==,
	                GOWL_FLOATDESK_SNAP_MAXIMIZE);
	/* A top corner maximizes: the top edge is checked first. */
	g_assert_cmpint(gowl_floatdesk_snap_at(0, 0, 1920, 1080, 1, 1, 6), ==,
	                GOWL_FLOATDESK_SNAP_MAXIMIZE);
	g_assert_cmpint(gowl_floatdesk_snap_at(0, 0, 1920, 1080, 3, 500, 6), ==,
	                GOWL_FLOATDESK_SNAP_LEFT);
	g_assert_cmpint(gowl_floatdesk_snap_at(0, 0, 1920, 1080, 1917, 500, 6), ==,
	                GOWL_FLOATDESK_SNAP_RIGHT);
	g_assert_cmpint(gowl_floatdesk_snap_at(0, 0, 1920, 1080, 960, 1078, 6), ==,
	                GOWL_FLOATDESK_SNAP_NONE);
	g_assert_cmpint(gowl_floatdesk_snap_at(0, 0, 1920, 1080, 960, 540, 6), ==,
	                GOWL_FLOATDESK_SNAP_NONE);
	/* The second output's left edge is its own, not the layout's. */
	g_assert_cmpint(gowl_floatdesk_snap_at(1920, 0, 1920, 1080, 1922, 500, 6),
	                ==, GOWL_FLOATDESK_SNAP_LEFT);
	g_assert_cmpint(gowl_floatdesk_snap_at(1920, 0, 1920, 1080, 100, 500, 6),
	                ==, GOWL_FLOATDESK_SNAP_NONE);
}

/* ── Applications ───────────────────────────────────────────────── */

static GowlFloatdeskCatalog *
sample_catalog(void)
{
	GowlFloatdeskCatalog *c = gowl_floatdesk_catalog_new();

	gowl_floatdesk_catalog_add(c, "firefox.desktop", "Firefox", "firefox",
	                           "firefox %u", NULL, FALSE, FALSE);
	gowl_floatdesk_catalog_add(c, "org.gnome.Nautilus.desktop", "Files",
	                           "org.gnome.Nautilus", "nautilus --new-window %U",
	                           NULL, FALSE, FALSE);
	gowl_floatdesk_catalog_add(c, "steam.desktop", "Steam", "steam",
	                           "steam %U", "Steam", FALSE, FALSE);
	gowl_floatdesk_catalog_add(c, "btop.desktop", "btop++", "btop",
	                           "btop", NULL, TRUE, FALSE);
	gowl_floatdesk_catalog_add(c, "hidden.desktop", "Hidden", NULL,
	                           "hidden", NULL, FALSE, TRUE);
	return c;
}

static void
test_windows_resolve_to_applications(void)
{
	GowlFloatdeskCatalog   *c = sample_catalog();
	const GowlFloatdeskApp *app;
	g_autofree gchar       *k1 = NULL, *k2 = NULL, *k3 = NULL, *k4 = NULL;

	app = gowl_floatdesk_catalog_resolve(c, "firefox");
	g_assert_nonnull(app);
	g_assert_cmpstr(app->exec, ==, "firefox");          /* %u removed */
	g_assert_cmpstr(gowl_floatdesk_catalog_resolve(c, "Firefox")->id, ==,
	                "firefox");
	/* StartupWMClass. */
	g_assert_cmpstr(gowl_floatdesk_catalog_resolve(c, "Steam")->id, ==,
	                "steam");
	/* The last dotted part, both ways. */
	g_assert_cmpstr(gowl_floatdesk_catalog_resolve(c, "nautilus")->id, ==,
	                "org.gnome.Nautilus");
	g_assert_cmpstr(gowl_floatdesk_catalog_resolve(c, "org.mozilla.firefox")->id,
	                ==, "firefox");
	/* A TUI in a terminal is its own application. */
	g_assert_cmpstr(gowl_floatdesk_catalog_resolve(c, "tui.btop")->id, ==,
	                "btop");
	g_assert_cmpstr(gowl_floatdesk_catalog_resolve(c, "TUI.btop")->id, ==,
	                "btop");
	g_assert_null(gowl_floatdesk_catalog_resolve(c, "foot"));
	g_assert_null(gowl_floatdesk_catalog_resolve(c, NULL));

	k1 = gowl_floatdesk_catalog_key(c, "org.gnome.Nautilus");
	k2 = gowl_floatdesk_catalog_key(c, "Foot");
	k3 = gowl_floatdesk_catalog_key(c, NULL);
	k4 = gowl_floatdesk_catalog_key(c, "tui.btop");
	g_assert_cmpstr(k1, ==, "org.gnome.Nautilus");
	g_assert_cmpstr(k2, ==, "foot");     /* no entry: its own app */
	g_assert_cmpstr(k3, ==, "unknown");
	g_assert_cmpstr(k4, ==, "btop");
	gowl_floatdesk_catalog_free(c);
}

static void
test_the_grid_lists_launchable_apps_by_name(void)
{
	GowlFloatdeskCatalog *c = sample_catalog();
	GPtrArray            *list = gowl_floatdesk_catalog_list(c);
	guint                 i;

	g_assert_cmpuint(list->len, ==, 4);           /* not the hidden one */
	for (i = 1; i < list->len; i++)
		g_assert_cmpint(g_utf8_collate(
			((GowlFloatdeskApp *)g_ptr_array_index(list, i - 1))->name,
			((GowlFloatdeskApp *)g_ptr_array_index(list, i))->name),
			<=, 0);
	g_ptr_array_unref(list);
	gowl_floatdesk_catalog_free(c);
}

static void
test_field_codes(void)
{
	g_autofree gchar *a = gowl_floatdesk_strip_field_codes("app %U --x %f");
	g_autofree gchar *b = gowl_floatdesk_strip_field_codes("echo 100%%");
	g_autofree gchar *c = gowl_floatdesk_strip_field_codes("tail %");

	g_assert_cmpstr(a, ==, "app  --x");
	g_assert_cmpstr(b, ==, "echo 100%");
	g_assert_cmpstr(c, ==, "tail");
}

static void
test_terminal_launch_lines(void)
{
	g_auto(GStrv) foot = NULL, kitty = NULL, st = NULL, other = NULL;
	g_autofree gchar *f = NULL, *k = NULL, *s = NULL, *o = NULL;

	foot = gowl_floatdesk_terminal_argv("foot", "tui.btop", "btop -p 1", NULL);
	kitty = gowl_floatdesk_terminal_argv("kitty", "tui.btop", "btop", NULL);
	st = gowl_floatdesk_terminal_argv("/usr/bin/gst", "tui.btop", "btop", NULL);
	other = gowl_floatdesk_terminal_argv("xterm", "tui.btop", "btop", NULL);
	f = g_strjoinv(" ", foot);
	k = g_strjoinv(" ", kitty);
	s = g_strjoinv(" ", st);
	o = g_strjoinv(" ", other);
	g_assert_cmpstr(f, ==, "foot --app-id=tui.btop btop -p 1");
	g_assert_cmpstr(k, ==, "kitty --class tui.btop btop");
	g_assert_cmpstr(s, ==, "/usr/bin/gst -c tui.btop -e btop");
	g_assert_cmpstr(o, ==, "xterm -e btop");
}

static void
test_click_cycle(void)
{
	g_assert_cmpint(gowl_floatdesk_cycle_next(0, -1), ==, -1);
	g_assert_cmpint(gowl_floatdesk_cycle_next(3, -1), ==, 0);
	g_assert_cmpint(gowl_floatdesk_cycle_next(3, 0), ==, 1);
	g_assert_cmpint(gowl_floatdesk_cycle_next(3, 2), ==, 0);
	g_assert_cmpint(gowl_floatdesk_cycle_next(1, 0), ==, 0);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/floatdesk/state/new-tiles-everything",
	                test_new_state_tiles_everything);
	g_test_add_func("/floatdesk/state/explicit-beats-default",
	                test_explicit_setting_beats_the_default);
	g_test_add_func("/floatdesk/state/mask-needs-every-tag",
	                test_a_mask_floats_only_if_every_tag_does);
	g_test_add_func("/floatdesk/state/previous-layout",
	                test_previous_layout_is_remembered_per_tag);
	g_test_add_func("/floatdesk/decide/enter",
	                test_enter_floats_and_claims_tiled_windows);
	g_test_add_func("/floatdesk/decide/exit-gives-back-only-its-own",
	                test_exit_gives_back_only_what_it_took);
	g_test_add_func("/floatdesk/decide/claim-on-tiled-cleared",
	                test_a_claim_on_a_tiled_window_is_cleared);
	g_test_add_func("/floatdesk/decide/move-in-and-out",
	                test_moving_between_workspaces);
	g_test_add_func("/floatdesk/decide/fullscreen-waits",
	                test_fullscreen_waits);
	g_test_add_func("/floatdesk/decide/never-touched",
	                test_windows_the_mode_never_touches);
	g_test_add_func("/floatdesk/file/round-trip",
	                test_round_trip_through_yaml);
	g_test_add_func("/floatdesk/file/empty-pins",
	                test_empty_pins_survive);
	g_test_add_func("/floatdesk/file/bad-file-changes-nothing",
	                test_a_bad_file_changes_nothing);
	g_test_add_func("/floatdesk/file/save-and-load",
	                test_save_and_load_a_file);
	g_test_add_func("/floatdesk/place/cascade",
	                test_cascade_centres_then_steps_then_wraps);
	g_test_add_func("/floatdesk/place/snap-edges", test_snap_edges);
	g_test_add_func("/floatdesk/apps/resolve",
	                test_windows_resolve_to_applications);
	g_test_add_func("/floatdesk/apps/grid-list",
	                test_the_grid_lists_launchable_apps_by_name);
	g_test_add_func("/floatdesk/apps/field-codes", test_field_codes);
	g_test_add_func("/floatdesk/apps/terminal-launch",
	                test_terminal_launch_lines);
	g_test_add_func("/floatdesk/apps/click-cycle", test_click_cycle);

	return g_test_run();
}
