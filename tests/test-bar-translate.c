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
 * The `translate' widget, offline.
 *
 * The pure half (modules/bar/bar-translate-model.c) is compiled into
 * this test directly, as test-bar-util.c does with bar-util.c: it has
 * no bar.so dependencies.  The widget itself is driven through bar.so
 * with the harness, against stand-ins for curl, ai, emacsctl, wl-copy
 * and wl-paste written to a scratch directory.  The fake curl records
 * every argv and stdin and answers from tests/fixtures/bar/translate.
 *
 * What the suite holds the widget to:
 *
 *   THE TEXT NEVER TRAVELS IN ARGV --- not to curl, not to ai, not to
 *   wl-copy --- and neither does the LibreTranslate key.
 *
 *   A LATE ANSWER IS DROPPED.  A slow reply for an older text must not
 *   paint over the newer one.
 *
 *   THE SAME REQUEST IS NEVER SENT TWICE, and typing is debounced.
 *
 *   ENGINE ERRORS SAY WHY, and an exhausted MyMemory quota is not shown
 *   as a translation.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#define BAR_HARNESS_MODULE GOWL_TEST_BAR_MODULE
#include "bar-widget-harness.h"
#include "gowl-enums.h"

#include "../modules/bar/bar-translate-model.c"

#ifndef GOWL_TEST_BAR_MODULE
#define GOWL_TEST_BAR_MODULE "build/release/modules/bar.so"
#endif
#ifndef GOWL_TEST_FIXTURES
#define GOWL_TEST_FIXTURES "tests/fixtures/bar"
#endif

/* ── Helpers ─────────────────────────────────────────────────────── */

static gchar *
fixture_path(const gchar *name)
{
	return g_build_filename(GOWL_TEST_FIXTURES, "translate", name, NULL);
}

static gchar *
fixture(const gchar *name)
{
	g_autofree gchar *path = fixture_path(name);
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	return text;
}

/*
 * A fake curl.  Each call N leaves calls/N.argv (one arg per line),
 * calls/N.stdin and calls/N.url (from the config on stdin for a GET,
 * the last argument for a POST).  It answers with the file named in
 * `reply' and the status in `status' --- or, when `reply' is `echo',
 * with a LibreTranslate body translating q as "T:<q>", which is how
 * chunks and stale answers are told apart.
 */
static gchar *
fake_curl(BarHarness *h)
{
	g_autofree gchar *body = NULL;

	body = g_strdup_printf(
		"d='%s'\n"
		"mkdir -p \"$d/calls\"\n"
		"n=$(ls \"$d/calls\" | grep -c '\\.argv$')\n"
		"printf '%%s\\n' \"$@\" > \"$d/calls/$n.argv\"\n"
		"cat > \"$d/calls/$n.stdin\"\n"
		"case \" $* \" in\n"
		"*' --config - '*) url=$(sed -n 's/^url = \"\\(.*\\)\"$/\\1/p' "
		"\"$d/calls/$n.stdin\") ;;\n"
		"*) url=\"${@: -1}\" ;;\n"
		"esac\n"
		"printf '%%s\\n' \"$url\" > \"$d/calls/$n.url\"\n"
		"reply=$(cat \"$d/reply\" 2>/dev/null)\n"
		"status=$(cat \"$d/status\" 2>/dev/null || echo 200)\n"
		"if [ \"$reply\" = echo ]; then\n"
		"  q=$(sed -n 's/^{\"q\": \"\\([^\"]*\\)\".*/\\1/p' \"$d/calls/$n.stdin\")\n"
		"  printf '{\"translatedText\": \"T:%%s\"}' \"$q\"\n"
		"elif [ \"$reply\" = fail ]; then\n"
		"  echo 'curl: (6) Could not resolve host: lt.test' >&2\n"
		"  exit 6\n"
		"else\n"
		"  cat \"$reply\"\n"
		"fi\n"
		"printf '\\n%%s' \"$status\"\n", h->tmp);
	return bar_harness_script(h, "fake-curl", body);
}

static void
set_reply(BarHarness *h, const gchar *reply, const gchar *status)
{
	g_autofree gchar *rp = bar_harness_path(h, "reply");
	g_autofree gchar *sp = bar_harness_path(h, "status");
	g_autofree gchar *target = NULL;

	if (g_strcmp0(reply, "echo") == 0 || g_strcmp0(reply, "fail") == 0)
		target = g_strdup(reply);
	else
		target = fixture_path(reply);
	g_assert_true(g_file_set_contents(rp, target, -1, NULL));
	g_assert_true(g_file_set_contents(sp, status, -1, NULL));
}

static guint
n_calls(BarHarness *h)
{
	g_autofree gchar *dir = bar_harness_path(h, "calls");
	GDir *d;
	const gchar *name;
	guint n = 0;

	d = g_dir_open(dir, 0, NULL);
	if (d == NULL)
		return 0;
	while ((name = g_dir_read_name(d)) != NULL)
		if (g_str_has_suffix(name, ".argv"))
			n++;
	g_dir_close(d);
	return n;
}

static gchar *
call_file(BarHarness *h, guint n, const gchar *ext)
{
	g_autofree gchar *name = g_strdup_printf("calls/%u.%s", n, ext);
	g_autofree gchar *path = bar_harness_path(h, name);
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	return text;
}

/* The value of the panel item with @id (an entry's text), or NULL. */
static gchar *
panel_value(BarHarness *h, const gchar *id)
{
	g_autoptr(GowlBarPanel) panel = bar_harness_panel(h);
	GowlBarPanelItem *item;

	item = gowl_bar_panel_find(panel, id);
	if (item == NULL)
		return NULL;
	return g_strdup(gowl_bar_panel_item_get_value(item));
}

static gchar *
panel_prop(BarHarness *h, const gchar *id, const gchar *prop)
{
	g_autoptr(GowlBarPanel) panel = bar_harness_panel(h);
	GowlBarPanelItem *item;

	item = gowl_bar_panel_find(panel, id);
	if (item == NULL)
		return NULL;
	return g_strdup(gowl_bar_panel_item_get_prop(item, prop));
}

static gboolean
panel_says(BarHarness *h, const gchar *needle)
{
	g_autoptr(GowlBarPanel) panel = bar_harness_panel(h);

	return bar_harness_panel_mentions(panel, needle);
}

/* A widget with a fake curl on LibreTranslate, whose answers come from
   `reply'.  Returns FALSE (skipped) when bar.so is not built. */
static gboolean
up_libre(BarHarness *h)
{
	g_autofree gchar *curl = NULL;

	if (!bar_harness_up(h, "bar_register_translate_plugins", "translate"))
		return FALSE;
	curl = fake_curl(h);
	bar_harness_set(h, "curl", curl);
	bar_harness_set(h, "engine", "libretranslate");
	bar_harness_set(h, "libretranslate-url", "http://lt.test:5000/");
	bar_harness_set(h, "ai-backend", "none");
	bar_harness_activate(h);
	return TRUE;
}

static void
type_text(BarHarness *h, const gchar *text, gboolean submit)
{
	gowl_bar_plugin_panel_text(h->plugin, "source", text, submit);
}

/* ── Pure: languages ─────────────────────────────────────────────── */

/* Catches a mistyped native name (a byte sequence that is not UTF-8
   would reach pango) and a duplicated code (the picker would list a
   language twice and lang:<code> would be ambiguous). */
static void
test_language_table_is_sound(void)
{
	const BarTranslateLang *langs;
	g_autoptr(GHashTable) seen = NULL;
	guint n, i;

	langs = bar_translate_languages(&n);
	g_assert_cmpuint(n, >=, 95);
	seen = g_hash_table_new(g_str_hash, g_str_equal);
	for (i = 0; i < n; i++) {
		g_assert_true(g_utf8_validate(langs[i].name, -1, NULL));
		g_assert_true(g_utf8_validate(langs[i].native, -1, NULL));
		g_assert_false(g_hash_table_contains(seen, langs[i].code));
		g_hash_table_add(seen, (gpointer)langs[i].code);
	}
	g_assert_cmpstr(bar_translate_language_name("auto"), ==,
	                "Auto-detect");
	g_assert_cmpstr(bar_translate_language_name("ES"), ==, "Spanish");
	/* What engines say when they detect: bare and alternative codes. */
	g_assert_cmpstr(bar_translate_language_find("zh")->code, ==, "zh-CN");
	g_assert_cmpstr(bar_translate_language_find("iw")->code, ==, "he");
	g_assert_null(bar_translate_language_find("auto"));
	g_assert_null(bar_translate_language_find("klingon"));
}

static gboolean
rows_have(GPtrArray *rows, const gchar *code)
{
	guint i;

	for (i = 0; i < rows->len; i++)
		if (g_strcmp0(((const BarTranslateLang *)
		               g_ptr_array_index(rows, i))->code, code) == 0)
			return TRUE;
	return FALSE;
}

/* The filter matches by code, English name and native name, ignoring
   case and accents --- `francais' must find Français, and an exact code
   must come first so Return picks it. */
static void
test_language_filter(void)
{
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GPtrArray) by_code = NULL;
	g_autoptr(GPtrArray) by_name = NULL;
	g_autoptr(GPtrArray) by_native = NULL;
	g_autoptr(GPtrArray) accents = NULL;
	g_autoptr(GPtrArray) cyrillic = NULL;
	g_autoptr(GPtrArray) target = NULL;
	g_autoptr(GPtrArray) none = NULL;
	g_autofree gchar *folded = NULL;

	all = bar_translate_filter_languages(NULL, TRUE);
	g_assert_cmpstr(((const BarTranslateLang *)
	                 g_ptr_array_index(all, 0))->code, ==, "auto");

	by_code = bar_translate_filter_languages("de", FALSE);
	g_assert_cmpstr(((const BarTranslateLang *)
	                 g_ptr_array_index(by_code, 0))->code, ==, "de");

	by_name = bar_translate_filter_languages("  GERM ", FALSE);
	g_assert_true(rows_have(by_name, "de"));

	by_native = bar_translate_filter_languages("deutsch", FALSE);
	g_assert_true(rows_have(by_native, "de"));

	accents = bar_translate_filter_languages("francais", FALSE);
	g_assert_true(rows_have(accents, "fr"));

	cyrillic = bar_translate_filter_languages(
		"\xd0\xa0\xd0\xa3\xd0\xa1", FALSE);   /* "РУС" */
	g_assert_true(rows_have(cyrillic, "ru"));

	/* The target picker never offers auto. */
	target = bar_translate_filter_languages("auto", FALSE);
	g_assert_false(rows_have(target, "auto"));

	none = bar_translate_filter_languages("zzzzqq", TRUE);
	g_assert_cmpuint(none->len, ==, 0);

	folded = bar_translate_fold("\xc3\x89SPA\xc3\x91OL");   /* ÉSPAÑOL */
	g_assert_cmpstr(folded, ==, "espanol");
	g_free(folded);
	folded = bar_translate_fold("\xff\xfe");
	g_assert_cmpstr(folded, ==, "");
}

/* ── Pure: engines ───────────────────────────────────────────────── */

/* auto prefers the private routes and never picks Google by itself; an
   engine named explicitly that cannot work is NONE, not a silent
   switch to a service the user did not choose. */
static void
test_engine_selection(void)
{
	g_assert_cmpint(bar_translate_parse_engine(NULL), ==,
	                BAR_TRANSLATE_ENGINE_AUTO);
	g_assert_cmpint(bar_translate_parse_engine("LibreTranslate"), ==,
	                BAR_TRANSLATE_ENGINE_LIBRETRANSLATE);
	g_assert_cmpint(bar_translate_parse_engine("google"), ==,
	                BAR_TRANSLATE_ENGINE_GOOGLE);
	g_assert_cmpint(bar_translate_parse_engine("nonsense"), ==,
	                BAR_TRANSLATE_ENGINE_AUTO);

	g_assert_cmpint(bar_translate_resolve_engine(
		BAR_TRANSLATE_ENGINE_AUTO, TRUE, "http://x"), ==,
		BAR_TRANSLATE_ENGINE_AI);
	g_assert_cmpint(bar_translate_resolve_engine(
		BAR_TRANSLATE_ENGINE_AUTO, FALSE, "http://x"), ==,
		BAR_TRANSLATE_ENGINE_LIBRETRANSLATE);
	g_assert_cmpint(bar_translate_resolve_engine(
		BAR_TRANSLATE_ENGINE_AUTO, FALSE, ""), ==,
		BAR_TRANSLATE_ENGINE_MYMEMORY);
	g_assert_cmpint(bar_translate_resolve_engine(
		BAR_TRANSLATE_ENGINE_AI, FALSE, "http://x"), ==,
		BAR_TRANSLATE_ENGINE_NONE);
	g_assert_cmpint(bar_translate_resolve_engine(
		BAR_TRANSLATE_ENGINE_LIBRETRANSLATE, TRUE, NULL), ==,
		BAR_TRANSLATE_ENGINE_NONE);
	g_assert_cmpint(bar_translate_resolve_engine(
		BAR_TRANSLATE_ENGINE_GOOGLE, TRUE, "http://x"), ==,
		BAR_TRANSLATE_ENGINE_GOOGLE);

	g_assert_cmpuint(bar_translate_engine_limit(
		BAR_TRANSLATE_ENGINE_MYMEMORY), ==, 450);
	g_assert_cmpstr(bar_translate_engine_code(
		BAR_TRANSLATE_ENGINE_LIBRETRANSLATE, "zh-CN"), ==, "zh-Hans");
	g_assert_cmpstr(bar_translate_engine_code(
		BAR_TRANSLATE_ENGINE_GOOGLE, "zh-CN"), ==, "zh-CN");
}

/* ── Pure: requests ──────────────────────────────────────────────── */

/* Text that holds URL syntax must stay text: an `&' or `=' that went
   through unencoded would add parameters (`&langpair=' could even
   redirect the pair), and a `+' would become a space. */
static void
test_url_encoding(void)
{
	g_autofree gchar *enc = NULL;
	g_autofree gchar *cfg = NULL;
	g_autofree gchar *g = NULL;

	enc = bar_translate_url_encode("a&b=c+d e\nf|\xc3\xa9#");
	g_assert_cmpstr(enc, ==, "a%26b%3Dc%2Bd%20e%0Af%7C%C3%A9%23");

	cfg = bar_translate_mymemory_config("fish & chips=1+1",
	                                    "en", "fr", "me+x@example.org");
	g_assert_cmpstr(cfg, ==,
		"url = \"https://api.mymemory.translated.net/get?"
		"q=fish%20%26%20chips%3D1%2B1&langpair=en%7Cfr"
		"&de=me%2Bx%40example.org\"\n");

	/* Exactly one line: a newline in the text cannot start a second
	   curl config directive. */
	g = bar_translate_google_config("line one\nurl = \"file:///etc\"",
	                                "auto", "es");
	g_assert_cmpuint(strlen(g) - 1, ==, (gsize)(strchr(g, '\n') - g));
	g_assert_null(strstr(g, "\"file"));
	g_assert_true(g_str_has_prefix(g,
		"url = \"https://clients5.google.com/translate_a/t?"
		"client=dict-chrome-ex&sl=auto&tl=es&q=line%20one%0Aurl"));
}

/* The JSON body is valid whatever the text holds --- quotes,
   backslashes, control bytes --- and carries the key; a body broken by
   a quote would be a 400 for any text with dialogue in it. */
static void
test_json_body(void)
{
	g_autofree gchar *body = NULL;
	g_autofree gchar *nokey = NULL;
	g_autofree gchar *round = NULL;
	const gchar *q;

	body = bar_translate_libre_body("say \"hi\" \\ \x01\ttab\n\xd0\xb6",
	                                "auto", "en", "s3cret");
	g_assert_cmpstr(body, ==,
		"{\"q\": \"say \\\"hi\\\" \\\\ \\u0001\\ttab\\n\xd0\xb6\", "
		"\"source\": \"auto\", \"target\": \"en\", "
		"\"format\": \"text\", \"api_key\": \"s3cret\"}");

	/* And it decodes back to the same text. */
	q = strstr(body, "\"q\": ") + 5;
	round = bar_translate_json_decode_string(q, NULL);
	g_assert_cmpstr(round, ==, "say \"hi\" \\ \x01\ttab\n\xd0\xb6");

	nokey = bar_translate_libre_body("x", "fr", "en", "");
	g_assert_null(strstr(nokey, "api_key"));
}

/* The argv never carries text or a key: GET engines read their URL
   from stdin, POST engines their body.  The protocol is pinned, so a
   `file://' URL cannot read a local file into the panel. */
static void
test_curl_argv(void)
{
	g_auto(GStrv) get = NULL;
	g_auto(GStrv) post = NULL;
	g_autofree gchar *joined = NULL;
	g_autofree gchar *libre = NULL;

	get = bar_translate_curl_argv(NULL, 0, NULL);
	joined = g_strjoinv(" ", get);
	g_assert_cmpstr(get[0], ==, "curl");
	g_assert_nonnull(strstr(joined, "--config -"));
	g_assert_nonnull(strstr(joined, "--proto =http,https"));
	g_assert_nonnull(strstr(joined, "--max-time 15"));
	g_assert_null(strstr(joined, "http"
	                     "s://"));   /* no URL at all */

	post = bar_translate_curl_argv("/opt/curl", 7, "http://lt:5000/translate");
	g_assert_cmpstr(post[0], ==, "/opt/curl");
	g_assert_cmpstr(post[g_strv_length(post) - 1], ==,
	                "http://lt:5000/translate");
	g_assert_true(g_strv_contains((const gchar * const *)post, "@-"));
	g_assert_true(g_strv_contains((const gchar * const *)post, "7"));

	libre = bar_translate_libre_url(" https://lt.example.org// ");
	g_assert_cmpstr(libre, ==, "https://lt.example.org/translate");
	g_free(libre);
	libre = bar_translate_libre_url("http://lt:5000/translate");
	g_assert_cmpstr(libre, ==, "http://lt:5000/translate");
	g_assert_null(bar_translate_libre_url("file:///etc/passwd"));
	g_assert_null(bar_translate_libre_url(""));
}

/* ── Pure: responses ─────────────────────────────────────────────── */

/* LibreTranslate escapes every non-ASCII character, surrogate pairs
   included: a reader that skipped \u showed a Russian translation as a
   row of backslashes. */
static void
test_parse_libre(void)
{
	g_autofree gchar *ok = fixture("libre-ok.json");
	g_autofree gchar *bad = fixture("libre-error.json");
	gchar *text = NULL, *detected = NULL, *error = NULL;

	g_assert_true(bar_translate_parse_libre(ok, 200, &text, &detected,
	                                        &error));
	g_assert_cmpstr(text, ==, "Hello, world \xe2\x80\x94 "
		"\xd0\xb6\xd0\xb8\xd0\xb7\xd0\xbd\xd1\x8c \xf0\x9f\x98\x80"
		"\nsecond line");
	g_assert_cmpstr(detected, ==, "fr");
	g_assert_null(error);
	g_clear_pointer(&text, g_free);
	g_clear_pointer(&detected, g_free);

	g_assert_false(bar_translate_parse_libre(bad, 400, &text, &detected,
	                                         &error));
	g_assert_null(text);
	g_assert_nonnull(strstr(error, "ru is not supported"));
	g_assert_nonnull(strstr(error, "400"));
	g_clear_pointer(&error, g_free);

	/* A 200 that is not a translation (a proxy's HTML page). */
	g_assert_false(bar_translate_parse_libre("<html>hi</html>", 200,
	                                         &text, NULL, &error));
	g_assert_nonnull(strstr(error, "not a translation"));
	g_clear_pointer(&error, g_free);

	/* A lone surrogate becomes U+FFFD, never invalid UTF-8. */
	g_assert_true(bar_translate_parse_libre(
		"{\"translatedText\": \"a\\ud800b\"}", 200, &text, NULL, &error));
	g_assert_true(g_utf8_validate(text, -1, NULL));
	g_assert_cmpstr(text, ==, "a\xef\xbf\xbd" "b");
	g_free(text);
}

/* MyMemory says 200 and puts its ERROR where the translation goes.
   The quota message must be an error naming the fix, and HTML entities
   from its translation memory must be decoded. */
static void
test_parse_mymemory(void)
{
	g_autofree gchar *ok = fixture("mymemory-ok.json");
	g_autofree gchar *quota = fixture("mymemory-quota.json");
	g_autofree gchar *pair = fixture("mymemory-badpair.json");
	gchar *text = NULL, *error = NULL;

	g_assert_true(bar_translate_parse_mymemory(ok, 200, &text, &error));
	g_assert_cmpstr(text, ==, "Bonjour le monde, c'est \"bien\"");
	g_clear_pointer(&text, g_free);

	g_assert_false(bar_translate_parse_mymemory(quota, 200, &text,
	                                            &error));
	g_assert_null(text);
	g_assert_nonnull(strstr(error, "quota"));
	g_assert_nonnull(strstr(error, "mymemory-email"));
	g_assert_null(strstr(error, "MYMEMORY WARNING"));
	g_clear_pointer(&error, g_free);

	/* responseStatus as a STRING "403". */
	g_assert_false(bar_translate_parse_mymemory(pair, 200, &text,
	                                            &error));
	g_assert_nonnull(strstr(error, "INVALID LANGUAGE PAIR"));
	g_assert_nonnull(strstr(error, "403"));
	g_clear_pointer(&error, g_free);

	g_assert_false(bar_translate_parse_mymemory("", 503, &text, &error));
	g_assert_nonnull(strstr(error, "503"));
	g_free(error);
}

/* Both shapes Google answers in, and a captcha page that must not be
   shown as a translation. */
static void
test_parse_google(void)
{
	g_autofree gchar *autoj = fixture("google-auto.json");
	g_autofree gchar *src = fixture("google-source.json");
	g_autofree gchar *captcha = fixture("google-captcha.html");
	gchar *text = NULL, *detected = NULL, *error = NULL;

	g_assert_true(bar_translate_parse_google(autoj, 200, &text,
	                                         &detected, &error));
	g_assert_cmpstr(text, ==, "Hola mundo \"y\" m\xc3\xa1s");
	g_assert_cmpstr(detected, ==, "en");
	g_clear_pointer(&text, g_free);
	g_clear_pointer(&detected, g_free);

	g_assert_true(bar_translate_parse_google(src, 200, &text, &detected,
	                                         &error));
	g_assert_cmpstr(text, ==, "Hola mundo");
	g_assert_null(detected);
	g_clear_pointer(&text, g_free);

	g_assert_false(bar_translate_parse_google(captcha, 200, &text, NULL,
	                                          &error));
	g_clear_pointer(&error, g_free);
	g_assert_false(bar_translate_parse_google(autoj, 429, &text, NULL,
	                                          &error));
	g_assert_nonnull(strstr(error, "rate limited"));
	g_free(error);
}

/* The status line curl appends is split off; with no status line the
   output is left alone and the status is unknown. */
static void
test_split_status_and_clean(void)
{
	gchar buf[64];
	g_autofree gchar *clean = NULL;
	g_autofree gchar *huge = NULL;
	g_autofree gchar *capped = NULL;

	g_strlcpy(buf, "{\"a\":1}\n200", sizeof(buf));
	g_assert_cmpint(bar_translate_split_status(buf), ==, 200);
	g_assert_cmpstr(buf, ==, "{\"a\":1}");

	g_strlcpy(buf, "line\nnot-a-status", sizeof(buf));
	g_assert_cmpint(bar_translate_split_status(buf), ==, -1);
	g_assert_cmpstr(buf, ==, "line\nnot-a-status");

	/* Hostile bytes never reach pango. */
	clean = bar_translate_clean_text("ok \xff\xfe bytes");
	g_assert_true(g_utf8_validate(clean, -1, NULL));

	/* And a runaway answer is bounded, on a character boundary. */
	huge = g_strnfill(BAR_TRANSLATE_MAX_TEXT + 100, 'a');
	huge[BAR_TRANSLATE_MAX_TEXT - 1] = '\xd0';
	huge[BAR_TRANSLATE_MAX_TEXT] = '\xb6';
	capped = bar_translate_clean_text(huge);
	g_assert_cmpuint(strlen(capped), <=, BAR_TRANSLATE_MAX_TEXT);
	g_assert_true(g_utf8_validate(capped, -1, NULL));
}

/* ── Pure: chunking ──────────────────────────────────────────────── */

static gchar *
chunk_texts(GPtrArray *chunks)
{
	GString *s = g_string_new(NULL);
	guint i;

	for (i = 0; i < chunks->len; i++) {
		BarTranslateChunk *c = g_ptr_array_index(chunks, i);

		g_string_append_printf(s, "[%s]", c->text);
	}
	return g_string_free(s, FALSE);
}

/* Cuts land on paragraph, then sentence, then word boundaries; a word
   longer than the limit is cut at a CHARACTER boundary (never inside a
   UTF-8 sequence, which the engine would reject); and joining puts
   back the original whitespace, paragraph breaks included. */
static void
test_chunking(void)
{
	g_autoptr(GPtrArray) para = NULL;
	g_autoptr(GPtrArray) sent = NULL;
	g_autoptr(GPtrArray) word = NULL;
	g_autoptr(GPtrArray) huge = NULL;
	g_autoptr(GPtrArray) small = NULL;
	g_autoptr(GPtrArray) cjk = NULL;
	g_autofree gchar *t = NULL;
	g_autofree gchar *joined = NULL;
	g_autofree gchar *long_word = NULL;
	const gchar *tr[] = { "A", "B", "C", NULL };
	guint i;

	para = bar_translate_chunk("First para here.\n\nSecond one.", 20);
	t = chunk_texts(para);
	g_assert_cmpstr(t, ==, "[First para here.][Second one.]");
	g_assert_cmpstr(((BarTranslateChunk *)
	                 g_ptr_array_index(para, 0))->sep, ==, "\n\n");
	joined = bar_translate_join(para, tr);
	g_assert_cmpstr(joined, ==, "A\n\nB");
	g_clear_pointer(&t, g_free);
	g_clear_pointer(&joined, g_free);

	sent = bar_translate_chunk("One two. Three four. Five six.", 22);
	t = chunk_texts(sent);
	g_assert_cmpstr(t, ==, "[One two. Three four.][Five six.]");
	g_clear_pointer(&t, g_free);

	word = bar_translate_chunk("alpha beta gamma delta", 12);
	t = chunk_texts(word);
	g_assert_cmpstr(t, ==, "[alpha beta][gamma delta]");
	g_clear_pointer(&t, g_free);

	/* 30 two-byte characters, limit 7: every piece is whole chars. */
	long_word = g_strnfill(0, 'x');
	g_free(long_word);
	{
		GString *s = g_string_new(NULL);

		for (i = 0; i < 30; i++)
			g_string_append(s, "\xd0\xb6");
		long_word = g_string_free(s, FALSE);
	}
	huge = bar_translate_chunk(long_word, 7);
	for (i = 0; i < huge->len; i++) {
		BarTranslateChunk *c = g_ptr_array_index(huge, i);

		g_assert_true(g_utf8_validate(c->text, -1, NULL));
		g_assert_cmpuint(strlen(c->text), <=, 7);
		g_assert_cmpuint(strlen(c->text), >, 0);
	}
	g_assert_cmpuint(huge->len, ==, 10);

	/* A limit smaller than one character still makes progress. */
	small = bar_translate_chunk("\xe6\x97\xa5\xe6\x9c\xac", 1);
	g_assert_cmpuint(small->len, ==, 2);

	/* CJK full stops end sentences without a following space. */
	cjk = bar_translate_chunk("\xe4\xbd\xa0\xe5\xa5\xbd\xe3\x80\x82"
	                          "\xe5\x86\x8d\xe8\xa7\x81\xe3\x80\x82", 12);
	t = chunk_texts(cjk);
	g_assert_cmpstr(t, ==, "[\xe4\xbd\xa0\xe5\xa5\xbd\xe3\x80\x82]"
	                       "[\xe5\x86\x8d\xe8\xa7\x81\xe3\x80\x82]");
}

/* ── Pure: AI, debounce, swap, state ─────────────────────────────── */

/* The detected-language marker is taken off, and a marker still being
   streamed is hidden rather than flashed at the user. */
static void
test_ai_prompt_and_marker(void)
{
	g_autofree gchar *sys_auto = NULL;
	g_autofree gchar *sys_fixed = NULL;
	g_autofree gchar *t = NULL;
	gchar *det = NULL;

	sys_auto = bar_translate_ai_system("auto", "ru");
	g_assert_nonnull(strstr(sys_auto, "Russian (ru)"));
	g_assert_nonnull(strstr(sys_auto, "<<fr>>"));
	sys_fixed = bar_translate_ai_system("de", "en");
	g_assert_nonnull(strstr(sys_fixed, "from German (de) into English"));
	g_assert_null(strstr(sys_fixed, "<<"));

	t = bar_translate_ai_strip_detected("<<fr>>\nHello", &det);
	g_assert_cmpstr(t, ==, "Hello");
	g_assert_cmpstr(det, ==, "fr");
	g_clear_pointer(&t, g_free);
	g_clear_pointer(&det, g_free);

	t = bar_translate_ai_strip_detected("<<f", &det);
	g_assert_cmpstr(t, ==, "");
	g_assert_null(det);
	g_clear_pointer(&t, g_free);

	t = bar_translate_ai_strip_detected("<<xx>>Hi", &det);
	g_assert_cmpstr(t, ==, "Hi");
	g_assert_null(det);   /* not a language we know */
	g_clear_pointer(&t, g_free);

	t = bar_translate_ai_strip_detected("a << b >> c", &det);
	g_assert_cmpstr(t, ==, "a << b >> c");
}

/* Typing must pause for the debounce, the toggle must be on, and the
   text must have changed --- each alone is not enough. */
static void
test_debounce_decision(void)
{
	const gint64 s = G_USEC_PER_SEC;

	g_assert_false(bar_translate_should_auto(TRUE, 10 * s,
		10 * s + 599000, 600, TRUE));
	g_assert_true(bar_translate_should_auto(TRUE, 10 * s,
		10 * s + 600000, 600, TRUE));
	g_assert_false(bar_translate_should_auto(FALSE, 10 * s, 20 * s,
		600, TRUE));
	g_assert_false(bar_translate_should_auto(TRUE, 10 * s, 20 * s,
		600, FALSE));
	g_assert_false(bar_translate_should_auto(TRUE, 0, 20 * s, 600,
		TRUE));
	g_assert_true(bar_translate_should_auto(TRUE, 10 * s, 10 * s, -5,
		TRUE));
}

/* Swapping an auto source uses the detected language, and refuses
   when there is none (auto cannot become a target). */
static void
test_swap_rule(void)
{
	gchar *ns = NULL, *nt = NULL;

	g_assert_true(bar_translate_swap("de", "en", NULL, &ns, &nt));
	g_assert_cmpstr(ns, ==, "en");
	g_assert_cmpstr(nt, ==, "de");
	g_clear_pointer(&ns, g_free);
	g_clear_pointer(&nt, g_free);

	g_assert_false(bar_translate_swap("auto", "en", NULL, &ns, &nt));
	g_assert_null(ns);

	g_assert_true(bar_translate_swap("auto", "en", "fr", &ns, &nt));
	g_assert_cmpstr(ns, ==, "en");
	g_assert_cmpstr(nt, ==, "fr");
	g_clear_pointer(&ns, g_free);
	g_clear_pointer(&nt, g_free);

	/* Detected the target's own language: nothing to swap to. */
	g_assert_false(bar_translate_swap("auto", "en", "en", &ns, &nt));
}

/* A round trip keeps multi-line UTF-8 text; a hand-edited file with
   nonsense in it yields defaults, not a pair every engine refuses. */
static void
test_state_round_trip(void)
{
	BarTranslateState in, out;
	g_autofree gchar *data = NULL;
	g_autofree gchar *notext = NULL;
	g_autofree gchar *good = fixture("state-good.state");
	g_autofree gchar *bad = fixture("state-bad.state");

	memset(&in, 0, sizeof(in));
	in.source = g_strdup("auto");
	in.target = g_strdup("ru");
	in.auto_translate = FALSE;
	in.text = g_strdup("line one\nline = two [x]\n\xd0\xb6 # not a comment");
	data = bar_translate_state_serialize(&in, TRUE);
	g_assert_true(bar_translate_state_parse(data, &out));
	g_assert_cmpstr(out.source, ==, "auto");
	g_assert_cmpstr(out.target, ==, "ru");
	g_assert_false(out.auto_translate);
	g_assert_cmpstr(out.text, ==, in.text);
	bar_translate_state_clear(&out);

	/* remember-text off: the text is not written at all. */
	notext = bar_translate_state_serialize(&in, FALSE);
	g_assert_null(strstr(notext, "line one"));
	bar_translate_state_clear(&in);

	g_assert_true(bar_translate_state_parse(good, &out));
	g_assert_cmpstr(out.source, ==, "de");
	g_assert_cmpstr(out.target, ==, "ja");
	g_assert_cmpstr(out.text, ==, "Guten Tag\nzweite Zeile");
	bar_translate_state_clear(&out);

	g_assert_true(bar_translate_state_parse(bad, &out));
	g_assert_null(out.source);
	g_assert_null(out.target);
	g_assert_true(out.auto_translate);
	bar_translate_state_clear(&out);

	g_assert_false(bar_translate_state_parse("not a key file", &out));
	bar_translate_state_clear(&out);
}

/* ── The widget ──────────────────────────────────────────────────── */

/* The whole LibreTranslate path: the body carries text and key on
   stdin, argv carries neither, and the answer (with its detected
   language) lands in the translation pane. */
static void
test_widget_libre_round_trip(void)
{
	BarHarness h;
	g_autofree gchar *argv = NULL;
	g_autofree gchar *in = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *label = NULL;

	if (!up_libre(&h))
		return;
	bar_harness_set(&h, "libretranslate-key", "TOPSECRETKEY");
	set_reply(&h, "libre-ok.json", "200");

	type_text(&h, "Bonjour \"le\" monde & co", TRUE);
	g_assert_cmpuint(n_calls(&h), ==, 1);

	argv = call_file(&h, 0, "argv");
	in = call_file(&h, 0, "stdin");
	url = call_file(&h, 0, "url");
	g_assert_null(strstr(argv, "Bonjour"));
	g_assert_null(strstr(argv, "TOPSECRETKEY"));
	g_assert_nonnull(strstr(in, "\"q\": \"Bonjour \\\"le\\\" monde & co\""));
	g_assert_nonnull(strstr(in, "\"api_key\": \"TOPSECRETKEY\""));
	g_assert_cmpstr(url, ==, "http://lt.test:5000/translate\n");

	out = panel_value(&h, "translation");
	g_assert_true(g_str_has_prefix(out, "Hello, world"));
	/* Auto source: the chip shows what was detected. */
	g_assert_true(panel_says(&h, "Auto (French)"));

	bar_harness_poll(&h);
	label = gowl_bar_plugin_dup_label(h.plugin);
	g_assert_cmpstr(label, ==, "fr\xe2\x86\x92" "EN");

	bar_harness_down(&h);
}

/* Server errors and an unreachable server are shown with the reason
   and in the error colour, and never as a translation. */
static void
test_widget_errors(void)
{
	BarHarness h;
	g_autofree gchar *out = NULL;
	g_autofree gchar *tip = NULL;

	if (!up_libre(&h))
		return;

	set_reply(&h, "libre-error.json", "400");
	type_text(&h, "Hallo", TRUE);
	g_assert_true(panel_says(&h, "ru is not supported"));
	out = panel_value(&h, "translation");
	g_assert_cmpstr(out, ==, "");

	set_reply(&h, "fail", "000");
	type_text(&h, "Hallo Welt", TRUE);
	g_assert_true(panel_says(&h, "Could not resolve host"));

	bar_harness_poll(&h);
	tip = gowl_bar_plugin_dup_tooltip(h.plugin);
	g_assert_nonnull(strstr(tip, "Could not resolve host"));
	g_assert_cmpint(gowl_bar_plugin_get_color(h.plugin), ==,
	                GOWL_BAR_COLOR_RED);

	bar_harness_down(&h);
}

/* Long text is split and each piece sent separately; the pieces come
   back joined with the original paragraph break. */
static void
test_widget_chunks_long_text(void)
{
	BarHarness h;
	g_autofree gchar *out = NULL;
	GString *text;
	guint i;

	if (!up_libre(&h))
		return;
	set_reply(&h, "echo", "200");

	/* 9 kB: two requests at LibreTranslate's 5000, cut at the
	   paragraph break rather than mid-sentence. */
	text = g_string_new(NULL);
	for (i = 0; i < 900; i++)
		g_string_append(text, "word ");
	g_string_append(text, "end.\n\n");
	for (i = 0; i < 900; i++)
		g_string_append(text, "mot ");
	g_string_append(text, "fin.");
	type_text(&h, text->str, TRUE);
	g_string_free(text, TRUE);

	g_assert_cmpuint(n_calls(&h), ==, 2);
	out = panel_value(&h, "translation");
	g_assert_true(g_str_has_prefix(out, "T:word word"));
	g_assert_nonnull(strstr(out, "word end.\n\nT:mot mot"));
	g_assert_true(g_str_has_suffix(out, "mot fin."));

	bar_harness_down(&h);
}

/* MyMemory with an auto source: sent as English and SAID so; the
   quota reply is an error, not a translation. */
static void
test_widget_mymemory(void)
{
	BarHarness h;
	g_autofree gchar *in = NULL;
	g_autofree gchar *argv = NULL;
	g_autofree gchar *out = NULL;

	if (!up_libre(&h))
		return;
	bar_harness_set(&h, "engine", "mymemory");
	bar_harness_set(&h, "mymemory-email", "zach@example.org");
	bar_harness_set(&h, "target", "fr");
	set_reply(&h, "mymemory-ok.json", "200");

	type_text(&h, "Hello world", TRUE);
	in = call_file(&h, 0, "stdin");
	argv = call_file(&h, 0, "argv");
	g_assert_nonnull(strstr(in, "q=Hello%20world&langpair=en%7Cfr"
	                            "&de=zach%40example.org"));
	g_assert_null(strstr(argv, "Hello"));
	g_assert_null(strstr(argv, "zach@"));
	g_assert_true(panel_says(&h, "assumed English"));
	out = panel_value(&h, "translation");
	g_assert_cmpstr(out, ==, "Bonjour le monde, c'est \"bien\"");

	set_reply(&h, "mymemory-quota.json", "200");
	type_text(&h, "Goodbye", TRUE);
	g_assert_true(panel_says(&h, "quota"));
	g_assert_false(panel_says(&h, "MYMEMORY WARNING"));

	bar_harness_down(&h);
}

/* Google is opt-in and works when chosen. */
static void
test_widget_google(void)
{
	BarHarness h;
	g_autofree gchar *url = NULL;
	g_autofree gchar *out = NULL;

	if (!up_libre(&h))
		return;
	bar_harness_set(&h, "engine", "google");
	bar_harness_set(&h, "target", "es");
	set_reply(&h, "google-auto.json", "200");

	type_text(&h, "Hello world", TRUE);
	url = call_file(&h, 0, "url");
	g_assert_true(g_str_has_prefix(url,
		"https://clients5.google.com/translate_a/t?client=dict-chrome-ex"
		"&sl=auto&tl=es&q=Hello%20world"));
	out = panel_value(&h, "translation");
	g_assert_cmpstr(out, ==, "Hola mundo \"y\" m\xc3\xa1s");

	bar_harness_down(&h);
}

/* An answer for text the user has since replaced is dropped: the jobs
   are run in REVERSE, so the old one finishes last. */
static void
test_widget_stale_result_ignored(void)
{
	BarHarness h;
	g_autofree gchar *out = NULL;
	BarHarnessWork *newer;

	if (!up_libre(&h))
		return;
	set_reply(&h, "echo", "200");
	h.host->hold_work = TRUE;

	type_text(&h, "first", TRUE);
	type_text(&h, "second", TRUE);
	g_assert_cmpuint(h.host->held->len, ==, 2);

	newer = g_ptr_array_steal_index(h.host->held, 1);
	newer->func(newer->plugin, newer->user_data);
	newer->destroy(newer->user_data);
	g_object_unref(newer->plugin);
	g_free(newer);
	bar_harness_run_held(&h);

	out = panel_value(&h, "translation");
	g_assert_cmpstr(out, ==, "T:second");
	/* The superseded request did not even reach curl. */
	g_assert_cmpuint(n_calls(&h), ==, 1);

	bar_harness_down(&h);
}

/* Translate-while-typing waits for the pause, then translates once;
   the same text is never sent twice, even by Ctrl+Return. */
static void
test_widget_debounce_and_dedupe(void)
{
	BarHarness h;

	if (!up_libre(&h))
		return;
	set_reply(&h, "echo", "200");

	bar_harness_set(&h, "debounce-ms", "60000");
	type_text(&h, "hola", FALSE);
	bar_harness_poll(&h);
	g_assert_cmpuint(n_calls(&h), ==, 0);

	bar_harness_set(&h, "debounce-ms", "0");
	bar_harness_poll(&h);
	g_assert_cmpuint(n_calls(&h), ==, 1);
	bar_harness_poll(&h);
	g_assert_cmpuint(n_calls(&h), ==, 1);

	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_Return,
	                                        GOWL_KEY_MOD_CTRL, 1));
	g_assert_cmpuint(n_calls(&h), ==, 1);

	/* Translate-while-typing off (Ctrl+P): edits wait for a request. */
	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_p,
	                                        GOWL_KEY_MOD_CTRL, 1));
	type_text(&h, "hola mundo", FALSE);
	bar_harness_poll(&h);
	g_assert_cmpuint(n_calls(&h), ==, 1);
	type_text(&h, "hola mundo", TRUE);
	g_assert_cmpuint(n_calls(&h), ==, 2);

	bar_harness_down(&h);
}

/* Swap exchanges the languages AND the texts, without a request;
   with an auto source and nothing detected it refuses and says why. */
static void
test_widget_swap(void)
{
	BarHarness h;
	g_autofree gchar *src = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *rev1 = NULL;
	g_autofree gchar *rev2 = NULL;
	g_autofree gchar *label = NULL;

	if (!up_libre(&h))
		return;
	set_reply(&h, "echo", "200");

	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_s,
	                                        GOWL_KEY_MOD_CTRL, 1));
	g_assert_true(panel_says(&h, "Nothing detected yet"));

	bar_harness_set(&h, "source", "de");
	bar_harness_set(&h, "target", "en");
	type_text(&h, "Hallo", TRUE);
	g_assert_cmpuint(n_calls(&h), ==, 1);
	rev1 = panel_prop(&h, "source", GOWL_BAR_ENTRY_PROP_REVISION);

	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_s,
	                                        GOWL_KEY_MOD_CTRL, 1));
	src = panel_value(&h, "source");
	out = panel_value(&h, "translation");
	rev2 = panel_prop(&h, "source", GOWL_BAR_ENTRY_PROP_REVISION);
	g_assert_cmpstr(src, ==, "T:Hallo");
	g_assert_cmpstr(out, ==, "Hallo");
	g_assert_cmpstr(rev1, !=, rev2);

	bar_harness_poll(&h);
	label = gowl_bar_plugin_dup_label(h.plugin);
	g_assert_cmpstr(label, ==, "EN\xe2\x86\x92" "DE");
	/* The swapped state is already translated: no request. */
	g_assert_cmpuint(n_calls(&h), ==, 1);

	bar_harness_down(&h);
}

/* The picker: keys open it for either side, the filter narrows it,
   Return in the filter picks the best match, and the target side
   never offers auto. */
static void
test_widget_picker_and_keys(void)
{
	BarHarness h;
	g_autoptr(GowlBarPanel) p1 = NULL;
	g_autoptr(GowlBarPanel) p2 = NULL;
	g_autoptr(GowlBarPanel) p3 = NULL;
	g_autofree gchar *label = NULL;

	if (!up_libre(&h))
		return;

	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_l,
	                                        GOWL_KEY_MOD_CTRL, 1));
	p1 = bar_harness_panel(&h);
	g_assert_nonnull(gowl_bar_panel_find(p1, "filter"));
	g_assert_nonnull(gowl_bar_panel_find(p1, "lang:auto"));
	g_assert_null(gowl_bar_panel_find(p1, "source"));

	gowl_bar_plugin_panel_text(h.plugin, "filter", "deu", FALSE);
	p2 = bar_harness_panel(&h);
	g_assert_nonnull(gowl_bar_panel_find(p2, "lang:de"));
	g_assert_null(gowl_bar_panel_find(p2, "lang:fr"));
	gowl_bar_plugin_panel_text(h.plugin, "filter", "deu", TRUE);

	/* Ctrl+Shift+L arrives as an upper-case L. */
	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_L,
		GOWL_KEY_MOD_CTRL | GOWL_KEY_MOD_SHIFT, 0));
	p3 = bar_harness_panel(&h);
	g_assert_null(gowl_bar_panel_find(p3, "lang:auto"));
	gowl_bar_plugin_panel_action(h.plugin, "lang:ja", 0, 0, 1);

	bar_harness_poll(&h);
	label = gowl_bar_plugin_dup_label(h.plugin);
	g_assert_cmpstr(label, ==, "DE\xe2\x86\x92" "JA");

	/* Picking the other side's language swaps rather than making a
	   pair from a language into itself. */
	gowl_bar_plugin_panel_action(h.plugin, "langs", 2, 0, 1);
	gowl_bar_plugin_panel_action(h.plugin, "lang:de", 0, 0, 1);
	bar_harness_poll(&h);
	g_free(label);
	label = gowl_bar_plugin_dup_label(h.plugin);
	g_assert_cmpstr(label, ==, "JA\xe2\x86\x92" "DE");

	/* F1 toggles the key list; keys that are not ours are refused so
	   the host can close the panel and pass them on. */
	g_assert_false(panel_says(&h, "Ctrl+Shift+X"));
	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_F1, 0, 1));
	g_assert_true(panel_says(&h, "Ctrl+Shift+X"));
	g_assert_false(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_a, 0, 1));
	g_assert_false(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_s,
		GOWL_KEY_MOD_CTRL | GOWL_KEY_MOD_ALT, 1));
	g_assert_false(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_l,
		GOWL_KEY_MOD_LOGO, 1));
	/* Ctrl+1 is claimed (not closing the panel) with a pointer to Tab. */
	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_1,
	                                        GOWL_KEY_MOD_CTRL, 1));
	g_assert_true(panel_says(&h, "Tab and Shift+Tab"));

	bar_harness_down(&h);
}

/* Ctrl+Shift+V puts the clipboard into the source pane (bumping its
   revision so the host takes it) and translates it on the next tick;
   Ctrl+Shift+C hands the translation to wl-copy on STDIN; Ctrl+Shift+X
   clears both panes. */
static void
test_widget_clipboard_flow(void)
{
	BarHarness h;
	g_autofree gchar *paste = NULL;
	g_autofree gchar *copy = NULL;
	g_autofree gchar *copied_path = NULL;
	g_autofree gchar *copy_args_path = NULL;
	g_autofree gchar *copied = NULL;
	g_autofree gchar *copy_args = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *src = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *rev_before = NULL;
	g_autofree gchar *rev_after = NULL;

	if (!up_libre(&h))
		return;
	set_reply(&h, "echo", "200");

	paste = bar_harness_script(&h, "fake-wl-paste",
		"[ \"$1\" = --no-newline ] || exit 9\nprintf 'Guten Morgen'");
	copied_path = bar_harness_path(&h, "copied");
	copy_args_path = bar_harness_path(&h, "copy-args");
	body = g_strdup_printf("printf '%%s\\n' \"$@\" > '%s'\ncat > '%s'",
	                       copy_args_path, copied_path);
	copy = bar_harness_script(&h, "fake-wl-copy", body);
	{
		g_autofree gchar *pcmd = g_strdup_printf(
			"%s --no-newline --type text", paste);

		bar_harness_set(&h, "paste-command", pcmd);
	}
	bar_harness_set(&h, "copy-command", copy);

	rev_before = panel_prop(&h, "source", GOWL_BAR_ENTRY_PROP_REVISION);
	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_V,
		GOWL_KEY_MOD_CTRL | GOWL_KEY_MOD_SHIFT, 0));
	src = panel_value(&h, "source");
	rev_after = panel_prop(&h, "source", GOWL_BAR_ENTRY_PROP_REVISION);
	g_assert_cmpstr(src, ==, "Guten Morgen");
	g_assert_cmpstr(rev_before, !=, rev_after);

	bar_harness_poll(&h);
	out = panel_value(&h, "translation");
	g_assert_cmpstr(out, ==, "T:Guten Morgen");

	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_C,
		GOWL_KEY_MOD_CTRL | GOWL_KEY_MOD_SHIFT, 0));
	g_assert_true(g_file_get_contents(copied_path, &copied, NULL, NULL));
	g_assert_true(g_file_get_contents(copy_args_path, &copy_args, NULL,
	                                  NULL));
	g_assert_cmpstr(copied, ==, "T:Guten Morgen");
	g_assert_null(strstr(copy_args, "Guten"));
	g_assert_true(panel_says(&h, "copied to the clipboard"));

	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_X,
		GOWL_KEY_MOD_CTRL | GOWL_KEY_MOD_SHIFT, 0));
	g_free(src);
	g_free(out);
	src = panel_value(&h, "source");
	out = panel_value(&h, "translation");
	g_assert_cmpstr(src, ==, "");
	g_assert_cmpstr(out, ==, "");

	bar_harness_down(&h);
}

/* Missing tools are named with their Fedora package; nothing spins or
   crashes. */
static void
test_widget_missing_dependencies(void)
{
	BarHarness h;
	g_autofree gchar *tip = NULL;

	if (!up_libre(&h))
		return;
	bar_harness_set(&h, "curl", "/nonexistent/curl");
	bar_harness_set(&h, "paste-command", "/nonexistent/wl-paste");

	bar_harness_poll(&h);
	g_assert_true(panel_says(&h, "Fedora package: curl"));
	tip = gowl_bar_plugin_dup_tooltip(h.plugin);
	g_assert_nonnull(strstr(tip, "curl"));

	type_text(&h, "Hallo", TRUE);
	g_assert_true(panel_says(&h, "not installed (Fedora package: curl)"));

	g_assert_true(gowl_bar_plugin_panel_key(h.plugin, XKB_KEY_V,
		GOWL_KEY_MOD_CTRL | GOWL_KEY_MOD_SHIFT, 0));
	g_assert_true(panel_says(&h, "wl-clipboard"));

	/* engine: libretranslate with no server is explained. */
	bar_harness_set(&h, "libretranslate-url", "");
	g_assert_true(panel_says(&h, "libretranslate-url"));

	bar_harness_down(&h);
}

/* The AI engine through ai-glib's CLI: the prompt asks for the target,
   the text goes on stdin, and the detected-language marker is taken
   off the answer. */
static void
test_widget_ai_cli(void)
{
	BarHarness h;
	g_autofree gchar *ai = NULL;
	g_autofree gchar *argfile = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *args = NULL;
	g_autofree gchar *out = NULL;

	if (!bar_harness_up(&h, "bar_register_translate_plugins", "translate"))
		return;
	argfile = bar_harness_path(&h, "ai-args");
	body = g_strdup_printf("printf '%%s\\n' \"$@\" > '%s'\n"
	                       "cat >/dev/null\nprintf '<<de>>\\nGood day\\n'",
	                       argfile);
	ai = bar_harness_script(&h, "fake-ai", body);
	bar_harness_set(&h, "ai-backend", "ai");
	bar_harness_set(&h, "ai-command", ai);
	bar_harness_set(&h, "target", "en");
	bar_harness_activate(&h);

	type_text(&h, "Guten Tag, geheim", TRUE);
	bar_harness_poll(&h);
	out = panel_value(&h, "translation");
	g_assert_cmpstr(out, ==, "Good day");
	g_assert_true(panel_says(&h, "Auto (German)"));

	g_assert_true(g_file_get_contents(argfile, &args, NULL, NULL));
	g_assert_nonnull(strstr(args, "English (en)"));
	g_assert_null(strstr(args, "geheim"));

	bar_harness_down(&h);
}

/* The cmacs route: emacsctl starts the request and returns; the answer
   arrives later as pushed `translate.ai-*' settings, and only the
   CURRENT request's answer is taken. */
static void
test_widget_ai_cmacs(void)
{
	BarHarness h;
	g_autofree gchar *ctl = NULL;
	g_autofree gchar *formfile = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *out = NULL;
	const gchar *p, *q;

	if (!bar_harness_up(&h, "bar_register_translate_plugins", "translate"))
		return;
	formfile = bar_harness_path(&h, "form");
	body = g_strdup_printf("[ \"$1\" = eval ] || exit 9\n"
	                       "printf '%%s' \"$2\" > '%s'\necho t", formfile);
	ctl = bar_harness_script(&h, "fake-emacsctl", body);
	bar_harness_set(&h, "ai-backend", "cmacs");
	bar_harness_set(&h, "engine", "ai");
	bar_harness_set(&h, "emacsctl", ctl);
	bar_harness_set(&h, "source", "fr");
	bar_harness_activate(&h);

	type_text(&h, "Bonjour", TRUE);
	g_assert_true(g_file_get_contents(formfile, &form, NULL, NULL));
	g_assert_true(g_str_has_prefix(form,
	                               "(cmacs-gowl-bar-ai-run \"translate\" \""));
	g_assert_nonnull(strstr(form, "\"Bonjour\""));
	p = form + strlen("(cmacs-gowl-bar-ai-run \"translate\" \"");
	q = strchr(p, '"');
	id = g_strndup(p, (gsize)(q - p));

	/* A late answer for some other request: ignored. */
	gowl_bar_plugin_set_setting(h.plugin, "ai-id", "translate-0-dead");
	gowl_bar_plugin_set_setting(h.plugin, "ai-state", "done");
	gowl_bar_plugin_set_setting(h.plugin, "ai-text", "stale");
	gowl_bar_plugin_configure(h.plugin, NULL);
	out = panel_value(&h, "translation");
	g_assert_cmpstr(out, ==, "");
	g_clear_pointer(&out, g_free);

	gowl_bar_plugin_set_setting(h.plugin, "ai-id", id);
	gowl_bar_plugin_set_setting(h.plugin, "ai-state", "done");
	gowl_bar_plugin_set_setting(h.plugin, "ai-text", "Hello");
	gowl_bar_plugin_configure(h.plugin, NULL);
	out = panel_value(&h, "translation");
	g_assert_cmpstr(out, ==, "Hello");

	bar_harness_down(&h);
}

/* The pair, toggle and text survive a restart through the state file,
   which is private (0600). */
static void
test_widget_state_persists(void)
{
	BarHarness h;
	g_autofree gchar *path = NULL;
	g_autofree gchar *src = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *good = NULL;
	GStatBuf st;

	if (!up_libre(&h))
		return;
	set_reply(&h, "echo", "200");
	bar_harness_poll(&h);   /* loads (nothing) */

	gowl_bar_plugin_panel_action(h.plugin, "langs", 2, 0, 1);
	gowl_bar_plugin_panel_action(h.plugin, "lang:ru", 0, 0, 1);
	type_text(&h, "remember me", FALSE);
	bar_harness_poll(&h);   /* saves */

	path = g_build_filename(h.host->state_dir, "translate.state", NULL);
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);

	/* A fresh widget over a state file from a previous session. */
	good = fixture("state-good.state");
	g_assert_true(g_file_set_contents(path, good, -1, NULL));
	gowl_bar_plugin_set_host(h.plugin, NULL);
	g_clear_object(&h.plugin);
	h.plugin = gowl_bar_registry_instantiate(h.registry, "translator",
	                                         NULL);
	g_assert_nonnull(h.plugin);
	gowl_bar_plugin_set_host(h.plugin, GOWL_BAR_HOST(h.host));
	/* Polled before activation, as a racing host might: no crash. */
	bar_harness_poll(&h);
	bar_harness_activate(&h);
	bar_harness_poll(&h);
	bar_harness_poll(&h);

	src = panel_value(&h, "source");
	g_assert_cmpstr(src, ==, "Guten Tag\nzweite Zeile");
	label = gowl_bar_plugin_dup_label(h.plugin);
	g_assert_cmpstr(label, ==, "DE\xe2\x86\x92" "JA");

	bar_harness_down(&h);
}

/* Theming: role names and literals for the chips (snapped to the
   nearest role, since panel rows draw in roles), a literal for the
   bar label, and show-pair off leaves the glyph alone. */
static void
test_widget_colours_and_label(void)
{
	BarHarness h;
	g_autoptr(GowlBarPanel) p1 = NULL;
	g_autoptr(GowlBarPanel) p2 = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *icon = NULL;
	gdouble rgba[4];

	if (!up_libre(&h))
		return;

	bar_harness_set(&h, "color-chip", "green");
	p1 = bar_harness_panel(&h);
	g_assert_cmpint(gowl_bar_panel_item_get_color(
		gowl_bar_panel_find(p1, "langs")), ==, GOWL_BAR_COLOR_GREEN);

	{
		const GowlBarTheme *theme;
		const gdouble *c;
		g_autofree gchar *hex = NULL;

		theme = gowl_bar_host_get_theme(GOWL_BAR_HOST(h.host));
		c = gowl_bar_theme_color(theme, GOWL_BAR_COLOR_PEACH);
		hex = g_strdup_printf("#%02x%02x%02x", (guint)(c[0] * 255.0 + 0.5),
		                      (guint)(c[1] * 255.0 + 0.5),
		                      (guint)(c[2] * 255.0 + 0.5));
		bar_harness_set(&h, "color-chip", hex);
	}
	p2 = bar_harness_panel(&h);
	g_assert_cmpint(gowl_bar_panel_item_get_color(
		gowl_bar_panel_find(p2, "langs")), ==, GOWL_BAR_COLOR_PEACH);

	bar_harness_set(&h, "color", "#123456");
	bar_harness_poll(&h);
	g_assert_true(gowl_bar_plugin_get_color_rgba(h.plugin, rgba));

	bar_harness_set(&h, "show-pair", "false");
	bar_harness_set(&h, "icon", "T");
	bar_harness_poll(&h);
	label = gowl_bar_plugin_dup_label(h.plugin);
	icon = gowl_bar_plugin_dup_icon(h.plugin);
	g_assert_null(label);
	g_assert_cmpstr(icon, ==, "T");

	bar_harness_down(&h);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-translate/model/languages",
	                test_language_table_is_sound);
	g_test_add_func("/bar-translate/model/filter", test_language_filter);
	g_test_add_func("/bar-translate/model/engines", test_engine_selection);
	g_test_add_func("/bar-translate/model/url-encoding", test_url_encoding);
	g_test_add_func("/bar-translate/model/json-body", test_json_body);
	g_test_add_func("/bar-translate/model/curl-argv", test_curl_argv);
	g_test_add_func("/bar-translate/model/parse-libre", test_parse_libre);
	g_test_add_func("/bar-translate/model/parse-mymemory",
	                test_parse_mymemory);
	g_test_add_func("/bar-translate/model/parse-google", test_parse_google);
	g_test_add_func("/bar-translate/model/status-and-clean",
	                test_split_status_and_clean);
	g_test_add_func("/bar-translate/model/chunking", test_chunking);
	g_test_add_func("/bar-translate/model/ai-marker",
	                test_ai_prompt_and_marker);
	g_test_add_func("/bar-translate/model/debounce", test_debounce_decision);
	g_test_add_func("/bar-translate/model/swap", test_swap_rule);
	g_test_add_func("/bar-translate/model/state", test_state_round_trip);

	g_test_add_func("/bar-translate/widget/libre",
	                test_widget_libre_round_trip);
	g_test_add_func("/bar-translate/widget/errors", test_widget_errors);
	g_test_add_func("/bar-translate/widget/chunks",
	                test_widget_chunks_long_text);
	g_test_add_func("/bar-translate/widget/mymemory", test_widget_mymemory);
	g_test_add_func("/bar-translate/widget/google", test_widget_google);
	g_test_add_func("/bar-translate/widget/stale",
	                test_widget_stale_result_ignored);
	g_test_add_func("/bar-translate/widget/debounce",
	                test_widget_debounce_and_dedupe);
	g_test_add_func("/bar-translate/widget/swap", test_widget_swap);
	g_test_add_func("/bar-translate/widget/picker-keys",
	                test_widget_picker_and_keys);
	g_test_add_func("/bar-translate/widget/clipboard",
	                test_widget_clipboard_flow);
	g_test_add_func("/bar-translate/widget/missing-deps",
	                test_widget_missing_dependencies);
	g_test_add_func("/bar-translate/widget/ai-cli", test_widget_ai_cli);
	g_test_add_func("/bar-translate/widget/ai-cmacs", test_widget_ai_cmacs);
	g_test_add_func("/bar-translate/widget/state",
	                test_widget_state_persists);
	g_test_add_func("/bar-translate/widget/colours",
	                test_widget_colours_and_label);

	return g_test_run();
}
