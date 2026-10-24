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
 * test-hotkey-engine.c - The hotkey engine against a fake clock.
 *
 * The engine (modules/hotkey/gowl-hotkey-engine.c) and the model it
 * reads, compiled straight in.  Every scenario is a script of input
 * events with the trace of what the engine decided -- swallowed or
 * not, and what followed: F(id) a hotkey fired, K(code+/-) a key put
 * back, R(sym+/-) a remap target, B(code+/-) a button put back.
 *
 * Covers suppression and `~', a swallowed press swallowing its release,
 * `$' and injected input, wildcards, left and right hands, UP hotkeys,
 * combinations and prefix keys (with and without their own hotkey, and
 * `~'), lone modifiers, double taps (in time, out of time, with a single
 * on the same key, interrupted), long presses (held, tapped, tapped with
 * a tap hotkey, interrupted), modes, suspend and its exemption, input
 * levels, remaps (and their release surviving a reload), priorities and
 * contexts, swallowing modes, buttons and the wheel, the key history,
 * the last hotkey, key state, and a reset.
 */

#include <glib.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include "gowl.h"
#include "../modules/hotkey/gowl-hotkey-model.c"
#include "../modules/hotkey/gowl-hotkey-engine.c"

/* ── The rig ───────────────────────────────────────────────────────── */

typedef struct {
	HkSet      *set;
	HkEngine   *e;
	GArray     *out;
	gint64      now;
	gchar      *mode;
	GHashTable *contexts;   /* names that hold */
	GHashTable *refuse;     /* hotkey ids the caller refuses (max-running) */
} Rig;

static gboolean
rig_allow(
	HkHotkey *hk,
	gpointer  data
){
	Rig *r = data;

	if (g_hash_table_contains(r->refuse, hk->id))
		return FALSE;
	if (hk->context != NULL)
		return g_hash_table_contains(r->contexts, hk->context);
	return TRUE;
}

static void
rig_init(Rig *r)
{
	memset(r, 0, sizeof *r);
	r->e = hk_engine_new();
	r->out = hk_out_new();
	r->now = 1000;
	r->contexts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	r->refuse = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	hk_engine_set_allow_func(r->e, rig_allow, r);
}

static void
rig_clear(Rig *r)
{
	hk_out_clear(r->out);
	g_array_unref(r->out);
	hk_engine_free(r->e);
	if (r->set != NULL)
		hk_set_unref(r->set);
	g_free(r->mode);
	g_hash_table_unref(r->contexts);
	g_hash_table_unref(r->refuse);
}

/* Loads a section; every problem fails the test. */
static void
load(
	Rig         *r,
	const gchar *yaml
){
	g_autoptr(HkLoader) loader = hk_loader_new();
	g_autoptr(GError) error = NULL;
	guint i;

	g_assert_true(hk_loader_add_text(loader, yaml, "test", &error));
	g_assert_no_error(error);
	if (r->set != NULL)
		hk_set_unref(r->set);
	r->set = hk_loader_finish(loader);
	for (i = 0; i < r->set->problems->len; i++)
		g_test_message("problem: %s",
		               (const gchar *)g_ptr_array_index(r->set->problems, i));
	g_assert_cmpuint(r->set->problems->len, ==, 0);
	hk_engine_set_hotkeys(r->e, r->set->hotkeys);
}

/* The trace of what followed since the last call, then forgotten. */
static gchar *
trace(Rig *r)
{
	GString *t = g_string_new(NULL);
	guint i;

	for (i = 0; i < r->out->len; i++) {
		HkOut *o = &g_array_index(r->out, HkOut, i);
		gchar name[64];

		if (t->len > 0)
			g_string_append_c(t, ' ');
		switch (o->kind) {
		case HK_OUT_FIRE:
			g_string_append_printf(t, "F(%s)", o->hotkey->id);
			break;
		case HK_OUT_KEY:
			if (g_strcmp0(o->why, "remap") == 0) {
				xkb_keysym_get_name(o->keysym, name, sizeof name);
				g_string_append_printf(t, "R(%s%s%s)",
				                       o->mods != 0 ? "mods:" : "", name,
				                       o->pressed ? "+" : "-");
			} else {
				g_string_append_printf(t, "K(%u%s)", o->keycode,
				                       o->pressed ? "+" : "-");
			}
			break;
		case HK_OUT_BUTTON:
			g_string_append_printf(t, "B(%u%s)", o->button,
			                       o->pressed ? "+" : "-");
			break;
		}
	}
	hk_out_clear(r->out);
	return g_string_free(t, FALSE);
}

#define ASSERT_TRACE(r, want) do { \
	g_autofree gchar *got_ = trace(r); \
	if (g_strcmp0(got_, (want)) != 0) \
		g_test_message("trace: `%s', want `%s'", got_, (want)); \
	g_assert_cmpstr(got_, ==, (want)); \
} while (0)

/* An input event on the real keyboard (or ours, with @own >= 0). */
static gboolean
key_full(
	Rig      *r,
	guint32   code,
	guint32   sym,
	guint     mods,
	gboolean  pressed,
	gboolean  synthetic,
	gint      own
){
	HkEvent ev;

	memset(&ev, 0, sizeof ev);
	ev.kind = GOWL_HOTKEY_INPUT_KEY;
	ev.keycode = code;
	ev.keysym = sym;
	ev.base_keysym = sym;
	ev.mods = mods;
	ev.pressed = pressed;
	ev.synthetic = synthetic;
	ev.own_level = own;
	return hk_engine_input(r->e, &ev, r->mode, r->now, r->out);
}

static gboolean
down(Rig *r, guint32 code, guint32 sym, guint mods)
{
	return key_full(r, code, sym, mods, TRUE, FALSE, -1);
}

static gboolean
up(Rig *r, guint32 code, guint32 sym, guint mods)
{
	return key_full(r, code, sym, mods, FALSE, FALSE, -1);
}

static gboolean
button(Rig *r, guint32 code, gboolean pressed, guint mods)
{
	HkEvent ev;

	memset(&ev, 0, sizeof ev);
	ev.kind = GOWL_HOTKEY_INPUT_BUTTON;
	ev.button = code;
	ev.mods = mods;
	ev.pressed = pressed;
	ev.own_level = -1;
	return hk_engine_input(r->e, &ev, r->mode, r->now, r->out);
}

static gboolean
wheel(Rig *r, guint32 dir, guint mods)
{
	HkEvent ev;

	memset(&ev, 0, sizeof ev);
	ev.kind = GOWL_HOTKEY_INPUT_WHEEL;
	ev.button = dir;
	ev.mods = mods;
	ev.pressed = TRUE;
	ev.own_level = -1;
	return hk_engine_input(r->e, &ev, r->mode, r->now, r->out);
}

static void
tick(Rig *r, gint64 ms)
{
	r->now += ms;
	hk_engine_tick(r->e, r->now, r->out);
}

/* Keys used throughout: evdev code and keysym */
#define A        KEY_A, XKB_KEY_a
#define B        KEY_B, XKB_KEY_b
#define J        KEY_J, XKB_KEY_j
#define T        KEY_T, XKB_KEY_t
#define ESC      KEY_ESC, XKB_KEY_Escape
#define CAPS     KEY_CAPSLOCK, XKB_KEY_Caps_Lock
#define LCTRL    KEY_LEFTCTRL, XKB_KEY_Control_L
#define RCTRL    KEY_RIGHTCTRL, XKB_KEY_Control_R
#define LSHIFT   KEY_LEFTSHIFT, XKB_KEY_Shift_L
#define LALT     KEY_LEFTALT, XKB_KEY_Alt_L
#define LWIN     KEY_LEFTMETA, XKB_KEY_Super_L
#define CTRL     GOWL_KEY_MOD_CTRL
#define SHIFT    GOWL_KEY_MOD_SHIFT
#define ALT      GOWL_KEY_MOD_ALT
#define SUPER    GOWL_KEY_MOD_LOGO

/* ── Plain hotkeys ─────────────────────────────────────────────────── */

static void
test_suppression(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys:\n"
	         "  - {key: \"#!t\", do: {action: spawn, arg: gst}}\n"
	         "  - {key: \"~^b\", do: {notify: hi}}\n");

	/* Swallowed, and its release too */
	g_assert_true(down(&r, T, SUPER | ALT));
	ASSERT_TRACE(&r, "F(#!t)");
	g_assert_true(up(&r, T, SUPER | ALT));
	ASSERT_TRACE(&r, "");
	/* A key with no hotkey reaches the window */
	g_assert_false(down(&r, A, 0));
	g_assert_false(up(&r, A, 0));
	ASSERT_TRACE(&r, "");
	/* ~ fires and lets it through */
	g_assert_false(down(&r, B, CTRL));
	ASSERT_TRACE(&r, "F(~^b)");
	g_assert_false(up(&r, B, CTRL));
	/* The same key without its modifiers is just a key */
	g_assert_false(down(&r, T, SUPER));
	g_assert_false(up(&r, T, SUPER));
	ASSERT_TRACE(&r, "");
	rig_clear(&r);
}

/* A press decided as swallowed keeps its release swallowed, however the
 * modifiers change in between. */
static void
test_release_follows_press(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys: [{key: \"^a\", do: {notify: x}}]\n");
	g_assert_false(down(&r, LCTRL, 0));
	g_assert_true(down(&r, A, CTRL));
	ASSERT_TRACE(&r, "F(^a)");
	g_assert_false(up(&r, LCTRL, CTRL));    /* Ctrl first */
	g_assert_true(up(&r, A, 0));             /* still swallowed */
	ASSERT_TRACE(&r, "");
	rig_clear(&r);
}

static void
test_wildcard_and_sides(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys:\n"
	         "  - {key: \"^a\", do: {notify: exact}}\n"
	         "  - {key: \"*^b\", do: {notify: wild}}\n"
	         "  - {key: \"<^j\", do: {notify: left}}\n");
	/* exact: Ctrl+Shift+a is not ^a */
	g_assert_false(down(&r, A, CTRL | SHIFT));
	up(&r, A, CTRL | SHIFT);
	ASSERT_TRACE(&r, "");
	/* wildcard: extra modifiers are fine, missing ones are not */
	g_assert_true(down(&r, B, CTRL | SHIFT | ALT));
	up(&r, B, CTRL | SHIFT | ALT);
	ASSERT_TRACE(&r, "F(*^b)");
	g_assert_false(down(&r, B, SHIFT));
	up(&r, B, SHIFT);
	ASSERT_TRACE(&r, "");
	/* left hand only */
	down(&r, RCTRL, 0);
	g_assert_false(down(&r, J, CTRL));
	up(&r, J, CTRL);
	up(&r, RCTRL, CTRL);
	ASSERT_TRACE(&r, "");
	down(&r, LCTRL, 0);
	g_assert_true(down(&r, J, CTRL));
	up(&r, J, CTRL);
	up(&r, LCTRL, CTRL);
	ASSERT_TRACE(&r, "F(<^j)");
	rig_clear(&r);
}

static void
test_up_hotkeys(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys:\n"
	         "  - {key: \"a UP\", do: {notify: released}}\n"
	         "  - {key: \"b\", do: {notify: pressed}}\n"
	         "  - {id: b-up, key: \"b UP\", do: {notify: released}}\n"
	         "  - {key: \"~j UP\", do: {notify: passthrough}}\n");
	/* An UP hotkey alone swallows the press, fires on the release */
	g_assert_true(down(&r, A, 0));
	ASSERT_TRACE(&r, "");
	g_assert_true(up(&r, A, 0));
	ASSERT_TRACE(&r, "F(a UP)");
	/* Down and up hotkeys on one key: each at its edge */
	g_assert_true(down(&r, B, 0));
	ASSERT_TRACE(&r, "F(b)");
	g_assert_true(up(&r, B, 0));
	ASSERT_TRACE(&r, "F(b-up)");
	/* ~ UP: the key works, the release fires */
	g_assert_false(down(&r, J, 0));
	g_assert_false(up(&r, J, 0));
	ASSERT_TRACE(&r, "F(~j UP)");
	rig_clear(&r);
}

/* ── Combinations and prefixes ─────────────────────────────────────── */

static void
test_combination(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys: [{key: \"CapsLock & j\", do: {send: \"{Down}\"}}]\n");
	/* CapsLock becomes a prefix: swallowed */
	g_assert_true(down(&r, CAPS, 0));
	ASSERT_TRACE(&r, "");
	g_assert_true(down(&r, J, 0));
	ASSERT_TRACE(&r, "F(CapsLock & j)");
	g_assert_true(up(&r, J, 0));
	g_assert_true(up(&r, CAPS, 0));
	/* With no hotkey of its own, a prefix tapped alone does nothing --
	 * AutoHotkey's rule: it loses its native function */
	g_assert_true(down(&r, CAPS, 0));
	g_assert_true(up(&r, CAPS, 0));
	ASSERT_TRACE(&r, "");
	/* j alone is j */
	g_assert_false(down(&r, J, 0));
	g_assert_false(up(&r, J, 0));
	/* A combination ignores other modifiers */
	down(&r, CAPS, 0);
	g_assert_true(down(&r, J, SHIFT));
	up(&r, J, SHIFT);
	up(&r, CAPS, 0);
	ASSERT_TRACE(&r, "F(CapsLock & j)");
	rig_clear(&r);
}

static void
test_prefix_solo(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys:\n"
	         "  - {key: \"CapsLock & j\", do: {send: \"{Down}\"}}\n"
	         "  - {key: \"CapsLock\", do: {send: \"{Esc}\"}}\n");
	/* Tapped alone: its own hotkey, on the release */
	g_assert_true(down(&r, CAPS, 0));
	ASSERT_TRACE(&r, "");
	g_assert_true(up(&r, CAPS, 0));
	ASSERT_TRACE(&r, "F(CapsLock)");
	/* Used as a prefix: only the combination */
	down(&r, CAPS, 0);
	down(&r, J, 0);
	up(&r, J, 0);
	up(&r, CAPS, 0);
	ASSERT_TRACE(&r, "F(CapsLock & j)");
	/* Another key in between counts as use too */
	down(&r, CAPS, 0);
	g_assert_false(down(&r, A, 0));
	up(&r, A, 0);
	up(&r, CAPS, 0);
	ASSERT_TRACE(&r, "");
	rig_clear(&r);
}

static void
test_prefix_passthrough(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys:\n"
	         "  - {key: \"~CapsLock & j\", do: {notify: x}}\n"
	         "  - {key: \"LShift & a\", do: {notify: y}}\n");
	/* ~ keeps the prefix working */
	g_assert_false(down(&r, CAPS, 0));
	g_assert_false(down(&r, J, 0));      /* the combination itself is ~ */
	up(&r, J, 0);
	g_assert_false(up(&r, CAPS, 0));
	ASSERT_TRACE(&r, "F(~CapsLock & j)");
	/* A modifier prefix is never swallowed: it still modifies */
	g_assert_false(down(&r, LSHIFT, 0));
	g_assert_true(down(&r, A, SHIFT));
	up(&r, A, SHIFT);
	up(&r, LSHIFT, SHIFT);
	ASSERT_TRACE(&r, "F(LShift & a)");
	rig_clear(&r);
}

/* ── Lone modifiers ────────────────────────────────────────────────── */

static void
test_lone_modifier(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys:\n"
	         "  - {key: \"LWin\", do: {action: spawn, arg: menu}}\n"
	         "  - {key: \"~LAlt\", do: {notify: alt}}\n");
	/* Tapped: fires on the release, and never swallowed */
	g_assert_false(down(&r, LWIN, 0));
	ASSERT_TRACE(&r, "");
	g_assert_false(up(&r, LWIN, SUPER));
	ASSERT_TRACE(&r, "F(LWin)");
	/* Used as a modifier: nothing */
	down(&r, LWIN, 0);
	down(&r, T, SUPER);
	up(&r, T, SUPER);
	up(&r, LWIN, SUPER);
	ASSERT_TRACE(&r, "");
	/* ~ fires on the press */
	g_assert_false(down(&r, LALT, 0));
	ASSERT_TRACE(&r, "F(~LAlt)");
	up(&r, LALT, ALT);
	ASSERT_TRACE(&r, "");
	rig_clear(&r);
}

static void
test_lone_modifier_double(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys: [{key: \"LShift\", double: 300, "
	         "do: {action: spawn, arg: menu}}]\n");
	down(&r, LSHIFT, 0);
	up(&r, LSHIFT, SHIFT);
	ASSERT_TRACE(&r, "");
	r.now += 150;
	down(&r, LSHIFT, 0);
	up(&r, LSHIFT, SHIFT);
	ASSERT_TRACE(&r, "F(LShift)");
	/* Too slow: no */
	down(&r, LSHIFT, 0);
	up(&r, LSHIFT, SHIFT);
	r.now += 500;
	down(&r, LSHIFT, 0);
	up(&r, LSHIFT, SHIFT);
	ASSERT_TRACE(&r, "");
	rig_clear(&r);
}

/* ── Double taps ───────────────────────────────────────────────────── */

static void
test_double_tap(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys: [{id: dbl, key: Esc, double: 300, do: {notify: x}}]\n");
	/* In time: the second press fires; both are swallowed */
	g_assert_true(down(&r, ESC, 0));
	g_assert_true(up(&r, ESC, 0));
	ASSERT_TRACE(&r, "");
	r.now += 120;
	g_assert_true(down(&r, ESC, 0));
	ASSERT_TRACE(&r, "F(dbl)");
	g_assert_true(up(&r, ESC, 0));
	tick(&r, 1000);
	ASSERT_TRACE(&r, "");
	/* Out of time: the single press is put back -- Escape still works */
	down(&r, ESC, 0);
	up(&r, ESC, 0);
	g_assert_cmpint(hk_engine_next_deadline(r.e), ==, r.now + 300);
	tick(&r, 299);
	ASSERT_TRACE(&r, "");
	tick(&r, 1);
	ASSERT_TRACE(&r, "K(1+) K(1-)");
	rig_clear(&r);
}

static void
test_double_with_single(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys:\n"
	         "  - {id: single, key: Esc, do: {notify: one}}\n"
	         "  - {id: dbl, key: Esc, double: 300, do: {notify: two}}\n");
	/* One tap: the single, once the double time has passed -- never
	 * both */
	down(&r, ESC, 0);
	up(&r, ESC, 0);
	ASSERT_TRACE(&r, "");
	tick(&r, 400);
	ASSERT_TRACE(&r, "F(single)");
	/* Two taps: only the double */
	down(&r, ESC, 0);
	up(&r, ESC, 0);
	r.now += 100;
	down(&r, ESC, 0);
	up(&r, ESC, 0);
	tick(&r, 400);
	ASSERT_TRACE(&r, "F(dbl)");
	rig_clear(&r);
}

static void
test_double_interrupted(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys: [{id: dbl, key: Esc, double: 300, do: {notify: x}}]\n");
	down(&r, ESC, 0);
	up(&r, ESC, 0);
	/* Another key first: Escape was a single, put back AHEAD of the new
	 * key, which is swallowed and put back after it */
	g_assert_true(down(&r, A, 0));
	ASSERT_TRACE(&r, "K(1+) K(1-) K(30+)");
	g_assert_false(up(&r, A, 0));        /* its press was put back */
	tick(&r, 1000);
	ASSERT_TRACE(&r, "");
	rig_clear(&r);
}

static void
test_double_passthrough(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys: [{id: dbl, key: \"~LButton\", double: 250, "
	         "do: {notify: x}}]\n");
	g_assert_false(button(&r, BTN_LEFT, TRUE, 0));
	g_assert_false(button(&r, BTN_LEFT, FALSE, 0));
	r.now += 100;
	g_assert_false(button(&r, BTN_LEFT, TRUE, 0));
	ASSERT_TRACE(&r, "F(dbl)");
	button(&r, BTN_LEFT, FALSE, 0);
	rig_clear(&r);
}

/* ── Long presses ──────────────────────────────────────────────────── */

static void
test_long_press(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys: [{id: lock, key: Esc, hold: 500, "
	         "do: {action: spawn, arg: lock}}]\n");
	/* Held: fires at its time; the release stays swallowed */
	g_assert_true(down(&r, ESC, 0));
	tick(&r, 499);
	ASSERT_TRACE(&r, "");
	tick(&r, 1);
	ASSERT_TRACE(&r, "F(lock)");
	g_assert_true(up(&r, ESC, 0));
	ASSERT_TRACE(&r, "");
	/* Tapped: put back as it was */
	g_assert_true(down(&r, ESC, 0));
	r.now += 100;
	g_assert_true(up(&r, ESC, 0));
	ASSERT_TRACE(&r, "K(1+) K(1-)");
	tick(&r, 1000);
	ASSERT_TRACE(&r, "");
	rig_clear(&r);
}

static void
test_long_press_with_tap(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys:\n"
	         "  - {id: lock, key: Esc, hold: 500, do: {notify: hold}}\n"
	         "  - {id: tap, key: Esc, do: {notify: tap}}\n");
	down(&r, ESC, 0);
	r.now += 50;
	up(&r, ESC, 0);
	ASSERT_TRACE(&r, "F(tap)");
	down(&r, ESC, 0);
	tick(&r, 600);
	up(&r, ESC, 0);
	ASSERT_TRACE(&r, "F(lock)");
	rig_clear(&r);
}

static void
test_long_press_interrupted(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys: [{id: lock, key: Esc, hold: 500, do: {notify: x}}]\n");
	down(&r, ESC, 0);
	/* Typing on: Escape was a tap, and comes first */
	g_assert_true(down(&r, A, 0));
	ASSERT_TRACE(&r, "K(1+) K(30+)");
	g_assert_false(up(&r, A, 0));
	g_assert_false(up(&r, ESC, 0));      /* its press was put back */
	tick(&r, 1000);
	ASSERT_TRACE(&r, "");
	rig_clear(&r);
}

/* ── Modes, suspend, levels ────────────────────────────────────────── */

static void
test_modes(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "modes:\n"
	         "  resize:\n"
	         "    hotkeys: [{key: h, do: {notify: shrink}}]\n"
	         "hotkeys:\n"
	         "  - {key: \"#r\", do: {mode: resize}}\n"
	         "  - {key: \"#q\", mode: \"*\", do: {notify: anywhere}}\n");
	/* default: only default hotkeys */
	g_assert_false(down(&r, KEY_H, XKB_KEY_h, 0));
	up(&r, KEY_H, XKB_KEY_h, 0);
	ASSERT_TRACE(&r, "");
	/* in the mode: its hotkeys, and not the default ones */
	r.mode = g_strdup("resize");
	g_assert_true(down(&r, KEY_H, XKB_KEY_h, 0));
	up(&r, KEY_H, XKB_KEY_h, 0);
	ASSERT_TRACE(&r, "F(h#resize)");
	g_assert_false(down(&r, KEY_R, XKB_KEY_r, SUPER));
	up(&r, KEY_R, XKB_KEY_r, SUPER);
	ASSERT_TRACE(&r, "");
	/* "*": every mode */
	g_assert_true(down(&r, KEY_Q, XKB_KEY_q, SUPER));
	up(&r, KEY_Q, XKB_KEY_q, SUPER);
	ASSERT_TRACE(&r, "F(#q#*)");
	rig_clear(&r);
}

static void
test_swallowing_mode(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "modes: {nav: {swallow: true, hotkeys: [{key: j, "
	         "do: {send: \"{Down}\"}}]}}\n");
	r.mode = g_strdup("nav");
	hk_engine_set_swallow(r.e, TRUE);
	g_assert_true(down(&r, J, 0));
	up(&r, J, 0);
	ASSERT_TRACE(&r, "F(j#nav)");
	/* unbound: eaten, release too */
	g_assert_true(down(&r, A, 0));
	g_assert_true(up(&r, A, 0));
	/* a modifier is not */
	g_assert_false(down(&r, LCTRL, 0));
	g_assert_false(up(&r, LCTRL, CTRL));
	ASSERT_TRACE(&r, "");
	rig_clear(&r);
}

static void
test_suspend(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys:\n"
	         "  - {key: a, do: {notify: x}}\n"
	         "  - {key: b, suspend-exempt: true, do: {suspend: toggle}}\n");
	hk_engine_set_suspended(r.e, TRUE);
	g_assert_false(down(&r, A, 0));
	up(&r, A, 0);
	g_assert_true(down(&r, B, 0));
	up(&r, B, 0);
	ASSERT_TRACE(&r, "F(b)");
	hk_engine_set_suspended(r.e, FALSE);
	g_assert_true(down(&r, A, 0));
	up(&r, A, 0);
	ASSERT_TRACE(&r, "F(a)");
	rig_clear(&r);
}

static void
test_levels_and_synthetic(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys:\n"
	         "  - {key: a, do: {notify: x}}\n"
	         "  - {key: \"$b\", do: {notify: y}}\n"
	         "  - {key: j, level: 2, do: {notify: z}}\n");
	/* Our own Send at level 0: no hotkey of level 0 */
	g_assert_false(key_full(&r, A, 0, TRUE, TRUE, 0));
	key_full(&r, A, 0, FALSE, TRUE, 0);
	ASSERT_TRACE(&r, "");
	/* At level 1 it does */
	g_assert_true(key_full(&r, A, 0, TRUE, TRUE, 1));
	key_full(&r, A, 0, FALSE, TRUE, 1);
	ASSERT_TRACE(&r, "F(a)");
	/* ... but never a $ hotkey */
	g_assert_false(key_full(&r, B, 0, TRUE, TRUE, 5));
	key_full(&r, B, 0, FALSE, TRUE, 5);
	ASSERT_TRACE(&r, "");
	/* and a level-2 hotkey needs a level above 2 */
	g_assert_false(key_full(&r, J, 0, TRUE, TRUE, 2));
	key_full(&r, J, 0, FALSE, TRUE, 2);
	g_assert_true(key_full(&r, J, 0, TRUE, TRUE, 3));
	key_full(&r, J, 0, FALSE, TRUE, 3);
	ASSERT_TRACE(&r, "F(j)");
	/* Someone else's injected input is ignored... */
	g_assert_false(key_full(&r, A, 0, TRUE, TRUE, -1));
	key_full(&r, A, 0, FALSE, TRUE, -1);
	ASSERT_TRACE(&r, "");
	/* ...unless accepted (a KVM keyboard is a keyboard), still not $ */
	hk_engine_set_accept_synthetic(r.e, TRUE);
	g_assert_true(key_full(&r, A, 0, TRUE, TRUE, -1));
	key_full(&r, A, 0, FALSE, TRUE, -1);
	g_assert_false(key_full(&r, B, 0, TRUE, TRUE, -1));
	key_full(&r, B, 0, FALSE, TRUE, -1);
	ASSERT_TRACE(&r, "F(a)");
	rig_clear(&r);
}

/* ── Remaps ────────────────────────────────────────────────────────── */

static void
test_remaps(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "remaps:\n"
	         "  CapsLock: Control_L\n"
	         "  F13: \"^Esc\"\n"
	         "  XButton1: LButton\n");
	g_assert_true(down(&r, CAPS, 0));
	ASSERT_TRACE(&r, "R(Control_L+)");
	g_assert_true(up(&r, CAPS, 0));
	ASSERT_TRACE(&r, "R(Control_L-)");
	/* a remap is a wildcard: modifiers held are carried through */
	g_assert_true(down(&r, CAPS, SHIFT));
	up(&r, CAPS, SHIFT);
	ASSERT_TRACE(&r, "R(Control_L+) R(Control_L-)");
	/* a target with modifiers of its own */
	g_assert_true(down(&r, KEY_F13, XKB_KEY_F13, 0));
	up(&r, KEY_F13, XKB_KEY_F13, 0);
	ASSERT_TRACE(&r, "R(mods:Escape+) R(mods:Escape-)");
	/* a button onto a button */
	g_assert_true(button(&r, BTN_SIDE, TRUE, 0));
	g_assert_true(button(&r, BTN_SIDE, FALSE, 0));
	ASSERT_TRACE(&r, "B(272+) B(272-)");
	rig_clear(&r);
}

/* A remap target held across a reload is still released. */
static void
test_remap_survives_reload(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "remaps: {CapsLock: Control_L}\n");
	down(&r, CAPS, 0);
	ASSERT_TRACE(&r, "R(Control_L+)");
	load(&r, "hotkeys: [{key: a, do: {notify: x}}]\n");
	g_assert_true(up(&r, CAPS, 0));
	ASSERT_TRACE(&r, "R(Control_L-)");
	/* and the next CapsLock is just CapsLock */
	g_assert_false(down(&r, CAPS, 0));
	g_assert_false(up(&r, CAPS, 0));
	ASSERT_TRACE(&r, "");
	rig_clear(&r);
}

/* A hotkey on the key wins over a remap of it. */
static void
test_hotkey_beats_remap(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "remaps: {a: b}\n"
	         "hotkeys: [{key: \"^a\", do: {notify: x}}]\n");
	down(&r, A, CTRL);
	up(&r, A, CTRL);
	ASSERT_TRACE(&r, "F(^a)");
	down(&r, A, 0);
	up(&r, A, 0);
	ASSERT_TRACE(&r, "R(b+) R(b-)");
	rig_clear(&r);
}

/* ── Choosing among hotkeys ────────────────────────────────────────── */

static void
test_contexts_and_priority(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys:\n"
	         "  - {id: global, key: \"^t\", do: {notify: g}}\n"
	         "  - {id: browser, key: \"^t\", context: browser, do: {notify: b}}\n"
	         "  - {id: low, key: \"^j\", do: {notify: l}}\n"
	         "  - {id: high, key: \"^j\", priority: 5, context: browser, "
	         "do: {notify: h}}\n"
	         "  - {id: top, key: \"^j\", priority: 10, do: {notify: t}}\n");
	/* Not in the browser: the global one */
	down(&r, T, CTRL);
	up(&r, T, CTRL);
	ASSERT_TRACE(&r, "F(global)");
	/* In it: the context one beats the global one */
	g_hash_table_add(r.contexts, g_strdup("browser"));
	down(&r, T, CTRL);
	up(&r, T, CTRL);
	ASSERT_TRACE(&r, "F(browser)");
	/* Priority first, then the context */
	down(&r, J, CTRL);
	up(&r, J, CTRL);
	ASSERT_TRACE(&r, "F(top)");
	/* The caller refusing one (it is running already) gives the next */
	g_hash_table_add(r.refuse, g_strdup("top"));
	down(&r, J, CTRL);
	up(&r, J, CTRL);
	ASSERT_TRACE(&r, "F(high)");
	g_hash_table_add(r.refuse, g_strdup("high"));
	g_hash_table_add(r.refuse, g_strdup("low"));
	/* None allowed: the key is a key */
	g_assert_false(down(&r, J, CTRL));
	up(&r, J, CTRL);
	ASSERT_TRACE(&r, "");
	rig_clear(&r);
}

/* A later definition of the same id replaces the earlier. */
static void
test_later_wins(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys:\n"
	         "  - {id: x, key: a, do: {notify: first}}\n"
	         "  - {id: x, key: b, do: {notify: second}}\n");
	g_assert_cmpuint(r.set->hotkeys->len, ==, 1);
	g_assert_false(down(&r, A, 0));
	up(&r, A, 0);
	g_assert_true(down(&r, B, 0));
	up(&r, B, 0);
	ASSERT_TRACE(&r, "F(x)");
	rig_clear(&r);
}

/* ── Buttons and the wheel ─────────────────────────────────────────── */

static void
test_buttons_and_wheel(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys:\n"
	         "  - {key: XButton2, do: {notify: fwd}}\n"
	         "  - {key: \"XButton1 & WheelUp\", do: {notify: prev}}\n"
	         "  - {key: \"^WheelDown\", do: {notify: zoom}}\n");
	g_assert_true(button(&r, BTN_EXTRA, TRUE, 0));
	g_assert_true(button(&r, BTN_EXTRA, FALSE, 0));
	ASSERT_TRACE(&r, "F(XButton2)");
	/* a button as a prefix, the wheel as the key */
	g_assert_true(button(&r, BTN_SIDE, TRUE, 0));
	g_assert_true(wheel(&r, GOWL_BUTTON_WHEEL_UP, 0));
	g_assert_true(wheel(&r, GOWL_BUTTON_WHEEL_UP, 0));
	g_assert_true(button(&r, BTN_SIDE, FALSE, 0));
	ASSERT_TRACE(&r, "F(XButton1 & WheelUp) F(XButton1 & WheelUp)");
	/* the wheel with a modifier; without it, it scrolls */
	g_assert_true(wheel(&r, GOWL_BUTTON_WHEEL_DOWN, CTRL));
	g_assert_false(wheel(&r, GOWL_BUTTON_WHEEL_DOWN, 0));
	g_assert_false(wheel(&r, GOWL_BUTTON_WHEEL_UP, 0));
	ASSERT_TRACE(&r, "F(^WheelDown)");
	/* an unbound button is a button */
	g_assert_false(button(&r, BTN_LEFT, TRUE, 0));
	g_assert_false(button(&r, BTN_LEFT, FALSE, 0));
	rig_clear(&r);
}

/* ── State ─────────────────────────────────────────────────────────── */

static void
test_history_and_state(void)
{
	Rig r;
	GPtrArray *h;
	GowlHotkeyInput in;
	const gchar *prior;
	guint i;

	rig_init(&r);
	load(&r, "hotkeys: [{key: a, do: {notify: x}}]\n");
	down(&r, A, 0);
	memset(&in, 0, sizeof in);
	in.kind = GOWL_HOTKEY_INPUT_KEY;
	in.keysym = XKB_KEY_a;
	g_assert_true(hk_engine_key_is_down(r.e, &in));
	up(&r, A, 0);
	g_assert_false(hk_engine_key_is_down(r.e, &in));
	prior = hk_engine_get_prior(r.e, NULL);
	g_assert_cmpstr(prior, ==, "a");
	h = hk_engine_get_history(r.e);
	g_assert_cmpuint(h->len, ==, 2);
	g_assert_cmpstr(((HkHistoryEntry *)g_ptr_array_index(h, 0))->fired, ==,
	                "a");
	g_assert_true(((HkHistoryEntry *)g_ptr_array_index(h, 0))->consumed);
	/* bounded */
	for (i = 0; i < 100; i++) {
		down(&r, B, 0);
		up(&r, B, 0);
	}
	g_assert_cmpuint(h->len, ==, HK_HISTORY_SIZE);
	hk_out_clear(r.out);
	rig_clear(&r);
}

static void
test_reset(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "remaps: {CapsLock: Control_L}\n"
	         "hotkeys: [{id: lock, key: Esc, hold: 500, do: {notify: x}}]\n");
	down(&r, CAPS, 0);
	down(&r, ESC, 0);
	trace(&r);
	g_assert_cmpuint(hk_engine_n_held(r.e), ==, 2);
	/* a lock: the remap target let go, the long press forgotten */
	hk_engine_reset(r.e, r.out);
	ASSERT_TRACE(&r, "R(Control_L-)");
	g_assert_cmpuint(hk_engine_n_held(r.e), ==, 0);
	g_assert_cmpint(hk_engine_next_deadline(r.e), ==, -1);
	tick(&r, 1000);
	ASSERT_TRACE(&r, "");
	/* the releases that follow are nobody's */
	g_assert_false(up(&r, CAPS, 0));
	g_assert_false(up(&r, ESC, 0));
	rig_clear(&r);
}

/* Our own injection of a key that is physically held (a key put back)
 * does not take the physical key's place. */
static void
test_own_injection_keeps_physical(void)
{
	Rig r;

	rig_init(&r);
	load(&r, "hotkeys: [{id: lock, key: Esc, hold: 500, do: {notify: x}}]\n");
	down(&r, ESC, 0);
	down(&r, A, 0);
	ASSERT_TRACE(&r, "K(1+) K(30+)");
	/* the caller injects what was put back */
	g_assert_false(key_full(&r, ESC, 0, TRUE, TRUE, 0));
	g_assert_false(key_full(&r, A, 0, TRUE, TRUE, 0));
	/* the physical releases are decided by the physical presses */
	g_assert_false(up(&r, A, 0));
	g_assert_false(up(&r, ESC, 0));
	g_assert_cmpuint(hk_engine_n_held(r.e), ==, 0);
	rig_clear(&r);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/hotkey/engine/suppression", test_suppression);
	g_test_add_func("/hotkey/engine/release-follows-press",
	                test_release_follows_press);
	g_test_add_func("/hotkey/engine/wildcard-and-sides",
	                test_wildcard_and_sides);
	g_test_add_func("/hotkey/engine/up", test_up_hotkeys);
	g_test_add_func("/hotkey/engine/combination", test_combination);
	g_test_add_func("/hotkey/engine/prefix-solo", test_prefix_solo);
	g_test_add_func("/hotkey/engine/prefix-passthrough",
	                test_prefix_passthrough);
	g_test_add_func("/hotkey/engine/lone-modifier", test_lone_modifier);
	g_test_add_func("/hotkey/engine/lone-modifier-double",
	                test_lone_modifier_double);
	g_test_add_func("/hotkey/engine/double", test_double_tap);
	g_test_add_func("/hotkey/engine/double-with-single",
	                test_double_with_single);
	g_test_add_func("/hotkey/engine/double-interrupted",
	                test_double_interrupted);
	g_test_add_func("/hotkey/engine/double-passthrough",
	                test_double_passthrough);
	g_test_add_func("/hotkey/engine/hold", test_long_press);
	g_test_add_func("/hotkey/engine/hold-with-tap", test_long_press_with_tap);
	g_test_add_func("/hotkey/engine/hold-interrupted",
	                test_long_press_interrupted);
	g_test_add_func("/hotkey/engine/modes", test_modes);
	g_test_add_func("/hotkey/engine/swallowing-mode", test_swallowing_mode);
	g_test_add_func("/hotkey/engine/suspend", test_suspend);
	g_test_add_func("/hotkey/engine/levels-and-synthetic",
	                test_levels_and_synthetic);
	g_test_add_func("/hotkey/engine/remaps", test_remaps);
	g_test_add_func("/hotkey/engine/remap-survives-reload",
	                test_remap_survives_reload);
	g_test_add_func("/hotkey/engine/hotkey-beats-remap",
	                test_hotkey_beats_remap);
	g_test_add_func("/hotkey/engine/contexts-and-priority",
	                test_contexts_and_priority);
	g_test_add_func("/hotkey/engine/later-wins", test_later_wins);
	g_test_add_func("/hotkey/engine/buttons-and-wheel",
	                test_buttons_and_wheel);
	g_test_add_func("/hotkey/engine/history-and-state",
	                test_history_and_state);
	g_test_add_func("/hotkey/engine/reset", test_reset);
	g_test_add_func("/hotkey/engine/own-injection-keeps-physical",
	                test_own_injection_keeps_physical);

	return g_test_run();
}
