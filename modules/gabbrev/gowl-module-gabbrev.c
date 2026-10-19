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
 * gowl-module-gabbrev.c - gabbrev, the text expander (opt-in).
 *
 * Type `;today' in any window and it becomes `2026-10-19'.  espanso's
 * idea and espanso's configuration format (a superset of it, see
 * docs/gabbrev.org), done in the compositor -- which is the one place
 * that sees every key on its way to a window, knows which window it is
 * going to, and can type into that window without any of the tricks a
 * client-side expander needs (evdev, uinput, virtual keyboards).
 *
 * Loaded only when asked for: `modules: gabbrev: {enabled: true}' in
 * standalone gowl; cmacs --gowl loads it always.
 *
 * How a keystroke travels:
 *
 *   filter_key (before every bind):     the toggle and search keys, and
 *       Backspace right after an expansion (undo) are taken here.
 *   key_forwarded (after the window got it): the key becomes a
 *       character for the engine (gowl-gabbrev-engine.c), which says
 *       whether it completed a trigger.  A shortcut, an arrow, a click
 *       or another window forgets what was typed.
 *
 * A completed trigger becomes a JOB.  The job renders the match
 * (gowl-gabbrev-render.c): variables resolve one at a time, the ones
 * that need the desktop here -- shell, clipboard, selection, pickers, C
 * -- and registered ones (cmacs's `elisp') through libgowl's registry.
 * When the text is ready the job queues STEPS -- Backspaces for the
 * trigger, the text as keys or a paste, the separator back, arrows for
 * the cursor hint, the clipboard put back -- which a wl_event_loop
 * timer plays.  Nothing ever blocks the compositor thread: subprocesses
 * are watched on the event loop (gowl-subprocess.c), clipboard reads are
 * asynchronous, and C variables compile on a worker.
 *
 * Under cmacs this matters twice over: the event loop runs on the gowl
 * dispatch thread, and answers from Lisp arrive on Emacs's.  Answers
 * from any thread are POSTED: queued under a mutex, an eventfd wakes
 * the event loop, and everything continues there.
 *
 * IPC (replies "OK ...", "OK <json>" or "ERROR ..."):
 *
 *   gabbrev-status                 state, counts, problems (JSON)
 *   gabbrev-list [FILTER]          matches (JSON)
 *   gabbrev-on / -off / -toggle    expand or not
 *   gabbrev-reload                 read every source again
 *   gabbrev-load PATH              add a file, directory or espanso dir
 *   gabbrev-add YAML               add matches (one line: flow YAML/JSON)
 *   gabbrev-remove ID              drop runtime matches with this id
 *   gabbrev-clear                  drop every runtime source
 *   gabbrev-expand TRIGGER|ID      type a match into the focused window
 *   gabbrev-insert TEXT            type TEXT (keys or paste, as configured)
 *   gabbrev-render [--id ID] [--custom FORM] TRIGGER|ID
 *                                  render only: the result goes to the
 *                                  event stream (and a custom action)
 *   gabbrev-test TEXT              which match TEXT would fire (JSON)
 *   gabbrev-search                 pick a match and type it
 *   gabbrev-resolve ID JSON        answer variable request ID
 *   gabbrev-resolve-error ID TEXT  fail variable request ID
 *   gabbrev-cancel                 drop every running expansion
 *   gabbrev-builtin                the built-in matches, as YAML
 *
 * Events: EVENT gabbrev expanded ID, failed ID REASON, on, off,
 * reloaded N, rendered ID JSON.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-gabbrev"

#include <errno.h>
#include <glib-object.h>
#include <gmodule.h>
#include <json-glib/json-glib.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-input-observer.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "core/gowl-compositor.h"
#include "core/gowl-core-private.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"
#include "core/gowl-seat.h"
#include "config/gowl-config.h"
#include "util/gowl-subprocess.h"
#include "util/gowl-filter.h"
#include "gabbrev/gowl-gabbrev.h"
#include "gabbrev/gowl-gabbrev-private.h"

#include "gowl-gabbrev-model.h"
#include "gowl-gabbrev-engine.h"
#include "gowl-gabbrev-render.h"
#include "gowl-gabbrev-cvar.h"

#define GABBREV_PREFIX            "gabbrev-"
#define GABBREV_MAX_JOBS          (8)
#define GABBREV_UNDO_MS           (8000)
#define GABBREV_DOUBLE_TAP_MS     (300)
#define GABBREV_SHELL_TIMEOUT_MS  (10000)
#define GABBREV_PICKER_TIMEOUT_MS (300000)
#define GABBREV_C_TIMEOUT_MS      (500)
#define GABBREV_GRAB_SETTLE_MS    (150)
#define GABBREV_MAX_FILE_BYTES    (1024 * 1024)

/* Never expanded into, unless the deny filter is changed: password
   managers, pinentry, polkit, and any window that says it wants a
   password.  The recorder keeps the same list for the same reason. */
#define GABBREV_DEFAULT_DENY \
	"app-id ~ '(?i)(keepassxc|pinentry|polkit|bitwarden|1password|" \
	"seahorse|kwalletd|gcr-prompter|lxqt-policykit)' " \
	"or title ~ '(?i)(password|passphrase|pin entry|unlock)'"

/* Terminals paste with Ctrl+Shift+V and copy with Ctrl+Shift+C: the
   one patch of espanso's every Linux user needs. */
#define GABBREV_DEFAULT_TERMINALS \
	"gst,foot,footclient,kitty,alacritty,Alacritty,org.wezfurlong.wezterm," \
	"com.mitchellh.ghostty,org.gnome.Terminal,org.gnome.Ptyxis,konsole," \
	"org.kde.konsole,xterm,XTerm,st,st-256color,urxvt,URxvt,tilix," \
	"com.gexperts.Tilix,terminator,xfce4-terminal,io.elementary.terminal," \
	"Kgx,org.gnome.Console,contour,rio,wezterm"

#define GOWL_TYPE_MODULE_GABBREV (gowl_module_gabbrev_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleGabbrev, gowl_module_gabbrev,
                     GOWL, MODULE_GABBREV, GowlModule)

typedef struct _Job Job;

/* ── Steps: what the player does, in order ─────────────────────────── */

typedef enum {
	STEP_KEY = 0,       /* tap keysym (or keycode) with mods */
	STEP_TEXT,          /* type text as keys */
	STEP_SET_CLIPBOARD, /* text (or bytes with a mime type) */
	STEP_SAVE_CLIPBOARD,/* remember the clipboard (async; pauses) */
	STEP_RESTORE_CLIPBOARD,
	STEP_WAIT,
	STEP_FOCUS,         /* give the target keyboard focus */
	STEP_CALL           /* a function, in sequence (may pause) */
} StepKind;

typedef gboolean (*StepFunc) (GowlModuleGabbrev *self, Job *job,
                              gpointer data);

typedef struct {
	StepKind  kind;
	Job      *job;        /* a reference */
	guint32   keycode;    /* 0: from keysym */
	guint32   keysym;
	guint32   mods;
	gchar    *text;
	GBytes   *bytes;
	gchar    *mime;
	guint     ms;
	StepFunc  func;       /* STEP_CALL: FALSE pauses the player */
	gpointer  data;
	GDestroyNotify destroy;
} Step;

/* ── Jobs ──────────────────────────────────────────────────────────── */

typedef enum {
	ORIGIN_TYPED = 0,   /* a trigger was typed */
	ORIGIN_EXPAND,      /* gabbrev-expand / search: nothing to rub out */
	ORIGIN_RENDER       /* gabbrev-render: nothing is typed at all */
} JobOrigin;

struct _Job {
	gint                ref_count;
	guint               id;
	GowlModuleGabbrev  *self;         /* not a reference: jobs die first */
	JobOrigin           origin;
	GabbrevSet         *set;
	GabbrevMatch       *match;
	gchar              *typed;
	guint               n_delete;
	gchar              *trailing;
	GabbrevTypedCase    typed_case;
	GHashTable         *captures;
	guint               target_id;
	gchar              *app_id;
	gchar              *title;
	GHashTable         *fields;       /* the window, at the trigger */
	gboolean            terminal;
	GabbrevOptions      opts;         /* the window's effective options */
	GabbrevRender      *render;
	gboolean            delete_first;
	gboolean            deleted;      /* the trigger is rubbed out */
	gboolean            grabbed;      /* text was selected for a grab */
	gboolean            injecting;    /* steps queued */
	gboolean            finished;
	gboolean            have_saved;
	gchar              *saved_clipboard;
	gint64              started_us;
	struct wl_event_source *timeout;
	gchar              *render_id;    /* ORIGIN_RENDER: the caller's id */
	gchar              *custom_form;  /* ORIGIN_RENDER: %r gets the text */
};

/* What the last expansion typed, for undo with Backspace. */
typedef struct {
	gboolean  valid;
	guint     target_id;
	guint     n_typed;      /* characters the replacement put in */
	gchar    *restore;      /* the trigger, to type back */
	gint64    when_us;
} LastExpansion;

/* One posted continuation: a function to run on the event loop. */
typedef struct {
	void     (*func) (GowlModuleGabbrev *self, gpointer data);
	gpointer   data;
	GDestroyNotify destroy;
} Posted;

/* A compiled `c' variable build in progress, and who waits for it. */
typedef struct {
	GowlModuleGabbrev *self;
	gchar             *key;
	GabbrevCvarSpec    spec;
	gchar             *cache_dir;
	gchar             *so_path;
	GError            *error;
	GPtrArray         *waiters;      /* GowlGabbrevRequest* refs */
} CvarBuild;

/* A source added at run time (gabbrev-add, gabbrev-load), by name, so
   adding under the same name again replaces it: cmacs keeps its Lisp
   matches as one source and its abbrev tables as another. */
typedef struct {
	gchar    *name;
	gboolean  is_file;
	gchar    *text;
} RuntimeSource;

/* A picker or prompt run, for choice, form and search. */
typedef struct _Picker Picker;

struct _GowlModuleGabbrev {
	GowlModule          parent_instance;

	GowlCompositor     *compositor;      /* weak */
	GowlConfig         *config;          /* ref: whose sections are loaded */
	gulong              reloaded_handler;
	gboolean            active;
	gboolean            on;

	/* what is loaded */
	GabbrevSet         *set;
	GabbrevEngine      *engine;
	guint               generation;      /* libgowl's, when last loaded */
	GPtrArray          *runtime;         /* RuntimeSource*: IPC, Lisp */

	/* settings (modules: gabbrev:) */
	gboolean            start_on;
	gboolean            builtin;
	gboolean            toast;
	gboolean            log;
	gboolean            expand_synthetic;
	gchar             **setting_files;
	gchar              *deny_text;
	GowlFilter         *deny;
	gchar             **terminals;
	gchar              *picker_command;  /* NULL: found at first use */
	gchar              *prompt_command;
	gchar              *cache_dir;
	guint               max_jobs;
	guint               c_timeout_ms;

	/* typing */
	guint               typing_target;   /* window the buffer belongs to */
	LastExpansion       last;
	guint32             tap_sym;         /* double-tap toggle tracking */
	gint64              tap_time_us;
	guint               tap_count;
	gboolean            tap_clean;       /* nothing else since the press */
	guint32             toggle_mods;     /* a chord toggle, or 0/0 */
	guint32             toggle_sym;
	guint32             toggle_tap_sym;  /* double-tap toggle: Alt_L, ... */
	guint32             toggle_tap_sym2; /* and its other side, or 0 */
	guint32             search_mods;
	guint32             search_sym;
	GHashTable         *swallowed;       /* keycodes whose press we took */

	/* jobs and steps */
	GPtrArray          *jobs;            /* Job* refs */
	guint               next_job_id;
	GQueue             *steps;           /* Step* */
	gboolean            paused;          /* a step waits for a callback */
	gboolean            waiting;         /* a WAIT step's timer is armed */
	gboolean            in_key;          /* inside the compositor's key
	                                        handling: type a moment later */
	struct wl_event_source *step_timer;

	/* posting from any thread */
	GMutex              post_lock;
	GQueue             *posted;          /* Posted* */
	gint                post_fd;
	struct wl_event_source *post_source;

	/* `c' variables */
	GHashTable         *cvar_funcs;      /* key -> GabbrevCvarFunc */
	GHashTable         *cvar_builds;     /* key -> CvarBuild* (in flight) */
	GHashTable         *cvar_faulted;    /* key -> reason */

	/* counters */
	guint64             n_expanded;
	guint64             n_failed;
	gchar              *last_error;

	struct wl_listener  display_destroy;
};

static void gab_observer_init (GowlInputObserverInterface   *iface);
static void gab_ipc_init      (GowlIpcHandlerInterface      *iface);
static void gab_startup_init  (GowlStartupHandlerInterface  *iface);
static void gab_shutdown_init (GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleGabbrev, gowl_module_gabbrev,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_INPUT_OBSERVER, gab_observer_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER, gab_ipc_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, gab_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, gab_shutdown_init))

static void job_fail       (GowlModuleGabbrev *self, Job *job, const gchar *why);
static void job_unref      (Job *job);
static void play_steps     (GowlModuleGabbrev *self);
static void reload         (GowlModuleGabbrev *self);

/* ── Small things ──────────────────────────────────────────────────── */

static gint64
now_us(void)
{
	return g_get_monotonic_time();
}

static void
gab_log(
	GowlModuleGabbrev *self,
	const gchar       *format,
	...
) G_GNUC_PRINTF(2, 3);

static void
gab_log(
	GowlModuleGabbrev *self,
	const gchar       *format,
	...
){
	va_list ap;
	g_autofree gchar *msg = NULL;

	if (!self->log)
		return;
	va_start(ap, format);
	msg = g_strdup_vprintf(format, ap);
	va_end(ap);
	g_message("gabbrev: %s", msg);
}

/* One event: `EVENT gabbrev ...' on the IPC stream, and the
   compositor's module-event signal, which is how an embedder with no
   socket (cmacs) hears it. */
static void
gab_event(
	GowlModuleGabbrev *self,
	const gchar       *format,
	...
) G_GNUC_PRINTF(2, 3);

static void
gab_event(
	GowlModuleGabbrev *self,
	const gchar       *format,
	...
){
	va_list ap;
	g_autofree gchar *msg = NULL;

	if (self->compositor == NULL)
		return;
	va_start(ap, format);
	msg = g_strdup_vprintf(format, ap);
	va_end(ap);
	g_strdelimit(msg, "\n", ' ');
	gowl_compositor_emit_module_event(self->compositor, "gabbrev", "%s", msg);
}

/* A notification: the bar's toast (when it is loaded) and the
   compositor's toast signal. */
static void
gab_notify(
	GowlModuleGabbrev *self,
	const gchar       *summary,
	const gchar       *body
){
	g_autofree gchar *s = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *line = NULL;

	if (self->compositor == NULL || !self->toast)
		return;
	if (self->set != NULL && !self->set->options.notifications)
		return;
	s = g_strdelimit(g_strdup(summary), "|\n", ' ');
	b = g_strdelimit(g_strdup(body != NULL ? body : ""), "|\n", ' ');
	line = g_strdup_printf("bar-notify %s|%s", s, b);
	g_free(gowl_compositor_run_command(self->compositor, line));
	g_signal_emit_by_name(self->compositor, "toast-requested",
	                      gowl_compositor_get_selected_monitor(self->compositor),
	                      s);
}

static GowlClient *
client_by_id(
	GowlModuleGabbrev *self,
	guint              id
){
	GList *l;

	if (self->compositor == NULL || id == 0)
		return NULL;
	for (l = gowl_compositor_get_clients(self->compositor); l != NULL;
	     l = l->next)
		if (gowl_client_get_id(GOWL_CLIENT(l->data)) == id)
			return GOWL_CLIENT(l->data);
	return NULL;
}

static gboolean
parse_bool_setting(
	const gchar *v,
	gboolean    *out
){
	if (v == NULL)
		return FALSE;
	if (g_ascii_strcasecmp(v, "true") == 0 || g_ascii_strcasecmp(v, "yes") == 0
	    || g_ascii_strcasecmp(v, "on") == 0 || g_strcmp0(v, "1") == 0) {
		*out = TRUE;
		return TRUE;
	}
	if (g_ascii_strcasecmp(v, "false") == 0 || g_ascii_strcasecmp(v, "no") == 0
	    || g_ascii_strcasecmp(v, "off") == 0 || g_strcmp0(v, "0") == 0) {
		*out = FALSE;
		return TRUE;
	}
	return FALSE;
}

/* ── The window being typed into ───────────────────────────────────── */

/* The program behind a window: /proc/PID/exe, for espanso's filter_exec */
static gchar *
exec_of(GowlClient *c)
{
	g_autofree gchar *link = NULL;
	gint pid = (gint)gowl_client_get_pid(c);

	if (pid <= 0)
		return g_strdup("");
	link = g_strdup_printf("/proc/%d/exe", pid);
	{
		gchar *target = g_file_read_link(link, NULL);

		return target != NULL ? target : g_strdup("");
	}
}

static void
set_field(
	GHashTable  *f,
	const gchar *key,
	const gchar *value
){
	g_hash_table_replace(f, g_strdup(key), g_strdup(value != NULL ? value : ""));
}

/*
 * The facts a `filter:' can name, for window @c (NULL: none focused).
 * Read on the compositor thread, at the keystroke.
 */
static GHashTable *
window_fields(
	GowlModuleGabbrev *self,
	GowlClient        *c
){
	static const gchar * const days[] = {
		"", "mon", "tue", "wed", "thu", "fri", "sat", "sun"
	};
	GHashTable *f;
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	GowlMonitor *mon = NULL;

	f = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	if (c != NULL) {
		g_autofree gchar *exec = exec_of(c);

		set_field(f, "app-id", gowl_client_get_app_id(c));
		set_field(f, "class", gowl_client_get_app_id(c));
		set_field(f, "title", gowl_client_get_title(c));
		set_field(f, "exec", exec);
		g_hash_table_replace(f, g_strdup("pid"), g_strdup_printf("%d",
		                     (gint)gowl_client_get_pid(c)));
		set_field(f, "xwayland", gowl_client_get_xwayland(c) ? "true" : "false");
		set_field(f, "floating", gowl_client_get_floating(c) ? "true" : "false");
		set_field(f, "fullscreen", gowl_client_get_fullscreen(c)
		          ? "true" : "false");
		mon = gowl_client_get_monitor(c);
	}
	if (mon == NULL && self->compositor != NULL)
		mon = gowl_compositor_get_selected_monitor(self->compositor);
	if (mon != NULL) {
		guint32 mask = gowl_monitor_get_tags(mon);
		GString *tags = g_string_new(NULL);
		gint lowest = 0;
		guint i;

		set_field(f, "monitor", gowl_monitor_get_name(mon));
		set_field(f, "layout", gowl_monitor_get_layout_symbol(mon));
		for (i = 0; i < 32; i++) {
			if ((mask & (1u << i)) == 0)
				continue;
			if (lowest == 0)
				lowest = (gint)i + 1;
			g_string_append_printf(tags, "%s%u", tags->len ? "," : "", i + 1);
		}
		g_hash_table_replace(f, g_strdup("tags"), g_string_free(tags, FALSE));
		g_hash_table_replace(f, g_strdup("tag"), g_strdup_printf("%d", lowest));
	}
	g_hash_table_replace(f, g_strdup("time"), g_date_time_format(now, "%H:%M"));
	g_hash_table_replace(f, g_strdup("hour"),
	                     g_strdup_printf("%d", g_date_time_get_hour(now)));
	set_field(f, "weekday", days[g_date_time_get_day_of_week(now)]);
	return f;
}

static gboolean
is_terminal(
	GowlModuleGabbrev *self,
	const gchar       *app_id
){
	guint i;

	for (i = 0; self->terminals != NULL && self->terminals[i] != NULL; i++)
		if (app_id != NULL && g_ascii_strcasecmp(self->terminals[i], app_id) == 0)
			return TRUE;
	return FALSE;
}

/* Everything known about the window one keystroke is going to. */
typedef struct {
	GowlClient           *client;
	GabbrevSet           *set;        /* borrowed */
	GHashTable           *fields;
	GabbrevWindow         window;
	const GabbrevProfile *profile;
	gboolean              denied;
	gboolean              disabled;   /* the profile says enable: false */
} Context;

static void
context_init(
	GowlModuleGabbrev *self,
	Context           *ctx,
	GowlClient        *c
){
	memset(ctx, 0, sizeof *ctx);
	ctx->client = c;
	ctx->set = self->set;
	ctx->fields = window_fields(self, c);
	ctx->window.app_id = g_hash_table_lookup(ctx->fields, "app-id");
	ctx->window.title = g_hash_table_lookup(ctx->fields, "title");
	ctx->window.exec = g_hash_table_lookup(ctx->fields, "exec");
	ctx->window.fields = ctx->fields;
	ctx->denied = self->deny != NULL && c != NULL
	              && gowl_filter_eval(self->deny, ctx->fields);
	if (self->set != NULL) {
		gboolean enable;

		ctx->profile = gabbrev_set_profile_for(self->set, &ctx->window);
		enable = self->set->options.enable;
		if (ctx->profile != NULL
		    && (ctx->profile->options.set & GABBREV_OPT_ENABLE))
			enable = ctx->profile->options.enable;
		ctx->disabled = !enable;
	}
}

static void
context_clear(Context *ctx)
{
	g_clear_pointer(&ctx->fields, g_hash_table_unref);
}

/* The options in force in a window: the set's, then its profile's. */
static void
effective_options(
	GowlModuleGabbrev    *self,
	const GabbrevProfile *profile,
	GabbrevOptions       *out
){
	gabbrev_options_copy(out, &self->set->options);
	if (profile != NULL)
		gabbrev_options_merge(out, &profile->options);
}

/* May @m expand in @ctx's window?  Filters, profile files, search-only. */
static gboolean
match_allowed(
	const GabbrevMatch *m,
	gpointer            data
){
	Context *ctx = data;

	if (m->search_only)
		return FALSE;
	if (ctx->set != NULL
	    && !gabbrev_set_match_allowed(ctx->set, ctx->profile, m))
		return FALSE;
	if (!gabbrev_regexes_match(m->filter_title, m->filter_class,
	                           m->filter_exec, &ctx->window))
		return FALSE;
	if (m->filter != NULL) {
		if (m->triggers->len > 0)
			set_field(ctx->fields, "trigger", g_ptr_array_index(m->triggers, 0));
		if (!gowl_filter_eval(m->filter, ctx->fields))
			return FALSE;
	}
	return TRUE;
}

/* Shift, Control, Alt, Super, AltGr, the locks: keys that only
   change what other keys mean. */
static gboolean
is_modifier_sym(guint32 sym)
{
	return (sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R)
	       || (sym >= XKB_KEY_ISO_Lock && sym <= XKB_KEY_ISO_Level5_Lock)
	       || sym == XKB_KEY_Mode_switch || sym == XKB_KEY_Num_Lock;
}

/* ── Chords ────────────────────────────────────────────────────────── */

/*
 * "CTRL+SHIFT+V" (espanso) or "Ctrl+Shift+v" / "Super+e" (gowl):
 * modifiers and one key, case-insensitive.  Returns FALSE for text it
 * cannot read; "off", "none" and "" are a valid nothing (0, 0).
 */
static gboolean
parse_chord(
	const gchar *text,
	guint32     *out_mods,
	guint32     *out_sym
){
	g_auto(GStrv) parts = NULL;
	guint32 mods = 0;
	guint i;
	guint n;

	*out_mods = 0;
	*out_sym = 0;
	if (text == NULL || *text == '\0' || g_ascii_strcasecmp(text, "off") == 0
	    || g_ascii_strcasecmp(text, "none") == 0)
		return TRUE;
	parts = g_strsplit(text, "+", -1);
	n = g_strv_length(parts);
	for (i = 0; i < n; i++) {
		gchar *p = g_strstrip(parts[i]);

		if (i + 1 < n) {
			if (g_ascii_strcasecmp(p, "ctrl") == 0
			    || g_ascii_strcasecmp(p, "control") == 0)
				mods |= GOWL_KEY_MOD_CTRL;
			else if (g_ascii_strcasecmp(p, "shift") == 0)
				mods |= GOWL_KEY_MOD_SHIFT;
			else if (g_ascii_strcasecmp(p, "alt") == 0
			         || g_ascii_strcasecmp(p, "option") == 0)
				mods |= GOWL_KEY_MOD_ALT;
			else if (g_ascii_strcasecmp(p, "super") == 0
			         || g_ascii_strcasecmp(p, "meta") == 0
			         || g_ascii_strcasecmp(p, "logo") == 0
			         || g_ascii_strcasecmp(p, "win") == 0
			         || g_ascii_strcasecmp(p, "cmd") == 0
			         || g_ascii_strcasecmp(p, "mod4") == 0)
				mods |= GOWL_KEY_MOD_LOGO;
			else
				return FALSE;
			continue;
		}
		*out_sym = xkb_keysym_from_name(p, XKB_KEYSYM_CASE_INSENSITIVE);
		if (*out_sym == XKB_KEY_NoSymbol)
			return FALSE;
		/* `V' named a letter: bind the key, not its shifted level */
		*out_sym = xkb_keysym_to_lower(*out_sym);
	}
	*out_mods = mods;
	return *out_sym != 0;
}

/*
 * espanso's toggle_key: a modifier pressed twice on its own ("ALT",
 * "LEFT_CTRL", ...).  Fills the one or two keysyms it means; FALSE when
 * @text is not a modifier name.
 */
static gboolean
parse_tap_modifier(
	const gchar *text,
	guint32     *sym1,
	guint32     *sym2
){
	static const struct {
		const gchar *name;
		guint32      a;
		guint32      b;
	} table[] = {
		{ "CTRL",        XKB_KEY_Control_L, XKB_KEY_Control_R },
		{ "LEFT_CTRL",   XKB_KEY_Control_L, 0 },
		{ "RIGHT_CTRL",  XKB_KEY_Control_R, 0 },
		{ "ALT",         XKB_KEY_Alt_L,     XKB_KEY_Alt_R },
		{ "LEFT_ALT",    XKB_KEY_Alt_L,     0 },
		{ "RIGHT_ALT",   XKB_KEY_Alt_R,     0 },
		{ "SHIFT",       XKB_KEY_Shift_L,   XKB_KEY_Shift_R },
		{ "LEFT_SHIFT",  XKB_KEY_Shift_L,   0 },
		{ "RIGHT_SHIFT", XKB_KEY_Shift_R,   0 },
		{ "META",        XKB_KEY_Super_L,   XKB_KEY_Super_R },
		{ "LEFT_META",   XKB_KEY_Super_L,   0 },
		{ "RIGHT_META",  XKB_KEY_Super_R,   0 },
	};
	guint i;

	*sym1 = *sym2 = 0;
	for (i = 0; text != NULL && i < G_N_ELEMENTS(table); i++) {
		if (g_ascii_strcasecmp(text, table[i].name) == 0) {
			*sym1 = table[i].a;
			*sym2 = table[i].b;
			return TRUE;
		}
	}
	return FALSE;
}

/* The toggle and search keys of the loaded options. */
static void
apply_keys(GowlModuleGabbrev *self)
{
	const GabbrevOptions *o = &self->set->options;

	self->toggle_mods = self->toggle_sym = 0;
	self->toggle_tap_sym = self->toggle_tap_sym2 = 0;
	if (o->toggle_key != NULL
	    && !parse_tap_modifier(o->toggle_key, &self->toggle_tap_sym,
	                           &self->toggle_tap_sym2)
	    && !parse_chord(o->toggle_key, &self->toggle_mods, &self->toggle_sym))
		g_warning("gabbrev: toggle_key `%s' is not a modifier (ALT, "
		          "LEFT_CTRL, ...) or a chord (Super+Alt+e)", o->toggle_key);
	self->search_mods = self->search_sym = 0;
	if (o->search_shortcut != NULL
	    && !parse_chord(o->search_shortcut, &self->search_mods,
	                    &self->search_sym))
		g_warning("gabbrev: search_shortcut `%s' is not a chord",
		          o->search_shortcut);
}

/* ── Posting from any thread ───────────────────────────────────────── */

/*
 * Runs @func(@self, @data) on the compositor's event loop, soon.  Safe
 * from any thread: an answer from Lisp (Emacs's thread), a compile
 * worker, a provider's own thread.  Before startup, or after the
 * display is gone, it runs nothing and frees @data.
 */
static void
post(
	GowlModuleGabbrev *self,
	void             (*func) (GowlModuleGabbrev *self, gpointer data),
	gpointer           data,
	GDestroyNotify     destroy
){
	Posted *p;
	guint64 one = 1;
	gboolean live;

	p = g_new0(Posted, 1);
	p->func = func;
	p->data = data;
	p->destroy = destroy;
	g_mutex_lock(&self->post_lock);
	live = self->post_fd >= 0;
	if (live)
		g_queue_push_tail(self->posted, p);
	g_mutex_unlock(&self->post_lock);
	if (!live) {
		if (destroy != NULL && data != NULL)
			destroy(data);
		g_free(p);
		return;
	}
	/* A full counter (it never is) still wakes the loop */
	if (write(self->post_fd, &one, sizeof one) < 0 && errno != EAGAIN)
		g_warning("gabbrev: cannot wake the event loop: %s",
		          g_strerror(errno));
}

static void
drain_posted(GowlModuleGabbrev *self)
{
	for (;;) {
		Posted *p;

		g_mutex_lock(&self->post_lock);
		p = g_queue_pop_head(self->posted);
		g_mutex_unlock(&self->post_lock);
		if (p == NULL)
			return;
		p->func(self, p->data);
		if (p->destroy != NULL && p->data != NULL)
			p->destroy(p->data);
		g_free(p);
	}
}

static int
on_post_fd(
	int       fd,
	uint32_t  mask,
	void     *data
){
	GowlModuleGabbrev *self = data;
	guint64 n;

	(void)mask;
	while (read(fd, &n, sizeof n) > 0)
		;
	drain_posted(self);
	return 0;
}

static void
post_open(GowlModuleGabbrev *self)
{
	struct wl_event_loop *loop;
	gint fd;

	if (self->post_source != NULL || self->compositor == NULL)
		return;
	loop = gowl_compositor_get_event_loop(self->compositor);
	if (loop == NULL)
		return;
	fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (fd < 0) {
		g_warning("gabbrev: eventfd: %s", g_strerror(errno));
		return;
	}
	self->post_source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
	                                         on_post_fd, self);
	if (self->post_source == NULL) {
		close(fd);
		return;
	}
	g_mutex_lock(&self->post_lock);
	self->post_fd = fd;
	g_mutex_unlock(&self->post_lock);
}

static void
posted_free(gpointer p)
{
	Posted *posted = p;

	if (posted->destroy != NULL && posted->data != NULL)
		posted->destroy(posted->data);
	g_free(posted);
}

static void
post_close(GowlModuleGabbrev *self)
{
	gint fd;

	g_mutex_lock(&self->post_lock);
	fd = self->post_fd;
	self->post_fd = -1;
	g_mutex_unlock(&self->post_lock);
	if (self->post_source != NULL) {
		wl_event_source_remove(self->post_source);
		self->post_source = NULL;
	}
	if (fd >= 0)
		close(fd);
	g_mutex_lock(&self->post_lock);
	g_queue_clear_full(self->posted, posted_free);
	g_mutex_unlock(&self->post_lock);
}

/* ── Loading ───────────────────────────────────────────────────────── */

/* espanso's search_trigger: typing it opens the search picker. */
static void
add_search_trigger(GabbrevSet *set)
{
	g_autoptr(YamlNode) none = NULL;
	GabbrevMatch *m;
	const gchar *t = set->options.search_trigger;

	if (t == NULL || *t == '\0')
		return;
	m = gabbrev_match_new();
	g_ptr_array_add(m->triggers, g_strdup(t));
	m->id = g_strdup("<search>");
	m->label = g_strdup("Search the matches");
	m->replace = g_strdup("{{picked}}");
	m->delete_first = TRUE;
	m->source = g_strdup(GABBREV_ORIGIN_BUILTIN);
	m->order = G_MAXUINT;
	g_ptr_array_add(m->vars, gabbrev_var_new("picked", "search", none));
	g_ptr_array_add(set->matches, m);
}

/*
 * Every source, in order -- built-in, the config's `gabbrev:' sections
 * and `files', what code added through libgowl (a C config, cmacs), and
 * what was added at run time -- into a fresh set.  The old set stays in
 * force for jobs already running (they hold it).
 */
static void
reload(GowlModuleGabbrev *self)
{
	g_autoptr(GabbrevLoader) loader = gabbrev_loader_new();
	g_autoptr(GPtrArray) code = NULL;
	const gchar *base = NULL;
	GabbrevSet *set;
	guint i;

	self->generation = gowl_gabbrev_get_generation();
	if (self->builtin)
		gabbrev_loader_add_builtin(loader);
	if (self->config != NULL) {
		GPtrArray *nodes = gowl_config_get_gabbrev_nodes(self->config);

		base = gowl_config_get_gabbrev_base_dir(self->config);
		for (i = 0; nodes != NULL && i < nodes->len; i++)
			gabbrev_loader_add_node(loader, g_ptr_array_index(nodes, i),
			                        base, GABBREV_ORIGIN_CONFIG);
	}
	for (i = 0; self->setting_files != NULL && self->setting_files[i] != NULL;
	     i++)
		if (*self->setting_files[i] != '\0')
			gabbrev_loader_add_path(loader, self->setting_files[i], base);

	code = gowl_gabbrev_dup_sources();
	for (i = 0; i < code->len; i++) {
		GowlGabbrevSource *s = g_ptr_array_index(code, i);

		if (s->is_file)
			gabbrev_loader_add_path(loader, s->text, NULL);
		else
			gabbrev_loader_add_text(loader, s->text, NULL,
			                        GABBREV_ORIGIN_CODE);
	}
	for (i = 0; i < self->runtime->len; i++) {
		RuntimeSource *s = g_ptr_array_index(self->runtime, i);

		if (s->is_file)
			gabbrev_loader_add_path(loader, s->text, NULL);
		else
			gabbrev_loader_add_text(loader, s->text, NULL,
			                        GABBREV_ORIGIN_RUNTIME);
	}

	set = gabbrev_loader_finish(loader);
	add_search_trigger(set);
	if (self->set != NULL)
		gabbrev_set_unref(self->set);
	self->set = set;
	gabbrev_engine_set_matches(self->engine, set->matches);
	gabbrev_engine_set_separators(self->engine, set->options.word_separators);
	gabbrev_engine_set_regex_buffer(self->engine, set->options.regex_buffer);
	self->typing_target = 0;
	apply_keys(self);
	gab_log(self, "loaded %u match(es) from %u file(s), %u problem(s)",
	        set->matches->len, set->files->len, set->problems->len);
	if (set->problems->len > 0)
		g_warning("gabbrev: %u configuration problem(s); first: %s "
		          "(gowl-msg gabbrev-status lists them)",
		          set->problems->len,
		          (gchar *)g_ptr_array_index(set->problems, 0));
	gab_event(self, "reloaded %u", set->matches->len);
}

static void sync_config (GowlModuleGabbrev *self);

/* Load again when code registered a type, a function or a source, or
   when a config reload REPLACED the config (Super+Shift+r builds a new
   one): cheap enough for every keystroke -- a pointer and a counter. */
static void
reload_if_stale(GowlModuleGabbrev *self)
{
	if (self->compositor != NULL
	    && gowl_compositor_get_config(self->compositor) != self->config) {
		sync_config(self);
		return;
	}
	if (self->set == NULL || self->generation != gowl_gabbrev_get_generation())
		reload(self);
}

static void on_config_reloaded(GowlConfig *config, gpointer data);

/* Follows the config the compositor uses: a reload replaces the object. */
static void
sync_config(GowlModuleGabbrev *self)
{
	GowlConfig *current;

	current = self->compositor != NULL
		? gowl_compositor_get_config(self->compositor) : NULL;
	if (current == self->config)
		return;
	if (self->config != NULL) {
		if (self->reloaded_handler != 0)
			g_signal_handler_disconnect(self->config, self->reloaded_handler);
		self->reloaded_handler = 0;
		g_clear_object(&self->config);
	}
	if (current != NULL) {
		self->config = g_object_ref(current);
		self->reloaded_handler = g_signal_connect(current, "reloaded",
			G_CALLBACK(on_config_reloaded), self);
	}
	reload(self);
}

static void
on_config_reloaded(
	GowlConfig *config,
	gpointer    data
){
	(void)config;
	reload(GOWL_MODULE_GABBREV(data));
}

/* ── Typing into a window ─────────────────────────────────────────── */

/* Whether @ch can be typed with this keymap at level 0 or 1 (plain or
   Shift).  AltGr levels are not: the injection path can only add Shift. */
static gboolean
typeable(
	GowlModuleGabbrev *self,
	gunichar           ch
){
	struct xkb_keymap *keymap;
	xkb_keysym_t sym;
	xkb_keycode_t kc;

	if (ch == '\n' || ch == '\t')
		return TRUE;
	if (self->compositor == NULL || self->compositor->wlr_kb_group == NULL)
		return FALSE;
	keymap = self->compositor->wlr_kb_group->keyboard.keymap;
	sym = xkb_utf32_to_keysym(ch);
	if (keymap == NULL || sym == XKB_KEY_NoSymbol)
		return FALSE;
	for (kc = xkb_keymap_min_keycode(keymap);
	     kc <= xkb_keymap_max_keycode(keymap); kc++) {
		xkb_level_index_t level;

		for (level = 0; level < 2
		     && level < xkb_keymap_num_levels_for_key(keymap, kc, 0); level++) {
			const xkb_keysym_t *syms;
			gint n = xkb_keymap_key_get_syms_by_level(keymap, kc, 0, level,
			                                           &syms);
			gint i;

			for (i = 0; i < n; i++)
				if (syms[i] == sym)
					return TRUE;
		}
	}
	return FALSE;
}

static gboolean
all_typeable(
	GowlModuleGabbrev *self,
	const gchar       *text
){
	const gchar *p;

	for (p = text; *p != '\0'; p = g_utf8_next_char(p))
		if (!typeable(self, g_utf8_get_char(p)))
			return FALSE;
	return TRUE;
}

static Job *job_ref (Job *job);

static void
step_free(Step *s)
{
	if (s == NULL)
		return;
	if (s->job != NULL)
		job_unref(s->job);
	g_free(s->text);
	if (s->bytes != NULL)
		g_bytes_unref(s->bytes);
	g_free(s->mime);
	if (s->destroy != NULL && s->data != NULL)
		s->destroy(s->data);
	g_free(s);
}

static Step *
step_new(
	StepKind  kind,
	Job      *job
){
	Step *s = g_new0(Step, 1);

	s->kind = kind;
	s->job = job != NULL ? job_ref(job) : NULL;
	return s;
}

static void
push_step(
	GowlModuleGabbrev *self,
	Step              *s
){
	g_queue_push_tail(self->steps, s);
}

static void
push_wait(
	GowlModuleGabbrev *self,
	Job               *job,
	guint              ms
){
	Step *s;

	if (ms == 0)
		return;
	s = step_new(STEP_WAIT, job);
	s->ms = ms;
	push_step(self, s);
}

/* One key, @times times, with the job's key delay between. */
static void
push_key(
	GowlModuleGabbrev *self,
	Job               *job,
	guint32            keycode,
	guint32            keysym,
	guint32            mods,
	guint              times
){
	guint i;

	for (i = 0; i < times; i++) {
		Step *s = step_new(STEP_KEY, job);

		s->keycode = keycode;
		s->keysym = keysym;
		s->mods = mods;
		push_step(self, s);
		if (job != NULL && i + 1 < times)
			push_wait(self, job, job->opts.key_delay_ms);
	}
}

static void
push_chord(
	GowlModuleGabbrev *self,
	Job               *job,
	const gchar       *chord
){
	guint32 mods;
	guint32 sym;

	if (!parse_chord(chord, &mods, &sym) || sym == 0) {
		g_warning("gabbrev: cannot press `%s'", chord);
		return;
	}
	push_key(self, job, 0, sym, mods, 1);
}

static void
push_text(
	GowlModuleGabbrev *self,
	Job               *job,
	const gchar       *text
){
	Step *s;

	if (text == NULL || *text == '\0')
		return;
	/* With a key delay, one character per step */
	if (job != NULL && job->opts.key_delay_ms > 0) {
		const gchar *p;

		for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
			s = step_new(STEP_TEXT, job);
			s->text = g_strndup(p, (gsize)(g_utf8_next_char(p) - p));
			push_step(self, s);
			push_wait(self, job, job->opts.key_delay_ms);
		}
		return;
	}
	s = step_new(STEP_TEXT, job);
	s->text = g_strdup(text);
	push_step(self, s);
}

static void
push_call(
	GowlModuleGabbrev *self,
	Job               *job,
	StepFunc           func,
	gpointer           data,
	GDestroyNotify     destroy
){
	Step *s = step_new(STEP_CALL, job);

	s->func = func;
	s->data = data;
	s->destroy = destroy;
	push_step(self, s);
}

/* Drops every queued step of @job (it failed or was cancelled). */
static void
drop_steps(
	GowlModuleGabbrev *self,
	Job               *job
){
	GList *l = self->steps->head;

	while (l != NULL) {
		GList *next = l->next;
		Step *s = l->data;

		if (s->job == job) {
			g_queue_delete_link(self->steps, l);
			step_free(s);
		}
		l = next;
	}
}

static gboolean
on_step_timer(gpointer data)
{
	GowlModuleGabbrev *self = GOWL_MODULE_GABBREV(data);

	self->waiting = FALSE;
	play_steps(self);
	return 0;
}

static void
arm_step_timer(
	GowlModuleGabbrev *self,
	guint              ms
){
	if (self->compositor == NULL)
		return;
	if (self->step_timer == NULL)
		self->step_timer = wl_event_loop_add_timer(
			gowl_compositor_get_event_loop(self->compositor),
			(wl_event_loop_timer_func_t)on_step_timer, self);
	if (self->step_timer != NULL)
		wl_event_source_timer_update(self->step_timer, (gint)MAX(ms, 1));
}

static void
resume_steps(GowlModuleGabbrev *self)
{
	self->paused = FALSE;
	play_steps(self);
}

/* --- clipboard save and restore --- */

typedef struct {
	GowlModuleGabbrev *self;
	Job               *job;
} SaveCtx;

static void
saved_on_loop(
	GowlModuleGabbrev *self,
	gpointer           data
){
	(void)data;
	resume_steps(self);
}

static void
on_clipboard_saved(
	gchar    *text,
	gpointer  data
){
	SaveCtx *c = data;

	c->job->saved_clipboard = text;
	c->job->have_saved = TRUE;
	job_unref(c->job);
	/* Back on the loop through the post queue, never re-entering the
	   player from inside a read callback */
	post(c->self, saved_on_loop, NULL, NULL);
	g_free(c);
}

static GowlSeat *
seat_of(GowlModuleGabbrev *self)
{
	return self->compositor != NULL
		? gowl_compositor_get_seat(self->compositor) : NULL;
}

/* One step.  FALSE: the player pauses (a callback resumes it). */
static gboolean
play_one(
	GowlModuleGabbrev *self,
	Step              *s
){
	Job *job = s->job;
	GowlClient *c = job != NULL ? client_by_id(self, job->target_id) : NULL;

	switch (s->kind) {
	case STEP_WAIT:
		self->waiting = TRUE;
		arm_step_timer(self, s->ms);
		return FALSE;
	case STEP_KEY: {
		guint32 keycode = s->keycode;
		guint32 mods = s->mods;

		if (c == NULL) {
			job_fail(self, job, "the window closed");
			return TRUE;
		}
		if (keycode == 0) {
			gboolean shift = FALSE;

			keycode = gowl_compositor_keysym_to_keycode(self->compositor,
			                                            s->keysym, &shift);
			if (shift)
				mods |= GOWL_KEY_MOD_SHIFT;
		}
		if (keycode == 0) {
			gab_log(self, "no key types keysym 0x%x here", s->keysym);
			return TRUE;
		}
		if (!gowl_compositor_send_key_to_client(self->compositor, c, keycode,
		                                        mods))
			job_fail(self, job, "the keyboard is not available (locked, or "
			         "a launcher holds it)");
		return TRUE;
	}
	case STEP_TEXT:
		if (c == NULL) {
			job_fail(self, job, "the window closed");
			return TRUE;
		}
		if (gowl_compositor_send_text_to_client(self->compositor, c,
		                                        s->text) < 0)
			job_fail(self, job, "the keyboard is not available (locked, or "
			         "a launcher holds it)");
		return TRUE;
	case STEP_SET_CLIPBOARD: {
		GowlSeat *seat = seat_of(self);

		if (seat == NULL)
			return TRUE;
		if (s->bytes != NULL)
			gowl_seat_set_clipboard_bytes(seat, s->bytes, s->mime);
		else
			gowl_seat_set_clipboard(seat, s->text != NULL ? s->text : "");
		return TRUE;
	}
	case STEP_SAVE_CLIPBOARD: {
		GowlSeat *seat = seat_of(self);
		SaveCtx *ctx;

		if (job == NULL || job->have_saved || seat == NULL)
			return TRUE;
		ctx = g_new0(SaveCtx, 1);
		ctx->self = self;
		ctx->job = job_ref(job);
		if (!gowl_seat_read_clipboard_async(seat,
		        gowl_compositor_get_event_loop(self->compositor),
		        on_clipboard_saved, ctx)) {
			/* Nothing on the clipboard: nothing to put back */
			job->have_saved = TRUE;
			job_unref(ctx->job);
			g_free(ctx);
			return TRUE;
		}
		return FALSE;
	}
	case STEP_RESTORE_CLIPBOARD: {
		GowlSeat *seat = seat_of(self);

		if (job != NULL && job->saved_clipboard != NULL && seat != NULL)
			gowl_seat_set_clipboard(seat, job->saved_clipboard);
		return TRUE;
	}
	case STEP_FOCUS:
		if (c != NULL
		    && gowl_compositor_get_focused_client(self->compositor) != c)
			gowl_compositor_focus_client(self->compositor, c, FALSE);
		return TRUE;
	case STEP_CALL:
		return s->func(self, job, s->data);
	default:
		return TRUE;
	}
}

/* Plays queued steps until one waits; reentrant calls do nothing. */
static void
play_steps(GowlModuleGabbrev *self)
{
	static gboolean playing;

	if (playing || self->waiting)
		return;
	playing = TRUE;
	while (!self->paused && !self->waiting) {
		Step *s = g_queue_pop_head(self->steps);
		gboolean go_on;

		if (s == NULL)
			break;
		go_on = play_one(self, s);
		/* A WAIT has armed the timer: the player is not paused, it just
		   stops here and the timer calls back. */
		if (s->kind == STEP_WAIT) {
			step_free(s);
			break;
		}
		if (!go_on)
			self->paused = TRUE;
		step_free(s);
	}
	playing = FALSE;
}

/* Starts playing, unless a step is already waiting for a timer or a
   callback: those continue the queue themselves. */
static void
kick_steps(GowlModuleGabbrev *self)
{
	if (self->paused || self->waiting)
		return;
	/* Not in the middle of the key that completed the trigger: the
	   window gets that key first, then ours, one loop turn later */
	if (self->in_key) {
		self->waiting = TRUE;
		arm_step_timer(self, 1);
		return;
	}
	play_steps(self);
}

/* ── Jobs ──────────────────────────────────────────────────────────── */

static Job *
job_ref(Job *job)
{
	g_atomic_int_inc(&job->ref_count);
	return job;
}

static void
job_unref(Job *job)
{
	if (job == NULL || !g_atomic_int_dec_and_test(&job->ref_count))
		return;
	if (job->render != NULL) {
		gabbrev_render_cancel(job->render);
		gabbrev_render_unref(job->render);
	}
	if (job->timeout != NULL)
		wl_event_source_remove(job->timeout);
	if (job->set != NULL)
		gabbrev_set_unref(job->set);
	if (job->match != NULL)
		gabbrev_match_unref(job->match);
	g_free(job->typed);
	g_free(job->trailing);
	if (job->captures != NULL)
		g_hash_table_unref(job->captures);
	g_free(job->app_id);
	g_free(job->title);
	if (job->fields != NULL)
		g_hash_table_unref(job->fields);
	gabbrev_options_clear(&job->opts);
	g_free(job->saved_clipboard);
	g_free(job->render_id);
	g_free(job->custom_form);
	g_free(job);
}

/* The job is over: out of the list, its timer gone. */
static void
job_retire(
	GowlModuleGabbrev *self,
	Job               *job
){
	job->finished = TRUE;
	if (job->timeout != NULL) {
		wl_event_source_remove(job->timeout);
		job->timeout = NULL;
	}
	if (job->render != NULL)
		gabbrev_render_cancel(job->render);
	/* The list owns its reference: removing drops it */
	g_ptr_array_remove(self->jobs, job);
}

/* A Lisp string literal: what `custom' actions are under cmacs */
static gchar *
lisp_quote(const gchar *s)
{
	GString *out = g_string_new("\"");
	const gchar *p;

	for (p = s; p != NULL && *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_c(out, '\\');
		g_string_append_c(out, *p);
	}
	g_string_append_c(out, '"');
	return g_string_free(out, FALSE);
}

static gchar *
json_quote(const gchar *s)
{
	g_autoptr(JsonNode) n = json_node_new(JSON_NODE_VALUE);

	json_node_set_string(n, s != NULL ? s : "");
	return json_to_string(n, FALSE);
}

static gboolean
step_put_back_done(
	GowlModuleGabbrev *self,
	Job               *job,
	gpointer           data
){
	(void)data;
	job_retire(self, job);
	return TRUE;
}

/*
 * The expansion cannot happen.  Says why, and leaves the window as it
 * was: a trigger already rubbed out is typed back (after collapsing a
 * grab's selection, so the trigger does not replace the text), and a
 * saved clipboard goes back.
 */
static void
job_fail(
	GowlModuleGabbrev *self,
	Job               *job,
	const gchar       *why
){
	if (job == NULL || job->finished)
		return;
	job->finished = TRUE;
	self->n_failed++;
	g_free(self->last_error);
	self->last_error = g_strdup_printf("%s: %s", job->match->id, why);
	/* An expansion failing is an outcome, not a fault in gowl */
	g_message("gabbrev: %s", self->last_error);
	gab_event(self, "failed %s %s", job->match->id, why);
	if (job->origin == ORIGIN_RENDER) {
		g_autofree gchar *q = json_quote(why);

		gab_event(self, "render-failed %s %s",
		          job->render_id != NULL ? job->render_id : "-", q);
	} else {
		gab_notify(self, "Expansion failed", self->last_error);
	}
	drop_steps(self, job);
	if (job->render != NULL)
		gabbrev_render_cancel(job->render);

	if (client_by_id(self, job->target_id) != NULL
	    && ((job->origin == ORIGIN_TYPED && job->deleted) || job->have_saved)) {
		g_autofree gchar *back = g_strconcat(job->typed != NULL ? job->typed
		                                     : "", job->trailing, NULL);

		/* Steps already queued for this job were dropped; these are
		   new, so they need the job to stay listed until they play */
		job->finished = FALSE;
		if (job->origin == ORIGIN_TYPED && job->deleted) {
			if (job->grabbed)
				push_key(self, job, KEY_RIGHT, 0, 0, 1);
			push_text(self, job, back);
		}
		if (job->have_saved)
			push_step(self, step_new(STEP_RESTORE_CLIPBOARD, job));
		push_call(self, job, step_put_back_done, NULL, NULL);
		job->finished = TRUE;
		kick_steps(self);
		return;
	}
	job_retire(self, job);
}

static gboolean
on_job_timeout(gpointer data)
{
	Job *job = data;
	g_autofree gchar *why = NULL;
	const gchar *what = job->render != NULL
		? gabbrev_render_get_pending_type(job->render) : NULL;

	why = g_strdup_printf("timed out after %u ms%s%s%s", job->opts.timeout_ms,
	                      what != NULL ? " (waiting for a `" : "",
	                      what != NULL ? what : "",
	                      what != NULL ? "' variable)" : "");
	job_fail(job->self, job, why);
	return 0;
}

/* --- after the text is in --- */

static gboolean
step_job_done(
	GowlModuleGabbrev *self,
	Job               *job,
	gpointer           data
){
	guint n_typed = GPOINTER_TO_UINT(data);

	if (job->finished)
		return TRUE;
	self->n_expanded++;
	gab_event(self, "expanded %s", job->match->id);
	if (job->match->notify)
		gab_notify(self, "Expanded", job->match->label != NULL
		           ? job->match->label : job->match->id);

	/* Undo with Backspace: only for a plain typed expansion whose
	   cursor is at its end */
	g_free(self->last.restore);
	memset(&self->last, 0, sizeof self->last);
	if (job->origin == ORIGIN_TYPED && job->opts.undo_backspace
	    && n_typed > 0) {
		self->last.valid = TRUE;
		self->last.target_id = job->target_id;
		self->last.n_typed = n_typed;
		self->last.restore = g_strdup(job->typed);
		self->last.when_us = now_us();
	}
	job_retire(self, job);
	return TRUE;
}

/* The paste chord for the job's window. */
static const gchar *
paste_chord(Job *job)
{
	if (job->match->paste_shortcut != NULL)
		return job->match->paste_shortcut;
	if (job->opts.paste_shortcut != NULL)
		return job->opts.paste_shortcut;
	return job->terminal && job->opts.apply_patch ? "CTRL+SHIFT+V" : "CTRL+V";
}

static const gchar *
copy_chord(
	Job          *job,
	GHashTable   *params
){
	const gchar *c = params != NULL ? g_hash_table_lookup(params, "copy") : NULL;

	if (c != NULL)
		return c;
	return job->terminal && job->opts.apply_patch ? "CTRL+SHIFT+C" : "CTRL+C";
}

/*
 * Queues what types the rendered text into the job's window:
 * the trigger rubbed out (unless it already was), the text as keys or
 * as a paste, the separator back, the cursor moved to the hint, and the
 * clipboard put back.
 */
static void
job_inject(
	GowlModuleGabbrev *self,
	Job               *job,
	const gchar       *text,
	const gchar       *html,
	gint               cursor_back
){
	GabbrevBackend mode;
	GowlClient *c = client_by_id(self, job->target_id);
	gboolean paste;
	guint n_typed;
	GowlClient *focused;

	if (c == NULL) {
		job_fail(self, job, "the window closed");
		return;
	}
	job->injecting = TRUE;

	/* Focus moved (a picker, the minibuffer): give it back first */
	focused = gowl_compositor_get_focused_client(self->compositor);
	if (focused != c) {
		if (!job->opts.refocus) {
			GowlSeat *seat = seat_of(self);

			if (seat != NULL)
				gowl_seat_set_clipboard(seat, text);
			gab_notify(self, "Expansion on the clipboard",
			           "the window lost focus; paste it where you want it");
			job_retire(self, job);
			return;
		}
		push_step(self, step_new(STEP_FOCUS, job));
		push_wait(self, job, job->opts.post_picker_delay_ms);
	}

	if (!job->deleted && job->n_delete > 0)
		push_key(self, job, KEY_BACKSPACE, 0, 0, job->n_delete);

	mode = job->match->force_mode >= 0 ? (GabbrevBackend)job->match->force_mode
	       : job->opts.backend;
	if (job->match->image_path != NULL || html != NULL)
		paste = TRUE;
	else if (mode == GABBREV_BACKEND_CLIPBOARD)
		paste = TRUE;
	else if (mode == GABBREV_BACKEND_KEYS)
		paste = FALSE;
	else
		paste = g_utf8_strlen(text, -1) > (glong)job->opts.clipboard_threshold
		        || !all_typeable(self, text);

	if (!paste) {
		push_text(self, job, text);
	} else {
		Step *set = step_new(STEP_SET_CLIPBOARD, job);

		if (job->opts.preserve_clipboard)
			push_step(self, step_new(STEP_SAVE_CLIPBOARD, job));
		if (job->match->image_path != NULL) {
			gchar *data = NULL;
			gsize len = 0;
			g_autoptr(GError) error = NULL;

			if (!g_file_get_contents(job->match->image_path, &data, &len,
			                         &error)) {
				step_free(set);
				job_fail(self, job, error->message);
				return;
			}
			set->bytes = g_bytes_new_take(data, len);
			set->mime = g_strdup(g_str_has_suffix(job->match->image_path, ".jpg")
			                     || g_str_has_suffix(job->match->image_path, ".jpeg")
			                     ? "image/jpeg"
			                     : g_str_has_suffix(job->match->image_path, ".svg")
			                     ? "image/svg+xml" : "image/png");
		} else if (html != NULL) {
			set->bytes = g_bytes_new(html, strlen(html));
			set->mime = g_strdup("text/html");
		} else {
			set->text = g_strdup(text);
		}
		push_step(self, set);
		push_wait(self, job, job->opts.pre_paste_delay_ms);
		push_chord(self, job, paste_chord(job));
		push_wait(self, job, job->opts.post_paste_delay_ms);
	}
	if (job->trailing != NULL)
		push_text(self, job, job->trailing);
	if (cursor_back > 0) {
		guint back = (guint)cursor_back
		             + (job->trailing != NULL
		                ? (guint)g_utf8_strlen(job->trailing, -1) : 0);

		push_key(self, job, KEY_LEFT, 0, 0, back);
	}
	if (paste && job->opts.preserve_clipboard) {
		push_wait(self, job, job->opts.restore_delay_ms);
		push_step(self, step_new(STEP_RESTORE_CLIPBOARD, job));
	} else if (job->have_saved) {
		/* a grab read the clipboard; it goes back regardless */
		push_step(self, step_new(STEP_RESTORE_CLIPBOARD, job));
	}

	/* Undo knows how much to rub out -- not after a cursor hint or a
	   picture, where Backspace no longer lands on the expansion */
	n_typed = 0;
	if (cursor_back < 0 && job->match->image_path == NULL && html == NULL)
		n_typed = (guint)g_utf8_strlen(text, -1)
		          + (job->trailing != NULL
		             ? (guint)g_utf8_strlen(job->trailing, -1) : 0);
	push_call(self, job, step_job_done, GUINT_TO_POINTER(n_typed), NULL);
	kick_steps(self);
}

/* The render finished (or failed): type it, or report it. */
static void
on_render_done(
	GabbrevRender *render,
	gpointer       data
){
	Job *job = data;
	GowlModuleGabbrev *self = job->self;
	const gchar *text = gabbrev_render_get_text(render);

	if (job->finished)
		return;
	if (text == NULL) {
		job_fail(self, job, gabbrev_render_get_error(render));
		return;
	}
	if (job->origin == ORIGIN_RENDER) {
		g_autofree gchar *q = json_quote(text);

		gab_event(self, "rendered %s %s",
		          job->render_id != NULL ? job->render_id : "-", q);
		if (job->custom_form != NULL && self->compositor != NULL) {
			g_autofree gchar *lisp = lisp_quote(text);
			g_auto(GStrv) parts = g_strsplit(job->custom_form, "%r", -1);
			g_autofree gchar *form = g_strjoinv(lisp, parts);
			GowlKeybindEntry kb;

			memset(&kb, 0, sizeof kb);
			kb.action = GOWL_ACTION_CUSTOM;
			kb.arg = form;
			gowl_compositor_run_keybind_entry(self->compositor, &kb);
		}
		self->n_expanded++;
		job_retire(self, job);
		return;
	}
	job_inject(self, job, text, gabbrev_render_get_html(render),
	           gabbrev_render_get_cursor_back(render));
}

/* --- the render host --- */

static gboolean host_resolve (gpointer host_data, GabbrevRender *render,
                              const GabbrevVar *var,
                              GowlGabbrevRequest *request);

typedef struct {
	GabbrevRender      *render;
	GowlGabbrevRequest *request;
} Delivery;

static void
delivery_free(gpointer p)
{
	Delivery *d = p;

	gabbrev_render_unref(d->render);
	gowl_gabbrev_request_unref(d->request);
	g_free(d);
}

static void
deliver_on_loop(
	GowlModuleGabbrev *self,
	gpointer           data
){
	Delivery *d = data;

	(void)self;
	gabbrev_render_deliver(d->render, d->request);
}

static void
host_post(
	gpointer            host_data,
	GabbrevRender      *render,
	GowlGabbrevRequest *request
){
	Job *job = host_data;
	Delivery *d = g_new0(Delivery, 1);

	d->render = render;     /* both references are ours now */
	d->request = request;
	post(job->self, deliver_on_loop, d, delivery_free);
}

static const GabbrevRenderHost render_host = {
	host_resolve,
	host_post
};

static gboolean
step_begin_render(
	GowlModuleGabbrev *self,
	Job               *job,
	gpointer           data
){
	(void)data;
	if (job->finished)
		return TRUE;
	job->deleted = job->deleted || (job->delete_first
	                                && job->origin == ORIGIN_TYPED);
	gabbrev_render_start(job->render);
	(void)self;
	return TRUE;
}

static gboolean
match_has_type(
	const GabbrevMatch *m,
	const gchar        *type
){
	guint i;

	for (i = 0; i < m->vars->len; i++)
		if (g_strcmp0(((GabbrevVar *)g_ptr_array_index(m->vars, i))->type,
		              type) == 0)
			return TRUE;
	return FALSE;
}

/*
 * Starts a job for @match typed (or asked for) in window @c.  A trigger
 * that is rubbed out first is rubbed out by the step player; the render
 * starts after that.  The job may be over -- and freed -- by the time
 * this returns (a render with nothing to wait for), so it returns the
 * job's number, never the job: 0 when it could not start.
 */
static guint
job_start(
	GowlModuleGabbrev *self,
	GabbrevMatch      *match,
	GabbrevHit        *hit,
	GowlClient        *c,
	const Context     *ctx,
	JobOrigin          origin,
	const gchar       *render_id,
	const gchar       *custom_form
){
	Job *job;
	GabbrevRenderInfo info;
	guint id;

	if (self->jobs->len >= self->max_jobs) {
		gab_notify(self, "Expansion skipped", "too many expansions running; "
		           "gowl-msg gabbrev-cancel stops them");
		return 0;
	}
	job = g_new0(Job, 1);
	job->ref_count = 1;
	job->id = ++self->next_job_id;
	job->self = self;
	job->origin = origin;
	job->set = gabbrev_set_ref(self->set);
	job->match = gabbrev_match_ref(match);
	if (hit != NULL) {
		job->typed = g_strdup(hit->typed);
		job->n_delete = hit->n_delete;
		job->trailing = g_strdup(hit->trailing);
		job->typed_case = hit->typed_case;
		job->captures = hit->captures != NULL
			? g_hash_table_ref(hit->captures) : NULL;
	}
	job->target_id = c != NULL ? gowl_client_get_id(c) : 0;
	job->app_id = g_strdup(c != NULL ? gowl_client_get_app_id(c) : "");
	job->title = g_strdup(c != NULL ? gowl_client_get_title(c) : "");
	job->fields = ctx != NULL && ctx->fields != NULL
		? g_hash_table_ref(ctx->fields) : window_fields(self, c);
	job->terminal = is_terminal(self, job->app_id);
	effective_options(self, ctx != NULL ? ctx->profile : NULL, &job->opts);
	job->delete_first = match->delete_first || match_has_type(match, "selection");
	job->started_us = now_us();
	job->render_id = g_strdup(render_id);
	job->custom_form = g_strdup(custom_form);
	id = job->id;
	g_ptr_array_add(self->jobs, job);

	{
		guint ms = match->timeout_ms > 0 ? match->timeout_ms
		           : job->opts.timeout_ms;

		if (ms > 0 && self->compositor != NULL) {
			job->timeout = wl_event_loop_add_timer(
				gowl_compositor_get_event_loop(self->compositor),
				(wl_event_loop_timer_func_t)on_job_timeout, job);
			if (job->timeout != NULL)
				wl_event_source_timer_update(job->timeout, (gint)ms);
			job->opts.timeout_ms = ms;
		}
	}

	memset(&info, 0, sizeof info);
	info.set = job->set;
	info.match = match;
	info.captures = job->captures;
	info.typed = job->typed;
	info.typed_case = job->typed_case;
	info.app_id = job->app_id;
	info.title = job->title;
	job->render = gabbrev_render_new(&info, &render_host, job,
	                                 on_render_done, job);

	if (origin == ORIGIN_TYPED && job->delete_first && job->n_delete > 0) {
		push_key(self, job, KEY_BACKSPACE, 0, 0, job->n_delete);
		push_call(self, job, step_begin_render, NULL, NULL);
		kick_steps(self);
	} else {
		/* The render may finish (and the job retire) right here */
		job_ref(job);
		gabbrev_render_start(job->render);
		job_unref(job);
	}
	return id;
}

/* ── Variable types the desktop provides ──────────────────────────── */

/* VAR_NAME -> GABBREV_VAR_NAME / ESPANSO_VAR_NAME */
static gchar *
env_name(
	const gchar *prefix,
	const gchar *name
){
	GString *s = g_string_new(prefix);
	const gchar *p;

	for (p = name; *p != '\0'; p++)
		g_string_append_c(s, g_ascii_isalnum(*p) ? g_ascii_toupper(*p) : '_');
	return g_string_free(s, FALSE);
}

/* `env NAME=value ...' before a command: every value so far, under both
   spellings, plus the trigger and the window. */
static void
add_env(
	GPtrArray          *argv,
	GowlGabbrevRequest *req
){
	GHashTableIter it;
	gpointer k;
	gpointer v;

	g_ptr_array_add(argv, g_strdup("env"));
	g_hash_table_iter_init(&it, gowl_gabbrev_request_get_vars(req));
	while (g_hash_table_iter_next(&it, &k, &v)) {
		g_autofree gchar *g = env_name("GABBREV_", k);
		g_autofree gchar *e = env_name("ESPANSO_", k);

		g_ptr_array_add(argv, g_strdup_printf("%s=%s", g, (gchar *)v));
		g_ptr_array_add(argv, g_strdup_printf("%s=%s", e, (gchar *)v));
	}
	g_ptr_array_add(argv, g_strdup_printf("GABBREV_TRIGGER=%s",
	                gowl_gabbrev_request_get_trigger(req)));
	g_ptr_array_add(argv, g_strdup_printf("GABBREV_APP_ID=%s",
	                gowl_gabbrev_request_get_app_id(req)));
	g_ptr_array_add(argv, g_strdup_printf("GABBREV_TITLE=%s",
	                gowl_gabbrev_request_get_title(req)));
}

typedef struct {
	GowlGabbrevRequest *req;
	gboolean            trim;
	gboolean            ignore_error;
	gboolean            debug;
	gchar              *what;
} ProcCtx;

static void
proc_ctx_free(ProcCtx *c)
{
	gowl_gabbrev_request_unref(c->req);
	g_free(c->what);
	g_free(c);
}

/* The last non-empty line of @text, for an error message */
static gchar *
last_line(const gchar *text)
{
	g_auto(GStrv) lines = g_strsplit(text != NULL ? text : "", "\n", -1);
	gint i;

	for (i = (gint)g_strv_length(lines) - 1; i >= 0; i--)
		if (*g_strstrip(lines[i]) != '\0')
			return g_strdup(lines[i]);
	return g_strdup("");
}

static void
on_proc_done(
	gint         status,
	const gchar *out,
	const gchar *err,
	gpointer     data
){
	ProcCtx *c = data;

	if (c->debug && err != NULL && *err != '\0')
		g_message("gabbrev: %s stderr: %s", c->what, err);
	if (status == -1) {
		gowl_gabbrev_request_return_error(c->req, "ran past its timeout "
		                                  "and was killed");
	} else if (!g_spawn_check_wait_status(status, NULL) && !c->ignore_error) {
		g_autofree gchar *line = last_line(err);
		g_autofree gchar *msg = g_strdup_printf("%s failed%s%s", c->what,
		                                        *line ? ": " : "", line);

		gowl_gabbrev_request_return_error(c->req, msg);
	} else if (c->trim) {
		g_autofree gchar *t = g_strdup(out != NULL ? out : "");

		gowl_gabbrev_request_return(c->req, g_strstrip(t));
	} else {
		gowl_gabbrev_request_return(c->req, out != NULL ? out : "");
	}
	proc_ctx_free(c);
}

static gboolean
param_bool(
	GowlGabbrevRequest *req,
	const gchar        *key,
	gboolean            fallback
){
	gboolean b;

	return parse_bool_setting(gowl_gabbrev_request_get_param(req, key), &b)
	       ? b : fallback;
}

static guint
param_uint(
	GowlGabbrevRequest *req,
	const gchar        *key,
	guint               fallback
){
	const gchar *v = gowl_gabbrev_request_get_param(req, key);
	guint64 n;

	if (v != NULL && g_ascii_string_to_unsigned(v, 10, 0, G_MAXUINT, &n, NULL))
		return (guint)n;
	return fallback;
}

static void
spawn_for(
	GowlModuleGabbrev  *self,
	GowlGabbrevRequest *req,
	GPtrArray          *argv,
	const gchar        *what,
	guint               timeout_ms,
	gboolean            trim
){
	g_autoptr(GError) error = NULL;
	ProcCtx *c = g_new0(ProcCtx, 1);

	g_ptr_array_add(argv, NULL);
	c->req = gowl_gabbrev_request_ref(req);
	c->trim = trim;
	c->ignore_error = param_bool(req, "ignore_error", FALSE);
	c->debug = param_bool(req, "debug", FALSE);
	c->what = g_strdup(what);
	if (gowl_subprocess_spawn(gowl_compositor_get_event_loop(self->compositor),
	                          (const gchar * const *)argv->pdata, timeout_ms,
	                          on_proc_done, c, &error) == NULL) {
		gowl_gabbrev_request_return_error(req, error->message);
		proc_ctx_free(c);
	}
}

/* `shell': params.cmd run by bash (or params.shell) */
static void
resolve_shell(
	GowlModuleGabbrev  *self,
	GowlGabbrevRequest *req
){
	const gchar *cmd = gowl_gabbrev_request_get_param(req, "cmd");
	const gchar *shell = gowl_gabbrev_request_get_param(req, "shell");
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *path = NULL;

	if (cmd == NULL) {
		gowl_gabbrev_request_return_error(req, "shell: params.cmd is the "
		                                  "command");
		return;
	}
	if (shell == NULL || *shell == '\0')
		shell = "bash";
	if (g_strcmp0(shell, "cmd") == 0 || g_strcmp0(shell, "powershell") == 0
	    || g_strcmp0(shell, "wsl") == 0) {
		gowl_gabbrev_request_return_error(req, "shell: that shell is "
		                                  "Windows-only");
		return;
	}
	path = g_path_is_absolute(shell) ? g_strdup(shell)
	       : g_find_program_in_path(shell);
	if (path == NULL) {
		g_autofree gchar *m = g_strdup_printf("shell: no `%s' on PATH", shell);

		gowl_gabbrev_request_return_error(req, m);
		return;
	}
	add_env(argv, req);
	g_ptr_array_add(argv, g_strdup(path));
	g_ptr_array_add(argv, g_strdup("-c"));
	g_ptr_array_add(argv, g_strdup(cmd));
	spawn_for(self, req, argv, "shell",
	          param_uint(req, "timeout", GABBREV_SHELL_TIMEOUT_MS),
	          param_bool(req, "trim", TRUE));
}

/* `script': params.args run directly, espanso's %CONFIG% and %HOME% */
static void
resolve_script(
	GowlModuleGabbrev  *self,
	GowlGabbrevRequest *req
){
	const gchar *args = gowl_gabbrev_request_get_param(req, "args");
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_auto(GStrv) parts = NULL;
	g_autofree gchar *config = g_build_filename(g_get_user_config_dir(),
	                                            "espanso", NULL);
	guint i;

	if (args == NULL || *args == '\0') {
		gowl_gabbrev_request_return_error(req, "script: params.args is the "
		                                  "command, as a list");
		return;
	}
	add_env(argv, req);
	parts = g_strsplit(args, "\n", -1);
	for (i = 0; parts[i] != NULL; i++) {
		g_auto(GStrv) a = g_strsplit(parts[i], "%CONFIG%", -1);
		g_autofree gchar *b = g_strjoinv(config, a);
		g_auto(GStrv) c = g_strsplit(b, "%HOME%", -1);

		g_ptr_array_add(argv, g_strjoinv(g_get_home_dir(), c));
	}
	spawn_for(self, req, argv, "script",
	          param_uint(req, "timeout", GABBREV_SHELL_TIMEOUT_MS),
	          param_bool(req, "trim", TRUE));
}

/* --- clipboard and selection --- */

static void
on_read_for_request(
	gchar    *text,
	gpointer  data
){
	GowlGabbrevRequest *req = data;

	gowl_gabbrev_request_return(req, text != NULL ? text : "");
	g_free(text);
	gowl_gabbrev_request_unref(req);
}

static void
resolve_clipboard(
	GowlModuleGabbrev  *self,
	GowlGabbrevRequest *req,
	gboolean            primary
){
	GowlSeat *seat = seat_of(self);
	gboolean ok;

	if (seat == NULL) {
		gowl_gabbrev_request_return(req, "");
		return;
	}
	gowl_gabbrev_request_ref(req);
	ok = primary
		? gowl_seat_read_primary_selection_async(seat,
		      gowl_compositor_get_event_loop(self->compositor),
		      on_read_for_request, req)
		: gowl_seat_read_clipboard_async(seat,
		      gowl_compositor_get_event_loop(self->compositor),
		      on_read_for_request, req);
	if (!ok) {
		gowl_gabbrev_request_return(req, "");
		gowl_gabbrev_request_unref(req);
	}
}

typedef struct {
	GowlGabbrevRequest *req;
	gboolean            allow_empty;
} GrabCtx;

static void
grab_ctx_free(gpointer p)
{
	GrabCtx *g = p;

	gowl_gabbrev_request_unref(g->req);
	g_free(g);
}

typedef struct {
	GowlModuleGabbrev *self;
	GrabCtx           *grab;
} GrabRead;

static void
grab_resume(
	GowlModuleGabbrev *self,
	gpointer           data
){
	(void)data;
	resume_steps(self);
}

static void
on_grab_read(
	gchar    *text,
	gpointer  data
){
	GrabRead *r = data;

	if ((text == NULL || *text == '\0') && !r->grab->allow_empty)
		gowl_gabbrev_request_return_error(r->grab->req, "nothing to grab "
		                                  "(the selection was empty)");
	else
		gowl_gabbrev_request_return(r->grab->req, text != NULL ? text : "");
	g_free(text);
	post(r->self, grab_resume, NULL, NULL);
	grab_ctx_free(r->grab);
	g_free(r);
}

/* After the copy chord: read what it put on the clipboard. */
static gboolean
step_grab_read(
	GowlModuleGabbrev *self,
	Job               *job,
	gpointer           data
){
	GrabCtx *g = data;
	GowlSeat *seat = seat_of(self);
	GrabRead *r;

	(void)job;
	if (seat == NULL) {
		gowl_gabbrev_request_return_error(g->req, "no seat");
		return TRUE;
	}
	r = g_new0(GrabRead, 1);
	r->self = self;
	r->grab = g_new0(GrabCtx, 1);
	r->grab->req = gowl_gabbrev_request_ref(g->req);
	r->grab->allow_empty = g->allow_empty;
	if (!gowl_seat_read_clipboard_async(seat,
	        gowl_compositor_get_event_loop(self->compositor),
	        on_grab_read, r)) {
		if (g->allow_empty)
			gowl_gabbrev_request_return(g->req, "");
		else
			gowl_gabbrev_request_return_error(g->req, "nothing to grab "
			                                  "(the copy put nothing on the "
			                                  "clipboard)");
		grab_ctx_free(r->grab);
		g_free(r);
		return TRUE;
	}
	return FALSE;
}

/*
 * `selection': text from the window.  scope primary (the default when
 * nothing is pressed) reads the primary selection; clipboard reads the
 * clipboard; line, word, paragraph and all SELECT that much behind the
 * cursor (Shift+Home, Ctrl+Shift+Left, Ctrl+Shift+Up, Ctrl+a -- or
 * params.select), copy it (Ctrl+c, Ctrl+Shift+c in a terminal, or
 * params.copy) and read the clipboard.  The text stays selected, so the
 * replacement goes over it: grab, rewrite, put back.
 */
static void
resolve_selection(
	GowlModuleGabbrev  *self,
	Job                *job,
	GowlGabbrevRequest *req
){
	const gchar *scope = gowl_gabbrev_request_get_param(req, "scope");
	const gchar *select = gowl_gabbrev_request_get_param(req, "select");
	GrabCtx *g;

	if (scope == NULL)
		scope = select != NULL ? "custom" : "primary";
	if (g_strcmp0(scope, "primary") == 0) {
		resolve_clipboard(self, req, TRUE);
		return;
	}
	if (g_strcmp0(scope, "clipboard") == 0) {
		resolve_clipboard(self, req, FALSE);
		return;
	}
	if (select == NULL) {
		if (g_strcmp0(scope, "line") == 0)
			select = "Shift+Home";
		else if (g_strcmp0(scope, "word") == 0)
			select = "Ctrl+Shift+Left";
		else if (g_strcmp0(scope, "paragraph") == 0)
			select = "Ctrl+Shift+Up";
		else if (g_strcmp0(scope, "all") == 0)
			select = "Ctrl+a";
		else {
			gowl_gabbrev_request_return_error(req, "selection: scope is "
			                                  "primary, clipboard, line, word, "
			                                  "paragraph or all");
			return;
		}
	}
	if (job == NULL || client_by_id(self, job->target_id) == NULL) {
		gowl_gabbrev_request_return_error(req, "selection: no window to "
		                                  "grab from");
		return;
	}
	job->grabbed = TRUE;
	if (!job->have_saved)
		push_step(self, step_new(STEP_SAVE_CLIPBOARD, job));
	{
		g_auto(GStrv) chords = g_strsplit(select, " ", -1);
		guint i;

		for (i = 0; chords[i] != NULL; i++)
			if (*chords[i] != '\0')
				push_chord(self, job, chords[i]);
	}
	push_wait(self, job, 40);
	push_chord(self, job, copy_chord(job, gowl_gabbrev_request_get_params(req)));
	push_wait(self, job, param_uint(req, "settle", GABBREV_GRAB_SETTLE_MS));
	g = g_new0(GrabCtx, 1);
	g->req = gowl_gabbrev_request_ref(req);
	g->allow_empty = param_bool(req, "allow_empty", FALSE);
	push_call(self, job, step_grab_read, g, grab_ctx_free);
	kick_steps(self);
}

/* `window': a fact about the window being typed into */
static void
resolve_window(
	Job                *job,
	GowlGabbrevRequest *req
){
	const gchar *field = gowl_gabbrev_request_get_param(req, "field");
	const gchar *v;

	if (field == NULL)
		field = "app-id";
	v = job != NULL && job->fields != NULL
		? g_hash_table_lookup(job->fields, field) : NULL;
	if (v == NULL) {
		g_autofree gchar *m = g_strdup_printf("window: no field `%s' (app-id, "
		                                      "title, exec, pid, monitor, "
		                                      "layout, tag, ...)", field);

		gowl_gabbrev_request_return_error(req, m);
		return;
	}
	gowl_gabbrev_request_return(req, v);
}

/* `file': a file's text */
static void
resolve_file(GowlGabbrevRequest *req)
{
	const gchar *path = gowl_gabbrev_request_get_param(req, "path");
	g_autofree gchar *full = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;
	gsize len = 0;

	if (path == NULL) {
		gowl_gabbrev_request_return_error(req, "file: params.path names it");
		return;
	}
	full = gabbrev_expand_path(path, NULL);
	if (!g_file_get_contents(full, &text, &len, &error)) {
		gowl_gabbrev_request_return_error(req, error->message);
		return;
	}
	if (len > GABBREV_MAX_FILE_BYTES) {
		gowl_gabbrev_request_return_error(req, "file: larger than 1 MiB");
		return;
	}
	if (param_bool(req, "trim", FALSE))
		g_strstrip(text);
	gowl_gabbrev_request_return(req, text);
}

/* --- pickers: choice, form, search --- */

typedef enum {
	PICK_CHOICE = 0,
	PICK_SEARCH,
	PICK_FORM
} PickKind;

struct _Picker {
	GowlModuleGabbrev  *self;
	Job                *job;
	GowlGabbrevRequest *req;
	PickKind            kind;
	GPtrArray          *labels;    /* gchar*: what is shown */
	GPtrArray          *ids;       /* gchar*: what a pick answers */
	GPtrArray          *matches;   /* GabbrevMatch* refs (search) */
	/* form */
	GPtrArray          *fields;    /* gchar*: names, layout order */
	JsonObject         *specs;     /* name -> {type, values, default} */
	JsonObject         *answers;
	guint               index;
	GabbrevRender      *child;     /* search: the picked match */
};

static void
picker_free(Picker *p)
{
	if (p == NULL)
		return;
	if (p->job != NULL)
		job_unref(p->job);
	gowl_gabbrev_request_unref(p->req);
	if (p->labels != NULL)
		g_ptr_array_unref(p->labels);
	if (p->ids != NULL)
		g_ptr_array_unref(p->ids);
	if (p->matches != NULL)
		g_ptr_array_unref(p->matches);
	if (p->fields != NULL)
		g_ptr_array_unref(p->fields);
	if (p->specs != NULL)
		json_object_unref(p->specs);
	if (p->answers != NULL)
		json_object_unref(p->answers);
	if (p->child != NULL) {
		gabbrev_render_cancel(p->child);
		gabbrev_render_unref(p->child);
	}
	g_free(p);
}

static Picker *
picker_new(
	GowlModuleGabbrev  *self,
	Job                *job,
	GowlGabbrevRequest *req,
	PickKind            kind
){
	Picker *p = g_new0(Picker, 1);

	p->self = self;
	p->job = job != NULL ? job_ref(job) : NULL;
	p->req = gowl_gabbrev_request_ref(req);
	p->kind = kind;
	p->labels = g_ptr_array_new_with_free_func(g_free);
	p->ids = g_ptr_array_new_with_free_func(g_free);
	return p;
}

/* The command a pick or a prompt runs: the setting, or the first of the
   usual launchers found.  $GABBREV_PROMPT is the question; for a pick,
   the rows arrive on stdin, one per line. */
static gchar *
find_command(
	GowlModuleGabbrev *self,
	gboolean           free_text
){
	static const struct {
		const gchar *program;
		const gchar *pick;
		const gchar *prompt;
	} known[] = {
		{ "fuzzel", "fuzzel --dmenu --prompt \"$GABBREV_PROMPT: \"",
		  "fuzzel --dmenu --prompt-only \"$GABBREV_PROMPT: \"" },
		{ "bemenu", "bemenu -p \"$GABBREV_PROMPT\"",
		  "bemenu -p \"$GABBREV_PROMPT\"" },
		{ "wofi", "wofi --dmenu --prompt \"$GABBREV_PROMPT\"",
		  "wofi --dmenu --prompt \"$GABBREV_PROMPT\"" },
		{ "tofi", "tofi --prompt-text \"$GABBREV_PROMPT: \"",
		  "tofi --require-match=false --prompt-text \"$GABBREV_PROMPT: \"" },
		{ "rofi", "rofi -dmenu -p \"$GABBREV_PROMPT\"",
		  "rofi -dmenu -p \"$GABBREV_PROMPT\"" },
		{ "zenity", NULL,
		  "zenity --entry --text \"$GABBREV_PROMPT\"" },
	};
	guint i;

	if (!free_text && self->picker_command != NULL)
		return g_strdup(self->picker_command);
	if (free_text && self->prompt_command != NULL)
		return g_strdup(self->prompt_command);
	for (i = 0; i < G_N_ELEMENTS(known); i++) {
		g_autofree gchar *path = g_find_program_in_path(known[i].program);
		const gchar *cmd = free_text ? known[i].prompt : known[i].pick;

		if (path != NULL && cmd != NULL)
			return g_strdup(cmd);
	}
	return NULL;
}

static void picker_next_field (Picker *p);

static void
picker_free_posted(
	GowlModuleGabbrev *self,
	gpointer           data
){
	(void)self;
	picker_free(data);
}

/* The match picked in a search has rendered: that is the answer.  The
   picker owns the child render, and goes with it. */
static void
on_search_child_done(
	GabbrevRender *child,
	gpointer       data
){
	Picker *p = data;
	const gchar *text = gabbrev_render_get_text(child);

	if (text != NULL)
		gowl_gabbrev_request_return(p->req, text);
	else
		gowl_gabbrev_request_return_error(p->req,
		                                  gabbrev_render_get_error(child));
	/* Not from inside the child's own done notify: free it later */
	post(p->self, picker_free_posted, p, NULL);
}

static void
on_picker_done(
	gint         status,
	const gchar *out,
	const gchar *err,
	gpointer     data
){
	Picker *p = data;
	g_autofree gchar *answer = g_strdup(out != NULL ? out : "");
	guint i;

	(void)err;
	g_strchomp(answer);
	if (status == -1 || !g_spawn_check_wait_status(status, NULL)
	    || (*answer == '\0' && p->kind != PICK_FORM)) {
		gowl_gabbrev_request_return_error(p->req, "cancelled");
		picker_free(p);
		return;
	}

	if (p->kind == PICK_FORM) {
		const gchar *name = g_ptr_array_index(p->fields, p->index);
		JsonObject *spec = json_object_has_member(p->specs, name)
			&& JSON_NODE_HOLDS_OBJECT(json_object_get_member(p->specs, name))
			? json_object_get_object_member(p->specs, name) : NULL;

		if (*answer == '\0' && spec != NULL
		    && json_object_has_member(spec, "default"))
			json_object_set_string_member(p->answers, name,
				json_object_get_string_member(spec, "default"));
		else
			json_object_set_string_member(p->answers, name, answer);
		p->index++;
		picker_next_field(p);
		return;
	}

	/* A pick answers with the id of the row picked; typed text that is
	   no row is taken as it is, for pickers that allow it */
	for (i = 0; i < p->labels->len; i++)
		if (g_strcmp0(g_ptr_array_index(p->labels, i), answer) == 0)
			break;
	if (p->kind == PICK_CHOICE) {
		gowl_gabbrev_request_return(p->req, i < p->ids->len
		                            ? g_ptr_array_index(p->ids, i) : answer);
		picker_free(p);
		return;
	}

	/* search: render the match picked, and answer with its text */
	if (i >= p->matches->len) {
		gowl_gabbrev_request_return_error(p->req, "no such match");
		picker_free(p);
		return;
	}
	{
		GabbrevMatch *m = g_ptr_array_index(p->matches, i);
		GabbrevRenderInfo info;

		memset(&info, 0, sizeof info);
		info.set = p->job->set;
		info.match = m;
		info.app_id = p->job->app_id;
		info.title = p->job->title;
		info.depth = 1;
		p->child = gabbrev_render_new(&info, &render_host, p->job,
		                              on_search_child_done, p);
		gabbrev_render_start(p->child);
	}
}

/* Runs the pick (@rows set) or the prompt (@rows NULL) for @p. */
static void
picker_run(
	Picker      *p,
	GPtrArray   *rows,
	const gchar *prompt
){
	GowlModuleGabbrev *self = p->self;
	g_autofree gchar *cmd = find_command(self, rows == NULL);
	g_autofree gchar *script = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;

	if (cmd == NULL) {
		gowl_gabbrev_request_return_error(p->req, rows != NULL
			? "no picker: install fuzzel, bemenu, wofi, tofi or rofi, or "
			  "set picker-command"
			: "no prompt: install fuzzel, bemenu, wofi, tofi, rofi or "
			  "zenity, or set prompt-command");
		picker_free(p);
		return;
	}
	if (rows != NULL) {
		GString *joined = g_string_new(NULL);
		guint i;

		for (i = 0; i < rows->len; i++) {
			if (i > 0)
				g_string_append_c(joined, '\n');
			g_string_append(joined, g_ptr_array_index(rows, i));
		}
		g_ptr_array_add(argv, g_strdup("env"));
		g_ptr_array_add(argv, g_strdup_printf("GABBREV_ROWS=%s", joined->str));
		g_string_free(joined, TRUE);
		script = g_strdup_printf("printf '%%s\\n' \"$GABBREV_ROWS\" | %s", cmd);
	} else {
		g_ptr_array_add(argv, g_strdup("env"));
		script = g_strdup_printf("%s < /dev/null", cmd);
	}
	g_ptr_array_add(argv, g_strdup_printf("GABBREV_PROMPT=%s", prompt));
	g_ptr_array_add(argv, g_strdup("bash"));
	g_ptr_array_add(argv, g_strdup("-c"));
	g_ptr_array_add(argv, g_steal_pointer(&script));
	g_ptr_array_add(argv, NULL);
	if (gowl_subprocess_spawn(gowl_compositor_get_event_loop(self->compositor),
	                          (const gchar * const *)argv->pdata,
	                          GABBREV_PICKER_TIMEOUT_MS, on_picker_done, p,
	                          &error) == NULL) {
		gowl_gabbrev_request_return_error(p->req, error->message);
		picker_free(p);
	}
}

/* `choice': params.values, texts or {label, id} */
static void
resolve_choice(
	GowlModuleGabbrev  *self,
	Job                *job,
	GowlGabbrevRequest *req
){
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *values = NULL;
	JsonArray *a;
	Picker *p;
	guint i;

	if (json_parser_load_from_data(parser,
	        gowl_gabbrev_request_get_params_json(req), -1, NULL)
	    && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		values = json_object_get_member(
			json_node_get_object(json_parser_get_root(parser)), "values");
	if (values == NULL || !JSON_NODE_HOLDS_ARRAY(values)
	    || json_array_get_length(a = json_node_get_array(values)) == 0) {
		gowl_gabbrev_request_return_error(req, "choice: params.values is a "
		                                  "list");
		return;
	}
	p = picker_new(self, job, req, PICK_CHOICE);
	for (i = 0; i < json_array_get_length(a); i++) {
		JsonNode *e = json_array_get_element(a, i);

		if (JSON_NODE_HOLDS_OBJECT(e)) {
			JsonObject *o = json_node_get_object(e);
			const gchar *label = json_object_has_member(o, "label")
				? json_object_get_string_member(o, "label") : NULL;
			const gchar *id = json_object_has_member(o, "id")
				? json_object_get_string_member(o, "id") : label;

			g_ptr_array_add(p->labels, g_strdup(label != NULL ? label : id));
			g_ptr_array_add(p->ids, g_strdup(id != NULL ? id : ""));
		} else if (JSON_NODE_HOLDS_VALUE(e)) {
			const gchar *s = json_node_get_value_type(e) == G_TYPE_STRING
				? json_node_get_string(e) : NULL;
			g_autofree gchar *text = s != NULL ? g_strdup(s)
				: g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(e));

			g_ptr_array_add(p->labels, g_strdup(text));
			g_ptr_array_add(p->ids, g_steal_pointer(&text));
		}
	}
	if (p->ids->len == 1) {
		gowl_gabbrev_request_return(req, g_ptr_array_index(p->ids, 0));
		picker_free(p);
		return;
	}
	picker_run(p, p->labels, "Choose");
}

/* The next field of a form, or the answer once there are none. */
static void
picker_next_field(Picker *p)
{
	const gchar *name;
	JsonObject *spec;
	const gchar *type;

	if (p->index >= p->fields->len) {
		g_autoptr(JsonNode) n = json_node_new(JSON_NODE_OBJECT);
		g_autofree gchar *text = NULL;

		json_node_set_object(n, p->answers);
		text = json_to_string(n, FALSE);
		gowl_gabbrev_request_return(p->req, text);
		picker_free(p);
		return;
	}
	name = g_ptr_array_index(p->fields, p->index);
	spec = p->specs != NULL && json_object_has_member(p->specs, name)
	       && JSON_NODE_HOLDS_OBJECT(json_object_get_member(p->specs, name))
	       ? json_object_get_object_member(p->specs, name) : NULL;
	type = spec != NULL && json_object_has_member(spec, "type")
	       ? json_object_get_string_member(spec, "type") : "text";
	if ((g_strcmp0(type, "choice") == 0 || g_strcmp0(type, "list") == 0)
	    && json_object_has_member(spec, "values")) {
		JsonNode *vn = json_object_get_member(spec, "values");
		guint i;

		g_ptr_array_set_size(p->labels, 0);
		if (JSON_NODE_HOLDS_ARRAY(vn)) {
			JsonArray *a = json_node_get_array(vn);

			for (i = 0; i < json_array_get_length(a); i++)
				if (JSON_NODE_HOLDS_VALUE(json_array_get_element(a, i)))
					g_ptr_array_add(p->labels, g_strdup(
						json_node_get_string(json_array_get_element(a, i))));
		} else if (JSON_NODE_HOLDS_VALUE(vn)) {
			g_auto(GStrv) lines = g_strsplit(json_node_get_string(vn), "\n", -1);

			for (i = 0; lines[i] != NULL; i++)
				if (*g_strstrip(lines[i]) != '\0')
					g_ptr_array_add(p->labels, g_strdup(lines[i]));
		}
		picker_run(p, p->labels, name);
		return;
	}
	picker_run(p, NULL, name);
}

/* `form': params.layout with [[field]]s, params.fields with their types */
static void
resolve_form(
	GowlModuleGabbrev  *self,
	Job                *job,
	GowlGabbrevRequest *req
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GRegex) re = NULL;
	g_autoptr(GMatchInfo) mi = NULL;
	JsonObject *params = NULL;
	const gchar *layout;
	Picker *p;

	if (json_parser_load_from_data(parser,
	        gowl_gabbrev_request_get_params_json(req), -1, NULL)
	    && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		params = json_node_get_object(json_parser_get_root(parser));
	layout = params != NULL && json_object_has_member(params, "layout")
		? json_object_get_string_member(params, "layout") : NULL;
	if (layout == NULL) {
		gowl_gabbrev_request_return_error(req, "form: params.layout is the "
		                                  "form, with [[field]]s");
		return;
	}
	p = picker_new(self, job, req, PICK_FORM);
	p->fields = g_ptr_array_new_with_free_func(g_free);
	p->answers = json_object_new();
	if (json_object_has_member(params, "fields")
	    && JSON_NODE_HOLDS_OBJECT(json_object_get_member(params, "fields")))
		p->specs = json_object_ref(json_object_get_object_member(params,
		                                                         "fields"));
	re = g_regex_new("\\[\\[\\s*([A-Za-z0-9_\\-]+)\\s*\\]\\]", 0, 0, NULL);
	if (g_regex_match(re, layout, 0, &mi)) {
		while (g_match_info_matches(mi)) {
			gchar *name = g_match_info_fetch(mi, 1);

			if (!g_ptr_array_find_with_equal_func(p->fields, name,
			                                      g_str_equal, NULL))
				g_ptr_array_add(p->fields, name);
			else
				g_free(name);
			g_match_info_next(mi, NULL);
		}
	}
	picker_next_field(p);
}

/* `search': pick any match, by trigger and label, and type it */
static void
resolve_search(
	GowlModuleGabbrev  *self,
	Job                *job,
	GowlGabbrevRequest *req
){
	Picker *p;
	guint i;

	if (job == NULL) {
		gowl_gabbrev_request_return_error(req, "search: no expansion");
		return;
	}
	p = picker_new(self, job, req, PICK_SEARCH);
	p->matches = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gabbrev_match_unref);
	for (i = 0; i < job->set->matches->len; i++) {
		GabbrevMatch *m = g_ptr_array_index(job->set->matches, i);
		g_autofree gchar *what = NULL;
		const gchar *trigger;

		if (g_strcmp0(m->id, "<search>") == 0 || m->triggers->len == 0)
			continue;
		/* A later match with the trigger replaces this one */
		trigger = g_ptr_array_index(m->triggers, 0);
		if (gabbrev_set_find_trigger(job->set, trigger) != m)
			continue;
		if (m->label != NULL) {
			what = g_strdup(m->label);
		} else {
			what = g_strndup(m->replace != NULL ? m->replace : "", 60);
			g_strdelimit(what, "\n\t", ' ');
		}
		g_ptr_array_add(p->labels, g_strdup_printf("%s \xe2\x80\x94 %s",
		                trigger, what));
		g_ptr_array_add(p->matches, gabbrev_match_ref(m));
	}
	picker_run(p, p->labels, "Expand");
}

/* --- `c' --- */

static void
cvar_build_free(CvarBuild *b)
{
	g_free(b->key);
	gabbrev_cvar_spec_clear(&b->spec);
	g_free(b->cache_dir);
	g_free(b->so_path);
	g_clear_error(&b->error);
	g_ptr_array_unref(b->waiters);
	g_free(b);
}

static void
cvar_answer(
	GowlModuleGabbrev  *self,
	const gchar        *key,
	GabbrevCvarFunc     func,
	GowlGabbrevRequest *req
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *v = gabbrev_cvar_call(func, req, self->c_timeout_ms,
	                                         &error);

	if (v != NULL) {
		gowl_gabbrev_request_return(req, v);
		return;
	}
	/* A fault holds the variable back until it changes or reloads */
	if (g_strrstr(error->message, "SIG") != NULL
	    || g_strrstr(error->message, "time") != NULL)
		g_hash_table_replace(self->cvar_faulted, g_strdup(key),
		                     g_strdup(error->message));
	gowl_gabbrev_request_return_error(req, error->message);
}

static void
cvar_built(
	GowlModuleGabbrev *self,
	gpointer           data
){
	CvarBuild *b = data;
	GabbrevCvarFunc func = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	g_hash_table_steal(self->cvar_builds, b->key);
	if (b->so_path != NULL)
		func = gabbrev_cvar_open(b->so_path, &error);
	else
		error = g_error_copy(b->error);
	if (func != NULL)
		g_hash_table_replace(self->cvar_funcs, g_strdup(b->key),
		                     (gpointer)func);
	for (i = 0; i < b->waiters->len; i++) {
		GowlGabbrevRequest *req = g_ptr_array_index(b->waiters, i);

		if (func != NULL)
			cvar_answer(self, b->key, func, req);
		else
			gowl_gabbrev_request_return_error(req, error->message);
	}
	cvar_build_free(b);
}

static gpointer
cvar_thread(gpointer data)
{
	CvarBuild *b = data;

	b->so_path = gabbrev_cvar_build(&b->spec, b->cache_dir, &b->error);
	post(b->self, cvar_built, b, NULL);
	return NULL;
}

static void
resolve_c(
	GowlModuleGabbrev  *self,
	GowlGabbrevRequest *req
){
	const gchar *func_name = gowl_gabbrev_request_get_param(req, "func");
	GabbrevCvarSpec spec;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *key = NULL;
	GabbrevCvarFunc func;
	CvarBuild *b;
	const gchar *faulted;

	if (func_name != NULL) {
		g_autofree gchar *v = gabbrev_cvar_call_registered(func_name, req,
			param_uint(req, "timeout", self->c_timeout_ms), &error);

		if (v != NULL)
			gowl_gabbrev_request_return(req, v);
		else
			gowl_gabbrev_request_return_error(req, error->message);
		return;
	}
	memset(&spec, 0, sizeof spec);
	if (!gabbrev_cvar_spec_from_params(&spec, gowl_gabbrev_request_get_params(req),
	                                   &error)) {
		gowl_gabbrev_request_return_error(req, error->message);
		return;
	}
	key = gabbrev_cvar_spec_key(&spec);
	faulted = g_hash_table_lookup(self->cvar_faulted, key);
	if (faulted != NULL) {
		g_autofree gchar *m = g_strdup_printf("c: held back (it %s); change "
		                                      "it, or gabbrev-reload", faulted);

		gabbrev_cvar_spec_clear(&spec);
		gowl_gabbrev_request_return_error(req, m);
		return;
	}
	/* A file may have changed since: files are always rebuilt-if-stale */
	func = spec.file == NULL ? g_hash_table_lookup(self->cvar_funcs, key) : NULL;
	if (func != NULL) {
		gabbrev_cvar_spec_clear(&spec);
		cvar_answer(self, key, func, req);
		return;
	}
	b = g_hash_table_lookup(self->cvar_builds, key);
	if (b != NULL) {
		gabbrev_cvar_spec_clear(&spec);
		g_ptr_array_add(b->waiters, gowl_gabbrev_request_ref(req));
		return;
	}
	b = g_new0(CvarBuild, 1);
	b->self = self;
	b->key = g_strdup(key);
	b->spec = spec;
	b->cache_dir = g_strdup(self->cache_dir);
	b->waiters = g_ptr_array_new_with_free_func(
		(GDestroyNotify)gowl_gabbrev_request_unref);
	g_ptr_array_add(b->waiters, gowl_gabbrev_request_ref(req));
	g_hash_table_insert(self->cvar_builds, g_strdup(key), b);
	g_thread_unref(g_thread_new("gabbrev-cc", cvar_thread, b));
}

/* --- the dispatch --- */

static gboolean
host_resolve(
	gpointer            host_data,
	GabbrevRender      *render,
	const GabbrevVar   *var,
	GowlGabbrevRequest *req
){
	Job *job = host_data;
	GowlModuleGabbrev *self = job->self;
	g_autoptr(GowlGabbrevHandle) h = NULL;

	(void)render;
	/* Registered types first: an embedder replaces a built-in one */
	h = gowl_gabbrev_lookup_var_type(var->type);
	if (h != NULL) {
		gowl_gabbrev_handle_call_var(h, req);
		return TRUE;
	}
	if (g_strcmp0(var->type, "shell") == 0)
		resolve_shell(self, req);
	else if (g_strcmp0(var->type, "script") == 0)
		resolve_script(self, req);
	else if (g_strcmp0(var->type, "clipboard") == 0)
		resolve_clipboard(self, req, FALSE);
	else if (g_strcmp0(var->type, "selection") == 0)
		resolve_selection(self, job, req);
	else if (g_strcmp0(var->type, "window") == 0)
		resolve_window(job, req);
	else if (g_strcmp0(var->type, "file") == 0)
		resolve_file(req);
	else if (g_strcmp0(var->type, "c") == 0)
		resolve_c(self, req);
	else if (g_strcmp0(var->type, "choice") == 0)
		resolve_choice(self, job, req);
	else if (g_strcmp0(var->type, "form") == 0)
		resolve_form(self, job, req);
	else if (g_strcmp0(var->type, "search") == 0)
		resolve_search(self, job, req);
	else
		return FALSE;
	return TRUE;
}

/* ── Starting expansions from outside the keyboard ────────────────── */

/* A match made on the spot: gabbrev-insert's text, search, undo. */
static GabbrevMatch *
plain_match(
	const gchar *id,
	const gchar *replace
){
	GabbrevMatch *m = gabbrev_match_new();

	m->id = g_strdup(id);
	m->replace = g_strdup(replace != NULL ? replace : "");
	m->source = g_strdup(GABBREV_ORIGIN_RUNTIME);
	return m;
}

static GabbrevMatch *
search_match(void)
{
	GabbrevMatch *m = plain_match("<search>", "{{picked}}");

	m->label = g_strdup("Search the matches");
	g_ptr_array_add(m->vars, gabbrev_var_new("picked", "search", NULL));
	return m;
}

static GowlClient *
focused_client(GowlModuleGabbrev *self)
{
	return self->compositor != NULL
		? gowl_compositor_get_focused_client(self->compositor) : NULL;
}

/* Expands @m into the focused window, typing nothing to rub out. */
static gchar *
expand_now(
	GowlModuleGabbrev *self,
	GabbrevMatch      *m,
	JobOrigin          origin,
	const gchar       *render_id,
	const gchar       *custom
){
	GowlClient *c = focused_client(self);
	Context ctx;
	guint id;

	if (c == NULL && origin != ORIGIN_RENDER)
		return g_strdup("ERROR no window has focus");
	context_init(self, &ctx, c);
	if (ctx.denied && origin != ORIGIN_RENDER) {
		context_clear(&ctx);
		return g_strdup("ERROR the focused window is on the deny list");
	}
	id = job_start(self, m, NULL, c, &ctx, origin, render_id, custom);
	context_clear(&ctx);
	if (id == 0)
		return g_strdup("ERROR too many expansions running");
	return g_strdup_printf("OK started %u %s", id, m->id);
}

static void
set_on(
	GowlModuleGabbrev *self,
	gboolean           on,
	const gchar       *why
){
	if (self->on == on)
		return;
	self->on = on;
	gabbrev_engine_reset(self->engine);
	self->last.valid = FALSE;
	gab_event(self, "%s %s", on ? "on" : "off", why);
	gab_notify(self, on ? "Text expansion on" : "Text expansion off", NULL);
}

static void
cancel_all(GowlModuleGabbrev *self)
{
	while (self->jobs->len > 0) {
		Job *j = g_ptr_array_index(self->jobs, 0);

		drop_steps(self, j);
		job_retire(self, j);
	}
	g_queue_clear_full(self->steps, (GDestroyNotify)step_free);
	self->paused = FALSE;
}

/* ── Undo ──────────────────────────────────────────────────────────── */

static gboolean
step_retire(
	GowlModuleGabbrev *self,
	Job               *job,
	gpointer           data
){
	(void)data;
	job_retire(self, job);
	return TRUE;
}

/* Backspace right after an expansion: the trigger comes back. */
static void
do_undo(
	GowlModuleGabbrev *self,
	GowlClient        *c
){
	g_autoptr(GabbrevMatch) m = plain_match("<undo>", "");
	Job *job;
	Context ctx;

	context_init(self, &ctx, c);
	job = g_new0(Job, 1);
	job->ref_count = 1;
	job->id = ++self->next_job_id;
	job->self = self;
	job->origin = ORIGIN_EXPAND;
	job->set = gabbrev_set_ref(self->set);
	job->match = gabbrev_match_ref(m);
	job->target_id = gowl_client_get_id(c);
	effective_options(self, ctx.profile, &job->opts);
	context_clear(&ctx);
	g_ptr_array_add(self->jobs, job);

	push_key(self, job, KEY_BACKSPACE, 0, 0, self->last.n_typed);
	push_text(self, job, self->last.restore);
	push_call(self, job, step_retire, NULL, NULL);
	g_free(self->last.restore);
	memset(&self->last, 0, sizeof self->last);
	gabbrev_engine_reset(self->engine);
	gab_event(self, "undone");
	kick_steps(self);
}

/* ── The keyboard ──────────────────────────────────────────────────── */

static gboolean
key_is(
	const GowlInputObserverKey *key,
	guint32                     mods,
	guint32                     sym
){
	return sym != 0 && key->modifiers == mods
	       && (xkb_keysym_to_lower(key->base_keysym) == sym
	           || xkb_keysym_to_lower(key->keysym) == sym);
}

static gboolean
gab_filter_key(
	GowlInputObserver          *observer,
	const GowlInputObserverKey *key
){
	GowlModuleGabbrev *self = GOWL_MODULE_GABBREV(observer);
	gpointer kc = GUINT_TO_POINTER(key->keycode);
	guint32 sym = key->base_keysym;

	if (!self->active || self->compositor == NULL || self->set == NULL)
		return FALSE;
	if (!key->pressed)
		return g_hash_table_remove(self->swallowed, kc);

	/* espanso's toggle: a modifier, twice, alone */
	if (self->toggle_tap_sym != 0
	    && (sym == self->toggle_tap_sym || sym == self->toggle_tap_sym2)) {
		gint64 t = now_us();

		if (self->tap_count == 1 && self->tap_sym == sym
		    && t - self->tap_time_us < GABBREV_DOUBLE_TAP_MS * 1000) {
			self->tap_count = 0;
			set_on(self, !self->on, "toggle-key");
		} else {
			self->tap_count = 1;
			self->tap_sym = sym;
			self->tap_time_us = t;
		}
		return FALSE;   /* the modifier still does its own job */
	}
	self->tap_count = 0;

	if (key_is(key, self->toggle_mods, self->toggle_sym)) {
		g_hash_table_add(self->swallowed, kc);
		set_on(self, !self->on, "toggle-key");
		return TRUE;
	}
	if (self->on && key_is(key, self->search_mods, self->search_sym)) {
		g_autoptr(GabbrevMatch) m = search_match();
		g_autofree gchar *r = NULL;

		g_hash_table_add(self->swallowed, kc);
		r = expand_now(self, m, ORIGIN_EXPAND, NULL, NULL);
		if (!g_str_has_prefix(r, "OK"))
			gab_notify(self, "Search", r);
		return TRUE;
	}

	/* Backspace straight after an expansion undoes it */
	if (self->last.valid) {
		gboolean undo = sym == XKB_KEY_BackSpace && key->modifiers == 0
			&& key->focused != NULL
			&& gowl_client_get_id(GOWL_CLIENT(key->focused))
			   == self->last.target_id
			&& now_us() - self->last.when_us < GABBREV_UNDO_MS * 1000;

		if (!is_modifier_sym(sym))
			self->last.valid = FALSE;
		if (undo && self->on) {
			g_hash_table_add(self->swallowed, kc);
			do_undo(self, GOWL_CLIENT(key->focused));
			return TRUE;
		}
	}
	return FALSE;
}

/* A key reached the window: what does it mean for what was typed? */
static void
gab_key_forwarded(
	GowlInputObserver          *observer,
	const GowlInputObserverKey *key
){
	GowlModuleGabbrev *self = GOWL_MODULE_GABBREV(observer);
	GowlClient *c;
	guint id;
	guint32 sym;
	gunichar ch;
	Context ctx;
	GabbrevHit *hit;
	guint i;

	if (!self->active || !self->on || self->compositor == NULL
	    || !key->pressed)
		return;
	if (key->synthetic && !self->expand_synthetic)
		return;
	reload_if_stale(self);
	if (self->set == NULL)
		return;

	c = key->focused != NULL ? GOWL_CLIENT(key->focused) : NULL;
	if (c == NULL) {
		gabbrev_engine_reset(self->engine);
		self->typing_target = 0;
		return;
	}
	id = gowl_client_get_id(c);
	if (id != self->typing_target) {
		gabbrev_engine_reset(self->engine);
		self->typing_target = id;
	}

	sym = key->keysym;
	if (is_modifier_sym(sym))
		return;

	/* Typing on into a window whose expansion has not been typed yet:
	   where it would go has moved, so it is dropped */
	for (i = 0; i < self->jobs->len; ) {
		Job *j = g_ptr_array_index(self->jobs, i);

		if (j->origin == ORIGIN_TYPED && j->target_id == id && !j->injecting
		    && !j->finished) {
			/* What was typed is the user's now: nothing is typed back
			   over it, not even the trigger */
			j->deleted = FALSE;
			j->grabbed = FALSE;
			job_fail(self, j, "you went on typing before it was ready");
			continue;
		}
		i++;
	}

	if ((key->modifiers & (GOWL_KEY_MOD_CTRL | GOWL_KEY_MOD_ALT
	                       | GOWL_KEY_MOD_LOGO)) != 0) {
		gabbrev_engine_reset(self->engine);
		return;
	}
	if (sym == XKB_KEY_BackSpace) {
		gabbrev_engine_backspace(self->engine);
		return;
	}
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)
		ch = '\n';
	else if (sym == XKB_KEY_Tab)
		ch = '\t';
	else
		ch = xkb_keysym_to_utf32(sym);
	if (ch == 0 || (ch < 0x20 && ch != '\n' && ch != '\t') || ch == 0x7f) {
		/* arrows, Home, F-keys, Escape: the cursor went elsewhere */
		gabbrev_engine_reset(self->engine);
		return;
	}

	context_init(self, &ctx, c);
	if (ctx.denied || ctx.disabled) {
		gabbrev_engine_reset(self->engine);
		context_clear(&ctx);
		return;
	}
	if (ctx.profile != NULL
	    && (ctx.profile->options.set & GABBREV_OPT_WORD_SEPARATORS))
		gabbrev_engine_set_separators(self->engine,
		                              ctx.profile->options.word_separators);
	else
		gabbrev_engine_set_separators(self->engine,
		                              self->set->options.word_separators);
	hit = gabbrev_engine_feed(self->engine, ch, match_allowed, &ctx);
	if (hit != NULL) {
		gab_log(self, "`%s' completes %s in %s", hit->typed, hit->match->id,
		        gowl_client_get_app_id(c) ? gowl_client_get_app_id(c) : "?");
		self->in_key = TRUE;
		job_start(self, hit->match, hit, c, &ctx, ORIGIN_TYPED, NULL, NULL);
		self->in_key = FALSE;
		gabbrev_hit_free(hit);
	}
	context_clear(&ctx);
}

/* A click moves the cursor somewhere the engine cannot see. */
static void
gab_button_forwarded(
	GowlInputObserver             *observer,
	const GowlInputObserverButton *button
){
	GowlModuleGabbrev *self = GOWL_MODULE_GABBREV(observer);

	if (button->pressed) {
		gabbrev_engine_reset(self->engine);
		self->last.valid = FALSE;
	}
}

static void
gab_observer_init(GowlInputObserverInterface *iface)
{
	iface->filter_key       = gab_filter_key;
	iface->key_forwarded    = gab_key_forwarded;
	iface->button_forwarded = gab_button_forwarded;
}

/* ── IPC ───────────────────────────────────────────────────────────── */

static gchar *
json_reply(JsonBuilder *b)
{
	g_autoptr(JsonNode) root = json_builder_get_root(b);
	g_autofree gchar *text = json_to_string(root, FALSE);

	return g_strdup_printf("OK %s", text);
}

static void
add_str(
	JsonBuilder *b,
	const gchar *key,
	const gchar *value
){
	json_builder_set_member_name(b, key);
	if (value != NULL)
		json_builder_add_string_value(b, value);
	else
		json_builder_add_null_value(b);
}

static void
add_strv(
	JsonBuilder *b,
	const gchar *key,
	gchar      **v
){
	guint i;

	json_builder_set_member_name(b, key);
	json_builder_begin_array(b);
	for (i = 0; v != NULL && v[i] != NULL; i++)
		json_builder_add_string_value(b, v[i]);
	json_builder_end_array(b);
}

static const gchar *
job_state(Job *j)
{
	if (j->finished)
		return "finishing";
	if (j->injecting)
		return "typing";
	return "resolving";
}

static gchar *
cmd_status(GowlModuleGabbrev *self)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_auto(GStrv) types = gowl_gabbrev_list_var_types();
	g_auto(GStrv) funcs = gowl_gabbrev_list_funcs();
	guint i;

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "active");
	json_builder_add_boolean_value(b, self->active);
	json_builder_set_member_name(b, "on");
	json_builder_add_boolean_value(b, self->on);
	json_builder_set_member_name(b, "matches");
	json_builder_add_int_value(b, self->set != NULL ? self->set->matches->len : 0);
	json_builder_set_member_name(b, "files");
	json_builder_begin_array(b);
	for (i = 0; self->set != NULL && i < self->set->files->len; i++)
		json_builder_add_string_value(b, g_ptr_array_index(self->set->files, i));
	json_builder_end_array(b);
	json_builder_set_member_name(b, "problems");
	json_builder_begin_array(b);
	for (i = 0; self->set != NULL && i < self->set->problems->len; i++)
		json_builder_add_string_value(b,
			g_ptr_array_index(self->set->problems, i));
	json_builder_end_array(b);
	json_builder_set_member_name(b, "profiles");
	json_builder_begin_array(b);
	for (i = 0; self->set != NULL && i < self->set->profiles->len; i++) {
		GabbrevProfile *p = g_ptr_array_index(self->set->profiles, i);

		json_builder_add_string_value(b, p->name != NULL ? p->name : "");
	}
	json_builder_end_array(b);
	json_builder_set_member_name(b, "runtime");
	json_builder_begin_array(b);
	for (i = 0; i < self->runtime->len; i++) {
		RuntimeSource *r = g_ptr_array_index(self->runtime, i);

		json_builder_add_string_value(b, r->name);
	}
	json_builder_end_array(b);
	json_builder_set_member_name(b, "jobs");
	json_builder_begin_array(b);
	for (i = 0; i < self->jobs->len; i++) {
		Job *j = g_ptr_array_index(self->jobs, i);
		const gchar *wait = j->render != NULL
			? gabbrev_render_get_pending_type(j->render) : NULL;

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "id");
		json_builder_add_int_value(b, j->id);
		add_str(b, "match", j->match->id);
		add_str(b, "state", job_state(j));
		add_str(b, "waiting-for", wait);
		json_builder_set_member_name(b, "seconds");
		json_builder_add_double_value(b, (now_us() - j->started_us) / 1e6);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	json_builder_set_member_name(b, "expanded");
	json_builder_add_int_value(b, (gint64)self->n_expanded);
	json_builder_set_member_name(b, "failed");
	json_builder_add_int_value(b, (gint64)self->n_failed);
	add_str(b, "last-error", self->last_error);
	add_str(b, "deny-filter", self->deny_text);
	{
		g_autofree gchar *pick = find_command(self, FALSE);
		g_autofree gchar *prompt = find_command(self, TRUE);

		add_str(b, "picker", pick);
		add_str(b, "prompt", prompt);
	}
	add_str(b, "toggle-key", self->set != NULL
	        ? self->set->options.toggle_key : NULL);
	add_str(b, "search-shortcut", self->set != NULL
	        ? self->set->options.search_shortcut : NULL);
	add_str(b, "search-trigger", self->set != NULL
	        ? self->set->options.search_trigger : NULL);
	add_strv(b, "registered-types", types);
	add_strv(b, "registered-funcs", funcs);
	json_builder_end_object(b);
	return json_reply(b);
}

static gchar *
cmd_list(
	GowlModuleGabbrev *self,
	const gchar       *filter
){
	g_autoptr(JsonBuilder) b = json_builder_new();
	guint i;

	json_builder_begin_array(b);
	for (i = 0; self->set != NULL && i < self->set->matches->len; i++) {
		GabbrevMatch *m = g_ptr_array_index(self->set->matches, i);
		g_autofree gchar *preview = NULL;
		guint j;

		if (filter != NULL && *filter != '\0'
		    && strstr(m->id, filter) == NULL
		    && (m->label == NULL || strstr(m->label, filter) == NULL))
			continue;
		json_builder_begin_object(b);
		add_str(b, "id", m->id);
		json_builder_set_member_name(b, "triggers");
		json_builder_begin_array(b);
		for (j = 0; j < m->triggers->len; j++)
			json_builder_add_string_value(b, g_ptr_array_index(m->triggers, j));
		json_builder_end_array(b);
		add_str(b, "regex", m->regex_text);
		add_str(b, "label", m->label);
		preview = m->replace != NULL ? g_strndup(m->replace, 200) : NULL;
		add_str(b, "replace", preview);
		add_str(b, "source", m->source);
		add_str(b, "filter", m->filter_text);
		json_builder_set_member_name(b, "word");
		json_builder_add_boolean_value(b, m->left_word && m->right_word);
		json_builder_set_member_name(b, "propagate-case");
		json_builder_add_boolean_value(b, m->propagate_case);
		json_builder_set_member_name(b, "search-only");
		json_builder_add_boolean_value(b, m->search_only);
		json_builder_set_member_name(b, "overridden");
		json_builder_add_boolean_value(b, m->triggers->len > 0
			&& gabbrev_set_find_trigger(self->set,
			                            g_ptr_array_index(m->triggers, 0)) != m);
		json_builder_set_member_name(b, "vars");
		json_builder_begin_array(b);
		for (j = 0; j < m->vars->len; j++) {
			GabbrevVar *v = g_ptr_array_index(m->vars, j);

			json_builder_begin_object(b);
			add_str(b, "name", v->name);
			add_str(b, "type", v->type);
			json_builder_end_object(b);
		}
		json_builder_end_array(b);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	return json_reply(b);
}

/* What TEXT, typed into the focused window, would fire (no typing) */
static gchar *
cmd_test(
	GowlModuleGabbrev *self,
	const gchar       *text
){
	g_autoptr(GabbrevEngine) e = gabbrev_engine_new();
	g_autoptr(JsonBuilder) b = json_builder_new();
	GowlClient *c = focused_client(self);
	Context ctx;
	const gchar *p;
	GabbrevHit *hit = NULL;

	gabbrev_engine_set_matches(e, self->set->matches);
	gabbrev_engine_set_separators(e, self->set->options.word_separators);
	gabbrev_engine_set_regex_buffer(e, self->set->options.regex_buffer);
	context_init(self, &ctx, c);
	for (p = text; *p != '\0' && hit == NULL; p = g_utf8_next_char(p))
		hit = gabbrev_engine_feed(e, g_utf8_get_char(p), match_allowed, &ctx);

	json_builder_begin_object(b);
	json_builder_set_member_name(b, "denied");
	json_builder_add_boolean_value(b, ctx.denied);
	json_builder_set_member_name(b, "disabled");
	json_builder_add_boolean_value(b, ctx.disabled);
	add_str(b, "profile", ctx.profile != NULL ? ctx.profile->name : NULL);
	add_str(b, "match", hit != NULL ? hit->match->id : NULL);
	add_str(b, "typed", hit != NULL ? hit->typed : NULL);
	json_builder_set_member_name(b, "delete");
	json_builder_add_int_value(b, hit != NULL ? hit->n_delete : 0);
	add_str(b, "trailing", hit != NULL ? hit->trailing : NULL);
	json_builder_set_member_name(b, "rest");
	json_builder_add_string_value(b, p);
	json_builder_end_object(b);
	gabbrev_hit_free(hit);
	context_clear(&ctx);
	return json_reply(b);
}

static void
runtime_source_free(RuntimeSource *r)
{
	g_free(r->name);
	g_free(r->text);
	g_free(r);
}

/* Adds (or replaces, by name) a runtime source. */
static void
runtime_put(
	GowlModuleGabbrev *self,
	const gchar       *name,
	gboolean           is_file,
	const gchar       *text
){
	RuntimeSource *r;
	guint i;

	for (i = 0; i < self->runtime->len; i++) {
		r = g_ptr_array_index(self->runtime, i);
		if (g_strcmp0(r->name, name) == 0) {
			r->is_file = is_file;
			g_free(r->text);
			r->text = g_strdup(text);
			return;
		}
	}
	r = g_new0(RuntimeSource, 1);
	r->name = g_strdup(name);
	r->is_file = is_file;
	r->text = g_strdup(text);
	g_ptr_array_add(self->runtime, r);
}

/* `--name NAME rest' (or just `rest'); returns the rest */
static const gchar *
take_name(
	const gchar  *arg,
	gchar       **out_name,
	const gchar  *fallback
){
	*out_name = NULL;
	if (g_str_has_prefix(arg, "--name ")) {
		const gchar *p = arg + strlen("--name ");
		const gchar *sp = strchr(p, ' ');

		if (sp != NULL) {
			*out_name = g_strndup(p, (gsize)(sp - p));
			return sp + 1;
		}
		*out_name = g_strdup(p);
		return "";
	}
	*out_name = g_strdup(fallback);
	return arg;
}

static GabbrevMatch *
find_for_expand(
	GowlModuleGabbrev *self,
	const gchar       *what
){
	return self->set != NULL ? gabbrev_set_find_id(self->set, what) : NULL;
}

static gchar *
gab_handle_command(
	GowlIpcHandler *handler,
	const gchar    *command,
	const gchar    *args
){
	GowlModuleGabbrev *self;
	const gchar *verb;
	g_autofree gchar *arg = NULL;

	if (command == NULL || !g_str_has_prefix(command, GABBREV_PREFIX))
		return NULL;
	self = GOWL_MODULE_GABBREV(handler);
	verb = command + strlen(GABBREV_PREFIX);
	arg = g_strstrip(g_strdup(args != NULL ? args : ""));

	reload_if_stale(self);

	if (g_strcmp0(verb, "status") == 0)
		return cmd_status(self);
	if (g_strcmp0(verb, "list") == 0)
		return cmd_list(self, arg);
	if (g_strcmp0(verb, "builtin") == 0) {
		g_autofree gchar *q = json_quote(gabbrev_builtin_yaml());

		return g_strdup_printf("OK %s", q);
	}
	if (g_strcmp0(verb, "test") == 0)
		return cmd_test(self, arg);
	if (g_strcmp0(verb, "resolve") == 0 || g_strcmp0(verb, "resolve-error") == 0) {
		gchar *end = NULL;
		guint64 id = g_ascii_strtoull(arg, &end, 10);
		g_autofree gchar *value = NULL;

		if (end == arg) {
			return g_strdup_printf("ERROR usage: gabbrev-%s ID %s", verb,
			                       g_strcmp0(verb, "resolve") == 0
			                       ? "JSON-STRING" : "MESSAGE");
		}
		while (*end == ' ')
			end++;
		if (g_strcmp0(verb, "resolve") == 0) {
			g_autoptr(JsonParser) p = json_parser_new();

			/* A JSON string carries newlines on one line; anything
			   else is taken as it is */
			if (json_parser_load_from_data(p, end, -1, NULL)
			    && JSON_NODE_HOLDS_VALUE(json_parser_get_root(p))
			    && json_node_get_value_type(json_parser_get_root(p))
			       == G_TYPE_STRING)
				value = g_strdup(json_node_get_string(json_parser_get_root(p)));
			else
				value = g_strdup(end);
			return gowl_gabbrev_request_return_by_id(id, value, NULL)
				? g_strdup("OK resolved")
				: g_strdup("ERROR no request waits with that id");
		}
		return gowl_gabbrev_request_return_by_id(id, NULL, end)
			? g_strdup("OK failed")
			: g_strdup("ERROR no request waits with that id");
	}
	if (g_strcmp0(verb, "on") == 0) {
		set_on(self, TRUE, "ipc");
		return g_strdup("OK on");
	}
	if (g_strcmp0(verb, "off") == 0) {
		set_on(self, FALSE, "ipc");
		return g_strdup("OK off");
	}
	if (g_strcmp0(verb, "toggle") == 0) {
		set_on(self, !self->on, "ipc");
		return g_strdup(self->on ? "OK on" : "OK off");
	}
	if (g_strcmp0(verb, "reload") == 0) {
		g_hash_table_remove_all(self->cvar_faulted);
		reload(self);
		return g_strdup_printf("OK %u match(es), %u problem(s)",
		                       self->set->matches->len,
		                       self->set->problems->len);
	}
	if (g_strcmp0(verb, "add") == 0 || g_strcmp0(verb, "load") == 0) {
		g_autofree gchar *name = NULL;
		gboolean is_file = g_strcmp0(verb, "load") == 0;
		const gchar *rest = take_name(arg, &name, NULL);
		guint before;

		if (*rest == '\0')
			return g_strdup_printf("ERROR usage: gabbrev-%s [--name NAME] %s",
			                       verb, is_file ? "PATH" : "YAML");
		if (!is_file) {
			g_autoptr(YamlParser) p = yaml_parser_new();
			g_autoptr(GError) error = NULL;

			if (!yaml_parser_load_from_data(p, rest, -1, &error))
				return g_strdup_printf("ERROR %s", error->message);
		}
		if (name == NULL)
			name = g_strdup_printf("%s-%u", verb, self->runtime->len + 1);
		before = self->set->problems->len;
		runtime_put(self, name, is_file,
		            is_file ? rest : rest);
		reload(self);
		return g_strdup_printf("OK %s: %u match(es) in all, %u problem(s)%s",
		                       name, self->set->matches->len,
		                       self->set->problems->len,
		                       self->set->problems->len > before
		                       ? " (see gabbrev-status)" : "");
	}
	if (g_strcmp0(verb, "remove") == 0) {
		guint i;

		for (i = 0; i < self->runtime->len; i++) {
			RuntimeSource *r = g_ptr_array_index(self->runtime, i);

			if (g_strcmp0(r->name, arg) == 0) {
				g_ptr_array_remove_index(self->runtime, i);
				reload(self);
				return g_strdup_printf("OK removed %s", arg);
			}
		}
		return g_strdup_printf("ERROR no runtime source `%s'", arg);
	}
	if (g_strcmp0(verb, "clear") == 0) {
		g_ptr_array_set_size(self->runtime, 0);
		reload(self);
		return g_strdup("OK cleared");
	}
	if (g_strcmp0(verb, "cancel") == 0) {
		guint n = self->jobs->len;

		cancel_all(self);
		return g_strdup_printf("OK cancelled %u", n);
	}
	if (!self->active)
		return g_strdup("ERROR the gabbrev module is not active");
	if (g_strcmp0(verb, "expand") == 0) {
		GabbrevMatch *m = find_for_expand(self, arg);

		if (m == NULL)
			return g_strdup_printf("ERROR no match `%s'", arg);
		return expand_now(self, m, ORIGIN_EXPAND, NULL, NULL);
	}
	if (g_strcmp0(verb, "insert") == 0) {
		g_autoptr(GabbrevMatch) m = plain_match("<insert>", arg);

		return expand_now(self, m, ORIGIN_EXPAND, NULL, NULL);
	}
	if (g_strcmp0(verb, "search") == 0) {
		g_autoptr(GabbrevMatch) m = search_match();

		return expand_now(self, m, ORIGIN_EXPAND, NULL, NULL);
	}
	if (g_strcmp0(verb, "render") == 0) {
		g_autofree gchar *rid = NULL;
		g_autofree gchar *custom = NULL;
		const gchar *rest = arg;
		GabbrevMatch *m;

		for (;;) {
			if (g_str_has_prefix(rest, "--id ")) {
				const gchar *v = rest + 5;
				const gchar *sp = strchr(v, ' ');

				if (sp == NULL)
					return g_strdup("ERROR usage: gabbrev-render [--id ID] "
					                "[--custom FORM] TRIGGER");
				g_free(rid);
				rid = g_strndup(v, (gsize)(sp - v));
				rest = sp + 1;
			} else if (g_str_has_prefix(rest, "--custom ")) {
				/* The form runs to ` -- ' */
				const gchar *v = rest + 9;
				const gchar *sep = strstr(v, " -- ");

				if (sep == NULL)
					return g_strdup("ERROR --custom FORM ends with ` -- '");
				g_free(custom);
				custom = g_strndup(v, (gsize)(sep - v));
				rest = sep + 4;
			} else {
				break;
			}
		}
		m = find_for_expand(self, rest);
		if (m == NULL)
			return g_strdup_printf("ERROR no match `%s'", rest);
		return expand_now(self, m, ORIGIN_RENDER, rid, custom);
	}
	return g_strdup_printf("ERROR unknown command %s: gabbrev-status, -list, "
	                       "-on, -off, -toggle, -reload, -load, -add, "
	                       "-remove, -clear, -expand, -insert, -render, "
	                       "-test, -search, -resolve, -resolve-error, "
	                       "-cancel, -builtin", command);
}

static void
gab_ipc_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = gab_handle_command;
}

/* ── Settings ──────────────────────────────────────────────────────── */

static void
set_deny(
	GowlModuleGabbrev *self,
	const gchar       *text
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *combined = NULL;
	GowlFilter *f = NULL;

	/* `+EXPR' widens the default rather than replacing it */
	if (text != NULL && text[0] == '+') {
		combined = g_strdup_printf("(%s) or (%s)", GABBREV_DEFAULT_DENY,
		                           text + 1);
		text = combined;
	}
	if (text != NULL && *text != '\0' && g_ascii_strcasecmp(text, "none") != 0) {
		f = gowl_filter_parse(text, gabbrev_filter_fields(), &error);
		if (f == NULL) {
			g_warning("gabbrev: deny-filter: %s; keeping the old one",
			          error->message);
			return;
		}
	}
	gowl_filter_free(self->deny);
	self->deny = f;
	g_free(self->deny_text);
	self->deny_text = g_strdup(f != NULL ? text : "none");
}

/*
 * The module's own settings, from `modules: gabbrev:'.  The matches
 * and espanso's options live in the top-level `gabbrev:' section (or
 * any file it names); these are about the module:
 *
 *   start-on          expand from the start (true)
 *   builtin           load the shipped matches (true)
 *   files             more match files / dirs / espanso dirs (a list)
 *   deny-filter       windows never expanded into (password managers...);
 *                     `+EXPR' adds EXPR to the default, `none' clears it
 *   terminals         app-ids that paste with Ctrl+Shift+V (`+a,b' adds)
 *   picker-command    the dmenu-style picker, rows on stdin
 *   prompt-command    the free-text prompt
 *   expand-synthetic  expand KVM and other injected typing too (true)
 *   max-jobs          expansions resolving at once (8)
 *   c-timeout-ms      a `c' variable's budget (500)
 *   cache-dir         where `c' variables are built
 *   toast, log        notifications, and a log line per expansion
 */
static void
gab_configure(
	GowlModule *mod,
	gpointer    config
){
	GowlModuleGabbrev *self = GOWL_MODULE_GABBREV(mod);
	GHashTable *settings = config;
	const gchar *v;
	gboolean b;
	guint64 n;

	if (settings == NULL)
		return;
#define BOOL_SETTING(key, field) \
	if ((v = g_hash_table_lookup(settings, key)) != NULL) { \
		if (parse_bool_setting(v, &b)) \
			self->field = b; \
		else \
			g_warning("gabbrev: " key " `%s' is no yes/no", v); \
	}
	BOOL_SETTING("start-on", start_on)
	BOOL_SETTING("builtin", builtin)
	BOOL_SETTING("toast", toast)
	BOOL_SETTING("log", log)
	BOOL_SETTING("expand-synthetic", expand_synthetic)
#undef BOOL_SETTING

	if ((v = g_hash_table_lookup(settings, "files")) != NULL) {
		g_strfreev(self->setting_files);
		self->setting_files = g_strsplit(v, "\n", -1);
	}
	if ((v = g_hash_table_lookup(settings, "deny-filter")) != NULL)
		set_deny(self, v);
	if ((v = g_hash_table_lookup(settings, "terminals")) != NULL) {
		g_autofree gchar *list = v[0] == '+'
			? g_strdup_printf("%s,%s", GABBREV_DEFAULT_TERMINALS, v + 1)
			: g_strdup(v);

		g_strfreev(self->terminals);
		self->terminals = g_strsplit_set(list, ",\n ", -1);
	}
	if ((v = g_hash_table_lookup(settings, "picker-command")) != NULL) {
		g_free(self->picker_command);
		self->picker_command = *v != '\0' ? g_strdup(v) : NULL;
	}
	if ((v = g_hash_table_lookup(settings, "prompt-command")) != NULL) {
		g_free(self->prompt_command);
		self->prompt_command = *v != '\0' ? g_strdup(v) : NULL;
	}
	if ((v = g_hash_table_lookup(settings, "max-jobs")) != NULL) {
		if (g_ascii_string_to_unsigned(v, 10, 1, 64, &n, NULL))
			self->max_jobs = (guint)n;
		else
			g_warning("gabbrev: max-jobs `%s' -- 1..64", v);
	}
	if ((v = g_hash_table_lookup(settings, "c-timeout-ms")) != NULL) {
		if (g_ascii_string_to_unsigned(v, 10, 10, 60000, &n, NULL))
			self->c_timeout_ms = (guint)n;
		else
			g_warning("gabbrev: c-timeout-ms `%s' -- 10..60000", v);
	}
	if ((v = g_hash_table_lookup(settings, "cache-dir")) != NULL && *v != '\0') {
		g_free(self->cache_dir);
		self->cache_dir = gabbrev_expand_path(v, NULL);
	}
	if (self->compositor != NULL)
		reload(self);
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

static gboolean
gab_activate(GowlModule *mod)
{
	GowlModuleGabbrev *self = GOWL_MODULE_GABBREV(mod);

	self->active = TRUE;
	self->on = self->start_on;
	/* Enabled after startup (cmacs's gowl-enable-module): the compositor
	   is known already; at first start the startup handler does this */
	if (self->compositor != NULL) {
		post_open(self);
		sync_config(self);
	}
	return TRUE;
}

static void
gab_deactivate(GowlModule *mod)
{
	GowlModuleGabbrev *self = GOWL_MODULE_GABBREV(mod);

	cancel_all(self);
	gabbrev_engine_reset(self->engine);
	self->active = FALSE;
	self->on = FALSE;
}

static const gchar *
gab_get_name(GowlModule *mod)
{
	(void)mod;
	return "gabbrev";
}

static const gchar *
gab_get_description(GowlModule *mod)
{
	(void)mod;
	return "Text expansion: type a trigger, get its replacement "
	       "(espanso-compatible)";
}

static const gchar *
gab_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/* The timers and the post fd belong to the display's event loop. */
static void
gab_display_destroyed(
	struct wl_listener *listener,
	void               *data
){
	GowlModuleGabbrev *self;

	(void)data;
	self = wl_container_of(listener, self, display_destroy);
	cancel_all(self);
	if (self->step_timer != NULL) {
		wl_event_source_remove(self->step_timer);
		self->step_timer = NULL;
	}
	post_close(self);
	wl_list_remove(&self->display_destroy.link);
	wl_list_init(&self->display_destroy.link);
}

static void
gab_on_startup(
	GowlStartupHandler *handler,
	gpointer            compositor
){
	GowlModuleGabbrev *self = GOWL_MODULE_GABBREV(handler);
	struct wl_display *display;

	if (self->compositor == NULL) {
		self->compositor = GOWL_COMPOSITOR(compositor);
		g_object_add_weak_pointer(G_OBJECT(compositor),
		                          (gpointer *)&self->compositor);
	}
	display = gowl_compositor_get_wl_display(self->compositor);
	if (display != NULL && wl_list_empty(&self->display_destroy.link))
		wl_display_add_destroy_listener(display, &self->display_destroy);
	post_open(self);
	sync_config(self);
	gab_log(self, "started: %u match(es), %s",
	        self->set != NULL ? self->set->matches->len : 0,
	        self->on ? "on" : "off");
}

static void
gab_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = gab_on_startup;
}

static void
gab_on_shutdown(
	GowlShutdownHandler *handler,
	gpointer             compositor
){
	GowlModuleGabbrev *self = GOWL_MODULE_GABBREV(handler);

	(void)compositor;
	cancel_all(self);
	self->on = FALSE;
}

static void
gab_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = gab_on_shutdown;
}

static void
gowl_module_gabbrev_finalize(GObject *object)
{
	GowlModuleGabbrev *self = GOWL_MODULE_GABBREV(object);

	cancel_all(self);
	wl_list_remove(&self->display_destroy.link);
	if (self->step_timer != NULL)
		wl_event_source_remove(self->step_timer);
	post_close(self);
	g_queue_free(self->steps);
	g_queue_free(self->posted);
	g_mutex_clear(&self->post_lock);
	if (self->config != NULL && self->reloaded_handler != 0)
		g_signal_handler_disconnect(self->config, self->reloaded_handler);
	g_clear_object(&self->config);
	if (self->compositor != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->compositor),
		                             (gpointer *)&self->compositor);
	if (self->set != NULL)
		gabbrev_set_unref(self->set);
	gabbrev_engine_free(self->engine);
	g_ptr_array_unref(self->runtime);
	g_ptr_array_unref(self->jobs);
	g_hash_table_unref(self->swallowed);
	g_hash_table_unref(self->cvar_funcs);
	g_hash_table_unref(self->cvar_builds);
	g_hash_table_unref(self->cvar_faulted);
	g_strfreev(self->setting_files);
	g_strfreev(self->terminals);
	gowl_filter_free(self->deny);
	g_free(self->deny_text);
	g_free(self->picker_command);
	g_free(self->prompt_command);
	g_free(self->cache_dir);
	g_free(self->last.restore);
	g_free(self->last_error);

	G_OBJECT_CLASS(gowl_module_gabbrev_parent_class)->finalize(object);
}

static void
gowl_module_gabbrev_class_init(GowlModuleGabbrevClass *klass)
{
	GObjectClass    *object_class = G_OBJECT_CLASS(klass);
	GowlModuleClass *module_class = GOWL_MODULE_CLASS(klass);

	object_class->finalize = gowl_module_gabbrev_finalize;

	module_class->activate        = gab_activate;
	module_class->deactivate      = gab_deactivate;
	module_class->get_name        = gab_get_name;
	module_class->get_description = gab_get_description;
	module_class->get_version     = gab_get_version;
	module_class->configure       = gab_configure;
}

static void
gowl_module_gabbrev_init(GowlModuleGabbrev *self)
{
	wl_list_init(&self->display_destroy.link);
	self->display_destroy.notify = gab_display_destroyed;
	self->engine = gabbrev_engine_new();
	self->runtime = g_ptr_array_new_with_free_func(
		(GDestroyNotify)runtime_source_free);
	self->jobs = g_ptr_array_new_with_free_func((GDestroyNotify)job_unref);
	self->steps = g_queue_new();
	self->posted = g_queue_new();
	g_mutex_init(&self->post_lock);
	self->post_fd = -1;
	self->swallowed = g_hash_table_new(g_direct_hash, g_direct_equal);
	self->cvar_funcs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                         NULL);
	self->cvar_builds = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                          NULL);
	self->cvar_faulted = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                           g_free, g_free);
	self->start_on = TRUE;
	self->builtin = TRUE;
	self->toast = TRUE;
	self->expand_synthetic = TRUE;
	self->max_jobs = GABBREV_MAX_JOBS;
	self->c_timeout_ms = GABBREV_C_TIMEOUT_MS;
	self->terminals = g_strsplit(GABBREV_DEFAULT_TERMINALS, ",", -1);
	self->cache_dir = g_build_filename(g_get_user_cache_dir(), "gowl",
	                                   "gabbrev", NULL);
	set_deny(self, GABBREV_DEFAULT_DENY);
}

/* ── Shared-object entry point ─────────────────────────────────────── */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_GABBREV;
}
