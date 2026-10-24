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
 * gowl-hotkey-model.h - The hotkey module's configuration, as data.
 *
 * A `hotkey:' section (the config's, a C config's, one added at run
 * time) is read into an HkSet: hotkeys (remaps are hotkeys too), modes,
 * named contexts and window groups, hotstrings, timers, event hooks and
 * functions, each hotkey holding the list of actions it runs.  Pure
 * GLib, yaml-glib and the filter language -- no compositor -- so the
 * tests read the shipped examples straight into a set.
 *
 * A problem costs the one thing that is wrong: it is recorded in the
 * set's `problems' and everything else still loads.
 */

#ifndef GOWL_HOTKEY_MODEL_H
#define GOWL_HOTKEY_MODEL_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include <yaml-glib.h>

#include "hotkey/gowl-hotkey.h"
#include "util/gowl-filter.h"

G_BEGIN_DECLS

/* Defaults, in milliseconds */
#define HK_DEFAULT_DOUBLE_MS    (300)
#define HK_DEFAULT_HOLD_MS      (500)
#define HK_DEFAULT_TIMEOUT_MS   (60000)
#define HK_MAX_LEVEL            (100)

/* ── Actions ───────────────────────────────────────────────────────── */

typedef enum {
	HK_ACT_ACTION,      /* a keybind action by nick: {action: spawn, arg: gst} */
	HK_ACT_COMMAND,     /* a gowl command line: {command: "view 4"} */
	HK_ACT_RPC,         /* a wire-protocol method: {rpc: window.center, params: {}} */
	HK_ACT_WINDOW,      /* sugar over window.* methods: {window: float} */
	HK_ACT_MACRO,       /* {macro: NAME, args: [...]} */
	HK_ACT_SHELL,       /* {shell: "cmd | cmd"} -- through /bin/sh -c */
	HK_ACT_RUN,         /* {run: [argv...]} or {run: "prog arg"} -- no shell */
	HK_ACT_C,           /* {c: "code"} or {c: {file: path}} -- crispy */
	HK_ACT_CALL,        /* {call: NAME, args: [...]} */
	HK_ACT_SEND,        /* {send: "^c{Enter}"} -- AutoHotkey Send syntax */
	HK_ACT_SEND_TEXT,   /* {send-text: "literal"} */
	HK_ACT_MODE,        /* {mode: resize} / {mode: default} */
	HK_ACT_SUSPEND,     /* {suspend: on|off|toggle} */
	HK_ACT_RELOAD,      /* {reload: true} */
	HK_ACT_MOUSE,       /* {mouse: click|move|drag|wheel, ...} */
	HK_ACT_CLIPBOARD,   /* {clipboard: "text"} */
	HK_ACT_NOTIFY,      /* {notify: "text"} -- a toast */
	HK_ACT_SLEEP,       /* {sleep: 250} */
	HK_ACT_CUSTOM,      /* {custom: ARG} -- the embedder's custom action (cmacs: Elisp) */
	HK_ACT_INPUT,       /* {input: {...}} -- start an input hook */
	HK_ACT_CONTEXT,     /* {context: NAME, value: true} -- set a boolean context */
	HK_ACT_LOCK,        /* {capslock: on|off|toggle|always-on|always-off} (numlock too) */
	HK_ACT_HOTKEY       /* {hotkey: enable|disable|toggle, id: ID} */
} HkActKind;

typedef enum {
	HK_TOGGLE_OFF    = 0,
	HK_TOGGLE_ON     = 1,
	HK_TOGGLE_TOGGLE = 2,
	HK_TOGGLE_ALWAYS_ON  = 3,
	HK_TOGGLE_ALWAYS_OFF = 4,
	HK_TOGGLE_NORMAL     = 5     /* lock: stop forcing it */
} HkToggle;

typedef struct {
	HkActKind   kind;
	gchar      *name;        /* action nick, macro, function, method, mode,
	                            window/mouse verb, lock name, hotkey id */
	gchar      *text;        /* arg, command line, shell line, keys, text,
	                            code or file, toast, custom argument */
	gchar     **argv;        /* run's argv; macro, call and input args */
	JsonObject *params;      /* rpc/window: the method's parameters */
	gchar      *target;      /* window/mouse: a filter for the window to
	                            act on, instead of the focused one */
	gboolean    wait;        /* shell/run/macro: finish first */
	guint       timeout_ms;  /* shell/run/c: budget */
	guint       ms;          /* sleep: how long; send: per-key delay */
	gint        send_mode;   /* send: -1 default, 0 input, 1 event */
	gint        level;       /* send: send level, -1 the module's */
	gboolean    is_file;     /* c: @text is a path */
	gchar      *cflags;      /* c: extra compiler flags */
	HkToggle    toggle;      /* suspend, lock, hotkey, context */
	/* mouse */
	guint32     button;      /* evdev; 0 for none */
	gint        count;
	gdouble     x;
	gdouble     y;
	gdouble     x2;          /* drag: where to */
	gdouble     y2;
	gboolean    has_xy;
	gboolean    relative;    /* x,y from where the pointer is */
	gboolean    window_rel;  /* x,y relative to the target window */
	gint        wheel;       /* wheel: notches, + down / - up */
	gboolean    horizontal;
} HkAction;

void hk_action_free (HkAction *action);

/* ── Conditions ────────────────────────────────────────────────────── */

/* Where a hotkey may fire: every filter given must hold.  @when is the
 * focused window, @over the window under the pointer (AutoHotkey's
 * MouseIsOver), @exists / @absent whether any window matches. */
typedef struct {
	gchar      *when_text;
	GowlFilter *when;
	gchar      *over_text;
	GowlFilter *over;
	gchar      *exists_text;
	GowlFilter *exists;
	gchar      *absent_text;
	GowlFilter *absent;
} HkCondition;

void     hk_condition_clear   (HkCondition       *cond);
gboolean hk_condition_is_empty (const HkCondition *cond);

/* A named context or window group */
typedef struct {
	gchar       *name;
	HkCondition  cond;
	gchar       *origin;
} HkNamedCondition;

/* ── Hotkeys ───────────────────────────────────────────────────────── */

typedef struct {
	gint            ref;
	gchar          *id;
	gchar          *key;          /* as written */
	gchar          *origin;       /* "config", a source id, "runtime" */
	guint           order;        /* load order: later wins a tie */
	GowlHotkeySpec  spec;
	gchar          *context;      /* a named context, or NULL */
	HkCondition     cond;         /* its own conditions */
	gchar          *mode;         /* NULL: the default mode; "*": every mode */
	guint           double_ms;    /* 0: not a double tap */
	guint           hold_ms;      /* 0: not a long press */
	gint            priority;
	gint            level;        /* input level, 0..100 */
	gboolean        suspend_exempt;
	guint           max_running;  /* 0: no limit */
	gboolean        enabled;
	gboolean        is_remap;
	GowlHotkeyInput remap_to;
	guint           remap_mods;
	gchar          *description;
	GPtrArray      *actions;      /* HkAction* */
} HkHotkey;

HkHotkey *hk_hotkey_new   (void);
HkHotkey *hk_hotkey_ref   (HkHotkey *hk);
void      hk_hotkey_unref (HkHotkey *hk);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(HkHotkey, hk_hotkey_unref)

/* ── Everything else ───────────────────────────────────────────────── */

typedef struct {
	gchar     *name;
	guint      timeout_ms;    /* 0: no timeout */
	gboolean   oneshot;       /* back to default after one hotkey */
	gboolean   swallow;       /* keys with no hotkey in it are dropped */
	gboolean   toast;         /* say so when entered */
	gchar     *label;         /* what the toast says */
	GPtrArray *on_enter;      /* HkAction* */
	GPtrArray *on_exit;
} HkMode;

typedef struct {
	gchar      *id;
	gchar      *raw;          /* `:*:btw::by the way', as written */
	gchar      *options;      /* `*' */
	gchar      *abbrev;       /* `btw' */
	gchar      *replacement;  /* `by the way' */
	gchar      *context;
	HkCondition cond;
	GPtrArray  *actions;      /* X hotstrings: what they run */
	gchar      *origin;
} HkHotstring;

typedef struct {
	gchar     *id;
	guint      period_ms;
	gboolean   once;
	guint      delay_ms;      /* before the first run; default period */
	gboolean   enabled;
	gchar     *context;
	HkCondition cond;
	GPtrArray *actions;
	gchar     *origin;
} HkTimer;

typedef struct {
	gchar      *id;
	gchar      *event;        /* focus, window-open, window-close, title,
	                             tag, monitor, mode, startup, lock, unlock */
	HkCondition cond;         /* `filter:' is `when:' on the event's window */
	GPtrArray  *actions;
	gchar      *origin;
} HkHook;

typedef struct {
	gchar     *name;
	GPtrArray *actions;
	gchar     *origin;
} HkFunction;

typedef struct {
	gint        ref;
	GPtrArray  *hotkeys;      /* HkHotkey*, remaps and mode hotkeys too */
	GHashTable *modes;        /* name -> HkMode* */
	GHashTable *contexts;     /* name -> HkNamedCondition* */
	GHashTable *groups;       /* name -> HkNamedCondition* */
	GPtrArray  *hotstrings;   /* HkHotstring* */
	GPtrArray  *timers;       /* HkTimer* */
	GPtrArray  *hooks;        /* HkHook* */
	GHashTable *functions;    /* name -> HkFunction* */
	GPtrArray  *scripts;      /* gchar*: crispy hotkey scripts, absolute */
	GPtrArray  *problems;     /* gchar*: one line each */
} HkSet;

HkSet    *hk_set_ref               (HkSet       *set);
void      hk_set_unref             (HkSet       *set);
HkHotkey *hk_set_find_hotkey       (HkSet       *set,
                                    const gchar *id);
guint     hk_set_count_enabled     (HkSet       *set);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(HkSet, hk_set_unref)

/* ── Loading ───────────────────────────────────────────────────────── */

typedef struct _HkLoader HkLoader;

HkLoader *hk_loader_new        (void);
void      hk_loader_free       (HkLoader    *loader);
void      hk_loader_add_node   (HkLoader    *loader,
                                YamlNode    *section,
                                const gchar *origin,
                                const gchar *base_dir);
gboolean  hk_loader_add_text   (HkLoader    *loader,
                                const gchar *yaml,
                                const gchar *origin,
                                GError     **error);
HkSet    *hk_loader_finish     (HkLoader    *loader);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(HkLoader, hk_loader_free)

/* One action or one hotkey entry, from text (`hotkey-add', the tests). */
GPtrArray *hk_parse_actions    (YamlNode    *node,
                                const gchar *where,
                                GPtrArray   *problems);

/* The field names every filter here may use. */
const gchar * const *hk_window_fields (void);

/* For the IPC list: a hotkey as JSON. */
void      hk_hotkey_to_json    (const HkHotkey *hk,
                                JsonBuilder    *b);
const gchar *hk_action_kind_name (HkActKind kind);

G_END_DECLS

#endif /* GOWL_HOTKEY_MODEL_H */
