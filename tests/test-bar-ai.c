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
 * The bar's one road to a language model (modules/bar/bar-ai.c), both
 * ways it can go, against stand-ins for `ai' and `emacsctl' written to
 * a scratch directory.
 *
 * Asserted:
 *
 *   THE PROMPT NEVER TRAVELS IN ARGV.  It goes on stdin: a paragraph of
 *   someone's email in `ps' output is a leak, and argv has a length
 *   limit a pasted document can reach.
 *
 *   THE ELISP FORM CANNOT BE BROKEN OUT OF.  A prompt holding quotes,
 *   backslashes and a closing paren stays one string literal; text
 *   that is not UTF-8 (which D-Bus would refuse anyway) is refused here
 *   with a reason rather than mangled.
 *
 *   A LATE ANSWER IS DROPPED.  cmacs echoes the request id; an answer
 *   for a request the user has moved past must not paint over the new
 *   one.
 *
 *   FAILURES SAY WHY.  A non-zero `ai' surfaces its stderr; a model
 *   that hangs is killed at the timeout; an unreachable cmacs says so.
 */

#include <glib.h>
#include <string.h>

#define BAR_HARNESS_MODULE GOWL_TEST_BAR_MODULE
#include "bar-widget-harness.h"
#include "barkit/gowl-bar-plugin-proxy.h"

#ifndef GOWL_TEST_BAR_MODULE
#define GOWL_TEST_BAR_MODULE "build/release/modules/bar.so"
#endif

/* Mirrors of modules/bar/bar-ai.h, reached through bar.so. */
typedef struct _BarAi BarAi;
enum { BACKEND_NONE = 0, BACKEND_CMACS, BACKEND_CLI };
enum { ST_IDLE = 0, ST_WAITING, ST_STREAMING, ST_DONE, ST_ERROR };

typedef struct {
	BarAi   *(*new_)        (const gchar *);
	void     (*free_)       (BarAi *);
	gint     (*parse)       (const gchar *, gboolean);
	gboolean (*start)       (BarAi *, GowlBarPlugin *, const gchar *,
	                         const gchar *, GError **);
	gboolean (*start_full)  (BarAi *, GowlBarPlugin *, const gchar *,
	                         const gchar *, const gchar *, const gchar *,
	                         GError **);
	void     (*cancel)      (BarAi *);
	gboolean (*absorb)      (BarAi *, GowlBarPlugin *);
	gint     (*get)         (BarAi *, gchar **, gchar **);
	gchar  **(*cli_argv)    (const gchar *, const gchar *, const gchar *,
	                         const gchar *);
	gchar   *(*cmacs_form)  (const gchar *, const gchar *, const gchar *,
	                         const gchar *, const gchar *, const gchar *);
	gchar   *(*request_id)  (BarAi *);
} AiApi;

static const GowlBarPluginVTable bare_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL,
	NULL, NULL, NULL, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL,
	NULL,
	NULL, NULL
};

typedef struct {
	BarHarness     h;
	AiApi          api;
	GowlBarPlugin *plugin;
	BarAi         *ai;
} Fixture;

static gboolean
fixture_up(Fixture *f)
{
	memset(f, 0, sizeof(*f));
	if (!bar_harness_up(&f->h, NULL, NULL))
		return FALSE;

#define SYM(field, name) \
	f->api.field = bar_harness_sym(&f->h, name); \
	if (f->api.field == NULL) { \
		g_test_skip("bar.so lacks " name); \
		return FALSE; \
	}
	SYM(new_,       "bar_ai_new");
	SYM(free_,      "bar_ai_free");
	SYM(parse,      "bar_ai_parse_backend");
	SYM(start,      "bar_ai_start");
	SYM(start_full, "bar_ai_start_full");
	SYM(cancel,     "bar_ai_cancel");
	SYM(absorb,     "bar_ai_absorb_settings");
	SYM(get,        "bar_ai_get");
	SYM(cli_argv,   "bar_ai_cli_argv");
	SYM(cmacs_form, "bar_ai_cmacs_form");
	SYM(request_id, "bar_ai_get_request_id");
#undef SYM

	f->plugin = gowl_bar_plugin_proxy_new("probe", "Probe", NULL,
	                                      &bare_vtable);
	gowl_bar_plugin_set_host(f->plugin, GOWL_BAR_HOST(f->h.host));
	f->ai = f->api.new_("probe");
	return TRUE;
}

static void
fixture_down(Fixture *f)
{
	if (f->ai != NULL)
		f->api.free_(f->ai);
	if (f->plugin != NULL) {
		gowl_bar_plugin_set_host(f->plugin, NULL);
		g_clear_object(&f->plugin);
	}
	bar_harness_down(&f->h);
}

/* ── Pure decisions ───────────────────────────────────────────────── */

static void
test_backend_selection(void)
{
	Fixture f;

	if (!fixture_up(&f))
		return;

	g_assert_cmpint(f.api.parse("cmacs", FALSE), ==, BACKEND_CMACS);
	g_assert_cmpint(f.api.parse("CMACS", TRUE), ==, BACKEND_CMACS);
	g_assert_cmpint(f.api.parse("ai", FALSE), ==, BACKEND_CLI);
	g_assert_cmpint(f.api.parse("ai-glib", FALSE), ==, BACKEND_CLI);
	g_assert_cmpint(f.api.parse("none", TRUE), ==, BACKEND_NONE);
	/* auto, unset and nonsense all follow what is installed. */
	g_assert_cmpint(f.api.parse("auto", TRUE), ==, BACKEND_CLI);
	g_assert_cmpint(f.api.parse("auto", FALSE), ==, BACKEND_NONE);
	g_assert_cmpint(f.api.parse(NULL, TRUE), ==, BACKEND_CLI);
	g_assert_cmpint(f.api.parse("bogus", FALSE), ==, BACKEND_NONE);

	fixture_down(&f);
}

static void
test_the_prompt_is_never_in_argv(void)
{
	Fixture f;
	g_auto(GStrv) full = NULL;
	g_auto(GStrv) bare = NULL;
	g_autofree gchar *joined = NULL;

	if (!fixture_up(&f))
		return;

	full = f.api.cli_argv("/opt/ai", "ollama", "qwen3", "Be terse.");
	joined = g_strjoinv(" ", full);
	g_assert_cmpstr(full[0], ==, "/opt/ai");
	g_assert_true(g_strv_contains((const gchar * const *)full,
	                              "--no-expand"));
	g_assert_nonnull(strstr(joined, "--provider ollama"));
	g_assert_nonnull(strstr(joined, "--model qwen3"));
	g_assert_nonnull(strstr(joined, "--system Be terse."));

	/* Unset provider and model leave ai to its saved defaults. */
	bare = f.api.cli_argv(NULL, "", NULL, NULL);
	g_assert_cmpstr(bare[0], ==, "ai");
	g_assert_false(g_strv_contains((const gchar * const *)bare,
	                               "--provider"));
	g_assert_false(g_strv_contains((const gchar * const *)bare,
	                               "--model"));
	g_assert_false(g_strv_contains((const gchar * const *)bare,
	                               "--system"));

	fixture_down(&f);
}

static void
test_the_elisp_form_cannot_be_broken_out_of(void)
{
	Fixture f;
	g_autofree gchar *form = NULL;
	g_autofree gchar *nil_form = NULL;
	g_autofree gchar *bad = NULL;

	if (!fixture_up(&f))
		return;

	form = f.api.cmacs_form("ai-rewrite", "id-1", NULL,
		"say \"hi\") (delete-file \"x\") \\ end\nline two",
		"claude", NULL);
	g_assert_cmpstr(form, ==,
		"(cmacs-gowl-bar-ai-run \"ai-rewrite\" \"id-1\" nil "
		"\"say \\\"hi\\\") (delete-file \\\"x\\\") \\\\ end\nline two\" "
		"\"claude\" nil)");

	nil_form = f.api.cmacs_form("w", "i", "", "p", "", "");
	g_assert_cmpstr(nil_form, ==,
	                "(cmacs-gowl-bar-ai-run \"w\" \"i\" nil \"p\" nil nil)");

	/* Not UTF-8: refused, not mangled. */
	bad = f.api.cmacs_form("w", "i", NULL, "\xff\xfe", NULL, NULL);
	g_assert_null(bad);

	fixture_down(&f);
}

/* ── The CLI route ────────────────────────────────────────────────── */

static void
test_the_cli_route_streams_an_answer(void)
{
	Fixture f;
	g_autofree gchar *argfile = NULL;
	g_autofree gchar *script = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *args = NULL;
	gchar *text = NULL, *err = NULL;
	GError *error = NULL;

	if (!fixture_up(&f))
		return;

	argfile = bar_harness_path(&f.h, "args");
	body = g_strdup_printf(
		"printf '%%s\\n' \"$@\" > '%s'\n"
		"in=$(cat)\n"
		"printf 'ANSWER: %%s\\n\\n' \"$in\"\n", argfile);
	script = bar_harness_script(&f.h, "fake-ai", body);

	gowl_bar_plugin_set_setting(f.plugin, "ai-backend", "ai");
	gowl_bar_plugin_set_setting(f.plugin, "ai-command", script);
	gowl_bar_plugin_set_setting(f.plugin, "ai-provider", "ollama");
	gowl_bar_plugin_set_setting(f.plugin, "ai-model", "qwen3");

	g_assert_true(f.api.start(f.ai, f.plugin, "Fix it.",
	                          "teh secret text", &error));
	g_assert_no_error(error);

	/* The stub ran the worker inline: the answer is in. */
	g_assert_cmpint(f.api.get(f.ai, &text, &err), ==, ST_DONE);
	g_assert_cmpstr(text, ==, "ANSWER: teh secret text");
	g_assert_null(err);
	g_assert_cmpuint(f.h.host->refreshes, >, 0);

	/* What ai was given: the settings, and NOT the prompt. */
	g_assert_true(g_file_get_contents(argfile, &args, NULL, NULL));
	g_assert_nonnull(strstr(args, "--no-expand\n"));
	g_assert_nonnull(strstr(args, "--provider\nollama\n"));
	g_assert_nonnull(strstr(args, "--model\nqwen3\n"));
	g_assert_nonnull(strstr(args, "--system\nFix it.\n"));
	g_assert_null(strstr(args, "secret"));

	g_free(text);
	g_free(err);
	fixture_down(&f);
}

/* A request's own provider and model win over the widget's settings;
   an empty one falls back to them. */
static void
test_a_request_names_its_own_model(void)
{
	Fixture f;
	g_autofree gchar *argfile = NULL;
	g_autofree gchar *script = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *args = NULL;
	g_autofree gchar *args2 = NULL;

	if (!fixture_up(&f))
		return;

	argfile = bar_harness_path(&f.h, "args");
	body = g_strdup_printf("printf '%%s\\n' \"$@\" > '%s'\ncat >/dev/null\n"
	                       "echo ok", argfile);
	script = bar_harness_script(&f.h, "fake-ai", body);
	gowl_bar_plugin_set_setting(f.plugin, "ai-backend", "ai");
	gowl_bar_plugin_set_setting(f.plugin, "ai-command", script);
	gowl_bar_plugin_set_setting(f.plugin, "ai-provider", "claude");
	gowl_bar_plugin_set_setting(f.plugin, "ai-model", "opus");

	g_assert_true(f.api.start_full(f.ai, f.plugin, NULL, "hi", "ollama",
	                               "qwen3", NULL));
	g_assert_true(g_file_get_contents(argfile, &args, NULL, NULL));
	g_assert_nonnull(strstr(args, "--provider\nollama\n"));
	g_assert_nonnull(strstr(args, "--model\nqwen3\n"));

	g_assert_true(f.api.start_full(f.ai, f.plugin, NULL, "hi", "", NULL,
	                               NULL));
	g_assert_true(g_file_get_contents(argfile, &args2, NULL, NULL));
	g_assert_nonnull(strstr(args2, "--provider\nclaude\n"));
	g_assert_nonnull(strstr(args2, "--model\nopus\n"));

	fixture_down(&f);
}

static void
test_a_failing_cli_says_why(void)
{
	Fixture f;
	g_autofree gchar *script = NULL;
	gchar *err = NULL;

	if (!fixture_up(&f))
		return;

	script = bar_harness_script(&f.h, "fake-ai",
		"cat >/dev/null\n"
		"echo 'warming up' >&2\n"
		"echo 'error: no API key for claude' >&2\n"
		"exit 3");
	gowl_bar_plugin_set_setting(f.plugin, "ai-backend", "ai");
	gowl_bar_plugin_set_setting(f.plugin, "ai-command", script);

	g_assert_true(f.api.start(f.ai, f.plugin, NULL, "hi", NULL));
	g_assert_cmpint(f.api.get(f.ai, NULL, &err), ==, ST_ERROR);
	/* The LAST line of stderr is the reason, not the first. */
	g_assert_nonnull(strstr(err, "no API key for claude"));
	g_assert_null(strstr(err, "warming up"));

	g_free(err);
	fixture_down(&f);
}

static void
test_a_hung_model_is_stopped(void)
{
	Fixture f;
	g_autofree gchar *script = NULL;
	gchar *err = NULL;
	gint64 started;

	if (!fixture_up(&f))
		return;

	script = bar_harness_script(&f.h, "fake-ai",
		"cat >/dev/null\nexec sleep 30");
	gowl_bar_plugin_set_setting(f.plugin, "ai-backend", "ai");
	gowl_bar_plugin_set_setting(f.plugin, "ai-command", script);
	gowl_bar_plugin_set_setting(f.plugin, "ai-timeout", "1");

	started = g_get_monotonic_time();
	g_assert_true(f.api.start(f.ai, f.plugin, NULL, "hi", NULL));
	g_assert_cmpint(f.api.get(f.ai, NULL, &err), ==, ST_ERROR);
	g_assert_nonnull(strstr(err, "too long"));
	g_assert_cmpint(g_get_monotonic_time() - started, <,
	                5 * G_USEC_PER_SEC);

	g_free(err);
	fixture_down(&f);
}

static void
test_nothing_to_send_and_no_backend(void)
{
	Fixture f;
	GError *error = NULL;

	if (!fixture_up(&f))
		return;

	gowl_bar_plugin_set_setting(f.plugin, "ai-backend", "ai");
	g_assert_false(f.api.start(f.ai, f.plugin, NULL, "", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);

	gowl_bar_plugin_set_setting(f.plugin, "ai-backend", "none");
	g_assert_false(f.api.start(f.ai, f.plugin, NULL, "hi", &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "ai-backend"));
	g_clear_error(&error);
	/* A refused request leaves the slot as it was. */
	g_assert_cmpint(f.api.get(f.ai, NULL, NULL), ==, ST_IDLE);

	fixture_down(&f);
}

/* ── The cmacs route ──────────────────────────────────────────────── */

static void
test_the_cmacs_route_takes_only_its_own_answer(void)
{
	Fixture f;
	g_autofree gchar *formfile = NULL;
	g_autofree gchar *script = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	gchar *text = NULL;

	if (!fixture_up(&f))
		return;

	formfile = bar_harness_path(&f.h, "form");
	body = g_strdup_printf(
		"[ \"$1\" = eval ] || exit 9\n"
		"printf '%%s' \"$2\" > '%s'\n"
		"echo t", formfile);
	script = bar_harness_script(&f.h, "fake-emacsctl", body);

	gowl_bar_plugin_set_setting(f.plugin, "ai-backend", "cmacs");
	gowl_bar_plugin_set_setting(f.plugin, "emacsctl", script);

	g_assert_true(f.api.start(f.ai, f.plugin, "sys", "first", NULL));
	first = f.api.request_id(f.ai);
	g_assert_true(f.api.start(f.ai, f.plugin, "sys", "second", NULL));
	second = f.api.request_id(f.ai);
	g_assert_cmpstr(first, !=, second);
	g_assert_cmpint(f.api.get(f.ai, NULL, NULL), ==, ST_WAITING);

	/* The form named the widget and the CURRENT request. */
	g_assert_true(g_file_get_contents(formfile, &form, NULL, NULL));
	g_assert_true(g_str_has_prefix(form,
	                               "(cmacs-gowl-bar-ai-run \"probe\" "));
	g_assert_nonnull(strstr(form, second));
	g_assert_nonnull(strstr(form, "\"second\""));

	/* cmacs answering the FIRST request, late: ignored. */
	gowl_bar_plugin_set_setting(f.plugin, "ai-id", first);
	gowl_bar_plugin_set_setting(f.plugin, "ai-state", "done");
	gowl_bar_plugin_set_setting(f.plugin, "ai-text", "stale");
	g_assert_false(f.api.absorb(f.ai, f.plugin));
	g_assert_cmpint(f.api.get(f.ai, NULL, NULL), ==, ST_WAITING);

	/* Streaming, then done, for the current one. */
	gowl_bar_plugin_set_setting(f.plugin, "ai-id", second);
	gowl_bar_plugin_set_setting(f.plugin, "ai-state", "streaming");
	gowl_bar_plugin_set_setting(f.plugin, "ai-text", "Hel");
	g_assert_true(f.api.absorb(f.ai, f.plugin));
	g_assert_cmpint(f.api.get(f.ai, &text, NULL), ==, ST_STREAMING);
	g_assert_cmpstr(text, ==, "Hel");
	g_clear_pointer(&text, g_free);

	gowl_bar_plugin_set_setting(f.plugin, "ai-state", "done");
	gowl_bar_plugin_set_setting(f.plugin, "ai-text", "Hello");
	g_assert_true(f.api.absorb(f.ai, f.plugin));
	g_assert_cmpint(f.api.get(f.ai, &text, NULL), ==, ST_DONE);
	g_assert_cmpstr(text, ==, "Hello");
	g_clear_pointer(&text, g_free);

	/* The same push again changes nothing. */
	g_assert_false(f.api.absorb(f.ai, f.plugin));

	fixture_down(&f);
}

static void
test_an_unreachable_cmacs_says_so(void)
{
	Fixture f;
	g_autofree gchar *script = NULL;
	gchar *err = NULL;

	if (!fixture_up(&f))
		return;

	script = bar_harness_script(&f.h, "fake-emacsctl", "exit 1");
	gowl_bar_plugin_set_setting(f.plugin, "ai-backend", "cmacs");
	gowl_bar_plugin_set_setting(f.plugin, "emacsctl", script);

	g_assert_true(f.api.start(f.ai, f.plugin, NULL, "hi", NULL));
	g_assert_cmpint(f.api.get(f.ai, NULL, &err), ==, ST_ERROR);
	g_assert_nonnull(strstr(err, "could not reach cmacs"));

	g_free(err);
	fixture_down(&f);
}

static void
test_cancel_returns_to_idle(void)
{
	Fixture f;
	g_autofree gchar *script = NULL;
	g_autofree gchar *id = NULL;

	if (!fixture_up(&f))
		return;

	script = bar_harness_script(&f.h, "fake-emacsctl", "echo t");
	gowl_bar_plugin_set_setting(f.plugin, "ai-backend", "cmacs");
	gowl_bar_plugin_set_setting(f.plugin, "emacsctl", script);

	g_assert_true(f.api.start(f.ai, f.plugin, NULL, "hi", NULL));
	id = f.api.request_id(f.ai);
	f.api.cancel(f.ai);
	g_assert_cmpint(f.api.get(f.ai, NULL, NULL), ==, ST_IDLE);

	/* An answer for the cancelled request is not resurrected. */
	gowl_bar_plugin_set_setting(f.plugin, "ai-id", id);
	gowl_bar_plugin_set_setting(f.plugin, "ai-state", "done");
	gowl_bar_plugin_set_setting(f.plugin, "ai-text", "late");
	g_assert_false(f.api.absorb(f.ai, f.plugin));
	g_assert_cmpint(f.api.get(f.ai, NULL, NULL), ==, ST_IDLE);

	fixture_down(&f);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-ai/backend-selection", test_backend_selection);
	g_test_add_func("/bar-ai/prompt-not-in-argv",
	                test_the_prompt_is_never_in_argv);
	g_test_add_func("/bar-ai/elisp-form-quoting",
	                test_the_elisp_form_cannot_be_broken_out_of);
	g_test_add_func("/bar-ai/cli-streams",
	                test_the_cli_route_streams_an_answer);
	g_test_add_func("/bar-ai/per-request-model",
	                test_a_request_names_its_own_model);
	g_test_add_func("/bar-ai/cli-failure", test_a_failing_cli_says_why);
	g_test_add_func("/bar-ai/cli-timeout", test_a_hung_model_is_stopped);
	g_test_add_func("/bar-ai/refusals", test_nothing_to_send_and_no_backend);
	g_test_add_func("/bar-ai/cmacs-own-answer-only",
	                test_the_cmacs_route_takes_only_its_own_answer);
	g_test_add_func("/bar-ai/cmacs-unreachable",
	                test_an_unreachable_cmacs_says_so);
	g_test_add_func("/bar-ai/cancel", test_cancel_returns_to_idle);

	return g_test_run();
}
