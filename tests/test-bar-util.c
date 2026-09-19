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
 * The pure helpers behind the shipped bar plugins, each pinned to the
 * input that was wrong before it existed.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "../modules/bar/bar-util.c"

/* ----------------------------------------------------------------
 * Shell detection
 *
 * The recorder's fallback command was `mkdir -p ~/Videos && wf-recorder
 * -g "$(slurp)" ...', spawned through g_shell_parse_argv() and
 * g_spawn_async(): mkdir received `&&', `wf-recorder' and a literal
 * `$(slurp)' as arguments and nothing was recorded.  The screenshot
 * fallback and the documented `button.command' example had the same
 * shape.
 * ---------------------------------------------------------------- */

static void
test_plain_commands_need_no_shell(void)
{
	g_assert_false(bar_shell_needs_sh("wpctl set-volume @DEFAULT_AUDIO_SINK@ 50%"));
	g_assert_false(bar_shell_needs_sh("gst -e btop"));
	g_assert_false(bar_shell_needs_sh("satty --filename '/tmp/a b.png'"));
	g_assert_false(bar_shell_needs_sh("~/bin/pomo"));
	g_assert_false(bar_shell_needs_sh(""));
	g_assert_false(bar_shell_needs_sh(NULL));
}

static void
test_shell_syntax_is_recognised(void)
{
	g_assert_true(bar_shell_needs_sh("wf-recorder -g \"$(slurp)\""));
	g_assert_true(bar_shell_needs_sh("mkdir -p ~/Videos && wf-recorder"));
	g_assert_true(bar_shell_needs_sh("pgrep -x foo | head -1"));
	g_assert_true(bar_shell_needs_sh("echo $HOME"));
	g_assert_true(bar_shell_needs_sh("cat *.log"));
	g_assert_true(bar_shell_needs_sh("sleep 1; notify-send done"));
	g_assert_true(bar_shell_needs_sh("cmd > /dev/null"));
	g_assert_true(bar_shell_needs_sh("echo `date`"));
}

/* ----------------------------------------------------------------
 * ANSI stripping
 *
 * The old loop skipped from ESC to the next `m' in the string, which
 * is right for SGR and wrong for everything else: `ESC [ K' (erase to
 * end of line) ate the text up to the next letter m.
 * ---------------------------------------------------------------- */

static void
test_sgr_is_removed(void)
{
	gchar text[] = "\033[1;32mgreen\033[0m and plain";

	bar_strip_ansi(text);
	g_assert_cmpstr(text, ==, "green and plain");
}

static void
test_non_sgr_csi_does_not_eat_text(void)
{
	gchar text[] = "\033[Ktemperature 40";

	bar_strip_ansi(text);
	g_assert_cmpstr(text, ==, "temperature 40");
}

static void
test_osc_and_bare_escapes(void)
{
	gchar osc[] = "\033]0;title\007label";
	gchar osc_st[] = "\033]8;;http://x\033\\link";
	gchar bare[] = "\033cclear";
	gchar trailing[] = "text\033";

	bar_strip_ansi(osc);
	g_assert_cmpstr(osc, ==, "label");
	bar_strip_ansi(osc_st);
	g_assert_cmpstr(osc_st, ==, "link");
	bar_strip_ansi(bare);
	g_assert_cmpstr(bare, ==, "clear");
	bar_strip_ansi(trailing);
	g_assert_cmpstr(trailing, ==, "text");
	bar_strip_ansi(NULL);
}

/* ----------------------------------------------------------------
 * Clock format
 *
 * The clock polled every five seconds whatever the format, so a
 * `%H:%M:%S' clock showed seconds that jumped in fives.
 * ---------------------------------------------------------------- */

static void
test_seconds_formats(void)
{
	g_assert_false(bar_strftime_has_seconds("%a %b %d  %H:%M"));
	g_assert_false(bar_strftime_has_seconds("%%S is a literal"));
	g_assert_false(bar_strftime_has_seconds(NULL));
	g_assert_true(bar_strftime_has_seconds("%H:%M:%S"));
	g_assert_true(bar_strftime_has_seconds("%T"));
	g_assert_true(bar_strftime_has_seconds("%-S"));
	g_assert_true(bar_strftime_has_seconds("%OS"));
	g_assert_true(bar_strftime_has_seconds("%c"));
	g_assert_true(bar_strftime_has_seconds("%s"));
	g_assert_true(bar_strftime_has_seconds("%r"));
	g_assert_false(bar_strftime_has_seconds("%"));
}

/* ----------------------------------------------------------------
 * Git
 *
 * The git widget looked for `.git/HEAD' as a file under a directory,
 * which is an ordinary checkout and nothing else.  In a worktree --
 * which is where feature work happens here -- `.git' is a FILE naming
 * the real directory, and the widget showed no branch at all.
 * ---------------------------------------------------------------- */

static gchar *
make_tree(const gchar *layout)
{
	g_autofree gchar *root = NULL;
	g_auto(GStrv) parts = NULL;
	gint i;

	root = g_dir_make_tmp("gowl-bar-git-XXXXXX", NULL);
	g_assert_nonnull(root);

	parts = g_strsplit(layout, "\n", -1);
	for (i = 0; parts[i] != NULL; i++) {
		g_autofree gchar *path = NULL;
		gchar *eq;

		if (parts[i][0] == '\0')
			continue;
		eq = strchr(parts[i], '=');
		if (eq == NULL) {
			path = g_build_filename(root, parts[i], NULL);
			g_assert_cmpint(g_mkdir_with_parents(path, 0700), ==, 0);
		} else {
			g_autofree gchar *parent = NULL;

			*eq = '\0';
			path = g_build_filename(root, parts[i], NULL);
			parent = g_path_get_dirname(path);
			g_assert_cmpint(g_mkdir_with_parents(parent, 0700),
			                ==, 0);
			g_assert_true(g_file_set_contents(path, eq + 1, -1,
			                                  NULL));
		}
	}
	return g_steal_pointer(&root);
}

static void
remove_tree(const gchar *root)
{
	g_autofree gchar *line = NULL;

	line = g_strdup_printf("rm -rf '%s'", root);
	g_assert_cmpint(system(line), ==, 0);
}

static void
test_ordinary_checkout(void)
{
	g_autofree gchar *root = make_tree(
		"repo/.git/HEAD=ref: refs/heads/main\n"
		"repo/src/deep\n");
	g_autofree gchar *deep = g_build_filename(root, "repo/src/deep", NULL);
	g_autofree gchar *want = g_build_filename(root, "repo/.git/HEAD", NULL);
	g_autofree gchar *head = bar_git_head_path(deep);

	g_assert_cmpstr(head, ==, want);
	remove_tree(root);
}

static void
test_worktree_gitdir_file(void)
{
	/* The real repository, and a worktree whose `.git' is a file
	   pointing at the repository's worktrees/<name> directory --
	   exactly what `git worktree add' writes. */
	g_autofree gchar *root = make_tree(
		"main/.git/HEAD=ref: refs/heads/master\n"
		"main/.git/worktrees/feature/HEAD=ref: refs/heads/feature\n");
	g_autofree gchar *wt = g_build_filename(root, "main/trees/feature",
	                                        NULL);
	g_autofree gchar *dotgit = g_build_filename(wt, ".git", NULL);
	g_autofree gchar *gitdir = g_build_filename(root,
		"main/.git/worktrees/feature", NULL);
	g_autofree gchar *body = g_strdup_printf("gitdir: %s\n", gitdir);
	g_autofree gchar *want = g_build_filename(gitdir, "HEAD", NULL);
	g_autofree gchar *sub = g_build_filename(wt, "src", NULL);
	g_autofree gchar *head = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *contents = NULL;

	g_assert_cmpint(g_mkdir_with_parents(sub, 0700), ==, 0);
	g_assert_true(g_file_set_contents(dotgit, body, -1, NULL));

	head = bar_git_head_path(sub);
	g_assert_cmpstr(head, ==, want);

	/* And the branch it names is the WORKTREE's, not master's. */
	g_assert_true(g_file_get_contents(head, &contents, NULL, NULL));
	label = bar_git_ref_label(contents);
	g_assert_cmpstr(label, ==, "feature");

	remove_tree(root);
}

static void
test_relative_gitdir(void)
{
	/* A submodule's `.git' file is relative: `gitdir: ../.git/modules/x'. */
	g_autofree gchar *root = make_tree(
		"super/.git/modules/lib/HEAD=ref: refs/heads/dev\n"
		"super/lib/.git=gitdir: ../.git/modules/lib\n");
	g_autofree gchar *lib = g_build_filename(root, "super/lib", NULL);
	g_autofree gchar *head = bar_git_head_path(lib);
	g_autofree gchar *want = g_build_filename(root,
		"super/lib/../.git/modules/lib/HEAD", NULL);

	g_assert_cmpstr(head, ==, want);
	g_assert_true(g_file_test(head, G_FILE_TEST_EXISTS));
	remove_tree(root);
}

static void
test_outside_a_repository(void)
{
	g_autofree gchar *root = make_tree("plain/dir\n");
	g_autofree gchar *dir = g_build_filename(root, "plain/dir", NULL);

	g_assert_null(bar_git_head_path(dir));
	g_assert_null(bar_git_head_path("relative/path"));
	g_assert_null(bar_git_head_path(NULL));
	remove_tree(root);
}

static void
test_ref_labels(void)
{
	g_autofree gchar *branch = bar_git_ref_label("ref: refs/heads/fix/x\n");
	g_autofree gchar *detached = bar_git_ref_label(
		"0123456789abcdef0123456789abcdef01234567\n");
	g_autofree gchar *other = bar_git_ref_label("ref: refs/tags/v1");
	g_autofree gchar *odd = bar_git_ref_label("not-a-sha-but-long-enough-to-be-forty-chars!!");

	g_assert_cmpstr(branch, ==, "fix/x");
	g_assert_cmpstr(detached, ==, "01234567");
	g_assert_cmpstr(other, ==, "refs/tags/v1");
	g_assert_cmpstr(odd, ==, "not-a-sha-but-long-enough-to-be-forty-chars!!");
}

/* ----------------------------------------------------------------
 * Calendar week numbers
 *
 * The grid starts on Sunday and printed g_date_time_get_week_of_year()
 * of that Sunday -- which, ISO weeks starting on Monday, is the week
 * BEFORE the one the other six cells are in.
 * ---------------------------------------------------------------- */

static void
test_sunday_row_reads_the_mondays_week(void)
{
	/* 2026-09-13 is a Sunday in ISO week 37; the Monday after it,
	   2026-09-14, opens week 38 -- and so do the Tue..Sat beside it. */
	GDateTime *sunday = g_date_time_new_utc(2026, 9, 13, 0, 0, 0);
	GDateTime *monday = g_date_time_new_utc(2026, 9, 14, 0, 0, 0);

	g_assert_cmpint(g_date_time_get_day_of_week(sunday), ==, 7);
	g_assert_cmpint(g_date_time_get_week_of_year(sunday), ==, 37);
	g_assert_cmpint(bar_iso_week_of_row(sunday), ==, 38);
	g_assert_cmpint(bar_iso_week_of_row(monday), ==, 38);

	g_date_time_unref(sunday);
	g_date_time_unref(monday);
}

static void
test_year_boundary_row(void)
{
	/* Sunday 2025-12-28 sits in ISO week 52 of 2025; Monday the 29th
	   is the first day of ISO week 1 of 2026. */
	GDateTime *sunday = g_date_time_new_utc(2025, 12, 28, 0, 0, 0);

	g_assert_cmpint(bar_iso_week_of_row(sunday), ==, 1);
	g_date_time_unref(sunday);
}

/* ----------------------------------------------------------------
 * Elisp quoting
 * ---------------------------------------------------------------- */

static void
test_elisp_quote(void)
{
	g_autofree gchar *plain = bar_elisp_quote("/tmp/shot.png");
	g_autofree gchar *quoted = bar_elisp_quote("/tmp/a \"b\"\\c.png");
	g_autofree gchar *none = bar_elisp_quote(NULL);

	g_assert_cmpstr(plain, ==, "/tmp/shot.png");
	g_assert_cmpstr(quoted, ==, "/tmp/a \\\"b\\\"\\\\c.png");
	g_assert_cmpstr(none, ==, "");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-util/plain-commands-need-no-shell",
	                test_plain_commands_need_no_shell);
	g_test_add_func("/bar-util/shell-syntax-is-recognised",
	                test_shell_syntax_is_recognised);
	g_test_add_func("/bar-util/sgr-is-removed", test_sgr_is_removed);
	g_test_add_func("/bar-util/non-sgr-csi-does-not-eat-text",
	                test_non_sgr_csi_does_not_eat_text);
	g_test_add_func("/bar-util/osc-and-bare-escapes",
	                test_osc_and_bare_escapes);
	g_test_add_func("/bar-util/seconds-formats", test_seconds_formats);
	g_test_add_func("/bar-util/git/ordinary-checkout",
	                test_ordinary_checkout);
	g_test_add_func("/bar-util/git/worktree-gitdir-file",
	                test_worktree_gitdir_file);
	g_test_add_func("/bar-util/git/relative-gitdir", test_relative_gitdir);
	g_test_add_func("/bar-util/git/outside-a-repository",
	                test_outside_a_repository);
	g_test_add_func("/bar-util/git/ref-labels", test_ref_labels);
	g_test_add_func("/bar-util/calendar/sunday-row-reads-the-mondays-week",
	                test_sunday_row_reads_the_mondays_week);
	g_test_add_func("/bar-util/calendar/year-boundary-row",
	                test_year_boundary_row);
	g_test_add_func("/bar-util/elisp-quote", test_elisp_quote);

	return g_test_run();
}
