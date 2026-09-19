/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * The menu module, against the real .so.
 *
 * It runs against a compositor that was never started: no display, no
 * scene, no seat.  That covers everything the module DECIDES -- what each
 * command answers, where the keyboard cursor goes, what typing does to
 * the list, what closes it -- and leaves only the pixels, which are the
 * bar's panel renderer and are covered where that lives.
 *
 * The properties here are the ones a screenshot would not show:
 *
 *   THE SAME KEY CLOSES IT.  A toggle that opens at a route has to close
 *   when asked for the same route and MOVE when asked for another, or
 *   one key per route means every second press is a close.
 *
 *   ESCAPE BACKS OUT ONE LAYER.  Filter first, menu second.  Escape that
 *   closes while a search is half typed loses the search and the menu
 *   for one keystroke.
 *
 *   BACKSPACE ON AN EMPTY FILTER GOES UP, so the key is reversible all
 *   the way back out.
 *
 *   THE CURSOR SKIPS WHAT CANNOT BE CHOSEN.  Landing on a disabled row
 *   means the first Return does nothing, which reads as a broken menu.
 *
 *   IT SWALLOWS EVERYTHING WHILE OPEN.  A key reaching the window
 *   underneath while a modal list is up types into that window.
 */

#include <gmodule.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "core/gowl-core-private.h"
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "menu/gowl-menu.h"
#include "module/gowl-module.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-keybind-handler.h"
#include "interfaces/gowl-startup-handler.h"

#ifndef GOWL_TEST_MENU_MODULE
#define GOWL_TEST_MENU_MODULE "build/release/modules/menu.so"
#endif

#define NEED_MODULE(f) \
	G_STMT_START { if ((f)->module == NULL) return; } G_STMT_END

typedef struct {
	GowlModule     *module;
	GowlCompositor *comp;
	GowlMonitor    *mon;
} Fixture;

static GType
menu_type(void)
{
	static GType type = 0;
	GModule *module;
	gpointer symbol;
	GType (*register_fn)(void);

	if (type != 0)
		return type;
	module = g_module_open(GOWL_TEST_MENU_MODULE, G_MODULE_BIND_LAZY);
	if (module == NULL)
		return 0;
	g_module_make_resident(module);
	if (!g_module_symbol(module, "gowl_module_register", &symbol))
		return 0;
	register_fn = (GType (*)(void))symbol;
	type = register_fn();
	return type;
}

/* A tree with the specific shapes the module has to get right. */
static const gchar *const TREE =
	"menu:\n"
	"  - id: style\n"
	"    label: Style\n"
	"    items:\n"
	"      - {id: one, label: One, spawn: \"/bin/true\"}\n"
	"      - {id: two, label: Two, spawn: \"/bin/true\"}\n"
	"      - {id: loud, label: Louder, spawn: \"/bin/true\", keep-open: true}\n"
	"  - id: system\n"
	"    label: System\n"
	"    items:\n"
	"      - {id: gone, label: Cannot pick me, spawn: \"/bin/true\",\n"
	"         disabled: {ipc: \"ping\", is: \"pong\"}}\n"
	"      - {id: lock, label: Lock, spawn: \"/bin/true\"}\n";

static void
fixture_setup(Fixture *f, gconstpointer data)
{
	GType type;

	(void)data;
	memset(f, 0, sizeof(*f));
	type = menu_type();
	if (type == 0) {
		g_test_skip("menu.so did not load");
		return;
	}

	f->comp = gowl_compositor_new();
	f->mon = g_object_new(GOWL_TYPE_MONITOR, NULL);
	f->mon->w.width = 1920;
	f->mon->w.height = 1080;
	f->comp->selmon = f->mon;
	f->comp->monitors = g_list_append(NULL, f->mon);

	/* The module takes the SHARED tree, so the fixture loads into that
	 * one rather than handing the module a second copy -- which is
	 * also the arrangement cmacs relies on. */
	g_assert_true(gowl_menu_load_data(gowl_menu_get_default(), TREE,
	                                  FALSE, NULL));

	f->module = (GowlModule *)g_object_new(type, NULL);
	g_assert_true(gowl_module_activate(f->module));
	gowl_startup_handler_on_startup(GOWL_STARTUP_HANDLER(f->module),
	                                f->comp);
}

static void
fixture_teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	if (f->comp == NULL)
		return;
	g_clear_object(&f->module);
	g_clear_pointer(&f->comp->monitors, g_list_free);
	f->comp->selmon = NULL;
	g_object_unref(f->comp);
	g_object_unref(f->mon);
}

static gchar *
run(Fixture *f, const gchar *command, const gchar *args)
{
	return gowl_ipc_handler_handle_command(GOWL_IPC_HANDLER(f->module),
	                                       command, args);
}

static gboolean
key(Fixture *f, guint keysym)
{
	return gowl_keybind_handler_handle_key(GOWL_KEYBIND_HANDLER(f->module),
	                                       0, keysym, TRUE);
}

static gboolean
ctrl_key(Fixture *f, guint keysym)
{
	return gowl_keybind_handler_handle_key(GOWL_KEYBIND_HANDLER(f->module),
	                                       WLR_MODIFIER_CTRL, keysym, TRUE);
}

static gboolean
is_open(Fixture *f)
{
	g_autofree gchar *reply = run(f, "menu", NULL);
	gboolean open = g_strcmp0(reply, "OK open") == 0;

	/* Asking cost a toggle, so put it back. */
	g_free(run(f, open ? "menu-close" : "menu-open", NULL));
	return !open;
}

/* ── Commands ───────────────────────────────────────────────────── */

static void
test_menu_toggles(Fixture *f, gconstpointer data)
{
	NEED_MODULE(f);

	{
		g_autofree gchar *first = run(f, "menu", NULL);

		g_assert_cmpstr(first, ==, "OK open");
	}
	{
		g_autofree gchar *second = run(f, "menu", NULL);

		g_assert_cmpstr(second, ==, "OK closed");
	}
}

static void
test_toggling_to_another_route_moves(Fixture *f, gconstpointer data)
{
	NEED_MODULE(f);

	g_free(run(f, "menu", "style"));
	{
		/* Same route: close.  One key per route, and pressing the
		 * one you are already in is how you leave. */
		g_autofree gchar *same = run(f, "menu", "style");

		g_assert_cmpstr(same, ==, "OK closed");
	}
	g_free(run(f, "menu", "style"));
	{
		/* Different route: move there rather than close, or the
		 * second of two route keys never opens. */
		g_autofree gchar *other = run(f, "menu", "system");

		g_assert_cmpstr(other, ==, "OK open");
	}
	g_free(run(f, "menu-close", NULL));
}

static void
test_open_never_closes(Fixture *f, gconstpointer data)
{
	NEED_MODULE(f);

	g_free(run(f, "menu-open", "style"));
	{
		g_autofree gchar *again = run(f, "menu-open", "style");

		g_assert_cmpstr(again, ==, "OK open");
	}
	g_free(run(f, "menu-close", NULL));
}

static void
test_list_answers_without_opening(Fixture *f, gconstpointer data)
{
	g_autofree gchar *reply = NULL;

	NEED_MODULE(f);

	reply = run(f, "menu-list", "style");
	g_assert_nonnull(reply);
	g_assert_nonnull(strstr(reply, "style.one\tOne\taction"));
	/* A query must not have opened anything. */
	g_assert_true(is_open(f) == FALSE);
}

static void
test_list_marks_a_submenu(Fixture *f, gconstpointer data)
{
	g_autofree gchar *reply = NULL;

	NEED_MODULE(f);

	reply = run(f, "menu-list", NULL);
	g_assert_nonnull(strstr(reply, "style\tStyle\tmenu"));
}

static void
test_an_unknown_command_is_not_ours(Fixture *f, gconstpointer data)
{
	g_autofree gchar *reply = NULL;

	NEED_MODULE(f);

	/* NULL, not an error: the compositor offers every command to every
	 * module, and a module that answered them all would shadow the
	 * module the command belongs to. */
	reply = run(f, "expo", NULL);
	g_assert_null(reply);
}

/* ── Keyboard ───────────────────────────────────────────────────── */

static void
test_keys_are_ignored_while_closed(Fixture *f, gconstpointer data)
{
	NEED_MODULE(f);

	/* Not open: every key belongs to somebody else. */
	g_assert_false(key(f, XKB_KEY_Escape));
	g_assert_false(key(f, XKB_KEY_a));
}

static void
test_everything_is_swallowed_while_open(Fixture *f, gconstpointer data)
{
	NEED_MODULE(f);

	g_free(run(f, "menu-open", NULL));
	/* Including keys the menu does nothing with: a modal list that
	 * lets F5 through is typing into the window underneath. */
	g_assert_true(key(f, XKB_KEY_F5));
	g_assert_true(gowl_keybind_handler_handle_key(
		GOWL_KEYBIND_HANDLER(f->module), 0, XKB_KEY_a, FALSE));
	g_free(run(f, "menu-close", NULL));
}

static void
test_escape_clears_the_filter_then_closes(Fixture *f, gconstpointer data)
{
	NEED_MODULE(f);

	g_free(run(f, "menu-open", NULL));
	g_assert_true(key(f, XKB_KEY_l));
	/* Still open with a filter typed. */
	g_assert_true(key(f, XKB_KEY_Escape));
	g_assert_true(is_open(f));
	/* Now the filter is gone, so this one closes. */
	g_free(run(f, "menu-open", NULL));
	g_assert_true(key(f, XKB_KEY_Escape));
	g_assert_false(is_open(f));
}

static void
test_backspace_goes_up_on_an_empty_filter(Fixture *f, gconstpointer data)
{
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	NEED_MODULE(f);

	g_free(run(f, "menu-open", "style"));
	/* Opening style then backspacing lands on the root, which is
	 * visible in what the next toggle closes. */
	g_assert_true(key(f, XKB_KEY_BackSpace));
	before = run(f, "menu", "style");
	/* At the root, asking for style MOVES rather than closes. */
	g_assert_cmpstr(before, ==, "OK open");
	g_free(run(f, "menu-close", NULL));

	/*
	 * And at the root, Backspace does NOTHING: the card stays.  It
	 * used to close, so holding Backspace to clear a search ran past
	 * the empty box and shut the menu -- the one motion a search box
	 * invites, ending in the one outcome nobody meant.  The toggle
	 * that follows therefore CLOSES, which is how "still open" reads
	 * through this interface.
	 */
	g_free(run(f, "menu-open", NULL));
	g_assert_true(key(f, XKB_KEY_BackSpace));
	g_assert_true(key(f, XKB_KEY_BackSpace));
	after = run(f, "menu", NULL);
	g_assert_cmpstr(after, ==, "OK closed");
}

/* Type, then hold Backspace past the end of it: the search empties,
 * the root comes back, and the card is still up. */
static void
test_backspacing_through_a_search_stays_open(Fixture *f, gconstpointer data)
{
	g_autofree gchar *reply = NULL;
	gint i;

	NEED_MODULE(f);

	g_free(run(f, "menu-open", NULL));
	g_assert_true(key(f, XKB_KEY_l));
	g_assert_true(key(f, XKB_KEY_o));
	for (i = 0; i < 6; i++)
		g_assert_true(key(f, XKB_KEY_BackSpace));
	g_assert_true(is_open(f));

	/* Escape is the way out, and only from an empty box. */
	g_assert_true(key(f, XKB_KEY_x));
	g_assert_true(key(f, XKB_KEY_Escape));
	g_assert_true(is_open(f));
	g_assert_true(key(f, XKB_KEY_Escape));
	reply = run(f, "menu", NULL);
	g_assert_cmpstr(reply, ==, "OK open");
	g_free(run(f, "menu-close", NULL));
}

/* Alt+N chooses the Nth row without moving the cursor to it first. */
static void
test_alt_number_picks_a_row(Fixture *f, gconstpointer data)
{
	g_autofree gchar *reply = NULL;

	NEED_MODULE(f);

	/* The root: style, system.  Alt+2 enters System. */
	g_free(run(f, "menu-open", NULL));
	g_assert_true(gowl_keybind_handler_handle_key(
		GOWL_KEYBIND_HANDLER(f->module), WLR_MODIFIER_ALT, XKB_KEY_2,
		TRUE));
	reply = run(f, "menu", "system");
	g_assert_cmpstr(reply, ==, "OK closed");
}

/* Ctrl+u clears the search and Ctrl+g closes, as at a prompt. */
static void
test_prompt_keys(Fixture *f, gconstpointer data)
{
	NEED_MODULE(f);

	g_free(run(f, "menu-open", NULL));
	g_assert_true(key(f, XKB_KEY_l));
	g_assert_true(key(f, XKB_KEY_o));
	g_assert_true(ctrl_key(f, XKB_KEY_u));
	/* Cleared: the first Escape now closes, where with text it would
	 * only have emptied the box. */
	g_assert_true(key(f, XKB_KEY_Escape));
	g_assert_false(is_open(f));

	g_free(run(f, "menu-open", NULL));
	g_assert_true(ctrl_key(f, XKB_KEY_g));
	g_assert_false(is_open(f));
}

/* A sum typed into the box is answered by a row of its own. */
static void
test_a_sum_is_answered(Fixture *f, gconstpointer data)
{
	g_autofree gchar *listing = NULL;

	NEED_MODULE(f);

	g_free(run(f, "menu-open", NULL));
	g_assert_true(key(f, XKB_KEY_equal));
	g_assert_true(key(f, XKB_KEY_6));
	g_assert_true(key(f, XKB_KEY_asterisk));
	g_assert_true(key(f, XKB_KEY_7));
	/* Return on the answer copies it and keeps the card up. */
	g_assert_true(key(f, XKB_KEY_Return));
	g_assert_true(is_open(f));
	g_free(run(f, "menu-close", NULL));
}

static void
test_return_on_a_submenu_enters_it(Fixture *f, gconstpointer data)
{
	g_autofree gchar *reply = NULL;

	NEED_MODULE(f);

	g_free(run(f, "menu-open", NULL));
	/* The cursor starts on the first row, which is Style. */
	g_assert_true(key(f, XKB_KEY_Return));
	/* Now inside it: asking to toggle style closes rather than moves. */
	reply = run(f, "menu", "style");
	g_assert_cmpstr(reply, ==, "OK closed");
}

static void
test_return_on_an_action_closes(Fixture *f, gconstpointer data)
{
	NEED_MODULE(f);

	g_free(run(f, "menu-open", "style"));
	g_assert_true(key(f, XKB_KEY_Return));
	g_assert_false(is_open(f));
}

static void
test_keep_open_leaves_it_up(Fixture *f, gconstpointer data)
{
	NEED_MODULE(f);

	g_free(run(f, "menu-open", "style"));
	/* One, Two, Louder: down twice to reach the row marked
	 * keep-open, which must not close the card. */
	g_assert_true(key(f, XKB_KEY_Down));
	g_assert_true(key(f, XKB_KEY_Down));
	g_assert_true(key(f, XKB_KEY_Return));
	g_assert_true(is_open(f));
	g_free(run(f, "menu-close", NULL));
}

static void
test_the_cursor_skips_a_disabled_row(Fixture *f, gconstpointer data)
{
	NEED_MODULE(f);

	/* `Cannot pick me' is first under System and is disabled, so the
	 * cursor has to start on Lock -- and Return then closes, which is
	 * how a run is told from a press that did nothing. */
	g_free(run(f, "menu-open", "system"));
	g_assert_true(key(f, XKB_KEY_Return));
	g_assert_false(is_open(f));
}

static void
test_typing_filters(Fixture *f, gconstpointer data)
{
	NEED_MODULE(f);

	g_free(run(f, "menu-open", NULL));
	/* `lock' finds the row two levels down, and Return runs it --
	 * which is the whole type-and-press-enter motion. */
	g_assert_true(key(f, XKB_KEY_l));
	g_assert_true(key(f, XKB_KEY_o));
	g_assert_true(key(f, XKB_KEY_c));
	g_assert_true(key(f, XKB_KEY_k));
	g_assert_true(key(f, XKB_KEY_Return));
	g_assert_false(is_open(f));
}

static void
test_a_modifier_does_not_type(Fixture *f, gconstpointer data)
{
	NEED_MODULE(f);

	g_free(run(f, "menu-open", NULL));
	/* Ctrl+C is a shortcut somewhere; a search box that ate it would
	 * be eating keys people press on purpose.  Swallowed, not typed:
	 * Escape then closes rather than clearing a filter. */
	g_assert_true(gowl_keybind_handler_handle_key(
		GOWL_KEYBIND_HANDLER(f->module), WLR_MODIFIER_CTRL,
		XKB_KEY_c, TRUE));
	g_assert_true(key(f, XKB_KEY_Escape));
	g_assert_false(is_open(f));
}

/* ── Reload ─────────────────────────────────────────────────────── */

static void
test_refresh_reports_what_it_read(Fixture *f, gconstpointer data)
{
	g_autofree gchar *reply = NULL;

	NEED_MODULE(f);

	reply = run(f, "menu-refresh", NULL);
	g_assert_nonnull(reply);
	/* Either it found a menu.yaml or it says why not; both are one
	 * line, and neither may be silence. */
	g_assert_true(g_str_has_prefix(reply, "OK ")
	              || g_str_has_prefix(reply, "ERROR "));
}

static void
test_an_empty_tree_opens_onto_an_empty_card(Fixture *f, gconstpointer data)
{
	NEED_MODULE(f);

	/*
	 * The failure this exists for: menu.yaml was not where it was
	 * looked for, so the tree is empty -- and the key then does
	 * NOTHING, which is indistinguishable from the key not being
	 * bound.  An empty card at least says the menu is there and has
	 * nothing in it, which is a thing somebody can act on.
	 */
	g_assert_true(gowl_menu_load_data(gowl_menu_get_default(),
	                                  "menu: []\n", FALSE, NULL));

	{
		g_autofree gchar *reply = run(f, "menu", NULL);

		g_assert_cmpstr(reply, ==, "OK open");
	}
	g_assert_true(is_open(f));
	/* And Escape still gets out of it. */
	g_assert_true(key(f, XKB_KEY_Escape));
	g_assert_false(is_open(f));
}

/* ── Ctrl+hjkl ──────────────────────────────────────────────────── */

/*
 * The four that navigate.
 *
 * Plain h/j/k/l cannot be it: every letter goes into the search, which
 * is the whole point of the card.  So these are the modified ones, and
 * left/right move ACROSS LEVELS -- the thing being navigated is a tree,
 * and there is nothing else those two directions could mean here.
 */

static void
test_ctrl_j_and_k_move_the_cursor(Fixture *f, gconstpointer data)
{
	NEED_MODULE(f);

	/*
	 * Under Style: One, Two, Louder.  Only the third leaves the card
	 * up when chosen (`keep-open'), so where the cursor ended up is
	 * read from whether the menu is still there afterwards.
	 *
	 * Each direction gets its own open: is_open() is a toggle, and
	 * asking it mid-sequence reopens the menu at the ROOT, which would
	 * silently move the cursor somewhere else before the next step.
	 */
	g_free(run(f, "menu-open", "style"));
	g_assert_true(ctrl_key(f, XKB_KEY_j));
	g_assert_true(ctrl_key(f, XKB_KEY_j));
	g_assert_true(key(f, XKB_KEY_Return));
	g_assert_true(is_open(f));
	g_free(run(f, "menu-close", NULL));

	/* Down three and back up one is Two, an ordinary row: it closes. */
	g_free(run(f, "menu-open", "style"));
	g_assert_true(ctrl_key(f, XKB_KEY_j));
	g_assert_true(ctrl_key(f, XKB_KEY_j));
	g_assert_true(ctrl_key(f, XKB_KEY_k));
	g_assert_true(key(f, XKB_KEY_Return));
	g_assert_false(is_open(f));
}

static void
test_ctrl_l_goes_into_a_submenu(Fixture *f, gconstpointer data)
{
	g_autofree gchar *reply = NULL;

	NEED_MODULE(f);

	g_free(run(f, "menu-open", NULL));
	/* The cursor starts on Style, which is a submenu. */
	g_assert_true(ctrl_key(f, XKB_KEY_l));
	/* Inside it now: toggling to style closes rather than moves. */
	reply = run(f, "menu", "style");
	g_assert_cmpstr(reply, ==, "OK closed");
}

static void
test_ctrl_h_comes_back_out(Fixture *f, gconstpointer data)
{
	g_autofree gchar *reply = NULL;

	NEED_MODULE(f);

	g_free(run(f, "menu-open", "style"));
	g_assert_true(ctrl_key(f, XKB_KEY_h));
	/* At the root now, so asking for style MOVES rather than closes. */
	reply = run(f, "menu", "style");
	g_assert_cmpstr(reply, ==, "OK open");
	g_free(run(f, "menu-close", NULL));
}

static void
test_ctrl_h_clears_the_search_first(Fixture *f, gconstpointer data)
{
	NEED_MODULE(f);

	/*
	 * The same layering Escape uses.  Going up a level while a search
	 * is half typed would lose both the search and the place, for one
	 * keystroke.
	 */
	g_free(run(f, "menu-open", "style"));
	g_assert_true(key(f, XKB_KEY_o));      /* filters */
	g_assert_true(ctrl_key(f, XKB_KEY_h)); /* clears the filter */
	{
		/* Still inside style: toggling to it closes. */
		g_autofree gchar *reply = run(f, "menu", "style");

		g_assert_cmpstr(reply, ==, "OK closed");
	}
}

static void
test_other_control_keys_are_still_swallowed(Fixture *f, gconstpointer data)
{
	g_autofree gchar *where = NULL;

	NEED_MODULE(f);

	/*
	 * Ctrl+c is a shortcut somewhere and must not reach the window
	 * underneath -- but it must also not DO anything here.  Asserting
	 * only that it was swallowed cannot tell the two apart: a Ctrl+c
	 * that navigated would also be swallowed, and would also leave the
	 * card closable.  So the check is where the menu still is.
	 */
	g_free(run(f, "menu-open", NULL));
	g_assert_true(ctrl_key(f, XKB_KEY_c));
	g_assert_true(ctrl_key(f, XKB_KEY_x));

	/* Still at the root: asking for style MOVES rather than closes. */
	where = run(f, "menu", "style");
	g_assert_cmpstr(where, ==, "OK open");
	g_free(run(f, "menu-close", NULL));
}

static void
test_super_hjkl_is_not_ours(Fixture *f, gconstpointer data)
{
	NEED_MODULE(f);

	/*
	 * Only Ctrl.  Super+j is the session's own focus bind and reaches
	 * this module only because nothing above it claimed the key;
	 * answering it here would mean the card quietly does something
	 * else with a key that means "focus the next window" everywhere.
	 *
	 * Read the same way as the Ctrl test: under Style only the third
	 * row leaves the card up, so two ignored moves means Return lands
	 * on the first row and closes.
	 */
	g_free(run(f, "menu-open", "style"));
	g_assert_true(gowl_keybind_handler_handle_key(
		GOWL_KEYBIND_HANDLER(f->module), WLR_MODIFIER_LOGO,
		XKB_KEY_j, TRUE));
	g_assert_true(gowl_keybind_handler_handle_key(
		GOWL_KEYBIND_HANDLER(f->module), WLR_MODIFIER_LOGO,
		XKB_KEY_j, TRUE));
	g_assert_true(key(f, XKB_KEY_Return));
	g_assert_false(is_open(f));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_setenv("GOWL_MENU_NO_HISTORY", "1", TRUE);

#define ADD(path, fn) \
	g_test_add(path, Fixture, NULL, fixture_setup, fn, fixture_teardown)

	ADD("/menu-module/toggles", test_menu_toggles);
	ADD("/menu-module/toggling-to-another-route-moves",
	    test_toggling_to_another_route_moves);
	ADD("/menu-module/open-never-closes", test_open_never_closes);
	ADD("/menu-module/list-answers-without-opening",
	    test_list_answers_without_opening);
	ADD("/menu-module/list-marks-a-submenu", test_list_marks_a_submenu);
	ADD("/menu-module/an-unknown-command-is-not-ours",
	    test_an_unknown_command_is_not_ours);

	ADD("/menu-module/keys-are-ignored-while-closed",
	    test_keys_are_ignored_while_closed);
	ADD("/menu-module/everything-is-swallowed-while-open",
	    test_everything_is_swallowed_while_open);
	ADD("/menu-module/escape-clears-the-filter-then-closes",
	    test_escape_clears_the_filter_then_closes);
	ADD("/menu-module/backspace-goes-up-on-an-empty-filter",
	    test_backspace_goes_up_on_an_empty_filter);
	ADD("/menu-module/return-on-a-submenu-enters-it",
	    test_return_on_a_submenu_enters_it);
	ADD("/menu-module/return-on-an-action-closes",
	    test_return_on_an_action_closes);
	ADD("/menu-module/keep-open-leaves-it-up", test_keep_open_leaves_it_up);
	ADD("/menu-module/the-cursor-skips-a-disabled-row",
	    test_the_cursor_skips_a_disabled_row);
	ADD("/menu-module/typing-filters", test_typing_filters);
	ADD("/menu-module/backspacing-through-a-search-stays-open",
	    test_backspacing_through_a_search_stays_open);
	ADD("/menu-module/alt-number-picks-a-row", test_alt_number_picks_a_row);
	ADD("/menu-module/prompt-keys", test_prompt_keys);
	ADD("/menu-module/a-sum-is-answered", test_a_sum_is_answered);
	ADD("/menu-module/a-modifier-does-not-type", test_a_modifier_does_not_type);

	ADD("/menu-module/ctrl-j-and-k-move-the-cursor",
	    test_ctrl_j_and_k_move_the_cursor);
	ADD("/menu-module/ctrl-l-goes-into-a-submenu",
	    test_ctrl_l_goes_into_a_submenu);
	ADD("/menu-module/ctrl-h-comes-back-out", test_ctrl_h_comes_back_out);
	ADD("/menu-module/ctrl-h-clears-the-search-first",
	    test_ctrl_h_clears_the_search_first);
	ADD("/menu-module/other-control-keys-are-still-swallowed",
	    test_other_control_keys_are_still_swallowed);
	ADD("/menu-module/super-hjkl-is-not-ours", test_super_hjkl_is_not_ours);
	ADD("/menu-module/an-empty-tree-opens-onto-an-empty-card",
	    test_an_empty_tree_opens_onto_an_empty_card);
	ADD("/menu-module/refresh-reports-what-it-read",
	    test_refresh_reports_what_it_read);

#undef ADD

	return g_test_run();
}
