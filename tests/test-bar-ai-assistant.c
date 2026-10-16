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
 * The `ai-assistant' widget (modules/bar/bar-plugins-assistant.c) and
 * its pure half (modules/bar/bar-assistant-model.c).
 *
 * Offline throughout: ollama is a fake curl serving the fixtures in
 * tests/fixtures/bar/ai-assistant by URL, the model is a fake `ai',
 * cmacs is a fake emacsctl plus settings pushed the way cmacs pushes
 * them, and /proc and /sys are fixture trees.
 *
 * The model file is compiled straight into this test (like
 * test-bar-util.c does with bar-util.c), so its functions are called
 * directly; the widget is driven through the real bar.so.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#define BAR_HARNESS_MODULE GOWL_TEST_BAR_MODULE
#include "bar-widget-harness.h"

#include "../modules/bar/bar-util.c"
#include "../modules/bar/bar-assistant-model.c"

#ifndef GOWL_TEST_BAR_MODULE
#define GOWL_TEST_BAR_MODULE "build/release/modules/bar.so"
#endif
#ifndef GOWL_TEST_FIXTURES
#define GOWL_TEST_FIXTURES "tests/fixtures/bar"
#endif

#define GIB ((guint64)1024 * 1024 * 1024)

/* The reduced-motion glyph, as the widget sets it (nf-md-robot). */
#define AS_TEST_GLYPH "\xf3\xb0\x9a\xa9"

static gchar *
fixture_path(const gchar *name)
{
	return g_build_filename(GOWL_TEST_FIXTURES, "ai-assistant", name, NULL);
}

static gchar *
fixture(const gchar *name)
{
	g_autofree gchar *path = fixture_path(name);
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	return text;
}

/* ════════════════════════════════════════════════════════════════════
 * The pure half
 * ════════════════════════════════════════════════════════════════════ */

/* Catches: a changed `ai --list-providers' format reading the header,
   the kind column or a note as a provider name. */
static void
test_provider_listing(void)
{
	g_autofree gchar *text = fixture("list-providers.txt");
	g_auto(GStrv) names = NULL;
	g_auto(GStrv) odd = NULL;
	g_auto(GStrv) none = NULL;

	names = bar_assistant_parse_provider_listing(text);
	g_assert_cmpuint(g_strv_length(names), ==, 13);
	g_assert_cmpstr(names[0], ==, "claude");
	g_assert_cmpstr(names[4], ==, "ollama");
	g_assert_true(g_strv_contains((const gchar * const *)names,
	                              "claude-code"));
	g_assert_true(g_strv_contains((const gchar * const *)names,
	                              "openai-compatible"));
	g_assert_false(g_strv_contains((const gchar * const *)names, "HTTP"));
	g_assert_false(g_strv_contains((const gchar * const *)names,
	                               "Providers:"));

	/* A trailing unindented line ends the list; junk is skipped. */
	odd = bar_assistant_parse_provider_listing(
		"Providers:\n  good  HTTP\n  $(rm -rf) CLI\n  -flag x\n"
		"Hint: set a default\n  after-hint HTTP\n");
	g_assert_cmpuint(g_strv_length(odd), ==, 1);
	g_assert_cmpstr(odd[0], ==, "good");

	none = bar_assistant_parse_provider_listing(NULL);
	g_assert_nonnull(none);
	g_assert_null(none[0]);
}

/* Catches: cmacs's comma list arriving with spaces, duplicates or a
   hostile entry. */
static void
test_split_list(void)
{
	g_auto(GStrv) providers = NULL;
	g_auto(GStrv) models = NULL;

	providers = bar_assistant_split_list(
		" claude, ollama ,claude,,bad name,ok-1", FALSE);
	g_assert_cmpuint(g_strv_length(providers), ==, 3);
	g_assert_cmpstr(providers[0], ==, "claude");
	g_assert_cmpstr(providers[1], ==, "ollama");
	g_assert_cmpstr(providers[2], ==, "ok-1");

	models = bar_assistant_split_list(
		"qwen3:8b,hf.co/LiquidAI/LFM2-1.2B-RAG-GGUF:Q8_0,"
		"x\",\"keep_alive\":0,-rm", TRUE);
	g_assert_cmpuint(g_strv_length(models), ==, 2);
	g_assert_cmpstr(models[1], ==, "hf.co/LiquidAI/LFM2-1.2B-RAG-GGUF:Q8_0");
}

/* Catches: a model or provider name that would break out of a JSON
   body, an argv slot or an Elisp form being accepted. */
static void
test_names(void)
{
	g_assert_true(bar_assistant_valid_model("qwen3:8b"));
	g_assert_true(bar_assistant_valid_model("library/llama3.2:3b-q4_K_M"));
	g_assert_true(bar_assistant_valid_model("user@host/model+x:1"));
	g_assert_false(bar_assistant_valid_model(NULL));
	g_assert_false(bar_assistant_valid_model(""));
	g_assert_false(bar_assistant_valid_model("-o/etc/passwd"));
	g_assert_false(bar_assistant_valid_model("/etc/passwd"));
	g_assert_false(bar_assistant_valid_model("a/../../b"));
	g_assert_false(bar_assistant_valid_model("x\",\"keep_alive\":0"));
	g_assert_false(bar_assistant_valid_model("qwen3:8b; rm -rf ~"));
	g_assert_false(bar_assistant_valid_model("name\nline"));

	g_assert_true(bar_assistant_valid_provider("claude-code"));
	g_assert_true(bar_assistant_valid_provider("my.server_2"));
	g_assert_false(bar_assistant_valid_provider("-p"));
	g_assert_false(bar_assistant_valid_provider("a b"));
	g_assert_false(bar_assistant_valid_provider("x\")"));
}

/* Catches: the real /api/tags shape not being read, or a nested
   `details' field being missed. */
static void
test_parse_tags(void)
{
	g_autofree gchar *json = fixture("tags.json");
	g_autoptr(GPtrArray) models = NULL;
	BarAssistantModel *m;
	gchar *err = NULL;

	models = bar_assistant_parse_models(json, &err);
	g_assert_null(err);
	g_assert_nonnull(models);
	g_assert_cmpuint(models->len, ==, 4);
	m = g_ptr_array_index(models, 0);
	g_assert_cmpstr(m->name, ==,
	                "hf.co/LiquidAI/LFM2.5-1.2B-Instruct-GGUF:Q8_0");
	g_assert_cmpuint(m->size, ==, 1246265401);
	g_assert_cmpstr(m->family, ==, "lfm2");
	g_assert_cmpstr(m->params, ==, "1.17B");
	g_assert_cmpstr(m->quant, ==, "unknown");
	m = g_ptr_array_index(models, 3);
	g_assert_cmpstr(m->name, ==, "tatertard-massive:latest");
	g_assert_cmpstr(m->quant, ==, "F16");
}

static void
test_parse_ps(void)
{
	g_autofree gchar *json = fixture("ps.json");
	g_autoptr(GPtrArray) models = NULL;
	BarAssistantModel *m;

	models = bar_assistant_parse_models(json, NULL);
	g_assert_cmpuint(models->len, ==, 1);
	m = g_ptr_array_index(models, 0);
	g_assert_cmpstr(m->name, ==, "qwen3:8b");
	g_assert_cmpuint(m->size_vram, ==, 6254362624);
	g_assert_cmpstr(m->expires_at, ==,
	                "2026-10-16T02:01:09.512431762-04:00");
}

/* Catches: an HTML error page or an `error' object being guessed into
   a model list, instead of reported. */
static void
test_parse_malformed(void)
{
	g_autofree gchar *html = fixture("tags-malformed.html");
	g_autofree gchar *errjson = fixture("tags-error.json");
	g_autoptr(GPtrArray) empty = NULL;
	gchar *err = NULL;

	g_assert_null(bar_assistant_parse_models(html, &err));
	g_assert_nonnull(strstr(err, "not an ollama reply"));
	g_free(err);

	g_assert_null(bar_assistant_parse_models(errjson, &err));
	g_assert_cmpstr(err, ==, "something went wrong loading the registry");
	g_free(err);

	g_assert_null(bar_assistant_parse_models(NULL, NULL));
	g_assert_null(bar_assistant_parse_models("", NULL));
	g_assert_null(bar_assistant_parse_models("{\"models\":", NULL));

	empty = bar_assistant_parse_models("{\"models\":[]}", NULL);
	g_assert_nonnull(empty);
	g_assert_cmpuint(empty->len, ==, 0);
}

/* Catches: a nested `name'/`size' taken for the model's, markup or
   escapes altered, invalid UTF-8 reaching pango, a float or negative
   size misread, a truncated reply crashing the reader. */
static void
test_parse_hostile(void)
{
	g_autofree gchar *json = fixture("tags-hostile.json");
	g_autoptr(GPtrArray) models = NULL;
	BarAssistantModel *m;
	guint i;

	models = bar_assistant_parse_models(json, NULL);
	g_assert_nonnull(models);
	g_assert_cmpuint(models->len, ==, 3);

	m = g_ptr_array_index(models, 0);
	g_assert_cmpstr(m->name, ==,
	                "<b>bold</b> & \"quoted\" \xef\xbf\xbd x");
	g_assert_cmpuint(m->size, ==, 4661224676);
	g_assert_cmpstr(m->family, ==, "llama");

	m = g_ptr_array_index(models, 1);
	g_assert_true(g_utf8_validate(m->name, -1, NULL));
	g_assert_true(g_str_has_prefix(m->name, "bad-"));
	g_assert_cmpuint(m->size, ==, 0);

	m = g_ptr_array_index(models, 2);
	g_assert_cmpstr(m->name, ==, "only-model-key:1b");
	g_assert_cmpuint(m->size, ==, 1500000000);

	for (i = 0; i < models->len; i++)
		g_assert_true(g_utf8_validate(((BarAssistantModel *)
			g_ptr_array_index(models, i))->name, -1, NULL));
}

/* Catches: a reply with thousands of models or a megabyte name
   growing memory without bound. */
static void
test_parse_huge(void)
{
	GString *json;
	g_autoptr(GPtrArray) models = NULL;
	g_autofree gchar *longname = NULL;
	guint i;

	json = g_string_new("{\"models\":[");
	for (i = 0; i < 1000; i++)
		g_string_append_printf(json, "%s{\"name\":\"m%u:1b\",\"size\":%u}",
		                       (i > 0) ? "," : "", i, i);
	g_string_append(json, "]}");
	models = bar_assistant_parse_models(json->str, NULL);
	g_assert_cmpuint(models->len, ==, BAR_ASSISTANT_MAX_MODELS);
	g_string_free(json, TRUE);
	g_clear_pointer(&models, g_ptr_array_unref);

	longname = g_strnfill(1024 * 1024, 'a');
	json = g_string_new("{\"models\":[{\"name\":\"");
	g_string_append(json, longname);
	g_string_append(json, "\",\"size\":99999999999999999999999}]}");
	models = bar_assistant_parse_models(json->str, NULL);
	g_assert_cmpuint(models->len, ==, 1);
	g_assert_cmpuint(strlen(((BarAssistantModel *)
		g_ptr_array_index(models, 0))->name), <=, JSON_MAX_STRING);
	g_assert_cmpuint(((BarAssistantModel *)
		g_ptr_array_index(models, 0))->size, ==, G_MAXUINT64);
	g_string_free(json, TRUE);
}

/* Catches: progress, success and the error line being misread. */
static void
test_pull_lines(void)
{
	g_autofree gchar *text = fixture("pull-progress.txt");
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	BarAssistantPull p;

	g_assert_true(bar_assistant_parse_pull_line(lines[0], &p));
	g_assert_cmpstr(p.status, ==, "pulling manifest");
	g_assert_false(p.done);
	bar_assistant_pull_clear(&p);

	g_assert_true(bar_assistant_parse_pull_line(lines[2], &p));
	g_assert_cmpuint(p.total, ==, 2019377376);
	g_assert_cmpuint(p.completed, ==, 1009688688);
	bar_assistant_pull_clear(&p);

	g_assert_true(bar_assistant_parse_pull_line(lines[5], &p));
	g_assert_true(p.done);
	bar_assistant_pull_clear(&p);

	g_assert_true(bar_assistant_parse_pull_line(
		"{\"error\":\"pull model manifest: file does not exist\"}", &p));
	g_assert_cmpstr(p.error, ==, "pull model manifest: file does not exist");
	bar_assistant_pull_clear(&p);

	/* completed beyond total is clamped, not a 300% bar. */
	g_assert_true(bar_assistant_parse_pull_line(
		"{\"status\":\"x\",\"total\":10,\"completed\":30}", &p));
	g_assert_cmpuint(p.completed, ==, 10);
	bar_assistant_pull_clear(&p);

	g_assert_false(bar_assistant_parse_pull_line("curl: (7) refused", &p));
	g_assert_false(bar_assistant_parse_pull_line(NULL, &p));
	g_assert_false(bar_assistant_parse_pull_line("{}", &p));
}

static gchar *
joined(gchar **argv)
{
	return g_strjoinv(" | ", argv);
}

/* Catches: any change to what curl is actually asked, a shell being
   involved, a URL read as an option, or an unescaped body. */
static void
test_curl_argv(void)
{
	g_auto(GStrv) tags = NULL;
	g_auto(GStrv) load = NULL;
	g_auto(GStrv) unload = NULL;
	g_auto(GStrv) del = NULL;
	g_auto(GStrv) pull = NULL;
	g_auto(GStrv) forever = NULL;
	g_autofree gchar *j = NULL;
	g_autofree gchar *esc = NULL;
	g_autofree gchar *url = NULL;

	tags = bar_assistant_curl_argv(NULL, "http://127.0.0.1:11434",
	                               BAR_ASSISTANT_OP_TAGS, NULL, NULL);
	j = joined(tags);
	g_assert_cmpstr(j, ==, "curl | -fsS | --max-time | 5 | --url | "
	                "http://127.0.0.1:11434/api/tags");
	g_clear_pointer(&j, g_free);

	load = bar_assistant_curl_argv("/usr/bin/curl", "http://h:1",
	                               BAR_ASSISTANT_OP_LOAD, "qwen3:8b", "2h");
	j = joined(load);
	g_assert_cmpstr(j, ==, "/usr/bin/curl | -fsS | --max-time | 300 | "
		"--write-out | %{http_code}\\n | -X | POST | -H | "
		"Content-Type: application/json | --data-binary | "
		"{\"model\":\"qwen3:8b\",\"prompt\":\"\",\"keep_alive\":\"2h\","
		"\"stream\":false} | --url | http://h:1/api/generate");
	g_clear_pointer(&j, g_free);

	unload = bar_assistant_curl_argv(NULL, "http://h:1",
	                                 BAR_ASSISTANT_OP_UNLOAD, "qwen3:8b",
	                                 NULL);
	g_assert_true(g_strv_contains((const gchar * const *)unload,
		"{\"model\":\"qwen3:8b\",\"prompt\":\"\",\"keep_alive\":0,"
		"\"stream\":false}"));

	del = bar_assistant_curl_argv(NULL, "http://h:1",
	                              BAR_ASSISTANT_OP_DELETE, "qwen3:8b", NULL);
	j = joined(del);
	g_assert_nonnull(strstr(j, "-X | DELETE"));
	g_assert_nonnull(strstr(j,
		"{\"model\":\"qwen3:8b\",\"name\":\"qwen3:8b\"}"));
	g_assert_true(g_str_has_suffix(j, "--url | http://h:1/api/delete"));
	g_clear_pointer(&j, g_free);

	pull = bar_assistant_curl_argv(NULL, "http://h:1",
	                               BAR_ASSISTANT_OP_PULL, "qwen3:8b", NULL);
	j = joined(pull);
	g_assert_cmpstr(j, ==, "curl | -fsS | -N | --connect-timeout | 5 | "
		"-X | POST | -H | Content-Type: application/json | "
		"--data-binary | {\"model\":\"qwen3:8b\",\"name\":\"qwen3:8b\","
		"\"stream\":true} | --url | http://h:1/api/pull");
	g_clear_pointer(&j, g_free);

	/* keep_alive: -1 and seconds are numbers, junk is refused. */
	forever = bar_assistant_curl_argv(NULL, "http://h:1",
		BAR_ASSISTANT_OP_LOAD, "m:1", "-1");
	j = joined(forever);
	g_assert_nonnull(strstr(j, "\"keep_alive\":-1,"));
	g_clear_pointer(&j, g_free);
	g_strfreev(forever);
	forever = bar_assistant_curl_argv(NULL, "http://h:1",
		BAR_ASSISTANT_OP_LOAD, "m:1", "\",\"x\":1");
	j = joined(forever);
	g_assert_nonnull(strstr(j, "\"keep_alive\":\"30m\","));
	g_clear_pointer(&j, g_free);

	/* A name that would escape the body never gets an argv. */
	g_assert_null(bar_assistant_curl_argv(NULL, "http://h:1",
		BAR_ASSISTANT_OP_DELETE, "x\",\"name\":\"other", NULL));
	g_assert_null(bar_assistant_curl_argv(NULL, "http://h:1",
		BAR_ASSISTANT_OP_PULL, NULL, NULL));

	/* The escaper on its own: the second lock. */
	esc = bar_assistant_json_escape("a\"b\\c\nd\x01");
	g_assert_cmpstr(esc, ==, "a\\\"b\\\\c\\nd\\u0001");

	/* URLs: only http(s), no whitespace, no trailing slash. */
	url = bar_assistant_normalise_url("  http://127.0.0.1:11434/// ");
	g_assert_cmpstr(url, ==, "http://127.0.0.1:11434");
	g_assert_null(bar_assistant_normalise_url("-o /tmp/x"));
	g_assert_null(bar_assistant_normalise_url("file:///etc/passwd"));
	g_assert_null(bar_assistant_normalise_url("http://a b"));
	g_assert_null(bar_assistant_normalise_url("http://"));
}

/* Catches: the fit thresholds drifting, an APU's carve-out counted
   as a GPU, or VRAM+RAM spill not being "tight". */
static void
test_fit(void)
{
	BarAssistantMachine gpu = { 0 };
	BarAssistantMachine cpu = { 0 };

	gpu.gpu_kind   = (gchar *)"NVIDIA";
	gpu.vram_total = 24 * GIB;
	gpu.vram_used  = 4 * GIB;      /* 20 GiB free */
	gpu.mem_avail  = 16 * GIB;

	/* need = size * 1.2 */
	g_assert_cmpint(bar_assistant_fit(10 * GIB, &gpu), ==,
	                BAR_ASSISTANT_FIT_COMFORTABLE);
	g_assert_cmpint(bar_assistant_fit(16 * GIB, &gpu), ==,
	                BAR_ASSISTANT_FIT_COMFORTABLE);   /* 19.2 <= 20 */
	g_assert_cmpint(bar_assistant_fit(17 * GIB, &gpu), ==,
	                BAR_ASSISTANT_FIT_TIGHT);         /* 20.4 > 20 */
	g_assert_cmpint(bar_assistant_fit(36 * GIB, &gpu), ==,
	                BAR_ASSISTANT_FIT_TIGHT);         /* <= 20 + 16 */
	g_assert_cmpint(bar_assistant_fit(37 * GIB, &gpu), ==,
	                BAR_ASSISTANT_FIT_TOO_BIG);

	cpu.mem_avail = 10 * GIB;
	g_assert_cmpint(bar_assistant_fit(8 * GIB, &cpu), ==,
	                BAR_ASSISTANT_FIT_COMFORTABLE);   /* 9.6 */
	g_assert_cmpint(bar_assistant_fit(9 * GIB, &cpu), ==,
	                BAR_ASSISTANT_FIT_TIGHT);
	g_assert_cmpint(bar_assistant_fit(11 * GIB, &cpu), ==,
	                BAR_ASSISTANT_FIT_TOO_BIG);

	/* A tiny `GPU' is ignored: RAM decides. */
	cpu.gpu_kind   = (gchar *)"AMD";
	cpu.vram_total = GIB / 2;
	g_assert_cmpint(bar_assistant_fit(8 * GIB, &cpu), ==,
	                BAR_ASSISTANT_FIT_COMFORTABLE);

	g_assert_cmpint(bar_assistant_fit(0, &cpu), ==,
	                BAR_ASSISTANT_FIT_UNKNOWN);
	cpu.mem_avail = 0;
	cpu.gpu_kind = NULL;
	g_assert_cmpint(bar_assistant_fit(GIB, &cpu), ==,
	                BAR_ASSISTANT_FIT_UNKNOWN);
	g_assert_cmpstr(bar_assistant_fit_label(BAR_ASSISTANT_FIT_TOO_BIG),
	                ==, "too big");
}

/* Catches: connector directories read as cards, an APU's carve-out
   counted, kernel threads in the top list, a missing NVIDIA preference. */
static void
test_read_machine(void)
{
	g_autofree gchar *proc = fixture_path("root/proc");
	g_autofree gchar *sys = fixture_path("root/sys");
	g_autofree gchar *smi = fixture("nvidia-smi.txt");
	g_autofree gchar *tmp = g_dir_make_tmp("as-disk-XXXXXX", NULL);
	g_autofree gchar *missing = g_build_filename(tmp, "not", "yet", NULL);
	BarAssistantMachine m;
	guint64 t = 0, u = 0;

	bar_assistant_read_machine(proc, sys, NULL, missing, &m);
	g_assert_cmpuint(m.cores, ==, 4);
	g_assert_cmpfloat_with_epsilon(m.load[0], 1.20, 1e-9);
	g_assert_cmpfloat_with_epsilon(m.load[2], 0.75, 1e-9);
	g_assert_cmpuint(m.mem_total, ==, (guint64)32768000 * 1024);
	g_assert_cmpuint(m.mem_avail, ==, (guint64)16384000 * 1024);
	g_assert_cmpstr(m.gpu_kind, ==, "AMD");
	g_assert_cmpuint(m.vram_total, ==, 17163091968);
	g_assert_cmpuint(m.vram_used, ==, 1073741824);
	/* The nearest existing parent of a models dir not made yet. */
	g_assert_cmpuint(m.disk_total, >, 0);
	g_assert_cmpuint(g_strv_length(m.top), ==, 3);
	g_assert_cmpstr(m.top[0], ==, "ollama");
	g_assert_cmpstr(m.top[1], ==, "firefox");
	g_assert_cmpstr(m.top[2], ==, "emacs");
	bar_assistant_machine_clear(&m);

	bar_assistant_read_machine(proc, sys, smi, NULL, &m);
	g_assert_cmpstr(m.gpu_kind, ==, "NVIDIA");
	g_assert_cmpuint(m.vram_total, ==, (guint64)(24576 + 8192) * 1024 * 1024);
	g_assert_cmpuint(m.vram_used, ==, (guint64)(3072 + 8192) * 1024 * 1024);
	bar_assistant_machine_clear(&m);

	g_assert_false(bar_assistant_parse_nvidia_smi(
		"NVIDIA-SMI has failed because it couldn't communicate", &t, &u));
	g_assert_false(bar_assistant_parse_nvidia_smi(NULL, &t, &u));
	g_assert_true(bar_assistant_parse_nvidia_smi("100, 200\n", &t, &u));
	g_assert_cmpuint(u, ==, t);   /* used clamped to total */

	g_rmdir(tmp);
}

/* Catches: the paragraph prepended to the system prompt changing
   shape, or printing a decimal comma. */
static void
test_machine_summary(void)
{
	BarAssistantMachine m = { 0 };
	const gchar *loaded[] = { "qwen3:8b", NULL };
	const gchar *none[] = { NULL };
	const gchar *top[] = { "ollama", "firefox", NULL };
	g_autofree gchar *s = NULL;
	g_autofree gchar *s2 = NULL;

	m.cores = 16;
	m.load[0] = 1.2;
	m.load[1] = 0.98;
	m.load[2] = 0.75;
	m.mem_total = 32000000000ULL;
	m.mem_avail = 20000000000ULL;
	m.gpu_kind = (gchar *)"NVIDIA";
	m.vram_total = 24000000000ULL;
	m.vram_used = 8000000000ULL;
	m.disk_total = 1000000000000ULL;
	m.disk_free = 250000000000ULL;
	m.top = (gchar **)top;

	s = bar_assistant_machine_summary(&m, loaded);
	g_assert_cmpstr(s, ==,
		"Live system info, read moments ago: 16 CPU threads, load "
		"1.20 0.98 0.75; memory 12.0 GB used of 32.0 GB (20.0 GB "
		"available); NVIDIA GPU with 8.0 GB of 24.0 GB VRAM in use; "
		"250.0 GB free on the model disk; largest processes by memory: "
		"ollama, firefox; loaded models: qwen3:8b.");

	m.gpu_kind = NULL;
	m.top = NULL;
	m.disk_total = 0;
	s2 = bar_assistant_machine_summary(&m, none);
	g_assert_true(g_str_has_suffix(s2, "; no local models loaded."));
	g_assert_null(strstr(s2, "GPU"));
}

/* Catches: an approval outliving its minute, a Cancel that still
   runs the operation, or Approve with nothing armed doing something. */
static void
test_approval(void)
{
	BarAssistantApproval a = { 0 };
	gchar *model = NULL;
	const gint64 t0 = 1000 * G_USEC_PER_SEC;

	g_assert_false(bar_assistant_approval_live(&a, t0));
	g_assert_cmpint(bar_assistant_approval_take(&a, t0, &model), ==,
	                BAR_ASSISTANT_OP_NONE);

	/* Approve within the minute. */
	bar_assistant_approval_arm(&a, BAR_ASSISTANT_OP_DELETE, "qwen3:8b",
	                           "Delete qwen3:8b", t0);
	g_assert_true(bar_assistant_approval_live(&a, t0 + 59 * G_USEC_PER_SEC));
	g_assert_cmpint(bar_assistant_approval_take(&a,
		t0 + 59 * G_USEC_PER_SEC, &model), ==, BAR_ASSISTANT_OP_DELETE);
	g_assert_cmpstr(model, ==, "qwen3:8b");
	g_clear_pointer(&model, g_free);
	/* Taken once: a second click does nothing. */
	g_assert_cmpint(bar_assistant_approval_take(&a, t0, NULL), ==,
	                BAR_ASSISTANT_OP_NONE);

	/* Expired at exactly sixty seconds. */
	bar_assistant_approval_arm(&a, BAR_ASSISTANT_OP_PULL, "x:1", "d", t0);
	g_assert_cmpint(bar_assistant_approval_take(&a,
		t0 + BAR_ASSISTANT_APPROVAL_TTL, &model), ==,
		BAR_ASSISTANT_OP_NONE);
	g_assert_null(model);
	g_assert_null(a.detail);

	/* Cancel. */
	bar_assistant_approval_arm(&a, BAR_ASSISTANT_OP_LOAD, "x:1", "d", t0);
	bar_assistant_approval_clear(&a);
	g_assert_cmpint(bar_assistant_approval_take(&a, t0, NULL), ==,
	                BAR_ASSISTANT_OP_NONE);

	/* A clock that went backwards does not keep it alive either. */
	bar_assistant_approval_arm(&a, BAR_ASSISTANT_OP_LOAD, "x:1", "d", t0);
	g_assert_false(bar_assistant_approval_live(&a, t0 - 1));

	/* Re-arming replaces the previous card. */
	bar_assistant_approval_arm(&a, BAR_ASSISTANT_OP_LOAD, "a:1", "d", t0);
	bar_assistant_approval_arm(&a, BAR_ASSISTANT_OP_DELETE, "b:1", "d", t0);
	g_assert_cmpint(bar_assistant_approval_take(&a, t0, &model), ==,
	                BAR_ASSISTANT_OP_DELETE);
	g_assert_cmpstr(model, ==, "b:1");
	g_free(model);
}

/* Catches: the prompt document's format changing, the history cap
   leaving an orphaned answer first, or the size cap dropping the
   question instead of the oldest turns. */
static void
test_transcript(void)
{
	BarAssistantTranscript *t = bar_assistant_transcript_new();
	g_autofree gchar *flat = NULL;
	g_autofree gchar *capped = NULL;
	g_autofree gchar *bare = NULL;
	g_autofree gchar *big = NULL;
	guint i;

	flat = bar_assistant_transcript_flatten(t, "first", 0);
	g_assert_cmpstr(flat, ==, "User: first");
	g_clear_pointer(&flat, g_free);

	bar_assistant_transcript_add(t, BAR_ASSISTANT_ROLE_USER, "Hi");
	bar_assistant_transcript_add(t, BAR_ASSISTANT_ROLE_ASSISTANT, "Hello!");
	flat = bar_assistant_transcript_flatten(t, "What now?", 0);
	g_assert_cmpstr(flat, ==,
	                "User: Hi\n\nAssistant: Hello!\n\nUser: What now?");

	bare = bar_assistant_transcript_flatten(t, NULL, 0);
	g_assert_cmpstr(bare, ==, "User: Hi\n\nAssistant: Hello!");

	/* Size cap: the oldest go first, the question always stays. */
	capped = bar_assistant_transcript_flatten(t, "Q", 30);
	g_assert_cmpstr(capped, ==, "Assistant: Hello!\n\nUser: Q");

	/* History cap of 3 leaves [A, U, A] -> the leading answer goes. */
	for (i = 0; i < 3; i++) {
		bar_assistant_transcript_add(t, BAR_ASSISTANT_ROLE_USER, "q");
		bar_assistant_transcript_add(t, BAR_ASSISTANT_ROLE_ASSISTANT, "a");
	}
	bar_assistant_transcript_trim(t, 3);
	g_assert_cmpuint(bar_assistant_transcript_len(t), ==, 2);
	g_assert_cmpint(bar_assistant_transcript_get(t, 0)->role, ==,
	                BAR_ASSISTANT_ROLE_USER);

	bar_assistant_transcript_pop(t);
	g_assert_cmpuint(bar_assistant_transcript_len(t), ==, 1);

	/* Non-UTF-8 and oversized messages are cleaned on the way in. */
	bar_assistant_transcript_add(t, BAR_ASSISTANT_ROLE_ASSISTANT,
	                             "bad\xff");
	g_assert_true(g_utf8_validate(
		bar_assistant_transcript_get(t, 1)->text, -1, NULL));
	big = g_strnfill(BAR_ASSISTANT_MAX_MESSAGE + 100, 'x');
	bar_assistant_transcript_add(t, BAR_ASSISTANT_ROLE_USER, big);
	g_assert_cmpuint(strlen(bar_assistant_transcript_get(t, 2)->text), ==,
	                 BAR_ASSISTANT_MAX_MESSAGE);

	bar_assistant_transcript_clear(t);
	g_assert_cmpuint(bar_assistant_transcript_len(t), ==, 0);
	bar_assistant_transcript_free(t);
}

/* Catches: wave heights leaving their range, the newest sample not
   being the rightmost bar, or the ripple being applied while idle. */
static void
test_wave(void)
{
	const gdouble samples[] = { 0.0, 1.0, 0.5, 2.0, -1.0 };
	gdouble out[7];
	gdouble moved[7];
	guint i;

	bar_assistant_wave(samples, 5, 5, 0.0, FALSE, out);
	g_assert_cmpfloat_with_epsilon(out[0], BAR_ASSISTANT_WAVE_MIN, 1e-9);
	g_assert_cmpfloat_with_epsilon(out[1], 1.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(out[2],
		BAR_ASSISTANT_WAVE_MIN + (1.0 - BAR_ASSISTANT_WAVE_MIN) * 0.5,
		1e-9);
	g_assert_cmpfloat_with_epsilon(out[3], 1.0, 1e-9);     /* clamped */
	g_assert_cmpfloat_with_epsilon(out[4], BAR_ASSISTANT_WAVE_MIN, 1e-9);

	/* Right-aligned: two samples into seven bars. */
	bar_assistant_wave(samples + 1, 2, 7, 0.0, FALSE, out);
	for (i = 0; i < 5; i++)
		g_assert_cmpfloat_with_epsilon(out[i], BAR_ASSISTANT_WAVE_MIN,
		                               1e-9);
	g_assert_cmpfloat_with_epsilon(out[5], 1.0, 1e-9);

	/* Animated: bounded, and it moves with the phase. */
	bar_assistant_wave(samples, 5, 7, 0.0, TRUE, out);
	bar_assistant_wave(samples, 5, 7, 1.7, TRUE, moved);
	for (i = 0; i < 7; i++) {
		g_assert_cmpfloat(out[i], >=, BAR_ASSISTANT_WAVE_MIN);
		g_assert_cmpfloat(out[i], <=, 1.0);
	}
	g_assert_true(memcmp(out, moved, sizeof(out)) != 0);

	/* No samples at all: every bar at the minimum, never zero. */
	bar_assistant_wave(NULL, 0, 3, 0.0, FALSE, out);
	g_assert_cmpfloat_with_epsilon(out[2], BAR_ASSISTANT_WAVE_MIN, 1e-9);
}

/* Catches: a state colouring through the wrong setting. */
static void
test_look_keys(void)
{
	g_assert_cmpstr(bar_assistant_look_color_key(BAR_ASSISTANT_LOOK_IDLE),
	                ==, "color-idle");
	g_assert_cmpstr(bar_assistant_look_color_key(
		BAR_ASSISTANT_LOOK_THINKING), ==, "color-thinking");
	g_assert_cmpstr(bar_assistant_look_color_key(
		BAR_ASSISTANT_LOOK_STREAMING), ==, "color-streaming");
	g_assert_cmpstr(bar_assistant_look_color_key(BAR_ASSISTANT_LOOK_ERROR),
	                ==, "color-error");
}

/* Catches: a transcript with quotes, a `'' or a `)' breaking the
   shell line or the Elisp form handed to emacsctl. */
static void
test_cmacs_cmdline(void)
{
	g_autofree gchar *line = NULL;
	g_autofree gchar *bare = NULL;
	g_autofree gchar *huge = NULL;
	g_autofree gchar *filler = NULL;
	g_autofree gchar *cut = NULL;
	g_auto(GStrv) argv = NULL;

	line = bar_assistant_cmacs_open_cmdline(NULL, "ollama", "qwen3:8b",
		"User: it's \"quoted\") (kill-emacs)\\\n\nAssistant: ok");
	g_assert_true(g_shell_parse_argv(line, NULL, &argv, NULL));
	g_assert_cmpuint(g_strv_length(argv), ==, 3);
	g_assert_cmpstr(argv[0], ==, "emacsctl");
	g_assert_cmpstr(argv[1], ==, "eval");
	g_assert_cmpstr(argv[2], ==,
		"(cmacs-gowl-bar-assistant-open-in-chat \"ollama\" \"qwen3:8b\" "
		"\"User: it's \\\"quoted\\\") (kill-emacs)\\\\\n\nAssistant: ok\")");

	bare = bar_assistant_cmacs_open_cmdline("/opt/x y/emacsctl", NULL, "",
	                                        NULL);
	g_assert_cmpstr(bare, ==, "'/opt/x y/emacsctl' eval "
		"'(cmacs-gowl-bar-assistant-open-in-chat nil nil nil)'");

	g_assert_null(bar_assistant_cmacs_open_cmdline(NULL, NULL, NULL,
	                                               "bad\xff"));

	/* Long: the END is kept, on a character boundary. */
	filler = g_strnfill(BAR_ASSISTANT_MAX_HANDOFF, 'x');
	huge = g_strconcat("START", filler, "\xc3\xa9" "END", NULL);
	cut = bar_assistant_cmacs_open_cmdline(NULL, NULL, NULL, huge);
	g_assert_nonnull(cut);
	g_assert_null(strstr(cut, "START"));
	g_assert_nonnull(strstr(cut, "\xc3\xa9" "END"));
	g_assert_true(g_utf8_validate(cut, -1, NULL));
}

static void
test_format_bytes(void)
{
	g_autofree gchar *a = bar_assistant_format_bytes(999);
	g_autofree gchar *b = bar_assistant_format_bytes(4661224676ULL);
	g_autofree gchar *c = bar_assistant_format_bytes(1500);

	g_assert_cmpstr(a, ==, "999 B");
	g_assert_cmpstr(b, ==, "4.7 GB");
	g_assert_cmpstr(c, ==, "1.5 kB");
}

/* ════════════════════════════════════════════════════════════════════
 * The widget, through the real bar.so
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
	BarHarness  h;
	gchar      *curl_log;
	gchar      *ai_args;
	gchar      *ai_stdin;
} W;

/* Every LABEL title in a panel, in order, joined with `\n---\n'. */
static gchar *
panel_labels(GowlBarPanel *panel)
{
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < gowl_bar_panel_n_items(panel); i++) {
		GowlBarPanelItem *item = gowl_bar_panel_get_item(panel, i);

		if (gowl_bar_panel_item_get_kind(item) != GOWL_BAR_ITEM_LABEL)
			continue;
		if (out->len > 0)
			g_string_append(out, "\n---\n");
		g_string_append(out, gowl_bar_panel_item_get_title(item));
	}
	return g_string_free(out, FALSE);
}

static gboolean
mentions(W *w, const gchar *needle)
{
	g_autoptr(GowlBarPanel) panel = bar_harness_panel(&w->h);
	g_autofree gchar *labels = panel_labels(panel);

	return bar_harness_panel_mentions(panel, needle) ||
	       strstr(labels, needle) != NULL;
}

static gchar *
read_file(const gchar *path)
{
	gchar *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		return g_strdup("");
	return text;
}

/*
 * Bring the widget up with a fake curl serving the fixtures and a fake
 * `ai'.  @pull_fixture is what /api/pull streams.
 */
static gboolean
w_up(W *w, const gchar *pull_fixture)
{
	g_autofree gchar *curl_body = NULL;
	g_autofree gchar *ai_body = NULL;
	g_autofree gchar *curl = NULL;
	g_autofree gchar *ai = NULL;
	g_autofree gchar *fixdir = NULL;
	g_autofree gchar *models = NULL;
	g_autofree gchar *proc = NULL;
	g_autofree gchar *sys = NULL;

	memset(w, 0, sizeof(*w));
	if (!bar_harness_up(&w->h, "bar_register_assistant_plugins",
	                    "ai-assistant"))
		return FALSE;

	fixdir = g_build_filename(GOWL_TEST_FIXTURES, "ai-assistant", NULL);
	w->curl_log = bar_harness_path(&w->h, "curl.log");
	w->ai_args  = bar_harness_path(&w->h, "ai.args");
	w->ai_stdin = bar_harness_path(&w->h, "ai.stdin");

	curl_body = g_strdup_printf(
		"for a in \"$@\"; do printf '%%s\\n' \"$a\" >> '%s'; done\n"
		"echo '--END--' >> '%s'\n"
		"url=; prev=\n"
		"for a in \"$@\"; do [ \"$prev\" = --url ] && url=$a; prev=$a; done\n"
		"case \"$url\" in\n"
		"  */api/tags) cat '%s/tags.json' ;;\n"
		"  */api/ps) cat '%s/ps.json' ;;\n"
		"  */api/pull) cat '%s/%s' ;;\n"
		"  */api/generate) echo '{\"done\":true}'; echo 200 ;;\n"
		"  */api/delete) echo 200 ;;\n"
		"  *) exit 7 ;;\n"
		"esac\n",
		w->curl_log, w->curl_log, fixdir, fixdir, fixdir,
		(pull_fixture != NULL) ? pull_fixture : "pull-progress.txt");
	curl = bar_harness_script(&w->h, "fake-curl", curl_body);

	/* The fake model: lists providers, or answers with what it was
	   asked, recording argv and stdin. */
	ai_body = g_strdup_printf(
		"if [ \"$1\" = --list-providers ]; then cat '%s/list-providers.txt';"
		" exit 0; fi\n"
		"printf '%%s\\n' \"$@\" > '%s'\n"
		"cat > '%s'\n"
		"if [ -n \"$FAKE_AI_ANSWER\" ]; then printf '%%s' \"$FAKE_AI_ANSWER\";"
		" else printf 'ANSWER %%s' \"$(wc -c < '%s' | tr -d ' ')\"; fi\n",
		fixdir, w->ai_args, w->ai_stdin, w->ai_stdin);
	ai = bar_harness_script(&w->h, "fake-ai", ai_body);

	models = bar_harness_path(&w->h, "models");
	proc = g_build_filename(fixdir, "root", "proc", NULL);
	sys  = g_build_filename(fixdir, "root", "sys", NULL);

	gowl_bar_plugin_set_setting(w->h.plugin, "curl", curl);
	gowl_bar_plugin_set_setting(w->h.plugin, "ai-command", ai);
	gowl_bar_plugin_set_setting(w->h.plugin, "ai-backend", "ai");
	gowl_bar_plugin_set_setting(w->h.plugin, "nvidia-smi",
	                            "/nonexistent/nvidia-smi");
	gowl_bar_plugin_set_setting(w->h.plugin, "models-dir", models);
	gowl_bar_plugin_set_setting(w->h.plugin, "proc-root", proc);
	gowl_bar_plugin_set_setting(w->h.plugin, "sys-root", sys);
	gowl_bar_plugin_set_setting(w->h.plugin, "ollama-url",
	                            "http://127.0.0.1:11434");
	bar_harness_activate(&w->h);
	gowl_bar_plugin_configure(w->h.plugin, NULL);
	return TRUE;
}

static void
w_down(W *w)
{
	g_unsetenv("FAKE_AI_ANSWER");
	bar_harness_down(&w->h);
	g_free(w->curl_log);
	g_free(w->ai_args);
	g_free(w->ai_stdin);
}

static void
w_ask(W *w, const gchar *text)
{
	gowl_bar_plugin_panel_text(w->h.plugin, "prompt", text, TRUE);
}

/* Catches: the widget or its aliases not being registered. */
static void
test_widget_aliases(void)
{
	W w;
	g_autoptr(GowlBarPlugin) nova = NULL;
	g_autoptr(GowlBarPlugin) assistant = NULL;

	if (!w_up(&w, NULL))
		return;
	nova = gowl_bar_registry_instantiate(w.h.registry, "nova", NULL);
	assistant = gowl_bar_registry_instantiate(w.h.registry, "assistant",
	                                          NULL);
	g_assert_nonnull(nova);
	g_assert_nonnull(assistant);
	w_down(&w);
}

/* Catches: the conversation not travelling with the next question,
   the prompt landing in argv, or the system setting being ignored. */
static void
test_widget_cli_chat(void)
{
	W w;
	g_autofree gchar *stdin_text = NULL;
	g_autofree gchar *args = NULL;
	g_autoptr(GowlBarPanel) panel = NULL;
	g_autofree gchar *labels = NULL;

	if (!w_up(&w, NULL))
		return;
	bar_harness_set(&w.h, "system", "Be brief.");
	bar_harness_set(&w.h, "ai-provider", "ollama");
	bar_harness_set(&w.h, "ai-model", "qwen3:8b");

	w_ask(&w, "  hello there  ");
	/* The stub host ran the request inline; the answer is in. */
	stdin_text = read_file(w.ai_stdin);
	g_assert_cmpstr(stdin_text, ==, "User: hello there");
	args = read_file(w.ai_args);
	g_assert_nonnull(strstr(args, "--system\nBe brief.\n"));
	g_assert_nonnull(strstr(args, "--provider\nollama\n"));
	g_assert_nonnull(strstr(args, "--model\nqwen3:8b\n"));
	g_assert_null(strstr(args, "hello"));

	panel = bar_harness_panel(&w.h);
	labels = panel_labels(panel);
	/* Newest first: the answer, then the question. */
	g_assert_nonnull(strstr(labels, "AI: ANSWER 17\n---\nYou: hello there"));

	g_clear_pointer(&stdin_text, g_free);
	w_ask(&w, "and again?");
	stdin_text = read_file(w.ai_stdin);
	g_assert_cmpstr(stdin_text, ==,
		"User: hello there\n\nAssistant: ANSWER 17\n\nUser: and again?");

	w_down(&w);
}

/* Catches: a model's `<b>' or `&' being interpreted (markup) or
   escaped instead of shown exactly as written. */
static void
test_widget_output_is_literal(void)
{
	W w;
	g_autoptr(GowlBarPanel) panel = NULL;
	g_autofree gchar *labels = NULL;

	if (!w_up(&w, NULL))
		return;
	g_setenv("FAKE_AI_ANSWER", "<b>bold</b> & <span color='red'>x</span>",
	         TRUE);
	w_ask(&w, "markup?");
	panel = bar_harness_panel(&w.h);
	labels = panel_labels(panel);
	g_assert_nonnull(strstr(labels,
		"AI: <b>bold</b> & <span color='red'>x</span>"));
	g_assert_null(strstr(labels, "&amp;"));
	w_down(&w);
}

static guint
count_prefix(const gchar *labels, const gchar *prefix)
{
	g_auto(GStrv) parts = g_strsplit(labels, "\n---\n", -1);
	guint i, n = 0;

	for (i = 0; parts[i] != NULL; i++)
		if (g_str_has_prefix(parts[i], prefix))
			n++;
	return n;
}

/* Catches: the history cap not applying, so the prompt grows forever. */
static void
test_widget_history_cap(void)
{
	W w;
	g_autoptr(GowlBarPanel) panel = NULL;
	g_autofree gchar *labels = NULL;
	g_autofree gchar *stdin_text = NULL;
	guint i;

	if (!w_up(&w, NULL))
		return;
	bar_harness_set(&w.h, "history-turns", "4");
	for (i = 0; i < 5; i++) {
		g_autofree gchar *q = g_strdup_printf("question %u", i);

		w_ask(&w, q);
	}
	panel = bar_harness_panel(&w.h);
	labels = panel_labels(panel);
	g_assert_cmpuint(count_prefix(labels, "You: "), ==, 2);
	g_assert_cmpuint(count_prefix(labels, "AI: "), ==, 2);
	g_assert_null(strstr(labels, "question 0"));

	/* The last prompt carried only what was kept. */
	stdin_text = read_file(w.ai_stdin);
	g_assert_null(strstr(stdin_text, "question 0"));
	g_assert_null(strstr(stdin_text, "question 1"));
	g_assert_true(g_str_has_suffix(stdin_text, "User: question 4"));
	w_down(&w);
}

/* A fake emacsctl recording the form it is given. */
static gchar *
fake_emacsctl(W *w, gchar **formfile)
{
	g_autofree gchar *body = NULL;

	*formfile = bar_harness_path(&w->h, "form");
	body = g_strdup_printf("[ \"$1\" = eval ] || exit 9\n"
	                       "printf '%%s' \"$2\" > '%s'\necho t", *formfile);
	return bar_harness_script(&w->h, "fake-emacsctl", body);
}

static gchar *
current_request_id(W *w)
{
	g_autofree gchar *form = NULL;
	gchar *start, *end;
	g_autofree gchar *formfile = bar_harness_path(&w->h, "form");

	form = read_file(formfile);
	/* (cmacs-gowl-bar-ai-run "ai-assistant" "ID" ... */
	start = strstr(form, "\"ai-assistant\" \"");
	g_assert_nonnull(start);
	start += strlen("\"ai-assistant\" \"");
	end = strchr(start, '"');
	return g_strndup(start, (gsize)(end - start));
}

/* Catches: the cmacs route not reaching cmacs-gowl-bar-ai-run with the
   whole conversation, cmacs's pushed answer not landing, or the hand-
   off button not spawning a quoted emacsctl line. */
static void
test_widget_cmacs_route(void)
{
	W w;
	g_autofree gchar *ctl = NULL;
	g_autofree gchar *formfile = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *id = NULL;

	if (!w_up(&w, NULL))
		return;
	ctl = fake_emacsctl(&w, &formfile);
	bar_harness_set(&w.h, "emacsctl", ctl);
	bar_harness_set(&w.h, "ai-backend", "cmacs");
	bar_harness_set(&w.h, "ai-default-provider", "claude-code");
	bar_harness_set(&w.h, "ai-default-model", "sonnet");
	/* cmacs's default model is shown beside cmacs's default provider. */
	g_assert_true(mentions(&w, "sonnet"));

	w_ask(&w, "say \"hi\")");
	form = read_file(formfile);
	g_assert_true(g_str_has_prefix(form,
		"(cmacs-gowl-bar-ai-run \"ai-assistant\" \""));
	g_assert_nonnull(strstr(form, "\"User: say \\\"hi\\\")\""));
	/* Provider unset: nil, so cmacs uses its own default. */
	g_assert_true(g_str_has_suffix(form, " nil nil)"));
	g_assert_true(mentions(&w, "AI: thinking"));

	/* cmacs streams, then finishes. */
	id = current_request_id(&w);
	bar_harness_set(&w.h, "ai-id", id);
	bar_harness_set(&w.h, "ai-state", "streaming");
	bar_harness_set(&w.h, "ai-text", "Hel");
	g_assert_true(mentions(&w, "AI: Hel"));
	/* cmacs pushes all keys at once; one at a time, the text has to
	   land before the state that says it is final. */
	bar_harness_set(&w.h, "ai-text", "Hello from cmacs");
	bar_harness_set(&w.h, "ai-state", "done");
	g_assert_true(mentions(&w, "AI: Hello from cmacs"));
	g_assert_false(mentions(&w, "AI: thinking"));

	/* Continue in cmacs: button 4, only offered on this route. */
	gowl_bar_plugin_panel_action(w.h.plugin, "chat", 4, 0.0, 1);
	g_assert_true(bar_harness_spawned(&w.h,
		"eval '(cmacs-gowl-bar-assistant-open-in-chat \"claude-code\" "
		"nil \"User: say \\\"hi\\\")\n\nAssistant: Hello from cmacs\")'"));
	w_down(&w);
}

/* Catches: Stop losing what had streamed, or a late answer for the
   stopped request being painted in afterwards. */
static void
test_widget_stop(void)
{
	W w;
	g_autofree gchar *ctl = NULL;
	g_autofree gchar *formfile = NULL;
	g_autofree gchar *id = NULL;

	if (!w_up(&w, NULL))
		return;
	ctl = fake_emacsctl(&w, &formfile);
	bar_harness_set(&w.h, "emacsctl", ctl);
	bar_harness_set(&w.h, "ai-backend", "cmacs");

	w_ask(&w, "long one");
	id = current_request_id(&w);
	bar_harness_set(&w.h, "ai-id", id);
	bar_harness_set(&w.h, "ai-state", "streaming");
	bar_harness_set(&w.h, "ai-text", "partial");
	g_assert_cmpint(gowl_bar_plugin_get_interval(w.h.plugin), ==, 1);

	gowl_bar_plugin_panel_action(w.h.plugin, "chat", 1, 0.0, 1);
	g_assert_true(mentions(&w, "AI: partial [stopped]"));
	g_assert_true(mentions(&w, "Stopped."));

	bar_harness_set(&w.h, "ai-state", "done");
	bar_harness_set(&w.h, "ai-text", "late full answer");
	g_assert_false(mentions(&w, "late full answer"));
	/* Idle again: back to the slow interval. */
	g_assert_cmpint(gowl_bar_plugin_get_interval(w.h.plugin), ==, 10);
	w_down(&w);
}

/* Catches: a state colouring with the wrong role, or a per-state
   override (role name or hex) being ignored. */
static void
test_widget_state_colour(void)
{
	W w;
	g_autofree gchar *ctl = NULL;
	g_autofree gchar *formfile = NULL;
	g_autofree gchar *id = NULL;
	gdouble rgba[4];

	if (!w_up(&w, NULL))
		return;
	ctl = fake_emacsctl(&w, &formfile);
	bar_harness_set(&w.h, "emacsctl", ctl);
	bar_harness_set(&w.h, "ai-backend", "cmacs");

	bar_harness_poll(&w.h);
	g_assert_cmpint(gowl_bar_plugin_get_color(w.h.plugin), ==,
	                GOWL_BAR_COLOR_SUBTEXT);

	w_ask(&w, "q");
	bar_harness_poll(&w.h);
	g_assert_cmpint(gowl_bar_plugin_get_color(w.h.plugin), ==,
	                GOWL_BAR_COLOR_YELLOW);

	id = current_request_id(&w);
	bar_harness_set(&w.h, "ai-id", id);
	bar_harness_set(&w.h, "ai-state", "streaming");
	bar_harness_set(&w.h, "ai-text", "abc");
	bar_harness_poll(&w.h);
	g_assert_cmpint(gowl_bar_plugin_get_color(w.h.plugin), ==,
	                GOWL_BAR_COLOR_ACCENT);

	bar_harness_set(&w.h, "color-streaming", "#ff0000");
	bar_harness_poll(&w.h);
	g_assert_true(gowl_bar_plugin_get_color_rgba(w.h.plugin, rgba));
	g_assert_cmpfloat_with_epsilon(rgba[0], 1.0, 1e-6);

	bar_harness_set(&w.h, "ai-error", "quota exceeded");
	bar_harness_set(&w.h, "ai-state", "error");
	bar_harness_poll(&w.h);
	g_assert_cmpint(gowl_bar_plugin_get_color(w.h.plugin), ==,
	                GOWL_BAR_COLOR_RED);
	g_assert_true(mentions(&w, "quota exceeded"));

	bar_harness_set(&w.h, "color-error", "peach");
	bar_harness_poll(&w.h);
	g_assert_cmpint(gowl_bar_plugin_get_color(w.h.plugin), ==,
	                GOWL_BAR_COLOR_PEACH);
	g_assert_false(gowl_bar_plugin_get_color_rgba(w.h.plugin, rgba));
	w_down(&w);
}

/* Catches: reduced-motion still animating (no glyph), the glyph
   showing otherwise, or show-label not labelling. */
static void
test_widget_reduced_motion_and_label(void)
{
	W w;
	g_autofree gchar *icon = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *icon2 = NULL;
	g_autofree gchar *tip = NULL;

	if (!w_up(&w, NULL))
		return;
	bar_harness_poll(&w.h);
	icon = gowl_bar_plugin_dup_icon(w.h.plugin);
	g_assert_null(icon);
	tip = gowl_bar_plugin_dup_tooltip(w.h.plugin);
	g_assert_nonnull(strstr(tip, "idle"));

	bar_harness_set(&w.h, "reduced-motion", "true");
	bar_harness_set(&w.h, "show-label", "true");
	bar_harness_set(&w.h, "ai-provider", "ollama");
	bar_harness_set(&w.h, "ai-model", "qwen3:8b");
	bar_harness_poll(&w.h);
	icon2 = gowl_bar_plugin_dup_icon(w.h.plugin);
	g_assert_cmpstr(icon2, ==, AS_TEST_GLYPH);
	label = gowl_bar_plugin_dup_label(w.h.plugin);
	g_assert_cmpstr(label, ==, "ollama/qwen3:8b");
	w_down(&w);
}

/* Catches: machine-context not reaching the system prompt, or the
   machine read ignoring the configured roots. */
static void
test_widget_machine_context(void)
{
	W w;
	g_autofree gchar *args = NULL;

	if (!w_up(&w, NULL))
		return;
	bar_harness_set(&w.h, "machine-context", "true");
	bar_harness_poll(&w.h);        /* the worker reads the machine */
	w_ask(&w, "how loaded am I?");
	args = read_file(w.ai_args);
	g_assert_nonnull(strstr(args, "Live system info"));
	g_assert_nonnull(strstr(args, "4 CPU threads, load 1.20 0.98 0.75"));
	g_assert_nonnull(strstr(args, "loaded models: qwen3:8b"));
	g_assert_nonnull(strstr(args, "largest processes by memory: ollama"));
	w_down(&w);
}

static guint
count_in(const gchar *haystack, const gchar *needle)
{
	guint n = 0;
	const gchar *p = haystack;

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
}

/* Catches: a delete running without its approval card, the card not
   saying what it costs, or the body sent being anything but the
   exact one. */
static void
test_widget_models_delete_needs_approval(void)
{
	W w;
	g_autofree gchar *log = NULL;
	const gchar *name = "hf.co/LiquidAI/LFM2-1.2B-RAG-GGUF:Q8_0";
	g_autofree gchar *id = g_strconcat("inst:", name, NULL);

	if (!w_up(&w, NULL))
		return;
	gowl_bar_plugin_panel_opened(w.h.plugin);
	gowl_bar_plugin_panel_action(w.h.plugin, "tab", 1, 0.0, 1);
	g_assert_true(mentions(&w, name));
	g_assert_true(mentions(&w, "comfortable"));
	g_assert_true(mentions(&w, "Installed (4)"));
	g_assert_true(mentions(&w, "qwen3:8b"));      /* loaded */

	gowl_bar_plugin_panel_action(w.h.plugin, id, -1, 0.0, 1);
	gowl_bar_plugin_panel_action(w.h.plugin, "model-actions", 2, 0.0, 1);
	g_assert_true(mentions(&w, "Confirm"));
	g_assert_true(mentions(&w, "Delete hf.co/LiquidAI/LFM2-1.2B-RAG-GGUF:"
	                       "Q8_0 (1.2 GB) from disk. Disk after:"));
	log = read_file(w.curl_log);
	g_assert_null(strstr(log, "DELETE"));
	g_clear_pointer(&log, g_free);

	/* Cancel: nothing happens. */
	gowl_bar_plugin_panel_action(w.h.plugin, "approval", 1, 0.0, 1);
	g_assert_false(mentions(&w, "Confirm"));
	log = read_file(w.curl_log);
	g_assert_null(strstr(log, "DELETE"));
	g_clear_pointer(&log, g_free);

	/* Approve: exactly one DELETE with exactly this body. */
	gowl_bar_plugin_panel_action(w.h.plugin, "model-actions", 2, 0.0, 1);
	gowl_bar_plugin_panel_action(w.h.plugin, "approval", 0, 0.0, 1);
	log = read_file(w.curl_log);
	g_assert_cmpuint(count_in(log, "DELETE\n"), ==, 1);
	g_assert_nonnull(strstr(log, "--data-binary\n{\"model\":\"hf.co/"
		"LiquidAI/LFM2-1.2B-RAG-GGUF:Q8_0\",\"name\":\"hf.co/LiquidAI/"
		"LFM2-1.2B-RAG-GGUF:Q8_0\"}\n--url\n"
		"http://127.0.0.1:11434/api/delete\n"));
	g_assert_true(mentions(&w, "Deleted hf.co/LiquidAI/"));
	/* Approve again with nothing armed: no second delete. */
	gowl_bar_plugin_panel_action(w.h.plugin, "approval", 0, 0.0, 1);
	g_clear_pointer(&log, g_free);
	log = read_file(w.curl_log);
	g_assert_cmpuint(count_in(log, "DELETE\n"), ==, 1);
	w_down(&w);
}

/* Catches: a load running without approval, or unload needing one. */
static void
test_widget_load_and_unload(void)
{
	W w;
	g_autofree gchar *log = NULL;

	if (!w_up(&w, NULL))
		return;
	bar_harness_set(&w.h, "keep-alive", "1h");
	gowl_bar_plugin_panel_opened(w.h.plugin);
	gowl_bar_plugin_panel_action(w.h.plugin, "tab", 1, 0.0, 1);
	gowl_bar_plugin_panel_action(w.h.plugin, "inst:tatertard-massive:latest",
	                             -1, 0.0, 1);
	gowl_bar_plugin_panel_action(w.h.plugin, "model-actions", 0, 0.0, 1);
	g_assert_true(mentions(&w, "Load tatertard-massive:latest into memory"));
	log = read_file(w.curl_log);
	g_assert_null(strstr(log, "/api/generate"));
	g_clear_pointer(&log, g_free);

	gowl_bar_plugin_panel_action(w.h.plugin, "approval", 0, 0.0, 1);
	log = read_file(w.curl_log);
	g_assert_nonnull(strstr(log, "{\"model\":\"tatertard-massive:latest\","
		"\"prompt\":\"\",\"keep_alive\":\"1h\",\"stream\":false}"));
	g_clear_pointer(&log, g_free);

	/* Unloading a loaded model is immediate. */
	gowl_bar_plugin_panel_action(w.h.plugin, "loaded:qwen3:8b", -1, 0.0, 1);
	log = read_file(w.curl_log);
	g_assert_nonnull(strstr(log, "{\"model\":\"qwen3:8b\",\"prompt\":\"\","
		"\"keep_alive\":0,\"stream\":false}"));
	w_down(&w);
}

/* Catches: Return in the Add-model field downloading without a card,
   a hostile name reaching curl, or progress/success not shown. */
static void
test_widget_pull(void)
{
	W w;
	g_autofree gchar *log = NULL;

	if (!w_up(&w, NULL))
		return;
	gowl_bar_plugin_panel_opened(w.h.plugin);
	gowl_bar_plugin_panel_action(w.h.plugin, "tab", 1, 0.0, 1);

	gowl_bar_plugin_panel_text(w.h.plugin, "pull-entry",
	                           "qwen3:8b; rm -rf ~", TRUE);
	g_assert_false(mentions(&w, "Confirm"));
	g_assert_true(mentions(&w, "That is not a model name"));

	gowl_bar_plugin_panel_text(w.h.plugin, "pull-entry", " qwen3:4b ",
	                           TRUE);
	g_assert_true(mentions(&w, "Download qwen3:4b from the ollama library"));
	log = read_file(w.curl_log);
	g_assert_null(strstr(log, "/api/pull"));
	g_clear_pointer(&log, g_free);

	gowl_bar_plugin_panel_action(w.h.plugin, "approval", 0, 0.0, 1);
	log = read_file(w.curl_log);
	g_assert_nonnull(strstr(log, "-N\n"));
	g_assert_nonnull(strstr(log, "{\"model\":\"qwen3:4b\",\"name\":"
	                        "\"qwen3:4b\",\"stream\":true}"));
	g_assert_true(mentions(&w, "Pulled qwen3:4b."));
	w_down(&w);
}

/* Catches: ollama's error line during a pull being lost. */
static void
test_widget_pull_error(void)
{
	W w;

	if (!w_up(&w, "pull-error.txt"))
		return;
	gowl_bar_plugin_panel_opened(w.h.plugin);
	gowl_bar_plugin_panel_action(w.h.plugin, "tab", 1, 0.0, 1);
	gowl_bar_plugin_panel_text(w.h.plugin, "pull-entry", "nope:1b", TRUE);
	gowl_bar_plugin_panel_action(w.h.plugin, "approval", 0, 0.0, 1);
	g_assert_true(mentions(&w, "Pull of nope:1b failed: pull model "
	                       "manifest: file does not exist"));
	bar_harness_poll(&w.h);
	g_assert_cmpint(gowl_bar_plugin_get_color(w.h.plugin), ==,
	                GOWL_BAR_COLOR_RED);
	w_down(&w);
}

/* Catches: Cancel not stopping a pull in flight. */
static void
test_widget_cancel_pull(void)
{
	W w;

	if (!w_up(&w, NULL))
		return;
	gowl_bar_plugin_panel_opened(w.h.plugin);
	gowl_bar_plugin_panel_action(w.h.plugin, "tab", 1, 0.0, 1);
	gowl_bar_plugin_panel_text(w.h.plugin, "pull-entry", "big:70b", TRUE);
	w.h.host->hold_work = TRUE;
	gowl_bar_plugin_panel_action(w.h.plugin, "approval", 0, 0.0, 1);
	g_assert_true(mentions(&w, "Pulling big:70b"));
	g_assert_cmpint(gowl_bar_plugin_get_interval(w.h.plugin), ==, 1);

	gowl_bar_plugin_panel_action(w.h.plugin, "pull-cancel", 0, 0.0, 1);
	g_assert_true(mentions(&w, "Cancelled the pull of big:70b."));
	/* The held worker runs now, sees it was cancelled, and does not
	   report success over the cancellation. */
	w.h.host->hold_work = FALSE;
	bar_harness_run_held(&w.h);
	g_assert_true(mentions(&w, "Cancelled the pull of big:70b."));
	g_assert_false(mentions(&w, "Pulled big:70b"));
	w_down(&w);
}

/* Catches: ollama being down or curl missing showing an empty panel
   instead of what to do about it. */
static void
test_widget_ollama_unreachable(void)
{
	W w;
	g_autofree gchar *dead = NULL;

	if (!w_up(&w, NULL))
		return;
	dead = bar_harness_script(&w.h, "dead-curl", "exit 7");
	bar_harness_set(&w.h, "curl", dead);
	bar_harness_set(&w.h, "ollama-url", "http://10.9.9.9:11434/");
	gowl_bar_plugin_panel_opened(w.h.plugin);
	gowl_bar_plugin_panel_action(w.h.plugin, "tab", 1, 0.0, 1);
	g_assert_true(mentions(&w, "Could not reach ollama at "
	                       "http://10.9.9.9:11434/"));
	g_assert_true(mentions(&w, "podman quadlet"));
	g_assert_true(mentions(&w, "not packaged in Fedora"));

	bar_harness_set(&w.h, "curl", "/nonexistent/curl");
	gowl_bar_plugin_panel_action(w.h.plugin, "refresh", 0, 0.0, 1);
	g_assert_true(mentions(&w, "curl is not installed"));
	w_down(&w);
}

/* Catches: Free model memory unloading without approval, or missing
   a model. */
static void
test_widget_free_memory(void)
{
	W w;
	g_autofree gchar *log = NULL;

	if (!w_up(&w, NULL))
		return;
	gowl_bar_plugin_panel_opened(w.h.plugin);
	gowl_bar_plugin_panel_action(w.h.plugin, "tab", 2, 0.0, 1);
	g_assert_true(mentions(&w, "AMD"));
	g_assert_true(mentions(&w, "qwen3:8b"));
	g_assert_true(mentions(&w, "ollama, firefox, emacs"));

	gowl_bar_plugin_panel_action(w.h.plugin, "free-memory", 0, 0.0, 1);
	g_assert_true(mentions(&w, "Unload all 1 loaded model, freeing about "
	                       "6.3 GB"));
	log = read_file(w.curl_log);
	g_assert_null(strstr(log, "\"keep_alive\":0"));
	g_clear_pointer(&log, g_free);

	gowl_bar_plugin_panel_action(w.h.plugin, "approval", 0, 0.0, 1);
	log = read_file(w.curl_log);
	g_assert_cmpuint(count_in(log, "\"keep_alive\":0"), ==, 1);
	w_down(&w);
}

/* Catches: the standalone provider list not coming from
   `ai --list-providers', or picking a provider not resetting the
   model (`sonnet' means nothing to ollama). */
static void
test_widget_provider_choice(void)
{
	W w;

	if (!w_up(&w, NULL))
		return;
	bar_harness_set(&w.h, "ai-model", "sonnet");
	bar_harness_poll(&w.h);        /* the worker asks `ai' once */
	gowl_bar_plugin_panel_action(w.h.plugin, "pick:provider", 0, 0.0, 1);
	g_assert_true(mentions(&w, "provider:claude-code"));
	g_assert_true(mentions(&w, "provider:openai-compatible"));

	bar_harness_set(&w.h, "ai-default-model", "default-of-another");
	gowl_bar_plugin_panel_action(w.h.plugin, "provider:ollama", 0, 0.0, 1);
	/* ...and never beside a provider the user picked. */
	g_assert_false(mentions(&w, "default-of-another"));
	g_assert_cmpstr(gowl_bar_plugin_get_setting(w.h.plugin, "ai-provider"),
	                ==, "ollama");
	g_assert_null(gowl_bar_plugin_get_setting(w.h.plugin, "ai-model"));

	/* ollama's model chooser lists what is installed. */
	gowl_bar_plugin_panel_opened(w.h.plugin);
	gowl_bar_plugin_panel_action(w.h.plugin, "pick:model", 0, 0.0, 1);
	g_assert_true(mentions(&w, "model:tatertard-massive:latest"));

	/* A pushed list wins over `ai', and other providers' models come
	   from cmacs's `models.<provider>'. */
	bar_harness_set(&w.h, "ai-providers", "claude,grok");
	bar_harness_set(&w.h, "models.claude", "sonnet,opus");
	gowl_bar_plugin_panel_action(w.h.plugin, "provider:claude", 0, 0.0, 1);
	gowl_bar_plugin_panel_action(w.h.plugin, "pick:provider", 0, 0.0, 1);
	g_assert_false(mentions(&w, "provider:claude-code"));
	g_assert_true(mentions(&w, "provider:grok"));
	gowl_bar_plugin_panel_action(w.h.plugin, "pick:model", 0, 0.0, 1);
	g_assert_true(mentions(&w, "model:opus"));

	/* Free text, validated. */
	gowl_bar_plugin_panel_text(w.h.plugin, "model-entry", "bad model!",
	                           TRUE);
	g_assert_null(gowl_bar_plugin_get_setting(w.h.plugin, "ai-model"));
	gowl_bar_plugin_panel_text(w.h.plugin, "model-entry", "claude-x-1",
	                           TRUE);
	g_assert_cmpstr(gowl_bar_plugin_get_setting(w.h.plugin, "ai-model"),
	                ==, "claude-x-1");
	w_down(&w);
}

/* Catches: Clear wiping the conversation on one click. */
static void
test_widget_clear_two_step(void)
{
	W w;

	if (!w_up(&w, NULL))
		return;
	w_ask(&w, "remember me");
	gowl_bar_plugin_panel_action(w.h.plugin, "chat", 2, 0.0, 1);
	g_assert_true(mentions(&w, "You: remember me"));
	g_assert_true(mentions(&w, "Click again to clear"));
	gowl_bar_plugin_panel_action(w.h.plugin, "chat", 2, 0.0, 1);
	g_assert_false(mentions(&w, "You: remember me"));
	w_down(&w);
}

/* Catches: no backend leaving a silent panel; a refused send losing
   the typed text. */
static void
test_widget_no_backend(void)
{
	W w;
	g_autoptr(GowlBarPanel) panel = NULL;
	GowlBarPanelItem *entry;

	if (!w_up(&w, NULL))
		return;
	bar_harness_set(&w.h, "ai-backend", "none");
	g_assert_true(mentions(&w, "No AI backend"));
	w_ask(&w, "keep this text");
	g_assert_true(mentions(&w, "no AI backend"));
	g_assert_false(mentions(&w, "You: keep this text"));
	panel = bar_harness_panel(&w.h);
	entry = gowl_bar_panel_find(panel, "prompt");
	g_assert_nonnull(entry);
	g_assert_cmpstr(gowl_bar_panel_item_get_value(entry), ==,
	                "keep this text");
	w_down(&w);
}

/* Catches: Copy putting the answer in argv instead of stdin, or a
   missing wl-copy failing silently. */
static void
test_widget_copy(void)
{
	W w;
	g_autofree gchar *copied = NULL;
	g_autofree gchar *argsfile = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *copy = NULL;
	g_autofree gchar *args = NULL;
	g_autofree gchar *got = NULL;

	if (!w_up(&w, NULL))
		return;
	copied = bar_harness_path(&w.h, "copied");
	argsfile = bar_harness_path(&w.h, "copy.args");
	body = g_strdup_printf("printf '%%s' \"$*\" > '%s'\ncat > '%s'",
	                       argsfile, copied);
	copy = bar_harness_script(&w.h, "fake-wl-copy", body);
	bar_harness_set(&w.h, "copy-command", copy);
	g_setenv("FAKE_AI_ANSWER", "the secret answer", TRUE);
	w_ask(&w, "q");
	gowl_bar_plugin_panel_action(w.h.plugin, "chat", 3, 0.0, 1);
	got = read_file(copied);
	g_assert_cmpstr(got, ==, "the secret answer");
	args = read_file(argsfile);
	g_assert_cmpstr(args, ==, "");

	bar_harness_set(&w.h, "copy-command", "/nonexistent/wl-copy");
	gowl_bar_plugin_panel_action(w.h.plugin, "chat", 3, 0.0, 1);
	g_assert_true(mentions(&w, "Fedora package: wl-clipboard"));
	w_down(&w);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-ai-assistant/model/provider-listing",
	                test_provider_listing);
	g_test_add_func("/bar-ai-assistant/model/split-list", test_split_list);
	g_test_add_func("/bar-ai-assistant/model/names", test_names);
	g_test_add_func("/bar-ai-assistant/model/ollama-tags", test_parse_tags);
	g_test_add_func("/bar-ai-assistant/model/ollama-ps", test_parse_ps);
	g_test_add_func("/bar-ai-assistant/model/ollama-malformed",
	                test_parse_malformed);
	g_test_add_func("/bar-ai-assistant/model/ollama-hostile",
	                test_parse_hostile);
	g_test_add_func("/bar-ai-assistant/model/ollama-huge", test_parse_huge);
	g_test_add_func("/bar-ai-assistant/model/pull-lines", test_pull_lines);
	g_test_add_func("/bar-ai-assistant/model/curl-argv", test_curl_argv);
	g_test_add_func("/bar-ai-assistant/model/fit", test_fit);
	g_test_add_func("/bar-ai-assistant/model/read-machine",
	                test_read_machine);
	g_test_add_func("/bar-ai-assistant/model/machine-summary",
	                test_machine_summary);
	g_test_add_func("/bar-ai-assistant/model/approval", test_approval);
	g_test_add_func("/bar-ai-assistant/model/transcript", test_transcript);
	g_test_add_func("/bar-ai-assistant/model/wave", test_wave);
	g_test_add_func("/bar-ai-assistant/model/look-keys", test_look_keys);
	g_test_add_func("/bar-ai-assistant/model/cmacs-cmdline",
	                test_cmacs_cmdline);
	g_test_add_func("/bar-ai-assistant/model/format-bytes",
	                test_format_bytes);

	g_test_add_func("/bar-ai-assistant/widget/aliases",
	                test_widget_aliases);
	g_test_add_func("/bar-ai-assistant/widget/cli-chat",
	                test_widget_cli_chat);
	g_test_add_func("/bar-ai-assistant/widget/output-is-literal",
	                test_widget_output_is_literal);
	g_test_add_func("/bar-ai-assistant/widget/history-cap",
	                test_widget_history_cap);
	g_test_add_func("/bar-ai-assistant/widget/cmacs-route",
	                test_widget_cmacs_route);
	g_test_add_func("/bar-ai-assistant/widget/stop", test_widget_stop);
	g_test_add_func("/bar-ai-assistant/widget/state-colour",
	                test_widget_state_colour);
	g_test_add_func("/bar-ai-assistant/widget/reduced-motion-and-label",
	                test_widget_reduced_motion_and_label);
	g_test_add_func("/bar-ai-assistant/widget/machine-context",
	                test_widget_machine_context);
	g_test_add_func("/bar-ai-assistant/widget/delete-needs-approval",
	                test_widget_models_delete_needs_approval);
	g_test_add_func("/bar-ai-assistant/widget/load-and-unload",
	                test_widget_load_and_unload);
	g_test_add_func("/bar-ai-assistant/widget/pull", test_widget_pull);
	g_test_add_func("/bar-ai-assistant/widget/pull-error",
	                test_widget_pull_error);
	g_test_add_func("/bar-ai-assistant/widget/cancel-pull",
	                test_widget_cancel_pull);
	g_test_add_func("/bar-ai-assistant/widget/ollama-unreachable",
	                test_widget_ollama_unreachable);
	g_test_add_func("/bar-ai-assistant/widget/free-memory",
	                test_widget_free_memory);
	g_test_add_func("/bar-ai-assistant/widget/provider-choice",
	                test_widget_provider_choice);
	g_test_add_func("/bar-ai-assistant/widget/clear-two-step",
	                test_widget_clear_two_step);
	g_test_add_func("/bar-ai-assistant/widget/no-backend",
	                test_widget_no_backend);
	g_test_add_func("/bar-ai-assistant/widget/copy", test_widget_copy);

	return g_test_run();
}
