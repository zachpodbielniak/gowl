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
 * gowl-hotkey-hotstring.c - Translating hotstring options.
 *
 *   AutoHotkey             gabbrev
 *   (default)              left_word + right_word: a whole word, fired
 *                          by the end character typed after it
 *   *                      no right_word: fires on the last character
 *   ?                      no left_word: fires inside a word too
 *   C                      exact case (propagate_case off)
 *   (default) / C0         any case, and the replacement follows it
 *                          (propagate_case on)
 *   C1                     any case, replacement as written: gabbrev
 *                          cannot match case-insensitively without
 *                          following the case, so this is C0 (warned)
 *   R / T                  the replacement is literal text
 *   (default)              the replacement is Send syntax: {Enter}, {Tab}
 *                          and the {x} literals become text; other keys
 *                          are dropped (warned) -- a hotstring types text
 *   X                      runs its `do:' actions instead of typing
 *   B0, O                  not available: gabbrev always rubs the
 *                          abbreviation out and types the end character
 *                          back (warned)
 *   Z, Pn, SI/SE/SP, S, Kn accepted; gabbrev already resets after an
 *                          expansion, has no priorities among hotstrings
 *                          and paces keys with its own key_delay (Kn
 *                          is warned)
 *
 * `context:'/`when:' become the match's `filter:' -- gabbrev speaks the
 * same filter language.  `over:', `exists:' and `absent:' cannot be
 * said to gabbrev and are warned.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-hotkey"

#include "gowl-hotkey-hotstring.h"
#include "gowl-hotkey-send.h"

#include <string.h>
#include <xkbcommon/xkbcommon.h>

/* A YAML double-quoted scalar. */
static void
append_quoted(
	GString     *out,
	const gchar *s
){
	g_string_append_c(out, '"');
	for (; *s != '\0'; s++) {
		guchar c = (guchar)*s;

		switch (c) {
		case '"':  g_string_append(out, "\\\""); break;
		case '\\': g_string_append(out, "\\\\"); break;
		case '\n': g_string_append(out, "\\n");  break;
		case '\t': g_string_append(out, "\\t");  break;
		case '\r': g_string_append(out, "\\r");  break;
		default:
			if (c < 0x20)
				g_string_append_printf(out, "\\x%02x", c);
			else
				g_string_append_c(out, (gchar)c);
		}
	}
	g_string_append_c(out, '"');
}

typedef struct {
	gboolean star;      /* * */
	gboolean inside;    /* ? */
	gint     cased;     /* 0 C0, 1 C, 2 C1 */
	gboolean raw;       /* R or T */
	gboolean execute;   /* X */
	gboolean no_bs;     /* B0 */
	gboolean omit;      /* O */
	gboolean key_delay; /* Kn */
} Options;

/* AutoHotkey's options string: letters, each optionally followed by 0
 * (off) or a number (P, K). */
static void
parse_options(
	const gchar *opts,
	Options     *o,
	const gchar *where,
	GPtrArray   *warnings
){
	const gchar *p;

	memset(o, 0, sizeof *o);
	for (p = opts; p != NULL && *p != '\0'; ) {
		gchar c = g_ascii_toupper(*p);
		gboolean off = p[1] == '0';
		gboolean on1 = p[1] == '1';

		p++;
		switch (c) {
		case '*': o->star = !off; break;
		case '?': o->inside = !off; break;
		case 'B': o->no_bs = off; break;
		case 'C': o->cased = off ? 0 : on1 ? 2 : 1; break;
		case 'O': o->omit = !off; break;
		case 'R':
		case 'T': o->raw = !off; break;
		case 'X': o->execute = !off; break;
		case 'Z':
			break;
		case 'K':
			o->key_delay = TRUE;
			break;
		case 'P':
			break;
		case 'S':
			/* SI, SE, SP, or S alone */
			if (*p == 'I' || *p == 'E' || *p == 'P' || *p == 'i'
			    || *p == 'e' || *p == 'p')
				p++;
			break;
		case ' ':
			break;
		default:
			g_ptr_array_add(warnings, g_strdup_printf(
				"%s: unknown hotstring option `%c'", where, *(p - 1)));
		}
		/* skip a number after the letter */
		while (g_ascii_isdigit(*p))
			p++;
	}
}

/* Send syntax to the text it types; what is not text is dropped. */
static gchar *
send_to_text(
	const gchar *repl,
	const gchar *where,
	GPtrArray   *warnings
){
	g_autoptr(HkSendPlan) plan = NULL;
	GString *out;
	GError *e = NULL;
	gboolean dropped = FALSE;
	guint i;

	plan = hk_send_parse(repl, &e);
	if (plan == NULL) {
		g_ptr_array_add(warnings, g_strdup_printf(
			"%s: %s; typed as written", where, e->message));
		g_error_free(e);
		return g_strdup(repl);
	}
	out = g_string_new(NULL);
	for (i = 0; i < plan->events->len; i++) {
		HkSendEvent *ev = &g_array_index(plan->events, HkSendEvent, i);

		if (ev->kind == HK_SEND_CHAR && ev->mods == 0)
			g_string_append_unichar(out, ev->ch);
		else if (ev->kind == HK_SEND_TEXT)
			g_string_append(out, ev->text);
		else if (ev->kind == HK_SEND_KEY && ev->mods == 0 && ev->updown == 0
		         && ev->keysym == XKB_KEY_Return)
			g_string_append_c(out, '\n');
		else if (ev->kind == HK_SEND_KEY && ev->mods == 0 && ev->updown == 0
		         && ev->keysym == XKB_KEY_Tab)
			g_string_append_c(out, '\t');
		else if (ev->kind == HK_SEND_KEY && ev->mods == 0 && ev->updown == 0
		         && ev->keysym == XKB_KEY_space)
			g_string_append_c(out, ' ');
		else
			dropped = TRUE;
	}
	if (dropped)
		g_ptr_array_add(warnings, g_strdup_printf(
			"%s: a hotstring types text; keys other than {Enter}, {Tab} "
			"and {Space} were left out (use X and a send action)", where));
	return g_string_free(out, FALSE);
}

/* The filter a hotstring's context and `when:' give, or NULL. */
static gchar *
filter_of(
	HkHotstring *hs,
	HkSet       *set,
	const gchar *where,
	GPtrArray   *warnings
){
	GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
	const HkCondition *conds[2] = { NULL, NULL };
	gchar *joined;
	guint i;

	if (hs->context != NULL) {
		HkNamedCondition *nc = set != NULL
		                       ? g_hash_table_lookup(set->contexts, hs->context)
		                       : NULL;

		if (nc == NULL)
			g_ptr_array_add(warnings, g_strdup_printf(
				"%s: context `%s' is not in contexts: (a C or Elisp "
				"context cannot be said to gabbrev); it applies everywhere",
				where, hs->context));
		else
			conds[0] = &nc->cond;
	}
	conds[1] = &hs->cond;
	for (i = 0; i < 2; i++) {
		if (conds[i] == NULL)
			continue;
		if (conds[i]->when_text != NULL)
			g_ptr_array_add(parts, g_strdup_printf("(%s)",
			                                       conds[i]->when_text));
		if (conds[i]->over_text != NULL || conds[i]->exists_text != NULL
		    || conds[i]->absent_text != NULL)
			g_ptr_array_add(warnings, g_strdup_printf(
				"%s: over:, exists: and absent: do not apply to a hotstring "
				"(gabbrev filters the focused window only)", where));
	}
	if (parts->len == 0) {
		g_ptr_array_unref(parts);
		return NULL;
	}
	g_ptr_array_add(parts, NULL);
	joined = g_strjoinv(" and ", (gchar **)parts->pdata);
	g_ptr_array_unref(parts);
	return joined;
}

/**
 * hk_hotstrings_to_gabbrev: (skip)
 * @hotstrings: (element-type HkHotstring): the hotstrings
 * @set: (nullable): the set they came from, for named contexts
 * @warnings: where what cannot be translated exactly is said
 *
 * Returns: (transfer full) (nullable): a gabbrev match file, or %NULL
 *   with no hotstrings
 */
gchar *
hk_hotstrings_to_gabbrev(
	GPtrArray *hotstrings,
	HkSet     *set,
	GPtrArray *warnings
){
	GString *out;
	guint i;

	if (hotstrings == NULL || hotstrings->len == 0)
		return NULL;
	out = g_string_new("matches:\n");
	for (i = 0; i < hotstrings->len; i++) {
		HkHotstring *hs = g_ptr_array_index(hotstrings, i);
		g_autofree gchar *where = g_strdup_printf("%s: hotstring `%s'",
		                                          hs->origin, hs->raw);
		g_autofree gchar *text = NULL;
		g_autofree gchar *filter = NULL;
		Options o;

		parse_options(hs->options, &o, where, warnings);
		if (hs->actions != NULL && hs->actions->len > 0)
			o.execute = TRUE;
		if (o.execute && (hs->actions == NULL || hs->actions->len == 0)) {
			g_ptr_array_add(warnings, g_strdup_printf(
				"%s: X runs the hotstring's do: actions, and it has none; "
				"left out", where));
			continue;
		}
		if (o.no_bs)
			g_ptr_array_add(warnings, g_strdup_printf(
				"%s: B0 is not available -- gabbrev always rubs the "
				"abbreviation out", where));
		if (o.omit)
			g_ptr_array_add(warnings, g_strdup_printf(
				"%s: O is not available -- gabbrev types the end character "
				"back", where));
		if (o.cased == 2)
			g_ptr_array_add(warnings, g_strdup_printf(
				"%s: C1 is treated as C0 -- the replacement follows the "
				"case typed", where));
		if (o.key_delay)
			g_ptr_array_add(warnings, g_strdup_printf(
				"%s: K (key delay) is gabbrev's key_delay option, not a "
				"hotstring's", where));

		g_string_append(out, "  - trigger: ");
		append_quoted(out, hs->abbrev);
		g_string_append(out, "\n    id: ");
		append_quoted(out, hs->id);
		g_string_append(out, "\n");
		if (o.execute) {
			g_string_append(out, "    replace: \"{{hk}}\"\n"
			                     "    vars:\n"
			                     "      - name: hk\n"
			                     "        type: " HK_GABBREV_VAR_TYPE "\n"
			                     "        params:\n"
			                     "          id: ");
			append_quoted(out, hs->id);
			g_string_append(out, "\n");
		} else {
			text = o.raw ? g_strdup(hs->replacement)
			             : send_to_text(hs->replacement, where, warnings);
			if (strstr(text, "{{") != NULL)
				g_ptr_array_add(warnings, g_strdup_printf(
					"%s: `{{' in the replacement is a gabbrev variable", where));
			g_string_append(out, "    replace: ");
			append_quoted(out, text);
			g_string_append(out, "\n");
		}
		g_string_append_printf(out, "    left_word: %s\n",
		                       o.inside ? "false" : "true");
		g_string_append_printf(out, "    right_word: %s\n",
		                       o.star ? "false" : "true");
		g_string_append_printf(out, "    propagate_case: %s\n",
		                       o.cased == 1 ? "false" : "true");
		filter = filter_of(hs, set, where, warnings);
		if (filter != NULL) {
			g_string_append(out, "    filter: ");
			append_quoted(out, filter);
			g_string_append(out, "\n");
		}
	}
	return g_string_free(out, FALSE);
}
