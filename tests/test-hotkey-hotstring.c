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
 * test-hotkey-hotstring.c - Hotstrings handed to gabbrev.
 *
 * The translation (modules/hotkey/gowl-hotkey-hotstring.c) of every
 * AutoHotkey hotstring option into a gabbrev match, read back by
 * GABBREV'S OWN LOADER (modules/gabbrev/gowl-gabbrev-model.c, compiled
 * in too) -- so a key gabbrev does not know, or a value it reads
 * differently, fails here rather than at the user's keyboard.  Then the
 * options gabbrev cannot do warned about (never dropped silently), and
 * libgowl's named gabbrev sources: replacing and removing the hotkey
 * module's source leaves everyone else's alone.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include "gowl.h"
#include "gabbrev/gowl-gabbrev-private.h"
#include "../modules/hotkey/gowl-hotkey-model.c"
#include "../modules/hotkey/gowl-hotkey-send.c"
#include "../modules/hotkey/gowl-hotkey-hotstring.c"
#include "../modules/gabbrev/gowl-gabbrev-model.c"
#include "../modules/gabbrev/gowl-gabbrev-builtin.c"

typedef struct {
	HkSet      *hk;
	GabbrevSet *gab;
	GPtrArray  *warnings;
	gchar      *yaml;
} Both;

/* Hotstrings through the translator and back through gabbrev. */
static void
both_load(
	Both        *b,
	const gchar *section
){
	g_autoptr(HkLoader) loader = hk_loader_new();
	g_autoptr(GabbrevLoader) gl = gabbrev_loader_new();
	g_autoptr(GError) error = NULL;
	guint i;

	memset(b, 0, sizeof *b);
	g_assert_true(hk_loader_add_text(loader, section, "test", &error));
	b->hk = hk_loader_finish(loader);
	for (i = 0; i < b->hk->problems->len; i++)
		g_test_message("hotkey problem: %s",
		               (const gchar *)g_ptr_array_index(b->hk->problems, i));
	g_assert_cmpuint(b->hk->problems->len, ==, 0);
	b->warnings = g_ptr_array_new_with_free_func(g_free);
	b->yaml = hk_hotstrings_to_gabbrev(b->hk->hotstrings, b->hk, b->warnings);
	g_assert_nonnull(b->yaml);
	g_assert_true(gabbrev_loader_add_text(gl, b->yaml, NULL, "<code>"));
	b->gab = gabbrev_loader_finish(gl);
	for (i = 0; i < b->gab->problems->len; i++)
		g_test_message("gabbrev problem: %s",
		               (const gchar *)g_ptr_array_index(b->gab->problems, i));
	g_assert_cmpuint(b->gab->problems->len, ==, 0);
}

static void
both_clear(Both *b)
{
	hk_set_unref(b->hk);
	gabbrev_set_unref(b->gab);
	g_ptr_array_unref(b->warnings);
	g_free(b->yaml);
}

static GabbrevMatch *
match_of(
	Both        *b,
	const gchar *trigger
){
	GabbrevMatch *m = gabbrev_set_find_trigger(b->gab, trigger);

	if (m == NULL)
		g_test_message("no gabbrev match for %s in:\n%s", trigger, b->yaml);
	g_assert_nonnull(m);
	return m;
}

static gboolean
warned(
	Both        *b,
	const gchar *about
){
	guint i;

	for (i = 0; i < b->warnings->len; i++)
		if (strstr(g_ptr_array_index(b->warnings, i), about) != NULL)
			return TRUE;
	return FALSE;
}

static void
test_word_options(void)
{
	Both b;
	GabbrevMatch *m;

	both_load(&b, "hotstrings:\n"
	              "  - \"::btw::by the way\"\n"
	              "  - \":*:brb::be right back\"\n"
	              "  - \":?:ing::ING\"\n"
	              "  - \":*?:xx::yy\"\n");
	/* default: a whole word, fired by the end character */
	m = match_of(&b, "btw");
	g_assert_cmpstr(m->replace, ==, "by the way");
	g_assert_true(m->left_word);
	g_assert_true(m->right_word);
	/* *: no end character */
	m = match_of(&b, "brb");
	g_assert_true(m->left_word);
	g_assert_false(m->right_word);
	/* ?: inside a word */
	m = match_of(&b, "ing");
	g_assert_false(m->left_word);
	g_assert_true(m->right_word);
	m = match_of(&b, "xx");
	g_assert_false(m->left_word);
	g_assert_false(m->right_word);
	g_assert_cmpuint(b.warnings->len, ==, 0);
	both_clear(&b);
}

static void
test_case_options(void)
{
	Both b;

	both_load(&b, "hotstrings:\n"
	              "  - \"::afaik::as far as I know\"\n"
	              "  - \":C:AFAIK::As Far As I Know\"\n"
	              "  - \":C0:lol::laughing\"\n"
	              "  - \":C1:imo::in my opinion\"\n");
	/* AutoHotkey's default follows the case typed */
	g_assert_true(match_of(&b, "afaik")->propagate_case);
	g_assert_false(match_of(&b, "AFAIK")->propagate_case);
	g_assert_true(match_of(&b, "lol")->propagate_case);
	/* C1 cannot be had exactly: said so */
	g_assert_true(match_of(&b, "imo")->propagate_case);
	g_assert_true(warned(&b, "C1"));
	both_clear(&b);
}

static void
test_replacement_text(void)
{
	Both b;

	both_load(&b, "hotstrings:\n"
	              "  - \"::sig::Regards,{Enter}Zach{Tab}x{Space}y\"\n"
	              "  - \":R:raw::{Enter} stays\"\n"
	              "  - \":T:txt::{Tab} too\"\n"
	              "  - \"::q::say \\\"hi\\\" \\\\ {{} {}}\"\n"
	              "  - \"::uni::caf{U+00E9}\"\n");
	g_assert_cmpstr(match_of(&b, "sig")->replace, ==, "Regards,\nZach\tx y");
	g_assert_cmpstr(match_of(&b, "raw")->replace, ==, "{Enter} stays");
	g_assert_cmpstr(match_of(&b, "txt")->replace, ==, "{Tab} too");
	g_assert_cmpstr(match_of(&b, "q")->replace, ==, "say \"hi\" \\ { }");
	g_assert_cmpstr(match_of(&b, "uni")->replace, ==, "café");
	g_assert_cmpuint(b.warnings->len, ==, 0);
	both_clear(&b);
}

static void
test_dropped_keys_warned(void)
{
	Both b;

	both_load(&b, "hotstrings: [\"::x::a^c{Left}b\"]\n");
	g_assert_cmpstr(match_of(&b, "x")->replace, ==, "ab");
	g_assert_true(warned(&b, "left out"));
	both_clear(&b);
}

static void
test_execute(void)
{
	Both b;
	GabbrevMatch *m;
	GabbrevVar *v;

	both_load(&b, "hotstrings:\n"
	              "  - {hotstring: \":X:;up::\", id: up, do: {shell: uptime}}\n"
	              "  - {hotstring: \"::;dt::\", do: {send: \"{End}\"}}\n");
	m = match_of(&b, ";up");
	g_assert_cmpstr(m->replace, ==, "{{hk}}");
	g_assert_cmpuint(m->vars->len, ==, 1);
	v = g_ptr_array_index(m->vars, 0);
	g_assert_cmpstr(v->type, ==, HK_GABBREV_VAR_TYPE);
	/* do: on a hotstring is X even without the letter */
	m = match_of(&b, ";dt");
	g_assert_cmpuint(m->vars->len, ==, 1);
	both_clear(&b);
}

static void
test_unavailable_warned(void)
{
	Both b;

	both_load(&b, "hotstrings:\n"
	              "  - \":B0:keep::kept\"\n"
	              "  - \":O:omit::omitted\"\n"
	              "  - \":K10:slow::typed slowly\"\n"
	              "  - \":ZP5SI:quiet::fine\"\n"
	              "  - {hotstring: \":X:nothing::\"}\n");
	g_assert_true(warned(&b, "B0"));
	g_assert_true(warned(&b, "O is not available"));
	g_assert_true(warned(&b, "key_delay"));
	/* Z, P and SI are accepted without a word */
	g_assert_nonnull(match_of(&b, "quiet"));
	/* X with nothing to run is left out, and said */
	g_assert_null(gabbrev_set_find_trigger(b.gab, "nothing"));
	g_assert_true(warned(&b, "has none"));
	both_clear(&b);
}

static void
test_filters(void)
{
	Both b;
	GabbrevMatch *m;

	both_load(&b, "contexts: {terms: \"app-id=gst or app-id=foot\"}\n"
	              "hotstrings:\n"
	              "  - {hotstring: \"::ll::ls -la\", context: terms}\n"
	              "  - {hotstring: \"::gm::good morning\", "
	              "when: \"app-id=*slack*\", over: \"title=*\"}\n"
	              "  - {hotstring: \"::cx::nope\", context: elisp-made}\n");
	m = match_of(&b, "ll");
	g_assert_nonnull(m->filter);
	g_assert_nonnull(strstr(m->filter_text, "app-id=gst"));
	m = match_of(&b, "gm");
	g_assert_nonnull(strstr(m->filter_text, "slack"));
	g_assert_true(warned(&b, "over:, exists: and absent:"));
	/* a context gabbrev cannot know applies everywhere, warned */
	g_assert_null(match_of(&b, "cx")->filter);
	g_assert_true(warned(&b, "elisp-made"));
	both_clear(&b);
}

/* libgowl's named sources: one id replaced in place, removed alone. */
static void
test_named_sources(void)
{
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(GError) error = NULL;
	guint gen;

	gowl_gabbrev_clear_sources();
	g_assert_true(gowl_gabbrev_add_yaml("matches: [{trigger: mine, "
	                                    "replace: user's}]\n", &error));
	gen = gowl_gabbrev_get_generation();
	g_assert_true(gowl_gabbrev_add_yaml_source("hotkey",
		"matches: [{trigger: a1, replace: b}]\n", &error));
	g_assert_cmpuint(gowl_gabbrev_get_generation(), !=, gen);
	g_assert_true(gowl_gabbrev_add_yaml("matches: [{trigger: later, "
	                                    "replace: x}]\n", &error));
	/* again under the same id: replaced where it stood */
	g_assert_true(gowl_gabbrev_add_yaml_source("hotkey",
		"matches: [{trigger: a2, replace: b}]\n", &error));
	sources = gowl_gabbrev_dup_sources();
	g_assert_cmpuint(sources->len, ==, 3);
	g_assert_nonnull(strstr(((GowlGabbrevSource *)
	                         g_ptr_array_index(sources, 1))->text, "a2"));
	g_assert_cmpstr(((GowlGabbrevSource *)g_ptr_array_index(sources, 1))->id,
	                ==, "hotkey");
	g_clear_pointer(&sources, g_ptr_array_unref);
	/* removed: only it */
	g_assert_true(gowl_gabbrev_remove_source("hotkey"));
	g_assert_false(gowl_gabbrev_remove_source("hotkey"));
	sources = gowl_gabbrev_dup_sources();
	g_assert_cmpuint(sources->len, ==, 2);
	g_assert_nonnull(strstr(((GowlGabbrevSource *)
	                         g_ptr_array_index(sources, 0))->text, "mine"));
	/* bad YAML is the caller's error, and adds nothing */
	g_assert_false(gowl_gabbrev_add_yaml_source("hotkey", "matches: [", &error));
	g_assert_nonnull(error);
	gowl_gabbrev_clear_sources();
}

static void
test_none(void)
{
	g_autoptr(GPtrArray) warnings = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) empty = g_ptr_array_new();

	g_assert_null(hk_hotstrings_to_gabbrev(empty, NULL, warnings));
	g_assert_null(hk_hotstrings_to_gabbrev(NULL, NULL, warnings));
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/hotkey/hotstring/word-options", test_word_options);
	g_test_add_func("/hotkey/hotstring/case-options", test_case_options);
	g_test_add_func("/hotkey/hotstring/replacement-text",
	                test_replacement_text);
	g_test_add_func("/hotkey/hotstring/dropped-keys-warned",
	                test_dropped_keys_warned);
	g_test_add_func("/hotkey/hotstring/execute", test_execute);
	g_test_add_func("/hotkey/hotstring/unavailable-warned",
	                test_unavailable_warned);
	g_test_add_func("/hotkey/hotstring/filters", test_filters);
	g_test_add_func("/hotkey/hotstring/named-sources", test_named_sources);
	g_test_add_func("/hotkey/hotstring/none", test_none);

	return g_test_run();
}
