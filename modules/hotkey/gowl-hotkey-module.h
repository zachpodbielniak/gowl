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
 * gowl-hotkey-module.h - The hotkey module's own state, shared by its
 * files: the module (input, configuration, IPC), the runner (actions,
 * typing, modes) and the D-Bus service.  Not installed.
 *
 * Threads: everything here runs on the compositor thread except the C
 * builds, which run on a worker and come back through post().  Nothing
 * here may wait on GLib's default main context: under cmacs that is
 * Emacs's thread.
 */

#ifndef GOWL_HOTKEY_MODULE_H
#define GOWL_HOTKEY_MODULE_H

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include <wayland-server-core.h>

#include "module/gowl-module.h"
#include "core/gowl-compositor.h"
#include "config/gowl-config.h"
#include "ipc/gowl-ipc.h"
#include "util/gowl-subprocess.h"

#include "gowl-hotkey-model.h"
#include "gowl-hotkey-engine.h"
#include "gowl-hotkey-send.h"
#include "gowl-hotkey-script.h"

G_BEGIN_DECLS

#define HK_PREFIX               "hotkey-"
#define HK_DEFAULT_SUSPEND_KEY  "Super+Shift+Pause"
#define HK_DEFAULT_C_TIMEOUT_MS (2000)
#define HK_MAX_CALL_DEPTH       (16)
#define HK_MAX_RUNNERS          (64)

#define GOWL_TYPE_MODULE_HOTKEY (gowl_module_hotkey_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleHotkey, gowl_module_hotkey,
                     GOWL, MODULE_HOTKEY, GowlModule)

typedef struct _HkRunner HkRunner;
typedef struct _HkPlay   HkPlay;
typedef struct _HkDbus   HkDbus;

/* A frame of a runner: one action list and where it is in it */
typedef struct {
	GPtrArray *actions;     /* HkAction*, a ref */
	guint      index;
	gchar    **args;        /* a `call''s arguments for the frame */
} HkFrame;

/* One running action list -- AutoHotkey's "thread" */
struct _HkRunner {
	GowlModuleHotkey       *self;
	guint                   serial;
	gchar                  *id;          /* hotkey, timer, hook, function */
	gchar                  *trigger;     /* hotkey, timer, event, ... */
	gchar                  *detail;
	gchar                  *key;         /* the hotkey's key, for {{key}} */
	guint                   focused_id;  /* the window when it fired */
	GPtrArray              *frames;      /* HkFrame* */
	gchar                  *input;       /* the last input hook's text */
	gchar                  *result;      /* hotkey-call's reply */
	struct wl_event_source *timer;       /* sleep */
	GowlSubprocess         *proc;        /* shell/run with wait */
	guint                   macro_id;    /* macro with wait */
	gchar                  *c_key;       /* a C build waited for */
	gboolean                waiting;     /* something above is pending */
	gboolean                counted;     /* in the running counts */
	void                  (*done) (HkRunner *r, gpointer data);
	gpointer                done_data;
};

/* A queued Send */
struct _HkPlay {
	HkSendPlan *plan;
	guint       index;
	gboolean    event_mode;
	guint       delay_ms;
	gint        level;
	guint       focused_id;
	gboolean    started;
	guint32     lifted[8];      /* modifier keycodes taken up for it */
	guint       n_lifted;
	HkRunner   *runner;         /* told when done, or NULL */
};

/* A running timer from `timers:' */
typedef struct {
	GowlModuleHotkey       *self;
	HkTimer                *def;        /* borrowed from the set */
	gchar                  *id;
	struct wl_event_source *source;
	gboolean                enabled;
	guint                   fired;
} HkTimerRun;

/* An input hook (AutoHotkey's InputHook) */
typedef struct {
	gboolean   active;
	gchar     *name;
	GString   *text;
	guint      max_len;          /* 0: none */
	gboolean   visible;          /* keys still reach the window */
	gboolean   case_sensitive;
	GPtrArray *end_keys;         /* GowlHotkeyInput* */
	gchar    **match;
	struct wl_event_source *timer;
	HkRunner  *runner;           /* the action waiting, or NULL */
	GHashTable *swallowed;       /* keycodes whose release is ours too */
} HkInputHook;

/* A crispy hotkey script loaded from `scripts:' */
typedef struct {
	gchar       *path;
	gchar       *so_path;
	HkCFiniFunc  fini;
	gchar      **funcs;          /* what its init registered */
	gchar      **contexts;
	gchar      **sources;
	gchar       *error;
	gboolean     building;
} HkScript;

struct _GowlModuleHotkey {
	GowlModule              parent_instance;

	GowlCompositor         *compositor;     /* weak */
	GowlConfig             *config;         /* ref: whose sections are loaded */
	gulong                  reloaded_handler;
	gboolean                active;
	gboolean                started;

	HkSet                  *set;
	HkEngine               *engine;
	guint                   generation;     /* libgowl's, when loaded */
	GPtrArray              *runtime;        /* GowlHotkeySource*: hotkey-add */
	GHashTable             *enabled;        /* id -> GINT_TO_POINTER(0|1) */
	GPtrArray              *warnings;       /* gchar*: hotstring translation */
	gboolean                hotstrings_installed;

	/* settings */
	gchar                  *suspend_text;
	guint                   suspend_mods;
	guint                   suspend_sym;
	guint                   key_delay_ms;
	gboolean                event_mode;     /* send-mode: event */
	gint                    send_level;
	gboolean                accept_synthetic;
	gboolean                toast;
	gboolean                log;
	guint                   c_timeout_ms;
	gchar                 **setting_scripts;
	gboolean                hotstrings;     /* hand hotstrings to gabbrev */
	gboolean                dbus;           /* own org.gowl.Hotkey1 */

	/* input */
	gint                    injecting;      /* depth of our own injection */
	gint                    inject_level;
	GHashTable             *swallowed;      /* keycodes the module took */
	gboolean                caps_before;    /* the lock before a key */
	gboolean                num_before;
	HkToggle                caps_force;     /* always-on/off, else NORMAL */
	HkToggle                num_force;
	gint                    restore_caps;   /* -1, or the value to put back */
	gint                    restore_num;
	struct wl_event_source *restore_idle;
	gdouble                 wheel_accum[2];
	GHashTable             *ev_fields;      /* the focused window's, per event */
	gpointer                ev_focused;
	/* A remap onto a modifier holds it in the keyboard group's state; a
	 * device's own modifier change is copied onto the group and would
	 * drop it, so while one is held a listener puts it back */
	guint8                  overlay_count[8]; /* per GowlKeyMod bit */
	struct wl_listener      group_mods;
	gboolean                group_mods_attached;
	gboolean                in_overlay;

	/* time */
	struct wl_event_source *deadline_timer;
	struct wl_event_source *mode_timer;
	gchar                  *mode;           /* the hotkey mode we entered */

	/* actions */
	GQueue                 *fires;          /* HkHotkey* refs, with ids */
	struct wl_event_source *fire_idle;
	GPtrArray              *runners;        /* HkRunner* */
	guint                   next_serial;
	GHashTable             *running;        /* id -> GUINT running */
	GQueue                 *plays;          /* HkPlay* */
	struct wl_event_source *play_timer;
	gboolean                playing;
	GHashTable             *elisp_funcs;    /* name -> form (hotkey-func-define) */
	guint                   macro_listener;
	HkInputHook             input;
	GPtrArray              *timers;         /* HkTimerRun* */

	/* C */
	GHashTable             *c_funcs;        /* key -> HkCActionFunc */
	GHashTable             *c_building;     /* key -> TRUE */
	GHashTable             *c_faulted;      /* key -> reason */
	GHashTable             *scripts;        /* path -> HkScript* */
	gchar                  *cache_dir;
	GMutex                  post_lock;
	gint                    post_fd;
	GQueue                 *posted;
	struct wl_event_source *post_source;
	gint                    watch_fd;
	struct wl_event_source *watch_source;
	GHashTable             *watch_dirs;     /* dir -> wd */

	/* surfaces */
	GowlIpc                *ipc;            /* weak: where methods are */
	HkDbus                 *dbus_service;

	/* status */
	guint64                 n_fired;
	guint64                 n_failed;
	gchar                  *last_error;

	struct wl_listener      display_destroy;
};

/* gowl-module-hotkey.c */
void      hk_log            (GowlModuleHotkey *self,
                             const gchar      *format,
                             ...) G_GNUC_PRINTF(2, 3);
void      hk_fail           (GowlModuleHotkey *self,
                             const gchar      *format,
                             ...) G_GNUC_PRINTF(2, 3);
void      hk_announce       (GowlModuleHotkey *self,
                             const gchar      *toast,
                             const gchar      *format,
                             ...) G_GNUC_PRINTF(3, 4);
GowlClient *hk_client_by_id (GowlModuleHotkey *self,
                             guint             id);
GHashTable *hk_fields_of    (GowlModuleHotkey *self,
                             GowlClient       *c);
GowlClient *hk_find_window  (GowlModuleHotkey *self,
                             const gchar      *filter_text,
                             GowlClient       *after,
                             GError          **error);
void      hk_reload         (GowlModuleHotkey *self);
gchar    *hk_dispatch       (GowlModuleHotkey *self,
                             const gchar      *verb,
                             const gchar      *args);
void      hk_set_suspended  (GowlModuleHotkey *self,
                             gboolean          suspended,
                             const gchar      *why);
gboolean  hk_set_hotkey_enabled (GowlModuleHotkey *self,
                             const gchar      *id,
                             HkToggle          how);
void      hk_set_lock       (GowlModuleHotkey *self,
                             gboolean          caps,
                             HkToggle          how);
void      hk_arm_deadline   (GowlModuleHotkey *self);
void      hk_overlay_hold   (GowlModuleHotkey *self,
                             guint             modbit,
                             gboolean          down);
void      hk_overlay_detach (GowlModuleHotkey *self);

/* gowl-hotkey-runner.c */
void      hk_runner_init    (GowlModuleHotkey *self);
void      hk_runner_finish  (GowlModuleHotkey *self);
void      hk_queue_fire     (GowlModuleHotkey *self,
                             HkHotkey         *hk);
HkRunner *hk_run_actions    (GowlModuleHotkey *self,
                             GPtrArray        *actions,
                             const gchar      *id,
                             const gchar      *trigger,
                             const gchar      *detail,
                             const gchar * const *args,
                             gboolean          count);
gchar    *hk_call_function_now (GowlModuleHotkey *self,
                             const gchar      *name,
                             const gchar * const *args,
                             const gchar      *trigger,
                             GError          **error);
void      hk_cancel_all     (GowlModuleHotkey *self);
gboolean  hk_send           (GowlModuleHotkey *self,
                             const gchar      *keys,
                             gboolean          text,
                             HkRunner         *runner,
                             GError          **error);
void      hk_inject_key     (GowlModuleHotkey *self,
                             guint32           keycode,
                             gboolean          pressed,
                             gint              level);
void      hk_inject_button  (GowlModuleHotkey *self,
                             guint32           button,
                             gboolean          pressed,
                             gint              level);
void      hk_play_outputs   (GowlModuleHotkey *self,
                             GArray           *out);
gboolean  hk_enter_mode     (GowlModuleHotkey *self,
                             const gchar      *name,
                             GError          **error);
void      hk_mode_changed   (GowlModuleHotkey *self,
                             const gchar      *mode);
void      hk_after_fire     (GowlModuleHotkey *self);
guint     hk_running_count  (GowlModuleHotkey *self,
                             const gchar      *id);
void      hk_input_start    (GowlModuleHotkey *self,
                             const gchar      *name,
                             const gchar      *options,
                             const gchar * const *end_and_match,
                             HkRunner         *runner);
gboolean  hk_input_key      (GowlModuleHotkey *self,
                             guint32           keycode,
                             guint32           keysym,
                             guint32           base_keysym,
                             guint             mods,
                             gboolean          pressed);
void      hk_input_stop     (GowlModuleHotkey *self,
                             const gchar      *reason);
void      hk_post           (GowlModuleHotkey *self,
                             void            (*func) (GowlModuleHotkey *self,
                                                      gpointer          data),
                             gpointer          data,
                             GDestroyNotify    destroy);
void      hk_post_open      (GowlModuleHotkey *self);
void      hk_post_close     (GowlModuleHotkey *self);
void      hk_scripts_sync   (GowlModuleHotkey *self);
void      hk_scripts_unload (GowlModuleHotkey *self);
void      hk_gabbrev_var    (gpointer          request,
                             gpointer          user_data);

/* gowl-hotkey-dbus.c */
HkDbus   *hk_dbus_new       (GowlModuleHotkey *self);
void      hk_dbus_free      (HkDbus           *dbus);
void      hk_dbus_emit      (HkDbus           *dbus,
                             const gchar      *signal,
                             const gchar      *a,
                             const gchar      *b);

G_END_DECLS

#endif /* GOWL_HOTKEY_MODULE_H */
