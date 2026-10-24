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
 * test-hotkey-model.c - Reading a `hotkey:' section.
 *
 * The model (modules/hotkey/gowl-hotkey-model.c) compiled straight in:
 * every action kind and its options, the shorthand action on an entry,
 * window verbs becoming wire-protocol calls, a bare string being a
 * command, remaps in both forms, modes and the hotkeys inside them,
 * contexts and groups, hotstrings, timers, event hooks, functions,
 * include:, durations, ids, and a problem costing only the entry it is
 * in -- each with the line it is reported as.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include "gowl.h"
#include "../modules/hotkey/gowl-hotkey-model.c"

static HkSet *
load(const gchar *yaml)
{
	g_autoptr(HkLoader) loader = hk_loader_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(hk_loader_add_text(loader, yaml, "test", &error));
	g_assert_no_error(error);
	return hk_loader_finish(loader);
}

static HkSet *
load_clean(const gchar *yaml)
{
	HkSet *set = load(yaml);
	guint i;

	for (i = 0; i < set->problems->len; i++)
		g_test_message("problem: %s",
		               (const gchar *)g_ptr_array_index(set->problems, i));
	g_assert_cmpuint(set->problems->len, ==, 0);
	return set;
}

/* A problem mentioning every one of the NULL-terminated words. */
static void
assert_problem(
	HkSet *set,
	...
){
	va_list ap;
	guint i;
	gboolean found = FALSE;

	for (i = 0; i < set->problems->len && !found; i++) {
		const gchar *p = g_ptr_array_index(set->problems, i);
		const gchar *w;
		gboolean all = TRUE;

		va_start(ap, set);
		while ((w = va_arg(ap, const gchar *)) != NULL)
			if (strstr(p, w) == NULL)
				all = FALSE;
		va_end(ap);
		found = all;
	}
	if (!found)
		for (i = 0; i < set->problems->len; i++)
			g_test_message("problem: %s",
			               (const gchar *)g_ptr_array_index(set->problems, i));
	g_assert_true(found);
}

static HkAction *
action_of(
	HkSet       *set,
	const gchar *id,
	guint        n
){
	HkHotkey *hk = hk_set_find_hotkey(set, id);

	g_assert_nonnull(hk);
	g_assert_cmpuint(hk->actions->len, >, n);
	return g_ptr_array_index(hk->actions, n);
}

static void
test_action_kinds(void)
{
	g_autoptr(HkSet) set = load_clean(
		"hotkeys:\n"
		"  - id: all\n"
		"    key: \"#a\"\n"
		"    do:\n"
		"      - {action: tag_view, arg: \"4\"}\n"
		"      - {command: \"view 2\"}\n"
		"      - {rpc: window.center, params: {width: 800}}\n"
		"      - {macro: tidy, args: [a, b], wait: true}\n"
		"      - {shell: \"notify-send hi\", wait: true, timeout: 5s}\n"
		"      - {run: [firefox, --new-window]}\n"
		"      - {run: \"foot -e htop\"}\n"
		"      - {c: \"gowl_hotkey_send(comp, \\\"x\\\");\"}\n"
		"      - {c: {file: ~/x.c, cflags: -O2}}\n"
		"      - {call: fn, args: [\"{{title}}\"]}\n"
		"      - {send: \"^c\", send-mode: event, key-delay: 20, level: 3}\n"
		"      - {send-text: \"hello\"}\n"
		"      - {mode: resize}\n"
		"      - {suspend: toggle}\n"
		"      - {reload: true}\n"
		"      - {mouse: click, button: right, count: 2, x: 10, y: 20}\n"
		"      - {clipboard: \"{{title}}\"}\n"
		"      - {notify: done}\n"
		"      - {sleep: 1.5s}\n"
		"      - {custom: \"(message \\\"hi\\\")\"}\n"
		"      - {input: tag, length: 2, timeout: 3000, end-keys: [Enter], match: [\"12\"]}\n"
		"      - {context: gaming, value: true}\n"
		"      - {capslock: always-off}\n"
		"      - {hotkey: disable, id: all}\n"
		"      - \"view 1\"\n"
		"modes: {resize: {}}\n");
	HkAction *a;
	guint i = 0;

	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_ACTION);
	g_assert_cmpstr(a->name, ==, "tag-view");     /* _ becomes - */
	g_assert_cmpstr(a->text, ==, "4");
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_COMMAND);
	g_assert_cmpstr(a->text, ==, "view 2");
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_RPC);
	g_assert_cmpstr(a->name, ==, "window.center");
	g_assert_cmpint(json_object_get_int_member(a->params, "width"), ==, 800);
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_MACRO);
	g_assert_cmpstr(a->name, ==, "tidy");
	g_assert_cmpuint(g_strv_length(a->argv), ==, 2);
	g_assert_true(a->wait);
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_SHELL);
	g_assert_true(a->wait);
	g_assert_cmpuint(a->timeout_ms, ==, 5000);
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_RUN);
	g_assert_cmpstr(a->argv[1], ==, "--new-window");
	a = action_of(set, "all", i++);
	g_assert_cmpstr(a->argv[2], ==, "htop");      /* split like a shell */
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_C);
	g_assert_false(a->is_file);
	a = action_of(set, "all", i++);
	g_assert_true(a->is_file);
	g_assert_cmpstr(a->cflags, ==, "-O2");
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_CALL);
	g_assert_cmpstr(a->argv[0], ==, "{{title}}");
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_SEND);
	g_assert_cmpint(a->send_mode, ==, 1);
	g_assert_cmpuint(a->ms, ==, 20);
	g_assert_cmpint(a->level, ==, 3);
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_SEND_TEXT);
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_MODE);
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_SUSPEND);
	g_assert_cmpint(a->toggle, ==, HK_TOGGLE_TOGGLE);
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_RELOAD);
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_MOUSE);
	g_assert_cmpuint(a->button, ==, BTN_RIGHT);
	g_assert_cmpint(a->count, ==, 2);
	g_assert_true(a->has_xy);
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_CLIPBOARD);
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_NOTIFY);
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_SLEEP);
	g_assert_cmpuint(a->ms, ==, 1500);
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_CUSTOM);
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_INPUT);
	g_assert_cmpstr(a->name, ==, "tag");
	g_assert_cmpstr(a->text, ==, "L2 T3000");
	g_assert_cmpstr(a->argv[0], ==, "Enter");
	g_assert_cmpstr(a->argv[1], ==, "--");
	g_assert_cmpstr(a->argv[2], ==, "12");
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_CONTEXT);
	g_assert_cmpint(a->toggle, ==, HK_TOGGLE_ON);
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_LOCK);
	g_assert_cmpstr(a->name, ==, "capslock");
	g_assert_cmpint(a->toggle, ==, HK_TOGGLE_ALWAYS_OFF);
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_HOTKEY);
	g_assert_cmpint(a->toggle, ==, HK_TOGGLE_OFF);
	a = action_of(set, "all", i++);
	g_assert_cmpint(a->kind, ==, HK_ACT_COMMAND);   /* a bare string */
	g_assert_cmpstr(a->text, ==, "view 1");
}

static void
test_window_verbs(void)
{
	g_autoptr(HkSet) set = load_clean(
		"hotkeys:\n"
		"  - {id: f, key: a, do: {window: float}}\n"
		"  - {id: f-on, key: b, do: {window: fullscreen, arg: on}}\n"
		"  - {id: min, key: c, do: {window: minimize, target: \"app-id=x\"}}\n"
		"  - {id: mv, key: d, do: {window: move, arg: \"100 200\"}}\n"
		"  - {id: rs, key: e, do: {window: resize, arg: 800x600}}\n"
		"  - {id: op, key: f, do: {window: opacity, arg: \"0.8\"}}\n"
		"  - {id: tg, key: g, do: {window: tag, arg: \"3\"}}\n"
		"  - {id: grp, key: h, do: {window: group-activate, arg: terms}}\n");
	HkAction *a;

	a = action_of(set, "f", 0);
	g_assert_cmpstr(json_object_get_string_member(a->params, "x-method"), ==,
	                "window.set_floating");
	g_assert_cmpstr(json_object_get_string_member(a->params, "value"), ==,
	                "toggle");
	a = action_of(set, "f-on", 0);
	g_assert_true(json_object_get_boolean_member(a->params, "value"));
	a = action_of(set, "min", 0);
	g_assert_cmpstr(json_object_get_string_member(a->params, "x-method"), ==,
	                "window.set_minimized");
	g_assert_true(json_object_get_boolean_member(a->params, "value"));
	g_assert_cmpstr(a->target, ==, "app-id=x");
	a = action_of(set, "mv", 0);
	g_assert_cmpint(json_object_get_int_member(a->params, "x"), ==, 100);
	g_assert_cmpint(json_object_get_int_member(a->params, "y"), ==, 200);
	a = action_of(set, "rs", 0);
	g_assert_cmpint(json_object_get_int_member(a->params, "width"), ==, 800);
	g_assert_cmpint(json_object_get_int_member(a->params, "height"), ==, 600);
	a = action_of(set, "op", 0);
	g_assert_cmpfloat(json_object_get_double_member(a->params, "opacity"), ==,
	                  0.8);
	a = action_of(set, "tg", 0);
	g_assert_cmpint(json_object_get_int_member(a->params, "tag"), ==, 3);
	a = action_of(set, "grp", 0);
	g_assert_cmpstr(a->name, ==, "group-activate");
	g_assert_cmpstr(a->text, ==, "terms");
}

static void
test_entry_forms(void)
{
	g_autoptr(HkSet) set = load_clean(
		"hotkeys:\n"
		"  - {key: \"#t\", action: spawn, arg: gst}\n"         /* shorthand */
		"  - {key: \"#b\", send: \"{Enter}\", context: x, mode: nav}\n"
		"modes: {nav: {}}\n"
		"contexts: {x: \"app-id=x\"}\n");
	g_autoptr(HkSet) compact = load_clean(
		"hotkeys:\n"
		"  \"#!t\": {action: spawn, arg: gst}\n"
		"  \"^j\": [{send: \"{Down}\"}, {send: \"{Down}\"}]\n");
	HkHotkey *hk;

	hk = hk_set_find_hotkey(set, "#t");
	g_assert_nonnull(hk);
	g_assert_cmpint(((HkAction *)g_ptr_array_index(hk->actions, 0))->kind, ==,
	                HK_ACT_ACTION);
	/* the shorthand's mode: is the hotkey's, not an action */
	hk = hk_set_find_hotkey(set, "#b@x#nav");
	g_assert_nonnull(hk);
	g_assert_cmpstr(hk->mode, ==, "nav");
	g_assert_cmpstr(hk->context, ==, "x");
	g_assert_cmpuint(hk->actions->len, ==, 1);
	/* {KEY: ACTIONS} */
	g_assert_cmpuint(compact->hotkeys->len, ==, 2);
	hk = hk_set_find_hotkey(compact, "^j");
	g_assert_cmpuint(hk->actions->len, ==, 2);
}

static void
test_options(void)
{
	g_autoptr(HkSet) set = load_clean(
		"hotkeys:\n"
		"  - {id: d, key: Esc, double: true, do: {notify: x}}\n"
		"  - {id: d2, key: a, double: 250ms, do: {notify: x}}\n"
		"  - {id: h, key: b, hold: true, priority: 3, level: 2, "
		"suspend-exempt: true, max-running: 0, enabled: false, "
		"description: \"a long press\", do: {notify: x}}\n"
		"  - {id: w, key: c, when: \"app-id=foot\", over: \"title=*x*\", "
		"exists: \"app-id=steam\", absent: \"app-id=zoom\", do: {notify: x}}\n");
	HkHotkey *hk;

	hk = hk_set_find_hotkey(set, "d");
	g_assert_cmpuint(hk->double_ms, ==, HK_DEFAULT_DOUBLE_MS);
	hk = hk_set_find_hotkey(set, "d2");
	g_assert_cmpuint(hk->double_ms, ==, 250);
	hk = hk_set_find_hotkey(set, "h");
	g_assert_cmpuint(hk->hold_ms, ==, HK_DEFAULT_HOLD_MS);
	g_assert_cmpint(hk->priority, ==, 3);
	g_assert_cmpint(hk->level, ==, 2);
	g_assert_true(hk->suspend_exempt);
	g_assert_cmpuint(hk->max_running, ==, 0);
	g_assert_false(hk->enabled);
	g_assert_cmpstr(hk->description, ==, "a long press");
	hk = hk_set_find_hotkey(set, "w");
	g_assert_nonnull(hk->cond.when);
	g_assert_nonnull(hk->cond.over);
	g_assert_nonnull(hk->cond.exists);
	g_assert_nonnull(hk->cond.absent);
}

static void
test_problems(void)
{
	g_autoptr(HkSet) set = load(
		"hotkeys:\n"
		"  - {key: \"Frob\", do: {notify: x}}\n"
		"  - {key: a, do: {notify: x}, colour: red}\n"
		"  - {key: b, do: {frobnicate: x}}\n"
		"  - {key: c, do: {send: x, shell: y}}\n"
		"  - {key: d, when: \"app-id ==\", do: {notify: x}}\n"
		"  - {key: e, double: 200, hold: 300, do: {notify: x}}\n"
		"  - {key: f, do: {window: wiggle}}\n"
		"  - {key: g}\n"
		"  - {do: {notify: x}}\n"
		"  - {key: \"WheelUp\", hold: true, do: {notify: x}}\n"
		"  - {id: ok, key: z, do: {notify: fine}}\n"
		"  - {key: h, do: {mode: nowhere}}\n"
		"hotstrings: [\"not one\"]\n"
		"timers: [{do: {notify: x}}]\n"
		"on: {sneeze: [{notify: x}]}\n"
		"frobs: 1\n");

	assert_problem(set, "Frob", "unknown key", NULL);
	assert_problem(set, "unknown key `colour'", NULL);
	assert_problem(set, "an action needs one of", NULL);
	assert_problem(set, "exactly one kind key", NULL);
	assert_problem(set, "when", "app-id ==", NULL);
	assert_problem(set, "not both", NULL);
	assert_problem(set, "unknown window verb `wiggle'", NULL);
	assert_problem(set, "nothing to do", NULL);
	assert_problem(set, "no key:", NULL);
	assert_problem(set, "cannot be held", NULL);
	assert_problem(set, "enters mode `nowhere'", NULL);
	assert_problem(set, "not :OPTIONS:ABBREVIATION::REPLACEMENT", NULL);
	assert_problem(set, "every: or once:", NULL);
	assert_problem(set, "unknown event `sneeze'", NULL);
	assert_problem(set, "unknown key `frobs'", NULL);
	/* a problem costs its own entry only; `a' still loads (its unknown
	 * key is reported, the rest of it is fine) */
	g_assert_nonnull(hk_set_find_hotkey(set, "ok"));
	g_assert_nonnull(hk_set_find_hotkey(set, "a"));
	g_assert_null(hk_set_find_hotkey(set, "Frob"));
	g_assert_null(hk_set_find_hotkey(set, "e"));
}

static void
test_remaps(void)
{
	g_autoptr(HkSet) map = load_clean("remaps: {CapsLock: Control_L, "
	                                  "XButton1: \"^c\"}\n");
	g_autoptr(HkSet) seq = load_clean(
		"remaps:\n"
		"  - {from: F13, to: Esc, context: x, id: pedal}\n"
		"contexts: {x: \"app-id=y\"}\n");
	g_autoptr(HkSet) bad = load("remaps: {\"a & b\": c, d: \"e & f\", "
	                            "LShift: a}\n");
	HkHotkey *hk;

	hk = hk_set_find_hotkey(map, "remap:CapsLock");
	g_assert_nonnull(hk);
	g_assert_true(hk->is_remap);
	g_assert_true(hk->spec.flags & GOWL_HOTKEY_FLAG_WILDCARD);
	g_assert_cmpuint(hk->remap_to.keysym, ==, XKB_KEY_Control_L);
	hk = hk_set_find_hotkey(map, "remap:XButton1");
	g_assert_cmpuint(hk->remap_mods, ==, GOWL_KEY_MOD_CTRL);
	g_assert_cmpuint(hk->remap_to.keysym, ==, XKB_KEY_c);
	hk = hk_set_find_hotkey(seq, "pedal");
	g_assert_nonnull(hk);
	g_assert_cmpstr(hk->context, ==, "x");
	assert_problem(bad, "no combinations", NULL);
	assert_problem(bad, "target is one key", NULL);
	/* a modifier source loads, with a warning that it still modifies */
	assert_problem(bad, "still acts as that modifier", NULL);
	g_assert_nonnull(hk_set_find_hotkey(bad, "remap:LShift"));
}

static void
test_modes_contexts_groups(void)
{
	g_autoptr(HkSet) set = load_clean(
		"contexts:\n"
		"  browser: \"app-id=firefox* or app-id=librewolf*\"\n"
		"  video: {when: \"title=*YouTube*\", exists: \"app-id=mpv\"}\n"
		"groups:\n"
		"  terms: \"app-id=gst or app-id=foot\"\n"
		"modes:\n"
		"  resize:\n"
		"    timeout: 3s\n"
		"    oneshot: true\n"
		"    swallow: true\n"
		"    toast: false\n"
		"    label: Resize\n"
		"    on-enter: {notify: in}\n"
		"    on-exit: [{notify: out}]\n"
		"    hotkeys: [{key: h, do: {notify: x}}]\n"
		"    remaps: {j: Down}\n");
	HkNamedCondition *nc;
	HkMode *md;
	HkHotkey *hk;

	nc = g_hash_table_lookup(set->contexts, "browser");
	g_assert_nonnull(nc->cond.when);
	nc = g_hash_table_lookup(set->contexts, "video");
	g_assert_nonnull(nc->cond.when);
	g_assert_nonnull(nc->cond.exists);
	g_assert_nonnull(g_hash_table_lookup(set->groups, "terms"));
	md = g_hash_table_lookup(set->modes, "resize");
	g_assert_cmpuint(md->timeout_ms, ==, 3000);
	g_assert_true(md->oneshot);
	g_assert_true(md->swallow);
	g_assert_false(md->toast);
	g_assert_cmpstr(md->label, ==, "Resize");
	g_assert_cmpuint(md->on_enter->len, ==, 1);
	g_assert_cmpuint(md->on_exit->len, ==, 1);
	hk = hk_set_find_hotkey(set, "h#resize");
	g_assert_cmpstr(hk->mode, ==, "resize");
	hk = hk_set_find_hotkey(set, "remap:j");
	g_assert_cmpstr(hk->mode, ==, "resize");
}

static void
test_hotstrings_timers_hooks_functions(void)
{
	g_autoptr(HkSet) set = load_clean(
		"hotstrings:\n"
		"  - \":*:btw::by the way\"\n"
		"  - \"::sig::Regards{Enter}Zach\"\n"
		"  - {hotstring: \":X:;up::\", do: {shell: uptime}, id: up}\n"
		"  - {trigger: brb, replace: be right back, options: \"*\"}\n"
		"timers:\n"
		"  - {id: water, every: 30m, do: {notify: drink}}\n"
		"  - {once: 5s, delay: 1s, do: {notify: hi}}\n"
		"on:\n"
		"  window-open:\n"
		"    - {filter: \"app-id=steam\", do: {notify: steam}}\n"
		"    - {do: {notify: any}}\n"
		"  focus: [{notify: focus}]\n"
		"  startup: {notify: started}\n"
		"functions:\n"
		"  tidy: [{command: \"view 1\"}, {notify: tidy}]\n");
	HkHotstring *hs;
	HkTimer *t;
	HkHook *h;
	HkFunction *f;

	g_assert_cmpuint(set->hotstrings->len, ==, 4);
	hs = g_ptr_array_index(set->hotstrings, 0);
	g_assert_cmpstr(hs->options, ==, "*");
	g_assert_cmpstr(hs->abbrev, ==, "btw");
	g_assert_cmpstr(hs->replacement, ==, "by the way");
	g_assert_cmpstr(hs->id, ==, "hotstring:btw");
	hs = g_ptr_array_index(set->hotstrings, 1);
	g_assert_cmpstr(hs->options, ==, "");
	g_assert_cmpstr(hs->replacement, ==, "Regards{Enter}Zach");
	hs = g_ptr_array_index(set->hotstrings, 2);
	g_assert_cmpstr(hs->id, ==, "up");
	g_assert_cmpuint(hs->actions->len, ==, 1);
	hs = g_ptr_array_index(set->hotstrings, 3);
	g_assert_cmpstr(hs->raw, ==, ":*:brb::be right back");

	g_assert_cmpuint(set->timers->len, ==, 2);
	t = g_ptr_array_index(set->timers, 0);
	g_assert_cmpstr(t->id, ==, "water");
	g_assert_cmpuint(t->period_ms, ==, 30 * 60 * 1000);
	g_assert_cmpuint(t->delay_ms, ==, t->period_ms);
	t = g_ptr_array_index(set->timers, 1);
	g_assert_true(t->once);
	g_assert_cmpuint(t->delay_ms, ==, 1000);

	g_assert_cmpuint(set->hooks->len, ==, 4);
	h = g_ptr_array_index(set->hooks, 0);
	g_assert_cmpstr(h->event, ==, "window-open");
	g_assert_nonnull(h->cond.when);
	h = g_ptr_array_index(set->hooks, 2);
	g_assert_cmpstr(h->event, ==, "focus");
	g_assert_null(h->cond.when);

	f = g_hash_table_lookup(set->functions, "tidy");
	g_assert_cmpuint(f->actions->len, ==, 2);
}

static void
test_include_and_scripts(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("gowl-hotkey-XXXXXX", NULL);
	g_autofree gchar *inc = g_build_filename(dir, "more.yaml", NULL);
	g_autofree gchar *loop = g_build_filename(dir, "loop.yaml", NULL);
	g_autofree gchar *yaml = NULL;
	g_autoptr(HkLoader) loader = hk_loader_new();
	g_autoptr(YamlParser) parser = yaml_parser_new();
	g_autoptr(HkSet) set = NULL;
	g_autoptr(GError) error = NULL;

	/* an included file may be a section, or a whole config */
	g_assert_true(g_file_set_contents(inc,
		"hotkey:\n  hotkeys: [{id: inc, key: a, do: {notify: x}}]\n"
		"  include: [loop.yaml]\n", -1, NULL));
	g_assert_true(g_file_set_contents(loop,
		"hotkeys: [{id: looped, key: b, do: {notify: x}}]\n"
		"include: [more.yaml]\n", -1, NULL));
	yaml = g_strdup_printf("include: [more.yaml]\nscripts: [hk.c, ~/hk2.c, "
	                       "/abs/hk3.c]\n");
	g_assert_true(yaml_parser_load_from_data(parser, yaml, -1, &error));
	hk_loader_add_node(loader, yaml_parser_get_root(parser), "config", dir);
	set = hk_loader_finish(loader);
	g_assert_nonnull(hk_set_find_hotkey(set, "inc"));
	g_assert_nonnull(hk_set_find_hotkey(set, "looped"));
	assert_problem(set, "included twice", NULL);
	g_assert_cmpuint(set->scripts->len, ==, 3);
	g_assert_true(g_str_has_prefix(g_ptr_array_index(set->scripts, 0), dir));
	g_assert_true(g_str_has_prefix(g_ptr_array_index(set->scripts, 1),
	                               g_get_home_dir()));
	g_assert_cmpstr(g_ptr_array_index(set->scripts, 2), ==, "/abs/hk3.c");
	g_unlink(inc);
	g_unlink(loop);
	g_rmdir(dir);
}

static void
test_json(void)
{
	g_autoptr(HkSet) set = load_clean(
		"hotkeys: [{id: x, key: \"!#t\", mode: \"*\", "
		"description: hi, do: [{notify: a}, {send: b}]}]\n"
		"remaps: {CapsLock: Control_L}\n");
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autoptr(JsonGenerator) gen = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *text = NULL;
	guint i;

	json_builder_begin_array(b);
	for (i = 0; i < set->hotkeys->len; i++)
		hk_hotkey_to_json(g_ptr_array_index(set->hotkeys, i), b);
	json_builder_end_array(b);
	root = json_builder_get_root(b);
	json_generator_set_root(gen, root);
	text = json_generator_to_data(gen, NULL);
	g_assert_nonnull(strstr(text, "\"canonical\":\"#!t\""));
	g_assert_nonnull(strstr(text, "\"kind\":\"press\""));
	g_assert_nonnull(strstr(text, "\"mode\":\"*\""));
	g_assert_nonnull(strstr(text, "\"actions\":[\"notify\",\"send\"]"));
	g_assert_nonnull(strstr(text, "\"kind\":\"remap\""));
	g_assert_nonnull(strstr(text, "\"to\":\"LCtrl\""));
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/hotkey/model/action-kinds", test_action_kinds);
	g_test_add_func("/hotkey/model/window-verbs", test_window_verbs);
	g_test_add_func("/hotkey/model/entry-forms", test_entry_forms);
	g_test_add_func("/hotkey/model/options", test_options);
	g_test_add_func("/hotkey/model/problems", test_problems);
	g_test_add_func("/hotkey/model/remaps", test_remaps);
	g_test_add_func("/hotkey/model/modes-contexts-groups",
	                test_modes_contexts_groups);
	g_test_add_func("/hotkey/model/hotstrings-timers-hooks-functions",
	                test_hotstrings_timers_hooks_functions);
	g_test_add_func("/hotkey/model/include-and-scripts",
	                test_include_and_scripts);
	g_test_add_func("/hotkey/model/json", test_json);

	return g_test_run();
}
