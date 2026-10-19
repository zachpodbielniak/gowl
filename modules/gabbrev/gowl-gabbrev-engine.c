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
 * gowl-gabbrev-engine.c - Matching typed text against triggers.
 *
 * Every keystroke reaches feed(), so it is cheap: triggers are indexed
 * by their last character (lower-cased), and a keystroke only compares
 * against the triggers ending in what was just typed.  Regex triggers
 * cannot be indexed and run on every character; there are few of them
 * in practice, and each sees only the regex-buffer tail.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-gabbrev"

#include "gowl-gabbrev-engine.h"

#include <string.h>

/* One trigger of one match, as characters. */
typedef struct {
	GabbrevMatch *match;     /* borrowed from the engine's array */
	gunichar     *chars;
	gunichar     *lower;
	glong         len;
} Entry;

/* One regex match, with its group names. */
typedef struct {
	GabbrevMatch *match;
	GStrv         groups;
} RegexEntry;

struct _GabbrevEngine {
	GPtrArray  *matches;       /* GabbrevMatch* refs */
	GHashTable *by_last;       /* gunichar (lower) -> GPtrArray<Entry*> */
	GPtrArray  *entries;       /* Entry*, owning */
	GPtrArray  *regexes;       /* RegexEntry*, owning */
	GArray     *buffer;        /* gunichar */
	gunichar   *separators;
	glong       n_separators;
	guint       regex_buffer;
};

static void
entry_free(gpointer p)
{
	Entry *e = p;

	g_free(e->chars);
	g_free(e->lower);
	g_free(e);
}

static void
regex_entry_free(gpointer p)
{
	RegexEntry *r = p;

	g_strfreev(r->groups);
	g_free(r);
}

/**
 * gabbrev_hit_free: (skip)
 * @hit: (transfer full): a hit
 */
void
gabbrev_hit_free(GabbrevHit *hit)
{
	if (hit == NULL)
		return;
	if (hit->match != NULL)
		gabbrev_match_unref(hit->match);
	g_free(hit->typed);
	g_free(hit->trailing);
	if (hit->captures != NULL)
		g_hash_table_unref(hit->captures);
	g_free(hit);
}

/**
 * gabbrev_engine_new: (skip)
 *
 * Returns: (transfer full): an engine with no matches and espanso's
 *   default separators
 */
GabbrevEngine *
gabbrev_engine_new(void)
{
	GabbrevEngine *e;
	GabbrevOptions defaults;

	e = g_new0(GabbrevEngine, 1);
	e->matches = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gabbrev_match_unref);
	e->by_last = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                                   (GDestroyNotify)g_ptr_array_unref);
	e->entries = g_ptr_array_new_with_free_func(entry_free);
	e->regexes = g_ptr_array_new_with_free_func(regex_entry_free);
	e->buffer = g_array_new(FALSE, FALSE, sizeof(gunichar));
	gabbrev_options_init_defaults(&defaults);
	gabbrev_engine_set_separators(e, defaults.word_separators);
	e->regex_buffer = defaults.regex_buffer;
	gabbrev_options_clear(&defaults);
	return e;
}

/**
 * gabbrev_engine_free: (skip)
 * @engine: (transfer full): an engine
 */
void
gabbrev_engine_free(GabbrevEngine *engine)
{
	if (engine == NULL)
		return;
	g_hash_table_unref(engine->by_last);
	g_ptr_array_unref(engine->entries);
	g_ptr_array_unref(engine->regexes);
	g_ptr_array_unref(engine->matches);
	g_array_unref(engine->buffer);
	g_free(engine->separators);
	g_free(engine);
}

/* The names of a regex's named groups: (?P<name>...) and (?<name>...) */
static GStrv
group_names(const gchar *regex)
{
	g_autoptr(GRegex) re = NULL;
	g_autoptr(GMatchInfo) mi = NULL;
	GPtrArray *out = g_ptr_array_new();

	re = g_regex_new("\\(\\?P?<([A-Za-z_][A-Za-z0-9_]*)>", 0, 0, NULL);
	if (regex != NULL && g_regex_match(re, regex, 0, &mi)) {
		while (g_match_info_matches(mi)) {
			g_ptr_array_add(out, g_match_info_fetch(mi, 1));
			g_match_info_next(mi, NULL);
		}
	}
	g_ptr_array_add(out, NULL);
	return (GStrv)g_ptr_array_free(out, FALSE);
}

/**
 * gabbrev_engine_set_matches: (skip)
 * @engine: an engine
 * @matches: (element-type GabbrevMatch): every match, load order; the
 *   engine takes its own references
 *
 * Indexes the triggers, and forgets what was typed.
 */
void
gabbrev_engine_set_matches(
	GabbrevEngine *engine,
	GPtrArray     *matches
){
	guint i;

	g_hash_table_remove_all(engine->by_last);
	g_ptr_array_set_size(engine->entries, 0);
	g_ptr_array_set_size(engine->regexes, 0);
	g_ptr_array_set_size(engine->matches, 0);

	for (i = 0; matches != NULL && i < matches->len; i++) {
		GabbrevMatch *m = g_ptr_array_index(matches, i);
		guint j;

		g_ptr_array_add(engine->matches, gabbrev_match_ref(m));
		for (j = 0; j < m->triggers->len; j++) {
			const gchar *t = g_ptr_array_index(m->triggers, j);
			Entry *e;
			GPtrArray *bucket;
			gpointer key;
			glong k;

			if (!g_utf8_validate(t, -1, NULL) || *t == '\0')
				continue;
			e = g_new0(Entry, 1);
			e->match = m;
			e->chars = g_utf8_to_ucs4_fast(t, -1, &e->len);
			e->lower = g_new(gunichar, e->len + 1);
			for (k = 0; k < e->len; k++)
				e->lower[k] = g_unichar_tolower(e->chars[k]);
			e->lower[e->len] = 0;
			g_ptr_array_add(engine->entries, e);

			key = GUINT_TO_POINTER(e->lower[e->len - 1]);
			bucket = g_hash_table_lookup(engine->by_last, key);
			if (bucket == NULL) {
				bucket = g_ptr_array_new();
				g_hash_table_insert(engine->by_last, key, bucket);
			}
			g_ptr_array_add(bucket, e);
		}
		if (m->regex != NULL) {
			RegexEntry *r = g_new0(RegexEntry, 1);

			r->match = m;
			r->groups = group_names(m->regex_text);
			g_ptr_array_add(engine->regexes, r);
		}
	}
	gabbrev_engine_reset(engine);
}

/**
 * gabbrev_engine_set_separators: (skip)
 * @engine: an engine
 * @separators: UTF-8; every character is one separator
 */
void
gabbrev_engine_set_separators(
	GabbrevEngine *engine,
	const gchar   *separators
){
	g_free(engine->separators);
	engine->separators = NULL;
	engine->n_separators = 0;
	if (separators != NULL && g_utf8_validate(separators, -1, NULL))
		engine->separators = g_utf8_to_ucs4_fast(separators, -1,
		                                         &engine->n_separators);
}

/**
 * gabbrev_engine_set_regex_buffer: (skip)
 * @engine: an engine
 * @chars: how many of the last characters a regex trigger sees
 */
void
gabbrev_engine_set_regex_buffer(
	GabbrevEngine *engine,
	guint          chars
){
	engine->regex_buffer = MAX(chars, 1);
}

/**
 * gabbrev_engine_reset: (skip)
 * @engine: an engine
 *
 * Forgets what was typed: the cursor went somewhere the engine cannot
 * follow.
 */
void
gabbrev_engine_reset(GabbrevEngine *engine)
{
	g_array_set_size(engine->buffer, 0);
}

/**
 * gabbrev_engine_backspace: (skip)
 * @engine: an engine
 *
 * The last character was rubbed out.
 */
void
gabbrev_engine_backspace(GabbrevEngine *engine)
{
	if (engine->buffer->len > 0)
		g_array_set_size(engine->buffer, engine->buffer->len - 1);
}

/**
 * gabbrev_engine_is_separator: (skip)
 * @engine: an engine
 * @c: a character
 *
 * Returns: %TRUE when @c separates words
 */
gboolean
gabbrev_engine_is_separator(
	GabbrevEngine *engine,
	gunichar       c
){
	glong i;

	for (i = 0; i < engine->n_separators; i++)
		if (engine->separators[i] == c)
			return TRUE;
	return FALSE;
}

/**
 * gabbrev_engine_get_text: (skip)
 * @engine: an engine
 *
 * Returns: (transfer full): what the engine believes was typed
 */
gchar *
gabbrev_engine_get_text(GabbrevEngine *engine)
{
	return g_ucs4_to_utf8((gunichar *)engine->buffer->data,
	                      (glong)engine->buffer->len, NULL, NULL, NULL);
}

/**
 * gabbrev_engine_get_length: (skip)
 * @engine: an engine
 *
 * Returns: how many characters are remembered
 */
guint
gabbrev_engine_get_length(GabbrevEngine *engine)
{
	return engine->buffer->len;
}

/* Does @e end at buffer index @end (exclusive)? */
static gboolean
entry_ends_at(
	GabbrevEngine *engine,
	const Entry   *e,
	glong          end
){
	const gunichar *buf = (const gunichar *)engine->buffer->data;
	gboolean ci = e->match->propagate_case;
	glong start = end - e->len;
	glong k;

	if (start < 0)
		return FALSE;
	for (k = 0; k < e->len; k++) {
		gunichar b = buf[start + k];

		if (ci ? g_unichar_tolower(b) != e->lower[k] : b != e->chars[k])
			return FALSE;
	}
	/* left word: the start of what was typed, or after a separator */
	if (e->match->left_word && start > 0
	    && !gabbrev_engine_is_separator(engine, buf[start - 1]))
		return FALSE;
	return TRUE;
}

/* The better of two candidates: the longer, then the later loaded. */
static gboolean
better(
	glong               len,
	const GabbrevMatch *m,
	glong               best_len,
	const GabbrevMatch *best
){
	if (best == NULL)
		return TRUE;
	if (len != best_len)
		return len > best_len;
	return m->order > best->order;
}

static gchar *
slice(
	GabbrevEngine *engine,
	glong          start,
	glong          end
){
	return g_ucs4_to_utf8((gunichar *)engine->buffer->data + start,
	                      end - start, NULL, NULL, NULL);
}

/**
 * gabbrev_engine_feed: (skip)
 * @engine: an engine
 * @c: the character just typed
 * @allow: (nullable): asked about every textual candidate
 * @user_data: for @allow
 *
 * Remembers @c and says whether it completed a trigger.  After a hit
 * the engine forgets what was typed: the replacement is not input.
 *
 * Returns: (transfer full) (nullable): the hit, or %NULL
 */
GabbrevHit *
gabbrev_engine_feed(
	GabbrevEngine    *engine,
	gunichar          c,
	GabbrevAllowFunc  allow,
	gpointer          user_data
){
	const Entry *best = NULL;
	glong best_len = 0;
	gboolean best_trailing = FALSE;
	const RegexEntry *best_re = NULL;
	g_autofree gchar *best_re_text = NULL;
	GHashTable *best_caps = NULL;
	GPtrArray *bucket;
	glong n;
	guint i;
	GabbrevHit *hit;

	g_array_append_val(engine->buffer, c);
	if (engine->buffer->len > GABBREV_BUFFER_MAX)
		g_array_remove_range(engine->buffer, 0,
		                     engine->buffer->len - GABBREV_BUFFER_MAX);
	n = (glong)engine->buffer->len;

	/* Triggers ending in @c that need no separator after them */
	bucket = g_hash_table_lookup(engine->by_last,
	                             GUINT_TO_POINTER(g_unichar_tolower(c)));
	for (i = 0; bucket != NULL && i < bucket->len; i++) {
		const Entry *e = g_ptr_array_index(bucket, i);

		if (e->match->right_word || !entry_ends_at(engine, e, n))
			continue;
		if (!better(e->len, e->match, best_len, best ? best->match : NULL))
			continue;
		if (allow != NULL && !allow(e->match, user_data))
			continue;
		best = e;
		best_len = e->len;
		best_trailing = FALSE;
	}

	/* Right-word triggers, completed by this separator */
	if (n >= 2 && gabbrev_engine_is_separator(engine, c)) {
		gunichar prev = g_array_index(engine->buffer, gunichar, n - 2);

		bucket = g_hash_table_lookup(engine->by_last,
		                             GUINT_TO_POINTER(g_unichar_tolower(prev)));
		for (i = 0; bucket != NULL && i < bucket->len; i++) {
			const Entry *e = g_ptr_array_index(bucket, i);

			if (!e->match->right_word || !entry_ends_at(engine, e, n - 1))
				continue;
			if (!better(e->len, e->match, best_len,
			            best ? best->match : NULL))
				continue;
			if (allow != NULL && !allow(e->match, user_data))
				continue;
			best = e;
			best_len = e->len;
			best_trailing = TRUE;
		}
	}

	/* Regex triggers, on the tail */
	if (engine->regexes->len > 0) {
		glong from = MAX(0, n - (glong)engine->regex_buffer);
		g_autofree gchar *tail = slice(engine, from, n);

		for (i = 0; tail != NULL && i < engine->regexes->len; i++) {
			const RegexEntry *r = g_ptr_array_index(engine->regexes, i);
			g_autoptr(GMatchInfo) mi = NULL;
			g_autofree gchar *whole = NULL;
			glong len;
			guint g;

			if (!g_regex_match(r->match->regex, tail, 0, &mi))
				continue;
			whole = g_match_info_fetch(mi, 0);
			if (whole == NULL || *whole == '\0')
				continue;
			len = g_utf8_strlen(whole, -1);
			if (!better(len, r->match, best_len,
			            best_re != NULL ? best_re->match
			            : best != NULL ? best->match : NULL))
				continue;
			if (allow != NULL && !allow(r->match, user_data))
				continue;
			best = NULL;
			best_re = r;
			best_len = len;
			best_trailing = FALSE;
			g_free(best_re_text);
			best_re_text = g_steal_pointer(&whole);
			if (best_caps != NULL)
				g_hash_table_unref(best_caps);
			best_caps = g_hash_table_new_full(g_str_hash, g_str_equal,
			                                  g_free, g_free);
			for (g = 0; r->groups[g] != NULL; g++) {
				gchar *v = g_match_info_fetch_named(mi, r->groups[g]);

				g_hash_table_insert(best_caps, g_strdup(r->groups[g]),
				                    v != NULL ? v : g_strdup(""));
			}
		}
	}

	if (best == NULL && best_re == NULL)
		return NULL;

	hit = g_new0(GabbrevHit, 1);
	if (best_re != NULL) {
		hit->match = gabbrev_match_ref(best_re->match);
		hit->typed = g_steal_pointer(&best_re_text);
		hit->n_delete = (guint)best_len;
		hit->captures = best_caps;
	} else {
		glong end = best_trailing ? n - 1 : n;

		hit->match = gabbrev_match_ref(best->match);
		hit->typed = slice(engine, end - best->len, end);
		hit->n_delete = (guint)best->len + (best_trailing ? 1 : 0);
		if (best_trailing)
			hit->trailing = g_ucs4_to_utf8(&c, 1, NULL, NULL, NULL);
		if (best->match->propagate_case)
			hit->typed_case = gabbrev_typed_case(hit->typed);
	}
	gabbrev_engine_reset(engine);
	return hit;
}

/* ── Case ─────────────────────────────────────────────────────────────── */

/**
 * gabbrev_typed_case: (skip)
 * @typed: a trigger as it was typed
 *
 * espanso's reading: every letter upper (two or more of them) is
 * uppercase; the first letter upper is capitalized; anything else is
 * as written.
 *
 * Returns: how @typed was cased
 */
GabbrevTypedCase
gabbrev_typed_case(const gchar *typed)
{
	const gchar *p;
	guint letters = 0;
	guint upper = 0;
	gboolean first_upper = FALSE;

	for (p = typed; p != NULL && *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar ch = g_utf8_get_char(p);

		if (!g_unichar_isalpha(ch))
			continue;
		if (letters == 0)
			first_upper = g_unichar_isupper(ch);
		letters++;
		if (g_unichar_isupper(ch))
			upper++;
	}
	if (letters >= 2 && upper == letters)
		return GABBREV_TYPED_UPPERCASE;
	if (first_upper)
		return GABBREV_TYPED_CAPITALIZED;
	return GABBREV_TYPED_AS_IS;
}

/* @text with the first letter of every word (or only the first word)
   upper-cased. */
static gchar *
capitalize(
	const gchar *text,
	gboolean     every_word
){
	GString *out = g_string_sized_new(strlen(text));
	gboolean at_start = TRUE;
	gboolean done = FALSE;
	const gchar *p;

	for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar ch = g_utf8_get_char(p);

		if (g_unichar_isalpha(ch)) {
			if (at_start && !done) {
				ch = g_unichar_totitle(ch);
				if (!every_word)
					done = TRUE;
			}
			at_start = FALSE;
		} else if (g_unichar_isspace(ch)) {
			at_start = TRUE;
		}
		g_string_append_unichar(out, ch);
	}
	return g_string_free(out, FALSE);
}

/**
 * gabbrev_apply_case: (skip)
 * @text: a replacement
 * @typed: how the trigger was typed
 * @style: the match's uppercase_style
 *
 * Returns: (transfer full): @text cased like the trigger was
 */
gchar *
gabbrev_apply_case(
	const gchar      *text,
	GabbrevTypedCase  typed,
	GabbrevCaseStyle  style
){
	switch (typed) {
	case GABBREV_TYPED_UPPERCASE:
		return g_utf8_strup(text, -1);
	case GABBREV_TYPED_CAPITALIZED:
		if (style == GABBREV_CASE_UPPERCASE)
			return g_utf8_strup(text, -1);
		return capitalize(text, style == GABBREV_CASE_CAPITALIZE_WORDS);
	case GABBREV_TYPED_AS_IS:
	default:
		return g_strdup(text);
	}
}
