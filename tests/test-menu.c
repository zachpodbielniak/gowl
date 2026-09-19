/* test-menu.c -- the menu model
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The menu is data, and every interesting property of it is a property
 * of the reading rather than of the drawing --- which is exactly why the
 * model is a separate thing from the card on screen.  What is asserted
 * here, and why none of it is visible by looking at the menu:
 *
 *   NESTING IS THE ROUTE.  A row's place in the file decides what it is
 *   called, so a submenu moved in the file silently renames everything
 *   under it unless the routes really are derived.
 *
 *   THE USER'S FILE IS AN OVERLAY, NOT A REPLACEMENT.  Repeating an id
 *   to change a label has to keep the icon, the action, the guards AND
 *   the position; the tempting implementation replaces the entry and
 *   sends the row to the bottom of the list with no icon.
 *
 *   A GUARD THAT FAILS HIDES EXACTLY ONE ROW.  The failure mode is a
 *   guard that hides its siblings, or one that hides nothing because it
 *   was never evaluated --- and a menu row that lies about the state it
 *   describes is worse than one that is missing.
 *
 *   SEARCH IS GLOBAL AND ORDERED.  Somebody typing `wifi' knows the word
 *   and not the submenu.  An exact label has to beat a prefix has to
 *   beat a substring, or the first row is not the answer and the whole
 *   type-and-press-enter motion stops working.
 *
 *   A PROVIDER ROW IS ACTED ON BY NAME.  Provider rows are not in the
 *   tree, so activating one has to find its parent and ask again.
 *
 *   AND THE SHIPPED TREE HAS TO PARSE, which is the one thing that
 *   cannot be checked by reading the code.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <math.h>

#include "menu/gowl-menu.h"
#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "module/gowl-module-manager.h"
#include "core/gowl-monitor.h"

/*
 * Warnings, captured rather than fatal.
 *
 * gowl is built with G_LOG_USE_STRUCTURED, and g_test_expect_message()
 * does not see structured logs -- an expected warning aborts the run
 * anyway.  A writer function does see them, so the one test that means
 * to provoke a warning can assert on it, and a warning nobody asked for
 * still fails the test it happened in.
 */
static gchar *last_warning;
static gboolean warning_expected;

static GLogWriterOutput
capture_writer(GLogLevelFlags level, const GLogField *fields, gsize n_fields,
               gpointer user_data)
{
	const gchar *message = NULL;
	gsize i;

	for (i = 0; i < n_fields; i++) {
		if (g_strcmp0(fields[i].key, "MESSAGE") == 0)
			message = fields[i].value;
	}
	if ((level & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL)) != 0) {
		g_free(last_warning);
		last_warning = g_strdup(message != NULL ? message : "");
		if (!warning_expected) {
			g_printerr("unexpected warning: %s\n", last_warning);
			g_test_fail();
		}
		return G_LOG_WRITER_HANDLED;
	}
	return G_LOG_WRITER_HANDLED;
}

/*
 * A directory of desktop entries, set up once for the whole process.
 *
 * Not per test: GIO reads $XDG_DATA_DIRS once and caches the result, so
 * a second test setting its own would silently get the first one's
 * applications -- which passes or fails depending on registration
 * order, and is the kind of test failure that takes an hour.
 */
static gchar *apps_dir;
static gchar *apps_marker;

static void
apps_fixture_up(void)
{
	g_autofree gchar *apps = NULL;

	apps_dir = g_dir_make_tmp("gowl-menu-apps-XXXXXX", NULL);
	g_assert_nonnull(apps_dir);
	apps = g_build_filename(apps_dir, "applications", NULL);
	g_assert_cmpint(g_mkdir_with_parents(apps, 0700), ==, 0);
	apps_marker = g_build_filename(apps_dir, "ran", NULL);

#define ENTRY(file, body) \
	G_STMT_START { \
		g_autofree gchar *p_ = g_build_filename(apps, (file), NULL); \
		g_assert_true(g_file_set_contents(p_, (body), -1, NULL)); \
	} G_STMT_END

	{
		g_autofree gchar *body = g_strdup_printf(
			"[Desktop Entry]\nType=Application\nName=Gowl Test App\n"
			"Exec=/usr/bin/touch %s %%U\n", apps_marker);

		ENTRY("gowl-test-app.desktop", body);
	}
	ENTRY("gowl-hidden-app.desktop",
	      "[Desktop Entry]\nType=Application\nName=Gowl Hidden App\n"
	      "NoDisplay=true\nExec=/bin/true\n");
	ENTRY("gowl-zzsearchable.desktop",
	      "[Desktop Entry]\nType=Application\nName=Zzsearchable Widget\n"
	      "Icon=utilities-terminal\nExec=/bin/true\n");
	ENTRY("gowl-zzclash.desktop",
	      "[Desktop Entry]\nType=Application\nName=Zzclash\n"
	      "Exec=/bin/true\n");
#undef ENTRY

	/*
	 * This directory FIRST, and the machine's own after it.
	 *
	 * Replacing the system directories outright would be tidier, and
	 * would also make the enumeration instant -- which silently turns
	 * the cache test below into a skip, since there would be nothing
	 * slow left to cache.  The fixture entries are named distinctively
	 * enough (`Zzclash', `Zzsearchable Widget') that a real
	 * application cannot collide with them.
	 */
	{
		const gchar *system_dirs = g_getenv("XDG_DATA_DIRS");
		g_autofree gchar *joined = g_strdup_printf("%s:%s", apps_dir,
			system_dirs != NULL && *system_dirs != '\0'
				? system_dirs : "/usr/local/share:/usr/share");

		g_setenv("XDG_DATA_HOME", apps_dir, TRUE);
		g_setenv("XDG_DATA_DIRS", joined, TRUE);
	}
}

static void
apps_fixture_down(void)
{
	g_autofree gchar *apps = NULL;
	g_autoptr(GDir) dir = NULL;
	const gchar *name;

	if (apps_dir == NULL)
		return;
	apps = g_build_filename(apps_dir, "applications", NULL);
	dir = g_dir_open(apps, 0, NULL);
	while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *path = g_build_filename(apps, name, NULL);

		g_unlink(path);
	}
	g_unlink(apps_marker);
	g_rmdir(apps);
	g_rmdir(apps_dir);
	g_clear_pointer(&apps_dir, g_free);
	g_clear_pointer(&apps_marker, g_free);
}

/* A small tree with one of everything the reader has to understand. */
static const gchar *const SAMPLE =
	"menu:\n"
	"  - id: style\n"
	"    icon: S\n"
	"    label: Style\n"
	"    aliases: [look]\n"
	"    items:\n"
	"      - id: backdrop\n"
	"        icon: B\n"
	"        label: Backdrop\n"
	"        desc: what shows through a window\n"
	"        spawn: \"/bin/true\"\n"
	"      - id: next-backdrop\n"
	"        label: Next backdrop\n"
	"        spawn: \"/bin/true\"\n"
	"      - id: tube\n"
	"        label: Cathode ray tube\n"
	"        spawn: \"/bin/true\"\n"
	"        checked: {ipc: \"ping\", is: \"pong\"}\n"
	"      - id: gone\n"
	"        label: Never here\n"
	"        spawn: \"/bin/true\"\n"
	"        when: {exists: gowl-no-such-program-anywhere}\n"
	"      - id: dim\n"
	"        label: Already installed\n"
	"        spawn: \"/bin/true\"\n"
	"        disabled: {ipc: \"ping\", is: \"pong\"}\n"
	"  - id: system\n"
	"    label: System\n"
	"    items:\n"
	"      - id: lock\n"
	"        label: Lock\n"
	"        spawn: \"/bin/true\"\n"
	"      - id: louder\n"
	"        label: Volume up\n"
	"        spawn: \"/bin/true\"\n"
	"        keep-open: true\n"
	"  - id: shortcut\n"
	"    label: Straight to style\n"
	"    target: style\n"
	"  - label: No Id At All\n"
	"    spawn: \"/bin/true\"\n";

static GowlMenu *
sample_menu(void)
{
	GowlMenu *menu = gowl_menu_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(gowl_menu_load_data(menu, SAMPLE, FALSE, &error));
	g_assert_no_error(error);
	return menu;
}

/* A compositor with no backend: enough for guards and for a spawn. */
static GowlCompositor *
bare_compositor(void)
{
	GowlCompositor *c = gowl_compositor_new();

	c->module_mgr = gowl_module_manager_new();
	return c;
}

static const GowlMenuRow *
row_named(GPtrArray *rows, const gchar *label)
{
	guint i;

	for (i = 0; rows != NULL && i < rows->len; i++) {
		const GowlMenuRow *r = g_ptr_array_index(rows, i);

		if (g_strcmp0(r->label, label) == 0)
			return r;
	}
	return NULL;
}

/* ── The tree ───────────────────────────────────────────────────── */

static void
test_nesting_makes_the_route(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();

	g_assert_true(gowl_menu_has_route(menu, "style"));
	g_assert_true(gowl_menu_has_route(menu, "style.backdrop"));
	g_assert_false(gowl_menu_has_route(menu, "backdrop"));

	/* And back up again, which is what Backspace in the card does. */
	{
		g_autofree gchar *up = gowl_menu_get_parent(menu, "style.backdrop");

		g_assert_cmpstr(up, ==, "style");
	}
	{
		g_autofree gchar *up = gowl_menu_get_parent(menu, "style");

		g_assert_cmpstr(up, ==, "root");
	}
	{
		g_autofree gchar *up = gowl_menu_get_parent(menu, "root");

		g_assert_null(up);
	}
}

static void
test_an_entry_without_an_id_gets_one(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();

	/* `No Id At All' -> `no-id-at-all': lower case, dashes for the
	 * spaces, because the result is something people type. */
	g_assert_true(gowl_menu_has_route(menu, "no-id-at-all"));
}

static void
test_root_lists_only_the_top(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	g_autoptr(GPtrArray) rows = gowl_menu_list(menu, NULL, NULL);

	g_assert_cmpuint(rows->len, ==, 4);
	g_assert_nonnull(row_named(rows, "Style"));
	g_assert_nonnull(row_named(rows, "System"));
	g_assert_null(row_named(rows, "Backdrop"));
}

static void
test_a_submenu_row_says_it_is_one(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	g_autoptr(GPtrArray) rows = gowl_menu_list(menu, NULL, NULL);
	const GowlMenuRow *style = row_named(rows, "Style");

	g_assert_nonnull(style);
	g_assert_true(style->submenu);
	/* Three of the four children are visible with no compositor: the
	 * `exists' guard on `gone' is answered without one. */
	g_assert_cmpuint(style->children, ==, 4);

	g_assert_false(row_named(rows, "No Id At All")->submenu);
}

static void
test_the_title_is_the_title_when_there_is_one(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	g_autofree gchar *plain = NULL;
	g_autofree gchar *titled = NULL;

	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n"
		"  - id: defaults\n"
		"    label: Defaults\n"
		"    items:\n"
		"      - id: browser\n"
		"        label: Browser\n"
		"        title: Default browser\n"
		"        items:\n"
		"          - {id: firefox, label: Firefox, spawn: \"/bin/true\"}\n"
		"      - id: plain\n"
		"        label: Plain\n"
		"        items:\n"
		"          - {id: x, label: X, spawn: \"/bin/true\"}\n",
		FALSE, NULL));

	titled = gowl_menu_get_title(menu, "defaults.browser");
	g_assert_cmpstr(titled, ==, "Default browser");
	plain = gowl_menu_get_title(menu, "defaults.plain");
	g_assert_cmpstr(plain, ==, "Plain");
}

/* ── The overlay ────────────────────────────────────────────────── */

static void
test_an_overlay_changes_only_what_it_names(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	g_autoptr(GPtrArray) rows = NULL;
	const GowlMenuRow *row;

	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n"
		"  - id: style\n"
		"    items:\n"
		"      - id: backdrop\n"
		"        label: Wallpaper effect\n",
		TRUE, NULL));

	rows = gowl_menu_list(menu, NULL, "style");
	row  = row_named(rows, "Wallpaper effect");
	g_assert_nonnull(row);
	/* The icon survived, which is the whole point of an overlay. */
	g_assert_cmpstr(row->icon, ==, "B");
	/* And so did its place: still the first row under Style. */
	g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(rows, 0))->label,
	                ==, "Wallpaper effect");
}

static void
test_an_overlay_adds_a_new_row_at_the_end(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	g_autoptr(GPtrArray) rows = NULL;

	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n"
		"  - id: style\n"
		"    items:\n"
		"      - {id: mine, label: Mine, spawn: \"/bin/true\"}\n",
		TRUE, NULL));

	rows = gowl_menu_list(menu, NULL, "style");
	g_assert_nonnull(row_named(rows, "Mine"));
	g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(rows, rows->len - 1))->label,
	                ==, "Mine");
}

/* ── Guards ─────────────────────────────────────────────────────── */

static void
test_a_failed_when_hides_one_row(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	g_autoptr(GPtrArray) rows = gowl_menu_list(menu, NULL, "style");

	g_assert_null(row_named(rows, "Never here"));
	/* Its siblings are all still there. */
	g_assert_nonnull(row_named(rows, "Backdrop"));
	g_assert_nonnull(row_named(rows, "Cathode ray tube"));
	g_assert_nonnull(row_named(rows, "Already installed"));
}

static void
test_an_ipc_guard_reads_the_answer(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	GowlCompositor *comp = bare_compositor();
	g_autoptr(GPtrArray) with = NULL;
	g_autoptr(GPtrArray) without = NULL;

	with = gowl_menu_list(menu, comp, "style");
	g_assert_true(row_named(with, "Cathode ray tube")->checked);

	/* No compositor, no answer, no tick -- rather than a tick that
	 * means nothing. */
	without = gowl_menu_list(menu, NULL, "style");
	g_assert_false(row_named(without, "Cathode ray tube")->checked);

	g_object_unref(comp);
}

static void
test_a_row_with_no_checked_guard_is_not_ticked(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	GowlCompositor *comp = bare_compositor();
	g_autoptr(GPtrArray) rows = gowl_menu_list(menu, comp, "style");

	/*
	 * No guard means not ticked, where no `when:' guard means visible.
	 * Read the other way round -- absence treated as success for every
	 * guard alike -- every row in the whole menu wears a tick, which
	 * is what the first version of this did.
	 */
	g_assert_false(row_named(rows, "Backdrop")->checked);
	g_assert_false(row_named(rows, "Next backdrop")->checked);
	/* And the one that does have a guard still ticks. */
	g_assert_true(row_named(rows, "Cathode ray tube")->checked);

	g_object_unref(comp);
}

static void
test_an_ipc_guard_can_be_inverted(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	GowlCompositor *comp = bare_compositor();
	g_autoptr(GPtrArray) rows = NULL;

	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n"
		"  - {id: a, label: Pong, spawn: \"/bin/true\",\n"
		"     when: {ipc: \"ping\", is: \"pong\"}}\n"
		"  - {id: b, label: Not pong, spawn: \"/bin/true\",\n"
		"     when: {ipc: \"ping\", is: \"pong\", not: true}}\n",
		FALSE, NULL));

	rows = gowl_menu_list(menu, comp, NULL);
	g_assert_nonnull(row_named(rows, "Pong"));
	g_assert_null(row_named(rows, "Not pong"));

	g_object_unref(comp);
}

static void
test_a_module_guard_reads_the_manager(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	GowlCompositor *comp = bare_compositor();
	g_autoptr(GPtrArray) rows = NULL;

	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n"
		"  - {id: a, label: Needs blur, spawn: \"/bin/true\",\n"
		"     when: {module: blur}}\n",
		FALSE, NULL));

	/* Nothing is loaded into this manager, so the row is not offered. */
	rows = gowl_menu_list(menu, comp, NULL);
	g_assert_null(row_named(rows, "Needs blur"));

	g_object_unref(comp);
}

static void
test_an_embedder_guard_follows_the_handler(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	GowlCompositor *comp = bare_compositor();
	g_autoptr(GPtrArray) before = NULL;
	g_autoptr(GPtrArray) after = NULL;

	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n"
		"  - {id: a, label: In cmacs, elisp: \"(ignore)\",\n"
		"     when: {embedder: true}}\n"
		"  - {id: b, label: Standalone, spawn: \"/bin/true\",\n"
		"     when: {embedder: false}}\n",
		FALSE, NULL));

	before = gowl_menu_list(menu, comp, NULL);
	g_assert_null(row_named(before, "In cmacs"));
	g_assert_nonnull(row_named(before, "Standalone"));

	comp->custom_action_func = (GowlCustomActionFunc)0x1;
	after = gowl_menu_list(menu, comp, NULL);
	g_assert_nonnull(row_named(after, "In cmacs"));
	g_assert_null(row_named(after, "Standalone"));
	comp->custom_action_func = NULL;

	g_object_unref(comp);
}

static void
test_a_disabled_row_is_listed_ticked_and_inert(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	GowlCompositor *comp = bare_compositor();
	g_autoptr(GPtrArray) rows = NULL;
	const GowlMenuRow *row;

	rows = gowl_menu_list(menu, comp, "style");
	row  = row_named(rows, "Already installed");
	g_assert_nonnull(row);
	g_assert_true(row->disabled);
	/* Dim means "you already have this", which is the same thing the
	 * tick means everywhere else. */
	g_assert_true(row->checked);

	g_assert_cmpint(gowl_menu_activate(menu, comp, "style.dim", NULL),
	                ==, GOWL_MENU_RESULT_NONE);

	g_object_unref(comp);
}

/* ── Routes and aliases ─────────────────────────────────────────── */

/*
 * Typing finds an application, not just a menu row.
 *
 * Without this the search covers everything somebody wrote in
 * menu.yaml and none of the two hundred applications on the machine --
 * which is the half people actually search for, and the half that makes
 * the menu a launcher rather than only a control surface.
 */
static void
test_search_finds_provider_rows(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	g_autoptr(GPtrArray) rows = NULL;
	GowlCompositor *comp;
	const GowlMenuRow *row;

	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n"
		"  - {id: apps, label: Apps, provider: apps}\n"
		"  - {id: other, label: Something else, spawn: \"/bin/true\"}\n",
		FALSE, NULL));

	comp = bare_compositor();
	rows = gowl_menu_search(menu, comp, "zzsearchable");

	row = row_named(rows, "Zzsearchable Widget");
	g_assert_nonnull(row);
	/* Routed under the provider's own submenu, so choosing it can find
	 * its way back to the provider that made it. */
	g_assert_true(g_str_has_prefix(row->route, "apps."));
	/* And it says where it lives, like every other search result. */
	g_assert_cmpstr(row->detail, ==, "Apps");
	/* The THEMED NAME, not pixels: turning it into an image needs an
	 * icon theme, which is a front end's business. */
	g_assert_cmpstr(row->icon_name, ==, "utilities-terminal");

	g_object_unref(comp);
}

/*
 * A declared row outranks an application that matched as well.
 *
 * An application called `Settings' must not take the route the Setup
 * menu is reached by -- omarchy documents having made exactly that
 * mistake, with htop's Keywords capturing the system menu.
 */
static void
test_a_declared_row_outranks_an_app(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	g_autoptr(GPtrArray) rows = NULL;
	GowlCompositor *comp;

	/* A declared row with the SAME label as an installed application. */
	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n"
		"  - {id: apps, label: Apps, provider: apps}\n"
		"  - {id: zzclash, label: Zzclash, spawn: \"/bin/true\"}\n",
		FALSE, NULL));

	comp = bare_compositor();
	rows = gowl_menu_search(menu, comp, "zzclash");
	g_assert_cmpuint(rows->len, ==, 2);
	g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(rows, 0))->route,
	                ==, "zzclash");

	g_object_unref(comp);
}

/*
 * The application list is cached, and it has to be.
 *
 * Enumerating desktop entries costs about 18ms and GIO does not
 * memoise it; search runs providers on every keystroke, on the
 * compositor thread.  Uncached, typing six characters is a tenth of a
 * second of stutter across the whole desktop.
 *
 * Asserted as a RATIO rather than an absolute time, and skipped
 * entirely on a machine where the first call was already fast -- a
 * build container with no desktop entries has nothing to measure.
 */
static void
test_the_app_list_is_cached(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	gint64 first, second, t;

	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n  - {id: apps, label: Apps, provider: apps}\n",
		FALSE, NULL));

	t = g_get_monotonic_time();
	{
		g_autoptr(GPtrArray) rows = gowl_menu_list(menu, NULL, "apps");

		g_assert_nonnull(rows);
	}
	first = g_get_monotonic_time() - t;

	if (first < 2000) {
		g_test_skip("nothing installed to enumerate");
		return;
	}

	t = g_get_monotonic_time();
	{
		g_autoptr(GPtrArray) rows = gowl_menu_list(menu, NULL, "apps");

		g_assert_nonnull(rows);
	}
	second = g_get_monotonic_time() - t;

	/* Generous: uncached the two are the same, so anything like a
	 * factor apart is the cache and nothing else. */
	g_assert_cmpint(second * 4, <, first);
}

/*
 * ... but only the expensive one, and only for a few seconds.
 *
 * A window list five seconds stale is a list of windows that may not be
 * there any more, and acting on one focuses whatever took its place.
 */
static void
test_only_the_app_list_is_cached(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	GowlCompositor *comp = bare_compositor();
	g_autoptr(GPtrArray) before = NULL;
	g_autoptr(GPtrArray) after = NULL;
	GowlMonitor *mon;

	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n  - {id: tags, label: Tags, provider: tags}\n",
		FALSE, NULL));

	mon = g_object_new(GOWL_TYPE_MONITOR, NULL);
	comp->selmon = mon;
	gowl_monitor_set_tags(mon, 1);

	before = gowl_menu_list(menu, comp, "tags");
	g_assert_true(((GowlMenuRow *)g_ptr_array_index(before, 0))->checked);

	/* Move to tag 2.  A cached answer would still tick tag 1. */
	gowl_monitor_set_tags(mon, 2);
	after = gowl_menu_list(menu, comp, "tags");
	g_assert_false(((GowlMenuRow *)g_ptr_array_index(after, 0))->checked);
	g_assert_true(((GowlMenuRow *)g_ptr_array_index(after, 1))->checked);

	comp->selmon = NULL;
	g_object_unref(mon);
	g_object_unref(comp);
}

static void
test_an_empty_submenu_disappears(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	g_autoptr(GPtrArray) rows = NULL;

	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n"
		"  - id: hardware\n"
		"    label: Hardware\n"
		"    items:\n"
		"      - {id: a, label: Touchpad, spawn: \"/bin/true\",\n"
		"         when: {exists: gowl-no-such-program-anywhere}}\n"
		"      - {id: b, label: Haptics, spawn: \"/bin/true\",\n"
		"         when: {exists: gowl-no-such-program-anywhere}}\n"
		"  - id: keeps\n"
		"    label: Keeps one\n"
		"    items:\n"
		"      - {id: a, label: Gone, spawn: \"/bin/true\",\n"
		"         when: {exists: gowl-no-such-program-anywhere}}\n"
		"      - {id: b, label: Here, spawn: \"/bin/true\"}\n"
		"  - {id: windows, label: Windows, provider: windows}\n",
		FALSE, NULL));

	rows = gowl_menu_list(menu, NULL, NULL);
	/* Every child hidden, so the parent goes too: entering it would
	 * find an empty list, which reads as broken rather than absent. */
	g_assert_null(row_named(rows, "Hardware"));
	/* One survivor is enough to keep the parent. */
	g_assert_nonnull(row_named(rows, "Keeps one"));
	/* A provider-backed submenu stays even with nothing in it: its
	 * rows are made when it opens, and "none right now" is an answer. */
	g_assert_nonnull(row_named(rows, "Windows"));
}

static void
test_routes_resolve(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	g_autofree gchar *by_alias = gowl_menu_resolve(menu, "look");
	g_autofree gchar *by_case = gowl_menu_resolve(menu, "Style.Backdrop");
	g_autofree gchar *by_under = gowl_menu_resolve(menu, "Next_Backdrop");
	g_autofree gchar *by_leaf = gowl_menu_resolve(menu, "tube");
	g_autofree gchar *empty = gowl_menu_resolve(menu, "");
	g_autofree gchar *unknown = gowl_menu_resolve(menu, "nonsense");

	g_assert_cmpstr(by_alias, ==, "style");
	g_assert_cmpstr(by_case, ==, "style.backdrop");
	/* An underscore reads as a dash INSIDE a segment -- the separator
	 * is the dot, and a dash is what the slug spells. */
	g_assert_cmpstr(by_under, ==, "style.next-backdrop");
	/* A leaf segment reaches its row when nothing else claims the
	 * word, which is what makes `gowl menu summon tube' work. */
	g_assert_cmpstr(by_leaf, ==, "style.tube");
	g_assert_cmpstr(empty, ==, "root");
	/* Unknown comes back unchanged so the caller can say what was
	 * asked for rather than silently opening the root. */
	g_assert_cmpstr(unknown, ==, "nonsense");
}

static void
test_an_exact_route_beats_an_alias(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	g_autofree gchar *resolved = NULL;

	/* `system' is a real route AND something else claims it as an
	 * alias.  The route has to win, or a row can capture a name that
	 * already means something. */
	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n"
		"  - {id: system, label: System, items: [{id: a, label: A, spawn: \"/bin/true\"}]}\n"
		"  - {id: other, label: Other, aliases: [system], spawn: \"/bin/true\"}\n",
		FALSE, NULL));

	resolved = gowl_menu_resolve(menu, "system");
	g_assert_cmpstr(resolved, ==, "system");
}

/* ── Search ─────────────────────────────────────────────────────── */

static void
test_search_is_global(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	g_autoptr(GPtrArray) rows = gowl_menu_search(menu, NULL, "lock");

	/* `Lock' lives two levels down and is found from the root. */
	g_assert_cmpuint(rows->len, >, 0);
	g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(rows, 0))->route,
	                ==, "system.lock");
}

static void
test_search_carries_the_path(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	g_autoptr(GPtrArray) rows = gowl_menu_search(menu, NULL, "lock");

	g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(rows, 0))->detail,
	                ==, "System");
}

static void
test_search_puts_the_best_answer_first(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	g_autoptr(GPtrArray) rows = NULL;

	/* Declaration order is deliberately the WORST order, so an
	 * implementation that forgot to sort fails this. */
	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n"
		"  - {id: a, label: Unlock the door, spawn: \"/bin/true\"}\n"
		"  - {id: b, label: Locking policy, spawn: \"/bin/true\"}\n"
		"  - {id: c, label: Lock, spawn: \"/bin/true\"}\n",
		FALSE, NULL));

	rows = gowl_menu_search(menu, NULL, "lock");
	g_assert_cmpuint(rows->len, ==, 3);
	/* exact, then prefix, then substring */
	g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(rows, 0))->label,
	                ==, "Lock");
	g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(rows, 1))->label,
	                ==, "Locking policy");
	g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(rows, 2))->label,
	                ==, "Unlock the door");
}

static void
test_search_matches_the_description(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	g_autoptr(GPtrArray) rows = gowl_menu_search(menu, NULL, "shows through");

	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(rows, 0))->route,
	                ==, "style.backdrop");
}

static void
test_search_skips_a_hidden_row(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	g_autoptr(GPtrArray) rows = gowl_menu_search(menu, NULL, "never");

	/* A row that is hidden in the list must not be reachable by
	 * typing its name either. */
	g_assert_cmpuint(rows->len, ==, 0);
}

/* ── Activation ─────────────────────────────────────────────────── */

static void
test_activating_a_submenu_opens_it(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	g_autofree gchar *next = NULL;

	g_assert_cmpint(gowl_menu_activate(menu, NULL, "style", &next),
	                ==, GOWL_MENU_RESULT_OPEN);
	g_assert_cmpstr(next, ==, "style");
}

static void
test_a_link_opens_its_target(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	g_autofree gchar *next = NULL;

	g_assert_cmpint(gowl_menu_activate(menu, NULL, "shortcut", &next),
	                ==, GOWL_MENU_RESULT_OPEN);
	g_assert_cmpstr(next, ==, "style");
}

static void
test_a_link_lists_what_it_points_at(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	g_autoptr(GPtrArray) rows = gowl_menu_list(menu, NULL, "shortcut");

	g_assert_nonnull(row_named(rows, "Backdrop"));
}

static void
test_an_action_runs_and_closes(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	GowlCompositor *comp = bare_compositor();

	g_assert_cmpint(gowl_menu_activate(menu, comp, "system.lock", NULL),
	                ==, GOWL_MENU_RESULT_RAN);
	g_object_unref(comp);
}

static void
test_keep_open_leaves_the_menu_up(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	GowlCompositor *comp = bare_compositor();

	/* A volume row is pressed several times in a row; closing after
	 * the first press is the bug this exists to stop. */
	g_assert_cmpint(gowl_menu_activate(menu, comp, "system.louder", NULL),
	                ==, GOWL_MENU_RESULT_RAN_OPEN);
	g_object_unref(comp);
}

static void
test_an_unknown_route_does_nothing(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	GowlCompositor *comp = bare_compositor();

	g_assert_cmpint(gowl_menu_activate(menu, comp, "not.a.route", NULL),
	                ==, GOWL_MENU_RESULT_NONE);
	g_object_unref(comp);
}

/* ── Providers ──────────────────────────────────────────────────── */

static void
test_a_provider_makes_rows(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	GowlCompositor *comp = bare_compositor();
	g_autoptr(GPtrArray) rows = NULL;

	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n"
		"  - {id: tags, label: Tags, provider: tags}\n",
		FALSE, NULL));

	rows = gowl_menu_list(menu, comp, "tags");
	g_assert_cmpuint(rows->len, ==, 9);
	g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(rows, 0))->label,
	                ==, "Tag 1");
	/* Routes under the provider's own submenu, so activating one can
	 * find its way back. */
	g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(rows, 0))->route,
	                ==, "tags.1");
	g_object_unref(comp);
}

/*
 * The launcher, against a desktop entry written for the purpose.
 *
 * Two things about it are not visible in the code: whether an entry is
 * found at all, and what happens to its FIELD CODES.  An Exec line reads
 * `firefox %u' -- a placeholder for the file being opened, which a menu
 * never has -- and leaving it in launches the program with a literal
 * `%u' as its argument, which most of them treat as a filename.
 *
 * The check is indirect, because a provider row's action is not exposed:
 * a NoDisplay entry must not be offered, a normal one must, and the
 * route is derived from the entry's id.  The field-code half is asserted
 * by activating the row and watching what runs.
 */
static void
test_the_apps_provider_reads_desktop_entries(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	g_autoptr(GPtrArray) rows = NULL;
	GowlCompositor *comp;
	const GowlMenuRow *row;
	gint waited;

	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n  - {id: apps, label: Apps, provider: apps}\n",
		FALSE, NULL));

	comp = bare_compositor();
	rows = gowl_menu_list(menu, comp, "apps");

	row = row_named(rows, "Gowl Test App");
	g_assert_nonnull(row);
	/* NoDisplay means the machine has it and the menu should not offer
	 * it -- a helper registered for a MIME type is not a thing anybody
	 * launches by name. */
	g_assert_null(row_named(rows, "Gowl Hidden App"));

	/*
	 * Activating it runs the Exec line with its field codes gone.  The
	 * marker is created only if `touch' was given exactly the path and
	 * not a literal `%U' as a second filename -- which would also
	 * create a file called `%U' in the working directory.
	 */
	g_unlink(apps_marker);
	g_assert_cmpint(gowl_menu_activate(menu, comp, row->route, NULL),
	                ==, GOWL_MENU_RESULT_RAN);
	for (waited = 0; waited < 200; waited++) {
		if (g_file_test(apps_marker, G_FILE_TEST_EXISTS))
			break;
		g_usleep(10000);
	}
	g_assert_true(g_file_test(apps_marker, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test("%U", G_FILE_TEST_EXISTS));

	g_object_unref(comp);
}

static void
test_a_provider_submenu_says_it_is_one(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	g_autoptr(GPtrArray) rows = NULL;

	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n"
		"  - {id: tags, label: Tags, provider: tags}\n",
		FALSE, NULL));

	/* It has no children in the file, so only the provider makes it a
	 * submenu -- and a row that does not say so cannot be entered. */
	rows = gowl_menu_list(menu, NULL, NULL);
	g_assert_true(row_named(rows, "Tags")->submenu);
	g_assert_true(gowl_menu_is_submenu(menu, "tags"));
}

static void
test_an_unknown_provider_costs_only_itself(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) top = NULL;

	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n"
		"  - {id: bogus, label: Bogus, provider: no-such-provider}\n"
		"  - {id: fine, label: Fine, spawn: \"/bin/true\"}\n",
		FALSE, NULL));

	warning_expected = TRUE;
	g_clear_pointer(&last_warning, g_free);
	rows = gowl_menu_list(menu, NULL, "bogus");
	warning_expected = FALSE;
	g_assert_nonnull(last_warning);
	g_assert_nonnull(strstr(last_warning, "unknown provider"));
	g_assert_cmpuint(rows->len, ==, 0);

	top = gowl_menu_list(menu, NULL, NULL);
	g_assert_nonnull(row_named(top, "Fine"));
}

/* ── The shipped tree ───────────────────────────────────────────── */

/*
 * The shipped tree has to be findable from ANY working directory.
 *
 * The process that looks for menu.yaml is not always gowl: under
 * `cmacs --gowl' the compositor is the editor, started from the
 * editor's tree.  A relative `data/menu.yaml' then finds nothing --
 * and finding nothing does not fail, it leaves the menu empty, which
 * on screen is a key that does nothing at all.
 */
static void
test_the_tree_is_found_from_anywhere(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *elsewhere = NULL;
	g_autofree gchar *back = g_get_current_dir();
	gboolean loaded;

	elsewhere = g_dir_make_tmp("gowl-menu-cwd-XXXXXX", NULL);
	g_assert_nonnull(elsewhere);
	g_assert_cmpint(g_chdir(elsewhere), ==, 0);

	loaded = gowl_menu_load(menu, &error);

	g_assert_cmpint(g_chdir(back), ==, 0);
	g_rmdir(elsewhere);

	g_assert_true(loaded);
	g_assert_cmpuint(gowl_menu_n_entries(menu), >, 0);
}

/*
 * An empty tree still opens.
 *
 * The root is a list by definition, even with nothing in it.  Answering
 * "not a submenu" for it sends the caller down the action path, where
 * an entry with no action does nothing and says nothing -- so a menu
 * whose file was not found looks exactly like a key that is not bound,
 * which is the one failure that cost this feature an evening.
 */
static void
test_an_empty_tree_still_opens(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	g_autoptr(GPtrArray) rows = NULL;

	g_assert_true(gowl_menu_load_data(menu, "menu: []\n", FALSE, NULL));
	g_assert_cmpuint(gowl_menu_n_entries(menu), ==, 0);

	g_assert_true(gowl_menu_is_submenu(menu, NULL));
	g_assert_true(gowl_menu_is_submenu(menu, "root"));

	rows = gowl_menu_list(menu, NULL, NULL);
	g_assert_cmpuint(rows->len, ==, 0);
}

static void
test_the_shipped_tree_parses(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;

	g_assert_true(gowl_menu_load_file(menu, GOWL_TEST_MENU_FILE, FALSE,
	                                  &error));
	g_assert_no_error(error);

	/* Every route the shipped keybinds and the docs name has to be
	 * there, because a route that stops resolving fails silently --
	 * `gowl menu summon system' simply opens nothing. */
	g_assert_true(gowl_menu_has_route(menu, "system"));
	g_assert_true(gowl_menu_has_route(menu, "apps"));
	g_assert_true(gowl_menu_has_route(menu, "style.backdrop"));
	g_assert_true(gowl_menu_has_route(menu, "system.lock"));

	{
		g_autofree gchar *power = gowl_menu_resolve(menu, "power");

		g_assert_cmpstr(power, ==, "system");
	}

	rows = gowl_menu_list(menu, NULL, NULL);
	g_assert_cmpuint(rows->len, >, 5);
}

static void
test_every_shipped_row_does_something(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	GowlCompositor *comp = bare_compositor();
	GQueue todo = G_QUEUE_INIT;
	guint seen = 0;

	g_assert_true(gowl_menu_load_file(menu, GOWL_TEST_MENU_FILE, FALSE,
	                                  NULL));

	/*
	 * Walk the whole shipped tree and assert that no row is a dead
	 * end: every one either opens something or runs something.  A row
	 * that does neither looks exactly like a working row and does
	 * nothing when pressed -- and it is not visible in the file, since
	 * what is wrong with it is a line that is not there.
	 *
	 * An empty submenu is the same failure one level up, so that is
	 * asserted on the way past.
	 */
	g_queue_push_tail(&todo, g_strdup("root"));
	while (!g_queue_is_empty(&todo)) {
		g_autofree gchar *route = g_queue_pop_head(&todo);
		g_autoptr(GPtrArray) rows = gowl_menu_list(menu, comp, route);
		guint i;

		for (i = 0; i < rows->len; i++) {
			GowlMenuRow *r = g_ptr_array_index(rows, i);

			seen++;
			g_assert_true(r->submenu || r->runnable);
			/*
			 * A submenu that says it has children must list
			 * them.  A submenu that says it has none is
			 * provider-backed, and "no windows are open" is a
			 * true answer rather than an empty menu.
			 */
			if (r->submenu && r->children > 0) {
				g_autoptr(GPtrArray) kids =
					gowl_menu_list(menu, comp, r->route);

				g_assert_cmpuint(kids->len, >=, r->children);
			}
			/* Descend only into rows the FILE declares.  A
			 * provider's rows are made up at open time and have
			 * no children of their own. */
			if (r->submenu && gowl_menu_has_route(menu, r->route))
				g_queue_push_tail(&todo, g_strdup(r->route));
		}
	}
	/* Enough of a tree to be worth walking, so that an empty parse
	 * cannot pass this by having nothing to check. */
	g_assert_cmpuint(seen, >, 40);

	g_queue_clear_full(&todo, g_free);
	g_object_unref(comp);
}

static void
test_a_dead_end_row_says_so(void)
{
	g_autoptr(GowlMenu) menu = gowl_menu_new();
	g_autoptr(GPtrArray) rows = NULL;

	g_assert_true(gowl_menu_load_data(menu,
		"menu:\n"
		"  - {id: a, label: Does nothing}\n"
		"  - {id: b, label: Does something, spawn: \"/bin/true\"}\n",
		FALSE, NULL));

	rows = gowl_menu_list(menu, NULL, NULL);
	g_assert_false(row_named(rows, "Does nothing")->runnable);
	g_assert_false(row_named(rows, "Does nothing")->submenu);
	g_assert_true(row_named(rows, "Does something")->runnable);
}


/* ── Matching ───────────────────────────────────────────────────── */

static gchar *
positions_text(GArray *positions)
{
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < positions->len; i++) {
		if (i > 0)
			g_string_append_c(out, ',');
		g_string_append_printf(out, "%u", g_array_index(positions, guint, i));
	}
	return g_string_free(out, FALSE);
}

/* The tiers, and the letters each one reports. */
static void
test_match_tiers_and_positions(void)
{
	g_autoptr(GArray) pos = g_array_new(FALSE, FALSE, sizeof(guint));
	g_autofree gchar *p1 = NULL;
	g_autofree gchar *p2 = NULL;
	g_autofree gchar *p3 = NULL;
	g_autofree gchar *p4 = NULL;

	g_assert_cmpint(gowl_menu_match("Firefox", "firefox", pos), ==, 0);
	g_assert_cmpint(gowl_menu_match("Firefox", "fire", pos), ==, 10);
	p1 = positions_text(pos);
	g_assert_cmpstr(p1, ==, "0,1,2,3");

	/* The start of a word beats the middle of one: `st' on "Add a
	 * Non-Steam Game" is the S of Steam, not the st of ... nothing
	 * earlier, but on "Restart System" it is the S of System. */
	g_assert_cmpint(gowl_menu_match("Restart System", "sy", pos), ==, 20);
	p2 = positions_text(pos);
	g_assert_cmpstr(p2, ==, "8,9");
	g_assert_cmpint(gowl_menu_match("Restart System", "star", pos), ==, 30);
	p3 = positions_text(pos);
	g_assert_cmpstr(p3, ==, "2,3,4,5");

	/* Letters in order with gaps, preferring word starts. */
	g_assert_cmpint(gowl_menu_match("Add a Non-Steam Game", "nsg", pos), ==, 40);
	p4 = positions_text(pos);
	g_assert_cmpstr(p4, ==, "6,10,16");
	g_assert_cmpint(gowl_menu_match("Firefox", "frx", NULL), ==, 40);

	/* And not at all. */
	g_assert_cmpint(gowl_menu_match("Firefox", "xrf", pos), ==, -1);
	g_assert_cmpuint(pos->len, ==, 0);
	g_assert_cmpint(gowl_menu_match("Firefox", "", NULL), ==, -1);
	g_assert_cmpint(gowl_menu_match(NULL, "f", NULL), ==, -1);

	/* Case does not matter, and a space splits the needle into words
	 * that are each found on their own: `cath' is a prefix (10) and
	 * `ray' a word start (20), so the row is a word-start match. */
	g_assert_cmpint(gowl_menu_match("Cathode ray tube", "CATH ray", pos), ==, 20);
	{
		g_autofree gchar *p5 = positions_text(pos);

		g_assert_cmpstr(p5, ==, "0,1,2,3,8,9,10");
	}
	g_assert_cmpint(gowl_menu_match("Cathode ray tube", "cath", NULL), ==, 10);
	g_assert_cmpint(gowl_menu_match("Cathode ray tube", "ray cath", NULL), ==, 20);
	g_assert_cmpint(gowl_menu_match("Cathode ray tube", "cath zebra", NULL), ==, -1);
}

/* Fuzzy finds what substring search never did, and ranks below it. */
static void
test_search_is_fuzzy_but_ranks_exact_first(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	g_autoptr(GPtrArray) rows = gowl_menu_search(menu, NULL, "crt");
	g_autoptr(GPtrArray) both = NULL;

	/* c-r-t is in "Cathode ray tube" only as initials. */
	g_assert_nonnull(row_named(rows, "Cathode ray tube"));

	/* `lock' is Lock exactly and also the l-o-c-k of nothing else
	 * here; the exact one is first. */
	both = gowl_menu_search(menu, NULL, "lo");
	g_assert_cmpuint(both->len, >, 0);
	g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(both, 0))->label,
	                ==, "Lock");
}

/* ── The calculator ────────────────────────────────────────────── */

static void
test_calc(void)
{
	gdouble v = 0;

	g_assert_true(gowl_menu_calc("2+2", &v));
	g_assert_cmpfloat(v, ==, 4.0);
	g_assert_true(gowl_menu_calc(" 2 * (3 + 4) / 7 ", &v));
	g_assert_cmpfloat(v, ==, 2.0);
	g_assert_true(gowl_menu_calc("-3^2", &v));
	g_assert_cmpfloat(v, ==, -9.0);       /* the power binds tighter */
	g_assert_true(gowl_menu_calc("(-3)^2", &v));
	g_assert_cmpfloat(v, ==, 9.0);
	g_assert_true(gowl_menu_calc("2^3^2", &v));
	g_assert_cmpfloat(v, ==, 512.0);      /* right associative */
	g_assert_true(gowl_menu_calc("10 % 4", &v));
	g_assert_cmpfloat(v, ==, 2.0);
	g_assert_true(gowl_menu_calc("sqrt(16) + abs(-2)", &v));
	g_assert_cmpfloat(v, ==, 6.0);
	g_assert_true(gowl_menu_calc("round(2.5) + floor(1.9) + ceil(1.1)", &v));
	g_assert_cmpfloat(v, ==, 6.0);
	g_assert_true(gowl_menu_calc("2 * pi", &v));
	g_assert_cmpfloat(fabs(v - 2 * G_PI), <, 1e-9);
	g_assert_true(gowl_menu_calc("1.5 x 4", &v));
	g_assert_cmpfloat(v, ==, 6.0);

	g_assert_false(gowl_menu_calc("1/0", &v));
	g_assert_false(gowl_menu_calc("2 +", &v));
	g_assert_false(gowl_menu_calc("(2", &v));
	g_assert_false(gowl_menu_calc("foo(2)", &v));
	g_assert_false(gowl_menu_calc("", &v));
	g_assert_false(gowl_menu_calc("rm -rf /", &v));
}

static void
test_number_formatting(void)
{
	g_autofree gchar *a = gowl_menu_format_number(4.0);
	g_autofree gchar *b = gowl_menu_format_number(2.5);
	g_autofree gchar *c = gowl_menu_format_number(1.0 / 3.0);
	g_autofree gchar *d = gowl_menu_format_number(-1234567.0);

	g_assert_cmpstr(a, ==, "4");
	g_assert_cmpstr(b, ==, "2.5");
	g_assert_cmpstr(c, ==, "0.3333333333");
	g_assert_cmpstr(d, ==, "-1234567");
}

/* ── Rows made from the typed text ─────────────────────────────── */

static void
test_typed_text_makes_rows(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	g_autoptr(GPtrArray) sum = gowl_menu_search(menu, NULL, "=6*7");
	g_autoptr(GPtrArray) bare = gowl_menu_search(menu, NULL, "6*7");
	g_autoptr(GPtrArray) cmd = gowl_menu_search(menu, NULL, "!true");
	g_autoptr(GPtrArray) url = gowl_menu_search(menu, NULL, "example.org");
	g_autoptr(GPtrArray) home = gowl_menu_search(menu, NULL, "~");
	g_autoptr(GPtrArray) word = gowl_menu_search(menu, NULL, "1password");
	const GowlMenuRow *row;

	/* `=' makes the sum the ONLY row; a bare sum leads the results. */
	g_assert_cmpuint(sum->len, ==, 1);
	row = g_ptr_array_index(sum, 0);
	g_assert_cmpstr(row->route, ==, "calc:42");
	g_assert_cmpstr(row->label, ==, "42");
	g_assert_cmpuint(bare->len, >=, 1);
	row = g_ptr_array_index(bare, 0);
	g_assert_cmpstr(row->route, ==, "calc:42");

	g_assert_cmpuint(cmd->len, ==, 1);
	row = g_ptr_array_index(cmd, 0);
	g_assert_cmpstr(row->route, ==, "run:true");

	g_assert_cmpuint(url->len, >=, 1);
	row = g_ptr_array_index(url, 0);
	g_assert_cmpstr(row->route, ==, "open:https://example.org");

	g_assert_cmpuint(home->len, >=, 1);
	row = g_ptr_array_index(home, 0);
	g_assert_true(g_str_has_prefix(row->route, "open:/"));

	/* Digits inside a word are a word, not a sum. */
	g_assert_true(word->len == 0
	              || !g_str_has_prefix(((GowlMenuRow *)g_ptr_array_index(word, 0))->route, "calc:"));
}

/* Choosing the sum copies it and keeps the card; the command runs. */
static void
test_typed_rows_activate(void)
{
	g_autoptr(GowlMenu) menu = sample_menu();
	GowlCompositor *comp = bare_compositor();

	/* No seat on a bare compositor: the copy has nowhere to go, but
	 * the row still answers "ran, stay open" rather than "nothing". */
	g_assert_cmpint(gowl_menu_activate(menu, comp, "calc:42", NULL),
	                ==, GOWL_MENU_RESULT_RAN_OPEN);
	g_assert_cmpint(gowl_menu_activate(menu, comp, "run:true", NULL),
	                ==, GOWL_MENU_RESULT_RAN);
	g_assert_cmpint(gowl_menu_activate(menu, comp, "calc:", NULL),
	                ==, GOWL_MENU_RESULT_NONE);
	g_object_unref(comp);
}

/* ── History ───────────────────────────────────────────────────── */

static void
test_recent_remembers_and_ranks(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("gowl-menu-hist-XXXXXX", NULL);
	g_autofree gchar *file = g_build_filename(dir, "recent.tsv", NULL);
	GowlCompositor *comp = bare_compositor();
	g_autoptr(GowlMenu) menu = NULL;
	g_autoptr(GPtrArray) recent = NULL;
	g_autoptr(GPtrArray) again = NULL;

	g_unsetenv("GOWL_MENU_NO_HISTORY");
	g_setenv("GOWL_MENU_HISTORY", file, TRUE);
	menu = sample_menu();

	g_assert_cmpuint(gowl_menu_recent(menu, comp, 5)->len, ==, 0);

	/* Lock, then Backdrop; a keep-open row is not recorded. */
	gowl_menu_activate(menu, comp, "system.lock", NULL);
	gowl_menu_activate(menu, comp, "style.backdrop", NULL);
	gowl_menu_activate(menu, comp, "system.louder", NULL);
	g_assert_cmpuint(gowl_menu_get_uses(menu, "system.lock"), ==, 1);
	g_assert_cmpuint(gowl_menu_get_uses(menu, "system.louder"), ==, 0);

	recent = gowl_menu_recent(menu, comp, 5);
	g_assert_cmpuint(recent->len, ==, 2);
	/* Most recent first, and it carries its path. */
	g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(recent, 0))->route,
	                ==, "style.backdrop");
	g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(recent, 0))->detail,
	                ==, "Style");

	/* Persisted: a fresh model reads it back. */
	g_assert_true(g_file_test(file, G_FILE_TEST_EXISTS));
	{
		g_autoptr(GowlMenu) reloaded = sample_menu();

		g_assert_cmpuint(gowl_menu_get_uses(reloaded, "system.lock"),
		                 ==, 1);
	}

	/*
	 * Ranking.  `o' is somewhere inside Backdrop, Cathode ray tube,
	 * Lock, Volume up and No Id At All alike -- one tier for all of
	 * them -- so tree order decides and Backdrop, chosen once like
	 * Lock, leads.  Choose Lock twice more and it leads instead: use
	 * reorders equals.  It never promotes across tiers: `backdrop'
	 * still puts the exact Backdrop above the word-start Next
	 * backdrop however often the latter is chosen.
	 */
	{
		g_autoptr(GPtrArray) rows = gowl_menu_search(menu, comp, "o");

		g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(rows, 0))->label,
		                ==, "Backdrop");
	}
	gowl_menu_activate(menu, comp, "system.lock", NULL);
	gowl_menu_activate(menu, comp, "system.lock", NULL);
	{
		g_autoptr(GPtrArray) rows = gowl_menu_search(menu, comp, "o");

		g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(rows, 0))->label,
		                ==, "Lock");
	}
	gowl_menu_activate(menu, comp, "style.next-backdrop", NULL);
	gowl_menu_activate(menu, comp, "style.next-backdrop", NULL);
	gowl_menu_activate(menu, comp, "style.next-backdrop", NULL);
	{
		g_autoptr(GPtrArray) rows = gowl_menu_search(menu, comp, "backdrop");

		g_assert_cmpstr(((GowlMenuRow *)g_ptr_array_index(rows, 0))->label,
		                ==, "Backdrop");
	}

	gowl_menu_forget_history(menu);
	again = gowl_menu_recent(menu, comp, 5);
	g_assert_cmpuint(again->len, ==, 0);
	g_assert_false(g_file_test(file, G_FILE_TEST_EXISTS));

	g_setenv("GOWL_MENU_NO_HISTORY", "1", TRUE);
	g_unsetenv("GOWL_MENU_HISTORY");
	g_rmdir(dir);
	g_object_unref(comp);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_log_set_writer_func(capture_writer, NULL, NULL);
	g_setenv("GOWL_MENU_NO_HISTORY", "1", TRUE);
	apps_fixture_up();

	g_test_add_func("/menu/nesting-makes-the-route",
	                test_nesting_makes_the_route);
	g_test_add_func("/menu/an-entry-without-an-id-gets-one",
	                test_an_entry_without_an_id_gets_one);
	g_test_add_func("/menu/root-lists-only-the-top",
	                test_root_lists_only_the_top);
	g_test_add_func("/menu/a-submenu-row-says-it-is-one",
	                test_a_submenu_row_says_it_is_one);
	g_test_add_func("/menu/the-title-is-the-title",
	                test_the_title_is_the_title_when_there_is_one);

	g_test_add_func("/menu/overlay-changes-only-what-it-names",
	                test_an_overlay_changes_only_what_it_names);
	g_test_add_func("/menu/overlay-adds-at-the-end",
	                test_an_overlay_adds_a_new_row_at_the_end);

	g_test_add_func("/menu/a-failed-when-hides-one-row",
	                test_a_failed_when_hides_one_row);
	g_test_add_func("/menu/an-ipc-guard-reads-the-answer",
	                test_an_ipc_guard_reads_the_answer);
	g_test_add_func("/menu/a-row-with-no-checked-guard-is-not-ticked",
	                test_a_row_with_no_checked_guard_is_not_ticked);
	g_test_add_func("/menu/an-ipc-guard-can-be-inverted",
	                test_an_ipc_guard_can_be_inverted);
	g_test_add_func("/menu/a-module-guard-reads-the-manager",
	                test_a_module_guard_reads_the_manager);
	g_test_add_func("/menu/an-embedder-guard-follows-the-handler",
	                test_an_embedder_guard_follows_the_handler);
	g_test_add_func("/menu/a-disabled-row-is-listed-ticked-and-inert",
	                test_a_disabled_row_is_listed_ticked_and_inert);

	g_test_add_func("/menu/search-finds-provider-rows",
	                test_search_finds_provider_rows);
	g_test_add_func("/menu/a-declared-row-outranks-an-app",
	                test_a_declared_row_outranks_an_app);
	g_test_add_func("/menu/the-app-list-is-cached",
	                test_the_app_list_is_cached);
	g_test_add_func("/menu/only-the-app-list-is-cached",
	                test_only_the_app_list_is_cached);
	g_test_add_func("/menu/an-empty-submenu-disappears",
	                test_an_empty_submenu_disappears);
	g_test_add_func("/menu/routes-resolve", test_routes_resolve);
	g_test_add_func("/menu/an-exact-route-beats-an-alias",
	                test_an_exact_route_beats_an_alias);

	g_test_add_func("/menu/search-is-global", test_search_is_global);
	g_test_add_func("/menu/search-carries-the-path",
	                test_search_carries_the_path);
	g_test_add_func("/menu/search-puts-the-best-answer-first",
	                test_search_puts_the_best_answer_first);
	g_test_add_func("/menu/search-matches-the-description",
	                test_search_matches_the_description);
	g_test_add_func("/menu/search-skips-a-hidden-row",
	                test_search_skips_a_hidden_row);

	g_test_add_func("/menu/activating-a-submenu-opens-it",
	                test_activating_a_submenu_opens_it);
	g_test_add_func("/menu/a-link-opens-its-target",
	                test_a_link_opens_its_target);
	g_test_add_func("/menu/a-link-lists-what-it-points-at",
	                test_a_link_lists_what_it_points_at);
	g_test_add_func("/menu/an-action-runs-and-closes",
	                test_an_action_runs_and_closes);
	g_test_add_func("/menu/keep-open-leaves-the-menu-up",
	                test_keep_open_leaves_the_menu_up);
	g_test_add_func("/menu/match-tiers-and-positions",
	                test_match_tiers_and_positions);
	g_test_add_func("/menu/search-is-fuzzy-but-ranks-exact-first",
	                test_search_is_fuzzy_but_ranks_exact_first);
	g_test_add_func("/menu/calc", test_calc);
	g_test_add_func("/menu/number-formatting", test_number_formatting);
	g_test_add_func("/menu/typed-text-makes-rows",
	                test_typed_text_makes_rows);
	g_test_add_func("/menu/typed-rows-activate", test_typed_rows_activate);
	g_test_add_func("/menu/recent-remembers-and-ranks",
	                test_recent_remembers_and_ranks);
	g_test_add_func("/menu/an-unknown-route-does-nothing",
	                test_an_unknown_route_does_nothing);

	g_test_add_func("/menu/a-provider-makes-rows", test_a_provider_makes_rows);
	g_test_add_func("/menu/the-apps-provider-reads-desktop-entries",
	                test_the_apps_provider_reads_desktop_entries);
	g_test_add_func("/menu/a-provider-submenu-says-it-is-one",
	                test_a_provider_submenu_says_it_is_one);
	g_test_add_func("/menu/an-unknown-provider-costs-only-itself",
	                test_an_unknown_provider_costs_only_itself);

	g_test_add_func("/menu/the-tree-is-found-from-anywhere",
	                test_the_tree_is_found_from_anywhere);
	g_test_add_func("/menu/an-empty-tree-still-opens",
	                test_an_empty_tree_still_opens);
	g_test_add_func("/menu/the-shipped-tree-parses",
	                test_the_shipped_tree_parses);
	g_test_add_func("/menu/every-shipped-row-does-something",
	                test_every_shipped_row_does_something);
	g_test_add_func("/menu/a-dead-end-row-says-so",
	                test_a_dead_end_row_says_so);

	{
		gint status = g_test_run();

		apps_fixture_down();
		return status;
	}
}
