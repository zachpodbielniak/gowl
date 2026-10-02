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
 * gowl-macro-voice.c - Match what was said to a macro.
 *
 * Speech-to-text output is a sentence, with capitals, punctuation and
 * numbers spelled out however the model felt like it: "Sort windows.",
 * "Pip corner, twenty-five."  Matching works on a normalised form --
 * lower case, words only, number words turned into digits -- in three
 * tiers, first hit wins:
 *
 *   1. A configured phrase.  `voice-phrases' maps something you would
 *      say ("tidy up") to a macro and its arguments ("sort-windows").
 *      The sentence must START with the phrase; the rest is appended as
 *      arguments.
 *   2. A macro's own name, said as words: `pip-corner' is "pip corner".
 *      Again a prefix, the rest becomes arguments, so "pip corner 25"
 *      runs `pip-corner 25'.  The longest name that fits wins: with
 *      both `sort' and `sort-windows', "sort windows 2" is
 *      `sort-windows 2', not `sort windows 2'.
 *   3. Every word of a name somewhere in the sentence, in any order
 *      ("windows, sort them").  No arguments: with the words scattered
 *      there is no telling which are meant as arguments.
 *
 * A few filler words are dropped from the front first -- "run", "do",
 * "macro", "please" -- and "please" or "thank you" from the end,
 * because people say them and no macro is named after them.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-macro"

#include <string.h>

#include "gowl-macro-voice.h"

/* Words dropped from the front of a sentence before matching. */
static const gchar *const filler[] = {
	"run", "do", "macro", "please", "okay", "ok", "hey", "computer",
	"gowl", "the", NULL
};

/* ... and from the end: politeness is not an argument. */
static const gchar *const trailing_filler[] = {
	"please", "thanks", "thank", "you", NULL
};

static const gchar *const units[] = {
	"zero", "one", "two", "three", "four", "five", "six", "seven",
	"eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen",
	"fifteen", "sixteen", "seventeen", "eighteen", "nineteen", NULL
};

static const gchar *const tens[] = {
	"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy",
	"eighty", "ninety", NULL
};

/* The value of a units word (0..19), or -1. */
static gint
unit_value(
	const gchar *w
){
	gint i;

	for (i = 0; units[i] != NULL; i++)
		if (g_strcmp0(units[i], w) == 0)
			return i;
	return -1;
}

/* The value of a tens word (20..90), or -1. */
static gint
tens_value(
	const gchar *w
){
	gint i;

	for (i = 2; tens[i] != NULL; i++)
		if (g_strcmp0(tens[i], w) == 0)
			return i * 10;
	return -1;
}

/*
 * numbers_to_digits:
 *
 * Folds runs of number words into one numeral: "twenty five" -> "25",
 * "one hundred" -> "100", "three" -> "3".  Everything else is copied.
 *
 * The run follows how numbers are said, so two numbers said one after
 * the other stay two: "two twenty five" is "2 25", not 27.  After a
 * units word (zero..nineteen) only "hundred" or "thousand" continues
 * the number; after a tens word only a units word from one to nine
 * does (or the multipliers); after "hundred", tens or units; after
 * "thousand", anything.
 */
typedef enum {
	NUM_NONE,
	NUM_UNIT,
	NUM_TENS,
	NUM_HUNDRED,
	NUM_THOUSAND
} NumLast;

static GPtrArray *
numbers_to_digits(
	gchar **words
){
	GPtrArray *out;
	guint i;

	out = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; words[i] != NULL; ) {
		NumLast last;
		gint total;
		gint cur;

		total = 0;
		cur = 0;
		last = NUM_NONE;
		for (; words[i] != NULL; i++) {
			gint v;

			if ((v = tens_value(words[i])) >= 0) {
				if (last == NUM_UNIT || last == NUM_TENS)
					break;
				cur += v;
				last = NUM_TENS;
			} else if ((v = unit_value(words[i])) >= 0) {
				if (last == NUM_UNIT
				    || (last == NUM_TENS && (v == 0 || v > 9)))
					break;
				cur += v;
				last = NUM_UNIT;
			} else if (last != NUM_NONE && last != NUM_HUNDRED
			           && last != NUM_THOUSAND
			           && g_strcmp0(words[i], "hundred") == 0) {
				cur = (cur == 0 ? 1 : cur) * 100;
				last = NUM_HUNDRED;
			} else if (last != NUM_NONE && last != NUM_THOUSAND
			           && g_strcmp0(words[i], "thousand") == 0) {
				total += (cur == 0 ? 1 : cur) * 1000;
				cur = 0;
				last = NUM_THOUSAND;
			} else {
				break;
			}
		}
		if (last != NUM_NONE) {
			g_ptr_array_add(out, g_strdup_printf("%d", total + cur));
			continue;
		}
		g_ptr_array_add(out, g_strdup(words[i]));
		i++;
	}
	g_ptr_array_add(out, NULL);
	return out;
}

/**
 * gowl_macro_voice_normalise:
 * @text: what speech-to-text produced
 *
 * Lower case, letters and digits only (everything else is a word
 * break), single spaces, number words as digits, filler dropped from
 * the front.  Also the form a macro name or a phrase is compared in:
 * `pip-corner' normalises to "pip corner".
 *
 * Returns: (transfer full): the normalised text, possibly empty
 */
gchar *
gowl_macro_voice_normalise(
	const gchar *text
){
	g_autofree gchar *folded = NULL;
	g_auto(GStrv) words = NULL;
	g_autoptr(GPtrArray) kept = NULL;
	g_autoptr(GPtrArray) digits = NULL;
	gchar *p;
	guint i;
	gboolean leading;

	if (text == NULL)
		return g_strdup("");
	folded = g_utf8_casefold(text, -1);
	/* Anything not a letter or a digit is a break: "twenty-five." is
	   two words, and so is "pip_corner". */
	for (p = folded; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (!g_unichar_isalnum(c)) {
			gchar *q;

			for (q = p; q < g_utf8_next_char(p); q++)
				*q = ' ';
		}
	}
	words = g_strsplit_set(folded, " ", -1);

	kept = g_ptr_array_new();
	leading = TRUE;
	for (i = 0; words[i] != NULL; i++) {
		if (*words[i] == '\0')
			continue;
		if (leading && g_strv_contains(filler, words[i]))
			continue;
		leading = FALSE;
		g_ptr_array_add(kept, words[i]);
	}
	/* "you" only goes as part of "thank you" */
	while (kept->len > 0) {
		const gchar *last = g_ptr_array_index(kept, kept->len - 1);

		if (!g_strv_contains(trailing_filler, last))
			break;
		if (g_strcmp0(last, "you") == 0
		    && (kept->len < 2
		        || g_strcmp0(g_ptr_array_index(kept, kept->len - 2),
		                     "thank") != 0))
			break;
		g_ptr_array_remove_index(kept, kept->len - 1);
	}
	g_ptr_array_add(kept, NULL);

	digits = numbers_to_digits((gchar **)kept->pdata);
	return g_strjoinv(" ", (gchar **)digits->pdata);
}

/*
 * prefix_rest:
 *
 * When @said starts with the words of @want, returns what follows
 * (possibly ""); otherwise %NULL.  Word boundaries matter: "sort" does
 * not prefix "sortable".
 */
static const gchar *
prefix_rest(
	const gchar *said,
	const gchar *want
){
	gsize n;

	if (want == NULL || *want == '\0')
		return NULL;
	n = strlen(want);
	if (strncmp(said, want, n) != 0)
		return NULL;
	if (said[n] == '\0')
		return said + n;
	if (said[n] == ' ')
		return said + n + 1;
	return NULL;
}

/* Whether every word of @want appears somewhere in @said. */
static gboolean
all_words_in(
	const gchar *said,
	const gchar *want
){
	g_auto(GStrv) have = NULL;
	g_auto(GStrv) need = NULL;
	guint i;

	if (want == NULL || *want == '\0')
		return FALSE;
	have = g_strsplit(said, " ", -1);
	need = g_strsplit(want, " ", -1);
	for (i = 0; need[i] != NULL; i++)
		if (!g_strv_contains((const gchar * const *)have, need[i]))
			return FALSE;
	return TRUE;
}

/* Joins two argument strings with a space, skipping empty ones. */
static gchar *
join_args(
	const gchar *a,
	const gchar *b
){
	if (a == NULL || *a == '\0')
		return g_strdup(b != NULL ? b : "");
	if (b == NULL || *b == '\0')
		return g_strdup(a);
	return g_strdup_printf("%s %s", a, b);
}

/**
 * gowl_macro_voice_match:
 * @text: what speech-to-text produced
 * @names: (array zero-terminated=1): the macros that exist
 * @phrases: (nullable) (element-type utf8 utf8): spoken phrase ->
 *   "macro [args]", checked first
 * @out_name: (out) (transfer full): the macro to run
 * @out_args: (out) (transfer full): its arguments, space separated,
 *   possibly ""
 *
 * Decides which macro @text names.  See the file comment for the three
 * tiers.
 *
 * Returns: %TRUE when a macro was matched
 */
gboolean
gowl_macro_voice_match(
	const gchar         *text,
	const gchar * const *names,
	GHashTable          *phrases,
	gchar              **out_name,
	gchar              **out_args
){
	g_autofree gchar *said = NULL;
	const gchar *best = NULL;
	const gchar *best_rest = NULL;
	gsize best_len = 0;
	guint best_words = 0;
	guint i;

	g_return_val_if_fail(out_name != NULL && out_args != NULL, FALSE);
	*out_name = NULL;
	*out_args = NULL;

	said = gowl_macro_voice_normalise(text);
	if (*said == '\0')
		return FALSE;

	/* --- 1. configured phrases, longest first --- */
	if (phrases != NULL) {
		GHashTableIter iter;
		gpointer key;
		gpointer value;
		const gchar *target = NULL;

		g_hash_table_iter_init(&iter, phrases);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			g_autofree gchar *want = gowl_macro_voice_normalise(key);
			const gchar *rest = prefix_rest(said, want);

			if (rest != NULL && strlen(want) > best_len) {
				best_len = strlen(want);
				best_rest = rest;
				target = value;
			}
		}
		if (target != NULL) {
			g_auto(GStrv) parts = g_strsplit(target, " ", 2);

			if (parts[0] != NULL && *parts[0] != '\0') {
				*out_name = g_strdup(parts[0]);
				*out_args = join_args(parts[1], best_rest);
				return TRUE;
			}
		}
	}

	/* --- 2. a name said as words, at the front --- */
	best_len = 0;
	for (i = 0; names != NULL && names[i] != NULL; i++) {
		g_autofree gchar *want = gowl_macro_voice_normalise(names[i]);
		const gchar *rest = prefix_rest(said, want);

		if (rest != NULL && strlen(want) > best_len) {
			best_len = strlen(want);
			best = names[i];
			best_rest = rest;
		}
	}
	if (best != NULL) {
		*out_name = g_strdup(best);
		*out_args = g_strdup(best_rest);
		return TRUE;
	}

	/* --- 3. every word of a name, anywhere: the most words wins --- */
	for (i = 0; names != NULL && names[i] != NULL; i++) {
		g_autofree gchar *want = gowl_macro_voice_normalise(names[i]);
		g_auto(GStrv) split = NULL;
		guint words;

		if (!all_words_in(said, want))
			continue;
		split = g_strsplit(want, " ", -1);
		words = g_strv_length(split);
		if (words > best_words) {
			best_words = words;
			best = names[i];
		}
	}
	if (best != NULL) {
		*out_name = g_strdup(best);
		*out_args = g_strdup("");
		return TRUE;
	}
	return FALSE;
}
