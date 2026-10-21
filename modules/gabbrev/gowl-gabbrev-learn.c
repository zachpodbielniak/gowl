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
 * gowl-gabbrev-learn.c - Counting repeated phrases.  See the header.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-gabbrev"

#include <string.h>

#include "gowl-gabbrev-learn.h"

/* One phrase seen. */
typedef struct {
	gchar   *display;   /* as last typed */
	guint    count;
	guint    n_words;
	gint64   last;
	guint64  seq;       /* the learner's clock when last seen */
} Entry;

struct _GabbrevLearner {
	GHashTable *entries;      /* casefolded phrase -> Entry* */
	GString    *word;         /* the word being typed */
	gboolean    tainted;      /* it has a character no word has */
	GPtrArray  *window;       /* gchar*, the phrase's words so far */
	GPtrArray  *ready;        /* gchar*, phrases that just reached
	                             min_count, until taken */
	guint64     seq;
	guint       min_words;
	guint       max_words;
	guint       min_chars;
	guint       min_count;
	guint       max_entries;
};

static void
entry_free(gpointer p)
{
	Entry *e = p;

	g_free(e->display);
	g_free(e);
}

/**
 * gabbrev_suggestion_free:
 * @s: (nullable): a suggestion
 */
void
gabbrev_suggestion_free(GabbrevSuggestion *s)
{
	if (s == NULL)
		return;
	g_free(s->phrase);
	g_free(s->trigger);
	g_free(s);
}

/**
 * gabbrev_learner_new:
 *
 * Returns: (transfer full): a learner with the default limits
 */
GabbrevLearner *
gabbrev_learner_new(void)
{
	GabbrevLearner *l;

	l = g_new0(GabbrevLearner, 1);
	l->entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                   entry_free);
	l->word = g_string_new(NULL);
	l->window = g_ptr_array_new_with_free_func(g_free);
	l->ready = g_ptr_array_new_with_free_func(g_free);
	l->min_words = GABBREV_LEARN_MIN_WORDS;
	l->max_words = GABBREV_LEARN_MAX_WORDS;
	l->min_chars = GABBREV_LEARN_MIN_CHARS;
	l->min_count = GABBREV_LEARN_MIN_COUNT;
	l->max_entries = GABBREV_LEARN_MAX_ENTRIES;
	return l;
}

/**
 * gabbrev_learner_free:
 * @learner: (nullable): the learner
 */
void
gabbrev_learner_free(GabbrevLearner *learner)
{
	if (learner == NULL)
		return;
	g_hash_table_unref(learner->entries);
	g_string_free(learner->word, TRUE);
	g_ptr_array_unref(learner->window);
	g_ptr_array_unref(learner->ready);
	g_free(learner);
}

/**
 * gabbrev_learner_set_limits:
 * @learner: the learner
 * @min_words: the shortest phrase, in words (at least 2)
 * @max_words: the longest phrase, in words
 * @min_chars: the shortest phrase, in characters
 * @min_count: how often a phrase is typed before it is suggested
 * @max_entries: how many phrases are remembered at most
 *
 * A zero keeps that limit as it is.
 */
void
gabbrev_learner_set_limits(
	GabbrevLearner *learner,
	guint           min_words,
	guint           max_words,
	guint           min_chars,
	guint           min_count,
	guint           max_entries
){
	g_return_if_fail(learner != NULL);
	if (min_words != 0)
		learner->min_words = MAX(min_words, 2);
	if (max_words != 0)
		learner->max_words = max_words;
	if (learner->max_words < learner->min_words)
		learner->max_words = learner->min_words;
	if (min_chars != 0)
		learner->min_chars = min_chars;
	if (min_count != 0)
		learner->min_count = min_count;
	if (max_entries != 0)
		learner->max_entries = max_entries;
}

/* Past the cap, forget what was seen once and longest ago, then what
   was seen twice, and so on: the phrases worth keeping are the ones
   that came back. */
static void
evict(GabbrevLearner *l)
{
	guint floor_count = 1;

	while (g_hash_table_size(l->entries) > l->max_entries) {
		guint64 cutoff = l->seq - MIN(l->seq, (guint64)l->max_entries / 2);
		GHashTableIter it;
		gpointer value;
		guint removed = 0;

		g_hash_table_iter_init(&it, l->entries);
		while (g_hash_table_iter_next(&it, NULL, &value)) {
			Entry *e = value;

			if (e->count <= floor_count && e->seq <= cutoff) {
				g_hash_table_iter_remove(&it);
				removed++;
			}
		}
		if (removed == 0)
			floor_count++;
		if (floor_count > G_MAXUINT / 2)
			break;
	}
}

/* Every phrase the newest word ends: the last N words, for each N
   from min_words up to the whole window. */
static void
count_phrases(
	GabbrevLearner *l,
	gint64          now
){
	guint n;

	for (n = l->min_words; n <= l->window->len; n++) {
		g_autoptr(GString) phrase = g_string_new(NULL);
		g_autofree gchar *key = NULL;
		Entry *e;
		guint i;

		for (i = l->window->len - n; i < l->window->len; i++) {
			if (phrase->len > 0)
				g_string_append_c(phrase, ' ');
			g_string_append(phrase, g_ptr_array_index(l->window, i));
		}
		if ((guint)g_utf8_strlen(phrase->str, -1) < l->min_chars)
			continue;
		key = g_utf8_casefold(phrase->str, -1);
		e = g_hash_table_lookup(l->entries, key);
		if (e == NULL) {
			e = g_new0(Entry, 1);
			e->n_words = n;
			g_hash_table_insert(l->entries, g_steal_pointer(&key), e);
		}
		e->count++;
		e->last = now;
		e->seq = ++l->seq;
		g_free(e->display);
		e->display = g_strdup(phrase->str);
		/* Exactly at the threshold, so a phrase is announced once;
		   bounded, in case nobody ever takes them */
		if (e->count == l->min_count && l->ready->len < 32)
			g_ptr_array_add(l->ready, g_strdup(phrase->str));
	}
	evict(l);
}

/* The word typed so far is done.  @boundary: the phrase is done too
   (a full stop, a comma, a newline). */
static void
end_word(
	GabbrevLearner *l,
	gboolean        boundary,
	gint64          now
){
	gchar *start, *end;

	if (l->tainted) {
		/* Not a word: nothing on either side of it is one phrase */
		l->tainted = FALSE;
		g_string_truncate(l->word, 0);
		g_ptr_array_set_size(l->window, 0);
		return;
	}

	/* Hyphens and apostrophes belong inside a word, not around it */
	start = l->word->str;
	while (*start == '-' || *start == '\'')
		start++;
	end = l->word->str + l->word->len;
	while (end > start && (end[-1] == '-' || end[-1] == '\''))
		end--;
	if (end > start) {
		g_ptr_array_add(l->window, g_strndup(start, (gsize)(end - start)));
		if (l->window->len > l->max_words)
			g_ptr_array_remove_index(l->window, 0);
		count_phrases(l, now);
	}
	g_string_truncate(l->word, 0);
	if (boundary)
		g_ptr_array_set_size(l->window, 0);
}

/**
 * gabbrev_learner_feed:
 * @learner: the learner
 * @c: one typed character
 * @now: Unix seconds
 *
 * Letters, apostrophes and hyphens make words, a space ends one, and
 * sentence punctuation ends a phrase.  Anything else -- a digit, a
 * symbol -- makes the run it is in no word at all.
 */
void
gabbrev_learner_feed(
	GabbrevLearner *learner,
	gunichar        c,
	gint64          now
){
	g_return_if_fail(learner != NULL);

	if (g_unichar_isalpha(c) || c == '\'' || c == 0x2019 || c == '-') {
		if (!learner->tainted)
			g_string_append_unichar(learner->word, c == 0x2019 ? '\'' : c);
		return;
	}
	if (c == ' ' || c == '\t') {
		end_word(learner, FALSE, now);
		return;
	}
	if (c == '.' || c == ',' || c == '!' || c == '?' || c == ':'
	    || c == ';' || c == '\n' || c == '\r' || c == '"' || c == '('
	    || c == ')') {
		end_word(learner, TRUE, now);
		return;
	}
	learner->tainted = TRUE;
}

/**
 * gabbrev_learner_backspace:
 * @learner: the learner
 *
 * A typo fixed inside a word is fine; a Backspace past the start of
 * the word goes somewhere the learner cannot see, so the phrase ends.
 */
void
gabbrev_learner_backspace(GabbrevLearner *learner)
{
	const gchar *prev;

	g_return_if_fail(learner != NULL);
	if (learner->word->len == 0 || learner->tainted) {
		gabbrev_learner_break(learner);
		return;
	}
	prev = g_utf8_find_prev_char(learner->word->str,
	                             learner->word->str + learner->word->len);
	g_string_truncate(learner->word,
	                  prev != NULL ? (gsize)(prev - learner->word->str) : 0);
}

/**
 * gabbrev_learner_break:
 * @learner: the learner
 *
 * The cursor went somewhere unseen (a click, an arrow, another
 * window): nothing typed after this continues anything before it.
 */
void
gabbrev_learner_break(GabbrevLearner *learner)
{
	g_return_if_fail(learner != NULL);
	g_string_truncate(learner->word, 0);
	learner->tainted = FALSE;
	g_ptr_array_set_size(learner->window, 0);
}

/* Longest first, for gabbrev_learner_take_ready() */
static gint
compare_longer(
	const gchar *a,
	const gchar *b
){
	gsize la = strlen(a);
	gsize lb = strlen(b);

	if (la != lb)
		return la > lb ? -1 : 1;
	return strcmp(a, b);
}

/**
 * gabbrev_learner_forget:
 * @learner: the learner
 * @phrase: a phrase, any case
 *
 * Returns: %TRUE if @phrase was remembered
 */
gboolean
gabbrev_learner_forget(
	GabbrevLearner *learner,
	const gchar    *phrase
){
	g_autofree gchar *key = NULL;

	g_return_val_if_fail(learner != NULL, FALSE);
	if (phrase == NULL)
		return FALSE;
	key = g_utf8_casefold(phrase, -1);
	return g_hash_table_remove(learner->entries, key);
}

/**
 * gabbrev_learner_clear:
 * @learner: the learner
 *
 * Forgets every phrase and whatever was being typed.
 */
void
gabbrev_learner_clear(GabbrevLearner *learner)
{
	g_return_if_fail(learner != NULL);
	g_hash_table_remove_all(learner->entries);
	g_ptr_array_set_size(learner->ready, 0);
	gabbrev_learner_break(learner);
}

/**
 * gabbrev_learner_take_ready:
 * @learner: the learner
 *
 * The phrases that reached min_count since the last call -- the moment
 * to tell the user about one -- longest first.
 *
 * Returns: (transfer full) (element-type utf8): the phrases, maybe none
 */
GPtrArray *
gabbrev_learner_take_ready(GabbrevLearner *learner)
{
	GPtrArray *out;

	g_return_val_if_fail(learner != NULL, NULL);
	out = g_steal_pointer(&learner->ready);
	learner->ready = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_sort_values(out, (GCompareFunc)compare_longer);
	return out;
}

/**
 * gabbrev_learner_size:
 * @learner: the learner
 *
 * Returns: how many phrases are remembered
 */
guint
gabbrev_learner_size(GabbrevLearner *learner)
{
	g_return_val_if_fail(learner != NULL, 0);
	return g_hash_table_size(learner->entries);
}

/**
 * gabbrev_learner_count:
 * @learner: the learner
 * @phrase: a phrase, any case
 *
 * Returns: how often @phrase was typed, 0 if never
 */
guint
gabbrev_learner_count(
	GabbrevLearner *learner,
	const gchar    *phrase
){
	g_autofree gchar *key = NULL;
	Entry *e;

	g_return_val_if_fail(learner != NULL, 0);
	if (phrase == NULL)
		return 0;
	key = g_utf8_casefold(phrase, -1);
	e = g_hash_table_lookup(learner->entries, key);
	return e != NULL ? e->count : 0;
}

/* Does @cand collide with a trigger in @taken?  A trigger that is a
   prefix of another one shadows it -- the engine fires on the shorter
   one's last character -- so a prefix either way is a collision. */
static gboolean
collides(
	const gchar *cand,
	GHashTable  *taken
){
	GHashTableIter it;
	gpointer key;

	if (taken == NULL)
		return FALSE;
	g_hash_table_iter_init(&it, taken);
	while (g_hash_table_iter_next(&it, &key, NULL))
		if (g_str_has_prefix(key, cand) || g_str_has_prefix(cand, key))
			return TRUE;
	return FALSE;
}

/* The first @n letters of @word, lower-cased, apostrophes skipped. */
static void
append_letters(
	GString     *out,
	const gchar *word,
	guint        n
){
	const gchar *p;
	guint got = 0;

	for (p = word; *p != '\0' && got < n; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (!g_unichar_isalnum(c))
			continue;
		g_string_append_unichar(out, g_unichar_tolower(c));
		got++;
	}
}

/**
 * gabbrev_learner_propose_trigger:
 * @phrase: the phrase
 * @taken: (nullable): triggers already in use, gchar* keys
 *
 * A trigger for @phrase no existing one collides with: `;' and the
 * initials (`thank you very much' is `;tyvm'), then the initials with
 * more of the last word, then two letters of every word.
 *
 * Returns: (transfer full) (nullable): the trigger, or %NULL when
 *   every candidate collides
 */
gchar *
gabbrev_learner_propose_trigger(
	const gchar *phrase,
	GHashTable  *taken
){
	g_auto(GStrv) words = NULL;
	g_autoptr(GString) initials = g_string_new(";");
	guint n, i, extra, last_len;

	g_return_val_if_fail(phrase != NULL, NULL);
	words = g_strsplit(phrase, " ", -1);
	n = g_strv_length(words);
	if (n == 0)
		return NULL;
	for (i = 0; i < n; i++)
		append_letters(initials, words[i], 1);
	if (g_utf8_strlen(initials->str, -1) >= 3
	    && !collides(initials->str, taken))
		return g_strdup(initials->str);

	/* More of the last word: ;tyvmu, ;tyvmuc, ... */
	last_len = (guint)g_utf8_strlen(words[n - 1], -1);
	for (extra = 2; extra <= last_len; extra++) {
		g_autoptr(GString) cand = g_string_new(";");

		for (i = 0; i + 1 < n; i++)
			append_letters(cand, words[i], 1);
		append_letters(cand, words[n - 1], extra);
		if (g_utf8_strlen(cand->str, -1) >= 3 && !collides(cand->str, taken))
			return g_string_free(g_steal_pointer(&cand), FALSE);
	}

	/* Two letters of every word: ;thyovemu */
	{
		g_autoptr(GString) cand = g_string_new(";");

		for (i = 0; i < n; i++)
			append_letters(cand, words[i], 2);
		if (!collides(cand->str, taken))
			return g_string_free(g_steal_pointer(&cand), FALSE);
	}
	return NULL;
}

typedef struct {
	const gchar *key;
	Entry       *entry;
	gboolean     dropped;
} Candidate;

/* Best first: what saves the most typing, count times length */
static gint
compare_candidates(
	gconstpointer a,
	gconstpointer b
){
	const Candidate *ca = *(const Candidate * const *)a;
	const Candidate *cb = *(const Candidate * const *)b;
	guint64 sa = (guint64)ca->entry->count * strlen(ca->key);
	guint64 sb = (guint64)cb->entry->count * strlen(cb->key);

	if (sa != sb)
		return sa > sb ? -1 : 1;
	if (ca->entry->seq != cb->entry->seq)
		return ca->entry->seq > cb->entry->seq ? -1 : 1;
	return strcmp(ca->key, cb->key);
}

/* Is @inner a run of whole words of @outer? */
static gboolean
contains_words(
	const gchar *outer,
	const gchar *inner
){
	gsize len = strlen(inner);
	const gchar *p;

	for (p = strstr(outer, inner); p != NULL; p = strstr(p + 1, inner)) {
		gboolean left = p == outer || p[-1] == ' ';
		gboolean right = p[len] == '\0' || p[len] == ' ';

		if (left && right)
			return TRUE;
	}
	return FALSE;
}

/**
 * gabbrev_learner_suggest:
 * @learner: the learner
 * @limit: the most to return, 0 for all
 * @triggers: (nullable): triggers in use (gchar* keys), never proposed
 * @replacements: (nullable): replacement texts already expanded to,
 *   casefolded (gchar* keys): a phrase that already has a match is
 *   not suggested again
 *
 * The phrases typed at least min_count times, best first.  A phrase
 * that is nearly always typed as part of a longer one (`thank you'
 * inside `thank you very much') is left out in its favour.
 *
 * Returns: (transfer full) (element-type GabbrevSuggestion): suggestions
 */
GPtrArray *
gabbrev_learner_suggest(
	GabbrevLearner *learner,
	guint           limit,
	GHashTable     *triggers,
	GHashTable     *replacements
){
	g_autoptr(GPtrArray) cands = NULL;
	g_autoptr(GHashTable) taken = NULL;
	GPtrArray *out;
	GHashTableIter it;
	gpointer key, value;
	guint i, j;

	g_return_val_if_fail(learner != NULL, NULL);
	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gabbrev_suggestion_free);
	cands = g_ptr_array_new_with_free_func(g_free);

	/* Everything typed often enough that is not a match already */
	g_hash_table_iter_init(&it, learner->entries);
	while (g_hash_table_iter_next(&it, &key, &value)) {
		Entry *e = value;
		Candidate *c;

		if (e->count < learner->min_count)
			continue;
		if (replacements != NULL && g_hash_table_contains(replacements, key))
			continue;
		c = g_new0(Candidate, 1);
		c->key = key;
		c->entry = e;
		g_ptr_array_add(cands, c);
	}

	/* A sub-phrase mostly typed inside a longer candidate goes */
	for (i = 0; i < cands->len; i++) {
		Candidate *p = g_ptr_array_index(cands, i);

		for (j = 0; j < cands->len && !p->dropped; j++) {
			Candidate *q = g_ptr_array_index(cands, j);

			if (q->entry->n_words > p->entry->n_words
			    && (guint64)q->entry->count * 4 >= (guint64)p->entry->count * 3
			    && contains_words(q->key, p->key))
				p->dropped = TRUE;
		}
	}

	g_ptr_array_sort(cands, compare_candidates);

	/* Proposals must not collide with each other either */
	taken = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	if (triggers != NULL) {
		g_hash_table_iter_init(&it, triggers);
		while (g_hash_table_iter_next(&it, &key, NULL))
			g_hash_table_add(taken, g_strdup(key));
	}
	for (i = 0; i < cands->len; i++) {
		Candidate *c = g_ptr_array_index(cands, i);
		GabbrevSuggestion *s;

		if (c->dropped)
			continue;
		if (limit != 0 && out->len >= limit)
			break;
		s = g_new0(GabbrevSuggestion, 1);
		s->phrase = g_strdup(c->entry->display);
		s->count = c->entry->count;
		s->n_words = c->entry->n_words;
		s->last = c->entry->last;
		s->trigger = gabbrev_learner_propose_trigger(s->phrase, taken);
		if (s->trigger != NULL)
			g_hash_table_add(taken, g_strdup(s->trigger));
		g_ptr_array_add(out, s);
	}
	return out;
}
