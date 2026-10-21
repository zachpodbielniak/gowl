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
 * test-gabbrev-learn.c - Repeated phrases, and the triggers proposed.
 *
 * The learner is the part of gabbrev that sees ordinary typing, so the
 * safety rules are tested as hard as the counting: nothing with a digit
 * or a symbol in it is ever a phrase, and the table stays bounded.
 */

#include <glib.h>
#include <string.h>

#include "../modules/gabbrev/gowl-gabbrev-learn.c"

static void
feed(
	GabbrevLearner *l,
	const gchar    *text
){
	const gchar *p;

	for (p = text; *p != '\0'; p = g_utf8_next_char(p))
		gabbrev_learner_feed(l, g_utf8_get_char(p), 1000);
}

static GHashTable *
set_of(const gchar * const *items)
{
	GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      NULL);
	guint i;

	for (i = 0; items != NULL && items[i] != NULL; i++)
		g_hash_table_add(h, g_strdup(items[i]));
	return h;
}

/* A phrase typed three times is suggested, with its initials */
static void
test_counts(void)
{
	g_autoptr(GabbrevLearner) l = gabbrev_learner_new();
	g_autoptr(GPtrArray) s = NULL;
	GabbrevSuggestion *first;

	feed(l, "thanks, thank you very much. ");
	feed(l, "Thank you very much! ");
	g_assert_cmpuint(gabbrev_learner_count(l, "thank you very much"), ==, 2);
	s = gabbrev_learner_suggest(l, 0, NULL, NULL);
	g_assert_cmpuint(s->len, ==, 0);
	g_clear_pointer(&s, g_ptr_array_unref);

	{
		g_autoptr(GPtrArray) ready = gabbrev_learner_take_ready(l);

		g_assert_cmpuint(ready->len, ==, 0);
	}
	feed(l, "and thank you very much.\n");
	g_assert_cmpuint(gabbrev_learner_count(l, "THANK YOU VERY MUCH"), ==, 3);
	{
		g_autoptr(GPtrArray) ready = gabbrev_learner_take_ready(l);
		g_autoptr(GPtrArray) again = gabbrev_learner_take_ready(l);

		/* Every phrase that just got there, the longest first, once */
		g_assert_cmpuint(ready->len, ==, 3);
		g_assert_cmpstr(g_ptr_array_index(ready, 0), ==, "thank you very much");
		g_assert_cmpuint(again->len, ==, 0);
	}
	s = gabbrev_learner_suggest(l, 0, NULL, NULL);
	g_assert_cmpuint(s->len, >=, 1);
	first = g_ptr_array_index(s, 0);
	/* The longest phrase wins over its parts (`thank you', `very much') */
	g_assert_cmpstr(first->phrase, ==, "thank you very much");
	g_assert_cmpuint(first->count, ==, 3);
	g_assert_cmpuint(first->n_words, ==, 4);
	g_assert_cmpstr(first->trigger, ==, ";tyvm");
	g_assert_cmpint(first->last, ==, 1000);
}

/* A run with a digit or a symbol is no word, and breaks the phrase */
static void
test_not_words(void)
{
	g_autoptr(GabbrevLearner) l = gabbrev_learner_new();
	guint i;

	for (i = 0; i < 5; i++) {
		feed(l, "my password hunter2 is here ");
		feed(l, "mail me at someone@example ok ");
		gabbrev_learner_break(l);
	}
	g_assert_cmpuint(gabbrev_learner_count(l, "my password hunter2 is"), ==, 0);
	g_assert_cmpuint(gabbrev_learner_count(l, "password hunter is"), ==, 0);
	g_assert_cmpuint(gabbrev_learner_count(l, "password is here"), ==, 0);
	/* What is on either side of the bad word still counts on its own */
	g_assert_cmpuint(gabbrev_learner_count(l, "my password"), ==, 5);
	g_assert_cmpuint(gabbrev_learner_count(l, "mail me at"), ==, 5);
	g_assert_cmpuint(gabbrev_learner_count(l, "at someone"), ==, 0);
}

/* Short phrases (under min_chars) are not worth a trigger */
static void
test_min_chars(void)
{
	g_autoptr(GabbrevLearner) l = gabbrev_learner_new();
	guint i;

	for (i = 0; i < 5; i++)
		feed(l, "ok go. ");
	g_assert_cmpuint(gabbrev_learner_count(l, "ok go"), ==, 0);
	g_assert_cmpuint(gabbrev_learner_size(l), ==, 0);
}

/* Backspace inside a word fixes the typo; past it, the phrase ends */
static void
test_backspace(void)
{
	g_autoptr(GabbrevLearner) l = gabbrev_learner_new();

	feed(l, "see you tomorrox");
	gabbrev_learner_backspace(l);
	feed(l, "w ");
	g_assert_cmpuint(gabbrev_learner_count(l, "see you tomorrow"), ==, 1);

	gabbrev_learner_break(l);
	feed(l, "see you ");
	gabbrev_learner_backspace(l);   /* the word is empty: break */
	feed(l, "tomorrow ");
	g_assert_cmpuint(gabbrev_learner_count(l, "see you tomorrow"), ==, 1);
}

/* Hyphens and apostrophes are inside words; curly quotes fold */
static void
test_word_chars(void)
{
	g_autoptr(GabbrevLearner) l = gabbrev_learner_new();

	feed(l, "it's well-known ");
	feed(l, "it\xe2\x80\x99s well-known ");
	g_assert_cmpuint(gabbrev_learner_count(l, "it's well-known"), ==, 2);
}

/* Phrases already in use, and triggers already taken, are respected */
static void
test_existing(void)
{
	g_autoptr(GabbrevLearner) l = gabbrev_learner_new();
	static const gchar * const taken[] = { ";tyvm", NULL };
	static const gchar * const repl[] = { "best regards", NULL };
	g_autoptr(GHashTable) t = set_of(taken);
	g_autoptr(GHashTable) r = set_of(repl);
	g_autoptr(GPtrArray) s = NULL;
	guint i;

	for (i = 0; i < 3; i++) {
		feed(l, "thank you very much.\n");
		feed(l, "Best regards.\n");
	}
	s = gabbrev_learner_suggest(l, 0, t, r);
	g_assert_cmpuint(s->len, ==, 1);
	g_assert_cmpstr(((GabbrevSuggestion *)g_ptr_array_index(s, 0))->phrase,
	                ==, "thank you very much");
	/* ;tyvm is taken, and shadows everything that starts with it:
	   two letters of every word */
	g_assert_cmpstr(((GabbrevSuggestion *)g_ptr_array_index(s, 0))->trigger,
	                ==, ";thyovemu");
}

/* Proposals: prefixes either way collide, batch proposals are unique */
static void
test_propose(void)
{
	static const gchar * const taken[] = { ";ty", ";tyvmuc", NULL };
	g_autoptr(GHashTable) t = set_of(taken);
	g_autofree gchar *a = gabbrev_learner_propose_trigger("thank you", NULL);
	g_autofree gchar *b = gabbrev_learner_propose_trigger("thank you", t);
	g_autofree gchar *c = gabbrev_learner_propose_trigger("thank you very much", t);

	g_assert_cmpstr(a, ==, ";ty");
	/* ;ty taken; ;tyo ... collide with ;ty as a prefix: two letters */
	g_assert_cmpstr(b, ==, ";thyo");
	/* Everything from the initials starts with ;ty */
	g_assert_cmpstr(c, ==, ";thyovemu");
}

/* Limit, and unique triggers in one batch */
static void
test_limit(void)
{
	g_autoptr(GabbrevLearner) l = gabbrev_learner_new();
	g_autoptr(GPtrArray) s = NULL;
	g_autoptr(GPtrArray) all = NULL;
	guint i;

	for (i = 0; i < 4; i++) {
		feed(l, "talk you later.\n");
		feed(l, "take you home.\n");
		feed(l, "tell your friends.\n");
	}
	s = gabbrev_learner_suggest(l, 2, NULL, NULL);
	g_assert_cmpuint(s->len, ==, 2);
	all = gabbrev_learner_suggest(l, 0, NULL, NULL);
	g_assert_cmpuint(all->len, ==, 3);
	for (i = 0; i < all->len; i++) {
		guint j;

		for (j = i + 1; j < all->len; j++) {
			const gchar *ti = ((GabbrevSuggestion *)g_ptr_array_index(all, i))->trigger;
			const gchar *tj = ((GabbrevSuggestion *)g_ptr_array_index(all, j))->trigger;

			g_assert_nonnull(ti);
			g_assert_nonnull(tj);
			g_assert_false(g_str_has_prefix(ti, tj));
			g_assert_false(g_str_has_prefix(tj, ti));
		}
	}
}

/* The table never grows past its cap; phrases seen often survive */
static void
test_bounded(void)
{
	g_autoptr(GabbrevLearner) l = gabbrev_learner_new();
	guint i;

	gabbrev_learner_set_limits(l, 0, 3, 0, 0, 50);
	for (i = 0; i < 5; i++)
		feed(l, "keep this phrase.\n");
	for (i = 0; i < 400; i++) {
		g_autofree gchar *w = g_strdup_printf("word%c%c ", 'a' + i % 26,
		                                      'a' + (i / 26) % 26);

		feed(l, "noise ");
		feed(l, w);
	}
	g_assert_cmpuint(gabbrev_learner_size(l), <=, 50);
	g_assert_cmpuint(gabbrev_learner_count(l, "keep this phrase"), ==, 5);
	gabbrev_learner_forget(l, "Keep This Phrase");
	g_assert_cmpuint(gabbrev_learner_count(l, "keep this phrase"), ==, 0);
	gabbrev_learner_clear(l);
	g_assert_cmpuint(gabbrev_learner_size(l), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gabbrev/learn/counts", test_counts);
	g_test_add_func("/gabbrev/learn/not-words", test_not_words);
	g_test_add_func("/gabbrev/learn/min-chars", test_min_chars);
	g_test_add_func("/gabbrev/learn/backspace", test_backspace);
	g_test_add_func("/gabbrev/learn/word-chars", test_word_chars);
	g_test_add_func("/gabbrev/learn/existing", test_existing);
	g_test_add_func("/gabbrev/learn/propose", test_propose);
	g_test_add_func("/gabbrev/learn/limit", test_limit);
	g_test_add_func("/gabbrev/learn/bounded", test_bounded);
	return g_test_run();
}
