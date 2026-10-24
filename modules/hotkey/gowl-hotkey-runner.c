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
 * gowl-hotkey-runner.c - Doing what a hotkey says.
 *
 * A hotkey fires a loop turn after its key (never inside the key's own
 * handling) and starts a RUNNER over its actions -- AutoHotkey's thread.
 * Most actions are done at once; the rest make the runner wait and pick
 * up again from the compositor's event loop:
 *
 *   sleep                     a wl_event_loop timer
 *   shell/run with wait       gowl_subprocess (pipes and reaping on the
 *                             loop, never GLib's default context)
 *   macro with wait           the macro module's finished listener
 *   c, the first time         the build, on a worker thread, posted back
 *   send / send-text          the typing queue finishing its item
 *   input                     the input hook ending
 *
 * A runner is counted against its hotkey's max-running while it runs.
 * Functions (`call:') run as frames on the same runner.
 *
 * Typing (Send): a queue of plans played on the loop, never inside a
 * key event.  Unless {Blind}, the modifiers physically held for the
 * hotkey are lifted first and pressed again after, as AutoHotkey does,
 * so `#!t' sending `hello' types hello and not Super+Alt+h...  Each key
 * is injected through the compositor's own key path at the action's
 * send level, which hotkeys of a lower level ignore.
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-hotkey"

#include <errno.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

#include "gowl-hotkey-module.h"
#include "gowl-hotkey-hotstring.h"

#include "core/gowl-core-private.h"
#include "core/gowl-client.h"
#include "core/gowl-seat.h"
#include "gabbrev/gowl-gabbrev.h"
#include "hotkey/gowl-hotkey-private.h"
#include "macro/gowl-macro.h"
#include "gowl-enums.h"

static void runner_step   (HkRunner *r);
static void runner_resume (HkRunner *r);
static void play_kick     (GowlModuleHotkey *self);
static gint play_tick     (gpointer data);

/* ── Our own input ───────────────────────────────────────────────────── */

void
hk_inject_key(
	GowlModuleHotkey *self,
	guint32           keycode,
	gboolean          pressed,
	gint              level
){
	gint saved;

	if (self->compositor == NULL || keycode == 0)
		return;
	saved = self->inject_level;
	self->injecting++;
	self->inject_level = level;
	gowl_compositor_inject_key(self->compositor, keycode, pressed);
	self->inject_level = saved;
	self->injecting--;
}

void
hk_inject_button(
	GowlModuleHotkey *self,
	guint32           button,
	gboolean          pressed,
	gint              level
){
	gint saved;

	if (self->compositor == NULL || button == 0)
		return;
	saved = self->inject_level;
	self->injecting++;
	self->inject_level = level;
	gowl_compositor_inject_button(self->compositor, button, pressed);
	gowl_compositor_inject_frame(self->compositor);
	self->inject_level = saved;
	self->injecting--;
}

/* The keycodes of the modifier keys in @mods, left hand. */
static guint
mod_keycodes(
	GowlModuleHotkey *self,
	guint             mods,
	guint32          *out
){
	static const struct { guint mod; guint32 sym; } order[] = {
		{ GOWL_KEY_MOD_CTRL,  XKB_KEY_Control_L },
		{ GOWL_KEY_MOD_SHIFT, XKB_KEY_Shift_L },
		{ GOWL_KEY_MOD_ALT,   XKB_KEY_Alt_L },
		{ GOWL_KEY_MOD_LOGO,  XKB_KEY_Super_L },
		{ GOWL_KEY_MOD_MOD5,  XKB_KEY_ISO_Level3_Shift }
	};
	guint i;
	guint n = 0;

	for (i = 0; i < G_N_ELEMENTS(order); i++) {
		guint32 kc;

		if (!(mods & order[i].mod))
			continue;
		kc = gowl_compositor_keysym_to_keycode(self->compositor,
		                                       order[i].sym, NULL);
		if (kc != 0)
			out[n++] = kc;
	}
	return n;
}

/* A key by keysym (or keycode), with modifiers held around a press. */
static void
key_with_mods(
	GowlModuleHotkey *self,
	guint32           keycode,
	guint32           keysym,
	guint             mods,
	gint              updown,
	gint              level
){
	guint32 mk[5];
	guint n;
	guint i;
	gboolean shift = FALSE;

	if (keycode == 0 && keysym != 0)
		keycode = gowl_compositor_keysym_to_keycode(self->compositor, keysym,
		                                            &shift);
	if (keycode == 0) {
		hk_fail(self, "no key on this layout makes keysym 0x%x", keysym);
		return;
	}
	if (shift)
		mods |= GOWL_KEY_MOD_SHIFT;
	n = mod_keycodes(self, mods, mk);
	if (updown >= 0)
		for (i = 0; i < n; i++)
			hk_inject_key(self, mk[i], TRUE, level);
	if (updown >= 0)
		hk_inject_key(self, keycode, TRUE, level);
	if (updown <= 0)
		hk_inject_key(self, keycode, FALSE, level);
	if (updown <= 0)
		for (i = n; i > 0; i--)
			hk_inject_key(self, mk[i - 1], FALSE, level);
}

/*
 * What the engine says follows an input: keys and buttons put back now
 * (the caller is inside a swallowed event, or a timer), hotkeys fired a
 * loop turn later.
 */
void
hk_play_outputs(
	GowlModuleHotkey *self,
	GArray           *out
){
	guint i;

	for (i = 0; i < out->len; i++) {
		HkOut *o = &g_array_index(out, HkOut, i);

		switch (o->kind) {
		case HK_OUT_FIRE:
			hk_queue_fire(self, o->hotkey);
			break;
		case HK_OUT_KEY:
			if (o->keycode != 0 && o->mods == 0)
				hk_inject_key(self, o->keycode, o->pressed, 0);
			else
				key_with_mods(self, o->keycode, o->keysym, o->mods,
				              o->pressed ? 1 : -1, 0);
			/* A remap onto a modifier holds it until its release */
			if (g_strcmp0(o->why, "remap") == 0
			    && gowl_hotkey_keysym_modifier(o->keysym) != 0)
				hk_overlay_hold(self, gowl_hotkey_keysym_modifier(o->keysym),
				                o->pressed);
			break;
		case HK_OUT_BUTTON: {
			guint32 mk[5];
			guint n = mod_keycodes(self, o->mods, mk);
			guint j;

			if (o->pressed)
				for (j = 0; j < n; j++)
					hk_inject_key(self, mk[j], TRUE, 0);
			hk_inject_button(self, o->button, o->pressed, 0);
			if (!o->pressed)
				for (j = n; j > 0; j--)
					hk_inject_key(self, mk[j - 1], FALSE, 0);
			break;
		}
		default:
			break;
		}
	}
}

/* ── Firing ──────────────────────────────────────────────────────────── */

guint
hk_running_count(
	GowlModuleHotkey *self,
	const gchar      *id
){
	return id != NULL ? GPOINTER_TO_UINT(g_hash_table_lookup(self->running,
	                                                         id)) : 0;
}

static void
running_add(
	GowlModuleHotkey *self,
	const gchar      *id,
	gint              delta
){
	guint n = hk_running_count(self, id);

	n = (delta < 0 && n == 0) ? 0 : (guint)((gint)n + delta);
	if (n == 0)
		g_hash_table_remove(self->running, id);
	else
		g_hash_table_replace(self->running, g_strdup(id),
		                     GUINT_TO_POINTER(n));
}

static void
fire_idle(gpointer data)
{
	GowlModuleHotkey *self = data;
	HkHotkey *hk;

	self->fire_idle = NULL;
	while ((hk = g_queue_pop_head(self->fires)) != NULL) {
		const gchar *mode;
		gboolean oneshot = FALSE;

		self->n_fired++;
		hk_announce(self, NULL, "fired %s", hk->id);
		if (self->dbus_service != NULL)
			hk_dbus_emit(self->dbus_service, "Fired", hk->id, hk->key);
		/* A one-shot mode ends after its hotkey, unless the hotkey
		 * itself went somewhere else */
		mode = self->compositor != NULL
		       ? gowl_compositor_get_key_mode(self->compositor) : "default";
		if (self->set != NULL && hk->mode != NULL
		    && g_strcmp0(hk->mode, mode) == 0) {
			HkMode *md = g_hash_table_lookup(self->set->modes, mode);

			oneshot = md != NULL && md->oneshot;
		}
		if (!hk->is_remap)
			hk_run_actions(self, hk->actions, hk->id, "hotkey", hk->key,
			               NULL, TRUE);
		if (oneshot && self->compositor != NULL
		    && g_strcmp0(gowl_compositor_get_key_mode(self->compositor),
		                 mode) == 0)
			gowl_compositor_set_key_mode(self->compositor, "default");
		hk_hotkey_unref(hk);
	}
}

/* Runs @hk's actions a loop turn from now. */
void
hk_queue_fire(
	GowlModuleHotkey *self,
	HkHotkey         *hk
){
	if (self->compositor == NULL)
		return;
	g_queue_push_tail(self->fires, hk_hotkey_ref(hk));
	if (self->fire_idle == NULL)
		self->fire_idle = wl_event_loop_add_idle(
			gowl_compositor_get_event_loop(self->compositor), fire_idle,
			self);
}

/* ── Runners ─────────────────────────────────────────────────────────── */

static void
frame_free(gpointer p)
{
	HkFrame *f = p;

	g_ptr_array_unref(f->actions);
	g_strfreev(f->args);
	g_free(f);
}

static void
push_frame(
	HkRunner            *r,
	GPtrArray           *actions,
	const gchar * const *args
){
	HkFrame *f = g_new0(HkFrame, 1);

	f->actions = g_ptr_array_ref(actions);
	f->args = g_strdupv((gchar **)args);
	g_ptr_array_add(r->frames, f);
}

static void
runner_free(HkRunner *r)
{
	if (r->timer != NULL)
		wl_event_source_remove(r->timer);
	if (r->proc != NULL)
		gowl_subprocess_cancel(r->proc);
	g_free(r->id);
	g_free(r->trigger);
	g_free(r->detail);
	g_free(r->key);
	g_ptr_array_unref(r->frames);
	g_free(r->input);
	g_free(r->result);
	g_free(r->c_key);
	g_free(r);
}

/* A runner is done: out of the counts, its waiter told, freed. */
static void
runner_end(HkRunner *r)
{
	GowlModuleHotkey *self = r->self;
	guint i;

	g_ptr_array_remove(self->runners, r);
	if (r->counted)
		running_add(self, r->id, -1);
	if (self->input.runner == r)
		self->input.runner = NULL;
	for (i = 0; i < self->plays->length; i++) {
		HkPlay *p = g_queue_peek_nth(self->plays, i);

		if (p->runner == r)
			p->runner = NULL;
	}
	if (r->done != NULL)
		r->done(r, r->done_data);
	runner_free(r);
}

/**
 * hk_run_actions:
 * @self: the module
 * @actions: (element-type HkAction): what to do
 * @id: (nullable): whose they are -- counted against max-running
 * @trigger: hotkey, hotstring, timer, event, function, ipc, ...
 * @detail: (nullable): more about it (the key, the event)
 * @args: (nullable): arguments (`call')
 * @count: whether it counts against @id's max-running
 *
 * Starts a runner; it may finish before this returns.
 *
 * Returns: (nullable) (transfer none): the runner while it is still
 *   running, else %NULL
 */
HkRunner *
hk_run_actions(
	GowlModuleHotkey    *self,
	GPtrArray           *actions,
	const gchar         *id,
	const gchar         *trigger,
	const gchar         *detail,
	const gchar * const *args,
	gboolean             count
){
	HkRunner *r;
	GowlClient *focused;
	guint serial;

	if (actions == NULL || actions->len == 0 || self->compositor == NULL)
		return NULL;
	if (self->runners->len >= HK_MAX_RUNNERS) {
		hk_fail(self, "%u action lists are running already; %s not started",
		        self->runners->len, id != NULL ? id : "this");
		return NULL;
	}
	r = g_new0(HkRunner, 1);
	r->self = self;
	r->serial = serial = ++self->next_serial;
	r->id = g_strdup(id);
	r->trigger = g_strdup(trigger);
	r->detail = g_strdup(detail);
	r->key = g_strdup(detail);
	focused = gowl_compositor_get_focused_client(self->compositor);
	r->focused_id = focused != NULL ? gowl_client_get_id(focused) : 0;
	r->frames = g_ptr_array_new_with_free_func(frame_free);
	push_frame(r, actions, args);
	if (count && id != NULL) {
		r->counted = TRUE;
		running_add(self, id, 1);
	}
	g_ptr_array_add(self->runners, r);
	runner_step(r);
	/* runner_step may have freed it; look it up again by serial */
	{
		guint i;

		for (i = 0; i < self->runners->len; i++)
			if (((HkRunner *)g_ptr_array_index(self->runners, i))->serial
			    == serial)
				return g_ptr_array_index(self->runners, i);
	}
	return NULL;
}

/* Stops every runner, typing and pending fire -- `hotkey-cancel', the
 * module going away. */
void
hk_cancel_all(GowlModuleHotkey *self)
{
	HkHotkey *hk;
	HkPlay *p;

	while (self->runners->len > 0) {
		HkRunner *r = g_ptr_array_index(self->runners, 0);

		r->done = NULL;
		runner_end(r);
	}
	while ((hk = g_queue_pop_head(self->fires)) != NULL)
		hk_hotkey_unref(hk);
	if (self->fire_idle != NULL) {
		wl_event_source_remove(self->fire_idle);
		self->fire_idle = NULL;
	}
	while ((p = g_queue_pop_head(self->plays)) != NULL) {
		hk_send_plan_free(p->plan);
		g_free(p);
	}
	if (self->play_timer != NULL) {
		wl_event_source_remove(self->play_timer);
		self->play_timer = NULL;
	}
	self->playing = FALSE;
}

/* ── Substitution ────────────────────────────────────────────────────── */

/*
 * {{id}} {{key}} {{trigger}} {{app-id}} {{title}} {{input}} {{prior}}
 * {{args}} {{1}}..{{9}} in a text an action types, shows or copies.
 * Never in a shell line (that gets the environment instead) and never
 * in a form evaluated as code.
 */
static gchar *
substitute(
	HkRunner    *r,
	const gchar *text
){
	GowlModuleHotkey *self = r->self;
	HkFrame *f = g_ptr_array_index(r->frames, r->frames->len - 1);
	GString *out;
	const gchar *p;
	GowlClient *c;

	if (strstr(text, "{{") == NULL)
		return g_strdup(text);
	c = hk_client_by_id(self, r->focused_id);
	out = g_string_new(NULL);
	for (p = text; *p != '\0'; ) {
		const gchar *close;
		g_autofree gchar *name = NULL;
		const gchar *v = NULL;
		g_autofree gchar *owned = NULL;

		if (p[0] != '{' || p[1] != '{'
		    || (close = strstr(p + 2, "}}")) == NULL) {
			g_string_append_c(out, *p++);
			continue;
		}
		name = g_strstrip(g_strndup(p + 2, (gsize)(close - p - 2)));
		if (g_strcmp0(name, "id") == 0)
			v = r->id;
		else if (g_strcmp0(name, "key") == 0)
			v = r->key;
		else if (g_strcmp0(name, "trigger") == 0)
			v = r->trigger;
		else if (g_strcmp0(name, "app-id") == 0)
			v = c != NULL ? gowl_client_get_app_id(c) : NULL;
		else if (g_strcmp0(name, "title") == 0)
			v = c != NULL ? gowl_client_get_title(c) : NULL;
		else if (g_strcmp0(name, "input") == 0)
			v = r->input;
		else if (g_strcmp0(name, "prior") == 0)
			v = hk_engine_get_prior(self->engine, NULL);
		else if (g_strcmp0(name, "args") == 0)
			v = owned = f->args != NULL ? g_strjoinv(" ", f->args) : NULL;
		else if (g_ascii_isdigit(name[0]) && name[1] == '\0') {
			guint i = (guint)(name[0] - '1');

			if (name[0] != '0' && f->args != NULL
			    && i < g_strv_length(f->args))
				v = f->args[i];
		} else {
			/* Not ours: left as written */
			g_string_append_len(out, p, (gssize)(close + 2 - p));
			p = close + 2;
			continue;
		}
		if (v != NULL)
			g_string_append(out, v);
		p = close + 2;
	}
	return g_string_free(out, FALSE);
}

/* A Lisp string literal: only " and \ escaped (never octal -- the
 * reader turns those into raw bytes). */
static void
append_lisp_string(
	GString     *out,
	const gchar *s
){
	g_string_append_c(out, '"');
	for (; s != NULL && *s != '\0'; s++) {
		if (*s == '"' || *s == '\\')
			g_string_append_c(out, '\\');
		g_string_append_c(out, *s);
	}
	g_string_append_c(out, '"');
}

/* %i (the id), %a (the arguments, a list of strings), %t (the trigger)
 * and %% in a custom form, each a Lisp literal. */
static gchar *
substitute_form(
	HkRunner    *r,
	const gchar *form,
	const gchar * const *args
){
	GString *out = g_string_new(NULL);
	const gchar *p;
	guint i;

	for (p = form; *p != '\0'; p++) {
		if (p[0] != '%' || p[1] == '\0') {
			g_string_append_c(out, *p);
			continue;
		}
		switch (p[1]) {
		case 'i':
			append_lisp_string(out, r->id != NULL ? r->id : "");
			p++;
			break;
		case 't':
			append_lisp_string(out, r->trigger);
			p++;
			break;
		case 'a':
			g_string_append(out, "(list");
			for (i = 0; args != NULL && args[i] != NULL; i++) {
				g_string_append_c(out, ' ');
				append_lisp_string(out, args[i]);
			}
			g_string_append_c(out, ')');
			p++;
			break;
		case '%':
			g_string_append_c(out, '%');
			p++;
			break;
		default:
			g_string_append_c(out, *p);
		}
	}
	return g_string_free(out, FALSE);
}

/* The environment a shell or run action gets, as `env' arguments. */
static void
env_argv(
	HkRunner  *r,
	GPtrArray *argv
){
	GowlModuleHotkey *self = r->self;
	HkFrame *f = g_ptr_array_index(r->frames, r->frames->len - 1);
	GowlClient *c = hk_client_by_id(self, r->focused_id);
	const gchar *prior = hk_engine_get_prior(self->engine, NULL);

	g_ptr_array_add(argv, g_strdup("env"));
	g_ptr_array_add(argv, g_strdup_printf("GOWL_HOTKEY_ID=%s",
	                                      r->id != NULL ? r->id : ""));
	g_ptr_array_add(argv, g_strdup_printf("GOWL_HOTKEY_KEY=%s",
	                                      r->key != NULL ? r->key : ""));
	g_ptr_array_add(argv, g_strdup_printf("GOWL_HOTKEY_TRIGGER=%s",
	                                      r->trigger));
	g_ptr_array_add(argv, g_strdup_printf("GOWL_HOTKEY_APP_ID=%s",
	                c != NULL && gowl_client_get_app_id(c) != NULL
	                ? gowl_client_get_app_id(c) : ""));
	g_ptr_array_add(argv, g_strdup_printf("GOWL_HOTKEY_TITLE=%s",
	                c != NULL && gowl_client_get_title(c) != NULL
	                ? gowl_client_get_title(c) : ""));
	g_ptr_array_add(argv, g_strdup_printf("GOWL_HOTKEY_WINDOW=%u",
	                                      r->focused_id));
	g_ptr_array_add(argv, g_strdup_printf("GOWL_HOTKEY_PID=%d",
	                c != NULL ? (gint)gowl_client_get_pid(c) : 0));
	g_ptr_array_add(argv, g_strdup_printf("GOWL_HOTKEY_PRIOR=%s",
	                                      prior != NULL ? prior : ""));
	g_ptr_array_add(argv, g_strdup_printf("GOWL_HOTKEY_INPUT=%s",
	                                      r->input != NULL ? r->input : ""));
	if (f->args != NULL) {
		g_autofree gchar *joined = g_strjoinv(" ", f->args);

		g_ptr_array_add(argv, g_strdup_printf("GOWL_HOTKEY_ARGS=%s", joined));
	}
}

/* ── Waiting ─────────────────────────────────────────────────────────── */

static HkRunner *
runner_by_serial(
	GowlModuleHotkey *self,
	guint             serial
){
	guint i;

	for (i = 0; i < self->runners->len; i++) {
		HkRunner *r = g_ptr_array_index(self->runners, i);

		if (r->serial == serial)
			return r;
	}
	return NULL;
}

static gint
sleep_done(gpointer data)
{
	HkRunner *r = data;

	wl_event_source_remove(r->timer);
	r->timer = NULL;
	runner_resume(r);
	return 0;
}

/* Spawns on the loop by serial, so a runner cancelled meanwhile is not
 * touched by its child finishing. */
typedef struct {
	GowlModuleHotkey *self;
	guint             serial;
	gchar            *what;
} ProcWait;

static void
proc_done(
	gint         status,
	const gchar *out,
	const gchar *err,
	gpointer     data
){
	ProcWait *w = data;
	HkRunner *r = runner_by_serial(w->self, w->serial);
	GError *e = NULL;

	(void)out;
	if (status == -1)
		hk_fail(w->self, "%s ran past its timeout and was killed", w->what);
	else if (!g_spawn_check_wait_status(status, &e)) {
		g_autofree gchar *last = NULL;

		if (err != NULL && *err != '\0') {
			gchar **lines = g_strsplit(g_strchomp(g_strdup(err)), "\n", -1);

			last = g_strdup(lines[0] != NULL
			                ? lines[g_strv_length(lines) - 1] : "");
			g_strfreev(lines);
		}
		hk_fail(w->self, "%s: %s%s%s", w->what, e->message,
		        last != NULL ? ": " : "", last != NULL ? last : "");
		g_error_free(e);
	}
	if (r != NULL) {
		r->proc = NULL;
		runner_resume(r);
	}
	g_free(w->what);
	g_free(w);
}

/* A shell or run action.  Without `wait' it is fire and forget (GLib
 * reaps it through its intermediate child: nothing is waited on). */
static gboolean
spawn_action(
	HkRunner *r,
	HkAction *a
){
	GowlModuleHotkey *self = r->self;
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	GError *e = NULL;
	guint i;

	env_argv(r, argv);
	if (a->kind == HK_ACT_SHELL) {
		g_ptr_array_add(argv, g_strdup("/bin/sh"));
		g_ptr_array_add(argv, g_strdup("-c"));
		g_ptr_array_add(argv, g_strdup(a->text));
	} else {
		for (i = 0; a->argv[i] != NULL; i++)
			g_ptr_array_add(argv, g_strdup(a->argv[i]));
	}
	g_ptr_array_add(argv, NULL);
	if (!a->wait) {
		if (!g_spawn_async(NULL, (gchar **)argv->pdata, NULL,
		                   G_SPAWN_SEARCH_PATH | G_SPAWN_STDIN_FROM_DEV_NULL,
		                   NULL, NULL, NULL, &e)) {
			hk_fail(self, "%s: %s", r->id != NULL ? r->id : "spawn",
			        e->message);
			g_error_free(e);
		}
		return FALSE;
	}
	{
		ProcWait *w = g_new0(ProcWait, 1);

		w->self = self;
		w->serial = r->serial;
		w->what = g_strdup_printf("%s (%s)", r->id != NULL ? r->id : "action",
		                          a->kind == HK_ACT_SHELL ? a->text
		                                                  : a->argv[0]);
		r->proc = gowl_subprocess_spawn(
			gowl_compositor_get_event_loop(self->compositor),
			(const gchar * const *)argv->pdata, a->timeout_ms, proc_done, w,
			&e);
		if (r->proc == NULL) {
			hk_fail(self, "%s: %s", w->what, e->message);
			g_error_free(e);
			g_free(w->what);
			g_free(w);
			return FALSE;
		}
	}
	return TRUE;
}

/* The macro module tells every listener when any run ends. */
static void
macro_finished(
	guint        id,
	const gchar *name,
	const gchar *result,
	const gchar *error,
	gpointer     data
){
	GowlModuleHotkey *self = data;
	guint i;

	(void)result;
	for (i = 0; i < self->runners->len; i++) {
		HkRunner *r = g_ptr_array_index(self->runners, i);

		if (r->macro_id != id)
			continue;
		if (error != NULL)
			hk_fail(self, "macro %s: %s", name, error);
		r->macro_id = 0;
		runner_resume(r);
		return;
	}
}

static gboolean
macro_action(
	HkRunner *r,
	HkAction *a
){
	GowlModuleHotkey *self = r->self;
	g_autoptr(GString) line = g_string_new("macro-run --trigger=hotkey ");
	g_autofree gchar *detail = NULL;
	g_autofree gchar *q = NULL;
	g_autofree gchar *reply = NULL;
	const gchar *idp;
	guint i;

	detail = g_strdup_printf("--detail=%s", r->id != NULL ? r->id : "");
	q = g_shell_quote(detail);
	g_string_append(line, q);
	g_free(q);
	q = g_shell_quote(a->name);
	g_string_append_printf(line, " -- %s", q);
	for (i = 0; a->argv != NULL && a->argv[i] != NULL; i++) {
		g_autofree gchar *qa = g_shell_quote(a->argv[i]);

		g_string_append_printf(line, " %s", qa);
	}
	reply = gowl_compositor_run_command(self->compositor, line->str);
	if (reply == NULL) {
		hk_fail(self, "macro %s: the macro module is not loaded", a->name);
		return FALSE;
	}
	if (!g_str_has_prefix(reply, "OK")) {
		hk_fail(self, "macro %s: %s", a->name, reply);
		return FALSE;
	}
	/* `OK started NAME id=N': it finishes later */
	if (a->wait && g_str_has_prefix(reply, "OK started")
	    && (idp = strstr(reply, " id=")) != NULL) {
		guint64 id = g_ascii_strtoull(idp + 4, NULL, 10);

		if (id > 0) {
			r->macro_id = (guint)id;
			return TRUE;
		}
	}
	return FALSE;
}

/* ── C ───────────────────────────────────────────────────────────────── */

typedef struct {
	GowlModuleHotkey *self;
	HkCSpec           spec;
	gchar            *key;
	gchar            *cache_dir;
	gchar            *so_path;
	GError           *error;
	gboolean          script;    /* a `scripts:' file, not an action */
} CBuild;

static void
cbuild_free(gpointer p)
{
	CBuild *b = p;

	hk_c_spec_clear(&b->spec);
	g_free(b->key);
	g_free(b->cache_dir);
	g_free(b->so_path);
	g_clear_error(&b->error);
	g_free(b);
}

static void script_built (GowlModuleHotkey *self, gpointer data);

/* A build came back: open it, and wake every runner waiting for it. */
static void
c_built(
	GowlModuleHotkey *self,
	gpointer          data
){
	CBuild *b = data;
	GModule *module = NULL;
	gpointer sym = NULL;
	GError *e = NULL;
	guint i;

	g_hash_table_remove(self->c_building, b->key);
	if (b->so_path == NULL) {
		g_hash_table_replace(self->c_faulted, g_strdup(b->key),
		                     g_strdup(b->error != NULL ? b->error->message
		                                               : "did not build"));
	} else if ((module = hk_c_open(b->so_path, &e)) == NULL) {
		g_hash_table_replace(self->c_faulted, g_strdup(b->key),
		                     g_strdup(e->message));
		g_error_free(e);
	} else if (!g_module_symbol(module, HK_C_ACTION_SYMBOL, &sym)
	           || sym == NULL) {
		g_hash_table_replace(self->c_faulted, g_strdup(b->key),
		                     g_strdup("it does not export "
		                              HK_C_ACTION_SYMBOL "()"));
	} else {
		g_hash_table_replace(self->c_funcs, g_strdup(b->key), sym);
	}
	if (g_hash_table_contains(self->c_faulted, b->key))
		hk_fail(self, "c action: %s",
		        (const gchar *)g_hash_table_lookup(self->c_faulted, b->key));
	/* Every runner waiting on this build carries on (and calls it, or
	 * reports the fault) */
	for (i = 0; i < self->runners->len; ) {
		HkRunner *r = g_ptr_array_index(self->runners, i);
		guint before = self->runners->len;

		if (r->c_key != NULL && g_strcmp0(r->c_key, b->key) == 0) {
			g_clear_pointer(&r->c_key, g_free);
			r->waiting = FALSE;
			runner_step(r);
			if (self->runners->len < before)
				continue;
		}
		i++;
	}
}

static gpointer
c_thread(gpointer data)
{
	CBuild *b = data;

	b->so_path = hk_c_build(&b->spec, b->cache_dir, &b->error);
	hk_post(b->self, b->script ? script_built : c_built, b, cbuild_free);
	return NULL;
}

/* Runs a `c' action: built and opened once, called every time under the
 * fault guard.  TRUE when the runner must wait for the build. */
static gboolean
c_action(
	HkRunner *r,
	HkAction *a
){
	GowlModuleHotkey *self = r->self;
	HkCSpec spec;
	g_autofree gchar *key = NULL;
	HkCActionFunc func;
	const gchar *faulted;

	memset(&spec, 0, sizeof spec);
	if (a->is_file)
		spec.file = g_str_has_prefix(a->text, "~/")
		            ? g_build_filename(g_get_home_dir(), a->text + 2, NULL)
		            : g_strdup(a->text);
	else
		spec.code = g_strdup(a->text);
	spec.cflags = g_strdup(a->cflags);
	key = hk_c_spec_key(&spec);

	if ((faulted = g_hash_table_lookup(self->c_faulted, key)) != NULL) {
		hk_fail(self, "c action held back: %s (hotkey-reload retries it)",
		        faulted);
		hk_c_spec_clear(&spec);
		return FALSE;
	}
	func = g_hash_table_lookup(self->c_funcs, key);
	if (func != NULL) {
		GowlHotkeyCall *call;
		HkFrame *f = g_ptr_array_index(r->frames, r->frames->len - 1);
		GError *e = NULL;

		call = gowl_hotkey_call_new(self->compositor,
		                            hk_client_by_id(self, r->focused_id),
		                            r->id, r->trigger, r->detail,
		                            (const gchar * const *)f->args);
		if (!hk_c_call_action(func, call, self->c_timeout_ms, &e)) {
			g_hash_table_replace(self->c_faulted, g_strdup(key),
			                     g_strdup(e->message));
			hk_fail(self, "%s", e->message);
			g_error_free(e);
		} else {
			g_free(r->result);
			r->result = gowl_hotkey_call_steal_result(call);
		}
		gowl_hotkey_call_free(call);
		hk_c_spec_clear(&spec);
		return FALSE;
	}
	/* Build it, once (hotkey-reload forgets every build, so an edited
	 * file is picked up then; crispy's cache makes an unchanged source
	 * free).  The runner waits and does this action again, which then
	 * finds the build. */
	r->c_key = g_strdup(key);
	{
		HkFrame *top = g_ptr_array_index(r->frames, r->frames->len - 1);

		top->index--;
	}
	if (!g_hash_table_contains(self->c_building, key)) {
		CBuild *b = g_new0(CBuild, 1);

		b->self = self;
		b->spec = spec;
		b->key = g_strdup(key);
		b->cache_dir = g_strdup(self->cache_dir);
		g_hash_table_add(self->c_building, g_strdup(key));
		g_thread_unref(g_thread_new("hotkey-cc", c_thread, b));
	} else {
		hk_c_spec_clear(&spec);
	}
	return TRUE;
}

/* ── Functions ───────────────────────────────────────────────────────── */

/* A custom action (cmacs: an Elisp form) with %i %a %t substituted. */
static void
run_custom(
	HkRunner            *r,
	const gchar         *form,
	const gchar * const *args
){
	GowlModuleHotkey *self = r->self;
	g_autofree gchar *text = substitute_form(r, form, args);
	GowlKeybindEntry kb;

	memset(&kb, 0, sizeof kb);
	kb.action = GOWL_ACTION_CUSTOM;
	kb.arg = text;
	if (!gowl_compositor_run_keybind_entry(self->compositor, &kb))
		hk_fail(self, "custom action: nothing handles it (custom actions "
		        "are the embedder's -- cmacs evaluates them as Elisp)");
}

/* `call: NAME': a `functions:' list (a frame on this runner), a C
 * function, or one defined with hotkey-func-define. */
static gboolean
call_action(
	HkRunner *r,
	HkAction *a
){
	GowlModuleHotkey *self = r->self;
	HkFunction *fn = self->set != NULL
	                 ? g_hash_table_lookup(self->set->functions, a->name) : NULL;
	const gchar *form;
	HkFrame *top = g_ptr_array_index(r->frames, r->frames->len - 1);
	g_auto(GStrv) args = NULL;
	guint i;

	/* Arguments, with the caller's {{...}} substituted */
	if (a->argv != NULL) {
		args = g_new0(gchar *, g_strv_length(a->argv) + 1);
		for (i = 0; a->argv[i] != NULL; i++)
			args[i] = substitute(r, a->argv[i]);
	} else {
		args = g_strdupv(top->args);
	}
	if (fn != NULL) {
		if (r->frames->len >= HK_MAX_CALL_DEPTH) {
			hk_fail(self, "%s: calls nested %u deep (a loop?)", a->name,
			        r->frames->len);
			return FALSE;
		}
		push_frame(r, fn->actions, (const gchar * const *)args);
		return FALSE;
	}
	if (gowl_hotkey_has_func(a->name)) {
		GowlHotkeyCall *call;
		GError *e = NULL;

		call = gowl_hotkey_call_new(self->compositor,
		                            hk_client_by_id(self, r->focused_id),
		                            r->id, r->trigger, r->detail,
		                            (const gchar * const *)args);
		if (!gowl_hotkey_invoke_func(a->name, call, self->c_timeout_ms, &e)) {
			hk_fail(self, "%s", e->message);
			g_error_free(e);
		} else {
			g_free(r->result);
			r->result = gowl_hotkey_call_steal_result(call);
		}
		gowl_hotkey_call_free(call);
		return FALSE;
	}
	if ((form = g_hash_table_lookup(self->elisp_funcs, a->name)) != NULL) {
		run_custom(r, form, (const gchar * const *)args);
		return FALSE;
	}
	hk_fail(self, "no function %s (functions:, a C function, or "
	        "hotkey-func-define)", a->name);
	return FALSE;
}

/*
 * hotkey-call NAME ARGS: a C or Elisp function's result at once; a
 * `functions:' list is started and answered with `started'.
 */
gchar *
hk_call_function_now(
	GowlModuleHotkey    *self,
	const gchar         *name,
	const gchar * const *args,
	const gchar         *trigger,
	GError             **error
){
	HkFunction *fn = self->set != NULL
	                 ? g_hash_table_lookup(self->set->functions, name) : NULL;
	const gchar *form;

	if (gowl_hotkey_has_func(name)) {
		GowlHotkeyCall *call;
		gchar *result;

		call = gowl_hotkey_call_new(self->compositor, self->compositor != NULL
			? gowl_compositor_get_focused_client(self->compositor) : NULL,
			name, trigger, NULL, args);
		if (!gowl_hotkey_invoke_func(name, call, self->c_timeout_ms, error)) {
			gowl_hotkey_call_free(call);
			return NULL;
		}
		result = gowl_hotkey_call_steal_result(call);
		gowl_hotkey_call_free(call);
		return result != NULL ? result : g_strdup("");
	}
	if (fn != NULL) {
		hk_run_actions(self, fn->actions, name, trigger, NULL, args, FALSE);
		return g_strdup("started");
	}
	if ((form = g_hash_table_lookup(self->elisp_funcs, name)) != NULL) {
		g_autoptr(GPtrArray) one = g_ptr_array_new_with_free_func(
			(GDestroyNotify)hk_action_free);
		HkAction *a = g_new0(HkAction, 1);

		a->kind = HK_ACT_CALL;
		a->name = g_strdup(name);
		a->argv = g_strdupv((gchar **)args);
		a->params = json_object_new();
		g_ptr_array_add(one, a);
		hk_run_actions(self, one, name, trigger, NULL, args, FALSE);
		return g_strdup("started");
	}
	g_set_error(error, GOWL_HOTKEY_ERROR, GOWL_HOTKEY_ERROR_NOT_FOUND,
	            "no function %s", name);
	return NULL;
}

/* ── Windows and the mouse ───────────────────────────────────────────── */

/* The window an action is about: `target:', else the focused one. */
static GowlClient *
action_window(
	HkRunner *r,
	HkAction *a
){
	GowlModuleHotkey *self = r->self;
	GError *e = NULL;
	GowlClient *c;

	if (a->target == NULL)
		return self->compositor != NULL
		       ? gowl_compositor_get_focused_client(self->compositor) : NULL;
	c = hk_find_window(self, a->target, NULL, &e);
	if (e != NULL) {
		hk_fail(self, "target `%s': %s", a->target, e->message);
		g_error_free(e);
	}
	return c;
}

static void
rpc_call(
	HkRunner    *r,
	const gchar *method,
	JsonObject  *params
){
	GowlModuleHotkey *self = r->self;
	GowlIpc *ipc = self->compositor != NULL
	               ? gowl_compositor_get_ipc(self->compositor) : NULL;
	g_autoptr(JsonNode) result = NULL;
	GError *e = NULL;

	if (ipc == NULL) {
		hk_fail(self, "rpc %s: the compositor has no IPC server", method);
		return;
	}
	result = gowl_ipc_call(ipc, method, params, &e);
	if (e != NULL) {
		hk_fail(self, "rpc %s: %s", method, e->message);
		g_error_free(e);
	}
}

static void
window_action(
	HkRunner *r,
	HkAction *a
){
	GowlModuleHotkey *self = r->self;
	g_autoptr(JsonObject) params = json_object_new();
	GowlClient *c;
	const gchar *method;
	GList *l;

	/* group-activate: the next window of the group after the focused
	 * one, cycling -- AutoHotkey's GroupActivate */
	if (g_strcmp0(a->name, "group-activate") == 0) {
		HkNamedCondition *nc = self->set != NULL
			? g_hash_table_lookup(self->set->groups, a->text) : NULL;
		GowlClient *focused = gowl_compositor_get_focused_client(
			self->compositor);
		GowlClient *next;

		if (nc == NULL || nc->cond.when_text == NULL) {
			hk_fail(self, "group-activate: no group %s", a->text);
			return;
		}
		next = hk_find_window(self, nc->cond.when_text, focused, NULL);
		if (next != NULL)
			gowl_compositor_activate_client(self->compositor, next);
		return;
	}
	c = action_window(r, a);
	if (c == NULL) {
		if (a->target != NULL)
			hk_log(self, "window %s: nothing matches %s", a->name, a->target);
		return;
	}
	/* The parsed parameters, the window, and the method */
	{
		GList *keys = json_object_get_members(a->params);

		for (l = keys; l != NULL; l = l->next)
			if (g_strcmp0(l->data, "x-method") != 0)
				json_object_set_member(params, l->data, json_node_copy(
					json_object_get_member(a->params, l->data)));
		g_list_free(keys);
	}
	json_object_set_int_member(params, "id", gowl_client_get_id(c));
	method = json_object_get_string_member(a->params, "x-method");
	rpc_call(r, method, params);
}

static void
mouse_action(
	HkRunner *r,
	HkAction *a
){
	GowlModuleHotkey *self = r->self;
	gdouble x = 0;
	gdouble y = 0;
	gint i;
	gint level = a->level >= 0 ? a->level : self->send_level;

	gowl_compositor_get_cursor_position(self->compositor, &x, &y);
	if (a->has_xy) {
		gdouble ox = 0;
		gdouble oy = 0;

		if (a->window_rel) {
			GowlClient *c = action_window(r, a);

			if (c == NULL) {
				hk_fail(self, "mouse %s: no window to be relative to", a->name);
				return;
			}
			ox = c->frame.x;
			oy = c->frame.y;
		} else if (a->relative) {
			ox = x;
			oy = y;
		}
		x = ox + a->x;
		y = oy + a->y;
		gowl_compositor_inject_pointer_warp(self->compositor, x, y);
		gowl_compositor_inject_frame(self->compositor);
	}
	if (g_strcmp0(a->name, "move") == 0)
		return;
	if (g_strcmp0(a->name, "wheel") == 0) {
		gint saved = self->inject_level;

		self->injecting++;
		self->inject_level = level;
		gowl_compositor_inject_axis(self->compositor, a->horizontal,
		                            15.0 * a->wheel, 120 * a->wheel);
		gowl_compositor_inject_frame(self->compositor);
		self->inject_level = saved;
		self->injecting--;
		return;
	}
	if (g_strcmp0(a->name, "drag") == 0) {
		hk_inject_button(self, a->button, TRUE, level);
		gowl_compositor_inject_pointer_warp(self->compositor,
			(a->relative ? x : 0) + a->x2, (a->relative ? y : 0) + a->y2);
		gowl_compositor_inject_frame(self->compositor);
		hk_inject_button(self, a->button, FALSE, level);
		return;
	}
	for (i = 0; i < a->count; i++) {
		if (g_strcmp0(a->name, "up") != 0)
			hk_inject_button(self, a->button, TRUE, level);
		if (g_strcmp0(a->name, "down") != 0)
			hk_inject_button(self, a->button, FALSE, level);
	}
}

/* ── One action ──────────────────────────────────────────────────────── */

/* Does @a; TRUE when the runner must now wait. */
static gboolean
do_action(
	HkRunner *r,
	HkAction *a
){
	GowlModuleHotkey *self = r->self;
	GError *e = NULL;

	switch (a->kind) {
	case HK_ACT_ACTION: {
		GEnumClass *klass = g_type_class_ref(GOWL_TYPE_ACTION);
		GEnumValue *v = g_enum_get_value_by_nick(klass, a->name);
		GowlKeybindEntry kb;

		g_type_class_unref(klass);
		if (v == NULL) {
			hk_fail(self, "unknown action %s", a->name);
			return FALSE;
		}
		memset(&kb, 0, sizeof kb);
		kb.action = v->value;
		kb.arg = a->text;
		gowl_compositor_run_keybind_entry(self->compositor, &kb);
		return FALSE;
	}
	case HK_ACT_COMMAND: {
		g_autofree gchar *reply = gowl_compositor_ipc_command(self->compositor,
		                                                      a->text);

		if (reply == NULL)
			hk_fail(self, "command `%s': unknown", a->text);
		else if (g_str_has_prefix(reply, "ERROR"))
			hk_fail(self, "command `%s': %s", a->text, reply);
		return FALSE;
	}
	case HK_ACT_RPC:
		rpc_call(r, a->name, a->params);
		return FALSE;
	case HK_ACT_WINDOW:
		window_action(r, a);
		return FALSE;
	case HK_ACT_MACRO:
		return macro_action(r, a);
	case HK_ACT_SHELL:
	case HK_ACT_RUN:
		return spawn_action(r, a);
	case HK_ACT_C:
		return c_action(r, a);
	case HK_ACT_CALL:
		return call_action(r, a);
	case HK_ACT_SEND:
	case HK_ACT_SEND_TEXT: {
		g_autofree gchar *text = a->kind == HK_ACT_SEND_TEXT
		                         ? substitute(r, a->text) : g_strdup(a->text);
		HkPlay *queued;

		if (!hk_send(self, text, a->kind == HK_ACT_SEND_TEXT, r, &e)) {
			hk_fail(self, "send: %s", e->message);
			g_error_free(e);
			return FALSE;
		}
		/* The plan just queued takes this action's own pacing */
		queued = g_queue_peek_tail(self->plays);
		if (queued != NULL && queued->runner == r) {
			if (a->send_mode >= 0)
				queued->event_mode = a->send_mode == 1;
			if (a->ms > 0) {
				queued->delay_ms = a->ms;
				queued->event_mode = TRUE;
			}
			if (a->level >= 0)
				queued->level = a->level;
		}
		return TRUE;
	}
	case HK_ACT_MODE:
		if (!hk_enter_mode(self, a->name, &e)) {
			hk_fail(self, "mode: %s", e->message);
			g_error_free(e);
		}
		return FALSE;
	case HK_ACT_SUSPEND:
		hk_set_suspended(self, a->toggle == HK_TOGGLE_TOGGLE
		                 ? !hk_engine_get_suspended(self->engine)
		                 : a->toggle == HK_TOGGLE_ON, "action");
		return FALSE;
	case HK_ACT_RELOAD:
		/* The runner holds its own reference to its actions, so the set
		 * it came from may be replaced under it */
		hk_reload(self);
		return FALSE;
	case HK_ACT_MOUSE:
		mouse_action(r, a);
		return FALSE;
	case HK_ACT_CLIPBOARD: {
		g_autofree gchar *text = substitute(r, a->text);
		GowlSeat *seat = gowl_compositor_get_seat(self->compositor);

		if (seat != NULL)
			gowl_seat_set_clipboard(seat, text);
		return FALSE;
	}
	case HK_ACT_NOTIFY: {
		g_autofree gchar *text = substitute(r, a->text);
		gboolean was = self->toast;

		self->toast = TRUE;
		hk_announce(self, text, NULL);
		self->toast = was;
		return FALSE;
	}
	case HK_ACT_SLEEP:
		r->timer = wl_event_loop_add_timer(
			gowl_compositor_get_event_loop(self->compositor), sleep_done, r);
		wl_event_source_timer_update(r->timer, (gint)MAX(a->ms, 1));
		return TRUE;
	case HK_ACT_CUSTOM: {
		HkFrame *f = g_ptr_array_index(r->frames, r->frames->len - 1);

		run_custom(r, a->text, (const gchar * const *)f->args);
		return FALSE;
	}
	case HK_ACT_INPUT:
		if (self->input.active)
			hk_input_stop(self, "replaced");
		hk_input_start(self, a->name, a->text,
		               (const gchar * const *)a->argv, r);
		return TRUE;
	case HK_ACT_CONTEXT: {
		gboolean cur = FALSE;

		gowl_hotkey_get_context(a->name, &cur);
		gowl_hotkey_set_context(a->name, a->toggle == HK_TOGGLE_TOGGLE
		                        ? !cur : a->toggle == HK_TOGGLE_ON);
		return FALSE;
	}
	case HK_ACT_LOCK:
		hk_set_lock(self, g_strcmp0(a->name, "capslock") == 0, a->toggle);
		return FALSE;
	case HK_ACT_HOTKEY:
		if (!hk_set_hotkey_enabled(self, a->name, a->toggle))
			hk_fail(self, "hotkey: no hotkey %s", a->name);
		return FALSE;
	default:
		return FALSE;
	}
}

/* Does actions until one has to wait, or the runner is done. */
static void
runner_step(HkRunner *r)
{
	GowlModuleHotkey *self = r->self;
	guint serial = r->serial;

	while (TRUE) {
		HkFrame *f;
		HkAction *a;

		if (r->frames->len == 0) {
			runner_end(r);
			return;
		}
		f = g_ptr_array_index(r->frames, r->frames->len - 1);
		if (f->index >= f->actions->len) {
			g_ptr_array_remove_index(r->frames, r->frames->len - 1);
			continue;
		}
		a = g_ptr_array_index(f->actions, f->index++);
		r->waiting = TRUE;
		if (do_action(r, a))
			return;
		/* An action may have cancelled everything (hotkey-cancel from a
		 * command action, a reload) */
		if (runner_by_serial(self, serial) == NULL)
			return;
		r->waiting = FALSE;
	}
}

static void
runner_resume(HkRunner *r)
{
	r->waiting = FALSE;
	runner_step(r);
}

/* ── Typing ──────────────────────────────────────────────────────────── */

static void
play_free(HkPlay *p)
{
	hk_send_plan_free(p->plan);
	g_free(p);
}

/* Lifts the modifiers held for the hotkey (unless {Blind}). */
static void
play_lift(
	GowlModuleHotkey *self,
	HkPlay           *p
){
	static const guint32 mod_syms[] = {
		XKB_KEY_Control_L, XKB_KEY_Control_R, XKB_KEY_Shift_L,
		XKB_KEY_Shift_R, XKB_KEY_Alt_L, XKB_KEY_Alt_R, XKB_KEY_Super_L,
		XKB_KEY_Super_R
	};
	guint i;

	p->n_lifted = 0;
	if (p->plan->blind)
		return;
	for (i = 0; i < G_N_ELEMENTS(mod_syms) && p->n_lifted < 8; i++) {
		GowlHotkeyInput in;
		guint32 kc;

		memset(&in, 0, sizeof in);
		in.kind = GOWL_HOTKEY_INPUT_KEY;
		in.keysym = mod_syms[i];
		if (!hk_engine_key_is_down(self->engine, &in))
			continue;
		kc = gowl_compositor_keysym_to_keycode(self->compositor, mod_syms[i],
		                                       NULL);
		if (kc == 0)
			continue;
		hk_inject_key(self, kc, FALSE, p->level);
		p->lifted[p->n_lifted++] = kc;
	}
}

/* ...and presses again the ones still physically held. */
static void
play_restore(
	GowlModuleHotkey *self,
	HkPlay           *p
){
	guint i;

	for (i = 0; i < p->n_lifted; i++) {
		GowlHotkeyInput in;

		memset(&in, 0, sizeof in);
		in.kind = GOWL_HOTKEY_INPUT_KEY;
		in.keycode = p->lifted[i];
		if (hk_engine_key_is_down(self->engine, &in))
			hk_inject_key(self, p->lifted[i], TRUE, p->level);
	}
	p->n_lifted = 0;
}

/* One event of a plan.  Returns how long to wait after it. */
static guint
play_event(
	GowlModuleHotkey *self,
	HkPlay           *p,
	HkSendEvent      *ev
){
	switch (ev->kind) {
	case HK_SEND_KEY:
		key_with_mods(self, ev->keycode, ev->keysym, ev->mods, ev->updown,
		              p->level);
		break;
	case HK_SEND_CHAR: {
		guint32 sym = xkb_utf32_to_keysym(ev->ch);
		gboolean shift = FALSE;
		guint32 kc = sym != XKB_KEY_NoSymbol
		             ? gowl_compositor_keysym_to_keycode(self->compositor, sym,
		                                                 &shift)
		             : 0;

		if (kc != 0) {
			key_with_mods(self, kc, 0, shift ? GOWL_KEY_MOD_SHIFT : 0, 0,
			              p->level);
		} else {
			/* Not on this layout: typed by the compositor's own means */
			gchar buf[8];
			gint n = g_unichar_to_utf8(ev->ch, buf);
			GowlClient *c = gowl_compositor_get_focused_client(
				self->compositor);

			buf[n] = '\0';
			if (c != NULL)
				gowl_compositor_send_text_to_client(self->compositor, c, buf);
		}
		break;
	}
	case HK_SEND_TEXT: {
		GowlClient *c = gowl_compositor_get_focused_client(self->compositor);

		if (c != NULL && gowl_compositor_send_text_to_client(self->compositor,
		                                                     c, ev->text) < 0)
			hk_fail(self, "send: the focused window refused text");
		break;
	}
	case HK_SEND_BUTTON: {
		guint32 mk[5];
		guint n = mod_keycodes(self, ev->mods, mk);
		guint j;

		for (j = 0; j < n; j++)
			hk_inject_key(self, mk[j], TRUE, p->level);
		if (ev->updown >= 0)
			hk_inject_button(self, ev->button, TRUE, p->level);
		if (ev->updown <= 0)
			hk_inject_button(self, ev->button, FALSE, p->level);
		for (j = n; j > 0; j--)
			hk_inject_key(self, mk[j - 1], FALSE, p->level);
		break;
	}
	case HK_SEND_SLEEP:
		return MAX(ev->ms, 1);
	default:
		break;
	}
	return p->event_mode ? MAX(p->delay_ms, 1) : 0;
}

static gint
play_tick(gpointer data)
{
	GowlModuleHotkey *self = data;

	play_kick(self);
	return 0;
}

/* Plays the queue until a delay is due, then sets the timer for it.
 * Always from the event loop (the play timer), never inside a key. */
static void
play_kick(GowlModuleHotkey *self)
{
	HkPlay *p;

	if (self->compositor == NULL)
		return;
	while ((p = g_queue_peek_head(self->plays)) != NULL) {
		if (!p->started) {
			p->started = TRUE;
			play_lift(self, p);
		}
		while (p->index < p->plan->events->len) {
			guint wait = play_event(self, p,
			                        &g_array_index(p->plan->events,
			                                       HkSendEvent, p->index++));

			if (wait > 0 && p->index < p->plan->events->len) {
				wl_event_source_timer_update(self->play_timer, (gint)wait);
				return;
			}
		}
		play_restore(self, p);
		g_queue_pop_head(self->plays);
		if (p->runner != NULL) {
			HkRunner *r = p->runner;

			p->runner = NULL;
			runner_resume(r);
		}
		play_free(p);
	}
	self->playing = FALSE;
}

/**
 * hk_send:
 * @self: the module
 * @keys: Send syntax, or text with @text
 * @text: @keys is literal text (SendText)
 * @runner: (nullable): the runner to wake when it has been typed
 * @error: a syntax error
 *
 * Queues typing for the next loop turn -- never inside a key event.
 *
 * Returns: %TRUE when queued
 */
gboolean
hk_send(
	GowlModuleHotkey *self,
	const gchar      *keys,
	gboolean          text,
	HkRunner         *runner,
	GError          **error
){
	HkSendPlan *plan;
	HkPlay *p;

	if (self->compositor == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
		            "the module has not started");
		return FALSE;
	}
	plan = text ? hk_send_text_plan(keys) : hk_send_parse(keys, error);
	if (plan == NULL)
		return FALSE;
	p = g_new0(HkPlay, 1);
	p->plan = plan;
	p->event_mode = self->event_mode || self->key_delay_ms > 0;
	p->delay_ms = self->key_delay_ms;
	p->level = self->send_level;
	p->runner = runner;
	g_queue_push_tail(self->plays, p);
	if (self->play_timer == NULL)
		self->play_timer = wl_event_loop_add_timer(
			gowl_compositor_get_event_loop(self->compositor), play_tick,
			self);
	if (!self->playing) {
		self->playing = TRUE;
		wl_event_source_timer_update(self->play_timer, 1);
	}
	return TRUE;
}

/* ── Modes ───────────────────────────────────────────────────────────── */

static gint
mode_timeout(gpointer data)
{
	GowlModuleHotkey *self = data;

	if (self->compositor != NULL && self->mode != NULL
	    && g_strcmp0(gowl_compositor_get_key_mode(self->compositor),
	                 self->mode) == 0)
		gowl_compositor_set_key_mode(self->compositor, "default");
	return 0;
}

/* Enters a mode (the compositor's key mode, which config keybinds in
 * that mode share); "default" leaves. */
gboolean
hk_enter_mode(
	GowlModuleHotkey *self,
	const gchar      *name,
	GError          **error
){
	if (self->compositor == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
		            "the module has not started");
		return FALSE;
	}
	if (name == NULL || *name == '\0' || strchr(name, ' ') != NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "a mode name is one word");
		return FALSE;
	}
	gowl_compositor_set_key_mode(self->compositor, name);
	return TRUE;
}

/* The compositor's key mode changed -- by us or by a keybind: the old
 * mode's on-exit, the new one's on-enter, toast and timeout. */
void
hk_mode_changed(
	GowlModuleHotkey *self,
	const gchar      *mode
){
	HkMode *old = NULL;
	HkMode *md = NULL;

	if (self->set != NULL && self->mode != NULL)
		old = g_hash_table_lookup(self->set->modes, self->mode);
	if (self->set != NULL && g_strcmp0(mode, "default") != 0)
		md = g_hash_table_lookup(self->set->modes, mode);
	if (self->mode_timer != NULL)
		wl_event_source_timer_update(self->mode_timer, 0);
	if (old != NULL && old->on_exit != NULL && old->on_exit->len > 0)
		hk_run_actions(self, old->on_exit, old->name, "mode", "exit", NULL,
		               FALSE);
	g_free(self->mode);
	self->mode = g_strcmp0(mode, "default") != 0 ? g_strdup(mode) : NULL;
	if (md != NULL) {
		if (md->toast) {
			g_autofree gchar *t = g_strdup_printf("%s", md->label != NULL
			                                      ? md->label : md->name);
			gboolean was = self->toast;

			self->toast = TRUE;
			hk_announce(self, t, NULL);
			self->toast = was;
		}
		if (md->on_enter != NULL && md->on_enter->len > 0)
			hk_run_actions(self, md->on_enter, md->name, "mode", "enter", NULL,
			               FALSE);
		if (md->timeout_ms > 0 && self->compositor != NULL) {
			if (self->mode_timer == NULL)
				self->mode_timer = wl_event_loop_add_timer(
					gowl_compositor_get_event_loop(self->compositor),
					mode_timeout, self);
			wl_event_source_timer_update(self->mode_timer,
			                             (gint)md->timeout_ms);
		}
	}
	hk_announce(self, NULL, "mode %s", mode);
	if (self->dbus_service != NULL)
		hk_dbus_emit(self->dbus_service, "ModeChanged", mode, "");
}

/* ── Input hooks ─────────────────────────────────────────────────────── */

static gint
input_timeout(gpointer data)
{
	hk_input_stop(data, "timeout");
	return 0;
}

static void
input_input_free(gpointer p)
{
	g_free(p);
}

/*
 * Starts AutoHotkey's InputHook: @options is `L5' (length), `T2000'
 * (milliseconds), `V' (keys still reach the window), `C' (the match list
 * is case-sensitive); @end_and_match is end keys, `--', then the match
 * list.  It ends at an end key (`endkey:NAME'), the length (`max'), a
 * match (`match'), the timeout, a click (`mouse') or hotkey-input-stop.
 */
void
hk_input_start(
	GowlModuleHotkey    *self,
	const gchar         *name,
	const gchar         *options,
	const gchar * const *end_and_match,
	HkRunner            *runner
){
	HkInputHook *in = &self->input;
	g_auto(GStrv) opts = NULL;
	GPtrArray *match;
	gboolean after = FALSE;
	guint timeout = 0;
	guint i;

	in->active = TRUE;
	g_free(in->name);
	in->name = g_strdup(name);
	if (in->text != NULL)
		g_string_free(in->text, TRUE);
	in->text = g_string_new(NULL);
	in->max_len = 0;
	in->visible = FALSE;
	in->case_sensitive = FALSE;
	in->runner = runner;
	if (in->end_keys != NULL)
		g_ptr_array_unref(in->end_keys);
	in->end_keys = g_ptr_array_new_with_free_func(input_input_free);
	if (in->swallowed == NULL)
		in->swallowed = g_hash_table_new(g_direct_hash, g_direct_equal);
	opts = g_strsplit(options != NULL ? options : "", " ", -1);
	for (i = 0; opts[i] != NULL; i++) {
		const gchar *o = opts[i];

		if (o[0] == 'L' && g_ascii_isdigit(o[1]))
			in->max_len = (guint)g_ascii_strtoull(o + 1, NULL, 10);
		else if (o[0] == 'T' && g_ascii_isdigit(o[1]))
			timeout = (guint)g_ascii_strtoull(o + 1, NULL, 10);
		else if (g_strcmp0(o, "V") == 0)
			in->visible = TRUE;
		else if (g_strcmp0(o, "C") == 0)
			in->case_sensitive = TRUE;
	}
	match = g_ptr_array_new();
	for (i = 0; end_and_match != NULL && end_and_match[i] != NULL; i++) {
		if (!after && g_strcmp0(end_and_match[i], "--") == 0) {
			after = TRUE;
			continue;
		}
		if (after) {
			g_ptr_array_add(match, g_strdup(end_and_match[i]));
		} else {
			GowlHotkeyInput *k = g_new0(GowlHotkeyInput, 1);

			if (gowl_hotkey_input_parse(end_and_match[i], k, NULL))
				g_ptr_array_add(in->end_keys, k);
			else
				g_free(k);
		}
	}
	g_ptr_array_add(match, NULL);
	g_strfreev(in->match);
	in->match = (gchar **)g_ptr_array_free(match, FALSE);
	if (timeout > 0 && self->compositor != NULL) {
		if (in->timer == NULL)
			in->timer = wl_event_loop_add_timer(
				gowl_compositor_get_event_loop(self->compositor),
				input_timeout, self);
		wl_event_source_timer_update(in->timer, (gint)timeout);
	}
	hk_announce(self, NULL, "input %s started", name);
}

/* A key while an input hook runs.  Returns TRUE to swallow it. */
gboolean
hk_input_key(
	GowlModuleHotkey *self,
	guint32           keycode,
	guint32           keysym,
	guint32           base_keysym,
	guint             mods,
	gboolean          pressed
){
	HkInputHook *in = &self->input;
	guint32 cp;
	guint i;

	if (!pressed)
		return g_hash_table_remove(in->swallowed, GUINT_TO_POINTER(keycode));
	for (i = 0; i < in->end_keys->len; i++) {
		GowlHotkeyInput *k = g_ptr_array_index(in->end_keys, i);

		if (gowl_hotkey_input_matches_key(k, keycode, keysym, base_keysym)) {
			g_autofree gchar *name = gowl_hotkey_input_to_string(k);
			g_autofree gchar *reason = g_strdup_printf("endkey:%s", name);

			if (!in->visible)
				g_hash_table_add(in->swallowed, GUINT_TO_POINTER(keycode));
			hk_input_stop(self, reason);
			return !in->visible;
		}
	}
	/* Modifiers alone and chords are not text */
	if (gowl_hotkey_keysym_modifier(base_keysym) != 0)
		return FALSE;
	if (keysym == XKB_KEY_BackSpace) {
		if (in->text->len > 0) {
			gchar *last = g_utf8_find_prev_char(in->text->str,
			                                    in->text->str + in->text->len);

			if (last != NULL)
				g_string_truncate(in->text, (gsize)(last - in->text->str));
		}
	} else if ((mods & (GOWL_KEY_MOD_CTRL | GOWL_KEY_MOD_ALT
	                    | GOWL_KEY_MOD_LOGO)) == 0
	           && (cp = xkb_keysym_to_utf32(keysym)) >= 0x20 && cp != 0x7f) {
		g_string_append_unichar(in->text, (gunichar)cp);
	} else {
		return FALSE;
	}
	if (!in->visible)
		g_hash_table_add(in->swallowed, GUINT_TO_POINTER(keycode));
	if (in->max_len > 0 && g_utf8_strlen(in->text->str, -1) >= in->max_len) {
		hk_input_stop(self, "max");
		return !in->visible;
	}
	for (i = 0; in->match != NULL && in->match[i] != NULL; i++) {
		gboolean eq = in->case_sensitive
		              ? g_strcmp0(in->match[i], in->text->str) == 0
		              : g_utf8_collate(in->match[i], in->text->str) == 0
		                || g_ascii_strcasecmp(in->match[i], in->text->str) == 0;

		if (eq) {
			hk_input_stop(self, "match");
			break;
		}
	}
	return !in->visible;
}

/* Ends the input hook, announcing what was typed and why it ended. */
void
hk_input_stop(
	GowlModuleHotkey *self,
	const gchar      *reason
){
	HkInputHook *in = &self->input;
	HkRunner *r;
	g_autofree gchar *text = NULL;

	if (!in->active)
		return;
	in->active = FALSE;
	if (in->timer != NULL)
		wl_event_source_timer_update(in->timer, 0);
	text = g_strdup(in->text != NULL ? in->text->str : "");
	hk_announce(self, NULL, "input %s %s %s", in->name, reason, text);
	if (self->dbus_service != NULL)
		hk_dbus_emit(self->dbus_service, "InputEnded", reason, text);
	r = g_steal_pointer(&in->runner);
	if (r != NULL) {
		g_free(r->input);
		r->input = g_steal_pointer(&text);
		runner_resume(r);
	}
}

/* ── Hotstrings' X actions, through gabbrev ──────────────────────────── */

/* gabbrev resolves a `hotkey-action' variable: run the hotstring's
 * actions and type nothing. */
void
hk_gabbrev_var(
	gpointer request,
	gpointer user_data
){
	GowlModuleHotkey *self = user_data;
	GowlGabbrevRequest *req = request;
	const gchar *id = gowl_gabbrev_request_get_param(req, "id");
	guint i;

	for (i = 0; self->set != NULL && i < self->set->hotstrings->len; i++) {
		HkHotstring *hs = g_ptr_array_index(self->set->hotstrings, i);

		if (g_strcmp0(hs->id, id) != 0 || hs->actions == NULL)
			continue;
		hk_run_actions(self, hs->actions, hs->id, "hotstring", hs->abbrev,
		               NULL, TRUE);
		gowl_gabbrev_request_return(req, "");
		return;
	}
	gowl_gabbrev_request_return_error(req, "no such hotstring");
}

static void
gabbrev_var(
	GowlGabbrevRequest *request,
	gpointer            user_data
){
	hk_gabbrev_var(request, user_data);
}

/* ── Posting from worker threads ─────────────────────────────────────── */

typedef struct {
	void          (*func) (GowlModuleHotkey *self, gpointer data);
	gpointer        data;
	GDestroyNotify  destroy;
} Posted;

/* Runs @func on the compositor thread, from any thread.  With the loop
 * gone it runs nothing and frees @data. */
void
hk_post(
	GowlModuleHotkey *self,
	void            (*func) (GowlModuleHotkey *self, gpointer data),
	gpointer          data,
	GDestroyNotify    destroy
){
	Posted *p = g_new0(Posted, 1);
	guint64 one = 1;
	gboolean live;

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
	if (write(self->post_fd, &one, sizeof one) < 0 && errno != EAGAIN)
		g_warning("hotkey: cannot wake the event loop: %s", g_strerror(errno));
}

static int
on_post_fd(
	int       fd,
	uint32_t  mask,
	void     *data
){
	GowlModuleHotkey *self = data;
	guint64 n;

	(void)mask;
	while (read(fd, &n, sizeof n) > 0)
		;
	for (;;) {
		Posted *p;

		g_mutex_lock(&self->post_lock);
		p = g_queue_pop_head(self->posted);
		g_mutex_unlock(&self->post_lock);
		if (p == NULL)
			break;
		p->func(self, p->data);
		if (p->destroy != NULL && p->data != NULL)
			p->destroy(p->data);
		g_free(p);
	}
	return 0;
}

void
hk_post_open(GowlModuleHotkey *self)
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
		g_warning("hotkey: eventfd: %s", g_strerror(errno));
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

void
hk_post_close(GowlModuleHotkey *self)
{
	gint fd;
	Posted *p;

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
	while ((p = g_queue_pop_head(self->posted)) != NULL) {
		if (p->destroy != NULL && p->data != NULL)
			p->destroy(p->data);
		g_free(p);
	}
	g_mutex_unlock(&self->post_lock);
}

/* ── Scripts ─────────────────────────────────────────────────────────── */

static void
script_free(gpointer p)
{
	HkScript *s = p;

	g_free(s->path);
	g_free(s->so_path);
	g_strfreev(s->funcs);
	g_strfreev(s->contexts);
	g_strfreev(s->sources);
	g_free(s->error);
	g_free(s);
}

/* The registry's names before and after a script's init: what is new
 * is the script's, and goes when it is reloaded or dropped. */
static GStrv
source_ids(void)
{
	g_autoptr(GPtrArray) sources = gowl_hotkey_dup_sources();
	GPtrArray *out = g_ptr_array_new();
	guint i;

	for (i = 0; i < sources->len; i++)
		g_ptr_array_add(out, g_strdup(((GowlHotkeySource *)
		                               g_ptr_array_index(sources, i))->id));
	g_ptr_array_add(out, NULL);
	return (GStrv)g_ptr_array_free(out, FALSE);
}

static GStrv
new_names(
	GStrv before,
	GStrv after
){
	GPtrArray *out = g_ptr_array_new();
	guint i;

	for (i = 0; after != NULL && after[i] != NULL; i++)
		if (before == NULL || !g_strv_contains((const gchar * const *)before,
		                                       after[i]))
			g_ptr_array_add(out, g_strdup(after[i]));
	g_ptr_array_add(out, NULL);
	return (GStrv)g_ptr_array_free(out, FALSE);
}

/* Undoes what a script registered, after its fini. */
static void
script_drop(
	GowlModuleHotkey *self,
	HkScript         *s
){
	guint i;

	if (s->fini != NULL) {
		GError *e = NULL;

		if (!hk_c_call_fini(s->fini, self->c_timeout_ms, &e)) {
			hk_fail(self, "%s: %s", s->path, e->message);
			g_error_free(e);
		}
		s->fini = NULL;
	}
	for (i = 0; s->funcs != NULL && s->funcs[i] != NULL; i++)
		gowl_hotkey_unregister_func(s->funcs[i]);
	for (i = 0; s->contexts != NULL && s->contexts[i] != NULL; i++)
		gowl_hotkey_unregister_context(s->contexts[i]);
	for (i = 0; s->sources != NULL && s->sources[i] != NULL; i++)
		gowl_hotkey_remove_yaml(s->sources[i]);
	g_clear_pointer(&s->funcs, g_strfreev);
	g_clear_pointer(&s->contexts, g_strfreev);
	g_clear_pointer(&s->sources, g_strfreev);
}

static void
script_built(
	GowlModuleHotkey *self,
	gpointer          data
){
	CBuild *b = data;
	HkScript *s = g_hash_table_lookup(self->scripts, b->spec.file);
	g_auto(GStrv) f0 = NULL;
	g_auto(GStrv) c0 = NULL;
	g_auto(GStrv) s0 = NULL;
	g_auto(GStrv) f1 = NULL;
	g_auto(GStrv) c1 = NULL;
	g_auto(GStrv) s1 = NULL;
	GModule *module;
	gpointer init = NULL;
	gpointer fini = NULL;
	GError *e = NULL;

	if (s == NULL)
		return;     /* dropped while it built */
	s->building = FALSE;
	g_clear_pointer(&s->error, g_free);
	if (b->so_path == NULL) {
		s->error = g_strdup(b->error != NULL ? b->error->message
		                                     : "did not build");
		hk_fail(self, "script %s: %s", s->path, s->error);
		return;
	}
	if (g_strcmp0(b->so_path, s->so_path) == 0 && s->funcs != NULL)
		return;     /* unchanged and loaded */
	module = hk_c_open(b->so_path, &e);
	if (module == NULL) {
		s->error = g_strdup(e->message);
		hk_fail(self, "script %s: %s", s->path, e->message);
		g_error_free(e);
		return;
	}
	if (!g_module_symbol(module, HK_C_INIT_SYMBOL, &init) || init == NULL) {
		s->error = g_strdup("it does not export " HK_C_INIT_SYMBOL "()");
		hk_fail(self, "script %s: %s", s->path, s->error);
		return;
	}
	g_module_symbol(module, HK_C_FINI_SYMBOL, &fini);
	/* The old build's registrations go first */
	script_drop(self, s);
	f0 = gowl_hotkey_list_funcs();
	c0 = gowl_hotkey_list_contexts();
	s0 = source_ids();
	if (!hk_c_call_init((HkCInitFunc)init, self->c_timeout_ms, &e)) {
		s->error = g_strdup(e->message);
		hk_fail(self, "script %s: %s", s->path, e->message);
		g_error_free(e);
	}
	f1 = gowl_hotkey_list_funcs();
	c1 = gowl_hotkey_list_contexts();
	s1 = source_ids();
	s->funcs = new_names(f0, f1);
	s->contexts = new_names(c0, c1);
	s->sources = new_names(s0, s1);
	s->fini = (HkCFiniFunc)fini;
	g_free(s->so_path);
	s->so_path = g_strdup(b->so_path);
	hk_log(self, "script %s: %u function(s), %u context(s), %u source(s)",
	       s->path, g_strv_length(s->funcs), g_strv_length(s->contexts),
	       g_strv_length(s->sources));
	hk_announce(self, NULL, "script %s loaded", s->path);
}

static void
script_build(
	GowlModuleHotkey *self,
	HkScript         *s
){
	CBuild *b;

	if (s->building)
		return;
	s->building = TRUE;
	b = g_new0(CBuild, 1);
	b->self = self;
	b->spec.file = g_strdup(s->path);
	b->key = hk_c_spec_key(&b->spec);
	b->cache_dir = g_strdup(self->cache_dir);
	b->script = TRUE;
	g_thread_unref(g_thread_new("hotkey-script", c_thread, b));
}

/* inotify: a script edited is rebuilt (and its init run again). */
static int
on_watch_fd(
	int       fd,
	uint32_t  mask,
	void     *data
){
	GowlModuleHotkey *self = data;
	gchar buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t len;

	(void)mask;
	while ((len = read(fd, buf, sizeof buf)) > 0) {
		gchar *p;

		for (p = buf; p < buf + len; ) {
			struct inotify_event *ev = (struct inotify_event *)p;
			GHashTableIter it;
			gpointer v;

			p += sizeof(struct inotify_event) + ev->len;
			if (ev->len == 0)
				continue;
			g_hash_table_iter_init(&it, self->scripts);
			while (g_hash_table_iter_next(&it, NULL, &v)) {
				HkScript *s = v;
				g_autofree gchar *base = g_path_get_basename(s->path);

				if (g_strcmp0(base, ev->name) == 0)
					script_build(self, s);
			}
		}
	}
	return 0;
}

static void
watch_dir(
	GowlModuleHotkey *self,
	const gchar      *dir
){
	gint wd;

	if (self->compositor == NULL || g_hash_table_contains(self->watch_dirs, dir))
		return;
	if (self->watch_fd < 0) {
		self->watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (self->watch_fd < 0)
			return;
		self->watch_source = wl_event_loop_add_fd(
			gowl_compositor_get_event_loop(self->compositor),
			self->watch_fd, WL_EVENT_READABLE, on_watch_fd, self);
	}
	wd = inotify_add_watch(self->watch_fd, dir,
	                       IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
	if (wd >= 0)
		g_hash_table_insert(self->watch_dirs, g_strdup(dir),
		                    GINT_TO_POINTER(wd));
}

/* Loads the scripts the configuration names (`scripts:' in the section
 * and the module setting), drops the ones it no longer names. */
void
hk_scripts_sync(GowlModuleHotkey *self)
{
	g_autoptr(GPtrArray) want = g_ptr_array_new_with_free_func(g_free);
	GHashTableIter it;
	gpointer k;
	gpointer v;
	guint i;

	if (self->compositor == NULL)
		return;
	for (i = 0; self->set != NULL && i < self->set->scripts->len; i++)
		g_ptr_array_add(want, g_strdup(g_ptr_array_index(self->set->scripts,
		                                                 i)));
	for (i = 0; self->setting_scripts != NULL
	            && self->setting_scripts[i] != NULL; i++) {
		const gchar *p = self->setting_scripts[i];

		if (*p == '\0')
			continue;
		g_ptr_array_add(want, g_str_has_prefix(p, "~/")
			? g_build_filename(g_get_home_dir(), p + 2, NULL) : g_strdup(p));
	}
	g_hash_table_iter_init(&it, self->scripts);
	while (g_hash_table_iter_next(&it, &k, &v)) {
		gboolean keep = FALSE;

		for (i = 0; i < want->len; i++)
			if (g_strcmp0(g_ptr_array_index(want, i), k) == 0)
				keep = TRUE;
		if (!keep) {
			script_drop(self, v);
			script_free(v);
			g_hash_table_iter_remove(&it);
		}
	}
	for (i = 0; i < want->len; i++) {
		const gchar *path = g_ptr_array_index(want, i);
		HkScript *s = g_hash_table_lookup(self->scripts, path);

		if (s == NULL) {
			g_autofree gchar *dir = g_path_get_dirname(path);

			s = g_new0(HkScript, 1);
			s->path = g_strdup(path);
			g_hash_table_insert(self->scripts, g_strdup(path), s);
			watch_dir(self, dir);
			script_build(self, s);
		}
	}
}

void
hk_scripts_unload(GowlModuleHotkey *self)
{
	GHashTableIter it;
	gpointer v;

	g_hash_table_iter_init(&it, self->scripts);
	while (g_hash_table_iter_next(&it, NULL, &v)) {
		script_drop(self, v);
		script_free(v);
		g_hash_table_iter_remove(&it);
	}
}

/* ── Setup ───────────────────────────────────────────────────────────── */

void
hk_runner_init(GowlModuleHotkey *self)
{
	if (self->macro_listener == 0)
		self->macro_listener = gowl_macro_add_finished_listener(
			macro_finished, self, NULL);
	gowl_gabbrev_register_var_type(HK_GABBREV_VAR_TYPE, gabbrev_var, self,
	                               NULL);
}

void
hk_runner_finish(GowlModuleHotkey *self)
{
	if (self->macro_listener != 0) {
		gowl_macro_remove_finished_listener(self->macro_listener);
		self->macro_listener = 0;
		gowl_gabbrev_unregister_var_type(HK_GABBREV_VAR_TYPE);
	}
	if (self->play_timer != NULL) {
		wl_event_source_remove(self->play_timer);
		self->play_timer = NULL;
	}
	if (self->mode_timer != NULL) {
		wl_event_source_remove(self->mode_timer);
		self->mode_timer = NULL;
	}
	if (self->input.timer != NULL) {
		wl_event_source_remove(self->input.timer);
		self->input.timer = NULL;
	}
	if (self->fire_idle != NULL) {
		wl_event_source_remove(self->fire_idle);
		self->fire_idle = NULL;
	}
	if (self->watch_source != NULL) {
		wl_event_source_remove(self->watch_source);
		self->watch_source = NULL;
	}
	if (self->watch_fd >= 0) {
		close(self->watch_fd);
		self->watch_fd = -1;
	}
	g_hash_table_remove_all(self->watch_dirs);
}
