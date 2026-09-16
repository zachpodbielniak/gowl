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

#ifndef GOWL_MENU_H
#define GOWL_MENU_H

#include <glib-object.h>

#include "gowl-types.h"

G_BEGIN_DECLS

/*
 * The menu: one tree of everything the session can be told to do.
 *
 * A tiling compositor gives you keys and nothing else.  That is right
 * for the twenty things you do every hour and wrong for the two hundred
 * you do every month -- picking a wifi network, changing the backdrop,
 * finding the layout whose name you have forgotten, turning the tube on.
 * Those need a LIST, because the point of a list is that you do not have
 * to have remembered anything to use it.
 *
 * Omarchy's menu is the shape worth copying: a tree of rows, defined as
 * data rather than code, opened with one key, driven by typing.  What is
 * not worth copying is where the work happens.  Omarchy forks a bash
 * process per open to answer its guards and shells out for every action.
 * Here a guard is an IPC query answered in-process and an action is a
 * keybind entry run through the same dispatcher a key press uses, so the
 * menu is exactly as capable as the keyboard is and costs a fork only
 * when the thing being run is itself a program.
 *
 * WHAT IS AND IS NOT IN HERE.  This file is the MODEL: the tree, where
 * it is read from, which rows are visible right now, and what happens
 * when one is chosen.  It draws nothing.  The card on screen is
 * modules/menu, and cmacs renders the same tree through completing-read
 * -- two front ends over one model, which is the only way the two can
 * agree about what the menu contains.
 *
 * THREADING.  Compositor thread throughout.  Guards and activation both
 * reach into the compositor, so there is nowhere else they could run.
 */

/**
 * GowlMenuGuardKind:
 * @GOWL_MENU_GUARD_NONE: no guard; the row is always shown.
 * @GOWL_MENU_GUARD_CONST: a literal `true' or `false'.
 * @GOWL_MENU_GUARD_EXISTS: succeeds when a program is on `$PATH'.
 * @GOWL_MENU_GUARD_FILE: succeeds when a path exists.
 * @GOWL_MENU_GUARD_MODULE: succeeds when a module is loaded.
 * @GOWL_MENU_GUARD_EMBEDDER: succeeds when something has registered a
 *   custom-action handler --- which is to say, when the compositor is
 *   embedded in cmacs and an `elisp:' row would actually run.
 * @GOWL_MENU_GUARD_IPC: runs an IPC query and reads the answer.
 *
 * What a `when:', `checked:' or `disabled:' guard asks.
 *
 * Deliberately NOT a shell command.  Every one of these is answered in
 * this process in microseconds, which is what lets the menu evaluate
 * every guard in the tree on the open path and still open in one frame;
 * a vocabulary of forks would have to be evaluated in a batch behind the
 * menu and would spend its first moments contradicting itself.  The
 * escape hatch for anything outside the vocabulary is an IPC query,
 * which is also how the vocabulary grows: every command the compositor
 * or a module answers is already a guard.
 */
typedef enum {
	GOWL_MENU_GUARD_NONE = 0,
	GOWL_MENU_GUARD_CONST,
	GOWL_MENU_GUARD_EXISTS,
	GOWL_MENU_GUARD_FILE,
	GOWL_MENU_GUARD_MODULE,
	GOWL_MENU_GUARD_EMBEDDER,
	GOWL_MENU_GUARD_IPC
} GowlMenuGuardKind;

/**
 * GowlMenuRow:
 * @route: the dotted route that names this row, e.g. `style.backdrop'
 * @icon: (nullable): the glyph in the icon column
 * @icon_name: (nullable): a THEMED ICON NAME for the same column, when
 *   the row has a real icon rather than a glyph --- an application's,
 *   from its desktop entry.  The model does not load it: turning a name
 *   into pixels needs an icon theme and a renderer, which is a front
 *   end's business, exactly as it is for the tray
 * @label: what the row says
 * @detail: (nullable): the second line --- a description, or the path
 *   through the tree when the row came out of a search
 * @value: (nullable): a right-hand reading, such as a provider row's
 *   current setting
 * @submenu: %TRUE when choosing this row opens another list
 * @runnable: %TRUE when choosing it runs something.  A row that is
 *   neither a submenu nor runnable is a dead end --- it looks exactly
 *   like a working row and does nothing when pressed, which is the one
 *   mistake in a menu file that reading it will not show you
 * @checked: %TRUE when this row is the current choice
 * @disabled: %TRUE when the row is listed but cannot be chosen
 * @children: how many rows are under a submenu
 *
 * One row, as a front end needs it.  Everything a renderer has to know
 * and nothing it does not: guards are already evaluated, providers are
 * already run, and the route is enough to act on.
 */
typedef struct {
	gchar    *route;
	gchar    *icon;
	gchar    *icon_name;
	gchar    *label;
	gchar    *detail;
	gchar    *value;
	gboolean  submenu;
	gboolean  runnable;
	gboolean  checked;
	gboolean  disabled;
	guint     children;
} GowlMenuRow;

#define GOWL_TYPE_MENU_ROW (gowl_menu_row_get_type())

GType gowl_menu_row_get_type (void) G_GNUC_CONST;

GowlMenuRow *gowl_menu_row_copy (const GowlMenuRow *self);
void         gowl_menu_row_free (GowlMenuRow *self);

/**
 * GowlMenuResult:
 * @GOWL_MENU_RESULT_NONE: the route named nothing that could be acted on.
 * @GOWL_MENU_RESULT_OPEN: a submenu; the out route is what to open.
 * @GOWL_MENU_RESULT_RAN: something ran and the menu should close.
 * @GOWL_MENU_RESULT_RAN_OPEN: something ran and the menu should stay up,
 *   which is what `keep-open: true' is for --- a volume row is pressed
 *   several times in a row and closing after the first is a bug.
 *
 * What gowl_menu_activate() did.
 */
typedef enum {
	GOWL_MENU_RESULT_NONE = 0,
	GOWL_MENU_RESULT_OPEN,
	GOWL_MENU_RESULT_RAN,
	GOWL_MENU_RESULT_RAN_OPEN
} GowlMenuResult;

#define GOWL_TYPE_MENU (gowl_menu_get_type())

G_DECLARE_FINAL_TYPE(GowlMenu, gowl_menu, GOWL, MENU, GObject)

/**
 * gowl_menu_new:
 *
 * Creates an empty menu.  Nothing is read from disk until
 * gowl_menu_load() is called, so a test can build a tree from a string
 * and never touch the user's files.
 *
 * Returns: (transfer full): a new #GowlMenu
 */
GowlMenu *gowl_menu_new (void);

/**
 * gowl_menu_get_default:
 *
 * The session's menu, created and loaded on first use.
 *
 * There is one tree per session because there is one MENU per session:
 * the card on screen, cmacs's completing-read front end and `gowl menu'
 * on the command line are three ways of looking at the same list, and a
 * second copy of it is a second copy that can disagree.
 *
 * Returns: (transfer none): the shared #GowlMenu
 */
GowlMenu *gowl_menu_get_default (void);

/**
 * gowl_menu_load:
 * @self: a #GowlMenu
 * @error: (nullable): return location for a #GError
 *
 * Reads the shipped tree and overlays the user's on top of it.
 *
 * The shipped file is `menu.yaml' next to the rest of gowl's data; the
 * user's is `$XDG_CONFIG_HOME/gowl/menu.yaml'.  A user file that fails
 * to parse leaves the shipped tree standing rather than emptying the
 * menu: a typo in an extension should cost the extension.
 *
 * Returns: %TRUE when at least one file was read
 */
gboolean gowl_menu_load (GowlMenu *self, GError **error);

/**
 * gowl_menu_load_file:
 * @self: a #GowlMenu
 * @path: the YAML file to read
 * @merge: %TRUE to overlay onto what is already loaded, %FALSE to replace
 * @error: (nullable): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean gowl_menu_load_file (GowlMenu     *self,
                               const gchar  *path,
                               gboolean      merge,
                               GError      **error);

/**
 * gowl_menu_load_data:
 * @self: a #GowlMenu
 * @yaml: the document text
 * @merge: %TRUE to overlay onto what is already loaded, %FALSE to replace
 * @error: (nullable): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean gowl_menu_load_data (GowlMenu     *self,
                               const gchar  *yaml,
                               gboolean      merge,
                               GError      **error);

/**
 * gowl_menu_get_source:
 * @self: a #GowlMenu
 *
 * Returns: (transfer none) (nullable): the files the tree was read
 *   from, joined by `:', or %NULL when it came from a string
 */
const gchar *gowl_menu_get_source (GowlMenu *self);

/**
 * gowl_menu_get_serial:
 * @self: a #GowlMenu
 *
 * Bumped by every load.  A front end that caches rows compares this to
 * know when to throw them away.
 *
 * Returns: the load serial
 */
guint gowl_menu_get_serial (GowlMenu *self);

/**
 * gowl_menu_n_entries:
 * @self: a #GowlMenu
 *
 * Returns: how many entries the tree holds, root excluded
 */
guint gowl_menu_n_entries (GowlMenu *self);

/**
 * gowl_menu_resolve:
 * @self: a #GowlMenu
 * @route: (nullable): a route, an alias, or %NULL
 *
 * Turns what somebody typed into a route this menu knows.  Matching is
 * case-insensitive and treats `_' as `-'; an exact route beats any
 * alias; %NULL, the empty string and `root' all mean the top.  An
 * unknown string comes back unchanged so a caller can report what was
 * actually asked for rather than silently opening the root.
 *
 * Returns: (transfer full): the resolved route
 */
gchar *gowl_menu_resolve (GowlMenu *self, const gchar *route);

/**
 * gowl_menu_has_route:
 * @self: a #GowlMenu
 * @route: (nullable): a resolved route
 *
 * Returns: %TRUE when the tree holds that route
 */
gboolean gowl_menu_has_route (GowlMenu *self, const gchar *route);

/**
 * gowl_menu_is_submenu:
 * @self: a #GowlMenu
 * @route: (nullable): a resolved route
 *
 * Returns: %TRUE when the route names something with rows under it
 */
gboolean gowl_menu_is_submenu (GowlMenu *self, const gchar *route);

/**
 * gowl_menu_get_title:
 * @self: a #GowlMenu
 * @route: (nullable): a resolved route
 *
 * The header text for an open submenu: its `title:' when it has one,
 * otherwise its label.  That is how a row reading `Browser' under
 * Defaults can open a list headed `Default browser'.
 *
 * Returns: (transfer full): the title
 */
gchar *gowl_menu_get_title (GowlMenu *self, const gchar *route);

/**
 * gowl_menu_get_parent:
 * @self: a #GowlMenu
 * @route: (nullable): a resolved route
 *
 * Returns: (transfer full) (nullable): the route one level up, or %NULL
 *   at the root
 */
gchar *gowl_menu_get_parent (GowlMenu *self, const gchar *route);

/**
 * gowl_menu_list:
 * @self: a #GowlMenu
 * @compositor: (nullable): the compositor guards and providers read
 * @route: (nullable): a resolved route, %NULL for the root
 *
 * The rows under @route that pass their `when:' guard, in declaration
 * order, with `checked:' and `disabled:' already evaluated and any
 * provider already run.
 *
 * A %NULL @compositor answers every compositor-dependent guard as
 * failed and runs no provider, which is what makes the tree testable
 * without a session.
 *
 * Returns: (transfer full) (element-type GowlMenuRow): the visible rows
 */
GPtrArray *gowl_menu_list (GowlMenu       *self,
                            GowlCompositor *compositor,
                            const gchar    *route);

/**
 * gowl_menu_search:
 * @self: a #GowlMenu
 * @compositor: (nullable): the compositor guards and providers read
 * @text: what was typed
 *
 * Every visible row in the WHOLE tree that matches @text, best first,
 * each carrying its path as its detail line.
 *
 * Provider rows are searched too, so typing finds an application as
 * readily as it finds a submenu --- which is most of what makes the
 * menu a launcher as well as a control surface.
 *
 * Search is global rather than per-level on purpose: somebody typing
 * `wifi' knows the word and not which of eight submenus it lives in,
 * and a search that only looks at the level you are standing on is a
 * filter, which is a different and much less useful thing.
 *
 * Returns: (transfer full) (element-type GowlMenuRow): the matches
 */
GPtrArray *gowl_menu_search (GowlMenu       *self,
                              GowlCompositor *compositor,
                              const gchar    *text);

/**
 * gowl_menu_activate:
 * @self: a #GowlMenu
 * @compositor: (nullable): the compositor the action runs against
 * @route: a resolved route
 * @out_route: (out) (transfer full) (nullable): the submenu to open
 *
 * Runs what @route names.  A submenu or a link reports
 * %GOWL_MENU_RESULT_OPEN and fills @out_route; anything else runs
 * through the compositor's keybind dispatcher, so a menu row can do
 * precisely what a key can do and nothing more.
 *
 * Returns: what happened
 */
GowlMenuResult gowl_menu_activate (GowlMenu       *self,
                                    GowlCompositor *compositor,
                                    const gchar    *route,
                                    gchar         **out_route);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GowlMenuRow, gowl_menu_row_free)

G_END_DECLS

#endif /* GOWL_MENU_H */
