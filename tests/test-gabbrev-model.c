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
 * test-gabbrev-model.c - Reading gabbrev and espanso configurations.
 *
 * The loader is a pure translation unit, included here.  The fixture
 * tests/fixtures/gabbrev/espanso is an espanso configuration directory
 * laid out the way espanso writes one: config/default.yml, two
 * app-specific configs, match files with `_'-prefixed ones that load
 * only when imported, and a package.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "gowl.h"
#include "../modules/gabbrev/gowl-gabbrev-model.c"
#include "../modules/gabbrev/gowl-gabbrev-builtin.c"

static GabbrevSet *
load_dir(const gchar *sub)
{
	g_autoptr(GabbrevLoader) l = gabbrev_loader_new();
	g_autofree gchar *path = g_build_filename(GOWL_TEST_FIXTURES, sub, NULL);

	gabbrev_loader_add_path(l, path, NULL);
	return gabbrev_loader_finish(l);
}

static gboolean
has_problem(
	GabbrevSet  *set,
	const gchar *needle
){
	guint i;

	for (i = 0; i < set->problems->len; i++)
		if (strstr(g_ptr_array_index(set->problems, i), needle) != NULL)
			return TRUE;
	return FALSE;
}

static GabbrevProfile *
profile_named(
	GabbrevSet  *set,
	const gchar *name
){
	guint i;

	for (i = 0; i < set->profiles->len; i++) {
		GabbrevProfile *p = g_ptr_array_index(set->profiles, i);

		if (g_strcmp0(p->name, name) == 0)
			return p;
	}
	return NULL;
}

/* The shipped matches load cleanly, and no trigger is the start of
   another -- a trigger fires on its last character, so a prefix would
   make the longer one impossible to type. */
static void
test_builtin(void)
{
	g_autoptr(GabbrevLoader) l = gabbrev_loader_new();
	g_autoptr(GabbrevSet) set = NULL;
	guint i;
	guint j;

	gabbrev_loader_add_builtin(l);
	set = gabbrev_loader_finish(l);
	for (i = 0; i < set->problems->len; i++)
		g_test_message("problem: %s", (gchar *)g_ptr_array_index(set->problems, i));
	g_assert_cmpuint(set->problems->len, ==, 0);
	g_assert_nonnull(gabbrev_set_find_trigger(set, ";today"));
	g_assert_nonnull(gabbrev_set_find_trigger(set, ";htoday"));
	g_assert_nonnull(gabbrev_set_find_trigger(set, ";mtoday"));
	g_assert_nonnull(gabbrev_set_find_trigger(set, ";now"));
	g_assert_nonnull(gabbrev_set_find_trigger(set, ";hnow"));
	g_assert_nonnull(gabbrev_set_find_trigger(set, ";mnow"));
	g_assert_nonnull(gabbrev_set_find_trigger(set, ";reword"));
	g_assert_cmpstr(gabbrev_set_find_trigger(set, ";shrug")->replace, ==,
	                "\xc2\xaf\\_(\xe3\x83\x84)_/\xc2\xaf");

	/* Every trigger of every match, aliases (;date) included */
	{
		g_autoptr(GPtrArray) all = g_ptr_array_new();
		guint k;

		for (i = 0; i < set->matches->len; i++) {
			GabbrevMatch *m = g_ptr_array_index(set->matches, i);

			for (k = 0; k < m->triggers->len; k++)
				g_ptr_array_add(all, g_ptr_array_index(m->triggers, k));
		}
		for (i = 0; i < all->len; i++) {
			const gchar *ta = g_ptr_array_index(all, i);

			g_assert_true(g_str_has_prefix(ta, ";"));
			for (j = 0; j < all->len; j++) {
				const gchar *tb = g_ptr_array_index(all, j);

				if (i != j && g_str_has_prefix(tb, ta))
					g_error("built-in `%s' is a prefix of `%s'", ta, tb);
			}
		}
	}
	g_assert_true(gabbrev_set_find_trigger(set, ";date")
	              == gabbrev_set_find_trigger(set, ";today"));
}

/* An espanso configuration directory, read as espanso reads it. */
static void
test_espanso_dir(void)
{
	g_autoptr(GabbrevSet) set = load_dir("espanso");
	GabbrevMatch *m;
	GabbrevProfile *term;
	GabbrevProfile *keepass;

	/* config/default.yml: espanso's capitalised values too */
	g_assert_cmpint(set->options.backend, ==, GABBREV_BACKEND_CLIPBOARD);
	g_assert_cmpuint(set->options.clipboard_threshold, ==, 50);
	g_assert_cmpstr(set->options.toggle_key, ==, "ALT");
	g_assert_cmpstr(set->options.search_shortcut, ==, "ALT+SPACE");
	g_assert_false(set->options.undo_backspace);

	/* match files: base, an import, a package; an `_' file only when
	   imported or included */
	g_assert_nonnull(gabbrev_set_find_trigger(set, ":hello"));
	g_assert_nonnull(gabbrev_set_find_trigger(set, ":shared"));
	g_assert_nonnull(gabbrev_set_find_trigger(set, ":alpha"));
	g_assert_null(gabbrev_set_find_trigger(set, ":never"));
	m = gabbrev_set_find_trigger(set, ":signature");
	g_assert_nonnull(m);
	g_assert_true(m == gabbrev_set_find_trigger(set, ":sig"));
	g_assert_cmpstr(m->replace, ==, "Best,\n{{who}}\n");
	g_assert_nonnull(gabbrev_set_find_global(set, "who"));

	/* regex, form, word */
	m = gabbrev_set_find_id(set, ":add\\((?P<x>\\d+),(?P<y>\\d+)\\)");
	g_assert_nonnull(m);
	g_assert_nonnull(m->regex);
	m = gabbrev_set_find_trigger(set, ":form");
	g_assert_cmpstr(m->replace, ==, "Dear {{form1.name}}, {{form1.mood}}");
	g_assert_cmpuint(m->vars->len, ==, 1);
	g_assert_cmpstr(((GabbrevVar *)g_ptr_array_index(m->vars, 0))->type, ==,
	                "form");
	m = gabbrev_set_find_trigger(set, ":word");
	g_assert_true(m->left_word && m->right_word && m->propagate_case);

	/* an unknown key is a problem, and the match still loads */
	g_assert_true(has_problem(set, "frobnicate"));
	g_assert_nonnull(gabbrev_set_find_trigger(set, ":bad"));

	/* app-specific configs */
	term = profile_named(set, "terminal");
	keepass = profile_named(set, "keepass");
	g_assert_nonnull(term);
	g_assert_nonnull(keepass);
	g_assert_true(term->options.set & GABBREV_OPT_BACKEND);
	g_assert_cmpint(term->options.backend, ==, GABBREV_BACKEND_KEYS);
	g_assert_true(keepass->options.set & GABBREV_OPT_ENABLE);
	g_assert_false(keepass->options.enable);
}

/* Which profile a window gets, and which files it allows. */
static void
test_profiles(void)
{
	g_autoptr(GabbrevSet) set = load_dir("espanso");
	GabbrevWindow foot = { "foot", "~", "/usr/bin/foot", NULL };
	GabbrevWindow kp = { "org.keepassxc.KeePassXC", "KeePass DB", "", NULL };
	GabbrevWindow other = { "firefox", "Mozilla Firefox", "", NULL };
	const GabbrevProfile *p;
	GabbrevMatch *ll;
	GabbrevMatch *hello;

	p = gabbrev_set_profile_for(set, &foot);
	g_assert_nonnull(p);
	g_assert_cmpstr(p->name, ==, "terminal");
	p = gabbrev_set_profile_for(set, &kp);
	g_assert_nonnull(p);
	g_assert_cmpstr(p->name, ==, "keepass");
	g_assert_null(gabbrev_set_profile_for(set, &other));

	/* :ll comes from _terminal.yml, which only the terminal profile
	   includes; :hello is in every selection */
	ll = gabbrev_set_find_trigger(set, ":ll");
	hello = gabbrev_set_find_trigger(set, ":hello");
	g_assert_nonnull(ll);
	g_assert_false(gabbrev_set_match_allowed(set, NULL, ll));
	g_assert_true(gabbrev_set_match_allowed(set,
		gabbrev_set_profile_for(set, &foot), ll));
	g_assert_true(gabbrev_set_match_allowed(set, NULL, hello));
	g_assert_true(gabbrev_set_match_allowed(set,
		gabbrev_set_profile_for(set, &foot), hello));
}

/* A directory that is not an espanso configuration: its files, but not
   the `_' ones. */
static void
test_plain_dir(void)
{
	g_autoptr(GabbrevSet) set = load_dir("plain");

	g_assert_nonnull(gabbrev_set_find_trigger(set, ";one"));
	g_assert_null(gabbrev_set_find_trigger(set, ";two"));
	g_assert_cmpuint(set->problems->len, ==, 0);
}

/* gowl's `gabbrev:' section: options, matches, inline profiles with a
   gowl filter, and `files' relative to the config's directory. */
static void
test_inline_section(void)
{
	g_autoptr(GabbrevLoader) l = gabbrev_loader_new();
	g_autoptr(GabbrevSet) set = NULL;
	GabbrevMatch *m;
	GabbrevProfile *p;

	gabbrev_loader_add_builtin(l);
	g_assert_true(gabbrev_loader_add_text(l,
		"backend: keys\n"
		"key-delay: 5\n"
		"files: [plain/_two.yaml]\n"
		"profiles:\n"
		"  - {name: games, filter: \"app-id ~ steam_app\", enable: false}\n"
		"matches:\n"
		"  - trigger: \";today\"\n"
		"    replace: mine\n"
		"  - trigger: \";mail\"\n"
		"    replace: me@example.org\n"
		"    left-word: true\n"
		"    filter: \"app-id = firefox\"\n",
		GOWL_TEST_FIXTURES, GABBREV_ORIGIN_CONFIG));
	set = gabbrev_loader_finish(l);
	g_assert_cmpuint(set->problems->len, ==, 0);
	g_assert_cmpint(set->options.backend, ==, GABBREV_BACKEND_KEYS);
	g_assert_cmpuint(set->options.key_delay_ms, ==, 5);
	/* an `_' file loads when it is named */
	g_assert_nonnull(gabbrev_set_find_trigger(set, ";two"));
	/* the later ;today wins */
	g_assert_cmpstr(gabbrev_set_find_trigger(set, ";today")->replace, ==, "mine");
	m = gabbrev_set_find_trigger(set, ";mail");
	g_assert_true(m->left_word);
	g_assert_false(m->right_word);
	g_assert_nonnull(m->filter);
	p = profile_named(set, "games");
	g_assert_nonnull(p);
	g_assert_nonnull(p->filter);
	g_assert_false(p->options.enable);
}

/* espanso's include globs */
static void
test_glob(void)
{
	g_assert_true(gabbrev_glob_match("**/[!_]*.yml", "base.yml"));
	g_assert_true(gabbrev_glob_match("**/[!_]*.yml", "packages/greek/package.yml"));
	g_assert_false(gabbrev_glob_match("**/[!_]*.yml", "_shared.yml"));
	g_assert_false(gabbrev_glob_match("**/[!_]*.yml", "base.yaml"));
	g_assert_true(gabbrev_glob_match("*.yml", "a.yml"));
	g_assert_false(gabbrev_glob_match("*.yml", "x/a.yml"));
	g_assert_true(gabbrev_glob_match("a?c", "abc"));
	g_assert_true(gabbrev_glob_match("[a-c]x", "bx"));
}

/* A file that imports itself is reported, not followed for ever. */
static void
test_import_loop(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("gabbrev-XXXXXX", NULL);
	g_autofree gchar *a = g_build_filename(dir, "a.yml", NULL);
	g_autofree gchar *b = g_build_filename(dir, "b.yml", NULL);
	g_autoptr(GabbrevLoader) l = gabbrev_loader_new();
	g_autoptr(GabbrevSet) set = NULL;

	g_assert_true(g_file_set_contents(a, "imports: [b.yml]\nmatches:\n"
	                                  "  - {trigger: ';a', replace: A}\n", -1,
	                                  NULL));
	g_assert_true(g_file_set_contents(b, "imports: [a.yml]\nmatches:\n"
	                                  "  - {trigger: ';b', replace: B}\n", -1,
	                                  NULL));
	gabbrev_loader_add_path(l, a, NULL);
	set = gabbrev_loader_finish(l);
	g_assert_nonnull(gabbrev_set_find_trigger(set, ";a"));
	g_assert_nonnull(gabbrev_set_find_trigger(set, ";b"));
	g_assert_true(has_problem(set, "imports itself"));
	g_unlink(a);
	g_unlink(b);
	g_rmdir(dir);
}

/* Problems cost the one thing that is wrong, nothing else. */
static void
test_problems(void)
{
	g_autoptr(GabbrevLoader) l = gabbrev_loader_new();
	g_autoptr(GabbrevSet) set = NULL;

	g_assert_false(gabbrev_loader_add_text(l, "matches: [\n", NULL,
	                                       GABBREV_ORIGIN_RUNTIME));
	g_assert_true(gabbrev_loader_add_text(l,
		"backend: sideways\n"
		"matches:\n"
		"  - {replace: no trigger}\n"
		"  - {trigger: ';x'}\n"
		"  - {regex: '(', replace: x}\n"
		"  - {trigger: ';v', replace: '{{v}}', vars: [{name: v}]}\n"
		"  - {trigger: ';f', replace: x, filter: 'colour = red'}\n"
		"  - {trigger: ';ok', replace: fine}\n",
		NULL, GABBREV_ORIGIN_RUNTIME));
	gabbrev_loader_add_path(l, "/nonexistent/gabbrev.yml", NULL);
	set = gabbrev_loader_finish(l);
	g_assert_true(has_problem(set, "backend"));
	g_assert_true(has_problem(set, "needs trigger"));
	g_assert_true(has_problem(set, "nothing to type"));
	g_assert_true(has_problem(set, "regex"));
	g_assert_true(has_problem(set, "no type"));
	g_assert_true(has_problem(set, "filter"));
	g_assert_true(has_problem(set, "does not exist"));
	g_assert_nonnull(gabbrev_set_find_trigger(set, ";ok"));
}

/* Options merge by what a profile sets, and only that. */
static void
test_options_merge(void)
{
	GabbrevOptions base;
	GabbrevOptions over;

	gabbrev_options_init_defaults(&base);
	memset(&over, 0, sizeof over);
	over.backend = GABBREV_BACKEND_CLIPBOARD;
	over.set = GABBREV_OPT_BACKEND;
	over.clipboard_threshold = 1;   /* not set: ignored */
	gabbrev_options_merge(&base, &over);
	g_assert_cmpint(base.backend, ==, GABBREV_BACKEND_CLIPBOARD);
	g_assert_cmpuint(base.clipboard_threshold, ==, 100);
	gabbrev_options_clear(&base);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gabbrev/model/builtin", test_builtin);
	g_test_add_func("/gabbrev/model/espanso-dir", test_espanso_dir);
	g_test_add_func("/gabbrev/model/profiles", test_profiles);
	g_test_add_func("/gabbrev/model/plain-dir", test_plain_dir);
	g_test_add_func("/gabbrev/model/inline-section", test_inline_section);
	g_test_add_func("/gabbrev/model/glob", test_glob);
	g_test_add_func("/gabbrev/model/import-loop", test_import_loop);
	g_test_add_func("/gabbrev/model/problems", test_problems);
	g_test_add_func("/gabbrev/model/options-merge", test_options_merge);
	return g_test_run();
}
