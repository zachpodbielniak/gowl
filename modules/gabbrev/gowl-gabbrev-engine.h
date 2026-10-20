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
 * gowl-gabbrev-engine.h - What was typed, and which match it finishes.
 *
 * The engine keeps the characters typed into the focused window since
 * the last thing that moved the cursor somewhere it cannot see (a
 * click, an arrow, a shortcut, a new window), and after every
 * character asks whether a trigger has just been completed.  It knows
 * nothing about keys or windows: the module turns keys into characters
 * and decides which matches are allowed in the window; this file
 * decides which of the allowed ones fires.  Tests drive it with
 * strings.
 *
 * The rules, espanso's:
 *
 *   - a trigger fires on its last character, unless it is a right-word
 *     (or word) trigger, which fires on the separator typed after it --
 *     and that separator is typed again after the replacement;
 *   - a left-word (or word) trigger only fires at the start of a word:
 *     after a separator or at the start of what was typed;
 *   - with propagate_case, `;Today' and `;TODAY' fire `;today' too, and
 *     say how the case was typed;
 *   - a regex trigger is tried against the last regex-buffer characters
 *     and must end at the cursor; its named groups become variables;
 *   - when several fire, the longest typed text wins, and between
 *     equals the match loaded last (a user's over a built-in).
 */

#ifndef GOWL_GABBREV_ENGINE_H
#define GOWL_GABBREV_ENGINE_H

#include <glib.h>

#include "gowl-gabbrev-model.h"

G_BEGIN_DECLS

/* The most characters kept; older ones fall off the front. */
#define GABBREV_BUFFER_MAX (256)

typedef enum {
	GABBREV_TYPED_AS_IS = 0,     /* as the trigger is written */
	GABBREV_TYPED_CAPITALIZED,   /* first letter upper */
	GABBREV_TYPED_UPPERCASE      /* every letter upper */
} GabbrevTypedCase;

/* One completed trigger. */
typedef struct {
	GabbrevMatch     *match;      /* a reference */
	gchar            *typed;      /* the trigger as typed, no separator */
	guint             n_delete;   /* characters to rub out, separator too */
	gchar            *trailing;   /* the separator to type back, or NULL */
	GabbrevTypedCase  typed_case;
	GHashTable       *captures;   /* regex groups, gchar* -> gchar*; or NULL */
} GabbrevHit;

void gabbrev_hit_free (GabbrevHit *hit);

/* Asked once per candidate: may @match expand in the current window?
   The module's answer covers filters, profiles and search-only. */
typedef gboolean (*GabbrevAllowFunc) (const GabbrevMatch *match,
                                      gpointer            user_data);

typedef struct _GabbrevEngine GabbrevEngine;

GabbrevEngine *gabbrev_engine_new            (void);
void           gabbrev_engine_free           (GabbrevEngine       *engine);
void           gabbrev_engine_set_matches    (GabbrevEngine       *engine,
                                              GPtrArray           *matches);
void           gabbrev_engine_set_separators (GabbrevEngine       *engine,
                                              const gchar         *separators);
void           gabbrev_engine_set_regex_buffer (GabbrevEngine     *engine,
                                              guint                chars);
void           gabbrev_engine_reset          (GabbrevEngine       *engine);
void           gabbrev_engine_backspace      (GabbrevEngine       *engine);
gboolean       gabbrev_engine_is_separator   (GabbrevEngine       *engine,
                                              gunichar             c);
GabbrevHit    *gabbrev_engine_feed           (GabbrevEngine       *engine,
                                              gunichar             c,
                                              GabbrevAllowFunc     allow,
                                              gpointer             user_data);
GabbrevHit    *gabbrev_engine_complete       (GabbrevEngine       *engine,
                                              GabbrevAllowFunc     allow,
                                              gpointer             user_data);
gchar         *gabbrev_engine_last_word      (GabbrevEngine       *engine);
gchar         *gabbrev_engine_get_text       (GabbrevEngine       *engine);
guint          gabbrev_engine_get_length     (GabbrevEngine       *engine);

/* Case helpers, shared with the renderer */
GabbrevTypedCase gabbrev_typed_case          (const gchar         *typed);
gchar         *gabbrev_apply_case            (const gchar         *text,
                                              GabbrevTypedCase     typed,
                                              GabbrevCaseStyle     style);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GabbrevEngine, gabbrev_engine_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GabbrevHit, gabbrev_hit_free)

G_END_DECLS

#endif /* GOWL_GABBREV_ENGINE_H */
