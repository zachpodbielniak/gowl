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
 * test-gabbrev-engine.c - Which trigger a keystroke completes.
 *
 * The engine is fed characters, as the module feeds it the characters
 * keys produce, and says which match fires: espanso's rules for word
 * boundaries, case, regex triggers and precedence.
 */

#include <glib.h>
#include <string.h>

#include "gowl.h"
#include "../modules/gabbrev/gowl-gabbrev-model.c"
#include "../modules/gabbrev/gowl-gabbrev-builtin.c"
#include "../modules/gabbrev/gowl-gabbrev-engine.c"

/* An engine over the matches in @yaml. */
static GabbrevEngine *
engine_for(
	const gchar  *yaml,
	GabbrevSet  **out_set
){
	g_autoptr(GabbrevLoader) l = gabbrev_loader_new();
	GabbrevEngine *e = gabbrev_engine_new();
	GabbrevSet *set;

	g_assert_true(gabbrev_loader_add_text(l, yaml, NULL,
	                                      GABBREV_ORIGIN_RUNTIME));
	set = gabbrev_loader_finish(l);
	g_assert_cmpuint(set->problems->len, ==, 0);
	gabbrev_engine_set_matches(e, set->matches);
	*out_set = set;
	return e;
}

/* Types @text; returns the first hit (the rest is not typed). */
static GabbrevHit *
type(
	GabbrevEngine *e,
	const gchar   *text
){
	const gchar *p;

	for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
		GabbrevHit *h = gabbrev_engine_feed(e, g_utf8_get_char(p), NULL, NULL);

		if (h != NULL)
			return h;
	}
	return NULL;
}

static void
test_plain(void)
{
	g_autoptr(GabbrevSet) set = NULL;
	g_autoptr(GabbrevEngine) e = engine_for(
		"matches: [{trigger: ';today', replace: x}]", &set);
	g_autoptr(GabbrevHit) h = NULL;

	h = type(e, "it is ;today");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->typed, ==, ";today");
	g_assert_cmpuint(h->n_delete, ==, 6);
	g_assert_null(h->trailing);
	/* the buffer is forgotten after a hit */
	g_assert_cmpuint(gabbrev_engine_get_length(e), ==, 0);
}

static void
test_backspace(void)
{
	g_autoptr(GabbrevSet) set = NULL;
	g_autoptr(GabbrevEngine) e = engine_for(
		"matches: [{trigger: ';today', replace: x}]", &set);
	g_autoptr(GabbrevHit) h = NULL;

	g_assert_null(type(e, ";tosx"));
	gabbrev_engine_backspace(e);
	gabbrev_engine_backspace(e);
	h = type(e, "day");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->typed, ==, ";today");
}

/* word: the separator after it fires it, and is typed back */
static void
test_word(void)
{
	g_autoptr(GabbrevSet) set = NULL;
	g_autoptr(GabbrevEngine) e = engine_for(
		"matches: [{trigger: 'btw', replace: 'by the way', word: true}]",
		&set);
	GabbrevHit *h;

	/* inside a word: no */
	g_assert_null(type(e, "abtw "));
	gabbrev_engine_reset(e);
	/* not before the separator */
	g_assert_null(type(e, "btw"));
	h = type(e, ",");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->typed, ==, "btw");
	g_assert_cmpuint(h->n_delete, ==, 4);
	g_assert_cmpstr(h->trailing, ==, ",");
	gabbrev_hit_free(h);
	/* after a separator, mid-sentence */
	h = type(e, "ok btw\n");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->trailing, ==, "\n");
	gabbrev_hit_free(h);
}

static void
test_left_word(void)
{
	g_autoptr(GabbrevSet) set = NULL;
	g_autoptr(GabbrevEngine) e = engine_for(
		"matches: [{trigger: 'teh', replace: the, left_word: true}]", &set);
	GabbrevHit *h;

	g_assert_null(type(e, "footeh"));
	gabbrev_engine_reset(e);
	h = type(e, "a teh");
	g_assert_nonnull(h);
	g_assert_null(h->trailing);
	gabbrev_hit_free(h);
}

static void
test_case(void)
{
	g_autoptr(GabbrevSet) set = NULL;
	g_autoptr(GabbrevEngine) e = engine_for(
		"matches: [{trigger: ';hello', replace: 'hello world', "
		"propagate_case: true}, {trigger: ';exact', replace: x}]", &set);
	GabbrevHit *h;
	g_autofree gchar *cap = NULL;
	g_autofree gchar *up = NULL;
	g_autofree gchar *words = NULL;

	h = type(e, ";Hello");
	g_assert_nonnull(h);
	g_assert_cmpint(h->typed_case, ==, GABBREV_TYPED_CAPITALIZED);
	gabbrev_hit_free(h);
	h = type(e, ";HELLO");
	g_assert_nonnull(h);
	g_assert_cmpint(h->typed_case, ==, GABBREV_TYPED_UPPERCASE);
	gabbrev_hit_free(h);
	/* without propagate_case the case must match exactly */
	g_assert_null(type(e, ";EXACT"));

	cap = gabbrev_apply_case("hello world", GABBREV_TYPED_CAPITALIZED,
	                         GABBREV_CASE_NONE);
	g_assert_cmpstr(cap, ==, "Hello world");
	words = gabbrev_apply_case("hello world", GABBREV_TYPED_CAPITALIZED,
	                           GABBREV_CASE_CAPITALIZE_WORDS);
	g_assert_cmpstr(words, ==, "Hello World");
	up = gabbrev_apply_case("hello world", GABBREV_TYPED_UPPERCASE,
	                        GABBREV_CASE_NONE);
	g_assert_cmpstr(up, ==, "HELLO WORLD");
}

static void
test_regex(void)
{
	g_autoptr(GabbrevSet) set = NULL;
	g_autoptr(GabbrevEngine) e = engine_for(
		"matches: [{regex: ':add\\((?P<x>\\d+),(?P<y>\\d+)\\)', "
		"replace: '{{x}}'}]", &set);
	g_autoptr(GabbrevHit) h = NULL;

	h = type(e, "sum :add(12,30)");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->typed, ==, ":add(12,30)");
	g_assert_cmpuint(h->n_delete, ==, 11);
	g_assert_cmpstr(g_hash_table_lookup(h->captures, "x"), ==, "12");
	g_assert_cmpstr(g_hash_table_lookup(h->captures, "y"), ==, "30");
}

/* The longest typed trigger wins; between equals, the later match */
static void
test_precedence(void)
{
	g_autoptr(GabbrevSet) set = NULL;
	g_autoptr(GabbrevEngine) e = engine_for(
		"matches:\n"
		"  - {trigger: 'ab', replace: short}\n"
		"  - {trigger: 'xab', replace: long}\n"
		"  - {trigger: ';q', replace: first, id: first}\n"
		"  - {trigger: ';q', replace: second, id: second}\n", &set);
	GabbrevHit *h;

	h = type(e, "xab");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->match->replace, ==, "long");
	gabbrev_hit_free(h);
	h = type(e, ";q");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->match->id, ==, "second");
	gabbrev_hit_free(h);
}

/* What the module says no to never fires, and does not hide another */
static gboolean
deny_long(
	const GabbrevMatch *m,
	gpointer            data
){
	(void)data;
	return g_strcmp0(m->replace, "long") != 0;
}

static void
test_allow(void)
{
	g_autoptr(GabbrevSet) set = NULL;
	g_autoptr(GabbrevEngine) e = engine_for(
		"matches:\n"
		"  - {trigger: 'ab', replace: short}\n"
		"  - {trigger: 'xab', replace: long}\n", &set);
	GabbrevHit *h = NULL;
	const gchar *p;

	for (p = "xab"; *p != '\0' && h == NULL; p++)
		h = gabbrev_engine_feed(e, (gunichar)*p, deny_long, NULL);
	g_assert_nonnull(h);
	g_assert_cmpstr(h->match->replace, ==, "short");
	gabbrev_hit_free(h);
}

/* Non-ASCII triggers and separators */
static void
test_unicode(void)
{
	g_autoptr(GabbrevSet) set = NULL;
	g_autoptr(GabbrevEngine) e = engine_for(
		"matches: [{trigger: 'ünï', replace: x, word: true}]", &set);
	g_autoptr(GabbrevHit) h = NULL;

	h = type(e, "ünï\xc2\xa0");   /* a no-break space separates too */
	g_assert_nonnull(h);
	g_assert_cmpuint(h->n_delete, ==, 4);
	g_assert_cmpstr(h->trailing, ==, "\xc2\xa0");
}

/* The built-in set, end to end through the matcher */
static void
test_builtin_triggers(void)
{
	g_autoptr(GabbrevLoader) l = gabbrev_loader_new();
	g_autoptr(GabbrevSet) set = NULL;
	g_autoptr(GabbrevEngine) e = gabbrev_engine_new();
	GabbrevHit *h;

	gabbrev_loader_add_builtin(l);
	set = gabbrev_loader_finish(l);
	gabbrev_engine_set_matches(e, set->matches);
	h = type(e, "see ;htoday");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->match->id, ==, ";htoday");
	gabbrev_hit_free(h);
	h = type(e, ";shrug");
	g_assert_nonnull(h);
	gabbrev_hit_free(h);
	h = type(e, "a ;=> b");
	g_assert_nonnull(h);
	g_assert_cmpstr(h->match->id, ==, ";=>");
	gabbrev_hit_free(h);
}

/* The expand key: the trigger the text ends with, word rules waived */
static void
test_complete(void)
{
	g_autoptr(GabbrevSet) set = NULL;
	g_autoptr(GabbrevEngine) e = engine_for(
		"matches:\n"
		"  - {trigger: 'brb', replace: 'be right back', word: true}\n"
		"  - {trigger: ';date', replace: d}\n", &set);
	g_autoptr(GabbrevHit) h = NULL;
	g_autofree gchar *word = NULL;

	/* `brb' typed, no separator: the key fires it */
	g_assert_null(type(e, "say brb"));
	h = gabbrev_engine_complete(e, NULL, NULL);
	g_assert_nonnull(h);
	g_assert_cmpstr(h->typed, ==, "brb");
	g_assert_cmpuint(h->n_delete, ==, 3);
	g_assert_null(h->trailing);
	g_assert_cmpuint(gabbrev_engine_get_length(e), ==, 0);

	/* nothing to complete: the word being typed, `;' kept */
	g_assert_null(type(e, "see ;dat"));
	g_assert_null(gabbrev_engine_complete(e, NULL, NULL));
	word = gabbrev_engine_last_word(e);
	g_assert_cmpstr(word, ==, ";dat");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gabbrev/engine/plain", test_plain);
	g_test_add_func("/gabbrev/engine/backspace", test_backspace);
	g_test_add_func("/gabbrev/engine/word", test_word);
	g_test_add_func("/gabbrev/engine/left-word", test_left_word);
	g_test_add_func("/gabbrev/engine/case", test_case);
	g_test_add_func("/gabbrev/engine/regex", test_regex);
	g_test_add_func("/gabbrev/engine/precedence", test_precedence);
	g_test_add_func("/gabbrev/engine/allow", test_allow);
	g_test_add_func("/gabbrev/engine/unicode", test_unicode);
	g_test_add_func("/gabbrev/engine/builtin-triggers", test_builtin_triggers);
	g_test_add_func("/gabbrev/engine/complete", test_complete);
	return g_test_run();
}
