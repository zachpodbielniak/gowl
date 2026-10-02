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
 * test-macro-voice.c - What was said, matched to a macro.
 *
 * Pure GLib: sentences in the shapes speech-to-text produces them --
 * capitals, full stops, numbers as words -- against a list of macro
 * names and a phrase table, checking each tier of the match and the
 * arguments left over.
 */

#include <glib.h>
#include <string.h>

#include "../modules/macro/gowl-macro-voice.c"

static const gchar *const names[] = {
	"sort-windows", "sort-by-app", "sort", "pip-corner", "focus-or-launch",
	"gather-app", "last-recording", NULL
};

/* Asserts @said matches @want_name with @want_args. */
static void
heard(const gchar *said, GHashTable *phrases, const gchar *want_name,
      const gchar *want_args)
{
	g_autofree gchar *name = NULL;
	g_autofree gchar *args = NULL;
	gboolean ok;

	ok = gowl_macro_voice_match(said, names, phrases, &name, &args);
	if (want_name == NULL) {
		if (ok)
			g_error("\"%s\" matched %s (%s), expected nothing", said,
			        name, args);
		return;
	}
	if (!ok)
		g_error("\"%s\" matched nothing, expected %s", said, want_name);
	g_assert_cmpstr(name, ==, want_name);
	g_assert_cmpstr(args, ==, want_args);
}

static void
test_normalise(void)
{
	struct { const gchar *in; const gchar *out; } cases[] = {
		{ "Sort windows.",               "sort windows" },
		{ "  PIP-corner, twenty-five! ", "pip corner 25" },
		{ "Run the sort_windows macro",  "sort windows macro" },
		{ "please do three",             "3" },
		{ "one hundred and twelve",      "100 and 12" },
		{ "two thousand five hundred",   "2500" },
		{ "rec two twenty five",         "rec 2 25" },
		{ "five six",                    "5 6" },
		{ "twenty thirty",               "20 30" },
		{ "twenty twelve",               "20 12" },
		{ "nineteen ninety nine",        "19 99" },
		{ "three hundred forty two",     "342" },
		{ "Ünïcode Wörds",               "ünïcode wörds" },
		{ "",                            "" },
		{ "...",                         "" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *got = gowl_macro_voice_normalise(cases[i].in);

		g_assert_cmpstr(got, ==, cases[i].out);
	}
}

/* Tier 2: the name said as words, at the front; the rest is arguments. */
static void
test_names(void)
{
	heard("Sort windows.", NULL, "sort-windows", "");
	heard("pip corner twenty five", NULL, "pip-corner", "25");
	heard("Pip corner, 40.", NULL, "pip-corner", "40");
	heard("run focus or launch firefox", NULL, "focus-or-launch", "firefox");
	/* the longest name that fits: not `sort' with "windows 2" */
	heard("sort windows 2", NULL, "sort-windows", "2");
	heard("sort", NULL, "sort", "");
	/* a word boundary: "sortable" is not "sort" */
	heard("sortable", NULL, NULL, NULL);
}

/* Tier 3: every word of a name, scattered; no arguments. */
static void
test_scattered(void)
{
	heard("the last recording please", NULL, "last-recording", "");
	heard("windows, sort them", NULL, "sort-windows", "");
	heard("app gather now", NULL, "gather-app", "");
	heard("sort windows, thank you", NULL, "sort-windows", "");
	/* "you" alone is a word somebody meant */
	heard("pip corner you", NULL, "pip-corner", "you");
	heard("make me a sandwich", NULL, NULL, NULL);
	heard("", NULL, NULL, NULL);
}

/* Tier 1: a configured phrase wins, and keeps its own arguments. */
static void
test_phrases(void)
{
	g_autoptr(GHashTable) p = NULL;

	p = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_insert(p, g_strdup("tidy up"), g_strdup("sort-windows"));
	g_hash_table_insert(p, g_strdup("small video"),
	                    g_strdup("pip-corner 20"));
	g_hash_table_insert(p, g_strdup("sort"), g_strdup("sort-by-app"));

	heard("Tidy up.", p, "sort-windows", "");
	heard("small video", p, "pip-corner", "20");
	/* words after the phrase are appended */
	heard("tidy up now", p, "sort-windows", "now");
	/* a phrase beats a macro of the same name */
	heard("sort", p, "sort-by-app", "");
	/* the longest phrase that fits wins over a shorter one */
	g_hash_table_insert(p, g_strdup("tidy"), g_strdup("gather-app"));
	heard("tidy up", p, "sort-windows", "");
	/* no phrase: names still work */
	heard("pip corner 30", p, "pip-corner", "30");
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/macro/voice/normalise", test_normalise);
	g_test_add_func("/macro/voice/names", test_names);
	g_test_add_func("/macro/voice/scattered", test_scattered);
	g_test_add_func("/macro/voice/phrases", test_phrases);
	return g_test_run();
}
