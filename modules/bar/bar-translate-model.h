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

#ifndef BAR_TRANSLATE_MODEL_H
#define BAR_TRANSLATE_MODEL_H

#include <glib.h>

#include "barkit/gowl-bar-theme.h"

G_BEGIN_DECLS

/*
 * The pure half of the `translate' widget.
 *
 * Everything that decides WHAT to send and what an answer MEANS lives
 * here: the language table, engine selection, request building for
 * every engine, response parsing, chunking, the debounce decision, the
 * swap rule and the state file format.  Nothing here spawns, touches a
 * plugin, or reads a file, so tests/test-bar-translate.c reaches all of
 * it directly through bar.so with exactly the input that went wrong.
 */

/* The largest translation the widget keeps.  A runaway engine (or a
   model looping on itself) must not grow the compositor's memory
   without bound. */
#define BAR_TRANSLATE_MAX_TEXT (256 * 1024)

/* The largest source text remembered in the state file. */
#define BAR_TRANSLATE_MAX_STATE_TEXT (64 * 1024)

/**
 * BarTranslateLang:
 * @code: the code the widget stores and shows (ISO 639-1, or a
 *   regional tag such as `zh-CN')
 * @name: the English name
 * @native: the language's name for itself
 *
 * One row of the static language table.
 */
typedef struct {
	const gchar *code;
	const gchar *name;
	const gchar *native;
} BarTranslateLang;

/**
 * BarTranslateEngine:
 * @BAR_TRANSLATE_ENGINE_AUTO: pick one: ai if a backend is reachable,
 *   else LibreTranslate if a server is configured, else MyMemory
 * @BAR_TRANSLATE_ENGINE_AI: the shared bar AI backend (bar-ai.h)
 * @BAR_TRANSLATE_ENGINE_LIBRETRANSLATE: a LibreTranslate server
 * @BAR_TRANSLATE_ENGINE_MYMEMORY: the public MyMemory API
 * @BAR_TRANSLATE_ENGINE_GOOGLE: Google's keyless web endpoint; opt-in
 * @BAR_TRANSLATE_ENGINE_NONE: nothing usable; the panel says why
 */
typedef enum {
	BAR_TRANSLATE_ENGINE_AUTO = 0,
	BAR_TRANSLATE_ENGINE_AI,
	BAR_TRANSLATE_ENGINE_LIBRETRANSLATE,
	BAR_TRANSLATE_ENGINE_MYMEMORY,
	BAR_TRANSLATE_ENGINE_GOOGLE,
	BAR_TRANSLATE_ENGINE_NONE
} BarTranslateEngine;

/**
 * BarTranslateChunk:
 * @text: the piece to translate, without its trailing whitespace
 * @sep: the whitespace that followed it in the original, put back
 *   verbatim when the translations are joined
 */
typedef struct {
	gchar *text;
	gchar *sep;
} BarTranslateChunk;

/**
 * BarTranslateState:
 * @source: the source language code, or `auto'
 * @target: the target language code
 * @auto_translate: translate-while-typing
 * @text: (nullable): the last source text
 *
 * What the widget remembers across sessions.
 */
typedef struct {
	gchar    *source;
	gchar    *target;
	gboolean  auto_translate;
	gchar    *text;
} BarTranslateState;

/* --- Languages ---------------------------------------------------- */

const BarTranslateLang *bar_translate_languages (guint *n_langs);
const BarTranslateLang *bar_translate_language_find (const gchar *code);
const gchar *bar_translate_language_name (const gchar *code);
gchar *bar_translate_fold (const gchar *text);
gboolean bar_translate_language_matches (const BarTranslateLang *lang,
                                         const gchar            *filter);
GPtrArray *bar_translate_filter_languages (const gchar *filter,
                                           gboolean     include_auto);
gchar *bar_translate_pair_label (const gchar *source,
                                 const gchar *target,
                                 const gchar *detected);
const gchar *bar_translate_engine_code (BarTranslateEngine engine,
                                        const gchar       *code);

/* --- Engines ------------------------------------------------------ */

BarTranslateEngine bar_translate_parse_engine (const gchar *spec);
BarTranslateEngine bar_translate_resolve_engine (BarTranslateEngine requested,
                                                 gboolean           have_ai,
                                                 const gchar       *libre_url);
const gchar *bar_translate_engine_name (BarTranslateEngine engine);
gsize bar_translate_engine_limit (BarTranslateEngine engine);

/* --- Encoding ----------------------------------------------------- */

gchar *bar_translate_url_encode (const gchar *text);
gchar *bar_translate_json_quote (const gchar *text);
gchar *bar_translate_json_decode_string (const gchar  *p,
                                         const gchar **end);
gchar *bar_translate_json_field (const gchar *json, const gchar *key);
gint bar_translate_json_int (const gchar *json,
                             const gchar *key,
                             gint         fallback);
gchar *bar_translate_html_unescape (const gchar *text);
gchar *bar_translate_clean_text (const gchar *raw);

/* --- Requests ----------------------------------------------------- */

gchar **bar_translate_curl_argv (const gchar *curl,
                                 gint         timeout_s,
                                 const gchar *post_url);
gchar *bar_translate_libre_url (const gchar *base);
gchar *bar_translate_libre_body (const gchar *text,
                                 const gchar *source,
                                 const gchar *target,
                                 const gchar *api_key);
const gchar *bar_translate_mymemory_source (const gchar *source,
                                            gboolean    *assumed);
gchar *bar_translate_mymemory_config (const gchar *text,
                                      const gchar *source,
                                      const gchar *target,
                                      const gchar *email);
gchar *bar_translate_google_config (const gchar *text,
                                    const gchar *source,
                                    const gchar *target);

/* --- Responses ---------------------------------------------------- */

gint bar_translate_split_status (gchar *output);
gboolean bar_translate_parse_libre (const gchar  *body,
                                    gint          status,
                                    gchar       **text,
                                    gchar       **detected,
                                    gchar       **error);
gboolean bar_translate_parse_mymemory (const gchar  *body,
                                       gint          status,
                                       gchar       **text,
                                       gchar       **error);
gboolean bar_translate_parse_google (const gchar  *body,
                                     gint          status,
                                     gchar       **text,
                                     gchar       **detected,
                                     gchar       **error);

/* --- Chunking ----------------------------------------------------- */

GPtrArray *bar_translate_chunk (const gchar *text, gsize limit);
gchar *bar_translate_join (GPtrArray *chunks, const gchar * const *translations);

/* --- The AI engine ------------------------------------------------ */

gchar *bar_translate_ai_system (const gchar *source, const gchar *target);
gchar *bar_translate_ai_strip_detected (const gchar  *text,
                                        gchar       **detected);

/* --- Decisions ---------------------------------------------------- */

gboolean bar_translate_should_auto (gboolean auto_on,
                                    gint64   last_edit_us,
                                    gint64   now_us,
                                    gint     debounce_ms,
                                    gboolean text_changed);
gboolean bar_translate_swap (const gchar  *source,
                             const gchar  *target,
                             const gchar  *detected,
                             gchar       **new_source,
                             gchar       **new_target);

/* --- Colour ------------------------------------------------------ */

GowlBarColor bar_translate_panel_role (const gchar        *spec,
                                       GowlBarColor        fallback,
                                       const GowlBarTheme *theme);

/* --- State -------------------------------------------------------- */

void bar_translate_state_clear (BarTranslateState *state);
gchar *bar_translate_state_serialize (const BarTranslateState *state,
                                      gboolean                 remember_text);
gboolean bar_translate_state_parse (const gchar       *data,
                                    BarTranslateState *out);

G_END_DECLS

#endif /* BAR_TRANSLATE_MODEL_H */
