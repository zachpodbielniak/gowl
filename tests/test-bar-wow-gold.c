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
 * The `wow-gold' widget, against captured tsmctl output and a stand-in
 * tsmctl that answers from it.
 *
 * Asserted:
 *
 *   MONEY IS EXACT.  Copper to gold/silver/copper, thousands
 *   separators, the compact form's rounding across a unit, negatives,
 *   and strings that are not money.
 *
 *   THE JSON WALK READS ONLY WHAT IT WAS ASKED.  A member of a nested
 *   object, or `"key":' inside a string, is never mistaken for the
 *   field; an overflowing number fails instead of wrapping; text that is
 *   not UTF-8 is made drawable.
 *
 *   A GAP IN THE HISTORY CARRIES FORWARD, and days before the first
 *   point are unknown, not zero.
 *
 *   TSMCTL IS RUN AS AN ARGV.  Settings become exact `--opt=VALUE'
 *   words; a host that is really an ssh option is refused; shell syntax
 *   in a filter reaches tsmctl as text and runs nothing.
 *
 *   ONLY THE FIRST CALL OF A READING GOES TO THE HOST, and only when
 *   one is due; a failure keeps the last reading and says it is stale.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#define BAR_HARNESS_MODULE GOWL_TEST_BAR_MODULE
#include "bar-widget-harness.h"

#include "../modules/bar/bar-wowgold-model.c"

#ifndef GOWL_TEST_BAR_MODULE
#define GOWL_TEST_BAR_MODULE "build/release/modules/bar.so"
#endif
#ifndef GOWL_TEST_FIXTURES
#define GOWL_TEST_FIXTURES "tests/fixtures/bar"
#endif

#define UP_ARROW   "\xe2\x96\xb2"
#define DOWN_ARROW "\xe2\x96\xbc"

/* ── Helpers ──────────────────────────────────────────────────────── */

static gchar *
fixture(const gchar *name)
{
	g_autofree gchar *path = NULL;
	gchar *text = NULL;

	path = g_build_filename(GOWL_TEST_FIXTURES, "wow-gold", name, NULL);
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	return text;
}

static BarWowgoldData *
parse_fixture(gboolean (*parse)(BarWowgoldData *, const gchar *),
              const gchar *name)
{
	BarWowgoldData *d = bar_wowgold_data_new();
	g_autofree gchar *json = fixture(name);

	g_assert_true(parse(d, json));
	return d;
}

static void
assert_money(gint64 copper, BarWowgoldMoneyFormat fmt, const gchar *want)
{
	g_autofree gchar *got = bar_wowgold_format_money(copper, fmt);

	g_assert_cmpstr(got, ==, want);
}

static void
assert_compact(gint64 copper, const gchar *want)
{
	g_autofree gchar *got = bar_wowgold_format_compact(copper);

	g_assert_cmpstr(got, ==, want);
}

static gint64
parse_ok(const gchar *text)
{
	gint64 v = -42;

	g_assert_true(bar_wowgold_parse_money(text, &v));
	return v;
}

static gchar *
joined(gchar **argv)
{
	return g_strjoinv("|", argv);
}

/* ── Money ────────────────────────────────────────────────────────── */

/* Catches: silver/copper leaking into the whole-gold form, missing or
   locale-dependent separators, "0g" for a sub-gold amount (postage
   would read as free), and the G_MININT64 negation overflow. */
static void
test_money_format(void)
{
	g_autofree gchar *min = NULL;

	assert_money(0, BAR_WOWGOLD_MONEY_G, "0g");
	assert_money(0, BAR_WOWGOLD_MONEY_GSC, "0c");
	assert_money(34901458374, BAR_WOWGOLD_MONEY_G, "3,490,145g");
	assert_money(34901458374, BAR_WOWGOLD_MONEY_GSC, "3,490,145g 83s 74c");
	assert_money(12340000, BAR_WOWGOLD_MONEY_G, "1,234g");
	assert_money(10000, BAR_WOWGOLD_MONEY_G, "1g");
	assert_money(9999, BAR_WOWGOLD_MONEY_G, "99s 99c");
	assert_money(5000, BAR_WOWGOLD_MONEY_G, "50s");
	assert_money(26, BAR_WOWGOLD_MONEY_G, "26c");
	assert_money(10005, BAR_WOWGOLD_MONEY_GSC, "1g 5c");
	assert_money(-80000000, BAR_WOWGOLD_MONEY_G, "-8,000g");
	assert_money(-150, BAR_WOWGOLD_MONEY_GSC, "-1s 50c");
	assert_money(1000000000000, BAR_WOWGOLD_MONEY_G, "100,000,000g");

	min = bar_wowgold_format_money(G_MININT64, BAR_WOWGOLD_MONEY_G);
	g_assert_true(g_str_has_prefix(min, "-922,337,203,685,477g"));
}

/* Catches: a nine-digit total in the bar, "1000Kg" at a unit boundary,
   and a decimal comma under a European locale. */
static void
test_money_compact(void)
{
	assert_compact(0, "0g");
	assert_compact(99990000, "9,999g");
	assert_compact(100000000, "10.0Kg");
	assert_compact(123456789, "12.3Kg");
	assert_compact(34901458374, "3.49Mg");
	assert_compact(356420000000, "35.6Mg");
	assert_compact(9999990000, "1.00Mg");     /* 999,999g */
	assert_compact(9995000000000, "1.00Bg");  /* 999.5M rounds up */
	assert_compact(-12000000, "-1,200g");
	assert_compact(-12000000000, "-1.20Mg");
	assert_compact(G_MAXINT64, "922Tg");
}

/* Catches: a gain without its `+' reading as a balance. */
static void
test_money_delta(void)
{
	g_autofree gchar *up = NULL;
	g_autofree gchar *down = NULL;
	g_autofree gchar *zero = NULL;
	g_autofree gchar *compact = NULL;

	up      = bar_wowgold_format_delta(1500000000, BAR_WOWGOLD_MONEY_G, FALSE);
	down    = bar_wowgold_format_delta(-800000000, BAR_WOWGOLD_MONEY_G, FALSE);
	zero    = bar_wowgold_format_delta(0, BAR_WOWGOLD_MONEY_G, FALSE);
	compact = bar_wowgold_format_delta(31000000000, BAR_WOWGOLD_MONEY_G, TRUE);
	g_assert_cmpstr(up, ==, "+150,000g");
	g_assert_cmpstr(down, ==, "-80,000g");
	g_assert_cmpstr(zero, ==, "0g");
	g_assert_cmpstr(compact, ==, "+3.10Mg");
}

/* Catches: a formatted string from a newer tsmctl being read as zero,
   and partial reads ("12g junk" = 12g) presented as the amount. */
static void
test_money_parse(void)
{
	gint64 v = 0;

	g_assert_cmpint(parse_ok("123g 45s 6c"), ==, 1234506);
	g_assert_cmpint(parse_ok("1,234g"), ==, 12340000);
	g_assert_cmpint(parse_ok("1.5g"), ==, 15000);
	g_assert_cmpint(parse_ok("  -5g "), ==, -50000);
	g_assert_cmpint(parse_ok("12s"), ==, 1200);
	g_assert_cmpint(parse_ok("7c"), ==, 7);
	g_assert_cmpint(parse_ok("12345"), ==, 12345);
	g_assert_cmpint(parse_ok("1,234g 5s"), ==, 12340500);
	g_assert_cmpint(parse_ok("2G 3S"), ==, 20300);

	g_assert_false(bar_wowgold_parse_money(NULL, &v));
	g_assert_false(bar_wowgold_parse_money("", &v));
	g_assert_false(bar_wowgold_parse_money("g", &v));
	g_assert_false(bar_wowgold_parse_money("12x", &v));
	g_assert_false(bar_wowgold_parse_money("12g junk", &v));
	g_assert_false(bar_wowgold_parse_money("5 6g", &v));
	g_assert_false(bar_wowgold_parse_money("1.5s", &v));
	g_assert_false(bar_wowgold_parse_money("--5g", &v));
	g_assert_false(bar_wowgold_parse_money("99999999999999999999g", &v));
	g_assert_false(bar_wowgold_parse_money("922337203685477g 99999s", &v));
}

/* ── JSON ─────────────────────────────────────────────────────────── */

/* Catches the bug class gowl-bar-json.h exists to prevent: a field read
   out of a NESTED object, or out of text inside a string, when the
   object's own member is absent or later. */
static void
test_json_member(void)
{
	const gchar *obj =
		"{ \"note\": \"\\\"gold_copper\\\": 5\", "
		"\"nested\": {\"gold_copper\": 777, \"x\": [1, {\"y\": 2}]}, "
		"\"list\": [\"}\", \"]\"], "
		"\"gold_copper\" : 42 }";
	g_autofree gchar *v = NULL;
	g_autofree gchar *nested = NULL;
	g_autofree gchar *list = NULL;
	gint64 n = 0;

	v = bar_wowgold_json_member(obj, "gold_copper");
	g_assert_cmpstr(v, ==, "42");
	nested = bar_wowgold_json_member(obj, "nested");
	g_assert_true(g_str_has_prefix(nested, "{\"gold_copper\": 777"));
	g_assert_true(g_str_has_suffix(nested, "}]}"));
	list = bar_wowgold_json_member(obj, "list");
	g_assert_cmpstr(list, ==, "[\"}\", \"]\"]");

	/* Absent here, present only nested: not found. */
	g_assert_false(bar_wowgold_json_int(
		"{\"a\": {\"gold_copper\": 9}}", "gold_copper", &n));
	g_assert_null(bar_wowgold_json_member("{\"y\": 2}", "x"));

	/* Not an object, unterminated, truncated. */
	g_assert_null(bar_wowgold_json_member("[1, 2]", "x"));
	g_assert_null(bar_wowgold_json_member("{\"x\": \"open", "x"));
	g_assert_null(bar_wowgold_json_member("{\"a\": 1, \"x\"", "x"));
	g_assert_null(bar_wowgold_json_member(NULL, "x"));
	g_assert_null(bar_wowgold_json_member("", "x"));
}

/* Catches: null read as 0, an overflow wrapping to a negative balance,
   a float or a money string silently dropped. */
static void
test_json_int(void)
{
	gint64 v = 0;

	g_assert_true(bar_wowgold_json_int("{\"a\": -12}", "a", &v));
	g_assert_cmpint(v, ==, -12);
	g_assert_true(bar_wowgold_json_int("{\"a\": 9223372036854775807}",
	                                   "a", &v));
	g_assert_cmpint(v, ==, G_MAXINT64);
	g_assert_true(bar_wowgold_json_int("{\"a\": 12.6}", "a", &v));
	g_assert_cmpint(v, ==, 13);
	g_assert_true(bar_wowgold_json_int("{\"a\": 1e4}", "a", &v));
	g_assert_cmpint(v, ==, 10000);
	g_assert_true(bar_wowgold_json_int("{\"a\": \"1,234g\"}", "a", &v));
	g_assert_cmpint(v, ==, 12340000);

	g_assert_false(bar_wowgold_json_int("{\"a\": null}", "a", &v));
	g_assert_false(bar_wowgold_json_int("{\"a\": 99999999999999999999}",
	                                    "a", &v));
	g_assert_false(bar_wowgold_json_int("{\"a\": 1e300}", "a", &v));
	g_assert_false(bar_wowgold_json_int("{\"a\": true}", "a", &v));
	g_assert_false(bar_wowgold_json_int("{\"a\": \"lots\"}", "a", &v));
	g_assert_false(bar_wowgold_json_int("{\"b\": 1}", "a", &v));
}

/* Catches: invalid UTF-8 reaching pango, a newline splitting a panel
   row, \u0000 truncating a name, surrogate pairs mangled, and an
   unbounded string. */
static void
test_json_decode_hostile(void)
{
	g_autofree gchar *pair = NULL;
	g_autofree gchar *lone = NULL;
	g_autofree gchar *ctrl = NULL;
	g_autofree gchar *bytes = NULL;
	g_autofree gchar *longtok = NULL;
	g_autofree gchar *longout = NULL;
	GString *big;
	guint i;

	pair = bar_wowgold_json_decode("\"R\\u00e9alm \\ud83d\\ude00\"");
	g_assert_cmpstr(pair, ==, "R\xc3\xa9" "alm \xf0\x9f\x98\x80");
	lone = bar_wowgold_json_decode("\"a\\ud800b\\u0000c\"");
	g_assert_cmpstr(lone, ==, "a\xef\xbf\xbd" "b\xef\xbf\xbd" "c");
	ctrl = bar_wowgold_json_decode("\"line1\\nline2\\t\\\"q\\\"\"");
	g_assert_cmpstr(ctrl, ==, "line1 line2 \"q\"");
	bytes = bar_wowgold_json_decode("\"Bad\xff\xfeName\"");
	g_assert_true(g_utf8_validate(bytes, -1, NULL));
	g_assert_true(g_str_has_prefix(bytes, "Bad"));
	g_assert_true(g_str_has_suffix(bytes, "Name"));
	g_assert_null(bar_wowgold_json_decode("12"));
	g_assert_null(bar_wowgold_json_decode(NULL));

	big = g_string_new("\"");
	for (i = 0; i < 5000; i++)
		g_string_append_c(big, 'x');
	g_string_append_c(big, '"');
	longtok = g_string_free(big, FALSE);
	longout = bar_wowgold_json_decode(longtok);
	g_assert_cmpint(g_utf8_strlen(longout, -1), ==,
	                BAR_WOWGOLD_TEXT_MAX + 1);
}

/* ── Documents ────────────────────────────────────────────────────── */

/* Catches: the TOTAL row ignored or double-counted, characters out of
   order, an empty or total-less table, and a traceback on stdout
   taken for an empty account. */
static void
test_parse_gold(void)
{
	BarWowgoldData *d;
	BarWowgoldChar *c;
	g_autofree gchar *junk = NULL;

	d = parse_fixture(bar_wowgold_parse_gold, "gold.json");
	g_assert_true(d->have_total);
	g_assert_cmpint(d->total, ==, 34901458374);
	g_assert_cmpuint(d->chars->len, ==, 9);
	c = g_ptr_array_index(d->chars, 0);
	g_assert_cmpstr(c->name, ==, "Goldmonger");
	g_assert_cmpstr(c->realm, ==, "Thorium Brotherhood");
	g_assert_cmpint(c->copper, ==, 27688336070);
	c = g_ptr_array_index(d->chars, 3);
	g_assert_cmpstr(c->name, ==, "Hoard\xc3\xa9");
	c = g_ptr_array_index(d->chars, 4);
	g_assert_cmpstr(c->kind, ==, "warbank");
	g_assert_cmpstr(c->realm, ==, "");
	bar_wowgold_data_free(d);

	d = parse_fixture(bar_wowgold_parse_gold, "gold-empty.json");
	g_assert_true(d->have_total);
	g_assert_cmpint(d->total, ==, 0);
	g_assert_cmpuint(d->chars->len, ==, 0);
	bar_wowgold_data_free(d);

	/* No totals row: the sum, and the null row skipped not zeroed. */
	d = parse_fixture(bar_wowgold_parse_gold, "gold-no-totals.json");
	g_assert_cmpint(d->total, ==, 26500000);
	g_assert_cmpuint(d->chars->len, ==, 2);
	c = g_ptr_array_index(d->chars, 0);
	g_assert_cmpstr(c->name, ==, "Bigfish");
	bar_wowgold_data_free(d);

	/* Overflow skipped, two maxima saturate instead of wrapping, a
	   float and a money string both read. */
	d = parse_fixture(bar_wowgold_parse_gold, "gold-huge.json");
	g_assert_cmpuint(d->chars->len, ==, 4);
	g_assert_cmpint(d->total, ==, G_MAXINT64);
	c = g_ptr_array_index(d->chars, 3);
	g_assert_cmpstr(c->name, ==, "Float");
	g_assert_cmpint(c->copper, ==, 12500);
	c = g_ptr_array_index(d->chars, 2);
	g_assert_cmpstr(c->name, ==, "Formatted");
	g_assert_cmpint(c->copper, ==, 12340500);
	bar_wowgold_data_free(d);

	d = bar_wowgold_data_new();
	junk = fixture("not-json.txt");
	g_assert_false(bar_wowgold_parse_gold(d, junk));
	g_assert_false(bar_wowgold_parse_gold(d, ""));
	g_assert_false(bar_wowgold_parse_gold(d, "{\"sections\": 3}"));
	g_assert_false(d->have_total);
	bar_wowgold_data_free(d);
}

/* Catches: decoys inside strings and nested objects, invalid UTF-8 and
   control characters from a corrupt SavedVariables name. */
static void
test_parse_gold_hostile(void)
{
	BarWowgoldData *d;
	BarWowgoldChar *c;

	d = parse_fixture(bar_wowgold_parse_gold, "gold-hostile.json");
	g_assert_cmpint(d->total, ==, 12350000);
	g_assert_cmpuint(d->chars->len, ==, 2);
	c = g_ptr_array_index(d->chars, 0);
	g_assert_cmpint(c->copper, ==, 12340000);   /* not 777, not 555 */
	g_assert_true(g_utf8_validate(c->name, -1, NULL));
	g_assert_null(strchr(c->name, '\n'));
	g_assert_null(strchr(c->name, '\t'));
	g_assert_true(g_str_has_prefix(c->name, "Bad"));
	g_assert_cmpstr(c->realm, ==, "R\xc3\xa9" "alm \xf0\x9f\x98\x80");
	c = g_ptr_array_index(d->chars, 1);
	g_assert_cmpstr(c->name, ==, "LoneSurrogate\xef\xbf\xbdx");
	bar_wowgold_data_free(d);
}

/* Catches: the "now" row duplicating today, a bad row zeroing a day,
   and rows out of order. */
static void
test_parse_history(void)
{
	BarWowgoldData *d;
	BarWowgoldPoint *p;

	d = parse_fixture(bar_wowgold_parse_history, "history.json");
	/* 23 rows, 09-24 .. 10-16; the "now" row is today's. */
	g_assert_cmpuint(d->points->len, ==, 23);
	p = &g_array_index(d->points, BarWowgoldPoint, 0);
	g_assert_cmpuint(p->day, ==, bar_wowgold_julian("2026-09-24"));
	g_assert_cmpint(p->copper, ==, 32550000000);
	p = &g_array_index(d->points, BarWowgoldPoint, 22);
	g_assert_cmpuint(p->day, ==, bar_wowgold_julian("2026-10-16"));
	bar_wowgold_data_free(d);

	d = parse_fixture(bar_wowgold_parse_history, "history-gaps.json");
	g_assert_cmpuint(d->points->len, ==, 4);
	p = &g_array_index(d->points, BarWowgoldPoint, 1);
	g_assert_cmpuint(p->day, ==, bar_wowgold_julian("2026-10-03"));
	g_assert_cmpint(p->copper, ==, 1600000);    /* the later row wins */
	bar_wowgold_data_free(d);
}

/* Catches: sales sorted as strings across a DST change, an unparseable
   time put first, a missing item name shown blank. */
static void
test_parse_sales(void)
{
	BarWowgoldData *d;
	BarWowgoldSale *s;

	d = parse_fixture(bar_wowgold_parse_sales, "sales.json");
	g_assert_cmpuint(d->sales->len, ==, 6);
	g_assert_cmpint(d->sales_listed, ==, 1867);
	s = g_ptr_array_index(d->sales, 0);
	g_assert_cmpstr(s->item, ==, "WoW Token");
	g_assert_cmpint(s->qty, ==, 2);
	g_assert_cmpint(s->total, ==, 5811000000);
	g_assert_cmpstr(s->character, ==, "Goldmonger");
	g_assert_cmpstr(s->source, ==, "Auction");
	g_assert_cmpint(s->when, >, 0);
	bar_wowgold_data_free(d);

	/* 01:45-04:00 is 05:45Z; 01:30-05:00 is 06:30Z.  As text the
	   first sorts later; in time it is earlier. */
	d = parse_fixture(bar_wowgold_parse_sales, "sales-dst.json");
	g_assert_cmpuint(d->sales->len, ==, 3);
	s = g_ptr_array_index(d->sales, 0);
	g_assert_cmpstr(s->item, ==, "After the change");
	s = g_ptr_array_index(d->sales, 1);
	g_assert_cmpstr(s->item, ==, "Before the change");
	s = g_ptr_array_index(d->sales, 2);
	g_assert_cmpstr(s->item, ==, "i:3");
	g_assert_cmpint(s->when, ==, 0);
	g_assert_cmpint(s->qty, ==, 1);
	g_assert_cmpint(s->total, ==, 50);
	bar_wowgold_data_free(d);
}

/* Catches: P&L lines read from the wrong section, and a day with no
   trading reported as no data. */
static void
test_parse_pnl(void)
{
	BarWowgoldData *d;

	d = parse_fixture(bar_wowgold_parse_pnl, "pnl.json");
	g_assert_true(d->have_pnl);
	g_assert_cmpint(d->pnl.revenue, ==, 12534500);
	g_assert_cmpint(d->pnl.purchases, ==, 2000000);
	g_assert_cmpint(d->pnl.expenses, ==, 150000);
	g_assert_cmpint(d->pnl.net, ==, 10384500);
	g_assert_cmpint(d->pnl.units, ==, 7);
	bar_wowgold_data_free(d);

	/* The gold document has no P&L block: parsed, but nothing. */
	d = parse_fixture(bar_wowgold_parse_pnl, "gold.json");
	g_assert_false(d->have_pnl);
	g_assert_false(bar_wowgold_parse_pnl(d, "nope"));
	bar_wowgold_data_free(d);
}

static void
test_parse_attention(void)
{
	BarWowgoldData *d;
	BarWowgoldAttention *a;

	d = parse_fixture(bar_wowgold_parse_attention, "attention.json");
	g_assert_true(d->have_attention);
	g_assert_cmpuint(d->attention->len, ==, 7);
	a = g_ptr_array_index(d->attention, 0);
	g_assert_cmpstr(a->what, ==, "auctions expired, collect");
	g_assert_cmpstr(a->character, ==, "Crafter");
	g_assert_cmpint(a->count, ==, 4);
	g_assert_cmpint(a->value, ==, 1992725);
	a = g_ptr_array_index(d->attention, 6);
	g_assert_cmpstr(a->what, ==, "mail waiting");
	bar_wowgold_data_free(d);
}

/* ── Days and trends ──────────────────────────────────────────────── */

/* Catches: a missing day plotted as zero, the window's leading days
   invented, a future point (skewed clock) leaking into today. */
static void
test_series_gaps(void)
{
	BarWowgoldData *d;
	gint64 values[10];
	gboolean known[10];
	guint32 today;
	guint n;

	d = parse_fixture(bar_wowgold_parse_history, "history-gaps.json");
	today = bar_wowgold_julian("2026-10-08");
	/* 09-29 .. 10-08 */
	n = bar_wowgold_daily_series(
		(const BarWowgoldPoint *)d->points->data, d->points->len,
		today, 10, values, known);
	g_assert_cmpuint(n, ==, 8);
	g_assert_false(known[0]);            /* 09-29 */
	g_assert_false(known[1]);            /* 09-30 */
	g_assert_true(known[2]);             /* 10-01 */
	g_assert_cmpint(values[2], ==, 1000000);
	g_assert_cmpint(values[3], ==, 1000000);  /* 10-02 carried */
	g_assert_cmpint(values[4], ==, 1600000);  /* 10-03 */
	g_assert_cmpint(values[6], ==, 1600000);  /* 10-05: null row */
	g_assert_cmpint(values[8], ==, 1200000);  /* 10-07 */
	g_assert_cmpint(values[9], ==, 1200000);  /* 10-08 carried */
	/* 10-12 is after "today" and must not appear. */
	bar_wowgold_data_free(d);

	g_assert_cmpuint(bar_wowgold_daily_series(NULL, 0, today, 10,
	                                          values, known), ==, 0);
	g_assert_false(known[9]);
}

/* Catches: deltas taken against an unknown day, a flat series dividing
   by zero, and samples outside 0..1. */
static void
test_trend(void)
{
	BarWowgoldData *d;
	BarWowgoldTrend t;
	gint64 v[4] = { 5, 5, 5, 5 };
	gboolean k[4] = { FALSE, TRUE, TRUE, TRUE };
	gdouble out[4];
	guint i;

	d = parse_fixture(bar_wowgold_parse_history, "history.json");
	bar_wowgold_trend(d, bar_wowgold_julian("2026-10-16"), 30, &t);
	g_assert_cmpuint(t.n_samples, ==, 30);
	g_assert_true(t.have_today);
	g_assert_cmpint(t.today, ==, 35650000000);
	g_assert_true(t.have_day);
	g_assert_cmpint(t.day_delta, ==, 0);
	g_assert_true(t.have_week);
	g_assert_cmpint(t.week_delta, ==, 0);
	g_assert_true(t.have_period);
	g_assert_cmpint(t.period_delta, ==, 35650000000 - 32550000000);
	for (i = 0; i < t.n_samples; i++) {
		g_assert_cmpfloat(t.samples[i], >=, 0.0);
		g_assert_cmpfloat(t.samples[i], <=, 1.0);
	}
	g_assert_cmpint(bar_wowgold_direction(&t), ==, BAR_WOWGOLD_FLAT);
	bar_wowgold_trend_clear(&t);
	g_assert_null(t.samples);

	/* A day with no point at all before it: no delta, no week. */
	bar_wowgold_trend(d, bar_wowgold_julian("2026-09-24"), 30, &t);
	g_assert_true(t.have_today);
	g_assert_false(t.have_day);
	g_assert_false(t.have_week);
	bar_wowgold_trend_clear(&t);
	bar_wowgold_data_free(d);

	/* Gaps: today 10-08 carries 10-07; yesterday carries too, so the
	   day is flat; seven days back is 10-01. */
	d = parse_fixture(bar_wowgold_parse_history, "history-gaps.json");
	bar_wowgold_trend(d, bar_wowgold_julian("2026-10-08"), 10, &t);
	g_assert_cmpint(t.day_delta, ==, 0);
	g_assert_true(t.have_week);
	g_assert_cmpint(t.week_delta, ==, 1200000 - 1000000);
	bar_wowgold_trend(d, bar_wowgold_julian("2026-10-07"), 10, &t);
	g_assert_cmpint(t.day_delta, ==, 1200000 - 1600000);
	g_assert_cmpint(bar_wowgold_direction(&t), ==, BAR_WOWGOLD_DOWN);
	bar_wowgold_trend_clear(&t);
	bar_wowgold_data_free(d);

	/* No history: nothing known, nothing allocated. */
	d = bar_wowgold_data_new();
	bar_wowgold_trend(d, bar_wowgold_julian("2026-10-07"), 10, &t);
	g_assert_cmpuint(t.n_samples, ==, 0);
	g_assert_false(t.have_today);
	bar_wowgold_data_free(d);

	bar_wowgold_normalise(v, k, 4, out);
	for (i = 0; i < 4; i++)
		g_assert_cmpfloat(out[i], ==, 0.5);
}

static void
test_period_days(void)
{
	g_assert_cmpuint(bar_wowgold_period_days(NULL), ==, 30);
	g_assert_cmpuint(bar_wowgold_period_days("30d"), ==, 30);
	g_assert_cmpuint(bar_wowgold_period_days("4w"), ==, 28);
	g_assert_cmpuint(bar_wowgold_period_days("3m"), ==, 90);
	g_assert_cmpuint(bar_wowgold_period_days("1y"), ==, 365);
	g_assert_cmpuint(bar_wowgold_period_days("14"), ==, 14);
	g_assert_cmpuint(bar_wowgold_period_days("1d"), ==, 2);
	g_assert_cmpuint(bar_wowgold_period_days("5y"), ==, 366);
	g_assert_cmpuint(bar_wowgold_period_days("month"), ==, 30);
	g_assert_cmpuint(bar_wowgold_period_days("2026-01-01"), ==, 30);
	g_assert_cmpuint(bar_wowgold_period_days("-3d"), ==, 30);
}

/* Catches: an arrow on a flat day, the wrong arrow, and the delta
   printed with a second sign beside the arrow. */
static void
test_label(void)
{
	BarWowgoldData *d;
	BarWowgoldTrend t;
	gchar *l;

	d = parse_fixture(bar_wowgold_parse_gold, "gold.json");
	memset(&t, 0, sizeof(t));

	l = bar_wowgold_label(d, &t, TRUE, FALSE, BAR_WOWGOLD_MONEY_G);
	g_assert_cmpstr(l, ==, "3.49Mg");
	g_free(l);

	t.have_day = TRUE;
	t.day_delta = 1500000000;
	l = bar_wowgold_label(d, &t, TRUE, FALSE, BAR_WOWGOLD_MONEY_G);
	g_assert_cmpstr(l, ==, "3.49Mg " UP_ARROW);
	g_free(l);
	l = bar_wowgold_label(d, &t, TRUE, TRUE, BAR_WOWGOLD_MONEY_G);
	g_assert_cmpstr(l, ==, "3.49Mg " UP_ARROW "150Kg");
	g_free(l);

	t.day_delta = -80000000;
	l = bar_wowgold_label(d, &t, FALSE, TRUE, BAR_WOWGOLD_MONEY_G);
	g_assert_cmpstr(l, ==, "3,490,145g " DOWN_ARROW "8,000g");
	g_free(l);

	g_assert_null(bar_wowgold_label(NULL, &t, TRUE, FALSE,
	                                BAR_WOWGOLD_MONEY_G));
	bar_wowgold_data_free(d);
}

/* Catches: a pasted summary with relative times, or missing the sale. */
static void
test_summary_and_tooltip(void)
{
	BarWowgoldData *d;
	BarWowgoldTrend t;
	g_autofree gchar *json = NULL;
	g_autofree gchar *sum = NULL;
	g_autofree gchar *tip = NULL;
	BarWowgoldSale *s;

	d = parse_fixture(bar_wowgold_parse_gold, "gold.json");
	json = fixture("sales.json");
	g_assert_true(bar_wowgold_parse_sales(d, json));
	memset(&t, 0, sizeof(t));
	t.have_day = TRUE;
	t.day_delta = 1500000000;
	t.have_week = TRUE;
	t.week_delta = -10000;
	t.n_samples = 1;

	sum = bar_wowgold_summary(d, &t, BAR_WOWGOLD_MONEY_G);
	g_assert_cmpstr(sum, ==, "WoW gold: 3,490,145g (today +150,000g, "
	                "7 days -1g); last sale WoW Token \xc3\x97" "2 for "
	                "581,100g");

	s = g_ptr_array_index(d->sales, 0);
	tip = bar_wowgold_tooltip(d, &t, BAR_WOWGOLD_MONEY_G, s->when + 7200);
	g_assert_cmpstr(tip, ==, "WoW gold: 3,490,145g\nToday: +150,000g\n"
	                "Last sale: WoW Token \xc3\x97" "2 for 581,100g, "
	                "2h ago");
	bar_wowgold_data_free(d);
}

static void
test_format_ago(void)
{
	gchar *s;

	s = bar_wowgold_format_ago(0, 1000);
	g_assert_cmpstr(s, ==, "");
	g_free(s);
	s = bar_wowgold_format_ago(1000, 1030);
	g_assert_cmpstr(s, ==, "just now");
	g_free(s);
	s = bar_wowgold_format_ago(1000, 1000 + 300);
	g_assert_cmpstr(s, ==, "5m ago");
	g_free(s);
	s = bar_wowgold_format_ago(1000, 1000 + 3 * 3600);
	g_assert_cmpstr(s, ==, "3h ago");
	g_free(s);
	s = bar_wowgold_format_ago(1000, 1000 + 2 * 86400);
	g_assert_cmpstr(s, ==, "2d ago");
	g_free(s);
	s = bar_wowgold_format_ago(1000, 1000 + 90 * 86400);
	g_assert_cmpuint(strlen(s), ==, 10);   /* a YYYY-MM-DD date */
	g_free(s);
}

/* ── argv ─────────────────────────────────────────────────────────── */

static gchar *
argv_for(const BarWowgoldArgs *a, BarWowgoldCommand cmd)
{
	g_auto(GStrv) argv = NULL;
	GError *error = NULL;

	argv = bar_wowgold_build_argv(a, cmd, &error);
	g_assert_no_error(error);
	g_assert_nonnull(argv);
	return joined(argv);
}

/* Catches: filters dropped or merged, a bare `--host VALUE' (which
   argparse would take as an option when VALUE starts with `-'),
   --offline sent with --local, and the report asked for JSON. */
static void
test_argv_exact(void)
{
	BarWowgoldArgs a;
	gchar *s;

	memset(&a, 0, sizeof(a));
	a.sales_count = 10;

	/* Defaults: tsmctl's own config decides the source. */
	s = argv_for(&a, BAR_WOWGOLD_CMD_GOLD);
	g_assert_cmpstr(s, ==, "tsmctl|--quiet|--color=never|--output=json|"
	                "get|gold");
	g_free(s);

	a.offline = TRUE;
	s = argv_for(&a, BAR_WOWGOLD_CMD_HISTORY);
	g_assert_cmpstr(s, ==, "tsmctl|--offline|--quiet|--color=never|"
	                "--output=json|get|gold-history|--by=day|--since=30d");
	g_free(s);

	a.host = "wow-pc";
	a.flavor = "_retail_";
	a.account = "MAIN";
	a.realm = "Thorium Brotherhood, Moon Guard,,";
	a.character = "Goldmonger";
	a.period = "4w";
	s = argv_for(&a, BAR_WOWGOLD_CMD_HISTORY);
	g_assert_cmpstr(s, ==, "tsmctl|--host=wow-pc|--offline|"
	                "--flavor=_retail_|--account=MAIN|"
	                "--realm=Thorium Brotherhood|--realm=Moon Guard|"
	                "--char=Goldmonger|--quiet|--color=never|"
	                "--output=json|get|gold-history|--by=day|--since=4w");
	g_free(s);

	/* local: --local, and no --offline (it means nothing there). */
	a.host = "LOCAL";
	a.flavor = NULL;
	a.account = NULL;
	a.realm = NULL;
	a.character = NULL;
	s = argv_for(&a, BAR_WOWGOLD_CMD_SALES);
	g_assert_cmpstr(s, ==, "tsmctl|--local|--quiet|--color=never|"
	                "--output=json|get|sales|--limit=10");
	g_free(s);

	a.sales_count = 5000;
	s = argv_for(&a, BAR_WOWGOLD_CMD_SALES);
	g_assert_true(g_str_has_suffix(s, "|--limit=100"));
	g_free(s);

	s = argv_for(&a, BAR_WOWGOLD_CMD_PNL);
	g_assert_true(g_str_has_suffix(s, "|--output=json|pnl|--period=today"));
	g_free(s);
	s = argv_for(&a, BAR_WOWGOLD_CMD_ATTENTION);
	g_assert_true(g_str_has_suffix(s, "|--output=json|attention"));
	g_free(s);
	s = argv_for(&a, BAR_WOWGOLD_CMD_REPORT);
	g_assert_cmpstr(s, ==, "tsmctl|--local|--quiet|--color=always|report");
	g_free(s);

	/* The command setting is split like a shell would, ~ expanded,
	   never handed to one. */
	a.command = "python3 '~/My Scripts/tsmctl'";
	a.host = NULL;
	s = argv_for(&a, BAR_WOWGOLD_CMD_ATTENTION);
	g_assert_true(g_str_has_prefix(s, "python3|~/My Scripts/tsmctl|"));
	g_free(s);
	a.command = "~/bin/tsmctl";
	s = argv_for(&a, BAR_WOWGOLD_CMD_ATTENTION);
	g_assert_true(g_str_has_prefix(s, g_get_home_dir()));
	g_free(s);
}

/* Catches: an ssh option smuggled in as the host, control characters
   in a filter, and a leading `-' in a filter turning into a flag. */
static void
test_argv_injection(void)
{
	BarWowgoldArgs a;
	g_auto(GStrv) argv = NULL;
	GError *error = NULL;
	gchar *s;
	guint i;

	memset(&a, 0, sizeof(a));
	a.sales_count = 10;

	a.host = "-oProxyCommand=touch /tmp/pwned";
	g_assert_null(bar_wowgold_build_argv(&a, BAR_WOWGOLD_CMD_GOLD, &error));
	g_assert_error(error, BAR_WOWGOLD_ERROR, 0);
	g_clear_error(&error);

	a.host = "wow pc";
	g_assert_null(bar_wowgold_build_argv(&a, BAR_WOWGOLD_CMD_GOLD, &error));
	g_clear_error(&error);
	a.host = "wow;reboot";
	g_assert_null(bar_wowgold_build_argv(&a, BAR_WOWGOLD_CMD_GOLD, &error));
	g_clear_error(&error);
	a.host = "zach@[fe80::1%eth0]";
	g_assert_true(bar_wowgold_host_ok(a.host));

	a.host = NULL;
	a.realm = "Bad\nRealm";
	g_assert_null(bar_wowgold_build_argv(&a, BAR_WOWGOLD_CMD_GOLD, &error));
	g_assert_nonnull(strstr(error->message, "realm"));
	g_clear_error(&error);

	a.realm = NULL;
	a.command = "tsmctl 'unterminated";
	g_assert_null(bar_wowgold_build_argv(&a, BAR_WOWGOLD_CMD_GOLD, &error));
	g_assert_nonnull(strstr(error->message, "tsmctl-command"));
	g_clear_error(&error);

	/* Shell syntax and a leading dash stay ONE word of data. */
	a.command = NULL;
	a.realm = "-x; touch /tmp/pwned";
	a.character = "$(reboot)";
	argv = bar_wowgold_build_argv(&a, BAR_WOWGOLD_CMD_GOLD, &error);
	g_assert_no_error(error);
	s = joined(argv);
	g_assert_nonnull(strstr(s, "|--realm=-x; touch /tmp/pwned|"));
	g_assert_nonnull(strstr(s, "|--char=$(reboot)|"));
	for (i = 0; argv[i] != NULL; i++) {
		g_assert_cmpstr(argv[i], !=, "sh");
		g_assert_cmpstr(argv[i], !=, "-c");
	}
	g_free(s);
}

/* ── The widget ───────────────────────────────────────────────────── */

/*
 * A stand-in tsmctl.  It logs every call (args joined with `|'), and
 * answers from the fixtures --- except gold-history, which it writes
 * relative to TODAY, since the widget measures against the real date.
 * Files in the scratch dir switch failure modes on and off.
 *
 * History: 8 days ago 800g, 3 days ago 1,000g, yesterday 1,200g, today
 * 1,500g (or 900g with `down', 1,200g with `flat').
 */
static gchar *
fake_tsmctl(BarHarness *h)
{
	g_autofree gchar *body = NULL;

	body = g_strdup_printf(
		"S='%s'\nF='%s/wow-gold'\n"
		"line=\"$0\"; for a in \"$@\"; do line=\"$line|$a\"; done\n"
		"printf '%%s\\n' \"$line\" >> \"$S/calls.log\"\n"
		"offline=0; cmd=''\n"
		"for a in \"$@\"; do case \"$a\" in\n"
		"  --offline) offline=1 ;;\n"
		"  gold|gold-history|sales|pnl|attention|report)"
		" [ -z \"$cmd\" ] && cmd=$a ;;\n"
		"esac; done\n"
		"[ -e \"$S/hang\" ] && exec sleep 30\n"
		"if [ -e \"$S/fail-all\" ]; then\n"
		"  echo 'tsmctl: warning: noise' >&2\n"
		"  echo 'tsmctl: error: the cache is corrupt' >&2; exit 1; fi\n"
		"if [ -e \"$S/fail-online\" ] && [ $offline = 0 ]; then\n"
		"  echo 'tsmctl: error: ssh: connect to host wow-pc: No route"
		" to host' >&2; exit 3; fi\n"
		"if [ -e \"$S/junk\" ]; then cat \"$F/not-json.txt\"; exit 0; fi\n"
		"case $cmd in\n"
		"  gold) cat \"$F/gold.json\" ;;\n"
		"  sales) cat \"$F/sales.json\" ;;\n"
		"  pnl) cat \"$F/pnl.json\" ;;\n"
		"  attention) [ -e \"$S/no-attention\" ] && { echo 'usage: "
		"invalid choice' >&2; exit 2; }; cat \"$F/attention.json\" ;;\n"
		"  gold-history)\n"
		"    T=15000000; [ -e \"$S/down\" ] && T=9000000\n"
		"    [ -e \"$S/flat\" ] && T=12000000\n"
		"    d() { date -d \"$1 days ago\" +%%Y-%%m-%%dT23:59:59-04:00; }\n"
		"    printf '{\"title\": \"Gold history\", \"sections\": [{\"title\": "
		"\"h\", \"values\": {}, \"tables\": [{\"title\": \"\", \"rows\": "
		"[{\"date\": \"%%s\", \"gold_copper\": 8000000}, {\"date\": \"%%s\", "
		"\"gold_copper\": 10000000}, {\"date\": \"%%s\", \"gold_copper\": "
		"12000000}, {\"date\": \"%%s\", \"gold_copper\": %%s}]}], "
		"\"notes\": []}]}\\n' \"$(d 8)\" \"$(d 3)\" \"$(d 1)\" \"$(d 0)\""
		" \"$T\" ;;\n"
		"  *) echo \"unexpected: $*\" >&2; exit 2 ;;\n"
		"esac\n",
		h->tmp, GOWL_TEST_FIXTURES);
	return bar_harness_script(h, "tsmctl", body);
}

static void
touch_flag(BarHarness *h, const gchar *name)
{
	g_autofree gchar *p = bar_harness_path(h, name);

	g_assert_true(g_file_set_contents(p, "", 0, NULL));
}

static void
clear_flag(BarHarness *h, const gchar *name)
{
	g_autofree gchar *p = bar_harness_path(h, name);

	g_unlink(p);
}

/* Every logged call, one string per call, args joined with `|'. */
static gchar **
read_calls(BarHarness *h)
{
	g_autofree gchar *p = bar_harness_path(h, "calls.log");
	g_autofree gchar *text = NULL;
	gchar **lines;

	if (!g_file_get_contents(p, &text, NULL, NULL))
		return g_new0(gchar *, 1);
	g_strchomp(text);
	lines = g_strsplit(text, "\n", -1);
	return lines;
}

static void
reset_calls(BarHarness *h)
{
	clear_flag(h, "calls.log");
}

/* Bring the widget up against the fake. */
static gboolean
widget_up(BarHarness *h, gchar **fake_out)
{
	if (!bar_harness_up(h, "bar_register_wowgold_plugins", "wow-gold"))
		return FALSE;
	*fake_out = fake_tsmctl(h);
	bar_harness_set(h, "tsmctl-command", *fake_out);
	bar_harness_activate(h);
	return TRUE;
}

/* Catches: no label after a poll, the wrong colour for an up day, the
   alias missing, and a poll that went to the host for EVERY call (five
   ssh round trips) instead of only the first. */
static void
test_widget_poll(void)
{
	BarHarness h;
	g_autofree gchar *fake = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *tip = NULL;
	g_auto(GStrv) calls = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *offline = NULL;
	GError *error = NULL;
	GowlBarPlugin *alias;
	guint i;

	if (!widget_up(&h, &fake))
		return;

	alias = gowl_bar_registry_instantiate(h.registry, "wowgold", &error);
	g_assert_no_error(error);
	g_assert_nonnull(alias);
	g_object_unref(alias);

	/* A fresh widget asks right away; after a fetch, the interval. */
	g_assert_cmpint(gowl_bar_plugin_get_interval(h.plugin), ==, 1);
	bar_harness_poll(&h);
	g_assert_cmpint(gowl_bar_plugin_get_interval(h.plugin), ==, 300);

	label = gowl_bar_plugin_dup_label(h.plugin);
	g_assert_cmpstr(label, ==, "3.49Mg " UP_ARROW);
	g_assert_cmpint(gowl_bar_plugin_get_color(h.plugin), ==,
	                GOWL_BAR_COLOR_GREEN);
	tip = gowl_bar_plugin_dup_tooltip(h.plugin);
	g_assert_nonnull(strstr(tip, "WoW gold: 3,490,145g"));
	g_assert_nonnull(strstr(tip, "Today: +300g"));
	g_assert_nonnull(strstr(tip, "Last sale: WoW Token"));

	calls = read_calls(&h);
	g_assert_cmpuint(g_strv_length(calls), ==, 5);
	first = g_strdup_printf("%s|--quiet|--color=never|--output=json|"
	                        "get|gold", fake);
	offline = g_strdup_printf("%s|--offline|", fake);
	g_assert_cmpstr(calls[0], ==, first);
	for (i = 1; i < 5; i++)
		g_assert_true(g_str_has_prefix(calls[i], offline));
	g_assert_nonnull(strstr(calls[1], "|get|gold-history|--by=day|"
	                        "--since=30d"));
	g_assert_true(g_str_has_suffix(calls[2], "|get|sales|--limit=10"));
	g_assert_true(g_str_has_suffix(calls[3], "|pnl|--period=today"));
	g_assert_true(g_str_has_suffix(calls[4], "|attention"));

	bar_harness_down(&h);
}

/* Catches: the periodic poll going to the host every time, the Refresh
   button NOT going to it, and a host change being served stale. */
static void
test_widget_offline_and_refresh(void)
{
	BarHarness h;
	g_autofree gchar *fake = NULL;
	g_autofree gchar *prefix = NULL;
	gchar **calls;
	guint i;

	if (!widget_up(&h, &fake))
		return;
	bar_harness_set(&h, "host", "wow-pc");
	bar_harness_poll(&h);
	reset_calls(&h);

	/* A second poll within refresh-every: all from the cache. */
	bar_harness_poll(&h);
	calls = read_calls(&h);
	g_assert_cmpuint(g_strv_length(calls), ==, 5);
	for (i = 0; i < 5; i++)
		g_assert_nonnull(strstr(calls[i], "|--offline|"));
	g_strfreev(calls);
	reset_calls(&h);

	/* Refresh: the first call reaches the host, the rest read the
	   cache it just filled. */
	gowl_bar_plugin_panel_action(h.plugin, "actions", 0, 0.0, 1);
	calls = read_calls(&h);
	g_assert_cmpuint(g_strv_length(calls), ==, 5);
	prefix = g_strdup_printf("%s|--host=wow-pc|--quiet|", fake);
	g_assert_true(g_str_has_prefix(calls[0], prefix));
	g_assert_null(strstr(calls[0], "--offline"));
	g_assert_nonnull(strstr(calls[1], "|--offline|"));
	g_strfreev(calls);
	reset_calls(&h);

	/* A changed filter: polled at once, and from the host. */
	bar_harness_set(&h, "realm", "Moon Guard");
	g_assert_cmpint(gowl_bar_plugin_get_interval(h.plugin), ==, 1);
	bar_harness_poll(&h);
	calls = read_calls(&h);
	g_assert_null(strstr(calls[0], "--offline"));
	g_assert_nonnull(strstr(calls[0], "|--realm=Moon Guard|"));
	g_strfreev(calls);

	/* A colour change is not a reason to fetch. */
	bar_harness_set(&h, "color-up", "mauve");
	g_assert_cmpint(gowl_bar_plugin_get_interval(h.plugin), ==, 300);
	g_assert_cmpint(gowl_bar_plugin_get_color(h.plugin), ==,
	                GOWL_BAR_COLOR_MAUVE);

	bar_harness_down(&h);
}

/* Catches: a failing tsmctl blanking the widget, the error swallowed,
   and the panel not saying how old the data is. */
static void
test_widget_stale(void)
{
	BarHarness h;
	g_autofree gchar *fake = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *tip = NULL;
	GowlBarPanel *panel;

	if (!widget_up(&h, &fake))
		return;
	bar_harness_poll(&h);
	touch_flag(&h, "fail-all");
	gowl_bar_plugin_panel_action(h.plugin, "actions", 0, 0.0, 1);

	label = gowl_bar_plugin_dup_label(h.plugin);
	g_assert_cmpstr(label, ==, "3.49Mg " UP_ARROW);
	tip = gowl_bar_plugin_dup_tooltip(h.plugin);
	g_assert_nonnull(strstr(tip, "Stale: tsmctl: error: the cache is "
	                        "corrupt"));

	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "3,490,145g"));
	g_assert_true(bar_harness_panel_mentions(panel,
		"Last refresh failed: tsmctl: error: the cache is corrupt"));
	g_assert_true(bar_harness_panel_mentions(panel, "Stale since "));
	g_assert_false(bar_harness_panel_mentions(panel, "an unknown time"));
	g_object_unref(panel);

	/* Recovery clears it. */
	clear_flag(&h, "fail-all");
	gowl_bar_plugin_panel_action(h.plugin, "actions", 0, 0.0, 1);
	panel = bar_harness_panel(&h);
	g_assert_false(bar_harness_panel_mentions(panel, "Last refresh failed"));
	g_object_unref(panel);

	bar_harness_down(&h);
}

/* Catches: an unreachable host leaving the widget blank when tsmctl's
   cache could have answered, and that fallback passed off as fresh. */
static void
test_widget_host_fallback(void)
{
	BarHarness h;
	g_autofree gchar *fake = NULL;
	g_autofree gchar *label = NULL;
	GowlBarPanel *panel;
	gchar **calls;

	if (!widget_up(&h, &fake))
		return;
	bar_harness_set(&h, "host", "wow-pc");
	touch_flag(&h, "fail-online");
	bar_harness_poll(&h);

	calls = read_calls(&h);
	g_assert_cmpuint(g_strv_length(calls), ==, 6);
	g_assert_null(strstr(calls[0], "--offline"));
	g_assert_nonnull(strstr(calls[1], "|--offline|"));
	g_assert_true(g_str_has_suffix(calls[1], "|get|gold"));
	g_strfreev(calls);

	label = gowl_bar_plugin_dup_label(h.plugin);
	g_assert_cmpstr(label, ==, "3.49Mg " UP_ARROW);
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel,
		"could not reach the host"));
	g_assert_true(bar_harness_panel_mentions(panel, "No route to host"));
	g_assert_true(bar_harness_panel_mentions(panel, "cached copy"));
	g_object_unref(panel);

	bar_harness_down(&h);
}

/* Catches: a crash or a blank panel when tsmctl is absent, and the
   panel not saying where tsmctl comes from (it is not packaged). */
static void
test_widget_missing(void)
{
	BarHarness h;
	g_autofree gchar *tip = NULL;
	g_autofree gchar *label = NULL;
	GowlBarPanel *panel;

	if (!bar_harness_up(&h, "bar_register_wowgold_plugins", "wow-gold"))
		return;
	bar_harness_set(&h, "tsmctl-command", "/nonexistent/dir/tsmctl");
	bar_harness_poll(&h);

	label = gowl_bar_plugin_dup_label(h.plugin);
	g_assert_null(label);
	g_assert_true(gowl_bar_plugin_get_visible(h.plugin));
	tip = gowl_bar_plugin_dup_tooltip(h.plugin);
	g_assert_cmpstr(tip, ==, "WoW gold: /nonexistent/dir/tsmctl not found");

	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "tsmctl not found"));
	g_assert_nonnull(gowl_bar_panel_find(panel, "missing"));
	{
		GowlBarPanelItem *label_item = gowl_bar_panel_get_item(panel, 1);
		const gchar *text = gowl_bar_panel_item_get_title(label_item);

		g_assert_nonnull(strstr(text, "not a Fedora package"));
		g_assert_nonnull(strstr(text, "wow-gold.tsmctl-command"));
		g_assert_nonnull(strstr(text, "/nonexistent/dir/tsmctl"));
	}
	g_object_unref(panel);

	/* Settings that name a dir, or are empty, are not a program. */
	bar_harness_set(&h, "tsmctl-command", "/tmp");
	bar_harness_poll(&h);
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "tsmctl not found"));
	g_object_unref(panel);
	g_assert_cmpuint(h.host->spawned->len, ==, 0);

	bar_harness_down(&h);
}

/* Catches: the direction colours swapped, a flat day coloured as a
   loss, and a #rrggbb override ignored. */
static void
test_widget_colours(void)
{
	BarHarness h;
	g_autofree gchar *fake = NULL;
	g_autofree gchar *label = NULL;
	gdouble rgba[4];

	if (!widget_up(&h, &fake))
		return;

	touch_flag(&h, "down");
	bar_harness_poll(&h);
	label = gowl_bar_plugin_dup_label(h.plugin);
	g_assert_cmpstr(label, ==, "3.49Mg " DOWN_ARROW);
	g_assert_cmpint(gowl_bar_plugin_get_color(h.plugin), ==,
	                GOWL_BAR_COLOR_RED);
	g_assert_false(gowl_bar_plugin_get_color_rgba(h.plugin, rgba));

	bar_harness_set(&h, "color-down", "#336699");
	g_assert_true(gowl_bar_plugin_get_color_rgba(h.plugin, rgba));
	g_assert_cmpfloat_with_epsilon(rgba[0], 0x33 / 255.0, 0.01);
	g_assert_cmpfloat_with_epsilon(rgba[2], 0x99 / 255.0, 0.01);

	clear_flag(&h, "down");
	touch_flag(&h, "flat");
	gowl_bar_plugin_panel_action(h.plugin, "actions", 0, 0.0, 1);
	g_free(label);
	label = gowl_bar_plugin_dup_label(h.plugin);
	g_assert_cmpstr(label, ==, "3.49Mg");
	g_assert_cmpint(gowl_bar_plugin_get_color(h.plugin), ==,
	                GOWL_BAR_COLOR_SUBTEXT);
	g_assert_false(gowl_bar_plugin_get_color_rgba(h.plugin, rgba));

	bar_harness_set(&h, "color-flat", "teal");
	g_assert_cmpint(gowl_bar_plugin_get_color(h.plugin), ==,
	                GOWL_BAR_COLOR_TEAL);

	/* Full label, with the change, in gsc. */
	clear_flag(&h, "flat");
	bar_harness_set(&h, "label-format", "full");
	bar_harness_set(&h, "show-delta", "true");
	gowl_bar_plugin_panel_action(h.plugin, "actions", 0, 0.0, 1);
	g_free(label);
	label = gowl_bar_plugin_dup_label(h.plugin);
	g_assert_cmpstr(label, ==, "3,490,145g " UP_ARROW "300g");
	g_assert_cmpint(gowl_bar_plugin_get_color(h.plugin), ==,
	                GOWL_BAR_COLOR_GREEN);

	bar_harness_down(&h);
}

/* Catches: a section missing or out of place, characters unsorted or
   zero balances listed, attention absent, and graph samples dropped. */
static void
test_widget_panel(void)
{
	BarHarness h;
	g_autofree gchar *fake = NULL;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	gint pos_sales = -1, pos_chars = -1, pos_attn = -1, pos_first_char = -1;
	guint i, n, n_samples = 0;

	if (!widget_up(&h, &fake))
		return;
	bar_harness_set(&h, "character-count", "3");
	bar_harness_set(&h, "attention-count", "2");
	bar_harness_set(&h, "sales-count", "4");
	bar_harness_poll(&h);
	panel = bar_harness_panel(&h);

	item = gowl_bar_panel_get_item(panel, 0);
	g_assert_cmpint(gowl_bar_panel_item_get_kind(item), ==,
	                GOWL_BAR_ITEM_HERO);
	g_assert_cmpstr(gowl_bar_panel_item_get_title(item), ==, "3,490,145g");
	g_assert_cmpstr(gowl_bar_panel_item_get_subtitle(item), ==,
	                UP_ARROW " +300g today");

	n = gowl_bar_panel_n_items(panel);
	for (i = 0; i < n; i++) {
		const gchar *title;

		item = gowl_bar_panel_get_item(panel, i);
		title = gowl_bar_panel_item_get_title(item);
		if (gowl_bar_panel_item_get_kind(item) == GOWL_BAR_ITEM_GRAPH)
			gowl_bar_panel_item_get_samples(item, &n_samples);
		if (gowl_bar_panel_item_get_kind(item) != GOWL_BAR_ITEM_SECTION)
			continue;
		if (g_strcmp0(title, "Last sales") == 0)
			pos_sales = (gint)i;
		else if (g_strcmp0(title, "Characters") == 0)
			pos_chars = (gint)i;
		else if (g_str_has_prefix(title, "Needs attention"))
			pos_attn = (gint)i;
	}
	g_assert_cmpuint(n_samples, ==, 30);
	g_assert_cmpint(pos_sales, >, 0);
	g_assert_cmpint(pos_chars, >, pos_sales);
	g_assert_cmpint(pos_attn, >, pos_chars);
	g_assert_true(bar_harness_panel_mentions(panel, "Needs attention (7)"));

	/* Four sales, newest first. */
	item = gowl_bar_panel_get_item(panel, (guint)pos_sales + 1);
	g_assert_cmpstr(gowl_bar_panel_item_get_id(item), ==, "sale:0");
	g_assert_cmpstr(gowl_bar_panel_item_get_title(item), ==,
	                "WoW Token \xc3\x97" "2");
	g_assert_cmpstr(gowl_bar_panel_item_get_value(item), ==, "581,100g");
	g_assert_nonnull(gowl_bar_panel_find(panel, "sale:3"));
	g_assert_null(gowl_bar_panel_find(panel, "sale:4"));

	/* Three characters, richest first, name-realm, then "+N more". */
	pos_first_char = pos_chars + 1;
	item = gowl_bar_panel_get_item(panel, (guint)pos_first_char);
	g_assert_cmpstr(gowl_bar_panel_item_get_title(item), ==,
	                "Goldmonger-Thorium Brotherhood");
	g_assert_cmpstr(gowl_bar_panel_item_get_value(item), ==,
	                "2,768,833g");
	g_assert_true(bar_harness_panel_mentions(panel, "+5 more with gold"));
	g_assert_false(bar_harness_panel_mentions(panel, "The Guild Vault"));

	/* Today's P&L. */
	g_assert_true(bar_harness_panel_mentions(panel, "Sales today"));
	g_assert_true(bar_harness_panel_mentions(panel, "1,253g"));
	g_assert_true(bar_harness_panel_mentions(panel, "+1,038g"));
	g_assert_true(bar_harness_panel_mentions(panel, "7-day change"));
	g_assert_true(bar_harness_panel_mentions(panel, "+700g"));

	/* Two attention rows of seven. */
	g_assert_nonnull(gowl_bar_panel_find(panel, "attn:1"));
	g_assert_null(gowl_bar_panel_find(panel, "attn:2"));

	item = gowl_bar_panel_find(panel, "actions");
	g_assert_nonnull(item);
	g_assert_cmpuint(gowl_bar_panel_item_n_children(item), ==, 3);
	g_object_unref(panel);

	/* An old tsmctl without `attention': the rest still shows. */
	touch_flag(&h, "no-attention");
	gowl_bar_plugin_panel_action(h.plugin, "actions", 0, 0.0, 1);
	panel = bar_harness_panel(&h);
	g_assert_false(bar_harness_panel_mentions(panel, "Needs attention"));
	g_assert_false(bar_harness_panel_mentions(panel, "Last refresh failed"));
	g_assert_true(bar_harness_panel_mentions(panel, "Characters"));
	g_object_unref(panel);

	bar_harness_down(&h);
}

/* Catches: copy putting unquoted text into a shell line, the report
   running tsmctl unquoted, and report-command ignored. */
static void
test_widget_actions(void)
{
	BarHarness h;
	g_autofree gchar *fake = NULL;
	const gchar *line;

	if (!widget_up(&h, &fake))
		return;
	bar_harness_set(&h, "realm", "it's; rm -rf ~");
	bar_harness_poll(&h);

	/* Copy summary. */
	gowl_bar_plugin_panel_action(h.plugin, "actions", 2, 0.0, 1);
	g_assert_cmpuint(h.host->spawned->len, ==, 1);
	line = g_ptr_array_index(h.host->spawned, 0);
	g_assert_true(g_str_has_prefix(line, "printf %s 'WoW gold: 3,490,145g"));
	g_assert_true(g_str_has_suffix(line, "' | wl-copy"));
	g_assert_true(bar_harness_toasted(&h, "Copied|WoW gold: 3,490,145g"));

	bar_harness_set(&h, "copy-command", "cmacs-copy --primary");
	gowl_bar_plugin_panel_action(h.plugin, "char:0", -1, 0.0, 1);
	line = g_ptr_array_index(h.host->spawned, 1);
	g_assert_cmpstr(line, ==, "printf %s 'Goldmonger-Thorium Brotherhood: "
	                "2,768,833g' | cmacs-copy --primary");
	gowl_bar_plugin_panel_action(h.plugin, "sale:0", -1, 0.0, 1);
	line = g_ptr_array_index(h.host->spawned, 2);
	g_assert_nonnull(strstr(line, "WoW Token"));
	/* A stale index copies nothing rather than the wrong row. */
	gowl_bar_plugin_panel_action(h.plugin, "sale:999", -1, 0.0, 1);
	g_assert_cmpuint(h.host->spawned->len, ==, 3);

	/* Open report: a terminal, sh -c, every word quoted twice. */
	g_setenv("TERMINAL", "fake-term", TRUE);
	gowl_bar_plugin_panel_action(h.plugin, "actions", 1, 0.0, 1);
	line = g_ptr_array_index(h.host->spawned, 3);
	g_assert_true(g_str_has_prefix(line, "fake-term -e sh -c "));
	g_assert_nonnull(strstr(line, "report"));
	g_assert_nonnull(strstr(line, "less -R"));
	g_assert_nonnull(strstr(line, "--offline"));
	g_assert_nonnull(strstr(line, "--color=always"));
	{
		/* Undo both quotings: the argv inside is exact. */
		g_auto(GStrv) outer = NULL;
		g_auto(GStrv) inner = NULL;
		gint no = 0, ni = 0;

		g_assert_true(g_shell_parse_argv(line, &no, &outer, NULL));
		g_assert_cmpint(no, ==, 5);
		g_assert_true(g_shell_parse_argv(outer[4], &ni, &inner, NULL));
		g_assert_cmpstr(inner[0], ==, fake);
		g_assert_cmpstr(inner[2], ==, "--realm=it's; rm -rf ~");
		g_assert_cmpstr(inner[ni - 1], ==, "-R");
	}

	bar_harness_set(&h, "report-command",
		"emacsctl eval '(cmacs-gowl-bar-wow-gold-report)'");
	gowl_bar_plugin_panel_key(h.plugin, 'o', 0, -1);
	line = g_ptr_array_index(h.host->spawned, 4);
	g_assert_cmpstr(line, ==,
	                "emacsctl eval '(cmacs-gowl-bar-wow-gold-report)'");

	/* Keys: c copies; a modified key is not ours. */
	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, 'c', 0, -1));
	g_assert_cmpuint(h.host->spawned->len, ==, 6);
	g_assert_false(gowl_bar_plugin_panel_key(h.plugin, 'c', 4, -1));
	g_assert_false(gowl_bar_plugin_panel_key(h.plugin, 'z', 0, -1));

	bar_harness_down(&h);
}

/* Catches: shell syntax in a setting being executed, and a hostile
   host reaching tsmctl at all. */
static void
test_widget_no_shell(void)
{
	BarHarness h;
	g_autofree gchar *fake = NULL;
	const gchar *realm = "x; touch wowgold-PWNED; $(touch wowgold-PWNED2)";
	GowlBarPanel *panel;
	gchar **calls;

	if (!widget_up(&h, &fake))
		return;
	/* Relative names: the child runs in our cwd, and a setting
	   holding a long scratch path would trip the length bound. */
	g_unlink("wowgold-PWNED");
	g_unlink("wowgold-PWNED2");
	bar_harness_set(&h, "realm", realm);
	bar_harness_poll(&h);
	g_assert_false(g_file_test("wowgold-PWNED", G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test("wowgold-PWNED2", G_FILE_TEST_EXISTS));
	calls = read_calls(&h);
	g_assert_nonnull(strstr(calls[0], realm));
	g_strfreev(calls);
	reset_calls(&h);

	bar_harness_set(&h, "host", "-oProxyCommand=sh");
	bar_harness_poll(&h);
	calls = read_calls(&h);
	g_assert_cmpuint(g_strv_length(calls), ==, 0);
	g_strfreev(calls);
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel,
		"the host setting is not an ssh host name"));
	g_object_unref(panel);
	/* And it is not retried every second. */
	g_assert_cmpint(gowl_bar_plugin_get_interval(h.plugin), ==, 300);

	bar_harness_down(&h);
}

/* Catches: a hung tsmctl (or its ssh) holding the worker forever; the
   whole process group must go, so its `sleep' does not outlive it. */
static void
test_widget_timeout(void)
{
	BarHarness h;
	g_autofree gchar *fake = NULL;
	GowlBarPanel *panel;
	gint64 start;

	if (!widget_up(&h, &fake))
		return;
	bar_harness_set(&h, "timeout", "1");
	touch_flag(&h, "hang");
	start = g_get_monotonic_time();
	bar_harness_poll(&h);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 5 * G_USEC_PER_SEC);

	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel,
		"tsmctl did not finish within 1 s"));
	g_assert_true(bar_harness_panel_mentions(panel, "No data yet"));
	g_object_unref(panel);
	{
		g_autofree gchar *label = gowl_bar_plugin_dup_label(h.plugin);

		g_assert_cmpstr(label, ==, "!");
		g_assert_cmpint(gowl_bar_plugin_get_color(h.plugin), ==,
		                GOWL_BAR_COLOR_RED);
	}
	bar_harness_down(&h);
}

/* Catches: tsmctl printing a traceback (exit 0) shown as an empty
   account; and the in-flight state not shown while a fetch is held. */
static void
test_widget_junk_and_busy(void)
{
	BarHarness h;
	g_autofree gchar *fake = NULL;
	GowlBarPanel *panel;

	if (!widget_up(&h, &fake))
		return;
	touch_flag(&h, "junk");
	bar_harness_poll(&h);
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel,
		"tsmctl get gold did not print tsmctl JSON"));
	g_object_unref(panel);
	clear_flag(&h, "junk");

	/* Held: queued, not yet run; the panel builds without blocking. */
	h.host->hold_work = TRUE;
	gowl_bar_plugin_panel_action(h.plugin, "actions", 0, 0.0, 1);
	g_assert_cmpuint(h.host->held->len, ==, 1);
	panel = bar_harness_panel(&h);
	g_assert_nonnull(panel);
	g_object_unref(panel);
	g_assert_cmpuint(bar_harness_run_held(&h), ==, 1);
	panel = bar_harness_panel(&h);
	g_assert_true(bar_harness_panel_mentions(panel, "3,490,145g"));
	g_assert_false(bar_harness_panel_mentions(panel, "Last refresh failed"));
	g_object_unref(panel);

	bar_harness_down(&h);
}

int
main(int argc, char **argv)
{
	g_setenv("TZ", "UTC", FALSE);
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-wow-gold/money/format", test_money_format);
	g_test_add_func("/bar-wow-gold/money/compact", test_money_compact);
	g_test_add_func("/bar-wow-gold/money/delta", test_money_delta);
	g_test_add_func("/bar-wow-gold/money/parse", test_money_parse);
	g_test_add_func("/bar-wow-gold/json/member", test_json_member);
	g_test_add_func("/bar-wow-gold/json/int", test_json_int);
	g_test_add_func("/bar-wow-gold/json/decode-hostile",
	                test_json_decode_hostile);
	g_test_add_func("/bar-wow-gold/parse/gold", test_parse_gold);
	g_test_add_func("/bar-wow-gold/parse/gold-hostile",
	                test_parse_gold_hostile);
	g_test_add_func("/bar-wow-gold/parse/history", test_parse_history);
	g_test_add_func("/bar-wow-gold/parse/sales", test_parse_sales);
	g_test_add_func("/bar-wow-gold/parse/pnl", test_parse_pnl);
	g_test_add_func("/bar-wow-gold/parse/attention", test_parse_attention);
	g_test_add_func("/bar-wow-gold/series/gaps", test_series_gaps);
	g_test_add_func("/bar-wow-gold/series/trend", test_trend);
	g_test_add_func("/bar-wow-gold/series/period-days", test_period_days);
	g_test_add_func("/bar-wow-gold/text/label", test_label);
	g_test_add_func("/bar-wow-gold/text/summary-tooltip",
	                test_summary_and_tooltip);
	g_test_add_func("/bar-wow-gold/text/ago", test_format_ago);
	g_test_add_func("/bar-wow-gold/argv/exact", test_argv_exact);
	g_test_add_func("/bar-wow-gold/argv/injection", test_argv_injection);
	g_test_add_func("/bar-wow-gold/widget/poll", test_widget_poll);
	g_test_add_func("/bar-wow-gold/widget/offline-and-refresh",
	                test_widget_offline_and_refresh);
	g_test_add_func("/bar-wow-gold/widget/stale", test_widget_stale);
	g_test_add_func("/bar-wow-gold/widget/host-fallback",
	                test_widget_host_fallback);
	g_test_add_func("/bar-wow-gold/widget/missing", test_widget_missing);
	g_test_add_func("/bar-wow-gold/widget/colours", test_widget_colours);
	g_test_add_func("/bar-wow-gold/widget/panel", test_widget_panel);
	g_test_add_func("/bar-wow-gold/widget/actions", test_widget_actions);
	g_test_add_func("/bar-wow-gold/widget/no-shell", test_widget_no_shell);
	g_test_add_func("/bar-wow-gold/widget/timeout", test_widget_timeout);
	g_test_add_func("/bar-wow-gold/widget/junk-and-busy",
	                test_widget_junk_and_busy);
	return g_test_run();
}
