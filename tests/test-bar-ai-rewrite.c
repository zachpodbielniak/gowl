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
 * The `ai-rewrite' bar widget (modules/bar/bar-plugins-airewrite.c and
 * its pure half, bar-airewrite-model.c), offline: wl-paste, wl-copy,
 * `ai' and emacsctl are stand-in scripts in a scratch directory that
 * record their argv and stdin.
 *
 * What these tests hold the widget to:
 *
 *   THE TEXT NEVER TRAVELS IN ARGV.  Not to wl-copy (the result and,
 *   on restore, the user's previous clipboard -- possibly a password)
 *   and not to the model (bar-ai's rule, checked again here through
 *   the widget).
 *
 *   THE FALLBACKS ARE THE RIGHT ONES, IN ORDER: the primary selection,
 *   then the clipboard (and the panel says so), then a text field
 *   whose result can only be copied, never pasted over anything.
 *
 *   THE PUSHED ACTION FORMAT SURVIVES HOSTILE PROMPTS: tabs, newlines,
 *   backslashes, quotes and non-ASCII text round-trip exactly; a
 *   broken line costs that line, not the list.
 *
 *   THE DIFF IS RIGHT, and both of its sides reassemble into the texts
 *   they came from (the canvas paints exactly the answer).
 *
 *   THE PASTE SEQUENCE IS: copy, keys, wait, restore -- and restores
 *   only what it saved, only after keys actually went.
 *
 * The one step no headless test can take is the keystroke into a real
 * window (it needs a compositor, a keymap and a client); it is isolated
 * in paste_inject(), and everything it decides is tested here.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <cairo.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define BAR_HARNESS_MODULE GOWL_TEST_BAR_MODULE
#include "bar-widget-harness.h"
#include "../modules/bar/bar-airewrite-model.h"

#ifndef GOWL_TEST_BAR_MODULE
#define GOWL_TEST_BAR_MODULE "build/release/modules/bar.so"
#endif
#ifndef GOWL_TEST_FIXTURES
#define GOWL_TEST_FIXTURES "tests/fixtures/bar"
#endif

#define MOD_SHIFT (1u << 0)
#define MOD_CTRL  (1u << 2)

/* ── The model, reached through bar.so ────────────────────────────── */

typedef struct {
	GPtrArray *(*builtins)   (void);
	gchar     *(*escape)     (const gchar *);
	gchar     *(*unescape)   (const gchar *);
	gchar     *(*serialize)  (GPtrArray *);
	GPtrArray *(*parse)      (const gchar *, guint *);
	GPtrArray *(*from_yaml)  (const gchar *, gboolean *, GError **);
	GPtrArray *(*merge)      (GPtrArray *, GPtrArray *, gboolean);
	const BarAirewriteAction *(*find_key) (GPtrArray *, gchar);
	const BarAirewriteAction *(*find_id)  (GPtrArray *, const gchar *);
	BarAirewriteAction *(*action_new) (gchar, const gchar *,
	                                   const gchar *, const gchar *,
	                                   const gchar *);
	void       (*action_free) (BarAirewriteAction *);
	gchar     *(*compose)    (const BarAirewriteAction *, const gchar *,
	                          const gchar *);
	gchar     *(*clean)      (const gchar *, const gchar *);
	gchar     *(*preview)    (const gchar *, guint, guint);
	GPtrArray *(*tokenize)   (const gchar *);
	GPtrArray *(*diff)       (const gchar *, const gchar *);
	gchar     *(*diff_str)   (GPtrArray *);
	gboolean   (*parse_keys) (const gchar *, guint *, gchar **);
	const gchar *(*keys_for) (const gchar *, const gchar *, const gchar *,
	                          const gchar *);
	BarAirewritePasteStep (*step) (BarAirewritePaste *, gint64);
	gint       (*wake)       (const BarAirewritePaste *, gint64);
	gboolean   (*key_usable) (gchar);
} Api;

typedef struct {
	BarHarness h;
	Api        api;
	gchar     *primary;     /* what fake wl-paste --primary prints */
	gchar     *clip;        /* what fake wl-paste prints */
	gchar     *paste_args;  /* every wl-paste argv, one arg a line */
	gchar     *copy_args;   /* every wl-copy argv */
	gchar     *copied;      /* every wl-copy stdin, `--END--' after each */
	gchar     *form;        /* the last emacsctl eval form */
	gchar     *prompt;      /* the last `ai' stdin */
} Rig;

static gboolean
api_load(BarHarness *h, Api *api)
{
#define SYM(field, name) \
	api->field = bar_harness_sym(h, name); \
	if (api->field == NULL) { \
		g_test_skip("bar.so lacks " name); \
		return FALSE; \
	}
	SYM(builtins,    "bar_airewrite_builtin_actions");
	SYM(escape,      "bar_airewrite_escape");
	SYM(unescape,    "bar_airewrite_unescape");
	SYM(serialize,   "bar_airewrite_actions_serialize");
	SYM(parse,       "bar_airewrite_actions_parse");
	SYM(from_yaml,   "bar_airewrite_actions_from_yaml");
	SYM(merge,       "bar_airewrite_actions_merge");
	SYM(find_key,    "bar_airewrite_actions_find_key");
	SYM(find_id,     "bar_airewrite_actions_find_id");
	SYM(action_new,  "bar_airewrite_action_new");
	SYM(action_free, "bar_airewrite_action_free");
	SYM(compose,     "bar_airewrite_compose_prompt");
	SYM(clean,       "bar_airewrite_clean_answer");
	SYM(preview,     "bar_airewrite_preview");
	SYM(tokenize,    "bar_airewrite_tokenize");
	SYM(diff,        "bar_airewrite_diff");
	SYM(diff_str,    "bar_airewrite_diff_to_string");
	SYM(parse_keys,  "bar_airewrite_parse_keys");
	SYM(keys_for,    "bar_airewrite_paste_keys_for");
	SYM(step,        "bar_airewrite_paste_step");
	SYM(wake,        "bar_airewrite_paste_wake_ms");
	SYM(key_usable,  "bar_airewrite_key_usable");
#undef SYM
	return TRUE;
}

/* Only the model: no widget instance. */
static gboolean
model_up(BarHarness *h, Api *api)
{
	if (!bar_harness_up(h, NULL, NULL))
		return FALSE;
	return api_load(h, api);
}

static gboolean
rig_up(Rig *r)
{
	g_autofree gchar *paste_body = NULL;
	g_autofree gchar *copy_body = NULL;
	g_autofree gchar *ctl_body = NULL;
	g_autofree gchar *paste = NULL;
	g_autofree gchar *copy = NULL;
	g_autofree gchar *ctl = NULL;
	g_autofree gchar *config = NULL;

	memset(r, 0, sizeof(*r));
	if (!bar_harness_up(&r->h, "bar_register_airewrite_plugins",
	                    "ai-rewrite"))
		return FALSE;
	if (!api_load(&r->h, &r->api))
		return FALSE;

	r->primary    = bar_harness_path(&r->h, "primary");
	r->clip       = bar_harness_path(&r->h, "clip");
	r->paste_args = bar_harness_path(&r->h, "paste-args");
	r->copy_args  = bar_harness_path(&r->h, "copy-args");
	r->copied     = bar_harness_path(&r->h, "copied");
	r->form       = bar_harness_path(&r->h, "form");
	r->prompt     = bar_harness_path(&r->h, "prompt");

	/* wl-paste: prints the primary or the clipboard file, or fails
	   the way the real one does when there is no selection. */
	paste_body = g_strdup_printf(
		"printf '%%s\\n' \"$@\" '--' >> '%s'\n"
		"case \" $* \" in\n"
		"*' --primary '*) f='%s' ;;\n"
		"*) f='%s' ;;\n"
		"esac\n"
		"[ -f \"$f\" ] || { echo 'No selection' >&2; exit 1; }\n"
		"cat \"$f\"\n",
		r->paste_args, r->primary, r->clip);
	paste = bar_harness_script(&r->h, "wl-paste", paste_body);

	/* wl-copy: records argv and stdin, and becomes the clipboard. */
	copy_body = g_strdup_printf(
		"printf '%%s\\n' \"$@\" '--' >> '%s'\n"
		"in=$(cat; printf x); in=${in%%x}\n"
		"printf '%%s\\n--END--\\n' \"$in\" >> '%s'\n"
		"printf '%%s' \"$in\" > '%s'\n",
		r->copy_args, r->copied, r->clip);
	copy = bar_harness_script(&r->h, "wl-copy", copy_body);

	/* emacsctl: keeps the form; cmacs's answer is simulated by
	   pushing settings, as cmacs does. */
	ctl_body = g_strdup_printf(
		"[ \"$1\" = eval ] || exit 9\n"
		"printf '%%s' \"$2\" > '%s'\n"
		"echo t\n", r->form);
	ctl = bar_harness_script(&r->h, "emacsctl", ctl_body);

	config = bar_harness_path(&r->h, "no-such-ai-rewrite.yaml");
	gowl_bar_plugin_set_setting(r->h.plugin, "paste-command", paste);
	gowl_bar_plugin_set_setting(r->h.plugin, "copy-command", copy);
	gowl_bar_plugin_set_setting(r->h.plugin, "config", config);
	gowl_bar_plugin_set_setting(r->h.plugin, "emacsctl", ctl);
	gowl_bar_plugin_set_setting(r->h.plugin, "ai-backend", "cmacs");
	bar_harness_activate(&r->h);
	return TRUE;
}

static void
rig_down(Rig *r)
{
	g_free(r->primary);
	g_free(r->clip);
	g_free(r->paste_args);
	g_free(r->copy_args);
	g_free(r->copied);
	g_free(r->form);
	g_free(r->prompt);
	bar_harness_down(&r->h);
}

static void
put(const gchar *path, const gchar *text)
{
	g_assert_true(g_file_set_contents(path, text, -1, NULL));
}

static gchar *
slurp(const gchar *path)
{
	gchar *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		return g_strdup("");
	return text;
}

static gchar *
fixture(const gchar *name)
{
	g_autofree gchar *path = g_build_filename(GOWL_TEST_FIXTURES,
	                                          "ai-rewrite", name, NULL);

	return slurp(path);
}

static guint
count(const gchar *haystack, const gchar *needle)
{
	guint n = 0;
	const gchar *p = haystack;

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
}

static GowlBarPanel *
panel(Rig *r)
{
	return bar_harness_panel(&r->h);
}

static gboolean
panel_says(Rig *r, const gchar *needle)
{
	g_autoptr(GowlBarPanel) p = panel(r);

	return bar_harness_panel_mentions(p, needle);
}

static gboolean
panel_has(Rig *r, const gchar *id)
{
	g_autoptr(GowlBarPanel) p = panel(r);

	return gowl_bar_panel_find(p, id) != NULL;
}

static gboolean
key(Rig *r, guint keysym, guint mods)
{
	return gowl_bar_plugin_panel_key(r->h.plugin, keysym, mods, -1);
}

/* The request id out of the last form emacsctl was given:
   (cmacs-gowl-bar-ai-run "ai-rewrite" "ID" ...). */
static gchar *
request_id(Rig *r)
{
	g_autofree gchar *form = slurp(r->form);
	const gchar *anchor = "\"ai-rewrite\" \"";
	const gchar *p = strstr(form, anchor);
	const gchar *end;

	g_assert_nonnull(p);
	p += strlen(anchor);
	end = strchr(p, '"');
	g_assert_nonnull(end);
	return g_strndup(p, (gsize)(end - p));
}

/* cmacs answering: the pushes cmacs-gowl-bar-ai-run makes. */
static void
cmacs_answers(Rig *r, const gchar *state, const gchar *text)
{
	g_autofree gchar *id = request_id(r);

	gowl_bar_plugin_set_setting(r->h.plugin, "ai-id", id);
	gowl_bar_plugin_set_setting(r->h.plugin, "ai-text", text);
	bar_harness_set(&r->h, "ai-state", state);
}

/* Open the panel with @primary selected (NULL: nothing). */
static void
open_with(Rig *r, const gchar *primary)
{
	if (primary != NULL)
		put(r->primary, primary);
	else
		g_unlink(r->primary);
	gowl_bar_plugin_panel_opened(r->h.plugin);
}

/* ── Registration ─────────────────────────────────────────────────── */

/* Catches: the stub registering nothing, or an alias that points at a
   name that does not exist (the widget then silently never appears). */
static void
test_registered(void)
{
	BarHarness h;
	GError *error = NULL;
	const gchar *names[] = { "ai-rewrite", "ai-edit", "rewrite" };
	guint i;
	void (*reg)(GowlBarRegistry *);

	if (!bar_harness_up(&h, "bar_register_airewrite_plugins",
	                    "ai-rewrite"))
		return;
	reg = bar_harness_sym(&h, "bar_register_airewrite_plugins");
	g_assert_nonnull(reg);
	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		GowlBarPlugin *p = gowl_bar_registry_instantiate(h.registry,
		                                                 names[i],
		                                                 &error);

		g_assert_no_error(error);
		g_assert_nonnull(p);
		g_object_unref(p);
	}
	bar_harness_down(&h);
}

/* ── The pushed format ────────────────────────────────────────────── */

/* Catches: a prompt with a tab or newline splitting the record, or a
   backslash sequence in a prompt being decoded into a control char. */
static void
test_escape_round_trip(void)
{
	BarHarness h;
	Api api;
	const gchar *cases[] = {
		"",
		"plain",
		"tab\there",
		"new\nline\r\nCRLF",
		"back\\slash \\t literal \\n not escapes",
		"trailing backslash \\",
		"\"quotes\" and 'single' and `backtick` (parens)",
		"Naïve café — “smart” 日本語 👍",
		"\t\n\\\t\n\\"
	};
	guint i;

	if (!model_up(&h, &api))
		return;
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *esc = api.escape(cases[i]);
		g_autofree gchar *back = api.unescape(esc);

		g_assert_null(strchr(esc, '\t'));
		g_assert_null(strchr(esc, '\n'));
		g_assert_null(strchr(esc, '\r'));
		g_assert_cmpstr(back, ==, cases[i]);
	}
	{
		/* An unknown escape keeps its character; a lone trailing
		   backslash is a backslash. */
		g_autofree gchar *u = api.unescape("a\\qb\\");

		g_assert_cmpstr(u, ==, "aqb\\");
	}
	bar_harness_down(&h);
}

/* Catches: a field lost or shifted in the round trip, flags inverted,
   or the hostile prompt corrupting the record that follows it. */
static void
test_actions_format_round_trip(void)
{
	BarHarness h;
	Api api;
	g_autoptr(GPtrArray) list = NULL;
	g_autoptr(GPtrArray) back = NULL;
	g_autofree gchar *text = NULL;
	BarAirewriteAction *hostile;
	guint i, rejected = 99;

	if (!model_up(&h, &api))
		return;

	list = api.builtins();
	hostile = api.action_new('t', "tone", "Tone\t\"down\"",
		"System line 1\nline 2\twith tab \\ backslash \\n literal",
		"Rewrite {input}:\n\n“quoted” 日本語");
	hostile->input    = g_strdup("How?\tReally\nhow?");
	hostile->provider = g_strdup("ollama");
	hostile->model    = g_strdup("qwen3.5:9b");
	hostile->preview  = FALSE;
	hostile->diff     = FALSE;
	g_ptr_array_add(list, hostile);

	text = api.serialize(list);
	g_assert_true(g_str_has_prefix(text, "#ai-rewrite-actions 1\n"));
	/* One header plus one line per action: nothing leaked a newline. */
	g_assert_cmpuint(count(text, "\n"), ==, list->len + 1);

	back = api.parse(text, &rejected);
	g_assert_cmpuint(rejected, ==, 0);
	g_assert_cmpuint(back->len, ==, list->len);
	for (i = 0; i < list->len; i++) {
		const BarAirewriteAction *a = g_ptr_array_index(list, i);
		const BarAirewriteAction *b = g_ptr_array_index(back, i);

		g_assert_cmpint(a->key, ==, b->key);
		g_assert_cmpstr(a->id, ==, b->id);
		g_assert_cmpstr(a->label, ==, b->label);
		g_assert_cmpstr(a->system, ==, b->system);
		g_assert_cmpstr(a->instruction, ==, b->instruction);
		g_assert_cmpstr(a->input, ==, b->input);
		g_assert_cmpstr(a->provider, ==, b->provider);
		g_assert_cmpstr(a->model, ==, b->model);
		g_assert_cmpint(a->preview, ==, b->preview);
		g_assert_cmpint(a->diff, ==, b->diff);
	}
	bar_harness_down(&h);
}

/* Catches: one bad line taking the whole list down, a duplicate id
   making two rows act the same, or a reserved key (j/k move the
   selection) being bound and then never reachable. */
static void
test_actions_parse_hostile(void)
{
	BarHarness h;
	Api api;
	g_autoptr(GPtrArray) got = NULL;
	guint rejected = 0;
	const gchar *spec =
		"#ai-rewrite-actions 1\r\n"
		"\n"
		"# a comment\n"
		"a\tgood\tGood\tpd\t\t\t\tsys\tinst\r\n"
		"b\ttoo\tfew\tfields\n"
		"c\t\tNo id\tp\t\t\t\t\tinst\n"
		"d\tbad-utf8\tBad \xff\tp\t\t\t\t\tinst\n"
		"e\tgood\tDuplicate id\tp\t\t\t\t\tinst\n"
		"j\treserved\tReserved key\tp\t\t\t\t\tinst\n"
		"a\tsecond\tKey taken\tp\t\t\t\t\tinst\n"
		"zz\tlong\tTwo-char key\t\t\t\t\t\tinst\n";

	if (!model_up(&h, &api))
		return;
	got = api.parse(spec, &rejected);
	g_assert_cmpuint(rejected, ==, 4);
	g_assert_cmpuint(got->len, ==, 4);
	g_assert_nonnull(api.find_id(got, "good"));
	g_assert_cmpint(api.find_id(got, "good")->key, ==, 'a');
	g_assert_cmpstr(api.find_id(got, "good")->system, ==, "sys");
	/* Kept, minus the key it could not have. */
	g_assert_cmpint(api.find_id(got, "reserved")->key, ==, '\0');
	g_assert_cmpint(api.find_id(got, "second")->key, ==, '\0');
	g_assert_cmpint(api.find_id(got, "long")->key, ==, '\0');
	g_assert_false(api.find_id(got, "long")->preview);

	/* The reserved set. */
	g_assert_false(api.key_usable('j'));
	g_assert_false(api.key_usable('k'));
	g_assert_false(api.key_usable('y'));
	g_assert_false(api.key_usable('v'));
	g_assert_false(api.key_usable('F'));
	g_assert_true(api.key_usable('f'));
	g_assert_true(api.key_usable('7'));

	/* Nothing at all is an empty list, not a crash. */
	g_ptr_array_unref(api.parse(NULL, NULL));
	g_ptr_array_unref(api.parse("", NULL));
	bar_harness_down(&h);
}

/* Catches: the two halves of the push drifting apart.  The fixture is
   what cmacs's serialiser writes (cmacs's ERT suite asserts it writes
   exactly these bytes), so a format change on either side fails one
   of the two suites instead of silently losing every cmacs action. */
static void
test_parses_what_cmacs_pushes(void)
{
	BarHarness h;
	Api api;
	g_autofree gchar *spec = fixture("pushed-from-cmacs.txt");
	g_autoptr(GPtrArray) got = NULL;
	const BarAirewriteAction *a;
	guint rejected = 9;

	if (!model_up(&h, &api))
		return;
	got = api.parse(spec, &rejected);
	g_assert_cmpuint(rejected, ==, 0);
	g_assert_cmpuint(got->len, ==, 1);
	a = g_ptr_array_index(got, 0);
	g_assert_cmpint(a->key, ==, 't');
	g_assert_cmpstr(a->id, ==, "tone");
	g_assert_cmpstr(a->label, ==, "Tone \"down\"");
	g_assert_cmpstr(a->system, ==, "Line 1\nLine\t2 \\ back");
	g_assert_cmpstr(a->instruction, ==,
	                "Rewrite: {input}\n\n\xe2\x80\x9c\xe6\x97\xa5\xe6\x9c\xac"
	                "\xe8\xaa\x9e\xe2\x80\x9d");
	g_assert_cmpstr(a->input, ==, "How?");
	g_assert_null(a->provider);
	g_assert_null(a->model);
	g_assert_true(a->preview);
	g_assert_false(a->diff);
	bar_harness_down(&h);
}

/* ── YAML and merging ─────────────────────────────────────────────── */

/* Catches: the file's fields mapped to the wrong members, a broken
   entry costing the list, or a malformed file crashing the worker. */
static void
test_actions_from_yaml(void)
{
	BarHarness h;
	Api api;
	g_autofree gchar *yaml = fixture("actions.yaml");
	g_autofree gchar *only = fixture("replace.yaml");
	g_autofree gchar *list = fixture("not-a-mapping.yaml");
	g_autofree gchar *bad = fixture("malformed.yaml");
	g_autoptr(GPtrArray) got = NULL;
	g_autoptr(GPtrArray) got2 = NULL;
	g_autoptr(GPtrArray) empty = NULL;
	const BarAirewriteAction *a;
	GError *error = NULL;
	gboolean replace = TRUE;

	if (!model_up(&h, &api))
		return;

	got = api.from_yaml(yaml, &replace, &error);
	g_assert_no_error(error);
	g_assert_false(replace);
	g_assert_cmpuint(got->len, ==, 3);
	a = api.find_id(got, "friendlier");
	g_assert_nonnull(a);
	g_assert_cmpint(a->key, ==, 't');
	g_assert_cmpstr(a->provider, ==, "ollama");
	g_assert_cmpstr(a->model, ==, "qwen3.5:9b");
	g_assert_nonnull(strstr(a->system, "warmer, friendlier"));
	g_assert_true(a->preview);
	a = api.find_id(got, "summarize");
	g_assert_false(a->preview);
	g_assert_false(a->diff);
	a = api.find_id(got, "question");
	g_assert_cmpstr(a->input, ==, "What do you want to know?");

	got2 = api.from_yaml(only, &replace, NULL);
	g_assert_true(replace);
	g_assert_cmpuint(got2->len, ==, 1);

	empty = api.from_yaml("", &replace, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(empty->len, ==, 0);

	g_assert_null(api.from_yaml(list, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_null(api.from_yaml(bad, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	bar_harness_down(&h);
}

/* Catches: a configured `s' not taking the key from the built-in
   Shorten (two rows answering one key), an override moving to the end
   of the list, or replace mode keeping the built-ins. */
static void
test_actions_merge(void)
{
	BarHarness h;
	Api api;
	g_autoptr(GPtrArray) base = NULL;
	g_autoptr(GPtrArray) extra = NULL;
	g_autoptr(GPtrArray) merged = NULL;
	g_autoptr(GPtrArray) replaced = NULL;
	g_autofree gchar *yaml = fixture("actions.yaml");
	BarAirewriteAction *over;

	if (!model_up(&h, &api))
		return;

	base = api.builtins();
	g_assert_cmpstr(((BarAirewriteAction *)g_ptr_array_index(base, 0))->id,
	                ==, "proofread");
	extra = api.from_yaml(yaml, NULL, NULL);
	over = api.action_new('f', "proofread", "My proofread", NULL, "x");
	g_ptr_array_add(extra, over);

	merged = api.merge(base, extra, FALSE);
	/* Same id: replaced in place, first row still. */
	g_assert_cmpstr(((BarAirewriteAction *)g_ptr_array_index(merged, 0))
	                ->label, ==, "My proofread");
	/* `s': the configured Summarize won it; Shorten keeps its row. */
	g_assert_cmpstr(api.find_key(merged, 's')->id, ==, "summarize");
	g_assert_nonnull(api.find_id(merged, "shorten"));
	g_assert_cmpint(api.find_id(merged, "shorten")->key, ==, '\0');
	g_assert_cmpstr(api.find_key(merged, 't')->id, ==, "friendlier");
	g_assert_cmpstr(api.find_key(merged, 'c')->id, ==, "custom");

	replaced = api.merge(base, extra, TRUE);
	g_assert_null(api.find_id(replaced, "shorten"));
	g_assert_nonnull(api.find_id(replaced, "proofread"));
	bar_harness_down(&h);
}

/* ── Prompts ──────────────────────────────────────────────────────── */

/* Catches: the documented prompt shape drifting (`instruction, blank
   line, text'), the selection being trimmed or altered on the way to
   the model, or {input} replaced only once. */
static void
test_compose_prompt(void)
{
	BarHarness h;
	Api api;
	BarAirewriteAction *a;
	g_autofree gchar *p1 = NULL;
	g_autofree gchar *p2 = NULL;
	g_autofree gchar *p3 = NULL;

	if (!model_up(&h, &api))
		return;
	a = api.action_new('x', "x", "X", NULL,
	                   "  Do this: {input} (yes, {input})  ");
	p1 = api.compose(a, "be brief", "  the text\n");
	g_assert_cmpstr(p1, ==,
	                "Do this: be brief (yes, be brief)\n\n  the text\n");

	g_free(a->instruction);
	a->instruction = g_strdup("{input}");
	p2 = api.compose(a, "", "only text");
	g_assert_cmpstr(p2, ==, "only text");

	g_free(a->instruction);
	a->instruction = g_strdup("Fix it.");
	p3 = api.compose(a, NULL, "a \"quoted\" \\ text");
	g_assert_cmpstr(p3, ==, "Fix it.\n\na \"quoted\" \\ text");
	api.action_free(a);
	bar_harness_down(&h);
}

/* Catches: a fenced or quoted answer being pasted fences and all, or a
   rewritten line losing the newline the selection ended with. */
static void
test_clean_answer(void)
{
	BarHarness h;
	Api api;
	struct { const gchar *answer, *original, *expected; } cases[] = {
		{ "The cat.", "Teh cat.", "The cat." },
		{ "\n\nThe cat.\n\n", "Teh cat.", "The cat." },
		{ "The cat.", "Teh cat.\n", "The cat.\n" },
		{ "```\nThe cat.\n```", "Teh cat.", "The cat." },
		{ "```text\nThe cat.\n```\n", "Teh cat.", "The cat." },
		{ "```c\nint x;\n```", "```c\nint  x;\n```", "```c\nint x;\n```" },
		{ "\"The cat.\"", "Teh cat.", "The cat." },
		{ "\"The cat.\"", "\"Teh cat.\"", "\"The cat.\"" },
		{ "\xe2\x80\x9cThe cat.\xe2\x80\x9d", "Teh cat.", "The cat." },
		{ "", "x", "" }
	};
	guint i;

	if (!model_up(&h, &api))
		return;
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *got = api.clean(cases[i].answer,
		                                  cases[i].original);

		g_assert_cmpstr(got, ==, cases[i].expected);
	}
	{
		g_autofree gchar *pv = api.preview("one\ntwo\nthree", 100, 2);
		g_autofree gchar *pc = api.preview("abcdef", 3, 9);
		g_autofree gchar *pb = api.preview("bad \xff byte", 100, 9);

		g_assert_cmpstr(pv, ==, "one\ntwo\xe2\x80\xa6");
		g_assert_cmpstr(pc, ==, "abc\xe2\x80\xa6");
		g_assert_true(g_utf8_validate(pb, -1, NULL));
	}
	bar_harness_down(&h);
}

/* ── The diff ─────────────────────────────────────────────────────── */

/* Catches: tokens that do not reassemble into the text (the canvas
   would paint something other than the answer). */
static void
test_tokenize(void)
{
	BarHarness h;
	Api api;
	g_autofree gchar *sel = fixture("selection.txt");
	g_autoptr(GPtrArray) t = NULL;
	g_autoptr(GPtrArray) p = NULL;
	GString *joined;
	guint i;

	if (!model_up(&h, &api))
		return;
	t = api.tokenize(sel);
	joined = g_string_new(NULL);
	for (i = 0; i < t->len; i++)
		g_string_append(joined, g_ptr_array_index(t, i));
	g_assert_cmpstr(joined->str, ==, sel);
	g_string_free(joined, TRUE);

	p = api.tokenize("Hi,  you!!");
	g_assert_cmpuint(p->len, ==, 6);
	g_assert_cmpstr(g_ptr_array_index(p, 0), ==, "Hi");
	g_assert_cmpstr(g_ptr_array_index(p, 1), ==, ",");
	g_assert_cmpstr(g_ptr_array_index(p, 2), ==, "  ");
	g_assert_cmpstr(g_ptr_array_index(p, 3), ==, "you");
	g_assert_cmpstr(g_ptr_array_index(p, 4), ==, "!");
	g_assert_cmpstr(g_ptr_array_index(p, 5), ==, "!");
	bar_harness_down(&h);
}

/* Both sides of a diff must reassemble into the texts. */
static void
assert_diff_sides(Api *api, const gchar *before, const gchar *after)
{
	g_autoptr(GPtrArray) ops = api->diff(before, after);
	GString *b = g_string_new(NULL);
	GString *a = g_string_new(NULL);
	guint i;

	for (i = 0; i < ops->len; i++) {
		const BarAirewriteDiffOp *op = g_ptr_array_index(ops, i);

		if (op->kind != BAR_AIREWRITE_DIFF_INSERT)
			g_string_append(b, op->text);
		if (op->kind != BAR_AIREWRITE_DIFF_DELETE)
			g_string_append(a, op->text);
		/* Never an empty op, never two of a kind in a row. */
		g_assert_cmpuint(strlen(op->text), >, 0);
		if (i > 0)
			g_assert_cmpint(op->kind, !=,
				((BarAirewriteDiffOp *)
				 g_ptr_array_index(ops, i - 1))->kind);
	}
	g_assert_cmpstr(b->str, ==, before);
	g_assert_cmpstr(a->str, ==, after);
	g_string_free(b, TRUE);
	g_string_free(a, TRUE);
}

/* Catches: a regression in what the user sees -- the golden cases are
   the exact markup (insert, delete, replace, punctuation, white space,
   newlines, Unicode, emoji, empty sides) -- and either side of any
   diff failing to reassemble. */
static void
test_diff_golden(void)
{
	BarHarness h;
	Api api;
	g_autofree gchar *golden = fixture("diff-golden.tsv");
	g_auto(GStrv) lines = NULL;
	guint i, n = 0;

	if (!model_up(&h, &api))
		return;
	lines = g_strsplit(golden, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) f = NULL;
		g_autofree gchar *before = NULL;
		g_autofree gchar *after = NULL;
		g_autofree gchar *want = NULL;
		g_autofree gchar *got = NULL;
		g_autoptr(GPtrArray) ops = NULL;

		if (lines[i][0] == '#' || lines[i][0] == '\0')
			continue;
		f = g_strsplit(lines[i], "\t", 3);
		g_assert_cmpuint(g_strv_length(f), ==, 3);
		before = api.unescape(f[0]);
		after  = api.unescape(f[1]);
		want   = api.unescape(f[2]);
		ops = api.diff(before, after);
		got = api.diff_str(ops);
		if (g_strcmp0(got, want) != 0)
			g_test_message("case %u: %s -> %s", n, f[0], f[1]);
		g_assert_cmpstr(got, ==, want);
		assert_diff_sides(&api, before, after);
		n++;
	}
	g_assert_cmpuint(n, >=, 12);
	bar_harness_down(&h);
}

/* Catches: the DP table being built for a huge middle on the
   compositor thread (it must fall back to one replacement), and the
   sides still reassembling when it does. */
static void
test_diff_large_falls_back(void)
{
	BarHarness h;
	Api api;
	GString *a, *b;
	g_autoptr(GPtrArray) ops = NULL;
	guint i;
	gint64 t0;

	if (!model_up(&h, &api))
		return;
	a = g_string_new("HEAD ");
	b = g_string_new("HEAD ");
	for (i = 0; i < 3000; i++) {
		g_string_append_printf(a, "a%u ", i);
		g_string_append_printf(b, "b%u ", i);
	}
	g_string_append(a, "TAIL");
	g_string_append(b, "TAIL");

	t0 = g_get_monotonic_time();
	ops = api.diff(a->str, b->str);
	g_assert_cmpint(g_get_monotonic_time() - t0, <, G_USEC_PER_SEC);
	g_assert_cmpuint(ops->len, ==, 4);
	assert_diff_sides(&api, a->str, b->str);
	g_string_free(a, TRUE);
	g_string_free(b, TRUE);
	bar_harness_down(&h);
}

/* ── Paste keys and the paste machine ─────────────────────────────── */

/* Catches: Ctrl+V sent to a terminal (it types ^V), a chord parsed
   into the wrong modifiers, or an app id matched by substring (`foot'
   must not catch `football'). */
static void
test_paste_keys(void)
{
	BarHarness h;
	Api api;
	guint mods = 0;
	gchar *k = NULL;
	const gchar *terms = "gst, foot ,kitty,org.wezfurlong.wezterm";

	if (!model_up(&h, &api))
		return;

	g_assert_true(api.parse_keys("ctrl+v", &mods, &k));
	g_assert_cmpuint(mods, ==, BAR_AIREWRITE_MOD_CTRL);
	g_assert_cmpstr(k, ==, "v");
	g_clear_pointer(&k, g_free);
	g_assert_true(api.parse_keys("Ctrl+Shift+V", &mods, &k));
	g_assert_cmpuint(mods, ==,
	                 BAR_AIREWRITE_MOD_CTRL | BAR_AIREWRITE_MOD_SHIFT);
	g_assert_cmpstr(k, ==, "v");
	g_clear_pointer(&k, g_free);
	g_assert_true(api.parse_keys("shift+Insert", &mods, &k));
	g_assert_cmpstr(k, ==, "Insert");
	g_clear_pointer(&k, g_free);
	g_assert_true(api.parse_keys("super+alt+x", &mods, &k));
	g_assert_cmpuint(mods, ==,
	                 BAR_AIREWRITE_MOD_LOGO | BAR_AIREWRITE_MOD_ALT);
	g_clear_pointer(&k, g_free);
	g_assert_false(api.parse_keys("hyper+v", &mods, &k));
	g_assert_false(api.parse_keys("ctrl+", &mods, &k));
	g_assert_false(api.parse_keys("", &mods, &k));

	g_assert_cmpstr(api.keys_for("foot", "ctrl+v", "ctrl+shift+v", terms),
	                ==, "ctrl+shift+v");
	g_assert_cmpstr(api.keys_for("KITTY", "ctrl+v", "ctrl+shift+v", terms),
	                ==, "ctrl+shift+v");
	g_assert_cmpstr(api.keys_for("org.wezfurlong.wezterm", "ctrl+v",
	                             "ctrl+shift+v", terms),
	                ==, "ctrl+shift+v");
	g_assert_cmpstr(api.keys_for("football", "ctrl+v", "ctrl+shift+v",
	                             terms), ==, "ctrl+v");
	g_assert_cmpstr(api.keys_for("firefox", "ctrl+v", "ctrl+shift+v",
	                             terms), ==, "ctrl+v");
	g_assert_cmpstr(api.keys_for(NULL, "ctrl+v", "ctrl+shift+v", terms),
	                ==, "ctrl+v");
	/* An empty terminal chord means "same as everyone". */
	g_assert_cmpstr(api.keys_for("foot", "ctrl+v", "", terms),
	                ==, "ctrl+v");
	bar_harness_down(&h);
}

/* Catches: the old clipboard restored before the application read the
   new one (it would paste the OLD text), a restore with nothing saved
   (clearing the clipboard), a restore after keys that never went, or
   keys sent after a copy that failed. */
static void
test_paste_machine(void)
{
	BarHarness h;
	Api api;
	BarAirewritePaste p;
	const gint64 s = G_USEC_PER_SEC;

	if (!model_up(&h, &api))
		return;

	/* Full sequence: copy, keys, wait, restore. */
	memset(&p, 0, sizeof(p));
	p.inject = TRUE;
	p.restore = TRUE;
	p.restore_delay = s;
	p.phase = BAR_AIREWRITE_PASTE_COPYING;
	g_assert_cmpint(api.step(&p, 0), ==, BAR_AIREWRITE_PASTE_WAIT);
	g_assert_cmpint(api.wake(&p, 0), >, 0);
	p.phase = BAR_AIREWRITE_PASTE_COPIED;       /* the worker */
	p.copied = TRUE;
	p.have_previous = TRUE;
	g_assert_cmpint(api.step(&p, 10 * s), ==, BAR_AIREWRITE_PASTE_INJECT);
	g_assert_cmpint(p.phase, ==, BAR_AIREWRITE_PASTE_SETTLING);
	g_assert_cmpint(api.step(&p, 10 * s + s / 2), ==,
	                BAR_AIREWRITE_PASTE_WAIT);
	g_assert_cmpint(api.wake(&p, 10 * s + s / 2), ==, 500);
	g_assert_cmpint(api.step(&p, 11 * s), ==, BAR_AIREWRITE_PASTE_RESTORE);
	g_assert_cmpint(p.phase, ==, BAR_AIREWRITE_PASTE_RESTORING);
	g_assert_cmpint(api.step(&p, 12 * s), ==, BAR_AIREWRITE_PASTE_WAIT);
	p.phase = BAR_AIREWRITE_PASTE_IDLE;         /* the worker */
	g_assert_cmpint(api.wake(&p, 12 * s), ==, -1);

	/* Nothing was saved: keys, then done -- no restore. */
	memset(&p, 0, sizeof(p));
	p.inject = TRUE;
	p.restore = TRUE;
	p.phase = BAR_AIREWRITE_PASTE_COPIED;
	p.copied = TRUE;
	g_assert_cmpint(api.step(&p, 0), ==, BAR_AIREWRITE_PASTE_INJECT);
	g_assert_cmpint(api.step(&p, 5 * s), ==, BAR_AIREWRITE_PASTE_FINISH);

	/* Restore off: keys, then done. */
	memset(&p, 0, sizeof(p));
	p.inject = TRUE;
	p.have_previous = TRUE;
	p.phase = BAR_AIREWRITE_PASTE_COPIED;
	p.copied = TRUE;
	g_assert_cmpint(api.step(&p, 0), ==, BAR_AIREWRITE_PASTE_INJECT);
	g_assert_cmpint(api.step(&p, 0), ==, BAR_AIREWRITE_PASTE_FINISH);

	/* The keys failed: the caller turns restore off, and nothing is
	   put back over the result the user may still paste by hand. */
	memset(&p, 0, sizeof(p));
	p.inject = TRUE;
	p.restore = TRUE;
	p.have_previous = TRUE;
	p.phase = BAR_AIREWRITE_PASTE_COPIED;
	p.copied = TRUE;
	g_assert_cmpint(api.step(&p, 0), ==, BAR_AIREWRITE_PASTE_INJECT);
	p.restore = FALSE;
	g_assert_cmpint(api.step(&p, 9 * s), ==, BAR_AIREWRITE_PASTE_FINISH);

	/* The copy failed: no keys at all. */
	memset(&p, 0, sizeof(p));
	p.inject = TRUE;
	p.restore = TRUE;
	p.phase = BAR_AIREWRITE_PASTE_COPIED;
	g_assert_cmpint(api.step(&p, 0), ==, BAR_AIREWRITE_PASTE_FINISH);
	g_assert_cmpint(p.phase, ==, BAR_AIREWRITE_PASTE_IDLE);

	/* Copy only: no keys, no restore. */
	memset(&p, 0, sizeof(p));
	p.phase = BAR_AIREWRITE_PASTE_COPIED;
	p.copied = TRUE;
	p.have_previous = TRUE;
	p.restore = TRUE;
	g_assert_cmpint(api.step(&p, 0), ==, BAR_AIREWRITE_PASTE_FINISH);
	bar_harness_down(&h);
}

/* ── Capture ──────────────────────────────────────────────────────── */

/* Catches: the primary selection not being asked for first, or asked
   for in a way that appends a newline to the user's text. */
static void
test_capture_primary(void)
{
	Rig r;
	g_autofree gchar *args = NULL;

	if (!rig_up(&r))
		return;
	put(r.clip, "the clipboard");
	open_with(&r, "Teh selected text");
	g_assert_true(panel_says(&r, "Selected text"));
	g_assert_true(panel_says(&r, "Teh selected text"));
	g_assert_false(panel_says(&r, "the clipboard"));
	g_assert_true(panel_has(&r, "act:proofread"));

	args = slurp(r.paste_args);
	g_assert_true(g_str_has_prefix(args, "--primary\n--no-newline\n"));
	/* The clipboard was never needed. */
	g_assert_cmpuint(count(args, "--primary"), ==, 1);
	g_assert_cmpuint(count(args, "--\n"), ==, 1);
	rig_down(&r);
}

/* Catches: an empty or binary primary selection being rewritten
   instead of falling back, and the fallback not being SAID. */
static void
test_capture_clipboard_fallback(void)
{
	Rig r;

	if (!rig_up(&r))
		return;
	put(r.clip, "From the clip");
	open_with(&r, "   \n  ");       /* white space only */
	g_assert_true(panel_says(&r, "From the clipboard"));
	g_assert_true(panel_says(&r, "Nothing was selected"));
	g_assert_true(panel_says(&r, "From the clip"));

	/* Not UTF-8: refused, so the clipboard again. */
	open_with(&r, "bad \xff\xfe bytes");
	g_assert_true(panel_says(&r, "From the clipboard"));
	rig_down(&r);
}

/* Catches: nothing to rewrite leaving a dead panel, and typed text
   then being pasted over whatever happens to be selected. */
static void
test_capture_entry_fallback(void)
{
	Rig r;
	g_autofree gchar *copied = NULL;

	if (!rig_up(&r))
		return;
	open_with(&r, NULL);
	g_assert_true(panel_has(&r, "text"));
	g_assert_true(panel_says(&r, "Nothing selected"));
	g_assert_false(panel_has(&r, "act:proofread"));

	/* An empty submit does nothing. */
	gowl_bar_plugin_panel_text(r.h.plugin, "text", "  \n ", TRUE);
	g_assert_true(panel_has(&r, "text"));
	/* Every keystroke arrives unsubmitted and is ignored. */
	gowl_bar_plugin_panel_text(r.h.plugin, "text", "Teh", FALSE);
	g_assert_true(panel_has(&r, "text"));

	gowl_bar_plugin_panel_text(r.h.plugin, "text", "Teh typed text\n",
	                           TRUE);
	g_assert_true(panel_says(&r, "Typed text"));
	g_assert_true(panel_has(&r, "act:proofread"));

	/* Run, answer, then Enter: copy only, the panel stays open. */
	g_assert_true(key(&r, XKB_KEY_f, 0));
	cmacs_answers(&r, "done", "The typed text");
	g_assert_true(panel_has(&r, "diff"));
	g_assert_true(panel_says(&r, "Copy"));
	gowl_bar_plugin_panel_action(r.h.plugin, "result", 0, 0.0, 1);
	bar_harness_poll(&r.h);
	copied = slurp(r.copied);
	/* The trailing newline the typed text ended with is kept. */
	g_assert_cmpstr(copied, ==, "The typed text\n\n--END--\n");
	g_assert_cmpuint(r.h.host->closed, ==, 0);
	g_assert_true(panel_says(&r, "nothing to paste over"));
	rig_down(&r);
}

/* Catches: a missing wl-clipboard crashing or spinning instead of
   naming the package that provides it. */
static void
test_missing_wl_clipboard(void)
{
	Rig r;

	if (!rig_up(&r))
		return;
	gowl_bar_plugin_set_setting(r.h.plugin, "paste-command",
	                            "/nonexistent/wl-paste");
	open_with(&r, "anything");
	g_assert_true(panel_says(&r, "wl-clipboard"));
	g_assert_true(panel_has(&r, "text"));
	rig_down(&r);
}

/* Catches: a capture for a panel that was closed landing in the next
   one (the user would rewrite text they did not select this time). */
static void
test_late_capture_dropped(void)
{
	Rig r;

	if (!rig_up(&r))
		return;
	r.h.host->hold_work = TRUE;
	open_with(&r, "first selection");
	gowl_bar_plugin_panel_closed(r.h.plugin);
	gowl_bar_plugin_panel_opened(r.h.plugin);
	put(r.primary, "second selection");
	g_assert_true(panel_says(&r, "Reading the selection"));
	/* Both jobs run now, and both read "second selection": what
	   matters is that only the CURRENT generation's result counts. */
	put(r.primary, "stale selection");
	{
		BarHarnessWork *first = g_ptr_array_steal_index(
			r.h.host->held, 0);

		/* The stale job runs first and must be ignored. */
		first->func(first->plugin, first->user_data);
		if (first->destroy != NULL)
			first->destroy(first->user_data);
		g_object_unref(first->plugin);
		g_free(first);
	}
	g_assert_true(panel_says(&r, "Reading the selection"));
	put(r.primary, "second selection");
	r.h.host->hold_work = FALSE;
	bar_harness_run_held(&r.h);
	g_assert_true(panel_says(&r, "second selection"));
	g_assert_false(panel_says(&r, "stale selection"));
	rig_down(&r);
}

/* ── Actions in the panel ─────────────────────────────────────────── */

/* Catches: the YAML file not reaching the panel, a cmacs push not
   replacing the built-ins in replace mode, or a hostile pushed prompt
   arriving mangled at the model. */
static void
test_yaml_and_pushed_actions(void)
{
	Rig r;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *spec = NULL;
	g_autofree gchar *form = NULL;
	g_autoptr(GPtrArray) pushed = NULL;
	BarAirewriteAction *a;

	if (!rig_up(&r))
		return;
	yaml = g_build_filename(GOWL_TEST_FIXTURES, "ai-rewrite",
	                        "actions.yaml", NULL);
	gowl_bar_plugin_set_setting(r.h.plugin, "config", yaml);
	open_with(&r, "Hello there");
	g_assert_true(panel_says(&r, "Make it friendlier"));
	g_assert_true(panel_has(&r, "act:proofread"));
	g_assert_true(panel_has(&r, "act:summarize"));

	/* What cmacs pushes: its own list, replacing. */
	pushed = r.api.parse("", NULL);
	a = r.api.action_new('p', "reply", "Reply",
		"System with\ttab and \"quotes\"\nand a newline \\ slash",
		"Draft a reply.\n\nHow: briefly.\n\nThe text:");
	g_ptr_array_add(pushed, a);
	spec = r.api.serialize(pushed);
	gowl_bar_plugin_set_setting(r.h.plugin, "actions", spec);
	bar_harness_set(&r.h, "actions-mode", "replace");

	g_assert_true(panel_has(&r, "act:reply"));
	g_assert_false(panel_has(&r, "act:proofread"));
	g_assert_false(panel_says(&r, "Make it friendlier"));

	g_assert_true(key(&r, XKB_KEY_p, 0));
	form = slurp(r.form);
	/* The prompt reached cmacs whole: elisp-escaped by bar-ai, with
	   the instruction, a blank line, then the text. */
	g_assert_nonnull(strstr(form,
		"\"Draft a reply.\n\nHow: briefly.\n\nThe text:\n\nHello there\""));
	g_assert_nonnull(strstr(form,
		"\"System with\ttab and \\\"quotes\\\"\nand a newline \\\\ slash\""));

	/* An empty push in replace mode must not leave nothing. */
	gowl_bar_plugin_set_setting(r.h.plugin, "actions", "");
	bar_harness_set(&r.h, "actions-mode", "replace");
	open_with(&r, "Hello there");
	g_assert_true(panel_has(&r, "act:proofread"));
	rig_down(&r);
}

/* Catches: keys the panel should own falling through and closing it
   (`?', Backspace, an unbound letter), and keys it must NOT take (a
   Ctrl chord) being swallowed. */
static void
test_keys(void)
{
	Rig r;

	if (!rig_up(&r))
		return;
	open_with(&r, "Some text");
	g_assert_false(panel_says(&r, "Shift+letter"));
	g_assert_true(key(&r, XKB_KEY_question, MOD_SHIFT));
	g_assert_true(panel_says(&r, "Shift+letter"));
	g_assert_true(key(&r, XKB_KEY_question, MOD_SHIFT));
	g_assert_false(panel_says(&r, "Shift+letter"));

	/* Unbound: claimed, nothing happens. */
	g_assert_true(key(&r, XKB_KEY_q, 0));
	g_assert_true(panel_has(&r, "act:proofread"));
	g_assert_false(g_file_test(r.form, G_FILE_TEST_EXISTS));
	/* Backspace at the start: claimed, stays. */
	g_assert_true(key(&r, XKB_KEY_BackSpace, 0));
	g_assert_true(panel_has(&r, "act:proofread"));
	/* Not ours. */
	g_assert_false(key(&r, XKB_KEY_c, MOD_CTRL));
	g_assert_false(key(&r, XKB_KEY_F5, 0));

	/* Custom asks first; Backspace returns; Return in the field
	   runs it with the instruction in place. */
	g_assert_true(key(&r, XKB_KEY_c, 0));
	g_assert_true(panel_has(&r, "input"));
	g_assert_true(key(&r, XKB_KEY_BackSpace, 0));
	g_assert_false(panel_has(&r, "input"));
	g_assert_true(key(&r, XKB_KEY_c, 0));
	gowl_bar_plugin_panel_text(r.h.plugin, "input", "make it rhyme",
	                           TRUE);
	{
		g_autofree gchar *form = slurp(r.form);

		g_assert_nonnull(strstr(form, "Instruction: make it rhyme"
		                              "\n\nSome text"));
	}
	g_assert_true(panel_says(&r, "Waiting for the model"));
	/* Once cmacs has said which provider it defaults to, say so. */
	bar_harness_set(&r.h, "ai-default-provider", "claude-code");
	g_assert_true(panel_says(&r, "Waiting for claude-code"));
	/* Backspace while running cancels back to the list. */
	g_assert_true(key(&r, XKB_KEY_BackSpace, 0));
	g_assert_true(panel_has(&r, "act:proofread"));
	rig_down(&r);
}

/* Catches: the cmacs route's answer not reaching the panel, chaining
   applying to the ORIGINAL rather than the result, the back stack
   popping the wrong step, and Again not re-running on the step below. */
static void
test_cmacs_route_chaining_and_back(void)
{
	Rig r;
	g_autofree gchar *form1 = NULL;
	g_autofree gchar *form2 = NULL;
	g_autofree gchar *form3 = NULL;

	if (!rig_up(&r))
		return;
	open_with(&r, "Teh cat sat.");

	g_assert_true(key(&r, XKB_KEY_f, 0));
	form1 = slurp(r.form);
	g_assert_nonnull(strstr(form1, "changing nothing else.\n\nTeh cat sat."));
	cmacs_answers(&r, "streaming", "The ca");
	g_assert_true(panel_says(&r, "The ca"));
	cmacs_answers(&r, "done", "The cat sat.");
	g_assert_true(panel_has(&r, "diff"));
	g_assert_true(panel_says(&r, "Fix spelling"));

	/* Chain: Shorten runs on the RESULT. */
	g_assert_true(key(&r, XKB_KEY_s, 0));
	form2 = slurp(r.form);
	g_assert_nonnull(strstr(form2, "Shorten the following text.\n\nThe cat sat.\""));
	cmacs_answers(&r, "done", "Cat sat.");
	g_assert_true(panel_says(&r, "step 2"));

	/* Back: to the first result; back again: to the list. */
	g_assert_true(key(&r, XKB_KEY_BackSpace, 0));
	g_assert_true(panel_has(&r, "diff"));
	g_assert_false(panel_says(&r, "step 2"));

	/* Again: proofread re-runs on the source. */
	gowl_bar_plugin_panel_action(r.h.plugin, "result", 2, 0.0, 1);
	form3 = slurp(r.form);
	g_assert_nonnull(strstr(form3, "\n\nTeh cat sat.\""));
	cmacs_answers(&r, "done", "The cat sat.");
	g_assert_true(key(&r, XKB_KEY_BackSpace, 0));
	g_assert_true(panel_has(&r, "act:proofread"));
	g_assert_false(panel_has(&r, "diff"));

	/* An error shows, and Back leaves it. */
	g_assert_true(key(&r, XKB_KEY_i, 0));
	cmacs_answers(&r, "error", "");
	g_assert_true(panel_says(&r, "It did not work"));
	gowl_bar_plugin_panel_action(r.h.plugin, "error", 1, 0.0, 1);
	g_assert_true(panel_has(&r, "act:proofread"));
	rig_down(&r);
}

/* Catches: the CLI route not reached through the widget, the prompt
   format drifting, or the selected text appearing in the model's argv. */
static void
test_cli_route(void)
{
	Rig r;
	g_autofree gchar *body = NULL;
	g_autofree gchar *ai = NULL;
	g_autofree gchar *args_path = NULL;
	g_autofree gchar *args = NULL;
	g_autofree gchar *prompt = NULL;

	if (!rig_up(&r))
		return;
	args_path = bar_harness_path(&r.h, "ai-args");
	body = g_strdup_printf(
		"printf '%%s\\n' \"$@\" > '%s'\n"
		"cat > '%s'\n"
		"printf 'Teh secret, fixed.\\n'\n", args_path, r.prompt);
	ai = bar_harness_script(&r.h, "fake-ai", body);
	gowl_bar_plugin_set_setting(r.h.plugin, "ai-backend", "ai");
	gowl_bar_plugin_set_setting(r.h.plugin, "ai-command", ai);

	open_with(&r, "Teh secret text");
	g_assert_true(key(&r, XKB_KEY_e, 0));
	bar_harness_poll(&r.h);
	g_assert_true(panel_has(&r, "diff"));

	prompt = slurp(r.prompt);
	g_assert_cmpstr(prompt, ==,
		"Translate the following text into English.\n\nTeh secret text");
	args = slurp(args_path);
	g_assert_null(strstr(args, "secret"));
	g_assert_nonnull(strstr(args, "translator"));
	rig_down(&r);
}

/* Catches: no backend being discovered only when an action fails
   silently, instead of being said up front. */
static void
test_backend_none(void)
{
	Rig r;

	if (!rig_up(&r))
		return;
	gowl_bar_plugin_set_setting(r.h.plugin, "ai-backend", "none");
	open_with(&r, "text");
	g_assert_true(panel_says(&r, "No AI backend"));
	g_assert_true(key(&r, XKB_KEY_f, 0));
	g_assert_true(panel_says(&r, "It did not work"));
	g_assert_true(panel_says(&r, "ai-backend"));
	rig_down(&r);
}

/* ── Paste-back through the widget ────────────────────────────────── */

/* Catches: the result handed to wl-copy in argv (visible in `ps' to
   every user), the copy closing the panel, or `y' doing anything but
   copy. */
static void
test_copy_uses_stdin(void)
{
	Rig r;
	g_autofree gchar *copied = NULL;
	g_autofree gchar *args = NULL;

	if (!rig_up(&r))
		return;
	open_with(&r, "Teh secret");
	g_assert_true(key(&r, XKB_KEY_f, 0));
	cmacs_answers(&r, "done", "The secret; with \"quotes\" and\nnewline");
	g_assert_true(key(&r, XKB_KEY_y, 0));
	bar_harness_poll(&r.h);

	copied = slurp(r.copied);
	g_assert_cmpstr(copied, ==,
		"The secret; with \"quotes\" and\nnewline\n--END--\n");
	args = slurp(r.copy_args);
	g_assert_null(strstr(args, "secret"));
	g_assert_cmpstr(args, ==, "--\n");
	g_assert_cmpuint(r.h.host->closed, ==, 0);
	g_assert_true(panel_says(&r, "Copied to the clipboard"));
	rig_down(&r);
}

/* Catches: Paste over a selection not closing the panel first, not
   saving the old clipboard before overwriting it, or restoring it
   when the keys never went (here: there is no compositor, so they
   cannot) -- which would put the old text back over the result. */
static void
test_paste_without_compositor(void)
{
	Rig r;
	g_autofree gchar *copied = NULL;
	g_autofree gchar *pargs = NULL;

	if (!rig_up(&r))
		return;
	put(r.clip, "previous clipboard");
	open_with(&r, "Teh selection");
	g_assert_true(key(&r, XKB_KEY_f, 0));
	cmacs_answers(&r, "done", "The selection");

	/* Return on the focused diff is Paste. */
	gowl_bar_plugin_panel_action(r.h.plugin, "diff", -1, -1.0, 1);
	/* No window was recorded (no compositor), so this is copy only
	   and the panel stays; with a window it would close. */
	bar_harness_poll(&r.h);
	bar_harness_poll(&r.h);
	copied = slurp(r.copied);
	g_assert_cmpuint(count(copied, "--END--"), ==, 1);
	g_assert_true(g_str_has_prefix(copied, "The selection\n"));
	pargs = slurp(r.paste_args);
	/* No restore was planned, so the old clipboard was not read. */
	g_assert_cmpuint(count(pargs, "--primary"), ==, 1);
	rig_down(&r);
}

/* Catches: Enter alone doing nothing on a fresh result.  The host
   offers Return to the widget while nothing in the panel has the
   keyboard; the result view must take it as Paste.  Before the answer
   arrives it must NOT (there is nothing to paste yet). */
static void
test_return_alone_pastes(void)
{
	Rig r;
	g_autofree gchar *copied = NULL;

	if (!rig_up(&r))
		return;
	open_with(&r, "Teh text");
	g_assert_true(key(&r, XKB_KEY_f, 0));
	g_assert_false(key(&r, XKB_KEY_Return, 0));   /* still running */
	cmacs_answers(&r, "done", "The text");
	g_assert_true(key(&r, XKB_KEY_Return, 0));
	bar_harness_poll(&r.h);
	bar_harness_poll(&r.h);
	copied = slurp(r.copied);
	g_assert_true(g_str_has_prefix(copied, "The text\n"));
	/* Ctrl+Return is not a paste. */
	rig_down(&r);
}

/* Catches: Shift+letter and `preview: false' answers waiting for a
   keypress instead of going straight to the clipboard. */
static void
test_auto_paste(void)
{
	Rig r;
	g_autofree gchar *copied = NULL;
	g_autoptr(GPtrArray) pushed = NULL;
	g_autofree gchar *spec = NULL;
	BarAirewriteAction *a;

	if (!rig_up(&r))
		return;
	open_with(&r, "Teh one");
	g_assert_true(key(&r, XKB_KEY_F, MOD_SHIFT));
	cmacs_answers(&r, "done", "The one");
	bar_harness_poll(&r.h);
	copied = slurp(r.copied);
	g_assert_cmpstr(copied, ==, "The one\n--END--\n");

	/* An action that never previews. */
	pushed = r.api.parse("", NULL);
	a = r.api.action_new('n', "now", "Now", NULL, "Do it.");
	a->preview = FALSE;
	g_ptr_array_add(pushed, a);
	spec = r.api.serialize(pushed);
	bar_harness_set(&r.h, "actions", spec);
	open_with(&r, "Teh two");
	g_assert_true(key(&r, XKB_KEY_n, 0));
	cmacs_answers(&r, "done", "The two");
	bar_harness_poll(&r.h);
	g_free(copied);
	copied = slurp(r.copied);
	g_assert_cmpuint(count(copied, "--END--"), ==, 2);
	g_assert_nonnull(strstr(copied, "The two\n--END--\n"));
	rig_down(&r);
}

/* ── Colour ───────────────────────────────────────────────────────── */

static void
count_colours(cairo_surface_t *s, guint *red, guint *green)
{
	guchar *data;
	gint x, y, w, hh, stride;

	cairo_surface_flush(s);
	data = cairo_image_surface_get_data(s);
	w = cairo_image_surface_get_width(s);
	hh = cairo_image_surface_get_height(s);
	stride = cairo_image_surface_get_stride(s);
	*red = *green = 0;
	for (y = 0; y < hh; y++) {
		for (x = 0; x < w; x++) {
			guint32 px = *(guint32 *)(data + y * stride + x * 4);
			gdouble a = (gdouble)((px >> 24) & 0xff);
			gdouble rr, gg, bb;

			if (a < 160)
				continue;
			rr = ((px >> 16) & 0xff) / a;
			gg = ((px >> 8) & 0xff) / a;
			bb = (px & 0xff) / a;
			if (rr > 0.8 && gg < 0.25 && bb < 0.25)
				(*red)++;
			if (gg > 0.8 && rr < 0.25 && bb < 0.25)
				(*green)++;
		}
	}
}

/* Catches: the per-widget colour settings being ignored (a user's
   `color-deleted: "#ff0000"' must paint red), and the key colour
   setting not reaching the rows. */
static void
test_colour_settings(void)
{
	Rig r;
	cairo_surface_t *s;
	cairo_t *cr;
	guint red = 0, green = 0;
	g_autoptr(GowlBarPanel) p = NULL;
	GowlBarPanelItem *row;

	if (!rig_up(&r))
		return;
	gowl_bar_plugin_set_setting(r.h.plugin, "color-deleted", "#ff0000");
	gowl_bar_plugin_set_setting(r.h.plugin, "color-inserted", "#00ff00");
	gowl_bar_plugin_set_setting(r.h.plugin, "color-key", "mauve");
	open_with(&r, "WWWW MMMM");
	g_assert_true(key(&r, XKB_KEY_f, 0));
	cmacs_answers(&r, "done", "HHHH MMMM");

	s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 500, 120);
	cr = cairo_create(s);
	gowl_bar_plugin_panel_canvas(r.h.plugin, "diff", cr,
	                             r.h.host->theme, 500, 120);
	cairo_destroy(cr);
	count_colours(s, &red, &green);
	cairo_surface_destroy(s);
	g_assert_cmpuint(red, >, 10);
	g_assert_cmpuint(green, >, 10);

	p = panel(&r);
	row = gowl_bar_panel_find(p, "act:proofread");
	g_assert_nonnull(row);
	g_assert_cmpint(gowl_bar_panel_item_get_color(row), ==,
	                GOWL_BAR_COLOR_MAUVE);
	g_assert_cmpstr(gowl_bar_panel_item_get_icon(row), ==, "f");
	rig_down(&r);
}

/* Catches: the bar label not being themeable, and `show-in-bar: false'
   not hiding the widget (it is keybind-driven; most users want it
   out of the bar). */
static void
test_label_and_visibility(void)
{
	Rig r;
	g_autofree gchar *label = NULL;
	gdouble rgba[4];

	if (!rig_up(&r))
		return;
	bar_harness_poll(&r.h);
	label = gowl_bar_plugin_dup_label(r.h.plugin);
	g_assert_cmpstr(label, ==, "\xe2\x9c\x8e");
	g_assert_true(gowl_bar_plugin_get_visible(r.h.plugin));
	bar_harness_set(&r.h, "color", "#123456");
	bar_harness_poll(&r.h);
	g_assert_true(gowl_bar_plugin_get_color_rgba(r.h.plugin, rgba));
	bar_harness_set(&r.h, "show-in-bar", "false");
	bar_harness_poll(&r.h);
	g_assert_false(gowl_bar_plugin_get_visible(r.h.plugin));
	rig_down(&r);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-ai-rewrite/registered", test_registered);
	g_test_add_func("/bar-ai-rewrite/escape-round-trip",
	                test_escape_round_trip);
	g_test_add_func("/bar-ai-rewrite/actions-format-round-trip",
	                test_actions_format_round_trip);
	g_test_add_func("/bar-ai-rewrite/actions-parse-hostile",
	                test_actions_parse_hostile);
	g_test_add_func("/bar-ai-rewrite/parses-what-cmacs-pushes",
	                test_parses_what_cmacs_pushes);
	g_test_add_func("/bar-ai-rewrite/actions-from-yaml",
	                test_actions_from_yaml);
	g_test_add_func("/bar-ai-rewrite/actions-merge", test_actions_merge);
	g_test_add_func("/bar-ai-rewrite/compose-prompt", test_compose_prompt);
	g_test_add_func("/bar-ai-rewrite/clean-answer", test_clean_answer);
	g_test_add_func("/bar-ai-rewrite/tokenize", test_tokenize);
	g_test_add_func("/bar-ai-rewrite/diff-golden", test_diff_golden);
	g_test_add_func("/bar-ai-rewrite/diff-large", test_diff_large_falls_back);
	g_test_add_func("/bar-ai-rewrite/paste-keys", test_paste_keys);
	g_test_add_func("/bar-ai-rewrite/paste-machine", test_paste_machine);
	g_test_add_func("/bar-ai-rewrite/capture-primary", test_capture_primary);
	g_test_add_func("/bar-ai-rewrite/capture-clipboard-fallback",
	                test_capture_clipboard_fallback);
	g_test_add_func("/bar-ai-rewrite/capture-entry-fallback",
	                test_capture_entry_fallback);
	g_test_add_func("/bar-ai-rewrite/missing-wl-clipboard",
	                test_missing_wl_clipboard);
	g_test_add_func("/bar-ai-rewrite/late-capture-dropped",
	                test_late_capture_dropped);
	g_test_add_func("/bar-ai-rewrite/yaml-and-pushed-actions",
	                test_yaml_and_pushed_actions);
	g_test_add_func("/bar-ai-rewrite/keys", test_keys);
	g_test_add_func("/bar-ai-rewrite/cmacs-chaining-back",
	                test_cmacs_route_chaining_and_back);
	g_test_add_func("/bar-ai-rewrite/cli-route", test_cli_route);
	g_test_add_func("/bar-ai-rewrite/backend-none", test_backend_none);
	g_test_add_func("/bar-ai-rewrite/copy-uses-stdin", test_copy_uses_stdin);
	g_test_add_func("/bar-ai-rewrite/paste-without-compositor",
	                test_paste_without_compositor);
	g_test_add_func("/bar-ai-rewrite/auto-paste", test_auto_paste);
	g_test_add_func("/bar-ai-rewrite/colour-settings", test_colour_settings);
	g_test_add_func("/bar-ai-rewrite/label-and-visibility",
	                test_label_and_visibility);
	g_test_add_func("/bar-ai-rewrite/widget/return-alone-pastes",
	                test_return_alone_pastes);

	return g_test_run();
}
