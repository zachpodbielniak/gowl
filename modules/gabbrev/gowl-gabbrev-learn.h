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
 * gowl-gabbrev-learn.h - Phrases typed again and again.
 *
 * The learner is fed the same characters as the engine and counts the
 * phrases -- runs of MIN_WORDS to MAX_WORDS words -- that keep coming
 * back, so the module can say "you have typed `thank you very much'
 * nine times; `;tyvm'?".  It is opt-in, and built to be safe to leave
 * on:
 *
 *   - memory only: nothing is ever written to disk;
 *   - a WORD is letters, apostrophes and hyphens only.  A run with a
 *     digit or a symbol in it is not a word and breaks the phrase, so a
 *     password, a code or an address never becomes a phrase;
 *   - the module never feeds it from a denied window, a terminal, or
 *     synthetic input -- that policy is the module's; this file only
 *     counts;
 *   - the table is bounded: past MAX_ENTRIES the phrases seen once, and
 *     longest ago, are dropped first.
 *
 * Phrases are counted case-insensitively and shown as last typed.
 */

#ifndef GOWL_GABBREV_LEARN_H
#define GOWL_GABBREV_LEARN_H

#include <glib.h>

G_BEGIN_DECLS

#define GABBREV_LEARN_MIN_WORDS    (2)
#define GABBREV_LEARN_MAX_WORDS    (8)
#define GABBREV_LEARN_MIN_CHARS    (10)
#define GABBREV_LEARN_MIN_COUNT    (3)
#define GABBREV_LEARN_MAX_ENTRIES  (4000)

/* One phrase worth a trigger. */
typedef struct {
	gchar   *phrase;     /* as last typed */
	guint    count;
	guint    n_words;
	gint64   last;       /* Unix seconds it was last completed */
	gchar   *trigger;    /* a proposal, or NULL when none was free */
} GabbrevSuggestion;

void gabbrev_suggestion_free (GabbrevSuggestion *s);

typedef struct _GabbrevLearner GabbrevLearner;

GabbrevLearner *gabbrev_learner_new        (void);
void            gabbrev_learner_free       (GabbrevLearner *learner);
void            gabbrev_learner_set_limits (GabbrevLearner *learner,
                                            guint           min_words,
                                            guint           max_words,
                                            guint           min_chars,
                                            guint           min_count,
                                            guint           max_entries);
void            gabbrev_learner_feed       (GabbrevLearner *learner,
                                            gunichar        c,
                                            gint64          now);
void            gabbrev_learner_backspace  (GabbrevLearner *learner);
void            gabbrev_learner_break      (GabbrevLearner *learner);
gboolean        gabbrev_learner_forget     (GabbrevLearner *learner,
                                            const gchar    *phrase);
void            gabbrev_learner_clear      (GabbrevLearner *learner);
guint           gabbrev_learner_size       (GabbrevLearner *learner);
guint           gabbrev_learner_count      (GabbrevLearner *learner,
                                            const gchar    *phrase);
GPtrArray      *gabbrev_learner_take_ready (GabbrevLearner *learner);
GPtrArray      *gabbrev_learner_suggest    (GabbrevLearner *learner,
                                            guint           limit,
                                            GHashTable     *triggers,
                                            GHashTable     *replacements);
gchar          *gabbrev_learner_propose_trigger (const gchar *phrase,
                                            GHashTable     *taken);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GabbrevLearner, gabbrev_learner_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GabbrevSuggestion, gabbrev_suggestion_free)

G_END_DECLS

#endif /* GOWL_GABBREV_LEARN_H */
