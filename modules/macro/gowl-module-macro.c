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
 * gowl-module-macro.c - The opt-in `macro' module.
 *
 * Macros are C files compiled with crispy and run inside the
 * compositor on demand (see src/macro/gowl-macro.h and
 * docs/macros.org).  This module finds and compiles them, runs them
 * under the fault guard, contains the ones that crash or run away,
 * remembers across sessions which one took a session down, tells the
 * user, and wires up the triggers:
 *
 *   IPC     macro-run [--trigger=K] [--detail=D] NAME [ARGS...]
 *           macro-stop [NAME|ID|all]   macro-list   macro-status
 *           macro-info NAME   macro-compile NAME   macro-dirs
 *           macro-reload   macro-clear [NAME|all]   macro-log [LEVEL]
 *           macro-define NAME command|custom|action ...   macro-undefine NAME
 *   keys    any keybind: {action: ipc-command, arg: "macro-run NAME"}
 *           Super+Escape stops every running macro
 *   events  `triggers:' -- "client-added: tidy", "every 60000: night"
 *   D-Bus   org.gowl.Macro1 on the session bus, when `dbus: true'
 *   remap   an input-remap {macro: NAME} target
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-macro"

#include <glib-object.h>
#include <gmodule.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

#include "module/gowl-module.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-keybind-handler.h"
#include "interfaces/gowl-startup-handler.h"
#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
/* gowl_compositor_run_keybind_entry(), as the MCP module uses it */
#include "core/gowl-core-private.h"
#include "config/gowl-config.h"
#include "config/gowl-keybind.h"
#include "ipc/gowl-ipc.h"
#include "macro/gowl-macro.h"
#include "macro/gowl-macro-private.h"
#include "util/gowl-fault-guard.h"

#include "gowl-macro-loader.h"
#include "gowl-macro-runner.h"
#include "gowl-module-macro.h"

#define MACRO_PREFIX          "macro-"
#define DEFAULT_TIMEOUT_MS    (2000)
#define DEFAULT_MAX_RUNNING   (4)
#define DEFAULT_MAX_STEPS     (100000)

typedef enum {
	LOG_NONE,
	LOG_FAULT,
	LOG_RUN,
	LOG_ALL
} MacroLogLevel;

static const gchar *const log_names[] = { "none", "fault", "run", "all", NULL };

typedef enum {
	ALIAS_COMMAND,
	ALIAS_CUSTOM,
	ALIAS_ACTION
} AliasKind;

typedef struct {
	AliasKind  kind;
	gchar     *text;       /* command line / custom form / action arg */
	GowlAction action;
} Alias;

typedef struct {
	gchar                  *event;     /* signal name, or NULL for a timer */
	guint                   interval;  /* ms, timers */
	gchar                  *macro;
	gchar                  *args;      /* shell-style, may be empty */
	gulong                  handler;
	struct wl_event_source *timer;
	gpointer                module;
} Trigger;

#define GOWL_TYPE_MODULE_MACRO (gowl_module_macro_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleMacro, gowl_module_macro, GOWL, MODULE_MACRO,
                     GowlModule)

struct _GowlModuleMacro {
	GowlModule        parent_instance;

	GowlCompositor   *compositor;      /* weak */
	GowlMacroLoader  *loader;
	GowlMacroRunner  *runner;
	gboolean          enabled;

	GHashTable       *aliases;         /* name -> Alias* */
	GHashTable       *quarantine;      /* name -> reason */
	gchar            *journal_path;    /* NULL: journal off */
	GKeyFile         *journal;
	guint             faults;

	/* settings */
	gchar            *macro_dir;
	guint             timeout_ms;
	guint             max_running;
	guint             max_steps;
	gboolean          reentrant;
	MacroLogLevel     log_level;
	gchar            *log_path;
	FILE             *log_file;
	gboolean          dbus_wanted;
	gchar            *on_fault;
	gchar            *on_fault_custom;
	gchar            *triggers_text;
	guint             stop_mods;       /* the stop key; keysym 0 = none */
	guint             stop_keysym;

	GPtrArray        *triggers;        /* Trigger* */
	GList            *pending_idles;   /* wl_event_source* */
	GowlMacroDbus    *dbus;

	struct wl_listener display_destroy;
};

static void macro_ipc_init(GowlIpcHandlerInterface *iface);
static void macro_keybind_init(GowlKeybindHandlerInterface *iface);
static void macro_startup_init(GowlStartupHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleMacro, gowl_module_macro, GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_IPC_HANDLER, macro_ipc_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_KEYBIND_HANDLER, macro_keybind_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, macro_startup_init))

/* ===================================================================
 * Logging, events, notifications
 * =================================================================== */

/* One line to the log file, or stderr, stamped with the local time. */
static void
log_line(
	GowlModuleMacro *self,
	const gchar     *msg
){
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *stamp = NULL;

	now = g_date_time_new_now_local();
	stamp = g_date_time_format_iso8601(now);
	if (self->log_file != NULL) {
		fprintf(self->log_file, "%s macro: %s\n", stamp, msg);
		fflush(self->log_file);
	} else {
		g_printerr("%s macro: %s\n", stamp, msg);
	}
}

static void G_GNUC_PRINTF(3, 4)
macro_log(
	GowlModuleMacro *self,
	MacroLogLevel    level,
	const gchar     *format,
	...
){
	g_autofree gchar *msg = NULL;
	va_list ap;

	if (level > self->log_level || self->log_level == LOG_NONE)
		return;
	va_start(ap, format);
	msg = g_strdup_vprintf(format, ap);
	va_end(ap);
	log_line(self, msg);
}

static void G_GNUC_PRINTF(2, 3)
macro_event(
	GowlModuleMacro *self,
	const gchar     *format,
	...
){
	GowlIpc *ipc;
	g_autofree gchar *msg = NULL;
	va_list ap;

	if (self->compositor == NULL)
		return;
	ipc = gowl_compositor_get_ipc(self->compositor);
	if (ipc == NULL)
		return;
	va_start(ap, format);
	msg = g_strdup_vprintf(format, ap);
	va_end(ap);
	gowl_ipc_push_event(ipc, "EVENT macro %s", msg);
}

/* A notification: a bar toast (when the bar is loaded) and the
   compositor's toast signal (layout-indicator, embedders). */
static void
macro_notify(
	GowlModuleMacro *self,
	const gchar     *summary,
	const gchar     *body
){
	g_autofree gchar *s = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *line = NULL;

	if (self->compositor == NULL)
		return;
	/* `|' separates the fields of bar-notify */
	s = g_strdelimit(g_strdup(summary), "|\n", ' ');
	b = g_strdelimit(g_strdup(body != NULL ? body : ""), "|\n", ' ');
	line = g_strdup_printf("bar-notify %s|%s", s, b);
	g_free(gowl_compositor_run_command(self->compositor, line));
	g_signal_emit_by_name(self->compositor, "toast-requested",
	                      gowl_compositor_get_selected_monitor(self->compositor),
	                      s);
}

static void
close_log(GowlModuleMacro *self)
{
	if (self->log_file != NULL) {
		fclose(self->log_file);
		self->log_file = NULL;
	}
}

static void
open_log(GowlModuleMacro *self)
{
	g_autofree gchar *path = NULL;

	close_log(self);
	if (self->log_path == NULL || *self->log_path == '\0'
	    || g_strcmp0(self->log_path, "stderr") == 0)
		return;
	path = g_str_has_prefix(self->log_path, "~/")
		? g_build_filename(g_get_home_dir(), self->log_path + 2, NULL)
		: g_strdup(self->log_path);
	self->log_file = fopen(path, "a");
	if (self->log_file == NULL)
		g_warning("macro: cannot open log-file '%s': %s; logging to stderr",
		          path, g_strerror(errno));
}

/* A string as a Lisp string literal: the only escapes a Lisp reader
   needs are backslash and double quote. */
static void
append_lisp_string(
	GString     *out,
	const gchar *s
){
	const gchar *p;

	g_string_append_c(out, '"');
	for (p = s; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			g_string_append_c(out, '\\');
		g_string_append_c(out, *p);
	}
	g_string_append_c(out, '"');
}

/*
 * Expands %n (name), %s (signal/reason word), %t (trigger), %a (args)
 * in @template.  In a custom (Lisp) template each becomes a string
 * literal and %a a list of them; in a command template they are
 * shell-quoted.
 */
static gchar *
expand_template(
	const gchar         *template,
	gboolean             lisp,
	const gchar         *name,
	const gchar         *word,
	const gchar         *trigger,
	const gchar * const *args
){
	GString *out;
	const gchar *p;
	guint i;

	out = g_string_new(NULL);
	for (p = template; *p != '\0'; p++) {
		const gchar *v;

		if (*p != '%' || p[1] == '\0') {
			g_string_append_c(out, *p);
			continue;
		}
		p++;
		v = NULL;
		switch (*p) {
		case 'n': v = name; break;
		case 's': v = word; break;
		case 't': v = trigger; break;
		case '%': g_string_append_c(out, '%'); continue;
		case 'a':
			if (lisp)
				g_string_append_c(out, '(');
			for (i = 0; args != NULL && args[i] != NULL; i++) {
				if (i > 0)
					g_string_append_c(out, ' ');
				if (lisp)
					append_lisp_string(out, args[i]);
				else {
					g_autofree gchar *q = g_shell_quote(args[i]);

					g_string_append(out, q);
				}
			}
			if (lisp)
				g_string_append_c(out, ')');
			continue;
		default:
			g_string_append_c(out, '%');
			g_string_append_c(out, *p);
			continue;
		}
		if (lisp)
			append_lisp_string(out, v != NULL ? v : "");
		else {
			g_autofree gchar *q = g_shell_quote(v != NULL ? v : "");

			g_string_append(out, q);
		}
	}
	return g_string_free(out, FALSE);
}

/* ===================================================================
 * The journal: which macro was running when the session ended
 * =================================================================== */

static void
journal_save(GowlModuleMacro *self)
{
	g_autofree gchar *data = NULL;
	g_autofree gchar *dir = NULL;
	gsize len;

	if (self->journal_path == NULL || self->journal == NULL)
		return;
	dir = g_path_get_dirname(self->journal_path);
	g_mkdir_with_parents(dir, 0700);
	data = g_key_file_to_data(self->journal, &len, NULL);
	if (!g_file_set_contents(self->journal_path, data, (gssize)len, NULL))
		g_warning("macro: cannot write %s", self->journal_path);
}

static void
quarantine_add(
	GowlModuleMacro *self,
	const gchar     *name,
	const gchar     *reason
){
	g_hash_table_replace(self->quarantine, g_strdup(name), g_strdup(reason));
	if (self->journal != NULL) {
		g_key_file_set_string(self->journal, "quarantine", name, reason);
		journal_save(self);
	}
}

/*
 * On start: re-arm quarantines, and quarantine any macro still marked
 * running -- it was running when the last session ended, which is the
 * one case the in-process guard cannot report (heap corruption, a crash
 * the guard did not see).
 */
static void
journal_recover(GowlModuleMacro *self)
{
	g_auto(GStrv) keys = NULL;
	gsize n;
	gsize i;

	if (self->journal_path == NULL)
		return;
	g_clear_pointer(&self->journal, g_key_file_free);
	self->journal = g_key_file_new();
	g_key_file_load_from_file(self->journal, self->journal_path,
	                          G_KEY_FILE_NONE, NULL);

	keys = g_key_file_get_keys(self->journal, "quarantine", &n, NULL);
	for (i = 0; keys != NULL && i < n; i++) {
		g_autofree gchar *reason = g_key_file_get_string(
			self->journal, "quarantine", keys[i], NULL);

		g_hash_table_replace(self->quarantine, g_strdup(keys[i]),
		                     g_strdup(reason != NULL ? reason : ""));
	}
	g_clear_pointer(&keys, g_strfreev);

	keys = g_key_file_get_keys(self->journal, "running", &n, NULL);
	for (i = 0; keys != NULL && i < n; i++) {
		macro_log(self, LOG_FAULT, "%s was running when the last session "
		          "ended; quarantined", keys[i]);
		quarantine_add(self, keys[i],
		               "the last session ended while it was running");
	}
	g_key_file_remove_group(self->journal, "running", NULL);
	journal_save(self);
}

/* ===================================================================
 * Runner callbacks (compositor thread)
 * =================================================================== */

static void
on_run_started(
	gpointer     module,
	guint        id,
	const gchar *name,
	gboolean     threaded
){
	GowlModuleMacro *self = module;

	macro_log(self, LOG_RUN, "started %s (id %u%s)", name, id,
	          threaded ? ", threaded" : "");
	macro_event(self, "started %s %u", name, id);
	if (self->dbus != NULL)
		gowl_macro_dbus_emit(self->dbus, "Started", name, "");
}

static void
on_run_finished(
	gpointer     module,
	guint        id,
	const gchar *name,
	const gchar *result,
	gboolean     cancelled
){
	GowlModuleMacro *self = module;

	macro_log(self, LOG_RUN, "%s %s (id %u)%s%s", cancelled ? "stopped"
	          : "finished", name, id, result != NULL ? ": " : "",
	          result != NULL ? result : "");
	macro_event(self, "%s %s %u", cancelled ? "stopped" : "finished",
	            name, id);
	if (self->dbus != NULL)
		gowl_macro_dbus_emit(self->dbus, "Finished", name,
		                     result != NULL ? result : "");
}

/* The fault path: contain, remember, tell. */
static void
on_run_faulted(
	gpointer     module,
	guint        id,
	const gchar *name,
	gint         signo
){
	GowlModuleMacro *self = module;
	const gchar *what;
	g_autofree gchar *reason = NULL;
	g_autofree gchar *summary = NULL;
	g_autofree gchar *body = NULL;

	self->faults++;
	what = gowl_fault_guard_signal_name(signo);
	reason = gowl_fault_guard_is_watchdog(signo)
		? g_strdup("ran past its time budget")
		: g_strdup_printf("crashed (%s)", what);
	quarantine_add(self, name, reason);

	macro_log(self, LOG_FAULT, "%s %s (id %u); quarantined -- lift with "
	          "macro-clear %s", name, reason, id, name);
	macro_event(self, "fault %s %s", name, what);

	summary = g_strdup_printf("Macro \"%s\" stopped", name);
	body = g_strdup_printf("It %s and is held back. Clear it with: "
	                       "gowl-msg macro-clear %s", reason, name);
	macro_notify(self, summary, body);

	if (self->dbus != NULL)
		gowl_macro_dbus_emit(self->dbus, "Faulted", name, what);

	/* User hooks: a command line, or a custom action (cmacs: Elisp) */
	if (self->compositor != NULL && self->on_fault != NULL
	    && *self->on_fault != '\0') {
		g_autofree gchar *line = expand_template(self->on_fault, FALSE, name,
		                                         what, "fault", NULL);

		g_free(gowl_compositor_run_command(self->compositor, line));
	}
	if (self->compositor != NULL && self->on_fault_custom != NULL
	    && *self->on_fault_custom != '\0') {
		g_autofree gchar *form = expand_template(self->on_fault_custom, TRUE,
		                                         name, what, "fault", NULL);
		GowlKeybindEntry kb;

		memset(&kb, 0, sizeof kb);
		kb.action = GOWL_ACTION_CUSTOM;
		kb.arg = form;
		gowl_compositor_run_keybind_entry(self->compositor, &kb);
	}
}

static void
on_run_notify(
	gpointer     module,
	const gchar *name,
	const gchar *summary,
	const gchar *body
){
	GowlModuleMacro *self = module;

	macro_log(self, LOG_ALL, "%s notifies: %s", name, summary);
	macro_notify(self, summary, body);
}

static void
on_run_log(
	gpointer     module,
	const gchar *name,
	const gchar *message
){
	GowlModuleMacro *self = module;

	/* A macro's own log lines are shown unless logging is off */
	if (self->log_level != LOG_NONE) {
		g_autofree gchar *line = g_strdup_printf("%s: %s", name, message);

		log_line(self, line);
	}
}

static void
on_run_journal(
	gpointer     module,
	const gchar *name,
	gboolean     running
){
	GowlModuleMacro *self = module;

	if (self->journal == NULL)
		return;
	if (running)
		g_key_file_set_int64(self->journal, "running", name,
		                     g_get_real_time() / G_USEC_PER_SEC);
	else
		g_key_file_remove_key(self->journal, "running", name, NULL);
	journal_save(self);
}

static const GowlMacroRunnerCallbacks runner_callbacks = {
	on_run_started,
	on_run_finished,
	on_run_faulted,
	on_run_notify,
	on_run_log,
	on_run_journal
};

/* ===================================================================
 * Running
 * =================================================================== */

typedef struct {
	GowlMacroFunc func;
	gpointer      data;
} RegisteredBody;

static gboolean
registered_body(
	GowlMacroContext *ctx,
	gpointer          data
){
	RegisteredBody *r = data;

	return r->func(ctx, r->data);
}

static gboolean
script_body(
	GowlMacroContext *ctx,
	gpointer          data
){
	const GowlMacroScript *s = data;

	return s->run(ctx);
}

static gchar *
run_alias(
	GowlModuleMacro     *self,
	const gchar         *name,
	Alias               *alias,
	const gchar * const *args,
	const gchar         *trigger
){
	g_autofree gchar *expanded = NULL;
	GowlKeybindEntry kb;

	switch (alias->kind) {
	case ALIAS_COMMAND:
		expanded = expand_template(alias->text, FALSE, name, "", trigger,
		                           args);
		g_free(gowl_compositor_run_command(self->compositor, expanded));
		break;
	case ALIAS_CUSTOM:
		expanded = expand_template(alias->text, TRUE, name, "", trigger,
		                           args);
		memset(&kb, 0, sizeof kb);
		kb.action = GOWL_ACTION_CUSTOM;
		kb.arg = expanded;
		gowl_compositor_run_keybind_entry(self->compositor, &kb);
		break;
	case ALIAS_ACTION:
	default:
		memset(&kb, 0, sizeof kb);
		kb.action = (gint)alias->action;
		kb.arg = alias->text;
		gowl_compositor_run_keybind_entry(self->compositor, &kb);
		break;
	}
	macro_log(self, LOG_RUN, "ran %s (defined)", name);
	macro_event(self, "finished %s 0", name);
	return g_strdup_printf("OK ran %s", name);
}

/*
 * Runs macro @spec with @args.  The single entry point every trigger
 * goes through.  Returns the reply line.
 */
static gchar *
run_macro(
	GowlModuleMacro     *self,
	const gchar         *spec,
	const gchar * const *args,
	GowlMacroTrigger     trigger,
	const gchar         *detail
){
	g_autofree gchar *key = NULL;
	g_autoptr(GowlMacroContext) ctx = NULL;
	g_autoptr(GError) error = NULL;
	GEnumClass *klass;
	GEnumValue *tv;
	const gchar *trigger_nick;
	GowlMacroFunc func;
	gpointer func_data;
	Alias *alias;
	gchar *reply;

	if (self->compositor == NULL)
		return g_strdup("ERROR the macro module has not started");
	if (!self->enabled)
		return g_strdup("ERROR the macro module is disabled");

	klass = (GEnumClass *)g_type_class_ref(GOWL_TYPE_MACRO_TRIGGER);
	tv = g_enum_get_value(klass, (gint)trigger);
	trigger_nick = tv != NULL ? tv->value_nick : "api";
	g_type_class_unref(klass);

	/* The name everything is keyed by: a defined or registered name as
	   is, a file by its basename without suffix. */
	alias = g_hash_table_lookup(self->aliases, spec);
	if (alias != NULL || gowl_macro_registry_lookup(spec, &func, &func_data))
		key = g_strdup(spec);
	else
		key = gowl_macro_loader_normalize_name(spec);

	if (g_hash_table_contains(self->quarantine, key))
		return g_strdup_printf("ERROR %s is held back (%s); clear it with "
		                       "macro-clear %s", key,
		                       (const gchar *)g_hash_table_lookup(
		                               self->quarantine, key), key);
	if (alias != NULL)
		return run_alias(self, key, alias, args, trigger_nick);
	if (gowl_macro_runner_n_active(self->runner) >= self->max_running)
		return g_strdup_printf("ERROR %u macros are already running "
		                       "(max-running)", self->max_running);
	if (!self->reentrant && gowl_macro_runner_is_running(self->runner, key))
		return g_strdup_printf("ERROR %s is already running", key);

	/* A macro registered from C */
	if (gowl_macro_registry_lookup(spec, &func, &func_data)) {
		RegisteredBody *rb;

		rb = g_new0(RegisteredBody, 1);
		rb->func = func;
		rb->data = func_data;
		ctx = gowl_macro_context_new(self->compositor, key, NULL, args,
		                             trigger, detail, FALSE);
		gowl_macro_runner_start(self->runner, ctx, registered_body, rb,
		                        self->timeout_ms, self->max_steps, &reply);
		/* The body data is only read during the (synchronous) run and
		   the steps it queued do not refer to it. */
		g_free(rb);
		return reply;
	}

	/* A file */
	{
		const GowlMacroScript *script;

		script = gowl_macro_loader_load(self->loader, spec, &error);
		if (script == NULL) {
			if (g_error_matches(error, GOWL_MACRO_LOADER_ERROR,
			                    GOWL_MACRO_LOADER_ERROR_FAULT)) {
				quarantine_add(self, key, "crashed while loading");
				macro_notify(self, "Macro held back", error->message);
			}
			macro_log(self, LOG_FAULT, "%s", error->message);
			return g_strdup_printf("ERROR %s", error->message);
		}
		ctx = gowl_macro_context_new(self->compositor, key, script->path,
		                             args, trigger, detail, script->threaded);
		gowl_macro_runner_start(self->runner, ctx, script_body,
		                        (gpointer)script,
		                        script->has_timeout ? script->timeout_ms
		                                            : self->timeout_ms,
		                        self->max_steps, &reply);
		return reply;
	}
}

/* ===================================================================
 * Triggers
 * =================================================================== */

typedef struct {
	GowlModuleMacro        *self;
	gchar                  *macro;
	gchar                  *args;
	gchar                  *detail;
	GowlMacroTrigger        trigger;
	struct wl_event_source *source;
} PendingRun;

static void
pending_free(PendingRun *p)
{
	g_free(p->macro);
	g_free(p->args);
	g_free(p->detail);
	g_free(p);
}

static void
run_with_argstring(
	GowlModuleMacro  *self,
	const gchar      *macro,
	const gchar      *argstring,
	GowlMacroTrigger  trigger,
	const gchar      *detail
){
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *reply = NULL;

	if (argstring != NULL && *argstring != '\0'
	    && !g_shell_parse_argv(argstring, NULL, &argv, NULL)) {
		macro_log(self, LOG_FAULT, "trigger for %s: bad arguments `%s'",
		          macro, argstring);
		return;
	}
	reply = run_macro(self, macro, (const gchar * const *)argv, trigger,
	                  detail);
	if (reply != NULL && g_str_has_prefix(reply, "ERROR"))
		macro_log(self, LOG_FAULT, "trigger %s -> %s", detail, reply);
}

/* An event trigger runs from an idle callback, never inside the signal
   emission: a macro that moves windows during client-added would be
   rearranging a list the compositor is in the middle of changing. */
static void
on_pending_idle(gpointer data)
{
	PendingRun *p = data;

	p->self->pending_idles = g_list_remove(p->self->pending_idles, p);
	run_with_argstring(p->self, p->macro, p->args, p->trigger, p->detail);
	pending_free(p);
}

static void
queue_trigger(
	GowlModuleMacro  *self,
	Trigger          *t,
	GowlMacroTrigger  kind,
	const gchar      *detail
){
	PendingRun *p;
	struct wl_event_loop *loop;

	if (self->compositor == NULL)
		return;
	loop = gowl_compositor_get_event_loop(self->compositor);
	p = g_new0(PendingRun, 1);
	p->self = self;
	p->macro = g_strdup(t->macro);
	p->args = g_strdup(t->args);
	p->detail = g_strdup(detail);
	p->trigger = kind;
	if (loop == NULL) {
		run_with_argstring(self, p->macro, p->args, kind, detail);
		pending_free(p);
		return;
	}
	p->source = wl_event_loop_add_idle(loop, on_pending_idle, p);
	self->pending_idles = g_list_prepend(self->pending_idles, p);
}

/*
 * One marshaller for every compositor signal, whatever its parameters:
 * the trigger only needs to know it fired.  A GowlClient first
 * argument is described in the detail (app-id and title), which is
 * what a tidy-on-map macro wants.
 */
static void
trigger_marshal(
	GClosure     *closure,
	GValue       *return_value,
	guint         n_params,
	const GValue *params,
	gpointer      hint,
	gpointer      marshal_data
){
	Trigger *t = closure->data;
	GowlModuleMacro *self = t->module;
	g_autofree gchar *detail = NULL;

	(void)return_value;
	(void)hint;
	(void)marshal_data;
	if (n_params > 1 && G_VALUE_HOLDS_OBJECT(&params[1])
	    && GOWL_IS_CLIENT(g_value_get_object(&params[1]))) {
		GowlClient *c = g_value_get_object(&params[1]);
		const gchar *app = gowl_client_get_app_id(c);
		const gchar *title = gowl_client_get_title(c);

		detail = g_strdup_printf("%s app-id=%s title=%s", t->event,
		                         app != NULL ? app : "",
		                         title != NULL ? title : "");
	} else {
		detail = g_strdup(t->event);
	}
	queue_trigger(self, t, GOWL_MACRO_TRIGGER_EVENT, detail);
}

static gint
on_trigger_timer(gpointer data)
{
	Trigger *t = data;
	GowlModuleMacro *self = t->module;
	g_autofree gchar *detail = NULL;

	detail = g_strdup_printf("every %u", t->interval);
	run_with_argstring(self, t->macro, t->args, GOWL_MACRO_TRIGGER_TIMER,
	                   detail);
	if (t->timer != NULL)
		wl_event_source_timer_update(t->timer, (gint)t->interval);
	return 0;
}

static void
trigger_free(gpointer data)
{
	Trigger *t = data;
	GowlModuleMacro *self = t->module;

	if (t->handler != 0 && self->compositor != NULL)
		g_signal_handler_disconnect(self->compositor, t->handler);
	if (t->timer != NULL)
		wl_event_source_remove(t->timer);
	g_free(t->event);
	g_free(t->macro);
	g_free(t->args);
	g_free(t);
}

/*
 * `triggers:' lines, each "EVENT: MACRO [ARGS]" or "every MS: MACRO
 * [ARGS]".  EVENT is any compositor signal: client-added, client-removed,
 * focus-changed, workspace-switched, layout-changed, monitor-added,
 * lock-changed, startup...  A line that does not parse is reported and
 * skipped.
 */
static void
install_triggers(GowlModuleMacro *self)
{
	g_auto(GStrv) lines = NULL;
	struct wl_event_loop *loop;
	guint i;

	g_ptr_array_set_size(self->triggers, 0);
	if (self->compositor == NULL || self->triggers_text == NULL)
		return;
	loop = gowl_compositor_get_event_loop(self->compositor);
	lines = g_strsplit(self->triggers_text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) halves = NULL;
		g_auto(GStrv) words = NULL;
		gchar *what;
		gchar *rest;
		Trigger *t;

		g_strstrip(lines[i]);
		if (*lines[i] == '\0' || *lines[i] == '#')
			continue;
		halves = g_strsplit(lines[i], ":", 2);
		if (halves[1] == NULL) {
			macro_log(self, LOG_FAULT, "trigger `%s': expected "
			          "\"EVENT: MACRO\"", lines[i]);
			continue;
		}
		what = g_strstrip(halves[0]);
		rest = g_strstrip(halves[1]);
		words = g_strsplit(rest, " ", 2);
		if (words[0] == NULL || *words[0] == '\0') {
			macro_log(self, LOG_FAULT, "trigger `%s': no macro", lines[i]);
			continue;
		}

		t = g_new0(Trigger, 1);
		t->module = self;
		t->macro = g_strdup(words[0]);
		t->args = g_strdup(words[1] != NULL ? g_strstrip(words[1]) : "");

		if (g_str_has_prefix(what, "every ")) {
			guint64 ms;

			if (!g_ascii_string_to_unsigned(g_strstrip(what + 6), 10, 100,
			                                G_MAXINT, &ms, NULL)
			    || loop == NULL) {
				macro_log(self, LOG_FAULT, "trigger `%s': the interval is "
				          "milliseconds, at least 100", lines[i]);
				trigger_free(t);
				continue;
			}
			t->interval = (guint)ms;
			t->timer = wl_event_loop_add_timer(loop, on_trigger_timer, t);
			wl_event_source_timer_update(t->timer, (gint)t->interval);
		} else {
			GClosure *closure;

			if (g_signal_lookup(what, G_OBJECT_TYPE(self->compositor)) == 0) {
				macro_log(self, LOG_FAULT, "trigger `%s': the compositor has "
				          "no `%s' event", lines[i], what);
				trigger_free(t);
				continue;
			}
			t->event = g_strdup(what);
			closure = g_closure_new_simple(sizeof(GClosure), t);
			g_closure_set_marshal(closure, trigger_marshal);
			t->handler = g_signal_connect_closure(self->compositor, what,
			                                      closure, TRUE);
		}
		g_ptr_array_add(self->triggers, t);
	}
}

static void
drop_pending(GowlModuleMacro *self)
{
	GList *l;

	for (l = self->pending_idles; l != NULL; l = l->next) {
		PendingRun *p = l->data;

		if (p->source != NULL)
			wl_event_source_remove(p->source);
		pending_free(p);
	}
	g_clear_pointer(&self->pending_idles, g_list_free);
}

/* ===================================================================
 * D-Bus
 * =================================================================== */

static gchar *list_json(GowlModuleMacro *self);
static gchar *status_json(GowlModuleMacro *self);

static gchar *
dbus_handler(
	gpointer     module,
	const gchar *method,
	const gchar *name,
	GStrv        args
){
	GowlModuleMacro *self = module;
	g_autofree gchar *stopped = NULL;

	if (g_strcmp0(method, "Run") == 0)
		return run_macro(self, name, (const gchar * const *)args,
		                 GOWL_MACRO_TRIGGER_DBUS, "dbus");
	if (g_strcmp0(method, "Stop") == 0)
		return g_strdup_printf("OK stopped %u",
		                       gowl_macro_runner_stop(self->runner,
		                                              *name != '\0' ? name
		                                                            : NULL));
	if (g_strcmp0(method, "List") == 0)
		return list_json(self);
	if (g_strcmp0(method, "Status") == 0)
		return status_json(self);
	return g_strdup("ERROR unknown method");
}

static void
update_dbus(GowlModuleMacro *self)
{
	if (self->dbus_wanted && self->dbus == NULL && self->compositor != NULL
	    && self->enabled) {
		self->dbus = gowl_macro_dbus_start(
			gowl_compositor_get_event_loop(self->compositor),
			G_BUS_TYPE_SESSION, dbus_handler, self);
	} else if ((!self->dbus_wanted || !self->enabled) && self->dbus != NULL) {
		gowl_macro_dbus_stop(self->dbus);
		self->dbus = NULL;
	}
}

/* ===================================================================
 * IPC
 * =================================================================== */

static void
json_add_string(
	JsonBuilder *b,
	const gchar *member,
	const gchar *value
){
	json_builder_set_member_name(b, member);
	if (value != NULL)
		json_builder_add_string_value(b, value);
	else
		json_builder_add_null_value(b);
}

static gchar *
json_finish_ok(JsonBuilder *b)
{
	g_autoptr(JsonGenerator) gen = json_generator_new();
	g_autoptr(JsonNode) root = json_builder_get_root(b);
	g_autofree gchar *text = NULL;

	json_generator_set_root(gen, root);
	text = json_generator_to_data(gen, NULL);
	return g_strdup_printf("OK %s", text);
}

/* Every macro that can be run by name: registered, defined, files. */
static gchar *
list_json(GowlModuleMacro *self)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autoptr(GPtrArray) files = NULL;
	g_auto(GStrv) registered = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	guint i;

	json_builder_begin_array(b);
	registered = gowl_macro_registry_names();
	for (i = 0; registered[i] != NULL; i++) {
		json_builder_begin_object(b);
		json_add_string(b, "name", registered[i]);
		json_add_string(b, "kind", "registered");
		json_add_string(b, "path", NULL);
		json_builder_set_member_name(b, "held-back");
		json_builder_add_boolean_value(b, g_hash_table_contains(
			self->quarantine, registered[i]));
		json_builder_end_object(b);
	}
	g_hash_table_iter_init(&iter, self->aliases);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		Alias *a = value;

		json_builder_begin_object(b);
		json_add_string(b, "name", key);
		json_add_string(b, "kind", a->kind == ALIAS_COMMAND ? "command"
		                : a->kind == ALIAS_CUSTOM ? "custom" : "action");
		json_add_string(b, "path", NULL);
		json_builder_set_member_name(b, "held-back");
		json_builder_add_boolean_value(b, FALSE);
		json_builder_end_object(b);
	}
	files = gowl_macro_loader_discover(self->loader);
	for (i = 0; i + 1 < files->len; i += 2) {
		const gchar *name = g_ptr_array_index(files, i);

		json_builder_begin_object(b);
		json_add_string(b, "name", name);
		json_add_string(b, "kind", "file");
		json_add_string(b, "path", g_ptr_array_index(files, i + 1));
		json_builder_set_member_name(b, "held-back");
		json_builder_add_boolean_value(b, g_hash_table_contains(
			self->quarantine, name));
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	return json_finish_ok(b);
}

static gchar *
status_json(GowlModuleMacro *self)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autofree gchar *running = NULL;
	g_autoptr(JsonParser) p = json_parser_new();
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	running = gowl_macro_runner_status_json(self->runner);
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "enabled");
	json_builder_add_boolean_value(b, self->enabled);
	json_builder_set_member_name(b, "running");
	if (json_parser_load_from_data(p, running, -1, NULL))
		json_builder_add_value(b, json_node_copy(json_parser_get_root(p)));
	else
		json_builder_add_null_value(b);
	json_builder_set_member_name(b, "held-back");
	json_builder_begin_array(b);
	g_hash_table_iter_init(&iter, self->quarantine);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		json_builder_begin_object(b);
		json_add_string(b, "name", key);
		json_add_string(b, "reason", value);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	json_builder_set_member_name(b, "faults");
	json_builder_add_int_value(b, self->faults);
	json_builder_set_member_name(b, "timeout-ms");
	json_builder_add_int_value(b, self->timeout_ms);
	json_builder_set_member_name(b, "dbus");
	json_builder_add_boolean_value(b, self->dbus != NULL
	                               && gowl_macro_dbus_is_owned(self->dbus));
	json_builder_end_object(b);
	return json_finish_ok(b);
}

static gchar *
cmd_run(
	GowlModuleMacro *self,
	const gchar     *args
){
	g_auto(GStrv) argv = NULL;
	g_autoptr(GError) error = NULL;
	GowlMacroTrigger trigger;
	g_autofree gchar *detail = NULL;
	guint i;

	if (args == NULL || *args == '\0')
		return g_strdup("ERROR macro-run needs a macro name");
	if (!g_shell_parse_argv(args, NULL, &argv, &error))
		return g_strdup_printf("ERROR %s", error->message);

	/* Leading options: who is running it */
	trigger = GOWL_MACRO_TRIGGER_IPC;
	detail = g_strdup_printf("macro-run %s", args);
	for (i = 0; argv[i] != NULL && g_str_has_prefix(argv[i], "--"); i++) {
		if (g_str_has_prefix(argv[i], "--trigger=")) {
			GEnumClass *klass;
			GEnumValue *v;

			klass = (GEnumClass *)g_type_class_ref(GOWL_TYPE_MACRO_TRIGGER);
			v = g_enum_get_value_by_nick(klass, argv[i] + 10);
			if (v != NULL)
				trigger = (GowlMacroTrigger)v->value;
			g_type_class_unref(klass);
		} else if (g_str_has_prefix(argv[i], "--detail=")) {
			g_free(detail);
			detail = g_strdup(argv[i] + 9);
		} else if (g_strcmp0(argv[i], "--") == 0) {
			i++;
			break;
		}
	}
	if (argv[i] == NULL)
		return g_strdup("ERROR macro-run needs a macro name");
	return run_macro(self, argv[i], (const gchar * const *)&argv[i + 1],
	                 trigger, detail);
}

static gchar *
cmd_info(
	GowlModuleMacro *self,
	const gchar     *name
){
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonBuilder) b = NULL;
	const GowlMacroScript *s;
	GowlMacroFunc f;
	gpointer d;

	if (name == NULL || *name == '\0')
		return g_strdup("ERROR macro-info needs a macro name");
	b = json_builder_new();
	json_builder_begin_object(b);
	json_add_string(b, "name", name);
	if (g_hash_table_contains(self->aliases, name)) {
		json_add_string(b, "kind", "defined");
	} else if (gowl_macro_registry_lookup(name, &f, &d)) {
		json_add_string(b, "kind", "registered");
	} else {
		s = gowl_macro_loader_load(self->loader, name, &error);
		if (s == NULL)
			return g_strdup_printf("ERROR %s", error->message);
		json_add_string(b, "kind", "file");
		json_add_string(b, "path", s->path);
		json_add_string(b, "so-path", s->so_path);
		json_add_string(b, "description", s->description);
		json_builder_set_member_name(b, "threaded");
		json_builder_add_boolean_value(b, s->threaded);
		json_builder_set_member_name(b, "timeout-ms");
		json_builder_add_int_value(b, s->has_timeout ? s->timeout_ms
		                                             : self->timeout_ms);
	}
	json_builder_set_member_name(b, "held-back");
	json_builder_add_boolean_value(b, g_hash_table_contains(self->quarantine,
	                                                        name));
	json_builder_end_object(b);
	return json_finish_ok(b);
}

static gchar *
cmd_dirs(GowlModuleMacro *self)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	const gchar * const *dirs;
	guint i;

	dirs = gowl_macro_loader_get_dirs(self->loader);
	json_builder_begin_array(b);
	for (i = 0; dirs != NULL && dirs[i] != NULL; i++)
		json_builder_add_string_value(b, dirs[i]);
	json_builder_end_array(b);
	return json_finish_ok(b);
}

static gchar *
cmd_clear(
	GowlModuleMacro *self,
	const gchar     *which
){
	guint n;

	if (which == NULL || *which == '\0' || g_strcmp0(which, "all") == 0) {
		n = g_hash_table_size(self->quarantine);
		g_hash_table_remove_all(self->quarantine);
		if (self->journal != NULL) {
			g_key_file_remove_group(self->journal, "quarantine", NULL);
			journal_save(self);
		}
		return g_strdup_printf("OK cleared %u", n);
	}
	if (!g_hash_table_remove(self->quarantine, which))
		return g_strdup_printf("ERROR %s is not held back", which);
	if (self->journal != NULL) {
		g_key_file_remove_key(self->journal, "quarantine", which, NULL);
		journal_save(self);
	}
	return g_strdup_printf("OK cleared %s", which);
}

static void
alias_free(gpointer data)
{
	Alias *a = data;

	g_free(a->text);
	g_free(a);
}

/* macro-define NAME command LINE | custom FORM | action ACTION [ARG] */
static gchar *
cmd_define(
	GowlModuleMacro *self,
	const gchar     *args
){
	g_auto(GStrv) parts = NULL;
	Alias *a;

	if (args == NULL)
		return g_strdup("ERROR macro-define NAME command|custom|action ...");
	parts = g_strsplit(args, " ", 3);
	if (g_strv_length(parts) < 3 || *parts[0] == '\0'
	    || strchr(parts[0], '/') != NULL)
		return g_strdup("ERROR macro-define NAME command|custom|action ...");

	a = g_new0(Alias, 1);
	if (g_strcmp0(parts[1], "command") == 0) {
		a->kind = ALIAS_COMMAND;
		a->text = g_strdup(parts[2]);
	} else if (g_strcmp0(parts[1], "custom") == 0) {
		a->kind = ALIAS_CUSTOM;
		a->text = g_strdup(parts[2]);
	} else if (g_strcmp0(parts[1], "action") == 0) {
		g_auto(GStrv) act = g_strsplit(parts[2], " ", 2);
		g_autofree gchar *nick = g_strdelimit(g_strdup(act[0]), "_", '-');
		GEnumClass *klass;
		GEnumValue *v;

		klass = (GEnumClass *)g_type_class_ref(GOWL_TYPE_ACTION);
		v = g_enum_get_value_by_nick(klass, nick);
		g_type_class_unref(klass);
		if (v == NULL) {
			g_free(a);
			return g_strdup_printf("ERROR unknown action %s", act[0]);
		}
		a->kind = ALIAS_ACTION;
		a->action = (GowlAction)v->value;
		a->text = act[1] != NULL ? g_strdup(act[1]) : NULL;
	} else {
		g_free(a);
		return g_strdup("ERROR the kind is command, custom or action");
	}
	g_hash_table_replace(self->aliases, g_strdup(parts[0]), a);
	return g_strdup_printf("OK defined %s", parts[0]);
}

static gchar *
cmd_log(
	GowlModuleMacro *self,
	const gchar     *args
){
	guint i;

	if (args == NULL || *args == '\0')
		return g_strdup_printf("OK %s", log_names[self->log_level]);
	for (i = 0; log_names[i] != NULL; i++) {
		if (g_ascii_strcasecmp(args, log_names[i]) == 0) {
			self->log_level = (MacroLogLevel)i;
			return g_strdup_printf("OK %s", log_names[i]);
		}
	}
	return g_strdup("ERROR expected none, fault, run or all");
}

static gchar *
macro_handle_command(
	GowlIpcHandler *handler,
	const gchar    *command,
	const gchar    *args
){
	GowlModuleMacro *self;
	const gchar *verb;
	g_autofree gchar *arg = NULL;

	if (command == NULL || !g_str_has_prefix(command, MACRO_PREFIX))
		return NULL;
	self = GOWL_MODULE_MACRO(handler);
	verb = command + strlen(MACRO_PREFIX);
	arg = args != NULL ? g_strstrip(g_strdup(args)) : NULL;

	if (g_strcmp0(verb, "run") == 0)
		return cmd_run(self, arg);
	if (g_strcmp0(verb, "stop") == 0)
		return g_strdup_printf("OK stopped %u",
		                       gowl_macro_runner_stop(self->runner,
		                                              arg != NULL && *arg != '\0'
		                                              ? arg : NULL));
	if (g_strcmp0(verb, "list") == 0)
		return list_json(self);
	if (g_strcmp0(verb, "status") == 0)
		return status_json(self);
	if (g_strcmp0(verb, "info") == 0)
		return cmd_info(self, arg);
	if (g_strcmp0(verb, "dirs") == 0)
		return cmd_dirs(self);
	if (g_strcmp0(verb, "compile") == 0) {
		g_autoptr(GError) error = NULL;

		if (arg == NULL || *arg == '\0')
			return g_strdup("ERROR macro-compile needs a macro name");
		if (!gowl_macro_loader_compile_only(self->loader, arg, &error))
			return g_strdup_printf("ERROR %s", error->message);
		return g_strdup_printf("OK compiled %s", arg);
	}
	if (g_strcmp0(verb, "reload") == 0) {
		gowl_macro_loader_forget(self->loader);
		install_triggers(self);
		return g_strdup_printf("OK reloaded, %u trigger(s)",
		                       self->triggers->len);
	}
	if (g_strcmp0(verb, "clear") == 0)
		return cmd_clear(self, arg);
	if (g_strcmp0(verb, "define") == 0)
		return cmd_define(self, arg);
	if (g_strcmp0(verb, "undefine") == 0) {
		if (arg == NULL || !g_hash_table_remove(self->aliases, arg))
			return g_strdup_printf("ERROR no macro defined as %s",
			                       arg != NULL ? arg : "");
		return g_strdup_printf("OK undefined %s", arg);
	}
	if (g_strcmp0(verb, "log") == 0)
		return cmd_log(self, arg);
	return g_strdup_printf("ERROR unknown command %s; the macro module "
	                       "knows " MACRO_PREFIX "run, -stop, -list, -status, "
	                       "-info, -compile, -dirs, -reload, -clear, "
	                       "-define, -undefine and -log", command);
}

static void
macro_ipc_init(GowlIpcHandlerInterface *iface)
{
	iface->handle_command = macro_handle_command;
}

/* ===================================================================
 * The stop key (Super+Escape by default) stops every running macro
 * =================================================================== */

/* The modifiers a stop key is compared on; lock keys do not count. */
#define STOP_KEY_MODS (GOWL_KEY_MOD_SHIFT | GOWL_KEY_MOD_CTRL \
                       | GOWL_KEY_MOD_ALT | GOWL_KEY_MOD_LOGO)

/*
 * Consumed only while a macro is running, so the key keeps whatever
 * else it does the rest of the time.  Module key handlers run AFTER the
 * configured keybinds, so a config that binds the same combo wins: the
 * `stop-key' setting moves it (cmacs, which binds Super+Escape to its
 * menu, moves it to Super+Alt+Escape).
 */
static gboolean
macro_handle_key(
	GowlKeybindHandler *handler,
	guint               modifiers,
	guint               keysym,
	gboolean            pressed
){
	GowlModuleMacro *self = GOWL_MODULE_MACRO(handler);
	g_autofree gchar *body = NULL;
	guint n;

	if (!pressed || self->stop_keysym == 0
	    || xkb_keysym_to_lower(keysym) != self->stop_keysym
	    || (modifiers & STOP_KEY_MODS) != self->stop_mods
	    || self->runner == NULL
	    || gowl_macro_runner_n_active(self->runner) == 0)
		return FALSE;
	n = gowl_macro_runner_stop(self->runner, NULL);
	body = g_strdup_printf("%u macro%s stopped from the keyboard", n,
	                       n == 1 ? "" : "s");
	macro_log(self, LOG_FAULT, "%s", body);
	macro_notify(self, "Macros stopped", body);
	return TRUE;
}

/* `stop-key': a combo, or `none'.  A bad one is reported and ignored. */
static void
set_stop_key(
	GowlModuleMacro *self,
	const gchar     *value
){
	guint mods;
	guint sym;

	if (value == NULL || g_ascii_strcasecmp(value, "none") == 0
	    || *value == '\0') {
		self->stop_mods = 0;
		self->stop_keysym = 0;
		return;
	}
	if (!gowl_keybind_parse(value, &mods, &sym) || sym == 0) {
		macro_log(self, LOG_FAULT, "stop-key `%s' is not a key combo; "
		          "keeping the old one", value);
		return;
	}
	self->stop_mods = mods & STOP_KEY_MODS;
	self->stop_keysym = xkb_keysym_to_lower(sym);
}

static void
macro_keybind_init(GowlKeybindHandlerInterface *iface)
{
	iface->handle_key = macro_handle_key;
}

/* ===================================================================
 * GowlModule
 * =================================================================== */

static gboolean
parse_uint(
	const gchar *value,
	guint64      min,
	guint64      max,
	guint       *out,
	const gchar *key
){
	guint64 v;

	if (!g_ascii_string_to_unsigned(value, 10, min, max, &v, NULL)) {
		g_warning("macro: %s '%s' -- expected %" G_GUINT64_FORMAT "..%"
		          G_GUINT64_FORMAT, key, value, min, max);
		return FALSE;
	}
	*out = (guint)v;
	return TRUE;
}

static gboolean
parse_bool(const gchar *v)
{
	return g_ascii_strcasecmp(v, "true") == 0 || g_ascii_strcasecmp(v, "yes") == 0
	       || g_strcmp0(v, "1") == 0 || g_ascii_strcasecmp(v, "on") == 0;
}

static void
apply_dirs(GowlModuleMacro *self)
{
	g_auto(GStrv) dirs = gowl_macro_loader_default_dirs(self->macro_dir);

	gowl_macro_loader_set_dirs(self->loader, (const gchar * const *)dirs);
}

/*
 * macro_configure:
 * @mod: the module
 * @config: a #GHashTable of string settings from `modules: macro:'
 *
 * macro-dir, timeout-ms, max-running, max-steps, reentrant, stop-key, log,
 * log-file, dbus, triggers (a list), on-fault, on-fault-custom, journal.
 */
static void
macro_configure(
	GowlModule *mod,
	gpointer    config
){
	GowlModuleMacro *self = GOWL_MODULE_MACRO(mod);
	GHashTable *s = config;
	const gchar *v;
	guint i;

	if (s == NULL)
		return;
	if ((v = g_hash_table_lookup(s, "macro-dir")) != NULL) {
		g_free(self->macro_dir);
		self->macro_dir = g_strdup(v);
		apply_dirs(self);
	}
	if ((v = g_hash_table_lookup(s, "timeout-ms")) != NULL)
		parse_uint(v, 0, 3600000, &self->timeout_ms, "timeout-ms");
	if ((v = g_hash_table_lookup(s, "max-running")) != NULL)
		parse_uint(v, 1, 256, &self->max_running, "max-running");
	if ((v = g_hash_table_lookup(s, "max-steps")) != NULL)
		parse_uint(v, 1, 10000000, &self->max_steps, "max-steps");
	if ((v = g_hash_table_lookup(s, "reentrant")) != NULL)
		self->reentrant = parse_bool(v);
	if ((v = g_hash_table_lookup(s, "stop-key")) != NULL)
		set_stop_key(self, v);
	if ((v = g_hash_table_lookup(s, "log")) != NULL) {
		for (i = 0; log_names[i] != NULL; i++)
			if (g_ascii_strcasecmp(v, log_names[i]) == 0)
				self->log_level = (MacroLogLevel)i;
	}
	if ((v = g_hash_table_lookup(s, "log-file")) != NULL) {
		g_free(self->log_path);
		self->log_path = g_strdup(v);
		open_log(self);
	}
	if ((v = g_hash_table_lookup(s, "on-fault")) != NULL) {
		g_free(self->on_fault);
		self->on_fault = g_strdup(v);
	}
	if ((v = g_hash_table_lookup(s, "on-fault-custom")) != NULL) {
		g_free(self->on_fault_custom);
		self->on_fault_custom = g_strdup(v);
	}
	if ((v = g_hash_table_lookup(s, "journal")) != NULL) {
		g_free(self->journal_path);
		self->journal_path = g_strcmp0(v, "none") == 0 ? NULL
			: g_str_has_prefix(v, "~/")
			? g_build_filename(g_get_home_dir(), v + 2, NULL)
			: g_strdup(v);
		g_hash_table_remove_all(self->quarantine);
		journal_recover(self);
	}
	if ((v = g_hash_table_lookup(s, "triggers")) != NULL) {
		g_free(self->triggers_text);
		self->triggers_text = g_strdup(v);
		install_triggers(self);
	}
	if ((v = g_hash_table_lookup(s, "dbus")) != NULL) {
		self->dbus_wanted = parse_bool(v);
		update_dbus(self);
	}
}

static gboolean
macro_activate(GowlModule *mod)
{
	GowlModuleMacro *self = GOWL_MODULE_MACRO(mod);

	self->enabled = TRUE;
	update_dbus(self);
	return TRUE;
}

/* Switched off: every run cancelled, triggers disconnected, the D-Bus
   name released.  Workers still running notice and exit on their own. */
static void
macro_deactivate(GowlModule *mod)
{
	GowlModuleMacro *self = GOWL_MODULE_MACRO(mod);

	self->enabled = FALSE;
	drop_pending(self);
	g_ptr_array_set_size(self->triggers, 0);
	if (self->runner != NULL)
		gowl_macro_runner_stop(self->runner, NULL);
	update_dbus(self);
}

static const gchar *
macro_get_name(GowlModule *mod)
{
	(void)mod;
	return "macro";
}

static const gchar *
macro_get_description(GowlModule *mod)
{
	(void)mod;
	return "Macros: crispy C scripts run on demand, guarded";
}

static const gchar *
macro_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

/* ===================================================================
 * GowlStartupHandler
 * =================================================================== */

/* Timers, idles and the worker queue belong to the display's event loop,
   which goes with the display: let go of them first. */
static void
macro_display_destroyed(
	struct wl_listener *listener,
	void               *data
){
	GowlModuleMacro *self;

	(void)data;
	self = wl_container_of(listener, self, display_destroy);
	drop_pending(self);
	g_ptr_array_set_size(self->triggers, 0);
	if (self->runner != NULL)
		gowl_macro_runner_shutdown(self->runner);
	wl_list_remove(&self->display_destroy.link);
	wl_list_init(&self->display_destroy.link);
}

static void
macro_on_startup(
	GowlStartupHandler *handler,
	gpointer            compositor
){
	GowlModuleMacro *self = GOWL_MODULE_MACRO(handler);
	struct wl_display *display;

	if (self->compositor == NULL) {
		self->compositor = GOWL_COMPOSITOR(compositor);
		g_object_add_weak_pointer(G_OBJECT(compositor),
		                          (gpointer *)&self->compositor);
	}
	display = gowl_compositor_get_wl_display(self->compositor);
	if (display != NULL && wl_list_empty(&self->display_destroy.link))
		wl_display_add_destroy_listener(display, &self->display_destroy);

	/* The compositor thread gets its alternate stack now, before the
	   first macro can overflow it. */
	gowl_fault_guard_thread_init();

	if (self->runner == NULL)
		self->runner = gowl_macro_runner_new(
			self->compositor, gowl_compositor_get_event_loop(self->compositor),
			&runner_callbacks, self);
	install_triggers(self);
	update_dbus(self);
	macro_log(self, LOG_RUN, "started; %u held back",
	          g_hash_table_size(self->quarantine));
}

static void
macro_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = macro_on_startup;
}

/* ===================================================================
 * GObject lifecycle
 * =================================================================== */

static void
gowl_module_macro_finalize(GObject *object)
{
	GowlModuleMacro *self = GOWL_MODULE_MACRO(object);

	wl_list_remove(&self->display_destroy.link);
	drop_pending(self);
	g_ptr_array_unref(self->triggers);
	if (self->dbus != NULL)
		gowl_macro_dbus_stop(self->dbus);
	if (self->runner != NULL) {
		gowl_macro_runner_shutdown(self->runner);
		gowl_macro_runner_unref(self->runner);
	}
	if (self->compositor != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->compositor),
		                             (gpointer *)&self->compositor);
	gowl_macro_loader_free(self->loader);
	g_hash_table_unref(self->aliases);
	g_hash_table_unref(self->quarantine);
	g_clear_pointer(&self->journal, g_key_file_free);
	close_log(self);
	g_free(self->journal_path);
	g_free(self->macro_dir);
	g_free(self->log_path);
	g_free(self->on_fault);
	g_free(self->on_fault_custom);
	g_free(self->triggers_text);

	G_OBJECT_CLASS(gowl_module_macro_parent_class)->finalize(object);
}

static void
gowl_module_macro_class_init(GowlModuleMacroClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GowlModuleClass *module_class = GOWL_MODULE_CLASS(klass);

	object_class->finalize = gowl_module_macro_finalize;
	module_class->activate = macro_activate;
	module_class->deactivate = macro_deactivate;
	module_class->get_name = macro_get_name;
	module_class->get_description = macro_get_description;
	module_class->get_version = macro_get_version;
	module_class->configure = macro_configure;
}

static void
gowl_module_macro_init(GowlModuleMacro *self)
{
	wl_list_init(&self->display_destroy.link);
	self->display_destroy.notify = macro_display_destroyed;
	self->loader = gowl_macro_loader_new(NULL);
	self->aliases = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      alias_free);
	self->quarantine = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                         g_free);
	self->triggers = g_ptr_array_new_with_free_func(trigger_free);
	self->timeout_ms = DEFAULT_TIMEOUT_MS;
	self->stop_mods = GOWL_KEY_MOD_LOGO;
	self->stop_keysym = XKB_KEY_Escape;
	self->max_running = DEFAULT_MAX_RUNNING;
	self->max_steps = DEFAULT_MAX_STEPS;
	self->log_level = LOG_FAULT;
	self->journal_path = g_build_filename(g_get_user_state_dir(), "gowl",
	                                      "macros.journal", NULL);
	journal_recover(self);
}

/* --- Shared-object entry point --- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_MACRO;
}
