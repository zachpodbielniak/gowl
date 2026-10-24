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
 * gowl-hotkey-engine.h - What a key, button or wheel notch means.
 *
 * The engine is the AutoHotkey keyboard hook: given one input event and
 * the hotkeys in force, it says whether the event is swallowed and what
 * happens because of it -- hotkeys to fire, keys or buttons to put back
 * (a long-press key released early, a remap target).  It keeps what
 * that takes: which inputs are held and how each was decided, prefix
 * keys, pending long presses and double taps, the suspend switch, the
 * key history, the last hotkey.
 *
 * No compositor, no clock: the caller passes the time, so the tests
 * drive it with a fake one.  The caller also answers "may this hotkey
 * fire now?" (its context, and how many of it are running) through a
 * callback, asked only of hotkeys that otherwise match.
 */

#ifndef GOWL_HOTKEY_ENGINE_H
#define GOWL_HOTKEY_ENGINE_H

#include <glib.h>

#include "gowl-hotkey-model.h"

G_BEGIN_DECLS

#define HK_HISTORY_SIZE (64)

typedef enum {
	HK_OUT_FIRE,      /* run @hotkey's actions */
	HK_OUT_KEY,       /* put a key event back: @keycode, or by @keysym */
	HK_OUT_BUTTON     /* put a button event back */
} HkOutKind;

typedef struct {
	HkOutKind  kind;
	HkHotkey  *hotkey;    /* FIRE: a reference, dropped by hk_out_clear() */
	guint32    keycode;   /* KEY: the evdev keycode, or 0 to find @keysym's */
	guint32    keysym;
	guint32    button;    /* BUTTON */
	gboolean   pressed;
	guint      mods;      /* KEY/BUTTON: modifiers to hold around a press
	                         (a remap target's own), #GowlKeyMod bits */
	const gchar *why;     /* "replay", "remap", "hotkey": for the log */
} HkOut;

/* One input event, as the engine sees it */
typedef struct {
	GowlHotkeyInputKind kind;   /* KEY, BUTTON or WHEEL */
	guint32  keycode;           /* KEY: evdev */
	guint32  keysym;            /* KEY: with modifiers applied */
	guint32  base_keysym;       /* KEY: with none */
	guint32  button;            /* BUTTON: evdev; WHEEL: GOWL_BUTTON_WHEEL_* */
	guint    mods;              /* #GowlKeyMod held, locks removed */
	gboolean pressed;           /* WHEEL: always TRUE */
	gboolean synthetic;         /* injected by someone (a KVM, a macro) */
	gint     own_level;         /* -1, or the send level of our own injection */
	gpointer focused;           /* for the history only */
} HkEvent;

/* A line of `hotkey-history' */
typedef struct {
	gint64   time_ms;
	GowlHotkeyInputKind kind;
	guint32  code;              /* keycode or button */
	guint32  keysym;
	gboolean pressed;
	gboolean synthetic;
	gboolean own;
	gboolean consumed;
	gchar   *fired;             /* id of a hotkey it fired, or NULL */
} HkHistoryEntry;

typedef gboolean (*HkAllowFunc) (HkHotkey *hk,
                                 gpointer  user_data);

typedef struct _HkEngine HkEngine;

HkEngine *hk_engine_new             (void);
void      hk_engine_free            (HkEngine     *e);
void      hk_engine_set_hotkeys     (HkEngine     *e,
                                     GPtrArray    *hotkeys);
void      hk_engine_set_allow_func  (HkEngine     *e,
                                     HkAllowFunc   func,
                                     gpointer      user_data);
void      hk_engine_set_accept_synthetic (HkEngine *e,
                                     gboolean      accept);
void      hk_engine_set_suspended   (HkEngine     *e,
                                     gboolean      suspended);
gboolean  hk_engine_get_suspended   (HkEngine     *e);
void      hk_engine_set_swallow     (HkEngine     *e,
                                     gboolean      swallow);

gboolean  hk_engine_input           (HkEngine     *e,
                                     const HkEvent *ev,
                                     const gchar  *mode,
                                     gint64        now_ms,
                                     GArray       *out);
gint64    hk_engine_next_deadline   (HkEngine     *e);
void      hk_engine_tick            (HkEngine     *e,
                                     gint64        now_ms,
                                     GArray       *out);
void      hk_engine_reset           (HkEngine     *e,
                                     GArray       *out);

gboolean  hk_engine_key_is_down     (HkEngine     *e,
                                     const GowlHotkeyInput *input);
guint     hk_engine_n_held          (HkEngine     *e);
const gchar *hk_engine_get_prior    (HkEngine     *e,
                                     gint64       *time_ms);
GPtrArray *hk_engine_get_history    (HkEngine     *e);

GArray   *hk_out_new                (void);
void      hk_out_clear              (GArray       *out);

G_END_DECLS

#endif /* GOWL_HOTKEY_ENGINE_H */
