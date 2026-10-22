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
 * portal-chooser.c - one line out of a list, chosen by a human.
 *
 * Inside gowl the list goes on the menu card the session already uses
 * for everything else (Super+space), so a screen-share prompt looks
 * like the rest of the desktop and is driven by the same keys.  The
 * card is asked for with the `menu-choose' text command; its answer
 * comes back later as `EVENT menu chosen TOKEN INDEX' (or `cancelled'),
 * on a connection that subscribed to `menu' events BEFORE asking, so
 * the answer cannot slip past between the two.
 *
 * Outside gowl -- or with the menu module not loaded -- the external
 * dmenu-style programs are what is left, exactly as before.
 *
 * Nothing here blocks.  The card's answer is read with
 * portal_gowl_read_async(), a dmenu's with
 * g_subprocess_communicate_utf8_async(); the only synchronous calls
 * are the three short requests that put the card up (connect,
 * subscribe, menu-choose), which the compositor answers in
 * microseconds.  The queue is module state: one pick ACTIVE, the rest
 * waiting their turn in arrival order.
 */

#include "portal-chooser.h"
#include "portal-gowl.h"

#include <string.h>

/* How long a human gets by default.  A chooser nobody answers is
 * withdrawn, and the request reads as cancelled. */
#define CHOOSER_TIMEOUT_S (300)

typedef struct {
	gchar               *prompt;
	gchar              **lines;
	PortalChooserResult  result;
	guint                index;
	gboolean             done;

	GCancellable        *cancellable;  /* the caller's, or NULL */
	gulong               cancel_id;
	GCancellable        *io;           /* ours: the active backend's I/O */
	guint                timeout_id;

	/* The menu card */
	PortalGowl          *gowl;
	gchar               *token;

	/* A dmenu */
	GSubprocess         *proc;
} Pick;

static GQueue  waiting = G_QUEUE_INIT;   /* GTask*, each holding a ref */
static GTask  *active;                   /* holds a ref, or NULL */
static guint   next_idle;

static void start_next_later(void);

static void
pick_free(gpointer data)
{
	Pick *p = data;

	g_free(p->prompt);
	g_strfreev(p->lines);
	g_clear_object(&p->cancellable);
	g_clear_object(&p->io);
	g_clear_pointer(&p->gowl, portal_gowl_free);
	g_free(p->token);
	g_clear_object(&p->proc);
	g_free(p);
}

/* The line as a chooser can show it: no tabs, no newlines. */
static gchar *
clean_line(const gchar *line)
{
	gchar *out = g_strdup(line != NULL ? line : "");

	g_strdelimit(out, "\t\n\r", ' ');
	return out;
}

static guint
chooser_timeout_s(void)
{
	const gchar *env = g_getenv("GOWL_PORTAL_CHOOSER_TIMEOUT");
	guint64 s = 0;

	if (env != NULL
	    && g_ascii_string_to_unsigned(env, 10, 1, 86400, &s, NULL))
		return (guint)s;
	return CHOOSER_TIMEOUT_S;
}

/*
 * Takes the card down if it still shows OUR choice -- never somebody
 * else's that replaced it.  On a connection of its own: the pick's own
 * has a read outstanding, and a stream allows one at a time.
 */
static void
withdraw_card(const gchar *token)
{
	g_autoptr(PortalGowl) gowl = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *want = NULL;

	if (token == NULL)
		return;
	gowl = portal_gowl_connect(NULL);
	if (gowl == NULL)
		return;
	want = g_strdup_printf("menu-choice %s", token);
	state = portal_gowl_command(gowl, want, NULL);
	if (state != NULL && g_str_has_suffix(state, " pending"))
		g_free(portal_gowl_command(gowl, "menu-close", NULL));
}

/*
 * The one way a pick ends.  Safe to call more than once and from any
 * of its callbacks: only the first counts.  Whatever is still running
 * for it is stopped -- a card withdrawn, a dmenu killed, a read
 * cancelled -- and the next pick in the queue is started.
 */
static void
finish(GTask *task, PortalChooserResult result, guint index)
{
	Pick *p = g_task_get_task_data(task);

	if (p->done)
		return;
	p->done = TRUE;
	p->result = result;
	p->index = index;

	if (p->timeout_id != 0) {
		g_source_remove(p->timeout_id);
		p->timeout_id = 0;
	}
	if (p->cancel_id != 0) {
		g_signal_handler_disconnect(p->cancellable, p->cancel_id);
		p->cancel_id = 0;
	}
	if (result != PORTAL_CHOOSER_CHOSEN && p->token != NULL
	    && result != PORTAL_CHOOSER_UNAVAILABLE)
		withdraw_card(p->token);
	/* The dmenu this pick started, by its own handle: nothing else. */
	if (p->proc != NULL && result != PORTAL_CHOOSER_CHOSEN)
		g_subprocess_force_exit(p->proc);
	g_cancellable_cancel(p->io);

	if (task == active) {
		active = NULL;
		start_next_later();
	} else {
		g_queue_remove(&waiting, task);
	}
	g_task_return_boolean(task, TRUE);
	/* The ref the queue (or the active slot) held. */
	g_object_unref(task);
}

static void
on_cancelled(GCancellable *cancellable, gpointer data)
{
	(void)cancellable;
	g_debug("chooser: a pick was withdrawn");
	finish(G_TASK(data), PORTAL_CHOOSER_CANCELLED, 0);
}

static gboolean
on_timeout(gpointer data)
{
	GTask *task = data;
	Pick *p = g_task_get_task_data(task);

	p->timeout_id = 0;
	g_message("chooser: nobody answered in %u s; withdrawing it",
	          chooser_timeout_s());
	finish(task, PORTAL_CHOOSER_CANCELLED, 0);
	return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------
 * gowl's menu card
 * --------------------------------------------------------------- */

static void menu_wait(GTask *task);

static void
menu_read_done(GObject *source, GAsyncResult *res, gpointer data)
{
	g_autoptr(GTask) task = data;
	Pick *p = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	gboolean ok;

	(void)source;
	ok = portal_gowl_read_finish(p->gowl, res, &error);
	if (p->done)
		return;
	if (!ok) {
		g_message("chooser: lost the menu card (%s)", error->message);
		finish(task, PORTAL_CHOOSER_CANCELLED, 0);
		return;
	}
	menu_wait(task);
}

/* Reads events until ours: `chosen TOKEN N' or `cancelled TOKEN'. */
static void
menu_wait(GTask *task)
{
	Pick *p = g_task_get_task_data(task);
	g_autofree gchar *chosen = g_strdup_printf("EVENT menu chosen %s ",
	                                           p->token);
	g_autofree gchar *cancelled = g_strdup_printf("EVENT menu cancelled %s",
	                                              p->token);
	gchar *event;

	while ((event = portal_gowl_take_event_text(p->gowl, "EVENT menu "))
	       != NULL) {
		g_autofree gchar *e = event;

		g_strchomp(e);
		if (g_str_has_prefix(e, chosen)) {
			guint64 n = 0;

			/* Never trusted to be in range: it indexes OUR array,
			 * and the socket is not us. */
			if (!g_ascii_string_to_unsigned(e + strlen(chosen), 10, 0,
			                                G_MAXUINT, &n, NULL)
			    || n >= g_strv_length(p->lines)) {
				finish(task, PORTAL_CHOOSER_CANCELLED, 0);
				return;
			}
			/* Ours is answered: nothing to withdraw. */
			g_clear_pointer(&p->token, g_free);
			finish(task, PORTAL_CHOOSER_CHOSEN, (guint)n);
			return;
		}
		if (g_strcmp0(e, cancelled) == 0) {
			g_clear_pointer(&p->token, g_free);
			finish(task, PORTAL_CHOOSER_CANCELLED, 0);
			return;
		}
		/* Somebody else's choice: keep waiting for ours. */
	}
	portal_gowl_read_async(p->gowl, p->io, menu_read_done,
	                       g_object_ref(task));
}

/* Puts the card up.  FALSE: there is no card to use. */
static gboolean
menu_start(GTask *task)
{
	Pick *p = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) cmd = g_string_new("menu-choose ");
	g_autoptr(JsonNode) sub = NULL;
	g_autofree gchar *reply = NULL;
	JsonObject *params;
	JsonArray *names;
	guint i;

	p->gowl = portal_gowl_connect(&error);
	if (p->gowl == NULL) {
		g_debug("chooser: no gowl menu: %s", error->message);
		return FALSE;
	}

	/* Subscribed first, so the answer cannot arrive unheard. */
	params = json_object_new();
	names = json_array_new();
	json_array_add_string_element(names, "menu");
	json_object_set_array_member(params, "events", names);
	sub = portal_gowl_call(p->gowl, "events.subscribe", params, &error);
	if (sub == NULL) {
		g_debug("chooser: cannot subscribe: %s", error->message);
		g_clear_pointer(&p->gowl, portal_gowl_free);
		return FALSE;
	}

	{
		g_autofree gchar *prompt = clean_line(p->prompt);

		g_string_append(cmd, prompt);
	}
	for (i = 0; p->lines[i] != NULL; i++) {
		g_autofree gchar *l = clean_line(p->lines[i]);

		g_string_append_c(cmd, '\t');
		g_string_append(cmd, l);
	}
	reply = portal_gowl_command(p->gowl, cmd->str, &error);
	/* `unknown command' is the menu module not being loaded, which is
	 * a configuration and not a failure. */
	if (reply == NULL || !g_str_has_prefix(reply, "OK ")) {
		g_debug("chooser: gowl menu declined: %s",
		        error != NULL ? error->message : reply);
		g_clear_pointer(&p->gowl, portal_gowl_free);
		return FALSE;
	}
	p->token = g_strstrip(g_strdup(reply + 3));
	p->timeout_id = g_timeout_add_seconds(chooser_timeout_s(), on_timeout,
	                                      task);
	menu_wait(task);
	return TRUE;
}

/* ---------------------------------------------------------------
 * dmenu-style programs
 * --------------------------------------------------------------- */

/* The menu program to pipe the list through, or NULL. */
static gchar *
dmenu_command(const gchar *prompt)
{
	const gchar *candidates[] = {
		"bemenu", "fuzzel", "wofi", "rofi", "dmenu", NULL
	};
	g_autofree gchar *quoted = g_shell_quote(prompt);
	g_autofree gchar *quoted_sp = NULL;
	gsize i;

	{
		g_autofree gchar *with = g_strdup_printf("%s: ", prompt);

		quoted_sp = g_shell_quote(with);
	}
	for (i = 0; candidates[i] != NULL; i++) {
		g_autofree gchar *path = g_find_program_in_path(candidates[i]);

		if (path == NULL)
			continue;
		/* Each of these reads lines on stdin and prints the chosen one,
		 * given the flag that makes it a picker rather than a runner. */
		if (g_strcmp0(candidates[i], "bemenu") == 0)
			return g_strdup_printf("%s -p %s -l 15", path, quoted);
		if (g_strcmp0(candidates[i], "fuzzel") == 0)
			return g_strdup_printf("%s --dmenu --prompt %s", path,
			                       quoted_sp);
		if (g_strcmp0(candidates[i], "wofi") == 0)
			return g_strdup_printf("%s --dmenu --prompt %s", path,
			                       quoted);
		if (g_strcmp0(candidates[i], "rofi") == 0)
			return g_strdup_printf("%s -dmenu -p %s", path, quoted);
		return g_strdup_printf("%s -p %s", path, quoted);
	}
	return NULL;
}

static void
dmenu_done(GObject *source, GAsyncResult *res, gpointer data)
{
	g_autoptr(GTask) task = data;
	Pick *p = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	guint i;

	if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res,
	                                          &out, NULL, &error)) {
		if (!p->done)
			g_warning("chooser: the menu program failed: %s",
			          error->message);
		finish(task, PORTAL_CHOOSER_UNAVAILABLE, 0);
		return;
	}
	if (p->done)
		return;
	/* An empty answer from a chooser that ran is the human pressing
	 * Escape, which is a cancellation and not an error. */
	if (out == NULL || *out == '\0') {
		finish(task, PORTAL_CHOOSER_CANCELLED, 0);
		return;
	}
	g_strchomp(out);
	for (i = 0; p->lines[i] != NULL; i++) {
		g_autofree gchar *l = clean_line(p->lines[i]);

		if (g_strcmp0(l, out) == 0) {
			finish(task, PORTAL_CHOOSER_CHOSEN, i);
			return;
		}
	}
	/* Typed text that is on no line: rofi and fuzzel allow it.  Not a
	 * choice, and not something to guess at. */
	g_message("chooser: '%s' is not one of the choices", out);
	finish(task, PORTAL_CHOOSER_CANCELLED, 0);
}

static void
dmenu_start(GTask *task, const gchar *cmd)
{
	Pick *p = g_task_get_task_data(task);
	g_auto(GStrv) argv = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) menu = g_string_new(NULL);
	guint i;

	if (!g_shell_parse_argv(cmd, NULL, &argv, &error)) {
		g_warning("chooser: '%s' is unparseable: %s", cmd, error->message);
		finish(task, PORTAL_CHOOSER_UNAVAILABLE, 0);
		return;
	}
	for (i = 0; p->lines[i] != NULL; i++) {
		g_autofree gchar *l = clean_line(p->lines[i]);

		g_string_append_printf(menu, "%s\n", l);
	}
	p->proc = g_subprocess_newv((const gchar * const *)argv,
		G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE,
		&error);
	if (p->proc == NULL) {
		g_warning("chooser: could not run '%s': %s", cmd, error->message);
		finish(task, PORTAL_CHOOSER_UNAVAILABLE, 0);
		return;
	}
	p->timeout_id = g_timeout_add_seconds(chooser_timeout_s(), on_timeout,
	                                      task);
	g_subprocess_communicate_utf8_async(p->proc, menu->str, p->io,
	                                    dmenu_done, g_object_ref(task));
}

/* ---------------------------------------------------------------
 * The queue
 * --------------------------------------------------------------- */

static void
begin(GTask *task)
{
	Pick *p = g_task_get_task_data(task);
	const gchar *env = g_getenv("GOWL_PORTAL_CHOOSER");
	g_autofree gchar *cmd = NULL;

	/* `gowl' names the menu card and nothing else; any other value is
	 * a command, and an explicit one is never second-guessed. */
	if (env != NULL && *env != '\0' && g_strcmp0(env, "gowl") != 0) {
		dmenu_start(task, env);
		return;
	}
	if (menu_start(task))
		return;
	if (g_strcmp0(env, "gowl") == 0) {
		finish(task, PORTAL_CHOOSER_UNAVAILABLE, 0);
		return;
	}
	cmd = dmenu_command(p->prompt);
	if (cmd == NULL) {
		finish(task, PORTAL_CHOOSER_UNAVAILABLE, 0);
		return;
	}
	dmenu_start(task, cmd);
}

static gboolean
start_next(gpointer data)
{
	(void)data;
	next_idle = 0;
	if (active == NULL && !g_queue_is_empty(&waiting)) {
		active = g_queue_pop_head(&waiting);
		g_debug("chooser: showing a pick, %u waiting",
		        g_queue_get_length(&waiting));
		begin(active);
	}
	return G_SOURCE_REMOVE;
}

/* From an idle, never inline: a pick ending inside another's callback
 * must not start the next one on that stack. */
static void
start_next_later(void)
{
	if (next_idle == 0)
		next_idle = g_idle_add(start_next, NULL);
}

void
portal_chooser_run_async(const gchar *prompt, const gchar *const *lines,
                         GCancellable *cancellable,
                         GAsyncReadyCallback callback, gpointer user_data)
{
	GTask *task;
	Pick *p;

	g_return_if_fail(lines != NULL);

	p = g_new0(Pick, 1);
	p->prompt = g_strdup(prompt != NULL ? prompt : "Choose");
	p->lines = g_strdupv((gchar **)lines);
	p->io = g_cancellable_new();
	task = g_task_new(NULL, NULL, callback, user_data);
	g_task_set_task_data(task, p, pick_free);
	g_task_set_source_tag(task, portal_chooser_run_async);

	if (lines[0] == NULL) {
		p->done = TRUE;
		p->result = PORTAL_CHOOSER_UNAVAILABLE;
		g_task_return_boolean(task, TRUE);
		g_object_unref(task);
		return;
	}
	g_queue_push_tail(&waiting, task);
	if (cancellable != NULL) {
		p->cancellable = g_object_ref(cancellable);
		if (g_cancellable_is_cancelled(cancellable)) {
			finish(task, PORTAL_CHOOSER_CANCELLED, 0);
			return;
		}
		p->cancel_id = g_signal_connect(cancellable, "cancelled",
		                                G_CALLBACK(on_cancelled), task);
	}
	start_next_later();
}

PortalChooserResult
portal_chooser_run_finish(GAsyncResult *result, guint *index)
{
	Pick *p;

	g_return_val_if_fail(G_IS_TASK(result), PORTAL_CHOOSER_UNAVAILABLE);
	p = g_task_get_task_data(G_TASK(result));
	if (index != NULL)
		*index = p->index;
	return p->result;
}

guint
portal_chooser_pending(void)
{
	return g_queue_get_length(&waiting) + (active != NULL ? 1u : 0u);
}
