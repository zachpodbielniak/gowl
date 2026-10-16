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
 * The one way a bar widget talks to a language model.
 *
 * Two routes, both out of process (see BarAiBackend for why):
 *
 *   cmacs   `emacsctl eval' asks the editor to start an ASYNC cmacs-ai
 *           stream and returns at once; cmacs pushes the answer back
 *           as `<widget>.ai-*' settings.  Nothing here ever waits on
 *           Lisp -- under --gowl the editor's thread is the
 *           compositor's, and a widget waiting on it is a frozen
 *           desktop.
 *
 *   ai      ai-glib's own command-line front end, on a bar worker
 *           thread, prompt on stdin, answer streamed from stdout.
 *
 * A slot holds one request at a time.  Each request bumps a generation
 * counter, and every write into the slot checks it, so an answer that
 * arrives after the user has moved on is dropped rather than painted
 * over the new one.
 */

#include "bar-ai.h"
#include "bar-internal.h"

#include <errno.h>
#include <gio/gio.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* The longest a CLI request may run before it is killed, unless the
   widget's `ai-timeout' says otherwise.  A local model on a laptop can
   genuinely take minutes. */
#define BAR_AI_DEFAULT_TIMEOUT_S (300)

/* Answers are capped: a model looping on itself should not be allowed
   to grow a widget's memory without bound. */
#define BAR_AI_MAX_TEXT (2 * 1024 * 1024)

struct _BarAi {
	GMutex      lock;
	gchar      *widget;
	guint       generation;   /* bumped per request and per cancel */
	gchar      *request_id;   /* what cmacs must echo back */
	BarAiState  state;
	GString    *text;
	gchar      *error;
	GPid        child;        /* CLI route, 0 when none */
};

BarAi *
bar_ai_new(const gchar *widget)
{
	BarAi *self;

	self = g_new0(BarAi, 1);
	g_mutex_init(&self->lock);
	self->widget = g_strdup((widget != NULL) ? widget : "ai");
	self->text   = g_string_new(NULL);
	self->state  = BAR_AI_IDLE;
	return self;
}

/*
 * Free a slot.  Call it only from the widget's vtable `destroy': a
 * worker holds a reference to the PLUGIN for as long as it runs, so
 * the plugin -- and with it this slot -- cannot be destroyed under it.
 */
void
bar_ai_free(BarAi *self)
{
	if (self == NULL)
		return;
	bar_ai_cancel(self);
	g_mutex_clear(&self->lock);
	g_free(self->widget);
	g_free(self->request_id);
	g_string_free(self->text, TRUE);
	g_free(self->error);
	g_free(self);
}

const gchar *
bar_ai_state_name(BarAiState state)
{
	switch (state) {
	case BAR_AI_IDLE:      return "idle";
	case BAR_AI_WAITING:   return "waiting";
	case BAR_AI_STREAMING: return "streaming";
	case BAR_AI_DONE:      return "done";
	case BAR_AI_ERROR:     return "error";
	default:               return "unknown";
	}
}

static BarAiState
state_from_name(const gchar *name)
{
	if (g_strcmp0(name, "waiting") == 0)
		return BAR_AI_WAITING;
	if (g_strcmp0(name, "streaming") == 0)
		return BAR_AI_STREAMING;
	if (g_strcmp0(name, "done") == 0)
		return BAR_AI_DONE;
	if (g_strcmp0(name, "error") == 0)
		return BAR_AI_ERROR;
	return BAR_AI_IDLE;
}

BarAiBackend
bar_ai_parse_backend(const gchar *spec, gboolean have_cli)
{
	if (spec != NULL) {
		if (g_ascii_strcasecmp(spec, "cmacs") == 0)
			return BAR_AI_BACKEND_CMACS;
		if (g_ascii_strcasecmp(spec, "ai") == 0 ||
		    g_ascii_strcasecmp(spec, "ai-glib") == 0 ||
		    g_ascii_strcasecmp(spec, "cli") == 0)
			return BAR_AI_BACKEND_CLI;
		if (g_ascii_strcasecmp(spec, "none") == 0 ||
		    g_ascii_strcasecmp(spec, "off") == 0)
			return BAR_AI_BACKEND_NONE;
	}
	/* auto, unset, or something unrecognised. */
	return have_cli ? BAR_AI_BACKEND_CLI : BAR_AI_BACKEND_NONE;
}

static const gchar *
cli_command(GowlBarPlugin *plugin)
{
	const gchar *cmd;

	cmd = gowl_bar_plugin_get_setting(plugin, "ai-command");
	return (cmd != NULL && cmd[0] != '\0') ? cmd : "ai";
}

BarAiBackend
bar_ai_backend_for(GowlBarPlugin *plugin)
{
	const gchar *spec;

	spec = gowl_bar_plugin_get_setting(plugin, "ai-backend");
	/* Only probe PATH when the answer depends on it. */
	if (spec != NULL && g_ascii_strcasecmp(spec, "auto") != 0)
		return bar_ai_parse_backend(spec, FALSE);
	return bar_ai_parse_backend(spec,
	                            bar_have_command(cli_command(plugin)));
}

gchar **
bar_ai_cli_argv(const gchar *command, const gchar *provider,
                const gchar *model, const gchar *system)
{
	GPtrArray *argv;

	argv = g_ptr_array_new();
	g_ptr_array_add(argv, g_strdup((command != NULL && command[0] != '\0')
	                               ? command : "ai"));
	g_ptr_array_add(argv, g_strdup("--no-expand"));
	g_ptr_array_add(argv, g_strdup("--stream"));
	if (provider != NULL && provider[0] != '\0') {
		g_ptr_array_add(argv, g_strdup("--provider"));
		g_ptr_array_add(argv, g_strdup(provider));
	}
	if (model != NULL && model[0] != '\0') {
		g_ptr_array_add(argv, g_strdup("--model"));
		g_ptr_array_add(argv, g_strdup(model));
	}
	if (system != NULL && system[0] != '\0') {
		g_ptr_array_add(argv, g_strdup("--system"));
		g_ptr_array_add(argv, g_strdup(system));
	}
	g_ptr_array_add(argv, NULL);
	return (gchar **)g_ptr_array_free(argv, FALSE);
}

/* A string argument for the form, or `nil'. */
static gboolean
append_lisp_string(GString *out, const gchar *text)
{
	g_autofree gchar *quoted = NULL;

	if (text == NULL || text[0] == '\0') {
		g_string_append(out, " nil");
		return TRUE;
	}
	if (!g_utf8_validate(text, -1, NULL))
		return FALSE;
	quoted = bar_elisp_quote(text);
	g_string_append_printf(out, " \"%s\"", quoted);
	return TRUE;
}

gchar *
bar_ai_cmacs_form(const gchar *widget, const gchar *request_id,
                  const gchar *system, const gchar *prompt,
                  const gchar *provider, const gchar *model)
{
	GString *form;

	g_return_val_if_fail(widget != NULL && request_id != NULL, NULL);

	form = g_string_new("(cmacs-gowl-bar-ai-run");
	if (!append_lisp_string(form, widget) ||
	    !append_lisp_string(form, request_id) ||
	    !append_lisp_string(form, system) ||
	    !append_lisp_string(form, (prompt != NULL) ? prompt : "") ||
	    !append_lisp_string(form, provider) ||
	    !append_lisp_string(form, model)) {
		g_string_free(form, TRUE);
		return NULL;
	}
	g_string_append_c(form, ')');
	return g_string_free(form, FALSE);
}

/* ----------------------------------------------------------------
 * Slot updates, all generation-checked
 * ---------------------------------------------------------------- */

static void
slot_fail(BarAi *self, guint generation, const gchar *message)
{
	g_mutex_lock(&self->lock);
	if (self->generation == generation) {
		self->state = BAR_AI_ERROR;
		g_free(self->error);
		self->error = g_strdup(message);
	}
	g_mutex_unlock(&self->lock);
}

static void
slot_append(BarAi *self, guint generation, const gchar *data, gsize len)
{
	g_mutex_lock(&self->lock);
	if (self->generation == generation) {
		if (self->text->len + len <= BAR_AI_MAX_TEXT)
			g_string_append_len(self->text, data, (gssize)len);
		self->state = BAR_AI_STREAMING;
	}
	g_mutex_unlock(&self->lock);
}

static void
slot_finish(BarAi *self, guint generation)
{
	g_mutex_lock(&self->lock);
	if (self->generation == generation) {
		/* Models end on a newline more often than not; a widget
		   showing the answer in one line does not want it. */
		while (self->text->len > 0 &&
		       g_ascii_isspace(self->text->str[self->text->len - 1]))
			g_string_truncate(self->text, self->text->len - 1);
		/* Text that is not UTF-8 cannot be laid out. */
		if (!g_utf8_validate(self->text->str, -1, NULL)) {
			g_autofree gchar *fixed = g_utf8_make_valid(
				self->text->str, (gssize)self->text->len);

			g_string_assign(self->text, fixed);
		}
		self->state = BAR_AI_DONE;
		self->child = 0;
	}
	g_mutex_unlock(&self->lock);
}

/* ----------------------------------------------------------------
 * The CLI route
 * ---------------------------------------------------------------- */

typedef struct {
	BarAi  *ai;           /* outlives the job: see bar_ai_free() */
	guint   generation;
	gchar **argv;
	gchar  *prompt;
	gint    timeout_s;
} CliJob;

static void
cli_job_free(gpointer data)
{
	CliJob *job = data;

	g_strfreev(job->argv);
	g_free(job->prompt);
	g_free(job);
}

static void
set_nonblock(gint fd)
{
	gint flags;

	flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * Run `ai', feeding the prompt on stdin while reading stdout and
 * stderr together.  All three are polled at once: a child that fills
 * its stderr pipe while we are still writing its stdin, or still
 * reading its stdout, would otherwise deadlock against us.
 */
static void
cli_work(GowlBarPlugin *plugin, gpointer user_data)
{
	CliJob *job = user_data;
	GPid pid = 0;
	gint in_fd = -1, out_fd = -1, err_fd = -1;
	GError *error = NULL;
	GString *errbuf;
	gsize written, prompt_len;
	gint64 deadline;
	gint status = 0;
	gboolean killed = FALSE;

	if (!g_spawn_async_with_pipes(NULL, job->argv, NULL,
	                              G_SPAWN_SEARCH_PATH |
	                              G_SPAWN_DO_NOT_REAP_CHILD,
	                              NULL, NULL, &pid, &in_fd, &out_fd,
	                              &err_fd, &error)) {
		g_autofree gchar *msg = g_strdup_printf(
			"could not run %s: %s", job->argv[0], error->message);

		slot_fail(job->ai, job->generation, msg);
		g_clear_error(&error);
		gowl_bar_plugin_request_panel_refresh(plugin);
		return;
	}

	g_mutex_lock(&job->ai->lock);
	if (job->ai->generation == job->generation)
		job->ai->child = pid;
	g_mutex_unlock(&job->ai->lock);

	set_nonblock(in_fd);
	set_nonblock(out_fd);
	set_nonblock(err_fd);

	errbuf     = g_string_new(NULL);
	written    = 0;
	prompt_len = strlen(job->prompt);
	deadline   = g_get_monotonic_time() +
	             (gint64)job->timeout_s * G_USEC_PER_SEC;

	if (prompt_len == 0) {
		close(in_fd);
		in_fd = -1;
	}

	while (out_fd >= 0 || err_fd >= 0) {
		struct pollfd fds[3];
		gint nfds = 0, i, rc;
		gint64 left;

		left = (deadline - g_get_monotonic_time()) / 1000;
		if (left <= 0) {
			kill(pid, SIGTERM);
			killed = TRUE;
			break;
		}

		if (in_fd >= 0) {
			fds[nfds].fd = in_fd;
			fds[nfds].events = POLLOUT;
			nfds++;
		}
		if (out_fd >= 0) {
			fds[nfds].fd = out_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}
		if (err_fd >= 0) {
			fds[nfds].fd = err_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		rc = poll(fds, (nfds_t)nfds, (gint)MIN(left, 500));
		if (rc < 0 && errno != EINTR)
			break;

		/* A superseded request stops as soon as it notices. */
		g_mutex_lock(&job->ai->lock);
		if (job->ai->generation != job->generation) {
			g_mutex_unlock(&job->ai->lock);
			kill(pid, SIGTERM);
			killed = TRUE;
			break;
		}
		g_mutex_unlock(&job->ai->lock);

		for (i = 0; i < nfds && rc > 0; i++) {
			gchar buf[4096];
			gssize n;

			if (fds[i].revents == 0)
				continue;

			if (fds[i].fd == in_fd) {
				n = write(in_fd, job->prompt + written,
				          prompt_len - written);
				if (n > 0)
					written += (gsize)n;
				if ((n < 0 && errno != EAGAIN &&
				     errno != EINTR) ||
				    written >= prompt_len) {
					close(in_fd);
					in_fd = -1;
				}
				continue;
			}

			n = read(fds[i].fd, buf, sizeof(buf));
			if (n > 0) {
				if (fds[i].fd == out_fd) {
					slot_append(job->ai, job->generation,
					            buf, (gsize)n);
					gowl_bar_plugin_request_panel_refresh(
						plugin);
				} else if (errbuf->len < 16384) {
					g_string_append_len(errbuf, buf, n);
				}
				continue;
			}
			if (n < 0 && (errno == EAGAIN || errno == EINTR))
				continue;
			/* EOF or a hard error on this stream. */
			if (fds[i].fd == out_fd) {
				close(out_fd);
				out_fd = -1;
			} else {
				close(err_fd);
				err_fd = -1;
			}
		}
	}

	if (in_fd >= 0)
		close(in_fd);
	if (out_fd >= 0)
		close(out_fd);
	if (err_fd >= 0)
		close(err_fd);

	/* Reaped here, on the worker, synchronously.  Never through a GLib
	   child watch: under cmacs that would fire on Emacs's thread. */
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	g_spawn_close_pid(pid);

	g_mutex_lock(&job->ai->lock);
	if (job->ai->generation == job->generation)
		job->ai->child = 0;
	g_mutex_unlock(&job->ai->lock);

	if (killed) {
		slot_fail(job->ai, job->generation,
		          "the model took too long and was stopped");
	} else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		g_autofree gchar *msg = NULL;
		gchar *last;

		/* The last non-empty line of stderr is usually the reason. */
		g_strstrip(errbuf->str);
		last = strrchr(errbuf->str, '\n');
		msg = g_strdup_printf("%s failed: %s", job->argv[0],
			(errbuf->str[0] == '\0') ? "no output"
			: (last != NULL ? last + 1 : errbuf->str));
		slot_fail(job->ai, job->generation, msg);
	} else {
		slot_finish(job->ai, job->generation);
	}

	g_string_free(errbuf, TRUE);
	gowl_bar_plugin_request_panel_refresh(plugin);
}

/* ----------------------------------------------------------------
 * The cmacs route
 * ---------------------------------------------------------------- */

typedef struct {
	BarAi  *ai;
	guint   generation;
	gchar **argv;
} CmacsJob;

static void
cmacs_job_free(gpointer data)
{
	CmacsJob *job = data;

	g_strfreev(job->argv);
	g_free(job);
}

/*
 * Ask cmacs to start.  `emacsctl eval' returns as soon as the stream
 * has been STARTED -- the Lisp side is asynchronous -- so this blocks a
 * worker for a D-Bus round trip, never for the model.
 */
static void
cmacs_work(GowlBarPlugin *plugin, gpointer user_data)
{
	CmacsJob *job = user_data;
	g_autofree gchar *out = NULL;

	out = bar_run_argv((const gchar * const *)job->argv);
	if (out == NULL) {
		slot_fail(job->ai, job->generation,
		          "could not reach cmacs (is emacsctl on PATH and "
		          "cmacs running?)");
		gowl_bar_plugin_request_panel_refresh(plugin);
	}
}

/* ----------------------------------------------------------------
 * Public
 * ---------------------------------------------------------------- */

gboolean
bar_ai_start(BarAi *self, GowlBarPlugin *plugin, const gchar *system,
             const gchar *prompt, GError **error)
{
	BarAiBackend backend;
	const gchar *provider, *model;
	guint generation;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(GOWL_IS_BAR_PLUGIN(plugin), FALSE);

	if (prompt == NULL || prompt[0] == '\0') {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		                    "nothing to send");
		return FALSE;
	}

	backend = bar_ai_backend_for(plugin);
	if (backend == BAR_AI_BACKEND_NONE) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"no AI backend: install ai-glib's `ai' command, or "
			"set ai-backend: cmacs under cmacs --gowl");
		return FALSE;
	}

	provider = gowl_bar_plugin_get_setting(plugin, "ai-provider");
	model    = gowl_bar_plugin_get_setting(plugin, "ai-model");

	/* Abandon whatever was running, then claim the slot. */
	bar_ai_cancel(self);
	g_mutex_lock(&self->lock);
	generation = ++self->generation;
	g_free(self->request_id);
	self->request_id = g_strdup_printf("%s-%u-%08x", self->widget,
	                                   generation, g_random_int());
	g_string_truncate(self->text, 0);
	g_clear_pointer(&self->error, g_free);
	self->state = BAR_AI_WAITING;
	g_mutex_unlock(&self->lock);

	if (backend == BAR_AI_BACKEND_CLI) {
		CliJob *job;
		gint timeout;

		timeout = gowl_bar_plugin_get_setting_int(plugin, "ai-timeout",
		                                          BAR_AI_DEFAULT_TIMEOUT_S);
		job = g_new0(CliJob, 1);
		job->ai         = self;
		job->generation = generation;
		job->argv       = bar_ai_cli_argv(cli_command(plugin), provider,
		                                  model, system);
		job->prompt     = g_strdup(prompt);
		job->timeout_s  = (timeout > 0) ? timeout
		                                : BAR_AI_DEFAULT_TIMEOUT_S;
		gowl_bar_plugin_queue_work(plugin, cli_work, job, cli_job_free);
		return TRUE;
	}

	/* cmacs */
	{
		CmacsJob *job;
		const gchar *ctl;
		gchar *form;
		gchar *id;

		g_mutex_lock(&self->lock);
		id = g_strdup(self->request_id);
		g_mutex_unlock(&self->lock);

		form = bar_ai_cmacs_form(self->widget, id, system, prompt,
		                         provider, model);
		g_free(id);
		if (form == NULL) {
			slot_fail(self, generation,
			          "the text is not valid UTF-8");
			g_set_error_literal(error, G_IO_ERROR,
			                    G_IO_ERROR_INVALID_DATA,
			                    "the text is not valid UTF-8");
			return FALSE;
		}

		ctl = gowl_bar_plugin_get_setting(plugin, "emacsctl");
		job = g_new0(CmacsJob, 1);
		job->ai         = self;
		job->generation = generation;
		job->argv       = g_new0(gchar *, 4);
		job->argv[0]    = g_strdup((ctl != NULL && ctl[0] != '\0')
		                           ? ctl : "emacsctl");
		job->argv[1]    = g_strdup("eval");
		job->argv[2]    = form;
		gowl_bar_plugin_queue_work(plugin, cmacs_work, job,
		                           cmacs_job_free);
		return TRUE;
	}
}

void
bar_ai_cancel(BarAi *self)
{
	GPid child;

	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->lock);
	child = self->child;
	self->child = 0;
	self->generation++;
	if (self->state == BAR_AI_WAITING || self->state == BAR_AI_STREAMING)
		self->state = BAR_AI_IDLE;
	g_mutex_unlock(&self->lock);

	/* The worker notices the generation and reaps; this only hurries
	   the child along. */
	if (child > 0)
		kill(child, SIGTERM);
}

gboolean
bar_ai_absorb_settings(BarAi *self, GowlBarPlugin *plugin)
{
	const gchar *id, *state, *text, *err;
	gboolean changed = FALSE;

	g_return_val_if_fail(self != NULL, FALSE);

	id = gowl_bar_plugin_get_setting(plugin, "ai-id");
	if (id == NULL)
		return FALSE;

	g_mutex_lock(&self->lock);
	if (self->request_id != NULL && g_strcmp0(id, self->request_id) == 0 &&
	    self->state != BAR_AI_IDLE) {
		BarAiState next;

		state = gowl_bar_plugin_get_setting(plugin, "ai-state");
		text  = gowl_bar_plugin_get_setting(plugin, "ai-text");
		err   = gowl_bar_plugin_get_setting(plugin, "ai-error");
		next  = state_from_name(state);

		if (next != BAR_AI_IDLE && next != self->state) {
			self->state = next;
			changed = TRUE;
		}
		if (text != NULL && g_strcmp0(text, self->text->str) != 0) {
			g_string_assign(self->text, text);
			changed = TRUE;
		}
		if (next == BAR_AI_ERROR &&
		    g_strcmp0(err, self->error) != 0) {
			g_free(self->error);
			self->error = g_strdup((err != NULL) ? err
			                       : "cmacs reported an error");
			changed = TRUE;
		}
	}
	g_mutex_unlock(&self->lock);
	return changed;
}

BarAiState
bar_ai_get(BarAi *self, gchar **text, gchar **error)
{
	BarAiState state;

	g_return_val_if_fail(self != NULL, BAR_AI_IDLE);

	g_mutex_lock(&self->lock);
	state = self->state;
	if (text != NULL)
		*text = g_strdup(self->text->str);
	if (error != NULL)
		*error = g_strdup(self->error);
	g_mutex_unlock(&self->lock);
	return state;
}

gboolean
bar_ai_is_busy(BarAi *self)
{
	BarAiState state;

	state = bar_ai_get(self, NULL, NULL);
	return state == BAR_AI_WAITING || state == BAR_AI_STREAMING;
}

gchar *
bar_ai_get_request_id(BarAi *self)
{
	gchar *id;

	g_return_val_if_fail(self != NULL, NULL);

	g_mutex_lock(&self->lock);
	id = g_strdup(self->request_id);
	g_mutex_unlock(&self->lock);
	return id;
}
