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
 * The `translate' widget: a two-pane translator overlay.
 *
 * A glyph in the bar (optionally the language pair, `EN→RU') and a
 * CENTRED panel --- an overlay opened from a keybind as often as from
 * the widget --- with the source text above and its translation below,
 * language chips to pick and swap the pair, and a filtered language
 * picker.  It translates as you type, once typing pauses.
 *
 * Engines (see bar-translate-model.c for the requests themselves):
 *
 *   ai              the shared bar AI backend: cmacs-ai under
 *                   cmacs --gowl, ai-glib's `ai' standalone
 *   libretranslate  a server you run (POST, JSON body on stdin)
 *   mymemory        the public MyMemory API (GET, URL on stdin)
 *   google          Google's keyless web endpoint; opt-in only
 *
 * Every HTTP request is a `curl' child on a worker thread: bar.so links
 * no HTTP library, and a request on the compositor thread would freeze
 * the desktop for as long as the network liked.  Every request carries
 * a generation number, and a result whose generation is no longer the
 * current one is dropped --- a slow answer for "hel" must not land on
 * top of the answer for "hello".
 *
 * Threads.  Everything in TranslateData is behind `lock'.  Panel build,
 * keys, text and actions run on the compositor thread; requests, the
 * clipboard and the state file run on workers.  Nothing ever calls
 * into the host (queue_work, the AI slot) while holding the lock: the
 * test host runs work inline, and the real one may too one day.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include "gowl-enums.h"
#include "barkit/gowl-bar-host.h"
#include "barkit/gowl-bar-panel.h"

#include "bar-internal.h"
#include "bar-ai.h"
#include "bar-translate-model.h"

/* The registry name, which is also what cmacs addresses its AI replies
   to (`translate.ai-*'). */
#define TR_WIDGET "translate"

/* 󰗊, the Nerd Font translate glyph. */
#define TR_DEFAULT_ICON "\xf3\xb0\x97\x8a"

#define TR_DEFAULT_DEBOUNCE_MS (600)
#define TR_DEFAULT_TIMEOUT_S   (15)
#define TR_IDLE_INTERVAL_S     (30)
#define TR_STATE_FILE          "translate.state"

/* What a clipboard read may bring in.  A screenshot on the clipboard
   asked for as text is refused by wl-paste; this bounds the rest. */
#define TR_MAX_PASTE (BAR_TRANSLATE_MAX_TEXT)

/* The most curl output kept per request: a body several times the
   largest translation is not a translation. */
#define TR_MAX_REPLY (4 * BAR_TRANSLATE_MAX_TEXT)

typedef enum {
	TR_PICK_NONE = 0,
	TR_PICK_SOURCE,
	TR_PICK_TARGET
} TrPicker;

typedef struct {
	GMutex    lock;
	BarAi    *ai;

	/* The pair and the toggle.  Until the user picks a pair (now or
	   in a session the state file remembers), it FOLLOWS the
	   `source'/`target' settings, so editing the config changes it. */
	gchar    *source;
	gchar    *target;
	gboolean  pair_chosen;  /* picked by the user, or from the state */
	gboolean  auto_on;
	gboolean  auto_known;   /* auto_on came from state or the user */
	gboolean  touched;      /* the user changed the pair this session */

	/* The source text, and the revision that makes the host take it
	   back from us (a swap, a paste, a clear). */
	gchar    *src_text;
	guint     src_rev;

	/* The translation shown, and its revision likewise. */
	gchar    *out_text;
	guint     out_rev;

	/* Ctrl+1 / Ctrl+2: which pane asked for the keyboard (1 source,
	   2 translation, 0 none) and the token that makes the host move
	   it (GOWL_BAR_PANEL_PROP_FOCUS). */
	gint      focus_pane;
	guint     focus_seq;
	gchar    *ai_seen;      /* the AI answer last copied into out_text */
	gboolean  ai_active;    /* out_text follows the AI slot */
	gchar    *detected;
	gchar    *error;
	gchar    *note;

	/* Requests. */
	guint     generation;
	gboolean  busy;         /* an HTTP request is in flight */
	gchar    *last_key;     /* what was last sent, to never send it twice */
	gboolean  last_failed;
	gboolean  want_now;     /* a worker asks the next poll to translate */
	gint64    last_edit_us;

	/* The panel. */
	gboolean  panel_open;
	TrPicker  picker;
	gchar    *filter;
	guint     filter_rev;
	gboolean  help;

	/* What is installed, and which engine the settings resolve to,
	   as of the last async poll (both probe PATH, so a worker asks). */
	gboolean  have_curl;
	gboolean  deps_known;
	BarTranslateEngine engine_cfg;

	/* Persistence. */
	gchar    *state_path;
	gboolean  loaded;
	gboolean  dirty;
} TranslateData;

static void tr_start(GowlBarPlugin *plugin, TranslateData *td);
static void tr_apply_label(GowlBarPlugin *plugin, TranslateData *td);

/* ----------------------------------------------------------------
 * Create / destroy
 * ---------------------------------------------------------------- */

static gpointer
tr_create(GowlBarPlugin *plugin)
{
	TranslateData *td;

	(void)plugin;
	td = g_new0(TranslateData, 1);
	g_mutex_init(&td->lock);
	td->ai        = bar_ai_new(TR_WIDGET);
	td->auto_on   = TRUE;
	td->have_curl = TRUE;   /* until a poll says otherwise */
	td->src_text  = g_strdup("");
	td->out_text  = g_strdup("");
	td->filter    = g_strdup("");
	return td;
}

/*
 * Only from the vtable's destroy: a worker holds a reference to the
 * plugin for as long as it runs, so this cannot run under one.
 */
static void
tr_destroy(GowlBarPlugin *plugin, gpointer data)
{
	TranslateData *td = data;

	(void)plugin;

	if (td == NULL)
		return;
	bar_ai_free(td->ai);
	g_free(td->source);
	g_free(td->target);
	g_free(td->src_text);
	g_free(td->out_text);
	g_free(td->ai_seen);
	g_free(td->detected);
	g_free(td->error);
	g_free(td->note);
	g_free(td->last_key);
	g_free(td->filter);
	g_free(td->state_path);
	g_mutex_clear(&td->lock);
	g_free(td);
}

/* ----------------------------------------------------------------
 * Small helpers (call with the lock held unless noted)
 * ---------------------------------------------------------------- */

static void
tr_set_str(gchar **slot, const gchar *value)
{
	if (g_strcmp0(*slot, value) == 0)
		return;
	g_free(*slot);
	*slot = g_strdup(value);
}

/*
 * The pair, read from the settings until the user or the state file
 * says otherwise.  An invalid setting falls back rather than producing
 * a pair every engine refuses.
 */
static void
tr_fill_defaults_locked(GowlBarPlugin *plugin, TranslateData *td)
{
	if (!td->pair_chosen) {
		g_clear_pointer(&td->source, g_free);
		g_clear_pointer(&td->target, g_free);
	}
	if (td->source == NULL) {
		const gchar *s = gowl_bar_plugin_get_setting(plugin, "source");

		if (g_strcmp0(s, "auto") != 0 &&
		    bar_translate_language_find(s) != NULL)
			td->source = g_strdup(bar_translate_language_find(s)->code);
		else
			td->source = g_strdup("auto");
	}
	if (td->target == NULL) {
		const gchar *t = gowl_bar_plugin_get_setting(plugin, "target");

		td->target = g_strdup((bar_translate_language_find(t) != NULL)
			? bar_translate_language_find(t)->code : "en");
	}
	if (!td->auto_known) {
		td->auto_on = gowl_bar_plugin_get_setting_bool(plugin,
			"auto-translate", TRUE);
	}
}

/* Which engine will run, with no lock held: bar_ai_backend_for() may
   probe PATH. */
static BarTranslateEngine
tr_engine(GowlBarPlugin *plugin)
{
	BarTranslateEngine requested;
	gboolean have_ai;

	requested = bar_translate_parse_engine(
		gowl_bar_plugin_get_setting(plugin, "engine"));
	/* Only ask about AI when the answer can matter. */
	have_ai = (requested == BAR_TRANSLATE_ENGINE_AUTO ||
	           requested == BAR_TRANSLATE_ENGINE_AI) &&
	          bar_ai_backend_for(plugin) != BAR_AI_BACKEND_NONE;
	return bar_translate_resolve_engine(requested, have_ai,
		gowl_bar_plugin_get_setting(plugin, "libretranslate-url"));
}

/* What one request is, as a string: never send the same one twice. */
static gchar *
tr_request_key(BarTranslateEngine engine, const gchar *source,
               const gchar *target, const gchar *text)
{
	return g_strdup_printf("%d\x1f%s\x1f%s\x1f%s", (gint)engine,
	                       source, target, text);
}

/* Why an engine resolved to none, for the status line. */
static const gchar *
tr_none_reason(GowlBarPlugin *plugin)
{
	BarTranslateEngine requested;

	requested = bar_translate_parse_engine(
		gowl_bar_plugin_get_setting(plugin, "engine"));
	if (requested == BAR_TRANSLATE_ENGINE_AI)
		return "engine: ai, but there is no AI backend --- install "
		       "ai-glib's `ai', or set translate.ai-backend: cmacs";
	if (requested == BAR_TRANSLATE_ENGINE_LIBRETRANSLATE)
		return "engine: libretranslate needs translate.libretranslate-url "
		       "(e.g. http://localhost:5000)";
	return "translation is off (engine: none)";
}

/* Copy the AI slot's answer into the shown translation, when it has
   moved.  Called from the compositor thread only. */
static gboolean
tr_sync_ai(GowlBarPlugin *plugin, TranslateData *td)
{
	BarAiState state;
	g_autofree gchar *raw = NULL;
	g_autofree gchar *err = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *detected = NULL;
	gboolean changed = FALSE;

	(void)plugin;
	state = bar_ai_get(td->ai, &raw, &err);

	g_mutex_lock(&td->lock);
	if (!td->ai_active) {
		g_mutex_unlock(&td->lock);
		return FALSE;
	}
	text = bar_translate_ai_strip_detected(raw, &detected);
	if (g_strcmp0(text, td->ai_seen) != 0 &&
	    (state == BAR_AI_STREAMING || state == BAR_AI_DONE)) {
		g_autofree gchar *clean = bar_translate_clean_text(text);

		tr_set_str(&td->ai_seen, text);
		tr_set_str(&td->out_text, clean);
		td->out_rev++;
		changed = TRUE;
	}
	if (detected != NULL && g_strcmp0(detected, td->detected) != 0) {
		tr_set_str(&td->detected, detected);
		changed = TRUE;
	}
	if (state == BAR_AI_ERROR && g_strcmp0(err, td->error) != 0) {
		g_autofree gchar *clean = bar_translate_clean_text(
			(err != NULL) ? err : "the model failed");

		tr_set_str(&td->error, clean);
		td->last_failed = TRUE;
		changed = TRUE;
	}
	g_mutex_unlock(&td->lock);
	return changed;
}

/* ----------------------------------------------------------------
 * Running a child on a worker
 * ---------------------------------------------------------------- */

static void
tr_set_nonblock(gint fd)
{
	gint flags;

	flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * One write to a child's stdin that cannot kill the process.
 *
 * A child that exits without reading its input turns our write into a
 * SIGPIPE, whose default action takes the whole compositor with it.
 * gowl ignores SIGPIPE once a clipboard has been served, but not
 * before, and a test binary never does --- so SIGPIPE is blocked on
 * this thread for the write, and one raised by it is consumed before
 * the mask is restored.
 */
static gssize
tr_write_nosigpipe(gint fd, const gchar *buf, gsize len)
{
	sigset_t set, old;
	gssize n;
	gint saved;

	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &set, &old);
	n = write(fd, buf, len);
	saved = errno;
	if (n < 0 && saved == EPIPE) {
		struct timespec zero = { 0, 0 };

		while (sigtimedwait(&set, NULL, &zero) < 0 && errno == EINTR)
			;
	}
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	errno = saved;
	return n;
}

/*
 * Run @argv with @input on stdin.  With @capture, stdout and stderr are
 * read back (bounded); without it they go to /dev/null --- wl-copy
 * forks a server that keeps the clipboard and would hold a pipe open
 * for as long as it lives.
 *
 * All three streams are polled together: a child that fills its stderr
 * while we are still writing its stdin would otherwise deadlock
 * against us.  A request superseded while it runs (its generation
 * moves) is killed early.  Reaped here, synchronously, on the worker;
 * never by a GLib child watch, which under cmacs fires on Emacs's
 * thread.
 *
 * Returns: TRUE when the child ran and exited 0
 */
static gboolean
tr_run(TranslateData *td, guint generation, gchar **argv,
       const gchar *input, gboolean capture, gint timeout_s,
       gchar **out, gchar **err_out)
{
	GPid pid = 0;
	gint in_fd = -1, out_fd = -1, err_fd = -1;
	GError *error = NULL;
	GString *obuf, *ebuf;
	gsize written = 0, input_len;
	gint64 deadline;
	gint status = 0;
	gboolean killed = FALSE, stale = FALSE;
	GSpawnFlags flags;

	if (out != NULL)
		*out = NULL;
	if (err_out != NULL)
		*err_out = NULL;

	flags = G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD;
	if (!capture)
		flags |= G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL;

	if (!g_spawn_async_with_pipes(NULL, argv, NULL, flags, NULL, NULL,
	                              &pid, &in_fd,
	                              capture ? &out_fd : NULL,
	                              capture ? &err_fd : NULL, &error)) {
		if (err_out != NULL)
			*err_out = g_strdup_printf("could not run %s: %s",
			                           argv[0], error->message);
		g_clear_error(&error);
		return FALSE;
	}

	tr_set_nonblock(in_fd);
	if (out_fd >= 0)
		tr_set_nonblock(out_fd);
	if (err_fd >= 0)
		tr_set_nonblock(err_fd);

	obuf = g_string_new(NULL);
	ebuf = g_string_new(NULL);
	input_len = (input != NULL) ? strlen(input) : 0;
	deadline = g_get_monotonic_time() +
	           (gint64)((timeout_s > 0) ? timeout_s : TR_DEFAULT_TIMEOUT_S)
	           * G_USEC_PER_SEC;
	if (input_len == 0) {
		close(in_fd);
		in_fd = -1;
	}

	while (in_fd >= 0 || out_fd >= 0 || err_fd >= 0) {
		struct pollfd fds[3];
		gint nfds = 0, i, rc;
		gint64 left;

		left = (deadline - g_get_monotonic_time()) / 1000;
		if (left <= 0) {
			kill(pid, SIGTERM);
			killed = TRUE;
			break;
		}
		if (generation != 0) {
			g_mutex_lock(&td->lock);
			stale = (td->generation != generation);
			g_mutex_unlock(&td->lock);
			if (stale) {
				kill(pid, SIGTERM);
				break;
			}
		}

		if (in_fd >= 0) {
			fds[nfds].fd = in_fd;
			fds[nfds].events = POLLOUT;
			fds[nfds].revents = 0;
			nfds++;
		}
		if (out_fd >= 0) {
			fds[nfds].fd = out_fd;
			fds[nfds].events = POLLIN;
			fds[nfds].revents = 0;
			nfds++;
		}
		if (err_fd >= 0) {
			fds[nfds].fd = err_fd;
			fds[nfds].events = POLLIN;
			fds[nfds].revents = 0;
			nfds++;
		}

		rc = poll(fds, (nfds_t)nfds, (gint)MIN(left, 250));
		if (rc < 0 && errno != EINTR)
			break;

		for (i = 0; i < nfds && rc > 0; i++) {
			gchar buf[4096];
			gssize n;

			if (fds[i].revents == 0)
				continue;

			if (fds[i].fd == in_fd) {
				n = tr_write_nosigpipe(in_fd, input + written,
				                       input_len - written);
				if (n > 0)
					written += (gsize)n;
				if ((n < 0 && errno != EAGAIN &&
				     errno != EINTR) ||
				    written >= input_len) {
					close(in_fd);
					in_fd = -1;
				}
				continue;
			}

			n = read(fds[i].fd, buf, sizeof(buf));
			if (n > 0) {
				GString *dst = (fds[i].fd == out_fd)
					? obuf : ebuf;
				gsize cap = (fds[i].fd == out_fd)
					? TR_MAX_REPLY : 16384;

				if (dst->len + (gsize)n <= cap)
					g_string_append_len(dst, buf, n);
				continue;
			}
			if (n < 0 && (errno == EAGAIN || errno == EINTR))
				continue;
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
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	g_spawn_close_pid(pid);

	if (killed || stale) {
		if (err_out != NULL)
			*err_out = g_strdup(killed
				? "timed out" : "superseded");
		g_string_free(obuf, TRUE);
		g_string_free(ebuf, TRUE);
		return FALSE;
	}

	if (out != NULL)
		*out = g_string_free(obuf, FALSE);
	else
		g_string_free(obuf, TRUE);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		if (err_out != NULL) {
			gchar *last;

			/* The last line of stderr is usually the reason. */
			g_strstrip(ebuf->str);
			last = strrchr(ebuf->str, '\n');
			*err_out = g_strdup_printf("%s failed: %s", argv[0],
				(ebuf->str[0] == '\0') ? "no output"
				: (last != NULL ? last + 1 : ebuf->str));
		}
		g_string_free(ebuf, TRUE);
		return FALSE;
	}
	g_string_free(ebuf, TRUE);
	return TRUE;
}

/* ----------------------------------------------------------------
 * The HTTP engines, on a worker
 * ---------------------------------------------------------------- */

typedef struct {
	TranslateData     *td;
	guint              generation;
	BarTranslateEngine engine;
	gchar             *text;
	gchar             *source;    /* table code, or auto */
	gchar             *target;
	gchar             *curl;
	gint               timeout_s;
	gchar             *libre_url;
	gchar             *libre_key;  /* secret: stdin only */
	gchar             *email;
} TrJob;

static void
tr_job_free(gpointer data)
{
	TrJob *job = data;

	g_free(job->text);
	g_free(job->source);
	g_free(job->target);
	g_free(job->curl);
	g_free(job->libre_url);
	if (job->libre_key != NULL) {
		memset(job->libre_key, 0, strlen(job->libre_key));
		g_free(job->libre_key);
	}
	g_free(job->email);
	g_free(job);
}

/*
 * Translate one chunk.  Returns the translation, or NULL with @error
 * set.  @detected collects what an auto source turned out to be.
 */
static gchar *
tr_http_one(TrJob *job, const gchar *chunk, gchar **detected,
            gchar **error)
{
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *input = NULL;
	g_autofree gchar *output = NULL;
	g_autofree gchar *run_err = NULL;
	g_autofree gchar *url = NULL;
	const gchar *src, *tgt;
	gchar *text = NULL;
	gint status;
	gboolean ok;

	*error = NULL;
	src = bar_translate_engine_code(job->engine, job->source);
	tgt = bar_translate_engine_code(job->engine, job->target);

	switch (job->engine) {
	case BAR_TRANSLATE_ENGINE_LIBRETRANSLATE:
		url = bar_translate_libre_url(job->libre_url);
		if (url == NULL) {
			*error = g_strdup("libretranslate-url must be an "
			                  "http:// or https:// URL");
			return NULL;
		}
		argv  = bar_translate_curl_argv(job->curl, job->timeout_s, url);
		input = bar_translate_libre_body(chunk, src, tgt,
		                                 job->libre_key);
		break;
	case BAR_TRANSLATE_ENGINE_MYMEMORY:
		argv  = bar_translate_curl_argv(job->curl, job->timeout_s, NULL);
		input = bar_translate_mymemory_config(chunk, src, tgt,
		                                      job->email);
		break;
	case BAR_TRANSLATE_ENGINE_GOOGLE:
		argv  = bar_translate_curl_argv(job->curl, job->timeout_s, NULL);
		input = bar_translate_google_config(chunk, src, tgt);
		break;
	default:
		*error = g_strdup("no engine");
		return NULL;
	}

	/* A little more than curl's own limit, so curl reports the
	   timeout itself and we only kill a curl that is truly stuck. */
	ok = tr_run(job->td, job->generation, argv, input, TRUE,
	            job->timeout_s + 5, &output, &run_err);
	if (input != NULL)
		memset(input, 0, strlen(input));
	if (!ok) {
		/* curl writes its reason on stderr, e.g. "curl: (6) Could
		   not resolve host". */
		*error = g_strdup_printf("%s: %s",
			bar_translate_engine_name(job->engine),
			(run_err != NULL) ? run_err : "request failed");
		return NULL;
	}

	status = bar_translate_split_status(output);
	switch (job->engine) {
	case BAR_TRANSLATE_ENGINE_LIBRETRANSLATE:
		bar_translate_parse_libre(output, status, &text, detected,
		                          error);
		break;
	case BAR_TRANSLATE_ENGINE_MYMEMORY:
		bar_translate_parse_mymemory(output, status, &text, error);
		break;
	case BAR_TRANSLATE_ENGINE_GOOGLE:
		bar_translate_parse_google(output, status, &text, detected,
		                           error);
		break;
	default:
		break;
	}
	return text;
}

static void
tr_http_work(GowlBarPlugin *plugin, gpointer user_data)
{
	TrJob *job = user_data;
	TranslateData *td = job->td;
	g_autoptr(GPtrArray) chunks = NULL;
	g_autoptr(GPtrArray) parts = NULL;
	g_autofree gchar *detected = NULL;
	g_autofree gchar *error = NULL;
	g_autofree gchar *joined = NULL;
	guint i;

	if (!bar_have_command(job->curl)) {
		error = g_strdup_printf("%s is not installed (Fedora package: "
		                        "curl)", job->curl);
		goto done;
	}

	chunks = bar_translate_chunk(job->text,
	                             bar_translate_engine_limit(job->engine));
	parts = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < chunks->len; i++) {
		BarTranslateChunk *c = g_ptr_array_index(chunks, i);
		gchar *one;
		gboolean stale;

		g_mutex_lock(&td->lock);
		stale = (td->generation != job->generation);
		g_mutex_unlock(&td->lock);
		if (stale)
			return;

		if (c->text[0] == '\0') {
			g_ptr_array_add(parts, g_strdup(""));
			continue;
		}
		one = tr_http_one(job, c->text,
		                  (detected == NULL) ? &detected : NULL,
		                  &error);
		if (one == NULL)
			goto done;
		g_ptr_array_add(parts, one);
	}
	g_ptr_array_add(parts, NULL);
	joined = bar_translate_join(chunks,
	                            (const gchar * const *)parts->pdata);

done:
	/* Swapped in only if this is still the request the user wants. */
	g_mutex_lock(&td->lock);
	if (td->generation == job->generation) {
		td->busy = FALSE;
		if (joined != NULL) {
			g_autofree gchar *clean = bar_translate_clean_text(joined);

			tr_set_str(&td->out_text, clean);
			td->out_rev++;
			if (detected != NULL)
				tr_set_str(&td->detected, detected);
			g_clear_pointer(&td->error, g_free);
			td->last_failed = FALSE;
		} else {
			tr_set_str(&td->error, error);
			td->last_failed = TRUE;
		}
	}
	g_mutex_unlock(&td->lock);
	gowl_bar_plugin_request_panel_refresh(plugin);
	gowl_bar_plugin_request_redraw(plugin);
}

/* ----------------------------------------------------------------
 * Starting a translation (compositor thread)
 * ---------------------------------------------------------------- */

/*
 * Start translating the current source text, unless exactly this
 * request --- engine, pair and text --- was already sent and did not
 * fail.  That holds for Ctrl+Return too: pressing it twice on the same
 * text spends one request, and after an error it retries.
 */
static void
tr_start(GowlBarPlugin *plugin, TranslateData *td)
{
	BarTranslateEngine engine;
	g_autofree gchar *text = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *target = NULL;
	g_autofree gchar *key = NULL;
	guint generation;
	gboolean assumed = FALSE;

	engine = tr_engine(plugin);

	g_mutex_lock(&td->lock);
	tr_fill_defaults_locked(plugin, td);
	text   = g_strdup(td->src_text);
	source = g_strdup(td->source);
	target = g_strdup(td->target);
	td->want_now = FALSE;

	/* Nothing to translate: an empty source means an empty result,
	   and any request in flight is now moot. */
	g_strstrip(text);
	if (text[0] == '\0') {
		td->generation++;
		td->busy = FALSE;
		td->ai_active = FALSE;
		g_clear_pointer(&td->last_key, g_free);
		g_clear_pointer(&td->error, g_free);
		g_clear_pointer(&td->note, g_free);
		if (td->out_text[0] != '\0') {
			tr_set_str(&td->out_text, "");
			td->out_rev++;
		}
		g_mutex_unlock(&td->lock);
		bar_ai_cancel(td->ai);
		return;
	}
	g_free(text);
	text = g_strdup(td->src_text);

	key = tr_request_key(engine, source, target, text);
	if (!td->last_failed && g_strcmp0(key, td->last_key) == 0) {
		g_mutex_unlock(&td->lock);
		return;
	}

	generation = ++td->generation;
	tr_set_str(&td->last_key, key);
	td->last_failed = FALSE;
	g_clear_pointer(&td->error, g_free);
	g_clear_pointer(&td->note, g_free);
	if (engine == BAR_TRANSLATE_ENGINE_MYMEMORY) {
		bar_translate_mymemory_source(source, &assumed);
		if (assumed)
			td->note = g_strdup("MyMemory cannot detect the "
				"language: assumed English. Pick a source "
				"language (Ctrl+L).");
	}
	if (g_strcmp0(source, "auto") == 0)
		g_clear_pointer(&td->detected, g_free);
	td->busy = (engine != BAR_TRANSLATE_ENGINE_AI &&
	            engine != BAR_TRANSLATE_ENGINE_NONE);
	td->ai_active = (engine == BAR_TRANSLATE_ENGINE_AI);
	g_clear_pointer(&td->ai_seen, g_free);
	g_mutex_unlock(&td->lock);

	if (engine == BAR_TRANSLATE_ENGINE_NONE) {
		bar_ai_cancel(td->ai);
		g_mutex_lock(&td->lock);
		tr_set_str(&td->error, tr_none_reason(plugin));
		td->last_failed = TRUE;
		g_mutex_unlock(&td->lock);
		return;
	}

	if (engine == BAR_TRANSLATE_ENGINE_AI) {
		g_autofree gchar *system = NULL;
		GError *error = NULL;

		system = bar_translate_ai_system(source, target);
		if (!bar_ai_start(td->ai, plugin, system, text, &error)) {
			g_mutex_lock(&td->lock);
			if (td->generation == generation) {
				tr_set_str(&td->error, error->message);
				td->last_failed = TRUE;
			}
			g_mutex_unlock(&td->lock);
			g_clear_error(&error);
		}
		tr_sync_ai(plugin, td);
		return;
	}

	/* An HTTP engine.  A model still streaming is abandoned. */
	bar_ai_cancel(td->ai);
	{
		TrJob *job;
		const gchar *curl;

		curl = gowl_bar_plugin_get_setting(plugin, "curl");
		job = g_new0(TrJob, 1);
		job->td         = td;
		job->generation = generation;
		job->engine     = engine;
		job->text       = g_steal_pointer(&text);
		job->source     = g_steal_pointer(&source);
		job->target     = g_steal_pointer(&target);
		job->curl       = g_strdup((curl != NULL && curl[0] != '\0')
		                           ? curl : "curl");
		job->timeout_s  = gowl_bar_plugin_get_setting_int(plugin,
			"timeout", TR_DEFAULT_TIMEOUT_S);
		job->libre_url  = g_strdup(gowl_bar_plugin_get_setting(plugin,
			"libretranslate-url"));
		job->libre_key  = g_strdup(gowl_bar_plugin_get_setting(plugin,
			"libretranslate-key"));
		job->email      = g_strdup(gowl_bar_plugin_get_setting(plugin,
			"mymemory-email"));
		gowl_bar_plugin_queue_work(plugin, tr_http_work, job,
		                           tr_job_free);
	}
}

/* ----------------------------------------------------------------
 * The clipboard, on workers
 * ---------------------------------------------------------------- */

typedef struct {
	TranslateData *td;
	gchar         *command;
	gchar         *text;
} TrClipJob;

static void
tr_clip_job_free(gpointer data)
{
	TrClipJob *job = data;

	g_free(job->command);
	g_free(job->text);
	g_free(job);
}

/* The command a clipboard setting names, as an argv.  Parsed with
   shell quoting but never run through a shell. */
static gchar **
tr_command_argv(const gchar *command)
{
	gchar **argv = NULL;

	if (command == NULL || !g_shell_parse_argv(command, NULL, &argv, NULL))
		return NULL;
	return argv;
}

static void
tr_note_locked(TranslateData *td, const gchar *note, gboolean is_error)
{
	if (is_error)
		tr_set_str(&td->error, note);
	else
		tr_set_str(&td->note, note);
}

static void
tr_copy_work(GowlBarPlugin *plugin, gpointer user_data)
{
	TrClipJob *job = user_data;
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *err = NULL;
	gboolean ok;

	argv = tr_command_argv(job->command);
	if (argv == NULL || argv[0] == NULL) {
		ok = FALSE;
		err = g_strdup("copy-command is empty");
	} else if (!bar_have_command(argv[0])) {
		ok = FALSE;
		err = g_strdup_printf("%s is not installed (Fedora package: "
		                      "wl-clipboard)", argv[0]);
	} else {
		/* The text on stdin: a translation of someone's email has
		   no business in `ps'. */
		ok = tr_run(job->td, 0, argv, job->text, FALSE, 10, NULL, &err);
	}

	g_mutex_lock(&job->td->lock);
	tr_note_locked(job->td, ok ? "Translation copied to the clipboard."
	               : err, !ok);
	g_mutex_unlock(&job->td->lock);
	gowl_bar_plugin_request_panel_refresh(plugin);
}

static void
tr_paste_work(GowlBarPlugin *plugin, gpointer user_data)
{
	TrClipJob *job = user_data;
	TranslateData *td = job->td;
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	gboolean ok;

	argv = tr_command_argv(job->command);
	if (argv == NULL || argv[0] == NULL) {
		ok = FALSE;
		err = g_strdup("paste-command is empty");
	} else if (!bar_have_command(argv[0])) {
		ok = FALSE;
		err = g_strdup_printf("%s is not installed (Fedora package: "
		                      "wl-clipboard)", argv[0]);
	} else {
		/* Off the compositor thread on purpose: the clipboard's
		   owner writes the data, and it can only do that while the
		   compositor is free to dispatch. */
		ok = tr_run(td, 0, argv, NULL, TRUE, 10, &out, &err);
	}

	if (ok && (out == NULL || out[0] == '\0')) {
		ok = FALSE;
		g_clear_pointer(&err, g_free);
		err = g_strdup("the clipboard is empty");
	}
	if (ok && (strlen(out) > TR_MAX_PASTE ||
	           !g_utf8_validate(out, -1, NULL))) {
		ok = FALSE;
		g_clear_pointer(&err, g_free);
		err = g_strdup("the clipboard does not hold text "
		               "(or holds too much of it)");
	}

	g_mutex_lock(&td->lock);
	if (ok) {
		tr_set_str(&td->src_text, out);
		td->src_rev++;
		td->want_now = TRUE;
		td->dirty = TRUE;
		g_clear_pointer(&td->error, g_free);
	} else {
		tr_set_str(&td->error, err);
	}
	g_mutex_unlock(&td->lock);
	gowl_bar_plugin_request_panel_refresh(plugin);
}

static void
tr_copy(GowlBarPlugin *plugin, TranslateData *td)
{
	TrClipJob *job;
	const gchar *cmd;

	job = g_new0(TrClipJob, 1);
	job->td = td;
	cmd = gowl_bar_plugin_get_setting(plugin, "copy-command");
	job->command = g_strdup((cmd != NULL) ? cmd : "wl-copy");
	g_mutex_lock(&td->lock);
	job->text = g_strdup(td->out_text);
	g_mutex_unlock(&td->lock);
	if (job->text[0] == '\0') {
		tr_clip_job_free(job);
		g_mutex_lock(&td->lock);
		tr_set_str(&td->note, "Nothing to copy yet.");
		g_mutex_unlock(&td->lock);
		return;
	}
	gowl_bar_plugin_queue_work(plugin, tr_copy_work, job,
	                           tr_clip_job_free);
}

static void
tr_paste(GowlBarPlugin *plugin, TranslateData *td)
{
	TrClipJob *job;
	const gchar *cmd;

	job = g_new0(TrClipJob, 1);
	job->td = td;
	cmd = gowl_bar_plugin_get_setting(plugin, "paste-command");
	job->command = g_strdup((cmd != NULL) ? cmd
		: "wl-paste --no-newline --type text");
	gowl_bar_plugin_queue_work(plugin, tr_paste_work, job,
	                           tr_clip_job_free);
}

/* ----------------------------------------------------------------
 * State file, on workers
 * ---------------------------------------------------------------- */

/* Read once.  What the user did before the read finished wins: the
   file is the PREVIOUS session, the edits are this one. */
static void
tr_state_load_locked_apply(TranslateData *td, BarTranslateState *st)
{
	if (!td->touched && (st->source != NULL || st->target != NULL)) {
		if (st->source != NULL)
			tr_set_str(&td->source, st->source);
		if (st->target != NULL)
			tr_set_str(&td->target, st->target);
		td->pair_chosen = TRUE;
	}
	if (!td->auto_known) {
		td->auto_on = st->auto_translate;
		td->auto_known = TRUE;
	}
	if (st->text != NULL && td->src_text[0] == '\0' &&
	    td->last_edit_us == 0) {
		tr_set_str(&td->src_text, st->text);
		td->src_rev++;
		/* Show it, but do not spend a request on it until asked. */
	}
}

static void
tr_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	TranslateData *td = data;
	g_autofree gchar *path = NULL;
	g_autofree gchar *save = NULL;
	gboolean need_load, have_curl;
	const gchar *curl;
	BarTranslateEngine engine;

	if (td == NULL)
		return;

	curl = gowl_bar_plugin_get_setting(plugin, "curl");
	have_curl = bar_have_command((curl != NULL && curl[0] != '\0')
	                             ? curl : "curl");
	engine = tr_engine(plugin);

	g_mutex_lock(&td->lock);
	td->have_curl  = have_curl;
	td->deps_known = TRUE;
	td->engine_cfg = engine;
	path = g_strdup(td->state_path);
	need_load = !td->loaded && path != NULL;
	if (td->loaded && td->dirty && path != NULL) {
		BarTranslateState st;

		memset(&st, 0, sizeof(st));
		/* The pair is saved only once the user has chosen one: an
		   untouched pair must keep following the settings. */
		st.source = td->pair_chosen ? td->source : NULL;
		st.target = td->pair_chosen ? td->target : NULL;
		st.auto_translate = td->auto_on;
		st.text = td->src_text;
		save = bar_translate_state_serialize(&st,
			gowl_bar_plugin_get_setting_bool(plugin,
				"remember-text", TRUE));
		td->dirty = FALSE;
	}
	g_mutex_unlock(&td->lock);

	if (need_load) {
		g_autofree gchar *contents = NULL;
		gsize len = 0;
		BarTranslateState st;

		/* Bounded: a state file is a few lines and the remembered
		   text; anything far larger is not ours. */
		if (g_file_get_contents(path, &contents, &len, NULL) &&
		    len <= 4 * BAR_TRANSLATE_MAX_STATE_TEXT &&
		    bar_translate_state_parse(contents, &st)) {
			g_mutex_lock(&td->lock);
			tr_state_load_locked_apply(td, &st);
			g_mutex_unlock(&td->lock);
			bar_translate_state_clear(&st);
		}
		g_mutex_lock(&td->lock);
		td->loaded = TRUE;
		g_mutex_unlock(&td->lock);
		gowl_bar_plugin_request_panel_refresh(plugin);
	}

	if (save != NULL) {
		g_autofree gchar *dir = g_path_get_dirname(path);

		/* 0600: the remembered text may be private. */
		g_mkdir_with_parents(dir, 0700);
		if (!g_file_set_contents_full(path, save, -1,
		                              G_FILE_SET_CONTENTS_CONSISTENT,
		                              0600, NULL))
			g_warning("gowl-bar: translate: could not save %s",
			          path);
	}

	/* The label's setters are thread-safe, and the dependency
	   verdict above belongs in the tooltip now, not a poll later. */
	tr_apply_label(plugin, td);
}

/* ----------------------------------------------------------------
 * Poll (compositor thread)
 * ---------------------------------------------------------------- */

static gint
tr_interval(GowlBarPlugin *plugin, gpointer data)
{
	TranslateData *td = data;
	gboolean quick;

	if (td == NULL)
		return TR_IDLE_INTERVAL_S;

	(void)plugin;
	g_mutex_lock(&td->lock);
	/* Once a second while anything is moving (and while the panel is
	   open, which is what bounds the debounce latency); otherwise the
	   widget has nothing to do. */
	quick = td->panel_open || td->busy || td->dirty || td->want_now ||
	        !td->loaded || (td->ai_active && bar_ai_is_busy(td->ai));
	g_mutex_unlock(&td->lock);
	return quick ? 1 : TR_IDLE_INTERVAL_S;
}

static void
tr_apply_label(GowlBarPlugin *plugin, TranslateData *td)
{
	g_autofree gchar *pair = NULL;
	g_autofree gchar *tip = NULL;
	g_autofree gchar *error = NULL;
	const gchar *icon;
	gboolean busy, show_pair, no_curl;
	BarTranslateEngine engine;

	g_mutex_lock(&td->lock);
	pair  = bar_translate_pair_label(td->source, td->target,
	                                 td->detected);
	error = g_strdup(td->error);
	busy  = td->busy || bar_ai_is_busy(td->ai);
	engine = td->engine_cfg;
	no_curl = td->deps_known && !td->have_curl;
	tip = g_strdup_printf("Translate: %s \xe2\x86\x92 %s",
		bar_translate_language_name(td->source),
		bar_translate_language_name(td->target));
	g_mutex_unlock(&td->lock);

	icon = gowl_bar_plugin_get_setting(plugin, "icon");
	gowl_bar_plugin_set_icon(plugin, (icon != NULL && icon[0] != '\0')
	                         ? icon : TR_DEFAULT_ICON);
	show_pair = gowl_bar_plugin_get_setting_bool(plugin, "show-pair",
	                                             TRUE);
	gowl_bar_plugin_set_label(plugin, show_pair ? pair : NULL);

	if (error != NULL) {
		g_autofree gchar *full = g_strdup_printf("%s\n%s", tip, error);

		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-error"),
			GOWL_BAR_COLOR_RED);
		gowl_bar_plugin_set_tooltip(plugin, full);
		return;
	}
	if (no_curl && engine != BAR_TRANSLATE_ENGINE_AI &&
	    engine != BAR_TRANSLATE_ENGINE_NONE) {
		g_autofree gchar *full = g_strdup_printf(
			"%s\nNeeds curl (Fedora package: curl) for the web "
			"engines", tip);

		gowl_bar_plugin_set_tooltip(plugin, full);
	} else {
		gowl_bar_plugin_set_tooltip(plugin, tip);
	}
	bar_plugin_apply_color(plugin,
		gowl_bar_plugin_get_setting(plugin, "color"),
		busy ? GOWL_BAR_COLOR_YELLOW : GOWL_BAR_COLOR_TEXT);
}

static void
tr_poll(GowlBarPlugin *plugin, gpointer data)
{
	TranslateData *td = data;
	gboolean go = FALSE;
	gint debounce;

	if (td == NULL)
		return;

	/* Where the state file lives: asked here, on the compositor
	   thread, and handed to the worker as a plain string. */
	g_mutex_lock(&td->lock);
	if (td->state_path == NULL) {
		GowlBarHost *host = gowl_bar_plugin_get_host(plugin);
		const gchar *dir = (host != NULL)
			? gowl_bar_host_get_state_dir(host) : NULL;

		if (dir != NULL)
			td->state_path = g_build_filename(dir, TR_STATE_FILE,
			                                  NULL);
		else
			td->loaded = TRUE;   /* nowhere to load from */
	}
	tr_fill_defaults_locked(plugin, td);
	g_mutex_unlock(&td->lock);

	if (tr_sync_ai(plugin, td))
		gowl_bar_plugin_request_panel_refresh(plugin);

	debounce = gowl_bar_plugin_get_setting_int(plugin, "debounce-ms",
	                                           TR_DEFAULT_DEBOUNCE_MS);
	g_mutex_lock(&td->lock);
	if (td->want_now) {
		go = TRUE;
	} else {
		g_autofree gchar *key = NULL;

		/* "Changed" is judged against the last request's text and
		   pair; the engine half of the key is re-resolved by
		   tr_start, which dedupes again. */
		key = g_strdup_printf("\x1f%s\x1f%s\x1f%s", td->source,
		                      td->target, td->src_text);
		go = bar_translate_should_auto(td->auto_on, td->last_edit_us,
			g_get_monotonic_time(), debounce,
			td->last_key == NULL ||
			!g_str_has_suffix(td->last_key, key));
		if (go && td->src_text[0] == '\0' && td->out_text[0] == '\0')
			go = FALSE;
	}
	g_mutex_unlock(&td->lock);

	if (go) {
		tr_start(plugin, td);
		gowl_bar_plugin_request_panel_refresh(plugin);
	}
	tr_apply_label(plugin, td);
}

/* ----------------------------------------------------------------
 * Configure (compositor thread)
 * ---------------------------------------------------------------- */

static void
tr_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	TranslateData *td = data;
	BarTranslateEngine engine;

	if (td == NULL)
		return;

	(void)settings;
	/* cmacs pushes the AI answer as `translate.ai-*' settings, and
	   the bar calls this on every push. */
	if (bar_ai_absorb_settings(td->ai, plugin) | tr_sync_ai(plugin, td))
		gowl_bar_plugin_request_panel_refresh(plugin);
	/* A changed `engine' or server shows in the panel at once rather
	   than at the next async poll.  The PATH probe inside only runs
	   for `ai-backend: auto', which cmacs never pushes. */
	engine = tr_engine(plugin);
	g_mutex_lock(&td->lock);
	td->engine_cfg = engine;
	tr_fill_defaults_locked(plugin, td);
	g_mutex_unlock(&td->lock);
	tr_apply_label(plugin, td);
}

/* ----------------------------------------------------------------
 * The panel (compositor thread)
 * ---------------------------------------------------------------- */

/* A panel colour from a `color-*' setting; see
   bar_translate_panel_role(). */
static GowlBarColor
tr_role(GowlBarPlugin *plugin, const gchar *key, GowlBarColor fallback)
{
	GowlBarHost *host = gowl_bar_plugin_get_host(plugin);

	return bar_translate_panel_role(
		gowl_bar_plugin_get_setting(plugin, key), fallback,
		(host != NULL) ? gowl_bar_host_get_theme(host) : NULL);
}

static gchar *
tr_chip_text(const gchar *code, const gchar *detected, gboolean is_source)
{
	if (is_source && g_strcmp0(code, "auto") == 0) {
		if (detected != NULL)
			return g_strdup_printf("Auto (%s)",
				bar_translate_language_name(detected));
		return g_strdup("Auto-detect");
	}
	return g_strdup(bar_translate_language_name(code));
}

static void
tr_panel_help(GowlBarPanel *panel, GowlBarColor muted)
{
	static const gchar *const lines[] = {
		"Ctrl+Return   translate now",
		"Ctrl+S        swap languages",
		"Ctrl+L        pick the source language",
		"Ctrl+Shift+L  pick the target language",
		"Ctrl+Shift+C  copy the translation",
		"Ctrl+Shift+V  translate the clipboard",
		"Ctrl+Shift+X  clear both panes",
		"Ctrl+P        translate while typing on / off",
		"Tab           next pane;  F1  this help;  Escape  close"
	};
	guint i;

	gowl_bar_panel_add_section(panel, "Keys");
	for (i = 0; i < G_N_ELEMENTS(lines); i++) {
		GowlBarPanelItem *item = gowl_bar_panel_add_label(panel, lines[i]);

		gowl_bar_panel_item_set_color(item, muted);
	}
}

static GowlBarPanel *
tr_panel(GowlBarPlugin *plugin, gpointer data)
{
	TranslateData *td = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	GowlBarColor chip, err_role, muted;
	g_autofree gchar *source = NULL;
	g_autofree gchar *target = NULL;
	g_autofree gchar *detected = NULL;
	g_autofree gchar *src_text = NULL;
	g_autofree gchar *out_text = NULL;
	g_autofree gchar *error = NULL;
	g_autofree gchar *note = NULL;
	g_autofree gchar *filter = NULL;
	g_autofree gchar *src_chip = NULL;
	g_autofree gchar *tgt_chip = NULL;
	g_autofree gchar *status = NULL;
	gchar rev[16];
	guint src_rev, out_rev, filter_rev, focus_seq;
	gint focus_pane;
	gboolean busy, auto_on, help, have_curl;
	TrPicker picker;
	BarTranslateEngine engine;

	if (td == NULL)
		return NULL;

	tr_sync_ai(plugin, td);
	/* The panel is rebuilt on every keystroke: use the engine the last
	   async poll resolved, and probe PATH here only before the first
	   one has run. */
	g_mutex_lock(&td->lock);
	engine = td->engine_cfg;
	if (!td->deps_known) {
		g_mutex_unlock(&td->lock);
		engine = tr_engine(plugin);
		g_mutex_lock(&td->lock);
	}
	tr_fill_defaults_locked(plugin, td);
	source     = g_strdup(td->source);
	target     = g_strdup(td->target);
	detected   = g_strdup(td->detected);
	src_text   = g_strdup(td->src_text);
	out_text   = g_strdup(td->out_text);
	error      = g_strdup(td->error);
	note       = g_strdup(td->note);
	filter     = g_strdup(td->filter);
	src_rev    = td->src_rev;
	out_rev    = td->out_rev;
	filter_rev = td->filter_rev;
	focus_pane = td->focus_pane;
	focus_seq  = td->focus_seq;
	busy       = td->busy || bar_ai_is_busy(td->ai);
	auto_on    = td->auto_on;
	help       = td->help;
	picker     = td->picker;
	have_curl  = !td->deps_known || td->have_curl;
	g_mutex_unlock(&td->lock);

	chip     = tr_role(plugin, "color-chip", GOWL_BAR_COLOR_ACCENT);
	err_role = tr_role(plugin, "color-error", GOWL_BAR_COLOR_RED);
	muted    = tr_role(plugin, "color-muted", GOWL_BAR_COLOR_MUTED);

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 640);
	gowl_bar_panel_set_centered(panel, TRUE);

	/*
	 * Item 0 is always the language row and item 1 always a text
	 * field (the source pane, or the picker's filter).  The host keeps
	 * the focused INDEX across a rebuild, so this order is what lands
	 * the keyboard in the filter when Ctrl+L is pressed from the
	 * source pane, and back in the source pane after a pick.
	 */
	src_chip = tr_chip_text(source, detected, TRUE);
	tgt_chip = tr_chip_text(target, NULL, FALSE);
	item = gowl_bar_panel_add_buttons(panel, "langs");
	gowl_bar_panel_add_button(item, src_chip, picker == TR_PICK_SOURCE);
	gowl_bar_panel_add_button(item, "\xe2\x87\x84", FALSE);
	gowl_bar_panel_add_button(item, tgt_chip, picker == TR_PICK_TARGET);
	gowl_bar_panel_item_set_color(item, chip);

	if (picker != TR_PICK_NONE) {
		g_autoptr(GPtrArray) rows = NULL;
		guint i;

		item = gowl_bar_panel_add_entry(panel, "filter",
			(picker == TR_PICK_SOURCE)
				? "Translate from... (name, native name or code)"
				: "Translate into... (name, native name or code)",
			filter);
		g_snprintf(rev, sizeof(rev), "%u", filter_rev);
		gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_REVISION,
		                             rev);
		gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_AUTOFOCUS,
		                             "true");

		rows = bar_translate_filter_languages(filter,
		                                      picker == TR_PICK_SOURCE);
		if (rows->len == 0) {
			item = gowl_bar_panel_add_label(panel,
				"No language matches.");
			gowl_bar_panel_item_set_color(item, muted);
		}
		for (i = 0; i < rows->len; i++) {
			const BarTranslateLang *l = g_ptr_array_index(rows, i);
			g_autofree gchar *id = g_strdup_printf("lang:%s", l->code);
			gboolean current;

			current = (g_strcmp0(l->code, (picker == TR_PICK_SOURCE)
			                     ? source : target) == 0);
			item = gowl_bar_panel_add_row(panel, id, NULL, l->name,
				(g_strcmp0(l->native, l->name) != 0)
					? l->native : NULL);
			gowl_bar_panel_item_set_value(item, l->code);
			gowl_bar_panel_item_set_selected(item, current);
			if (current)
				gowl_bar_panel_item_set_color(item, chip);
		}
		item = gowl_bar_panel_add_row(panel, "pick-cancel", NULL,
		                              "Cancel", NULL);
		gowl_bar_panel_item_set_color(item, muted);
		return panel;
	}

	item = gowl_bar_panel_add_entry(panel, "source",
	                                "Type or paste text to translate",
	                                src_text);
	gowl_bar_panel_item_set_title(item,
		bar_translate_language_name(
			(g_strcmp0(source, "auto") == 0 && detected != NULL)
			? detected : source));
	gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_MULTILINE,
	                             "true");
	gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_AUTOFOCUS,
	                             "true");
	g_snprintf(rev, sizeof(rev), "%u", src_rev);
	gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_REVISION, rev);
	gowl_bar_panel_item_set_height(item, 120);
	if (focus_pane == 1) {
		g_snprintf(rev, sizeof(rev), "%u", focus_seq);
		gowl_bar_panel_item_set_prop(item, GOWL_BAR_PANEL_PROP_FOCUS,
		                             rev);
	}

	/* The translation is a field too, not a label: it can be read
	   with a cursor, corrected before copying, and selected. */
	item = gowl_bar_panel_add_entry(panel, "translation",
		busy ? "Translating..." : "The translation appears here",
		out_text);
	gowl_bar_panel_item_set_title(item, bar_translate_language_name(target));
	gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_MULTILINE,
	                             "true");
	g_snprintf(rev, sizeof(rev), "%u", out_rev);
	gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_REVISION, rev);
	gowl_bar_panel_item_set_height(item, 120);
	if (focus_pane == 2) {
		g_snprintf(rev, sizeof(rev), "%u", focus_seq);
		gowl_bar_panel_item_set_prop(item, GOWL_BAR_PANEL_PROP_FOCUS,
		                             rev);
	}

	/* Status: engine, size, and what is going on. */
	status = g_strdup_printf("%s \xc2\xb7 %ld characters \xc2\xb7 %s",
		bar_translate_engine_name(engine),
		(glong)g_utf8_strlen(src_text, -1),
		busy ? "translating\xe2\x80\xa6"
		     : auto_on ? "translates as you type"
		               : "Ctrl+Return to translate");
	item = gowl_bar_panel_add_label(panel, status);
	gowl_bar_panel_item_set_color(item, muted);

	if (engine == BAR_TRANSLATE_ENGINE_NONE) {
		item = gowl_bar_panel_add_label(panel, tr_none_reason(plugin));
		gowl_bar_panel_item_set_color(item, err_role);
	} else if (engine != BAR_TRANSLATE_ENGINE_AI && !have_curl) {
		item = gowl_bar_panel_add_label(panel,
			"curl is not installed; the web engines need it. "
			"Fedora package: curl");
		gowl_bar_panel_item_set_color(item, err_role);
	}
	if (error != NULL) {
		item = gowl_bar_panel_add_label(panel, error);
		gowl_bar_panel_item_set_color(item, err_role);
	} else if (note != NULL) {
		item = gowl_bar_panel_add_label(panel, note);
		gowl_bar_panel_item_set_color(item, muted);
	}

	gowl_bar_panel_add_toggle(panel, "auto", "Translate while typing",
	                          auto_on);

	item = gowl_bar_panel_add_buttons(panel, "tools");
	gowl_bar_panel_add_button(item, "Translate", FALSE);
	gowl_bar_panel_add_button(item, "Copy", FALSE);
	gowl_bar_panel_add_button(item, "Clipboard", FALSE);
	gowl_bar_panel_add_button(item, "Clear", FALSE);
	gowl_bar_panel_add_button(item, "Keys", help);

	if (help)
		tr_panel_help(panel, muted);
	return panel;
}

static void
tr_panel_opened(GowlBarPlugin *plugin, gpointer data)
{
	TranslateData *td = data;

	if (td == NULL)
		return;

	(void)plugin;
	g_mutex_lock(&td->lock);
	td->panel_open = TRUE;
	g_mutex_unlock(&td->lock);
}

static void
tr_panel_closed(GowlBarPlugin *plugin, gpointer data)
{
	TranslateData *td = data;

	if (td == NULL)
		return;

	(void)plugin;
	g_mutex_lock(&td->lock);
	td->panel_open = FALSE;
	td->picker = TR_PICK_NONE;
	g_mutex_unlock(&td->lock);
}

/* ----------------------------------------------------------------
 * Actions and keys (compositor thread)
 * ---------------------------------------------------------------- */

static void
tr_open_picker(TranslateData *td, TrPicker which)
{
	g_mutex_lock(&td->lock);
	if (td->picker == which) {
		td->picker = TR_PICK_NONE;
	} else {
		td->picker = which;
		tr_set_str(&td->filter, "");
		td->filter_rev++;
	}
	g_mutex_unlock(&td->lock);
}

/*
 * Choose a language for the open picker.  Picking for one side the
 * language the other side already has swaps them rather than making a
 * pair that translates a language into itself.
 */
static void
tr_pick(GowlBarPlugin *plugin, TranslateData *td, const gchar *code)
{
	const BarTranslateLang *lang;
	gboolean is_auto, start;

	is_auto = (g_strcmp0(code, "auto") == 0);
	lang = bar_translate_language_find(code);

	g_mutex_lock(&td->lock);
	tr_fill_defaults_locked(plugin, td);
	if (td->picker == TR_PICK_SOURCE && (is_auto || lang != NULL)) {
		const gchar *c = is_auto ? "auto" : lang->code;

		if (g_strcmp0(c, td->target) == 0 &&
		    g_strcmp0(td->source, "auto") != 0)
			tr_set_str(&td->target, td->source);
		tr_set_str(&td->source, c);
	} else if (td->picker == TR_PICK_TARGET && lang != NULL) {
		if (g_strcmp0(lang->code, td->source) == 0)
			tr_set_str(&td->source, td->target);
		tr_set_str(&td->target, lang->code);
	} else {
		g_mutex_unlock(&td->lock);
		return;
	}
	td->picker = TR_PICK_NONE;
	td->touched = TRUE;
	td->pair_chosen = TRUE;
	td->dirty = TRUE;
	start = td->auto_on && td->src_text[0] != '\0';
	g_mutex_unlock(&td->lock);

	if (start)
		tr_start(plugin, td);
}

static void
tr_swap(GowlBarPlugin *plugin, TranslateData *td)
{
	g_autofree gchar *ns = NULL;
	g_autofree gchar *nt = NULL;
	BarTranslateEngine engine;

	engine = tr_engine(plugin);
	bar_ai_cancel(td->ai);

	g_mutex_lock(&td->lock);
	tr_fill_defaults_locked(plugin, td);
	if (!bar_translate_swap(td->source, td->target, td->detected,
	                        &ns, &nt)) {
		tr_set_str(&td->note, (g_strcmp0(td->source, "auto") == 0)
			? "Nothing detected yet: translate something first, "
			  "or pick a source language (Ctrl+L)."
			: "Both sides are the same language.");
		g_mutex_unlock(&td->lock);
		return;
	}
	tr_set_str(&td->source, ns);
	tr_set_str(&td->target, nt);

	/* The texts swap with the languages: the translation becomes the
	   new source, and the old source is already ITS translation, so
	   no request is needed for the swapped state. */
	if (td->out_text[0] != '\0') {
		gchar *tmp = td->src_text;

		td->src_text = td->out_text;
		td->out_text = tmp;
		td->src_rev++;
		td->out_rev++;
	}
	td->generation++;     /* anything in flight is for the old pair */
	td->busy = FALSE;
	td->ai_active = FALSE;
	g_clear_pointer(&td->ai_seen, g_free);
	g_clear_pointer(&td->detected, g_free);
	g_clear_pointer(&td->error, g_free);
	g_clear_pointer(&td->note, g_free);
	g_free(td->last_key);
	td->last_key = tr_request_key(engine, td->source, td->target,
	                              td->src_text);
	td->last_failed = FALSE;
	td->touched = TRUE;
	td->pair_chosen = TRUE;
	td->dirty = TRUE;
	g_mutex_unlock(&td->lock);
}

static void
tr_clear(TranslateData *td)
{
	bar_ai_cancel(td->ai);
	g_mutex_lock(&td->lock);
	tr_set_str(&td->src_text, "");
	tr_set_str(&td->out_text, "");
	td->src_rev++;
	td->out_rev++;
	td->generation++;
	td->busy = FALSE;
	td->ai_active = FALSE;
	td->last_edit_us = 0;
	g_clear_pointer(&td->ai_seen, g_free);
	g_clear_pointer(&td->detected, g_free);
	g_clear_pointer(&td->error, g_free);
	g_clear_pointer(&td->note, g_free);
	g_clear_pointer(&td->last_key, g_free);
	td->dirty = TRUE;
	g_mutex_unlock(&td->lock);
}

static void
tr_toggle_auto(TranslateData *td)
{
	g_mutex_lock(&td->lock);
	td->auto_on = !td->auto_on;
	td->auto_known = TRUE;
	td->dirty = TRUE;
	g_mutex_unlock(&td->lock);
}

static void
tr_toggle_help(TranslateData *td)
{
	g_mutex_lock(&td->lock);
	td->help = !td->help;
	g_mutex_unlock(&td->lock);
}

static void
tr_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
          gint index, gdouble value, guint button)
{
	TranslateData *td = data;

	if (td == NULL)
		return;

	(void)value;
	(void)button;
	if (item_id == NULL)
		return;

	if (g_strcmp0(item_id, "langs") == 0) {
		switch (index) {
		case 0:  tr_open_picker(td, TR_PICK_SOURCE); break;
		case 1:  tr_swap(plugin, td);                break;
		case 2:  tr_open_picker(td, TR_PICK_TARGET); break;
		default: break;
		}
		return;
	}
	if (g_str_has_prefix(item_id, "lang:")) {
		tr_pick(plugin, td, item_id + strlen("lang:"));
		return;
	}
	if (g_strcmp0(item_id, "pick-cancel") == 0) {
		g_mutex_lock(&td->lock);
		td->picker = TR_PICK_NONE;
		g_mutex_unlock(&td->lock);
		return;
	}
	if (g_strcmp0(item_id, "auto") == 0) {
		tr_toggle_auto(td);
		return;
	}
	if (g_strcmp0(item_id, "tools") == 0) {
		switch (index) {
		case 0:  tr_start(plugin, td); break;
		case 1:  tr_copy(plugin, td);        break;
		case 2:  tr_paste(plugin, td);       break;
		case 3:  tr_clear(td);               break;
		case 4:  tr_toggle_help(td);         break;
		default: break;
		}
	}
}

static void
tr_panel_text(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
              const gchar *text, gboolean submitted)
{
	TranslateData *td = data;

	if (td == NULL)
		return;

	if (item_id == NULL || text == NULL)
		return;

	if (g_strcmp0(item_id, "filter") == 0) {
		g_autoptr(GPtrArray) rows = NULL;
		TrPicker picker;

		g_mutex_lock(&td->lock);
		tr_set_str(&td->filter, text);
		picker = td->picker;
		g_mutex_unlock(&td->lock);
		if (!submitted || picker == TR_PICK_NONE)
			return;
		/* Return in the filter takes the best match. */
		rows = bar_translate_filter_languages(text,
		                                      picker == TR_PICK_SOURCE);
		if (rows->len > 0)
			tr_pick(plugin, td,
			        ((const BarTranslateLang *)
			         g_ptr_array_index(rows, 0))->code);
		return;
	}

	if (g_strcmp0(item_id, "source") == 0) {
		g_mutex_lock(&td->lock);
		if (g_strcmp0(td->src_text, text) != 0) {
			tr_set_str(&td->src_text, text);
			td->last_edit_us = g_get_monotonic_time();
			td->dirty = TRUE;
		}
		g_mutex_unlock(&td->lock);
		if (submitted)
			tr_start(plugin, td);
		return;
	}

	if (g_strcmp0(item_id, "translation") == 0) {
		/* An edit to the translation is the user's to keep: it is
		   what Copy copies.  Ctrl+Return there translates again. */
		g_mutex_lock(&td->lock);
		tr_set_str(&td->out_text, text);
		g_mutex_unlock(&td->lock);
		if (submitted)
			tr_start(plugin, td);
	}
}

static gboolean
tr_panel_key(GowlBarPlugin *plugin, gpointer data, guint keysym,
             guint modifiers, gint focused_item)
{
	TranslateData *td = data;
	gboolean ctrl, shift;
	guint key;

	if (td == NULL)
		return FALSE;

	(void)focused_item;
	/* Alt and Super belong to the window manager. */
	if (modifiers & (GOWL_KEY_MOD_ALT | GOWL_KEY_MOD_LOGO))
		return FALSE;
	ctrl  = (modifiers & GOWL_KEY_MOD_CTRL) != 0;
	shift = (modifiers & GOWL_KEY_MOD_SHIFT) != 0;

	if (keysym == XKB_KEY_F1 && !ctrl) {
		tr_toggle_help(td);
		return TRUE;
	}
	if (!ctrl)
		return FALSE;

	/* Shift turns the keysym upper-case; compare one form. */
	key = (keysym >= XKB_KEY_A && keysym <= XKB_KEY_Z)
		? keysym - XKB_KEY_A + XKB_KEY_a : keysym;

	switch (key) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		tr_start(plugin, td);
		return TRUE;
	case XKB_KEY_s:
		if (shift)
			return FALSE;
		tr_swap(plugin, td);
		return TRUE;
	case XKB_KEY_l:
		tr_open_picker(td, shift ? TR_PICK_TARGET : TR_PICK_SOURCE);
		return TRUE;
	case XKB_KEY_c:
		if (!shift)
			return FALSE;
		tr_copy(plugin, td);
		return TRUE;
	case XKB_KEY_v:
		if (!shift)
			return FALSE;
		tr_paste(plugin, td);
		return TRUE;
	case XKB_KEY_x:
		if (!shift)
			return FALSE;
		tr_clear(td);
		return TRUE;
	case XKB_KEY_p:
		tr_toggle_auto(td);
		return TRUE;
	case XKB_KEY_1:
	case XKB_KEY_2:
		/* Ask the host to move the keyboard: a new token on the
		   chosen pane, obeyed on the rebuild that follows. */
		g_mutex_lock(&td->lock);
		td->focus_pane = (keysym == XKB_KEY_1) ? 1 : 2;
		td->focus_seq++;
		g_mutex_unlock(&td->lock);
		gowl_bar_plugin_request_panel_refresh(plugin);
		return TRUE;
	default:
		break;
	}
	return FALSE;
}

static const GowlBarPluginVTable translate_vtable = {
	sizeof(GowlBarPluginVTable),
	tr_create, tr_destroy,
	NULL, NULL, tr_configure,
	tr_interval, tr_poll, tr_poll_async,
	NULL, NULL,
	NULL, NULL,
	tr_panel, tr_action,
	tr_panel_opened, tr_panel_closed,
	tr_panel_key,
	NULL,
	tr_panel_text, NULL
};

/**
 * bar_register_translate_plugins:
 * @registry: the registry to populate
 *
 * Registers `translate' (alias `translator').
 */
void
bar_register_translate_plugins(GowlBarRegistry *registry)
{
	gowl_bar_registry_register_vtable(registry, TR_WIDGET, "Translate",
		"A two-pane translator overlay", &translate_vtable);
	gowl_bar_registry_register_alias(registry, "translator", TR_WIDGET);
}
