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
 * test-macro-filter.c - The trigger filter language, on its own.
 *
 * The engine is pure GLib, so every rule of the language is checked
 * here without a compositor: precedence (and over or), parentheses,
 * not, every operator, quoting, numeric against lexical comparison,
 * absent fields, the column in each error, splitting a trigger line
 * around its [filter], and that the normalised text parses back to the
 * same filter.
 */

#include <glib.h>
#include <string.h>

#include "../modules/macro/gowl-macro-filter.c"

/* A field table from "key=value" strings. */
static GHashTable *
fields_of(const gchar *first, ...)
{
	GHashTable *f;
	const gchar *kv;
	va_list ap;

	f = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	va_start(ap, first);
	for (kv = first; kv != NULL; kv = va_arg(ap, const gchar *)) {
		const gchar *eq = strchr(kv, '=');

		g_assert_nonnull(eq);
		g_hash_table_insert(f, g_strndup(kv, (gsize)(eq - kv)),
		                    g_strdup(eq + 1));
	}
	va_end(ap);
	return f;
}

/* Parse EXPR (must succeed) and evaluate it against FIELDS. */
static gboolean
eval(
	const gchar *expr,
	GHashTable  *fields
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GowlMacroFilter) f = gowl_macro_filter_parse(expr, &error);

	if (f == NULL)
		g_test_message("%s: %s", expr, error->message);
	g_assert_no_error(error);
	g_assert_nonnull(f);
	return gowl_macro_filter_eval(f, fields);
}

static void
test_single_conditions(void)
{
	g_autoptr(GHashTable) f = fields_of("app-id=firefox",
	                                    "title=Rick - YouTube",
	                                    "clients=3", "time=22:15",
	                                    "floating=false", NULL);

	g_assert_true(eval("app-id=firefox", f));
	g_assert_true(eval("app-id == firefox", f));        /* == is = */
	g_assert_true(eval("app-id=fire*", f));             /* glob */
	g_assert_true(eval("app-id=f?refox", f));
	g_assert_false(eval("app-id=Firefox", f));          /* case matters */
	g_assert_true(eval("app-id!=foot", f));
	g_assert_false(eval("app-id!=fire*", f));
	g_assert_true(eval("title~YouTube$", f));           /* regex */
	g_assert_true(eval("title~'(?i)youtube'", f));
	/* a backslash in quotes is kept unless it escapes the quote */
	g_assert_true(eval("title~'^Rick\\s-'", f));
	g_assert_true(eval("title~'\\bYouTube\\b'", f));
	g_assert_false(eval("title='It\\'s'", f));
	g_assert_true(eval("title!~^Twitch", f));
	g_assert_true(eval("title=\"Rick - YouTube\"", f)); /* quoted */
	g_assert_true(eval("title='*- YouTube'", f));
	g_assert_true(eval("floating=false", f));
}

/* Numbers compare as numbers; anything else as text. */
static void
test_comparisons(void)
{
	g_autoptr(GHashTable) f = fields_of("clients=10", "time=09:05",
	                                    "hour=9", NULL);

	g_assert_true(eval("clients>9", f));      /* 10 > 9 numerically */
	g_assert_true(eval("clients>=10", f));
	g_assert_false(eval("clients<10", f));
	g_assert_true(eval("clients<=10", f));
	g_assert_true(eval("hour<10", f));
	g_assert_true(eval("time<22:00", f));     /* zero-padded text */
	g_assert_false(eval("time>=22:00", f));
	g_assert_true(eval("time>=09:00 and time<17:30", f));
}

static void
test_and_or_not(void)
{
	g_autoptr(GHashTable) f = fields_of("app-id=mpv", "title=movie.mkv",
	                                    "monitor=DP-1", "clients=2", NULL);

	g_assert_true(eval("app-id=mpv and monitor=DP-1", f));
	g_assert_false(eval("app-id=mpv and monitor=HDMI-A-1", f));
	g_assert_true(eval("app-id=vlc or app-id=mpv", f));
	g_assert_false(eval("app-id=vlc or app-id=celluloid", f));
	g_assert_true(eval("app-id=mpv && monitor=DP-* || app-id=vlc", f));
	g_assert_true(eval("not app-id=vlc", f));
	g_assert_true(eval("!app-id=vlc", f));
	g_assert_true(eval("! (app-id=vlc or app-id=celluloid)", f));
	g_assert_true(eval("NOT app-id=vlc AND clients>1", f)); /* any case */
	/* three and more, mixed */
	g_assert_true(eval("app-id=mpv and title=*.mkv and monitor=DP-1 "
	                   "and clients<5", f));
	g_assert_true(eval("app-id=a or app-id=b or app-id=c or app-id=mpv", f));
}

/* `and' binds tighter than `or'; parentheses change it. */
static void
test_precedence(void)
{
	g_autoptr(GHashTable) f = fields_of("app-id=x", "title=y", NULL);

	/* true or (false and false) = true */
	g_assert_true(eval("app-id=x or title=no and title=never", f));
	/* (true or false) and false = false */
	g_assert_false(eval("(app-id=x or title=no) and title=never", f));
	/* not binds tightest: (not false) and true */
	g_assert_true(eval("not title=no and app-id=x", f));
	g_assert_false(eval("not (title=y and app-id=x)", f));
	g_assert_true(eval("((app-id=x))", f));
}

/* An absent field is the empty string. */
static void
test_missing_fields(void)
{
	g_autoptr(GHashTable) f = fields_of("event=timer", NULL);

	g_assert_false(eval("app-id=*?*", f));    /* needs a window */
	g_assert_true(eval("app-id=\"\"", f));
	g_assert_true(eval("app-id!=firefox", f));
	g_assert_true(eval("event=timer", f));
	/* no table at all */
	g_assert_false(eval("app-id=x", NULL));
}

static void
assert_parse_error(
	const gchar *expr,
	gint         code,
	const gchar *fragment
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GowlMacroFilter) f = gowl_macro_filter_parse(expr, &error);

	g_assert_null(f);
	g_assert_error(error, GOWL_MACRO_FILTER_ERROR, code);
	if (strstr(error->message, fragment) == NULL)
		g_test_message("%s -> %s", expr, error->message);
	g_assert_nonnull(strstr(error->message, fragment));
}

static void
test_errors(void)
{
	assert_parse_error("", GOWL_MACRO_FILTER_ERROR_SYNTAX, "column 1");
	assert_parse_error("appid=x", GOWL_MACRO_FILTER_ERROR_FIELD,
	                   "unknown field `appid'");
	assert_parse_error("app-id=x and titel=y", GOWL_MACRO_FILTER_ERROR_FIELD,
	                   "column 14");
	assert_parse_error("app-id", GOWL_MACRO_FILTER_ERROR_SYNTAX,
	                   "expected an operator");
	assert_parse_error("app-id=", GOWL_MACRO_FILTER_ERROR_SYNTAX,
	                   "expected a value");
	assert_parse_error("app-id=x and", GOWL_MACRO_FILTER_ERROR_SYNTAX,
	                   "found the end");
	assert_parse_error("(app-id=x", GOWL_MACRO_FILTER_ERROR_SYNTAX,
	                   "expected `)'");
	assert_parse_error("app-id=x)", GOWL_MACRO_FILTER_ERROR_SYNTAX,
	                   "unexpected");
	assert_parse_error("app-id=x title=y", GOWL_MACRO_FILTER_ERROR_SYNTAX,
	                   "joined with and/or");
	assert_parse_error("title=\"open", GOWL_MACRO_FILTER_ERROR_SYNTAX,
	                   "unterminated");
	assert_parse_error("title~'('", GOWL_MACRO_FILTER_ERROR_REGEX,
	                   "bad regex");
}

/* The normalised text says how it was read, and reads back the same. */
static void
test_to_string_round_trip(void)
{
	static const gchar * const exprs[] = {
		"app-id=x or title=y and clients>2",
		"not (app-id=firefox* || title~'(?i)you\"tube')",
		"layout=[]= and time>=22:00",
		NULL
	};
	g_autoptr(GHashTable) a = fields_of("app-id=x", "title=nope",
	                                    "clients=1", NULL);
	g_autoptr(GHashTable) b = fields_of("app-id=q", "title=y",
	                                    "clients=9", "layout=[]=",
	                                    "time=23:00", NULL);
	guint i;

	for (i = 0; exprs[i] != NULL; i++) {
		g_autoptr(GowlMacroFilter) f1 = gowl_macro_filter_parse(exprs[i],
		                                                        NULL);
		g_autofree gchar *s1 = NULL;
		g_autofree gchar *s2 = NULL;
		g_autoptr(GowlMacroFilter) f2 = NULL;

		g_assert_nonnull(f1);
		s1 = gowl_macro_filter_to_string(f1);
		f2 = gowl_macro_filter_parse(s1, NULL);
		g_assert_nonnull(f2);
		s2 = gowl_macro_filter_to_string(f2);
		g_assert_cmpstr(s1, ==, s2);
		g_assert_cmpint(gowl_macro_filter_eval(f1, a), ==,
		                gowl_macro_filter_eval(f2, a));
		g_assert_cmpint(gowl_macro_filter_eval(f1, b), ==,
		                gowl_macro_filter_eval(f2, b));
	}
	{
		g_autoptr(GowlMacroFilter) f = gowl_macro_filter_parse(exprs[0],
		                                                       NULL);
		g_autofree gchar *s = gowl_macro_filter_to_string(f);

		/* and grouped first */
		g_assert_cmpstr(s, ==,
		                "(app-id=\"x\" or (title=\"y\" and clients>\"2\"))");
	}
}

static void
split(
	const gchar *line,
	const gchar *what,
	const gchar *filter,
	const gchar *rest
){
	g_autofree gchar *w = NULL;
	g_autofree gchar *f = NULL;
	g_autofree gchar *r = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(gowl_macro_filter_split_line(line, &w, &f, &r, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(w, ==, what);
	g_assert_cmpstr(f, ==, filter);
	g_assert_cmpstr(r, ==, rest);
}

static void
split_fails(
	const gchar *line,
	const gchar *fragment
){
	g_autofree gchar *w = NULL;
	g_autofree gchar *f = NULL;
	g_autofree gchar *r = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_false(gowl_macro_filter_split_line(line, &w, &f, &r, &error));
	g_assert_nonnull(error);
	if (strstr(error->message, fragment) == NULL)
		g_test_message("%s -> %s", line, error->message);
	g_assert_nonnull(strstr(error->message, fragment));
}

static void
test_split_line(void)
{
	split("client-added: tidy-on-map", "client-added", NULL, "tidy-on-map");
	split("client-added [app-id=foot]: x a b", "client-added", "app-id=foot",
	      "x a b");
	split("every 1000 [hour>=22]: night", "every 1000", "hour>=22", "night");
	/* a colon inside the filter, bare or quoted, is not the separator */
	split("focus-changed [title=*a:b*]: m", "focus-changed", "title=*a:b*",
	      "m");
	split("focus-changed [title=\"x]: y\"]: m", "focus-changed",
	      "title=\"x]: y\"", "m");
	/* balanced brackets in a value (a layout symbol) */
	split("layout-changed [arg=[]=]: m", "layout-changed", "arg=[]=", "m");
	split("layout-changed  [ arg=x ]  :  m  a ", "layout-changed", "arg=x",
	      "m  a");

	split_fails("client-added tidy", "expected");
	split_fails("client-added [app-id=x: m", "without `]'");
	split_fails("client-added app-id=x]: m", "without `['");
	split_fails("client-added [a=b] [c=d]: m", "one [filter]");
	split_fails("client-added [app-id=x] junk: m", "right before");
	split_fails("client-added [title=\"x]: m", "unterminated");
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/macro/filter/single-conditions", test_single_conditions);
	g_test_add_func("/macro/filter/comparisons", test_comparisons);
	g_test_add_func("/macro/filter/and-or-not", test_and_or_not);
	g_test_add_func("/macro/filter/precedence", test_precedence);
	g_test_add_func("/macro/filter/missing-fields", test_missing_fields);
	g_test_add_func("/macro/filter/errors", test_errors);
	g_test_add_func("/macro/filter/round-trip", test_to_string_round_trip);
	g_test_add_func("/macro/filter/split-line", test_split_line);
	return g_test_run();
}
