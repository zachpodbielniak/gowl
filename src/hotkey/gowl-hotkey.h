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
 * gowl-hotkey.h - The API the hotkey module and its extenders share.
 *
 * The hotkey module (modules/hotkey, docs/hotkey.org) is AutoHotkey for
 * gowl: hotkeys with AutoHotkey's modifier semantics, custom
 * combinations, mouse and wheel hotkeys, remaps, modes, contexts,
 * timers and event hooks, each running a list of actions.  This header
 * is the half of it that lives in libgowl, so that it works whether or
 * not the module is loaded yet -- a C config, a crispy hotkey script,
 * a macro or an embedder (cmacs) uses it:
 *
 *   - KEY SPECS.  gowl_hotkey_spec_parse() reads a hotkey the way the
 *     module does, in AutoHotkey's notation (`#!t', `~LButton',
 *     `CapsLock & j', `^a UP') or gowl's (`Super+Alt+t').
 *   - FUNCTIONS.  gowl_hotkey_register_func() names a C function that
 *     a `{call: NAME}' action runs.
 *   - CONTEXTS.  gowl_hotkey_register_context() names a C predicate a
 *     hotkey's `context:' can use; gowl_hotkey_set_context() names a
 *     plain boolean someone else keeps up to date (cmacs's Elisp
 *     contexts are these: Lisp never runs while a key is decided).
 *   - SOURCES.  gowl_hotkey_add_yaml() adds a `hotkey:' section that
 *     outlives a config reload; gowl_hotkey_define() is the one-line
 *     form for "this key runs this C function".
 *   - COMMANDS.  gowl_hotkey_send() and friends talk to the running
 *     module through gowl_compositor_run_command().
 *
 * Everything here is thread-safe.  Registered functions and predicates
 * are called on the compositor thread, under the fault guard.
 */

#ifndef GOWL_HOTKEY_H
#define GOWL_HOTKEY_H

#include <glib-object.h>

#include "gowl-types.h"

G_BEGIN_DECLS

/**
 * GOWL_HOTKEY_ERROR:
 *
 * The error domain of hotkey specs and sources.
 */
#define GOWL_HOTKEY_ERROR (gowl_hotkey_error_quark())

/**
 * GowlHotkeyError:
 * @GOWL_HOTKEY_ERROR_SYNTAX: the text is not a hotkey
 * @GOWL_HOTKEY_ERROR_UNKNOWN_KEY: a key or button name gowl does not know
 * @GOWL_HOTKEY_ERROR_UNSUPPORTED: something AutoHotkey itself refuses,
 *   such as a three-key combination or a wheel `UP' hotkey
 * @GOWL_HOTKEY_ERROR_NOT_FOUND: no such function, context or source
 */
typedef enum {
	GOWL_HOTKEY_ERROR_SYNTAX,
	GOWL_HOTKEY_ERROR_UNKNOWN_KEY,
	GOWL_HOTKEY_ERROR_UNSUPPORTED,
	GOWL_HOTKEY_ERROR_NOT_FOUND
} GowlHotkeyError;

GQuark gowl_hotkey_error_quark (void);

/* ── Key specs ─────────────────────────────────────────────────────── */

/**
 * GowlHotkeyInputKind:
 * @GOWL_HOTKEY_INPUT_NONE: nothing (an absent prefix)
 * @GOWL_HOTKEY_INPUT_KEY: a keyboard key
 * @GOWL_HOTKEY_INPUT_BUTTON: a pointer button
 * @GOWL_HOTKEY_INPUT_WHEEL: one notch of a scroll wheel
 *
 * What kind of input a hotkey's trigger (or prefix) is.
 */
typedef enum {
	GOWL_HOTKEY_INPUT_NONE,
	GOWL_HOTKEY_INPUT_KEY,
	GOWL_HOTKEY_INPUT_BUTTON,
	GOWL_HOTKEY_INPUT_WHEEL
} GowlHotkeyInputKind;

/**
 * GowlHotkeyInput:
 * @kind: what kind of input
 * @keysym: for a key, its XKB keysym, lower-cased; 0 for a `code:NN' key
 * @keysym_alt: for a key, a second keysym that also matches -- the
 *   right-hand key of a neutral modifier (`Ctrl' is Control_L or
 *   Control_R) -- else 0
 * @keycode: for a `code:NN' key, its evdev keycode; else 0
 * @button: for a button, its evdev code (BTN_LEFT ...); for a wheel,
 *   GOWL_BUTTON_WHEEL_UP / _DOWN / _LEFT / _RIGHT
 *
 * One key, button or wheel direction, as a hotkey names it.
 */
typedef struct {
	GowlHotkeyInputKind kind;
	guint32             keysym;
	guint32             keysym_alt;
	guint32             keycode;
	guint32             button;
} GowlHotkeyInput;

/**
 * GowlHotkeyFlags:
 * @GOWL_HOTKEY_FLAG_NONE: none
 * @GOWL_HOTKEY_FLAG_WILDCARD: `*' -- fires with extra modifiers held
 * @GOWL_HOTKEY_FLAG_PASSTHROUGH: `~' -- the key still reaches the window
 * @GOWL_HOTKEY_FLAG_NO_SYNTHETIC: `$' -- never fired by injected input,
 *   whatever its send level
 * @GOWL_HOTKEY_FLAG_ON_RELEASE: ` UP' -- fires when the key is released
 *
 * The prefix symbols of an AutoHotkey hotkey (and the `UP' suffix).
 */
typedef enum {
	GOWL_HOTKEY_FLAG_NONE         = 0,
	GOWL_HOTKEY_FLAG_WILDCARD     = (1 << 0),
	GOWL_HOTKEY_FLAG_PASSTHROUGH  = (1 << 1),
	GOWL_HOTKEY_FLAG_NO_SYNTHETIC = (1 << 2),
	GOWL_HOTKEY_FLAG_ON_RELEASE   = (1 << 3)
} GowlHotkeyFlags;

/**
 * GowlHotkeySpec:
 * @trigger: the key, button or wheel that fires it
 * @prefix: for a custom combination `A & B', the prefix A (held while
 *   B is pressed); %GOWL_HOTKEY_INPUT_NONE otherwise
 * @mods: the #GowlKeyMod bits that must be held
 * @left: the bits of @mods that must be held on the LEFT-hand key
 *   (`<^' is the left Control)
 * @right: the bits of @mods that must be held on the right-hand key
 * @flags: #GowlHotkeyFlags
 *
 * A parsed hotkey.  Plain data: compare with gowl_hotkey_spec_equal(),
 * print with gowl_hotkey_spec_to_string().
 */
typedef struct {
	GowlHotkeyInput trigger;
	GowlHotkeyInput prefix;
	guint           mods;
	guint           left;
	guint           right;
	guint           flags;
} GowlHotkeySpec;

gboolean  gowl_hotkey_spec_parse        (const gchar           *text,
                                         GowlHotkeySpec        *spec,
                                         GError               **error);
gchar    *gowl_hotkey_spec_to_string    (const GowlHotkeySpec  *spec);
gboolean  gowl_hotkey_spec_equal        (const GowlHotkeySpec  *a,
                                         const GowlHotkeySpec  *b);

gboolean  gowl_hotkey_input_parse       (const gchar           *name,
                                         GowlHotkeyInput       *input,
                                         GError               **error);
gchar    *gowl_hotkey_input_to_string   (const GowlHotkeyInput *input);
gboolean  gowl_hotkey_input_matches_key (const GowlHotkeyInput *input,
                                         guint32                keycode,
                                         guint32                keysym,
                                         guint32                base_keysym);

guint     gowl_hotkey_keysym_modifier   (guint32                keysym);
gint      gowl_hotkey_keysym_side       (guint32                keysym);

/* ── Calls into C ──────────────────────────────────────────────────── */

/**
 * GowlHotkeyCall:
 *
 * One call of a registered function or predicate: who called it and
 * with what.  Opaque, and valid for the length of the call only.
 */
typedef struct _GowlHotkeyCall GowlHotkeyCall;

GowlCompositor      *gowl_hotkey_call_get_compositor (GowlHotkeyCall *call);
gpointer             gowl_hotkey_call_get_focused    (GowlHotkeyCall *call);
const gchar         *gowl_hotkey_call_get_id         (GowlHotkeyCall *call);
const gchar         *gowl_hotkey_call_get_trigger    (GowlHotkeyCall *call);
const gchar         *gowl_hotkey_call_get_detail     (GowlHotkeyCall *call);
const gchar * const *gowl_hotkey_call_get_args       (GowlHotkeyCall *call);
const gchar         *gowl_hotkey_call_get_arg        (GowlHotkeyCall *call,
                                                      guint           index);
void                 gowl_hotkey_call_set_result     (GowlHotkeyCall *call,
                                                      const gchar    *result);

/**
 * GowlHotkeyFunc:
 * @call: the call: its compositor, the hotkey's id, the arguments
 * @user_data: the data given to gowl_hotkey_register_func()
 *
 * A function a `{call: NAME}' action runs.  Called on the compositor
 * thread under the fault guard; it must not block.  What it gives
 * gowl_hotkey_call_set_result() is the reply to `hotkey-call'.
 */
typedef void (*GowlHotkeyFunc) (GowlHotkeyCall *call,
                                gpointer        user_data);

/**
 * GowlHotkeyPredicate:
 * @call: the call; gowl_hotkey_call_get_focused() is the window the
 *   key is going to (or %NULL), gowl_hotkey_call_get_id() the hotkey
 * @user_data: the data given to gowl_hotkey_register_context()
 *
 * A context: whether a hotkey that names it may fire now.  Called on
 * the compositor thread, under the fault guard, while a key is being
 * decided -- so it must be quick.  A predicate that faults is false.
 *
 * Returns: %TRUE when the hotkey may fire
 */
typedef gboolean (*GowlHotkeyPredicate) (GowlHotkeyCall *call,
                                         gpointer        user_data);

/* ── Registries ────────────────────────────────────────────────────── */

void      gowl_hotkey_register_func      (const gchar          *name,
                                          GowlHotkeyFunc        func,
                                          gpointer              user_data,
                                          GDestroyNotify        destroy);
gboolean  gowl_hotkey_unregister_func    (const gchar          *name);
GStrv     gowl_hotkey_list_funcs         (void);

void      gowl_hotkey_register_context   (const gchar          *name,
                                          GowlHotkeyPredicate   predicate,
                                          gpointer              user_data,
                                          GDestroyNotify        destroy);
gboolean  gowl_hotkey_unregister_context (const gchar          *name);
void      gowl_hotkey_set_context        (const gchar          *name,
                                          gboolean              value);
gboolean  gowl_hotkey_get_context        (const gchar          *name,
                                          gboolean             *value);
gboolean  gowl_hotkey_clear_context      (const gchar          *name);
GStrv     gowl_hotkey_list_contexts      (void);

gboolean  gowl_hotkey_add_yaml           (const gchar          *id,
                                          const gchar          *yaml,
                                          GError              **error);
gboolean  gowl_hotkey_remove_yaml        (const gchar          *id);
void      gowl_hotkey_clear_sources      (void);

gboolean  gowl_hotkey_define             (const gchar          *id,
                                          const gchar          *key,
                                          GowlHotkeyFunc        func,
                                          gpointer              user_data,
                                          GDestroyNotify        destroy,
                                          GError              **error);
gboolean  gowl_hotkey_undefine           (const gchar          *id);

/* ── Talking to the running module ─────────────────────────────────── */

gboolean  gowl_hotkey_send               (GowlCompositor       *compositor,
                                          const gchar          *keys);
gboolean  gowl_hotkey_send_text          (GowlCompositor       *compositor,
                                          const gchar          *text);
gboolean  gowl_hotkey_set_mode           (GowlCompositor       *compositor,
                                          const gchar          *mode);
gboolean  gowl_hotkey_set_suspended      (GowlCompositor       *compositor,
                                          gboolean              suspended);
gchar    *gowl_hotkey_call_function      (GowlCompositor       *compositor,
                                          const gchar          *name,
                                          const gchar * const  *args);
gchar    *gowl_hotkey_command            (GowlCompositor       *compositor,
                                          const gchar          *line);

G_END_DECLS

#endif /* GOWL_HOTKEY_H */
