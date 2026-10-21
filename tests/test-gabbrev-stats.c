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
 * test-gabbrev-stats.c - Usage counts, in memory and on disk.
 *
 * The records are what `gabbrev-stats' and the list's `uses' column
 * show, and what a user prunes a snippet file by -- so a count must
 * survive a restart, a merge must add rather than replace, and one bad
 * line must not cost the rest.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "../modules/gabbrev/gowl-gabbrev-stats.c"

/* A scratch file path in a fresh directory, removed by the caller */
static gchar *
scratch_path(gchar **dir)
{
	*dir = g_dir_make_tmp("gabbrev-stats-XXXXXX", NULL);
	g_assert_nonnull(*dir);
	return g_build_filename(*dir, "sub", "stats.tsv", NULL);
}

static void
remove_scratch(
	const gchar *dir,
	const gchar *path
){
	g_autofree gchar *sub = g_path_get_dirname(path);

	g_unlink(path);
	g_rmdir(sub);
	g_rmdir(dir);
}

/* Counting: count, first, last, and a clock that goes backwards */
static void
test_record(void)
{
	g_autoptr(GabbrevStats) s = gabbrev_stats_new();
	const GabbrevStat *st;

	g_assert_null(gabbrev_stats_lookup(s, ";today"));
	g_assert_false(gabbrev_stats_is_dirty(s));
	gabbrev_stats_record(s, ";today", 100);
	gabbrev_stats_record(s, ";today", 300);
	gabbrev_stats_record(s, ";today", 200);   /* the clock went back */
	st = gabbrev_stats_lookup(s, ";today");
	g_assert_nonnull(st);
	g_assert_cmpuint(st->count, ==, 3);
	g_assert_cmpint(st->first, ==, 100);
	g_assert_cmpint(st->last, ==, 300);
	g_assert_true(gabbrev_stats_is_dirty(s));

	/* An empty id is no match at all */
	gabbrev_stats_record(s, "", 1);
	gabbrev_stats_record(s, NULL, 1);
	g_assert_cmpuint(gabbrev_stats_size(s), ==, 1);
}

/* Order: most used, then most recent, then by id */
static void
test_order(void)
{
	g_autoptr(GabbrevStats) s = gabbrev_stats_new();
	g_auto(GStrv) ids = NULL;

	gabbrev_stats_record(s, "b", 10);
	gabbrev_stats_record(s, "a", 10);
	gabbrev_stats_record(s, "c", 50);
	gabbrev_stats_record(s, "hot", 1);
	gabbrev_stats_record(s, "hot", 2);
	ids = gabbrev_stats_ids(s);
	g_assert_cmpuint(g_strv_length(ids), ==, 4);
	g_assert_cmpstr(ids[0], ==, "hot");
	g_assert_cmpstr(ids[1], ==, "c");
	g_assert_cmpstr(ids[2], ==, "a");
	g_assert_cmpstr(ids[3], ==, "b");
}

/* A round trip, with ids that would break a naive format */
static void
test_round_trip(void)
{
	g_autoptr(GabbrevStats) s = gabbrev_stats_new();
	g_autoptr(GabbrevStats) back = gabbrev_stats_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = scratch_path(&dir);
	static const gchar *odd = "tab\there\nnew \"line\" \\ \xc2\xaf\\_(\xe3\x83\x84)_/\xc2\xaf";
	const GabbrevStat *st;

	gabbrev_stats_record(s, ";shrug", 1700000000);
	gabbrev_stats_record(s, ";shrug", 1700000500);
	gabbrev_stats_record(s, odd, 42);
	/* The directory does not exist yet: save makes it */
	g_assert_true(gabbrev_stats_save(s, path, &error));
	g_assert_no_error(error);
	g_assert_false(gabbrev_stats_is_dirty(s));

	g_assert_true(gabbrev_stats_load(back, path, &error));
	g_assert_no_error(error);
	g_assert_false(gabbrev_stats_is_dirty(back));
	g_assert_cmpuint(gabbrev_stats_size(back), ==, 2);
	st = gabbrev_stats_lookup(back, ";shrug");
	g_assert_nonnull(st);
	g_assert_cmpuint(st->count, ==, 2);
	g_assert_cmpint(st->first, ==, 1700000000);
	g_assert_cmpint(st->last, ==, 1700000500);
	st = gabbrev_stats_lookup(back, odd);
	g_assert_nonnull(st);
	g_assert_cmpuint(st->count, ==, 1);
	remove_scratch(dir, path);
}

/* A load merges: uses before the load are not lost, counts add */
static void
test_merge(void)
{
	g_autoptr(GabbrevStats) s = gabbrev_stats_new();
	g_autoptr(GabbrevStats) live = gabbrev_stats_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = scratch_path(&dir);
	const GabbrevStat *st;

	gabbrev_stats_record(s, "x", 100);
	gabbrev_stats_record(s, "x", 200);
	g_assert_true(gabbrev_stats_save(s, path, &error));

	gabbrev_stats_record(live, "x", 50);
	gabbrev_stats_record(live, "y", 60);
	g_assert_true(gabbrev_stats_load(live, path, &error));
	g_assert_no_error(error);
	st = gabbrev_stats_lookup(live, "x");
	g_assert_cmpuint(st->count, ==, 3);
	g_assert_cmpint(st->first, ==, 50);
	g_assert_cmpint(st->last, ==, 200);
	/* The merged records differ from the file: still to be saved */
	g_assert_true(gabbrev_stats_is_dirty(live));
	remove_scratch(dir, path);
}

/* Bad lines are skipped, good ones kept; a missing file is fine */
static void
test_bad_lines(void)
{
	g_autoptr(GabbrevStats) s = gabbrev_stats_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *path = scratch_path(&dir);
	g_autofree gchar *sub = g_path_get_dirname(path);

	g_assert_true(gabbrev_stats_load(s, path, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(gabbrev_stats_size(s), ==, 0);

	g_assert_cmpint(g_mkdir_with_parents(sub, 0700), ==, 0);
	g_assert_true(g_file_set_contents(path,
		"# header\n"
		"\n"
		"3\t10\t20\tgood\n"
		"x\t10\t20\tnot-a-count\n"
		"0\t10\t20\tzero\n"
		"-4\t10\t20\tnegative\n"
		"2\t10\tlate\tbad-last\n"
		"2\t10\t20\n"
		"5\t1\t2\t\n"
		"1\t5\t6\talso good\n", -1, NULL));
	g_assert_true(gabbrev_stats_load(s, path, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(gabbrev_stats_size(s), ==, 2);
	g_assert_cmpuint(gabbrev_stats_lookup(s, "good")->count, ==, 3);
	g_assert_cmpuint(gabbrev_stats_lookup(s, "also good")->count, ==, 1);
	remove_scratch(dir, path);
}

/* Forgetting one, then all */
static void
test_forget(void)
{
	g_autoptr(GabbrevStats) s = gabbrev_stats_new();

	gabbrev_stats_record(s, "a", 1);
	gabbrev_stats_record(s, "b", 1);
	g_assert_true(gabbrev_stats_forget(s, "a"));
	g_assert_false(gabbrev_stats_forget(s, "a"));
	g_assert_cmpuint(gabbrev_stats_size(s), ==, 1);
	gabbrev_stats_clear(s);
	g_assert_cmpuint(gabbrev_stats_size(s), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gabbrev/stats/record", test_record);
	g_test_add_func("/gabbrev/stats/order", test_order);
	g_test_add_func("/gabbrev/stats/round-trip", test_round_trip);
	g_test_add_func("/gabbrev/stats/merge", test_merge);
	g_test_add_func("/gabbrev/stats/bad-lines", test_bad_lines);
	g_test_add_func("/gabbrev/stats/forget", test_forget);
	return g_test_run();
}
