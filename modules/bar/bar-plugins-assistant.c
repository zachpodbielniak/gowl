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

/**
 * SECTION:bar-plugins-assistant
 * @title: The ai-assistant widget
 * @short_description: chat with any provider, manage local ollama
 *   models, see what the machine has left
 *
 * A small activity wave in the bar and a three-tab dropdown:
 *
 *   Chat     provider and model choosers, a multi-line field, the
 *            conversation.  Goes through bar-ai.c, so it reaches every
 *            cmacs-ai provider under `cmacs --gowl' and ai-glib's `ai'
 *            command standalone.
 *   Models   the local ollama server: what is installed, what is
 *            loaded, whether each model fits this machine, and load /
 *            unload / delete / pull --- every one that downloads,
 *            deletes or loads behind an approval card.
 *   Machine  CPU, memory, GPU, disk, network, and what the loaded
 *            models are holding.
 *
 * ollama is reached with curl on a worker thread.  bar.so links no HTTP
 * library and is not going to grow one for this: the requests are a
 * handful of small JSON calls and one streamed download, and curl is
 * on every machine that has ollama.
 *
 * What it deliberately does not do: install ollama (on Fedora it is not
 * packaged; it runs as a podman quadlet the user manages), or update
 * itself or anything else.  The panel says how instead.
 *
 * Threads, as everywhere in the bar: everything the bar calls runs on
 * the compositor's thread (under cmacs, Emacs's) and must not block.
 * curl, nvidia-smi, `ai --list-providers' and wl-copy all run on
 * workers; their results are swapped into the plugin data under its
 * mutex, and the panel builder takes the same mutex for the few
 * microseconds it reads them.
 */

#include "bar-internal.h"
#include "bar-ai.h"
#include "bar-assistant-model.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* The widget's registry name; cmacs addresses its AI replies to it. */
#define AS_WIDGET "ai-assistant"

/* The bar icon, and the panel's: a robot head (nf-md-robot).  The
   old equaliser-only look is `icon-style: wave'. */
#define AS_MINI_BARS (3)
/* Wide by default: answers are paragraphs and code, and the Models tab
   is a table.  `width' overrides it. */
#define AS_PANEL_WIDTH (680)
#define AS_GLYPH "\xf3\xb0\x9a\xa9"

static gboolean as_wave_style (GowlBarPlugin *plugin);

/* How many activity samples the wave remembers.  More than the widest
   wave anyone configures, so `bars' can grow without the history
   being the limit. */
#define AS_SAMPLES (16)

/* The fewest seconds between two ollama refreshes, however often the
   panel asks.  A refresh is two curl calls and a /proc scan; once
   every few seconds is plenty for a list that changes when the user
   changes it. */
#define AS_REFRESH_MIN_US (5 * G_USEC_PER_SEC)

/* How long the "click again to clear" arming lasts. */
#define AS_CLEAR_ARM_US (5 * G_USEC_PER_SEC)

/* The longest conversation sent with a question.  Past it the oldest
   messages are left out --- see bar_assistant_transcript_flatten(). */
#define AS_MAX_PROMPT (192 * 1024)

/* Pull progress lines longer than this are not ollama's and are
   dropped rather than buffered without limit. */
#define AS_MAX_LINE (64 * 1024)

#define AS_DEFAULT_SYSTEM \
	"You are a concise assistant in the user's Linux desktop bar. " \
	"Answer in plain text, without Markdown."

typedef enum {
	AS_TAB_CHAT = 0,
	AS_TAB_MODELS,
	AS_TAB_MACHINE
} AsTab;

typedef enum {
	AS_PICK_NONE = 0,
	AS_PICK_PROVIDER,
	AS_PICK_MODEL
} AsPick;

typedef struct {
	/*
	 * Guards every pointer, array and string below.  Workers rebuild
	 * the model lists, the machine reading and the pull progress
	 * while the panel builder walks them on the compositor thread;
	 * the host serialises none of that.  Never held across a call
	 * into bar-ai or a subprocess.
	 */
	GMutex lock;

	/* The conversation.  bar-ai has its own lock. */
	BarAi                  *ai;
	BarAssistantTranscript *transcript;
	gchar                  *pending_id;   /* asked, not yet in the transcript */
	gchar                  *last_answer;  /* what Copy copies */
	gchar                  *chat_note;    /* "Stopped.", or why it failed */
	gboolean                chat_note_is_error;
	gchar                  *draft;        /* the prompt field, as typed */
	guint                   entry_rev;    /* bumped to replace the field's text */
	gint64                  clear_armed_at;

	/* The panel's own state. */
	AsTab    tab;
	AsPick   pick;
	gboolean panel_open;

	/* Providers from `ai --list-providers', for the standalone route. */
	gchar  **cli_providers;
	gchar   *cli_providers_for;   /* the ai-command they came from */

	/* ollama, as of the last refresh. */
	GPtrArray *installed;         /* BarAssistantModel* */
	GPtrArray *loaded;            /* BarAssistantModel* */
	gboolean   ollama_ok;
	gboolean   no_curl;
	gchar     *ollama_error;
	gchar     *ollama_url_tried;
	gint64     refreshed_at;
	gboolean   refreshing;
	gchar     *selected;          /* the model the action row is for */

	/* The machine, as of the last refresh. */
	BarAssistantMachine machine;
	gboolean            have_machine;

	/* A model operation in flight (load, unload, delete). */
	gboolean op_busy;
	gchar   *op_status;
	gboolean op_failed;

	/* A pull in flight. */
	gboolean pulling;
	gchar   *pull_model;
	gchar   *pull_status;
	gdouble  pull_fraction;
	GPid     pull_pid;
	guint    pull_gen;

	BarAssistantApproval approval;

	/* The bar indicator. */
	gdouble          samples[AS_SAMPLES];
	guint            n_samples;
	gdouble          heights[16];
	guint            n_heights;
	gdouble          phase;
	gsize            last_len;
	gint64           last_sample_at;
	BarAssistantLook look;
	gboolean         error_look;
} Assistant;

/* ----------------------------------------------------------------
 * Small readers
 * ---------------------------------------------------------------- */

static const gchar *
as_setting(GowlBarPlugin *plugin, const gchar *key, const gchar *fallback)
{
	const gchar *v;

	v = gowl_bar_plugin_get_setting(plugin, key);
	return (v != NULL && v[0] != '\0') ? v : fallback;
}

/* The provider a question goes to: the one picked in the panel or
   configured, else the one cmacs says is its default, else none (the
   backend's own default). */
static const gchar *
as_provider(GowlBarPlugin *plugin)
{
	const gchar *p;

	p = as_setting(plugin, "ai-provider", NULL);
	if (p == NULL)
		p = as_setting(plugin, "ai-default-provider", NULL);
	return p;
}

/* The model to SHOW: the one picked or configured, else --- only while
   the provider is cmacs's default too --- the default model cmacs
   pushed.  A default model belongs to its provider, so it is never
   shown beside a provider the user picked.  Display only: requests
   send `ai-model' alone and leave the rest to the backend. */
static const gchar *
as_shown_model(GowlBarPlugin *plugin)
{
	const gchar *m;

	m = as_setting(plugin, "ai-model", NULL);
	if (m == NULL && as_setting(plugin, "ai-provider", NULL) == NULL)
		m = as_setting(plugin, "ai-default-model", NULL);
	return m;
}

static gint
as_idle_interval(GowlBarPlugin *plugin)
{
	return CLAMP(gowl_bar_plugin_get_setting_int(plugin, "refresh", 10),
	             5, 60);
}

static guint
as_history(GowlBarPlugin *plugin)
{
	return (guint)CLAMP(gowl_bar_plugin_get_setting_int(plugin,
	                    "history-turns", 12), 2, 200);
}

/* The setting naming curl, nvidia-smi and friends: a command, never a
   command line, since it is argv[0]. */
static const gchar *
as_command(GowlBarPlugin *plugin, const gchar *key, const gchar *fallback)
{
	return as_setting(plugin, key, fallback);
}

static gchar *
as_models_dir(GowlBarPlugin *plugin)
{
	const gchar *dir;

	dir = as_setting(plugin, "models-dir", NULL);
	if (dir != NULL)
		return bar_expand_tilde(dir);
	/* ollama's own default, and what OLLAMA_MODELS overrides. */
	if (g_getenv("OLLAMA_MODELS") != NULL &&
	    g_getenv("OLLAMA_MODELS")[0] != '\0')
		return g_strdup(g_getenv("OLLAMA_MODELS"));
	return g_build_filename(g_get_home_dir(), ".ollama", "models", NULL);
}

/*
 * A palette role named by a colour setting, for PANEL items, which
 * take roles only.  The bar's own colour goes through
 * bar_plugin_apply_color(), where a `#rrggbb' literal works too; a
 * literal here falls back to the default role, and the documentation
 * says so.
 */
static GowlBarColor
as_role_setting(GowlBarPlugin *plugin, const gchar *key,
                GowlBarColor fallback)
{
	const gchar *spec;
	gint r;

	spec = gowl_bar_plugin_get_setting(plugin, key);
	if (spec == NULL || spec[0] == '\0')
		return fallback;
	for (r = 0; r < GOWL_BAR_COLOR_COUNT; r++) {
		if (g_ascii_strcasecmp(spec,
		        gowl_bar_theme_color_name((GowlBarColor)r)) == 0)
			return (GowlBarColor)r;
	}
	return fallback;
}

static GowlBarColor
as_look_role(BarAssistantLook look)
{
	switch (look) {
	case BAR_ASSISTANT_LOOK_THINKING:  return GOWL_BAR_COLOR_YELLOW;
	case BAR_ASSISTANT_LOOK_STREAMING: return GOWL_BAR_COLOR_ACCENT;
	case BAR_ASSISTANT_LOOK_ERROR:     return GOWL_BAR_COLOR_RED;
	case BAR_ASSISTANT_LOOK_IDLE:
	default:                           return GOWL_BAR_COLOR_SUBTEXT;
	}
}

static GowlBarColor
as_fit_role(BarAssistantFit fit)
{
	switch (fit) {
	case BAR_ASSISTANT_FIT_COMFORTABLE: return GOWL_BAR_COLOR_GREEN;
	case BAR_ASSISTANT_FIT_TIGHT:       return GOWL_BAR_COLOR_YELLOW;
	case BAR_ASSISTANT_FIT_TOO_BIG:     return GOWL_BAR_COLOR_RED;
	case BAR_ASSISTANT_FIT_UNKNOWN:
	default:                            return GOWL_BAR_COLOR_MUTED;
	}
}

static const gchar *
as_backend_name(BarAiBackend backend)
{
	switch (backend) {
	case BAR_AI_BACKEND_CMACS: return "cmacs";
	case BAR_AI_BACKEND_CLI:   return "ai command";
	case BAR_AI_BACKEND_NONE:
	default:                   return "none";
	}
}

/* Find a model by name in one of the lists.  Caller holds the lock. */
static BarAssistantModel *
as_find_model(GPtrArray *list, const gchar *name)
{
	guint i;

	if (list == NULL || name == NULL)
		return NULL;
	for (i = 0; i < list->len; i++) {
		BarAssistantModel *m = g_ptr_array_index(list, i);

		if (g_strcmp0(m->name, name) == 0)
			return m;
	}
	return NULL;
}

/* ----------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------- */

static gpointer
as_create(GowlBarPlugin *plugin)
{
	Assistant *as;

	(void)plugin;
	as = g_new0(Assistant, 1);
	g_mutex_init(&as->lock);
	as->ai         = bar_ai_new(AS_WIDGET);
	as->transcript = bar_assistant_transcript_new();
	as->installed  = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_assistant_model_free);
	as->loaded     = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_assistant_model_free);
	as->n_heights  = 5;
	return as;
}

/*
 * A worker holds a reference to the PLUGIN for as long as it runs, so
 * this --- called when the plugin is finalised --- never frees what a
 * worker is using.  A pull still downloading is told to stop; its
 * worker has already finished by the time we are here.
 */
static void
as_destroy(GowlBarPlugin *plugin, gpointer data)
{
	Assistant *as = data;

	(void)plugin;
	if (as == NULL)
		return;
	if (as->pull_pid > 0)
		kill(as->pull_pid, SIGTERM);
	bar_ai_free(as->ai);
	bar_assistant_transcript_free(as->transcript);
	g_free(as->pending_id);
	g_free(as->last_answer);
	g_free(as->chat_note);
	g_free(as->draft);
	g_strfreev(as->cli_providers);
	g_free(as->cli_providers_for);
	g_ptr_array_unref(as->installed);
	g_ptr_array_unref(as->loaded);
	g_free(as->ollama_error);
	g_free(as->ollama_url_tried);
	g_free(as->selected);
	bar_assistant_machine_clear(&as->machine);
	g_free(as->op_status);
	g_free(as->pull_model);
	g_free(as->pull_status);
	bar_assistant_approval_clear(&as->approval);
	g_mutex_clear(&as->lock);
	g_free(as);
}

/* ----------------------------------------------------------------
 * The conversation
 * ---------------------------------------------------------------- */

/*
 * Move a finished answer into the transcript.
 *
 * bar-ai holds the answer in its slot; this widget keeps the
 * conversation.  Called from every place the slot can have changed ---
 * the poll, a cmacs push (configure), and the panel build --- and
 * idempotent, because the pending id is cleared the moment the answer
 * is taken.
 *
 * Returns: %TRUE when the transcript changed
 */
static gboolean
as_sync_answer(GowlBarPlugin *plugin, Assistant *as)
{
	g_autofree gchar *text = NULL;
	g_autofree gchar *err = NULL;
	g_autofree gchar *id = NULL;
	BarAiState state;
	gboolean changed = FALSE;

	state = bar_ai_get(as->ai, &text, &err);
	id    = bar_ai_get_request_id(as->ai);

	g_mutex_lock(&as->lock);
	if (as->pending_id != NULL && g_strcmp0(id, as->pending_id) == 0) {
		if (state == BAR_AI_DONE) {
			bar_assistant_transcript_add(as->transcript,
				BAR_ASSISTANT_ROLE_ASSISTANT, text);
			bar_assistant_transcript_trim(as->transcript,
				as_history(plugin));
			g_free(as->last_answer);
			as->last_answer = g_utf8_make_valid(
				(text != NULL) ? text : "", -1);
			g_clear_pointer(&as->pending_id, g_free);
			g_clear_pointer(&as->chat_note, g_free);
			as->error_look = FALSE;
			changed = TRUE;
		} else if (state == BAR_AI_ERROR) {
			g_free(as->chat_note);
			as->chat_note = g_utf8_make_valid(
				(err != NULL) ? err : "the request failed", -1);
			as->chat_note_is_error = TRUE;
			as->error_look = TRUE;
			g_clear_pointer(&as->pending_id, g_free);
			changed = TRUE;
		}
	}
	g_mutex_unlock(&as->lock);
	return changed;
}

/* The system prompt for one question: the configured one, and the
   machine paragraph when `machine-context' asks for it. */
static gchar *
as_system_prompt(GowlBarPlugin *plugin, Assistant *as)
{
	const gchar *base;
	GString *out;

	base = as_setting(plugin, "system", AS_DEFAULT_SYSTEM);
	out = g_string_new(base);

	if (gowl_bar_plugin_get_setting_bool(plugin, "machine-context",
	                                     FALSE)) {
		g_mutex_lock(&as->lock);
		if (as->have_machine) {
			g_autoptr(GPtrArray) names = NULL;
			g_autofree gchar *summary = NULL;
			guint i;

			names = g_ptr_array_new();
			if (as->ollama_ok)
				for (i = 0; i < as->loaded->len; i++)
					g_ptr_array_add(names, ((BarAssistantModel *)
						g_ptr_array_index(as->loaded, i))->name);
			g_ptr_array_add(names, NULL);
			summary = bar_assistant_machine_summary(&as->machine,
				as->ollama_ok
					? (const gchar * const *)names->pdata
					: NULL);
			g_string_append(out, "\n\n");
			g_string_append(out, summary);
		}
		g_mutex_unlock(&as->lock);
	}
	return g_string_free(out, FALSE);
}

static void
as_note(Assistant *as, const gchar *note, gboolean is_error)
{
	g_mutex_lock(&as->lock);
	g_free(as->chat_note);
	as->chat_note = g_strdup(note);
	as->chat_note_is_error = is_error;
	g_mutex_unlock(&as->lock);
}

/*
 * Ask.  The conversation so far goes with the question as one
 * document (see bar_assistant_transcript_flatten()), because neither
 * backend keeps a session between bar requests --- which is also what
 * lets the provider be switched mid-conversation.
 */
static void
as_send(GowlBarPlugin *plugin, Assistant *as, const gchar *text)
{
	g_autofree gchar *question = NULL;
	g_autofree gchar *system = NULL;
	g_autofree gchar *prompt = NULL;
	g_autoptr(GError) error = NULL;
	gboolean ok;

	question = g_strstrip(g_strdup((text != NULL) ? text : ""));
	if (question[0] == '\0')
		return;
	if (bar_ai_is_busy(as->ai)) {
		as_note(as, "Still answering: press Stop first.", FALSE);
		return;
	}

	system = as_system_prompt(plugin, as);

	g_mutex_lock(&as->lock);
	prompt = bar_assistant_transcript_flatten(as->transcript, question,
	                                          AS_MAX_PROMPT);
	bar_assistant_transcript_add(as->transcript,
	                             BAR_ASSISTANT_ROLE_USER, question);
	g_clear_pointer(&as->draft, g_free);
	g_clear_pointer(&as->chat_note, g_free);
	g_clear_pointer(&as->pending_id, g_free);
	as->entry_rev++;
	as->error_look = FALSE;
	as->clear_armed_at = 0;
	g_mutex_unlock(&as->lock);

	/* Outside our lock: on the CLI route the request can run inline
	   in a test host, and it reports back through bar-ai's own. */
	ok = bar_ai_start(as->ai, plugin, system, prompt, &error);

	g_mutex_lock(&as->lock);
	if (ok) {
		as->pending_id = bar_ai_get_request_id(as->ai);
	} else {
		/* The question never left: take it back out, and put it
		   back in the field so nothing typed is lost. */
		bar_assistant_transcript_pop(as->transcript);
		g_free(as->draft);
		as->draft = g_strdup(question);
		as->entry_rev++;
		g_free(as->chat_note);
		as->chat_note = g_strdup((error != NULL) ? error->message
		                                         : "could not send");
		as->chat_note_is_error = TRUE;
		as->error_look = TRUE;
	}
	bar_assistant_transcript_trim(as->transcript, as_history(plugin));
	g_mutex_unlock(&as->lock);

	as_sync_answer(plugin, as);
	gowl_bar_plugin_request_redraw(plugin);
}

/* Stop: abandon the request, keeping whatever had arrived. */
static void
as_stop(GowlBarPlugin *plugin, Assistant *as)
{
	g_autofree gchar *text = NULL;
	BarAiState state;

	state = bar_ai_get(as->ai, &text, NULL);
	bar_ai_cancel(as->ai);

	g_mutex_lock(&as->lock);
	if (as->pending_id != NULL) {
		if (state == BAR_AI_STREAMING && text != NULL &&
		    text[0] != '\0') {
			g_autofree gchar *partial = g_strconcat(text,
				" [stopped]", NULL);

			bar_assistant_transcript_add(as->transcript,
				BAR_ASSISTANT_ROLE_ASSISTANT, partial);
		}
		g_clear_pointer(&as->pending_id, g_free);
		g_free(as->chat_note);
		as->chat_note = g_strdup("Stopped.");
		as->chat_note_is_error = FALSE;
	}
	g_mutex_unlock(&as->lock);
	(void)plugin;
}

/* Clear, two-step: the first click arms, a second within five seconds
   forgets the conversation. */
static void
as_clear(GowlBarPlugin *plugin, Assistant *as)
{
	gint64 now = g_get_monotonic_time();
	gboolean armed;

	g_mutex_lock(&as->lock);
	armed = (as->clear_armed_at != 0 &&
	         now - as->clear_armed_at < AS_CLEAR_ARM_US);
	if (!armed) {
		as->clear_armed_at = now;
		g_mutex_unlock(&as->lock);
		return;
	}
	as->clear_armed_at = 0;
	g_mutex_unlock(&as->lock);

	bar_ai_cancel(as->ai);

	g_mutex_lock(&as->lock);
	bar_assistant_transcript_clear(as->transcript);
	g_clear_pointer(&as->pending_id, g_free);
	g_clear_pointer(&as->last_answer, g_free);
	g_clear_pointer(&as->chat_note, g_free);
	as->error_look = FALSE;
	g_mutex_unlock(&as->lock);
	(void)plugin;
}

/* ── Copy the last answer ─────────────────────────────────────────── */

typedef struct {
	Assistant *as;
	gchar     *command;
	gchar     *text;
} CopyJob;

static void
copy_job_free(gpointer data)
{
	CopyJob *job = data;

	g_free(job->command);
	g_free(job->text);
	g_free(job);
}

/*
 * wl-copy with the answer on stdin, on a worker.  Not in argv: an
 * answer can be long, and `ps' should not show it.  wl-copy forks a
 * server and its foreground half exits once stdin is read; that exit
 * is reaped here, with a deadline, never through a GLib child watch.
 */
static void
as_copy_work(GowlBarPlugin *plugin, gpointer user_data)
{
	CopyJob *job = user_data;
	const gchar *argv[2];
	g_autoptr(GError) error = NULL;
	GPid pid = 0;
	gint in_fd = -1, status = 0;
	gsize len, written;
	gint64 deadline;
	pid_t done;

	argv[0] = job->command;
	argv[1] = NULL;
	if (!g_spawn_async_with_pipes(NULL, (gchar **)argv, NULL,
	                              G_SPAWN_SEARCH_PATH |
	                              G_SPAWN_DO_NOT_REAP_CHILD |
	                              G_SPAWN_STDOUT_TO_DEV_NULL |
	                              G_SPAWN_STDERR_TO_DEV_NULL,
	                              NULL, NULL, &pid, &in_fd, NULL, NULL,
	                              &error)) {
		/* Said in the panel, not a toast: a worker may only set
		   state and ask for a refresh. */
		g_mutex_lock(&job->as->lock);
		g_free(job->as->chat_note);
		job->as->chat_note = g_strdup_printf("Could not copy: %s is not "
			"installed (Fedora package: wl-clipboard).", job->command);
		job->as->chat_note_is_error = TRUE;
		g_mutex_unlock(&job->as->lock);
		gowl_bar_plugin_request_panel_refresh(plugin);
		return;
	}

	len = strlen(job->text);
	written = 0;
	while (written < len) {
		gssize n = write(in_fd, job->text + written, len - written);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		written += (gsize)n;
	}
	close(in_fd);

	deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
	for (;;) {
		done = waitpid(pid, &status, WNOHANG);
		if (done == pid || (done < 0 && errno != EINTR))
			break;
		if (g_get_monotonic_time() > deadline) {
			kill(pid, SIGTERM);
			waitpid(pid, &status, 0);
			break;
		}
		g_usleep(20 * 1000);
	}
	g_spawn_close_pid(pid);
}

static void
as_copy(GowlBarPlugin *plugin, Assistant *as)
{
	CopyJob *job;

	g_mutex_lock(&as->lock);
	if (as->last_answer == NULL || as->last_answer[0] == '\0') {
		g_mutex_unlock(&as->lock);
		return;
	}
	job = g_new0(CopyJob, 1);
	job->as   = as;
	job->text = g_strdup(as->last_answer);
	g_mutex_unlock(&as->lock);

	job->command = g_strdup(as_command(plugin, "copy-command", "wl-copy"));
	gowl_bar_plugin_queue_work(plugin, as_copy_work, job, copy_job_free);
}

/* ── Continue in cmacs ────────────────────────────────────────────── */

/*
 * Hand the conversation to a cmacs-ai chat buffer.  Detached through
 * gowl_bar_plugin_spawn(); the Elisp side returns at once and opens
 * the buffer from a timer, so nothing here waits on the editor.
 */
static void
as_continue_in_cmacs(GowlBarPlugin *plugin, Assistant *as)
{
	g_autofree gchar *flat = NULL;
	g_autofree gchar *cmdline = NULL;

	g_mutex_lock(&as->lock);
	flat = bar_assistant_transcript_flatten(as->transcript, NULL, 0);
	g_mutex_unlock(&as->lock);

	cmdline = bar_assistant_cmacs_open_cmdline(
		as_setting(plugin, "emacsctl", "emacsctl"),
		as_provider(plugin),
		as_setting(plugin, "ai-model", NULL), flat);
	if (cmdline == NULL) {
		as_note(as, "The conversation is not valid UTF-8; "
		        "it cannot be handed to cmacs.", TRUE);
		return;
	}
	gowl_bar_plugin_spawn(plugin, cmdline);
}

/* ----------------------------------------------------------------
 * ollama and the machine: refresh (worker)
 * ---------------------------------------------------------------- */

/*
 * One refresh: installed and loaded models, and a fresh look at the
 * machine.  Worker thread only --- it runs curl and nvidia-smi and
 * waits for both.  Results are built into locals and swapped in under
 * the lock, so the panel never walks an array being freed.
 */
static void
as_refresh_now(GowlBarPlugin *plugin, Assistant *as)
{
	g_autofree gchar *url = NULL;
	g_autofree gchar *tags = NULL;
	g_autofree gchar *ps = NULL;
	g_autofree gchar *nvidia = NULL;
	g_autofree gchar *models_dir = NULL;
	g_autofree gchar *error = NULL;
	g_autoptr(GPtrArray) installed = NULL;
	g_autoptr(GPtrArray) loaded = NULL;
	BarAssistantMachine machine;
	const gchar *curl;
	const gchar *smi;
	gboolean no_curl = FALSE;
	gboolean ok = FALSE;

	curl = as_command(plugin, "curl", "curl");
	url  = bar_assistant_normalise_url(as_setting(plugin, "ollama-url",
	                                              "http://127.0.0.1:11434"));

	if (url == NULL) {
		error = g_strdup("ollama-url must start with http:// or https://");
	} else if (!bar_have_command(curl)) {
		no_curl = TRUE;
		error = g_strdup("curl is not installed (Fedora package: curl)");
	} else {
		g_auto(GStrv) targv = NULL;
		g_auto(GStrv) pargv = NULL;

		targv = bar_assistant_curl_argv(curl, url, BAR_ASSISTANT_OP_TAGS,
		                                NULL, NULL);
		tags = bar_run_argv((const gchar * const *)targv);
		if (tags == NULL) {
			error = g_strdup("no answer");
		} else {
			installed = bar_assistant_parse_models(tags, &error);
			if (installed != NULL) {
				pargv = bar_assistant_curl_argv(curl, url,
					BAR_ASSISTANT_OP_PS, NULL, NULL);
				ps = bar_run_argv((const gchar * const *)pargv);
				loaded = bar_assistant_parse_models(ps, NULL);
				ok = TRUE;
			}
		}
	}

	/* nvidia-smi only when it is there: probing a missing command
	   costs a PATH walk, running one costs a fork. */
	smi = as_command(plugin, "nvidia-smi", "nvidia-smi");
	if (bar_have_command(smi)) {
		const gchar *argv[] = { smi,
			"--query-gpu=memory.total,memory.used",
			"--format=csv,noheader,nounits", NULL };

		nvidia = bar_run_argv(argv);
	}
	models_dir = as_models_dir(plugin);
	bar_assistant_read_machine(as_setting(plugin, "proc-root", "/proc"),
	                           as_setting(plugin, "sys-root", "/sys"),
	                           nvidia, models_dir, &machine);

	if (installed == NULL)
		installed = g_ptr_array_new_with_free_func(
			(GDestroyNotify)bar_assistant_model_free);
	if (loaded == NULL)
		loaded = g_ptr_array_new_with_free_func(
			(GDestroyNotify)bar_assistant_model_free);

	g_mutex_lock(&as->lock);
	g_ptr_array_unref(as->installed);
	as->installed = g_steal_pointer(&installed);
	g_ptr_array_unref(as->loaded);
	as->loaded = g_steal_pointer(&loaded);
	as->ollama_ok = ok;
	as->no_curl = no_curl;
	g_free(as->ollama_error);
	as->ollama_error = ok ? NULL : g_steal_pointer(&error);
	g_free(as->ollama_url_tried);
	as->ollama_url_tried = g_strdup(as_setting(plugin, "ollama-url",
	                                           "http://127.0.0.1:11434"));
	bar_assistant_machine_clear(&as->machine);
	as->machine = machine;
	as->have_machine = TRUE;
	as->refreshed_at = g_get_monotonic_time();
	as->refreshing = FALSE;
	g_mutex_unlock(&as->lock);

	gowl_bar_plugin_request_panel_refresh(plugin);
}

static void
as_refresh_work(GowlBarPlugin *plugin, gpointer user_data)
{
	as_refresh_now(plugin, user_data);
}

/* Queue a refresh unless one is running or the last was moments ago.
   Compositor thread; never blocks. */
static void
as_queue_refresh(GowlBarPlugin *plugin, Assistant *as, gboolean force)
{
	gint64 now = g_get_monotonic_time();

	g_mutex_lock(&as->lock);
	if (as->refreshing ||
	    (!force && as->refreshed_at != 0 &&
	     now - as->refreshed_at < AS_REFRESH_MIN_US)) {
		g_mutex_unlock(&as->lock);
		return;
	}
	as->refreshing = TRUE;
	g_mutex_unlock(&as->lock);
	gowl_bar_plugin_queue_work(plugin, as_refresh_work, as, NULL);
}

/* ----------------------------------------------------------------
 * Model operations (worker)
 * ---------------------------------------------------------------- */

typedef struct {
	Assistant      *as;
	BarAssistantOp  op;
	gchar         **models;   /* one, or every loaded one */
} OpJob;

static void
op_job_free(gpointer data)
{
	OpJob *job = data;

	g_strfreev(job->models);
	g_free(job);
}

static const gchar *
as_op_verb(BarAssistantOp op)
{
	switch (op) {
	case BAR_ASSISTANT_OP_LOAD:   return "Loaded";
	case BAR_ASSISTANT_OP_UNLOAD: return "Unloaded";
	case BAR_ASSISTANT_OP_DELETE: return "Deleted";
	default:                      return "Done";
	}
}

static void
as_op_work(GowlBarPlugin *plugin, gpointer user_data)
{
	OpJob *job = user_data;
	Assistant *as = job->as;
	g_autofree gchar *url = NULL;
	g_autofree gchar *status = NULL;
	guint i, failed = 0, done = 0;
	const gchar *first_failed = NULL;

	url = bar_assistant_normalise_url(as_setting(plugin, "ollama-url",
	                                             "http://127.0.0.1:11434"));
	for (i = 0; url != NULL && job->models[i] != NULL; i++) {
		g_auto(GStrv) argv = NULL;
		g_autofree gchar *out = NULL;

		argv = bar_assistant_curl_argv(
			as_command(plugin, "curl", "curl"), url, job->op,
			job->models[i], as_setting(plugin, "keep-alive", "30m"));
		if (argv != NULL)
			out = bar_run_argv((const gchar * const *)argv);
		if (out == NULL) {
			failed++;
			if (first_failed == NULL)
				first_failed = job->models[i];
		} else {
			done++;
		}
	}

	if (url == NULL)
		status = g_strdup("ollama-url must start with http:// or https://");
	else if (failed == 0 && done == 1)
		status = g_strdup_printf("%s %s.", as_op_verb(job->op),
		                         job->models[0]);
	else if (failed == 0)
		status = g_strdup_printf("%s %u models.", as_op_verb(job->op),
		                         done);
	else
		status = g_strdup_printf("ollama refused %s for %s.",
			(job->op == BAR_ASSISTANT_OP_LOAD) ? "the load"
			: (job->op == BAR_ASSISTANT_OP_DELETE) ? "the delete"
			: "the unload", first_failed);

	g_mutex_lock(&as->lock);
	as->op_busy = FALSE;
	as->op_failed = (failed > 0 || url == NULL);
	as->error_look = as->op_failed;
	g_free(as->op_status);
	as->op_status = g_steal_pointer(&status);
	if (job->op == BAR_ASSISTANT_OP_DELETE && failed == 0)
		g_clear_pointer(&as->selected, g_free);
	as->refreshing = TRUE;
	g_mutex_unlock(&as->lock);

	/* What changed is what the lists show; read them again now rather
	   than at the next interval. */
	as_refresh_now(plugin, as);
}

static void
as_start_op(GowlBarPlugin *plugin, Assistant *as, BarAssistantOp op,
            gchar **models)
{
	OpJob *job;

	g_mutex_lock(&as->lock);
	if (as->op_busy) {
		g_mutex_unlock(&as->lock);
		g_strfreev(models);
		return;
	}
	as->op_busy = TRUE;
	as->op_failed = FALSE;
	g_free(as->op_status);
	as->op_status = g_strdup_printf("%s %s...",
		(op == BAR_ASSISTANT_OP_LOAD) ? "Loading"
		: (op == BAR_ASSISTANT_OP_DELETE) ? "Deleting" : "Unloading",
		(models[0] != NULL && models[1] == NULL) ? models[0]
		                                         : "models");
	g_mutex_unlock(&as->lock);

	job = g_new0(OpJob, 1);
	job->as     = as;
	job->op     = op;
	job->models = models;
	gowl_bar_plugin_queue_work(plugin, as_op_work, job, op_job_free);
}

/* ----------------------------------------------------------------
 * Pulling a model (worker)
 * ---------------------------------------------------------------- */

typedef struct {
	Assistant *as;
	gchar     *model;
	guint      gen;
} PullJob;

static void
pull_job_free(gpointer data)
{
	PullJob *job = data;

	g_free(job->model);
	g_free(job);
}

static void
as_set_nonblock(gint fd)
{
	gint flags;

	flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Read one progress line into the plugin's state.  Returns FALSE when
   the line was an error, with @error set. */
static gboolean
as_pull_absorb(Assistant *as, guint gen, const gchar *line, gchar **error)
{
	BarAssistantPull pull;
	gboolean ok = TRUE;

	if (!bar_assistant_parse_pull_line(line, &pull))
		return TRUE;
	g_mutex_lock(&as->lock);
	if (as->pull_gen == gen) {
		if (pull.error != NULL) {
			ok = FALSE;
			*error = g_strdup(pull.error);
		} else {
			g_free(as->pull_status);
			as->pull_status = g_strdup(pull.status);
			if (pull.total > 0)
				as->pull_fraction = (gdouble)pull.completed /
				                    (gdouble)pull.total;
		}
	}
	g_mutex_unlock(&as->lock);
	bar_assistant_pull_clear(&pull);
	return ok;
}

/*
 * `POST /api/pull' streams one JSON object per line until `success'.
 * curl runs with -N so each line arrives as ollama sends it; the read
 * loop polls with a timeout so a Cancel --- which bumps the generation
 * and signals curl --- is noticed within half a second even when the
 * server has gone quiet.
 */
static void
as_pull_work(GowlBarPlugin *plugin, gpointer user_data)
{
	PullJob *job = user_data;
	Assistant *as = job->as;
	g_autofree gchar *url = NULL;
	g_auto(GStrv) argv = NULL;
	g_autoptr(GError) gerr = NULL;
	g_autofree gchar *error = NULL;
	GString *line;
	GPid pid = 0;
	gint out_fd = -1, status = 0;
	gboolean success = FALSE, cancelled = FALSE, failed = FALSE;
	gint64 last_refresh = 0;

	url = bar_assistant_normalise_url(as_setting(plugin, "ollama-url",
	                                             "http://127.0.0.1:11434"));
	if (url != NULL)
		argv = bar_assistant_curl_argv(as_command(plugin, "curl", "curl"),
		                               url, BAR_ASSISTANT_OP_PULL,
		                               job->model, NULL);
	if (argv == NULL ||
	    !g_spawn_async_with_pipes(NULL, argv, NULL,
	                              G_SPAWN_SEARCH_PATH |
	                              G_SPAWN_DO_NOT_REAP_CHILD |
	                              G_SPAWN_STDERR_TO_DEV_NULL,
	                              NULL, NULL, &pid, NULL, &out_fd, NULL,
	                              &gerr)) {
		g_mutex_lock(&as->lock);
		if (as->pull_gen == job->gen) {
			as->pulling = FALSE;
			as->error_look = TRUE;
			g_free(as->pull_status);
			as->pull_status = g_strdup_printf("Could not start the "
				"download: %s", (gerr != NULL) ? gerr->message
				: "bad ollama-url or model name");
		}
		g_mutex_unlock(&as->lock);
		gowl_bar_plugin_request_panel_refresh(plugin);
		return;
	}

	g_mutex_lock(&as->lock);
	if (as->pull_gen == job->gen)
		as->pull_pid = pid;
	g_mutex_unlock(&as->lock);

	as_set_nonblock(out_fd);
	line = g_string_new(NULL);
	while (out_fd >= 0) {
		struct pollfd pfd;
		gchar buf[4096];
		gssize n;
		gint rc;
		gboolean stale;

		g_mutex_lock(&as->lock);
		stale = (as->pull_gen != job->gen);
		g_mutex_unlock(&as->lock);
		if (stale) {
			kill(pid, SIGTERM);
			cancelled = TRUE;
			break;
		}

		pfd.fd = out_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		rc = poll(&pfd, 1, 500);
		if (rc < 0 && errno != EINTR)
			break;
		if (rc <= 0)
			continue;

		n = read(out_fd, buf, sizeof(buf));
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (n <= 0)
			break;

		g_string_append_len(line, buf, n);
		for (;;) {
			gchar *nl = memchr(line->str, '\n', line->len);
			g_autofree gchar *one = NULL;

			if (nl == NULL)
				break;
			one = g_strndup(line->str, (gsize)(nl - line->str));
			g_string_erase(line, 0, (gssize)(nl - line->str) + 1);
			if (!as_pull_absorb(as, job->gen, one, &error)) {
				failed = TRUE;
				break;
			}
			if (strstr(one, "\"success\"") != NULL) {
				BarAssistantPull p;

				if (bar_assistant_parse_pull_line(one, &p) &&
				    p.done)
					success = TRUE;
				bar_assistant_pull_clear(&p);
			}
		}
		if (failed)
			break;
		if (line->len > AS_MAX_LINE)
			g_string_truncate(line, 0);

		/* A few panel refreshes a second, not one per line. */
		if (g_get_monotonic_time() - last_refresh > 250 * 1000) {
			last_refresh = g_get_monotonic_time();
			gowl_bar_plugin_request_panel_refresh(plugin);
		}
	}
	g_string_free(line, TRUE);
	if (out_fd >= 0)
		close(out_fd);
	if (failed)
		kill(pid, SIGTERM);

	/* Reaped here, on the worker.  Never through a GLib child watch:
	   under cmacs that would fire on Emacs's thread. */
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	g_spawn_close_pid(pid);

	g_mutex_lock(&as->lock);
	if (as->pull_gen == job->gen) {
		as->pull_pid = 0;
		as->pulling = FALSE;
		g_free(as->pull_status);
		if (success) {
			as->pull_status = g_strdup_printf("Pulled %s.", job->model);
			as->error_look = FALSE;
		} else {
			as->pull_status = g_strdup_printf("Pull of %s failed: %s",
				job->model, (error != NULL) ? error
				: (WIFEXITED(status) && WEXITSTATUS(status) != 0)
					? "curl could not reach ollama"
					: "the download ended early");
			as->error_look = TRUE;
		}
	}
	g_mutex_unlock(&as->lock);

	if (!cancelled) {
		g_mutex_lock(&as->lock);
		as->refreshing = TRUE;
		g_mutex_unlock(&as->lock);
		as_refresh_now(plugin, as);
	} else {
		gowl_bar_plugin_request_panel_refresh(plugin);
	}
}

static void
as_start_pull(GowlBarPlugin *plugin, Assistant *as, const gchar *model)
{
	PullJob *job;

	g_mutex_lock(&as->lock);
	if (as->pulling) {
		g_mutex_unlock(&as->lock);
		return;
	}
	as->pulling = TRUE;
	as->pull_fraction = 0.0;
	as->pull_gen++;
	g_free(as->pull_model);
	as->pull_model = g_strdup(model);
	g_free(as->pull_status);
	as->pull_status = g_strdup("starting");
	job = g_new0(PullJob, 1);
	job->as    = as;
	job->model = g_strdup(model);
	job->gen   = as->pull_gen;
	g_mutex_unlock(&as->lock);

	gowl_bar_plugin_queue_work(plugin, as_pull_work, job, pull_job_free);
}

static void
as_cancel_pull(Assistant *as)
{
	GPid pid;

	g_mutex_lock(&as->lock);
	pid = as->pull_pid;
	as->pull_pid = 0;
	as->pull_gen++;
	if (as->pulling) {
		as->pulling = FALSE;
		g_free(as->pull_status);
		as->pull_status = g_strdup_printf("Cancelled the pull of %s.",
			(as->pull_model != NULL) ? as->pull_model : "the model");
	}
	g_mutex_unlock(&as->lock);

	/* Only the curl this widget started, by its PID.  The worker
	   notices the generation and reaps it. */
	if (pid > 0)
		kill(pid, SIGTERM);
}

/* ----------------------------------------------------------------
 * Approvals
 * ---------------------------------------------------------------- */

/* Arm the card for @op on @model, with what it will cost. */
static void
as_arm(GowlBarPlugin *plugin, Assistant *as, BarAssistantOp op,
       const gchar *model)
{
	g_autofree gchar *detail = NULL;
	g_autofree gchar *free_now = NULL;
	BarAssistantModel *m;

	(void)plugin;
	g_mutex_lock(&as->lock);
	free_now = as->have_machine && as->machine.disk_total > 0
		? bar_assistant_format_bytes(as->machine.disk_free)
		: g_strdup("unknown");
	m = as_find_model(as->installed, model);

	switch (op) {
	case BAR_ASSISTANT_OP_PULL:
		detail = g_strdup_printf("Download %s from the ollama library. "
			"Its size is known once the download starts; %s is free "
			"on the model disk now.", model, free_now);
		break;
	case BAR_ASSISTANT_OP_DELETE: {
		g_autofree gchar *size = NULL;
		g_autofree gchar *after = NULL;

		size  = bar_assistant_format_bytes((m != NULL) ? m->size : 0);
		after = (as->have_machine && as->machine.disk_total > 0)
			? bar_assistant_format_bytes(as->machine.disk_free +
			                             ((m != NULL) ? m->size : 0))
			: g_strdup("unknown");
		detail = g_strdup_printf("Delete %s (%s) from disk. Disk after: "
			"%s free.", model, size, after);
		break;
	}
	case BAR_ASSISTANT_OP_LOAD: {
		g_autofree gchar *size = NULL;
		BarAssistantFit fit;

		size = bar_assistant_format_bytes((m != NULL) ? m->size : 0);
		fit  = (m != NULL && as->have_machine)
			? bar_assistant_fit(m->size, &as->machine)
			: BAR_ASSISTANT_FIT_UNKNOWN;
		detail = g_strdup_printf("Load %s into memory (%s, fit: %s), "
			"kept for %s. Disk is unchanged: %s free.", model, size,
			bar_assistant_fit_label(fit),
			as_setting(plugin, "keep-alive", "30m"), free_now);
		break;
	}
	case BAR_ASSISTANT_OP_UNLOAD_ALL: {
		guint64 held = 0;
		g_autofree gchar *size = NULL;
		guint i;

		for (i = 0; i < as->loaded->len; i++)
			held += ((BarAssistantModel *)
			         g_ptr_array_index(as->loaded, i))->size;
		size = bar_assistant_format_bytes(held);
		detail = g_strdup_printf("Unload all %u loaded model%s, "
			"freeing about %s of memory. Nothing is deleted.",
			as->loaded->len, (as->loaded->len == 1) ? "" : "s", size);
		break;
	}
	case BAR_ASSISTANT_OP_NONE:
	case BAR_ASSISTANT_OP_TAGS:
	case BAR_ASSISTANT_OP_PS:
	case BAR_ASSISTANT_OP_UNLOAD:
	default:
		g_mutex_unlock(&as->lock);
		return;
	}

	bar_assistant_approval_arm(&as->approval, op, model, detail,
	                           g_get_monotonic_time());
	g_mutex_unlock(&as->lock);
}

/* Approve: run what the live card was for.  An expired card runs
   nothing. */
static void
as_approve(GowlBarPlugin *plugin, Assistant *as)
{
	g_autofree gchar *model = NULL;
	BarAssistantOp op;
	gchar **models = NULL;

	g_mutex_lock(&as->lock);
	op = bar_assistant_approval_take(&as->approval,
	                                 g_get_monotonic_time(), &model);
	if (op == BAR_ASSISTANT_OP_UNLOAD_ALL) {
		GPtrArray *names = g_ptr_array_new();
		guint i;

		for (i = 0; i < as->loaded->len; i++)
			g_ptr_array_add(names, g_strdup(((BarAssistantModel *)
				g_ptr_array_index(as->loaded, i))->name));
		g_ptr_array_add(names, NULL);
		models = (gchar **)g_ptr_array_free(names, FALSE);
	}
	g_mutex_unlock(&as->lock);

	switch (op) {
	case BAR_ASSISTANT_OP_PULL:
		as_start_pull(plugin, as, model);
		break;
	case BAR_ASSISTANT_OP_LOAD:
	case BAR_ASSISTANT_OP_DELETE: {
		gchar **one = g_new0(gchar *, 2);

		one[0] = g_steal_pointer(&model);
		as_start_op(plugin, as, op, one);
		break;
	}
	case BAR_ASSISTANT_OP_UNLOAD_ALL:
		if (models != NULL && models[0] != NULL) {
			as_start_op(plugin, as, BAR_ASSISTANT_OP_UNLOAD, models);
			models = NULL;
		}
		break;
	case BAR_ASSISTANT_OP_NONE:
	case BAR_ASSISTANT_OP_TAGS:
	case BAR_ASSISTANT_OP_PS:
	case BAR_ASSISTANT_OP_UNLOAD:
	default:
		break;
	}
	g_strfreev(models);
}

/* ----------------------------------------------------------------
 * The bar indicator
 * ---------------------------------------------------------------- */

static BarAssistantLook
as_compute_look(Assistant *as)
{
	BarAiState state;
	BarAssistantLook look;

	state = bar_ai_get(as->ai, NULL, NULL);
	g_mutex_lock(&as->lock);
	if (state == BAR_AI_STREAMING)
		look = BAR_ASSISTANT_LOOK_STREAMING;
	else if (state == BAR_AI_WAITING || as->pulling || as->op_busy)
		look = BAR_ASSISTANT_LOOK_THINKING;
	else if (as->error_look)
		look = BAR_ASSISTANT_LOOK_ERROR;
	else
		look = BAR_ASSISTANT_LOOK_IDLE;
	g_mutex_unlock(&as->lock);
	return look;
}

/*
 * One activity sample per poll.  Streaming: characters per second
 * against a brisk 400.  Thinking: a steady middle value the ripple
 * animates.  Idle: the busier of CPU and GPU, from the shared reader,
 * so the wave still says something about the machine at rest.
 */
static void
as_sample_activity(Assistant *as, BarAssistantLook look)
{
	g_autofree gchar *text = NULL;
	gint64 now = g_get_monotonic_time();
	gdouble v = 0.0;
	gsize len;

	bar_ai_get(as->ai, &text, NULL);
	len = (text != NULL) ? strlen(text) : 0;

	g_mutex_lock(&as->lock);
	if (look == BAR_ASSISTANT_LOOK_STREAMING && as->last_sample_at != 0 &&
	    now > as->last_sample_at) {
		gdouble secs = (gdouble)(now - as->last_sample_at) /
		               (gdouble)G_USEC_PER_SEC;
		gdouble rate = (len > as->last_len)
			? (gdouble)(len - as->last_len) / secs : 0.0;

		v = MAX(0.25, rate / 400.0);
	} else if (look == BAR_ASSISTANT_LOOK_THINKING) {
		v = 0.4;
	} else {
		const BarEnv *env = bar_env();

		if (env != NULL && env->sysinfo != NULL) {
			gint cpu = bar_sysinfo_cpu_percent(env->sysinfo);
			gint gpu = bar_sysinfo_gpu_percent(env->sysinfo);

			v = (gdouble)MAX(cpu, gpu) / 100.0;
		}
	}
	as->last_len = len;
	as->last_sample_at = now;

	if (as->n_samples == AS_SAMPLES) {
		memmove(as->samples, as->samples + 1,
		        (AS_SAMPLES - 1) * sizeof(gdouble));
		as->n_samples--;
	}
	as->samples[as->n_samples++] = CLAMP(v, 0.0, 1.0);
	if (look != BAR_ASSISTANT_LOOK_IDLE &&
	    look != BAR_ASSISTANT_LOOK_ERROR)
		as->phase += 0.9;
	g_mutex_unlock(&as->lock);
}

static void
as_apply_look(GowlBarPlugin *plugin, Assistant *as)
{
	BarAssistantLook look;
	gboolean animate;
	guint bars;
	g_autofree gchar *tip = NULL;
	g_autofree gchar *label = NULL;
	const gchar *provider;
	const gchar *model;

	look = as_compute_look(as);
	as_sample_activity(as, look);

	/* The robot carries a small three-bar wave while it works; the
	   wave style is all bars. */
	bars = as_wave_style(plugin)
		? (guint)CLAMP(gowl_bar_plugin_get_setting_int(plugin, "bars",
		                                              5), 3, 12)
		: AS_MINI_BARS;
	animate = (look == BAR_ASSISTANT_LOOK_THINKING ||
	           look == BAR_ASSISTANT_LOOK_STREAMING);

	g_mutex_lock(&as->lock);
	as->look = look;
	as->n_heights = bars;
	bar_assistant_wave(as->samples, as->n_samples, bars, as->phase,
	                   animate, as->heights);
	g_mutex_unlock(&as->lock);

	bar_plugin_apply_color(plugin,
		gowl_bar_plugin_get_setting(plugin,
			bar_assistant_look_color_key(look)),
		as_look_role(look));

	/* The robot, unless the bar is all wave. */
	gowl_bar_plugin_set_icon(plugin,
		as_wave_style(plugin) ? NULL : AS_GLYPH);

	provider = as_provider(plugin);
	model = as_shown_model(plugin);
	if (gowl_bar_plugin_get_setting_bool(plugin, "show-label", FALSE)) {
		label = (model != NULL)
			? g_strdup_printf("%s/%s",
			                  (provider != NULL) ? provider : "ai", model)
			: g_strdup((provider != NULL) ? provider : "AI");
		if (g_utf8_strlen(label, -1) > 24) {
			gchar *cut = g_utf8_substring(label, 0, 23);

			g_free(label);
			label = g_strconcat(cut, "\xe2\x80\xa6", NULL);
			g_free(cut);
		}
	}
	gowl_bar_plugin_set_label(plugin, label);

	tip = g_strdup_printf("AI assistant: %s (%s%s%s)",
		(look == BAR_ASSISTANT_LOOK_THINKING) ? "thinking"
		: (look == BAR_ASSISTANT_LOOK_STREAMING) ? "answering"
		: (look == BAR_ASSISTANT_LOOK_ERROR) ? "the last request failed"
		: "idle",
		(provider != NULL) ? provider : "default provider",
		(model != NULL) ? " / " : "", (model != NULL) ? model : "");
	gowl_bar_plugin_set_tooltip(plugin, tip);
}

static gint
as_interval(GowlBarPlugin *plugin, gpointer data)
{
	Assistant *as = data;
	gboolean busy;

	busy = bar_ai_is_busy(as->ai);
	g_mutex_lock(&as->lock);
	busy = busy || as->pulling || as->op_busy || as->pending_id != NULL;
	g_mutex_unlock(&as->lock);
	return busy ? 1 : as_idle_interval(plugin);
}

/* Cheap, compositor thread: no subprocess is reachable from here (the
   source guard checks). */
static void
as_poll(GowlBarPlugin *plugin, gpointer data)
{
	Assistant *as = data;

	if (as_sync_answer(plugin, as))
		gowl_bar_plugin_request_panel_refresh(plugin);
	as_apply_look(plugin, as);
}

/*
 * Worker: the provider list for the standalone route, once per `ai'
 * command, and the ollama/machine refresh while somebody is looking
 * at it --- the panel is open, or `machine-context' needs a current
 * reading to send.
 */
static void
as_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	Assistant *as = data;
	const gchar *cmd;
	gboolean cli, want_probe, want_refresh;
	gint64 now = g_get_monotonic_time();

	cmd = as_setting(plugin, "ai-command", "ai");
	/* Outside the lock: under `auto' this walks PATH. */
	cli = (bar_ai_backend_for(plugin) == BAR_AI_BACKEND_CLI);
	g_mutex_lock(&as->lock);
	want_probe = (cli &&
	              as_setting(plugin, "ai-providers", NULL) == NULL &&
	              g_strcmp0(as->cli_providers_for, cmd) != 0);
	/* The chat tab only needs ollama's list for ollama's model
	   chooser; the other two tabs show it. */
	want_refresh = (!as->refreshing &&
	                ((as->panel_open &&
	                  (as->tab != AS_TAB_CHAT ||
	                   g_strcmp0(as_provider(plugin), "ollama") == 0)) ||
	                 gowl_bar_plugin_get_setting_bool(plugin,
	                     "machine-context", FALSE)) &&
	                (as->refreshed_at == 0 ||
	                 now - as->refreshed_at >= AS_REFRESH_MIN_US));
	if (want_refresh)
		as->refreshing = TRUE;
	g_mutex_unlock(&as->lock);

	if (want_probe) {
		const gchar *argv[] = { cmd, "--list-providers", NULL };
		g_autofree gchar *out = NULL;
		gchar **names;

		out = bar_run_argv(argv);
		names = bar_assistant_parse_provider_listing(out);
		g_mutex_lock(&as->lock);
		g_strfreev(as->cli_providers);
		as->cli_providers = names;
		g_free(as->cli_providers_for);
		as->cli_providers_for = g_strdup(cmd);
		g_mutex_unlock(&as->lock);
		gowl_bar_plugin_request_panel_refresh(plugin);
	}

	if (want_refresh)
		as_refresh_now(plugin, as);
}

/* cmacs pushes its answers and its provider list as settings; the bar
   runs this after every push. */
static void
as_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	Assistant *as = data;
	gboolean changed;

	(void)settings;
	changed = bar_ai_absorb_settings(as->ai, plugin);
	changed = as_sync_answer(plugin, as) || changed;
	as_apply_look(plugin, as);
	if (changed)
		gowl_bar_plugin_request_panel_refresh(plugin);
	gowl_bar_plugin_request_redraw(plugin);
}

/* ── Measure and draw ─────────────────────────────────────────────── */

/* The wave's geometry for a bar @height pixels tall. */
static void
as_wave_metrics(gint height, gint *bar_w, gint *gap)
{
	*bar_w = MAX(2, height / 8);
	*gap   = MAX(1, *bar_w / 2 + 1);
}

static gboolean
as_reduced(GowlBarPlugin *plugin)
{
	return gowl_bar_plugin_get_setting_bool(plugin, "reduced-motion",
	                                        FALSE);
}

/* `icon-style: wave', the equaliser on its own.  reduced-motion wins:
   it asks for no moving parts at all. */
static gboolean
as_wave_style(GowlBarPlugin *plugin)
{
	return !as_reduced(plugin) &&
	       g_strcmp0(gowl_bar_plugin_get_setting(plugin, "icon-style"),
	                 "wave") == 0;
}

/* Whether the robot shows its small wave: only while the model works,
   and never under reduced-motion.  Compositor thread. */
static gboolean
as_robot_busy(GowlBarPlugin *plugin, Assistant *as)
{
	BarAssistantLook look;

	if (as_reduced(plugin) || as_wave_style(plugin))
		return FALSE;
	g_mutex_lock(&as->lock);
	look = as->look;
	g_mutex_unlock(&as->lock);
	return look == BAR_ASSISTANT_LOOK_THINKING ||
	       look == BAR_ASSISTANT_LOOK_STREAMING;
}

/* The small wave's width beside the robot, for a bar @height tall. */
static gint
as_mini_width(const GowlBarTheme *theme, gint height)
{
	gint bar_w, gap;

	as_wave_metrics(height, &bar_w, &gap);
	return gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_ITEM_PAD) / 2 +
	       AS_MINI_BARS * (bar_w + gap) - gap;
}

static gint
as_measure(GowlBarPlugin *plugin, gpointer data, PangoLayout *layout,
           const GowlBarTheme *theme, gint height)
{
	Assistant *as = data;
	g_autofree gchar *label = NULL;
	gint pad, bar_w, gap, width;
	guint bars;

	/* The robot: the ordinary icon (and label), plus the small wave
	   while the model works.  The bar re-measures when the signature
	   changes, which it does when the look does. */
	if (!as_wave_style(plugin))
		return gowl_bar_plugin_measure_label(plugin, layout, theme,
		                                     height) +
		       (as_robot_busy(plugin, as)
		        ? as_mini_width(theme, height) : 0);

	g_mutex_lock(&as->lock);
	bars = as->n_heights;
	g_mutex_unlock(&as->lock);

	pad = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_ITEM_PAD);
	as_wave_metrics(height, &bar_w, &gap);
	width = 2 * pad + (gint)bars * (bar_w + gap) - gap;

	label = gowl_bar_plugin_dup_label(plugin);
	if (label != NULL && label[0] != '\0') {
		PangoFontDescription *desc;
		gint tw = 0;

		desc = pango_font_description_from_string(
			gowl_bar_theme_get_font(theme));
		pango_layout_set_font_description(layout, desc);
		pango_layout_set_width(layout, -1);
		pango_layout_set_attributes(layout, NULL);
		/* Plain text: a model id is never markup. */
		pango_layout_set_text(layout, label, -1);
		pango_layout_get_pixel_size(layout, &tw, NULL);
		pango_font_description_free(desc);
		width += pad / 2 + 1 + tw;
	}
	return width;
}

static void
as_draw(GowlBarPlugin *plugin, gpointer data, cairo_t *cr,
        PangoLayout *layout, const GowlBarTheme *theme,
        gint x, gint y, gint width, gint height,
        gboolean hovered, gboolean panel_open)
{
	Assistant *as = data;
	gdouble heights[16];
	g_autofree gchar *label = NULL;
	gint pad, radius, bar_w, gap, max_h, cx, center;
	guint bars, i;

	pad    = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_ITEM_PAD);
	radius = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_RADIUS);

	if (!as_wave_style(plugin)) {
		gint mini = as_robot_busy(plugin, as)
			? as_mini_width(theme, height) : 0;

		if (mini == 0) {
			gowl_bar_plugin_draw_label(plugin, cr, layout, theme,
			                           x, y, width, height, hovered,
			                           panel_open);
			return;
		}
		/* The hover wash and open-panel rule span the robot and
		   its wave alike, so they are drawn here for the whole
		   width and the robot is drawn plain. */
		if (hovered || panel_open) {
			gowl_bar_cairo_rounded_rect(cr, x + 1, y + 2, width - 2,
			                            height - 4, radius * 0.7);
			gowl_bar_theme_cairo_set_alpha(theme, cr,
				GOWL_BAR_COLOR_SURFACE,
				panel_open ? 0.95 : 0.55);
			cairo_fill(cr);
		}
		if (panel_open) {
			gowl_bar_theme_cairo_set(theme, cr,
			                         GOWL_BAR_COLOR_ACCENT);
			cairo_rectangle(cr, x + pad / 2, y + height - 3,
			                width - pad, 2);
			cairo_fill(cr);
		}
		gowl_bar_plugin_draw_label(plugin, cr, layout, theme, x, y,
		                           width - mini, height, FALSE, FALSE);

		g_mutex_lock(&as->lock);
		bars = MIN(as->n_heights, G_N_ELEMENTS(heights));
		memcpy(heights, as->heights, bars * sizeof(gdouble));
		g_mutex_unlock(&as->lock);

		/* The wave tucks in after the robot, inside the right pad. */
		as_wave_metrics(height, &bar_w, &gap);
		max_h  = MAX(4, (height * 2) / 5);
		center = y + height / 2;
		cx     = x + width - mini - pad / 2;
		gowl_bar_plugin_cairo_set_color(plugin, theme, cr);
		for (i = 0; i < bars; i++) {
			gint h = MAX(2, (gint)lround(heights[i] *
			                             (gdouble)max_h));

			cairo_rectangle(cr, cx, center - h / 2, bar_w, h);
			cx += bar_w + gap;
		}
		cairo_fill(cr);
		return;
	}

	/* The same hover wash and open-panel rule as every other widget. */
	if (hovered || panel_open) {
		gowl_bar_cairo_rounded_rect(cr, x + 1, y + 2, width - 2,
		                            height - 4, radius * 0.7);
		gowl_bar_theme_cairo_set_alpha(theme, cr, GOWL_BAR_COLOR_SURFACE,
		                               panel_open ? 0.95 : 0.55);
		cairo_fill(cr);
	}
	if (panel_open) {
		gowl_bar_theme_cairo_set(theme, cr, GOWL_BAR_COLOR_ACCENT);
		cairo_rectangle(cr, x + pad / 2, y + height - 3, width - pad, 2);
		cairo_fill(cr);
	}

	g_mutex_lock(&as->lock);
	bars = MIN(as->n_heights, G_N_ELEMENTS(heights));
	memcpy(heights, as->heights, bars * sizeof(gdouble));
	g_mutex_unlock(&as->lock);

	/* A row of pixel bars around the vertical centre, an equaliser. */
	as_wave_metrics(height, &bar_w, &gap);
	max_h  = MAX(4, (height * 3) / 5);
	center = y + height / 2;
	cx     = x + pad;
	gowl_bar_plugin_cairo_set_color(plugin, theme, cr);
	for (i = 0; i < bars; i++) {
		gint h = MAX(2, (gint)lround(heights[i] * (gdouble)max_h));

		cairo_rectangle(cr, cx, center - h / 2, bar_w, h);
		cx += bar_w + gap;
	}
	cairo_fill(cr);

	label = gowl_bar_plugin_dup_label(plugin);
	if (label != NULL && label[0] != '\0') {
		PangoFontDescription *desc;
		gint tw = 0, th = 0;

		desc = pango_font_description_from_string(
			gowl_bar_theme_get_font(theme));
		pango_layout_set_font_description(layout, desc);
		pango_layout_set_width(layout, -1);
		pango_layout_set_attributes(layout, NULL);
		pango_layout_set_text(layout, label, -1);
		pango_layout_get_pixel_size(layout, &tw, &th);
		cairo_move_to(cr, cx - gap + pad / 2 + 1, center - th / 2);
		pango_cairo_show_layout(cr, layout);
		pango_font_description_free(desc);
	}
}

/* The wave is not the label, icon or colour, so the bar has to be told
   when it moved or it will not repaint. */
static void
as_signature(GowlBarPlugin *plugin, gpointer data, GString *out)
{
	Assistant *as = data;
	guint i;

	g_string_append_printf(out, "|%d%d", as_reduced(plugin) ? 1 : 0,
	                       as_wave_style(plugin) ? 1 : 0);
	g_mutex_lock(&as->lock);
	g_string_append_printf(out, "|%s|", bar_assistant_look_name(as->look));
	for (i = 0; i < as->n_heights && i < G_N_ELEMENTS(as->heights); i++)
		g_string_append_printf(out, "%d,",
		                       (gint)lround(as->heights[i] * 20.0));
	g_mutex_unlock(&as->lock);
}

/* ----------------------------------------------------------------
 * The panel
 * ---------------------------------------------------------------- */

/* The approval card, if one is live.  Caller holds the lock. */
static void
as_panel_approval(GowlBarPanel *panel, Assistant *as)
{
	GowlBarPanelItem *item;

	if (!bar_assistant_approval_live(&as->approval,
	                                 g_get_monotonic_time()))
		return;
	gowl_bar_panel_add_section(panel, "Confirm");
	item = gowl_bar_panel_add_label(panel, as->approval.detail);
	gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
	item = gowl_bar_panel_add_buttons(panel, "approval");
	gowl_bar_panel_add_button(item, "Approve", TRUE);
	gowl_bar_panel_add_button(item, "Cancel", FALSE);
	gowl_bar_panel_add_label(panel, "Expires in a minute.");
	gowl_bar_panel_add_separator(panel);
}

/* The models the chat's model chooser offers for @provider.  Caller
   holds the lock. */
static gchar **
as_models_for(GowlBarPlugin *plugin, Assistant *as, const gchar *provider)
{
	if (g_strcmp0(provider, "ollama") == 0 && as->installed->len > 0) {
		GPtrArray *names = g_ptr_array_new();
		guint i;

		for (i = 0; i < as->installed->len; i++)
			g_ptr_array_add(names, g_strdup(((BarAssistantModel *)
				g_ptr_array_index(as->installed, i))->name));
		g_ptr_array_add(names, NULL);
		return (gchar **)g_ptr_array_free(names, FALSE);
	}
	if (provider != NULL) {
		g_autofree gchar *key = g_strdup_printf("models.%s", provider);

		return bar_assistant_split_list(
			gowl_bar_plugin_get_setting(plugin, key), TRUE);
	}
	return g_new0(gchar *, 1);
}

/* The providers the chooser offers: what cmacs pushed or the user
   configured, else what `ai --list-providers' said.  Caller holds the
   lock. */
static gchar **
as_providers(GowlBarPlugin *plugin, Assistant *as)
{
	const gchar *pushed;

	pushed = gowl_bar_plugin_get_setting(plugin, "ai-providers");
	if (pushed != NULL && pushed[0] != '\0')
		return bar_assistant_split_list(pushed, FALSE);
	if (as->cli_providers != NULL)
		return g_strdupv(as->cli_providers);
	return g_new0(gchar *, 1);
}

static void
as_panel_chat(GowlBarPlugin *plugin, Assistant *as, GowlBarPanel *panel,
              BarAiBackend backend)
{
	GowlBarPanelItem *item;
	const gchar *provider, *model, *shown;
	gchar rev[16];
	GowlBarColor user_role, ai_role;
	g_autofree gchar *pending_text = NULL;
	BarAiState state;
	gboolean busy, clear_armed;
	guint i, n;

	provider = as_provider(plugin);
	model    = as_setting(plugin, "ai-model", NULL);
	shown    = as_shown_model(plugin);
	state    = bar_ai_get(as->ai, &pending_text, NULL);
	busy     = (state == BAR_AI_WAITING || state == BAR_AI_STREAMING);

	if (backend == BAR_AI_BACKEND_NONE) {
		item = gowl_bar_panel_add_label(panel,
			"No AI backend. Standalone, install ai-glib's `ai' "
			"command (its providers and keys come from "
			"~/.config/ai-glib/config.yaml); under cmacs --gowl this "
			"widget uses cmacs-ai.");
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
	}

	/* The two choosers, each a row that opens its list below it. */
	item = gowl_bar_panel_add_row(panel, "pick:provider", "\xf3\xb0\x9a\xa9",
	                              "Provider", NULL);
	gowl_bar_panel_item_set_value(item, (provider != NULL) ? provider
	                                                       : "default");
	gowl_bar_panel_item_set_active(item, as->pick == AS_PICK_PROVIDER);
	if (as->pick == AS_PICK_PROVIDER) {
		g_auto(GStrv) names = as_providers(plugin, as);

		item = gowl_bar_panel_add_row(panel, "provider:", NULL,
		                              "Default", "the backend's own choice");
		gowl_bar_panel_item_set_selected(item, provider == NULL);
		for (i = 0; names[i] != NULL; i++) {
			g_autofree gchar *id = g_strconcat("provider:", names[i],
			                                   NULL);

			item = gowl_bar_panel_add_row(panel, id, NULL, names[i],
			                              NULL);
			gowl_bar_panel_item_set_selected(item,
				g_strcmp0(provider, names[i]) == 0);
		}
		if (names[0] == NULL)
			gowl_bar_panel_add_label(panel,
				"No provider list yet: cmacs pushes one, and "
				"standalone it comes from `ai --list-providers'.");
	}

	item = gowl_bar_panel_add_row(panel, "pick:model", "\xf3\xb0\xa7\x91",
	                              "Model", NULL);
	gowl_bar_panel_item_set_value(item, (shown != NULL) ? shown : "default");
	gowl_bar_panel_item_set_active(item, as->pick == AS_PICK_MODEL);
	if (as->pick == AS_PICK_MODEL) {
		g_auto(GStrv) names = as_models_for(plugin, as, provider);

		item = gowl_bar_panel_add_row(panel, "model:", NULL,
		                              "Provider default", NULL);
		gowl_bar_panel_item_set_selected(item, model == NULL);
		for (i = 0; names[i] != NULL && i < 64; i++) {
			g_autofree gchar *id = g_strconcat("model:", names[i],
			                                   NULL);

			item = gowl_bar_panel_add_row(panel, id, NULL, names[i],
			                              NULL);
			gowl_bar_panel_item_set_selected(item,
				g_strcmp0(model, names[i]) == 0);
		}
		item = gowl_bar_panel_add_entry(panel, "model-entry",
			"Any model id, then Return", NULL);
		gowl_bar_panel_item_set_title(item, "Other model");
	}

	gowl_bar_panel_add_separator(panel);

	/* The question.  Return is a newline; Ctrl+Return sends. */
	item = gowl_bar_panel_add_entry(panel, "prompt",
		"Ask anything (Ctrl+Return sends)", as->draft);
	gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_MULTILINE,
	                             "true");
	if (as->pick == AS_PICK_NONE)
		gowl_bar_panel_item_set_prop(item,
			GOWL_BAR_ENTRY_PROP_AUTOFOCUS, "true");
	g_snprintf(rev, sizeof(rev), "%u", as->entry_rev);
	gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_REVISION, rev);
	gowl_bar_panel_item_set_height(item, 72);

	clear_armed = (as->clear_armed_at != 0 &&
	               g_get_monotonic_time() - as->clear_armed_at <
	               AS_CLEAR_ARM_US);
	item = gowl_bar_panel_add_buttons(panel, "chat");
	gowl_bar_panel_add_button(item, "Send", !busy);
	gowl_bar_panel_add_button(item, "Stop", busy);
	gowl_bar_panel_add_button(item, clear_armed ? "Click again to clear"
	                                            : "Clear", FALSE);
	gowl_bar_panel_add_button(item, "Copy", FALSE);
	if (backend == BAR_AI_BACKEND_CMACS)
		gowl_bar_panel_add_button(item, "In cmacs", FALSE);

	if (as->chat_note != NULL) {
		item = gowl_bar_panel_add_label(panel, as->chat_note);
		gowl_bar_panel_item_set_color(item, as->chat_note_is_error
			? GOWL_BAR_COLOR_RED : GOWL_BAR_COLOR_MUTED);
	}

	/*
	 * The conversation, NEWEST FIRST.  The question field is at the
	 * top because the panel opens scrolled to the top with the
	 * keyboard in that field; putting the latest answer right under
	 * it means nobody scrolls to read what they just asked for.
	 *
	 * Every message is a plain LABEL --- pango text, never markup ---
	 * so `<b>' or `&amp;' in a model's answer is shown as written.
	 */
	user_role = as_role_setting(plugin, "color-user", GOWL_BAR_COLOR_BLUE);
	ai_role   = as_role_setting(plugin, "color-assistant",
	                            GOWL_BAR_COLOR_TEXT);
	n = bar_assistant_transcript_len(as->transcript);
	if (as->pending_id != NULL || n > 0)
		gowl_bar_panel_add_section(panel, "Conversation");
	if (as->pending_id != NULL) {
		g_autofree gchar *shown = NULL;
		g_autofree gchar *valid = g_utf8_make_valid(
			(pending_text != NULL) ? pending_text : "", -1);

		shown = (valid[0] != '\0')
			? g_strconcat("AI: ", valid, NULL)
			: g_strdup("AI: thinking\xe2\x80\xa6");
		item = gowl_bar_panel_add_label(panel, shown);
		gowl_bar_panel_item_set_color(item, ai_role);
	}
	for (i = n; i > 0; i--) {
		const BarAssistantTurn *turn;
		g_autofree gchar *shown = NULL;

		turn = bar_assistant_transcript_get(as->transcript, i - 1);
		shown = g_strconcat((turn->role == BAR_ASSISTANT_ROLE_USER)
		                    ? "You: " : "AI: ", turn->text, NULL);
		item = gowl_bar_panel_add_label(panel, shown);
		gowl_bar_panel_item_set_color(item,
			(turn->role == BAR_ASSISTANT_ROLE_USER) ? user_role
			                                        : ai_role);
	}
}

static void
as_panel_models(GowlBarPlugin *plugin, Assistant *as, GowlBarPanel *panel)
{
	GowlBarPanelItem *item;
	guint i;

	as_panel_approval(panel, as);

	if (as->pulling || as->pull_status != NULL) {
		g_autofree gchar *title = NULL;

		if (as->pulling) {
			gchar pct[G_ASCII_DTOSTR_BUF_SIZE];

			g_ascii_formatd(pct, sizeof(pct), "%.0f",
			                as->pull_fraction * 100.0);
			title = g_strdup_printf("Pulling %s: %s %s%%",
				(as->pull_model != NULL) ? as->pull_model : "",
				(as->pull_status != NULL) ? as->pull_status : "",
				pct);
			gowl_bar_panel_add_progress(panel, title,
			                            as->pull_fraction);
			item = gowl_bar_panel_add_buttons(panel, "pull-cancel");
			gowl_bar_panel_add_button(item, "Cancel the pull", FALSE);
		} else {
			item = gowl_bar_panel_add_label(panel, as->pull_status);
			gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_MUTED);
		}
	}
	if (as->op_status != NULL) {
		item = gowl_bar_panel_add_label(panel, as->op_status);
		gowl_bar_panel_item_set_color(item, as->op_failed
			? GOWL_BAR_COLOR_RED : GOWL_BAR_COLOR_MUTED);
	}

	if (!as->ollama_ok) {
		g_autofree gchar *why = NULL;

		if (as->refreshed_at == 0) {
			gowl_bar_panel_add_label(panel, "Asking ollama\xe2\x80\xa6");
			return;
		}
		if (as->no_curl) {
			why = g_strdup("curl is not installed. On Fedora: the "
			               "`curl' package.");
		} else {
			why = g_strdup_printf("Could not reach ollama at %s (%s). "
				"ollama is not packaged in Fedora: run it as a podman "
				"quadlet container (the widget's documentation has "
				"one) and make sure it listens there, or point "
				"ai-assistant.ollama-url at where it does.",
				(as->ollama_url_tried != NULL) ? as->ollama_url_tried
				                               : "?",
				(as->ollama_error != NULL) ? as->ollama_error
				                           : "no answer");
		}
		item = gowl_bar_panel_add_label(panel, why);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
		item = gowl_bar_panel_add_buttons(panel, "refresh");
		gowl_bar_panel_add_button(item, "Try again", FALSE);
		return;
	}

	/* What there is to fit into. */
	if (as->have_machine) {
		g_autofree gchar *vram = NULL;
		g_autofree gchar *ram = NULL;
		g_autofree gchar *disk = NULL;

		vram = (as->machine.gpu_kind != NULL)
			? bar_assistant_format_bytes(
				(as->machine.vram_total > as->machine.vram_used)
				? as->machine.vram_total - as->machine.vram_used : 0)
			: g_strdup("no GPU");
		ram  = bar_assistant_format_bytes(as->machine.mem_avail);
		disk = (as->machine.disk_total > 0)
			? bar_assistant_format_bytes(as->machine.disk_free)
			: g_strdup("?");
		gowl_bar_panel_add_field_pair(panel, "VRAM free", vram,
		                              "RAM free", ram);
		gowl_bar_panel_add_field(panel, "Disk free for models", disk);
	}

	if (as->loaded->len > 0) {
		gowl_bar_panel_add_section(panel, "Loaded");
		for (i = 0; i < as->loaded->len; i++) {
			BarAssistantModel *m = g_ptr_array_index(as->loaded, i);
			g_autofree gchar *id = g_strconcat("loaded:", m->name, NULL);
			g_autofree gchar *size = bar_assistant_format_bytes(m->size);
			g_autofree gchar *vram = bar_assistant_format_bytes(
				m->size_vram);
			g_autofree gchar *sub = g_strdup_printf("%s, %s in VRAM",
			                                        size, vram);

			item = gowl_bar_panel_add_row(panel, id, NULL, m->name, sub);
			gowl_bar_panel_item_set_value(item, "unload");
			gowl_bar_panel_item_set_value_color(item,
				GOWL_BAR_COLOR_MUTED);
		}
	}

	{
		g_autofree gchar *head = g_strdup_printf("Installed (%u)",
		                                         as->installed->len);

		gowl_bar_panel_add_section(panel, head);
	}
	if (as->installed->len == 0)
		gowl_bar_panel_add_label(panel,
			"No models yet. Add one below, e.g. qwen3:8b.");
	for (i = 0; i < as->installed->len; i++) {
		BarAssistantModel *m = g_ptr_array_index(as->installed, i);
		g_autofree gchar *id = g_strconcat("inst:", m->name, NULL);
		g_autofree gchar *size = bar_assistant_format_bytes(m->size);
		GString *sub = g_string_new(size);
		BarAssistantFit fit;
		gboolean is_loaded;

		if (m->family != NULL && m->family[0] != '\0')
			g_string_append_printf(sub, " \xc2\xb7 %s", m->family);
		if (m->params != NULL && m->params[0] != '\0')
			g_string_append_printf(sub, " \xc2\xb7 %s", m->params);
		if (m->quant != NULL && m->quant[0] != '\0' &&
		    g_strcmp0(m->quant, "unknown") != 0)
			g_string_append_printf(sub, " \xc2\xb7 %s", m->quant);

		is_loaded = (as_find_model(as->loaded, m->name) != NULL);
		fit = as->have_machine ? bar_assistant_fit(m->size, &as->machine)
		                       : BAR_ASSISTANT_FIT_UNKNOWN;

		item = gowl_bar_panel_add_row(panel, id, NULL, m->name, sub->str);
		gowl_bar_panel_item_set_value(item, is_loaded ? "loaded"
		                              : bar_assistant_fit_label(fit));
		gowl_bar_panel_item_set_value_color(item, is_loaded
			? GOWL_BAR_COLOR_ACCENT : as_fit_role(fit));
		gowl_bar_panel_item_set_selected(item,
			g_strcmp0(as->selected, m->name) == 0);
		g_string_free(sub, TRUE);

		if (g_strcmp0(as->selected, m->name) == 0) {
			item = gowl_bar_panel_add_buttons(panel, "model-actions");
			gowl_bar_panel_add_button(item, "Load", FALSE);
			gowl_bar_panel_add_button(item, "Unload", FALSE);
			gowl_bar_panel_add_button(item, "Delete", FALSE);
			gowl_bar_panel_add_button(item, "Chat with it", FALSE);
		}
	}

	gowl_bar_panel_add_separator(panel);
	item = gowl_bar_panel_add_entry(panel, "pull-entry",
		"name:tag, e.g. qwen3:8b, then Return", NULL);
	gowl_bar_panel_item_set_title(item, "Add model");
	item = gowl_bar_panel_add_buttons(panel, "refresh");
	gowl_bar_panel_add_button(item, "Refresh", FALSE);
	(void)plugin;
}

static void
as_panel_machine(GowlBarPlugin *plugin, Assistant *as, GowlBarPanel *panel)
{
	const BarEnv *env = bar_env();
	GowlBarPanelItem *item;
	const BarAssistantMachine *m = &as->machine;
	gchar l1[G_ASCII_DTOSTR_BUF_SIZE];
	gchar l5[G_ASCII_DTOSTR_BUF_SIZE];
	gchar l15[G_ASCII_DTOSTR_BUF_SIZE];
	g_autofree gchar *cpu = NULL;
	g_autofree gchar *load = NULL;
	guint i;

	(void)plugin;
	as_panel_approval(panel, as);

	if (!as->have_machine) {
		gowl_bar_panel_add_label(panel, "Reading the machine\xe2\x80\xa6");
		return;
	}

	/* CPU and network rates need two samples; the bar's shared reader
	   has them, a one-shot read does not. */
	if (env != NULL && env->sysinfo != NULL)
		cpu = g_strdup_printf("%d%% of %u threads",
		                      bar_sysinfo_cpu_percent(env->sysinfo),
		                      m->cores);
	else
		cpu = g_strdup_printf("%u threads", m->cores);
	g_ascii_formatd(l1, sizeof(l1), "%.2f", m->load[0]);
	g_ascii_formatd(l5, sizeof(l5), "%.2f", m->load[1]);
	g_ascii_formatd(l15, sizeof(l15), "%.2f", m->load[2]);
	load = g_strdup_printf("%s %s %s", l1, l5, l15);
	gowl_bar_panel_add_field_pair(panel, "CPU", cpu, "Load", load);

	{
		g_autofree gchar *used = bar_assistant_format_bytes(
			(m->mem_total > m->mem_avail) ? m->mem_total - m->mem_avail
			                              : 0);
		g_autofree gchar *total = bar_assistant_format_bytes(m->mem_total);
		g_autofree gchar *avail = bar_assistant_format_bytes(m->mem_avail);
		g_autofree gchar *mem = g_strdup_printf("%s / %s", used, total);

		gowl_bar_panel_add_field_pair(panel, "Memory", mem,
		                              "Available", avail);
	}

	if (m->gpu_kind != NULL) {
		g_autofree gchar *used = bar_assistant_format_bytes(m->vram_used);
		g_autofree gchar *total = bar_assistant_format_bytes(m->vram_total);
		g_autofree gchar *gpu = NULL;

		if (env != NULL && env->sysinfo != NULL &&
		    bar_sysinfo_gpu_percent(env->sysinfo) >= 0)
			gpu = g_strdup_printf("%s, %s / %s VRAM, %d%% busy",
				m->gpu_kind, used, total,
				bar_sysinfo_gpu_percent(env->sysinfo));
		else
			gpu = g_strdup_printf("%s, %s / %s VRAM", m->gpu_kind,
			                      used, total);
		gowl_bar_panel_add_field(panel, "GPU", gpu);
	} else {
		gowl_bar_panel_add_field(panel, "GPU",
		                         "none with its own memory");
	}

	if (m->disk_total > 0) {
		g_autofree gchar *free_text = bar_assistant_format_bytes(
			m->disk_free);
		g_autofree gchar *total = bar_assistant_format_bytes(m->disk_total);
		g_autofree gchar *disk = g_strdup_printf("%s free of %s",
		                                         free_text, total);

		gowl_bar_panel_add_field(panel, "Model disk", disk);
	}

	if (env != NULL && env->sysinfo != NULL) {
		glong rx = 0, tx = 0;
		gchar rbuf[32], tbuf[32];
		g_autofree gchar *net = NULL;
		const gchar *iface;

		iface = bar_sysinfo_net(env->sysinfo, NULL, &rx, &tx);
		bar_sysinfo_format_rate(rx, rbuf, sizeof(rbuf));
		bar_sysinfo_format_rate(tx, tbuf, sizeof(tbuf));
		net = g_strdup_printf("\xe2\x86\x93 %s/s  \xe2\x86\x91 %s/s%s%s",
			rbuf, tbuf, (iface != NULL) ? "  " : "",
			(iface != NULL) ? iface : "");
		gowl_bar_panel_add_field(panel, "Network", net);
	}

	if (m->top != NULL && m->top[0] != NULL) {
		g_autofree gchar *names = g_strjoinv(", ", m->top);

		gowl_bar_panel_add_section(panel, "Largest processes");
		gowl_bar_panel_add_label(panel, names);
	}

	gowl_bar_panel_add_section(panel, "Loaded models");
	if (!as->ollama_ok) {
		gowl_bar_panel_add_label(panel, "ollama is not reachable; see "
		                         "the Models tab.");
		return;
	}
	if (as->loaded->len == 0) {
		gowl_bar_panel_add_label(panel, "None: no model is holding "
		                         "memory.");
		return;
	}
	for (i = 0; i < as->loaded->len; i++) {
		BarAssistantModel *lm = g_ptr_array_index(as->loaded, i);
		g_autofree gchar *size = bar_assistant_format_bytes(lm->size);
		g_autofree gchar *vram = bar_assistant_format_bytes(lm->size_vram);
		g_autofree gchar *sub = g_strdup_printf("%s, %s in VRAM", size,
		                                        vram);
		g_autofree gchar *id = g_strconcat("loaded:", lm->name, NULL);

		item = gowl_bar_panel_add_row(panel, id, NULL, lm->name, sub);
		gowl_bar_panel_item_set_value(item, "unload");
		gowl_bar_panel_item_set_value_color(item, GOWL_BAR_COLOR_MUTED);
	}
	item = gowl_bar_panel_add_buttons(panel, "free-memory");
	gowl_bar_panel_add_button(item, "Free model memory", FALSE);
}

static GowlBarPanel *
as_panel(GowlBarPlugin *plugin, gpointer data)
{
	Assistant *as = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	BarAiBackend backend;
	g_autofree gchar *head = NULL;
	const gchar *provider;

	as_sync_answer(plugin, as);
	backend  = bar_ai_backend_for(plugin);
	provider = as_provider(plugin);

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel,
		CLAMP(gowl_bar_plugin_get_setting_int(plugin, "width",
		                                      AS_PANEL_WIDTH),
		      320, 1200));
	/* A long conversation scrolls inside a dropdown-sized panel rather
	   than growing to the height of the screen. */
	gowl_bar_panel_set_max_height(panel, 760);

	head = g_strdup_printf("%s \xc2\xb7 %s", as_backend_name(backend),
	                       (provider != NULL) ? provider
	                                          : "default provider");
	gowl_bar_panel_add_hero(panel, AS_GLYPH, "AI assistant", head);

	g_mutex_lock(&as->lock);
	item = gowl_bar_panel_add_buttons(panel, "tab");
	gowl_bar_panel_add_button(item, "Chat", as->tab == AS_TAB_CHAT);
	gowl_bar_panel_add_button(item, "Models", as->tab == AS_TAB_MODELS);
	gowl_bar_panel_add_button(item, "Machine", as->tab == AS_TAB_MACHINE);

	switch (as->tab) {
	case AS_TAB_MODELS:
		as_panel_models(plugin, as, panel);
		break;
	case AS_TAB_MACHINE:
		as_panel_machine(plugin, as, panel);
		break;
	case AS_TAB_CHAT:
	default:
		as_panel_chat(plugin, as, panel, backend);
		break;
	}
	g_mutex_unlock(&as->lock);
	return panel;
}

/* A name from an item id after its prefix, or NULL. */
static const gchar *
as_id_arg(const gchar *id, const gchar *prefix)
{
	if (id == NULL || !g_str_has_prefix(id, prefix))
		return NULL;
	return id + strlen(prefix);
}

static void
as_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
          gint index, gdouble value, guint button)
{
	Assistant *as = data;
	const gchar *arg;

	(void)value;
	(void)button;

	if (g_strcmp0(item_id, "tab") == 0) {
		g_mutex_lock(&as->lock);
		as->tab = (index == 1) ? AS_TAB_MODELS
		        : (index == 2) ? AS_TAB_MACHINE : AS_TAB_CHAT;
		as->pick = AS_PICK_NONE;
		g_mutex_unlock(&as->lock);
		if (index == 1 || index == 2)
			as_queue_refresh(plugin, as, FALSE);
		return;
	}

	if (g_strcmp0(item_id, "pick:provider") == 0 ||
	    g_strcmp0(item_id, "pick:model") == 0) {
		AsPick want = (g_strcmp0(item_id, "pick:provider") == 0)
			? AS_PICK_PROVIDER : AS_PICK_MODEL;

		g_mutex_lock(&as->lock);
		as->pick = (as->pick == want) ? AS_PICK_NONE : want;
		g_mutex_unlock(&as->lock);
		/* The ollama chooser lists what is installed: make sure
		   that is current. */
		if (want == AS_PICK_MODEL &&
		    g_strcmp0(as_provider(plugin), "ollama") == 0)
			as_queue_refresh(plugin, as, FALSE);
		return;
	}

	if ((arg = as_id_arg(item_id, "provider:")) != NULL) {
		/* A model belongs to its provider: `sonnet' means nothing to
		   ollama, so choosing a provider resets the model. */
		if (arg[0] == '\0')
			gowl_bar_plugin_set_setting(plugin, "ai-provider", NULL);
		else if (bar_assistant_valid_provider(arg))
			gowl_bar_plugin_set_setting(plugin, "ai-provider", arg);
		gowl_bar_plugin_set_setting(plugin, "ai-model", NULL);
		g_mutex_lock(&as->lock);
		as->pick = AS_PICK_NONE;
		g_mutex_unlock(&as->lock);
		as_apply_look(plugin, as);
		return;
	}

	if ((arg = as_id_arg(item_id, "model:")) != NULL) {
		if (arg[0] == '\0')
			gowl_bar_plugin_set_setting(plugin, "ai-model", NULL);
		else if (bar_assistant_valid_model(arg))
			gowl_bar_plugin_set_setting(plugin, "ai-model", arg);
		g_mutex_lock(&as->lock);
		as->pick = AS_PICK_NONE;
		g_mutex_unlock(&as->lock);
		as_apply_look(plugin, as);
		return;
	}

	if (g_strcmp0(item_id, "chat") == 0) {
		g_autofree gchar *draft = NULL;

		switch (index) {
		case 0:
			g_mutex_lock(&as->lock);
			draft = g_strdup(as->draft);
			g_mutex_unlock(&as->lock);
			as_send(plugin, as, draft);
			break;
		case 1:
			as_stop(plugin, as);
			break;
		case 2:
			as_clear(plugin, as);
			break;
		case 3:
			as_copy(plugin, as);
			break;
		case 4:
			if (bar_ai_backend_for(plugin) == BAR_AI_BACKEND_CMACS)
				as_continue_in_cmacs(plugin, as);
			break;
		default:
			break;
		}
		as_apply_look(plugin, as);
		return;
	}

	if ((arg = as_id_arg(item_id, "inst:")) != NULL) {
		g_mutex_lock(&as->lock);
		if (g_strcmp0(as->selected, arg) == 0) {
			g_clear_pointer(&as->selected, g_free);
		} else {
			g_free(as->selected);
			as->selected = g_strdup(arg);
		}
		g_mutex_unlock(&as->lock);
		return;
	}

	if ((arg = as_id_arg(item_id, "loaded:")) != NULL) {
		/* Unloading frees memory and deletes nothing, so it runs at
		   once; it is the one model operation with no card. */
		if (bar_assistant_valid_model(arg)) {
			gchar **one = g_new0(gchar *, 2);

			one[0] = g_strdup(arg);
			as_start_op(plugin, as, BAR_ASSISTANT_OP_UNLOAD, one);
		}
		return;
	}

	if (g_strcmp0(item_id, "model-actions") == 0) {
		g_autofree gchar *selected = NULL;

		g_mutex_lock(&as->lock);
		selected = g_strdup(as->selected);
		g_mutex_unlock(&as->lock);
		if (selected == NULL || !bar_assistant_valid_model(selected))
			return;
		switch (index) {
		case 0:
			as_arm(plugin, as, BAR_ASSISTANT_OP_LOAD, selected);
			break;
		case 1: {
			gchar **one = g_new0(gchar *, 2);

			one[0] = g_steal_pointer(&selected);
			as_start_op(plugin, as, BAR_ASSISTANT_OP_UNLOAD, one);
			break;
		}
		case 2:
			as_arm(plugin, as, BAR_ASSISTANT_OP_DELETE, selected);
			break;
		case 3:
			gowl_bar_plugin_set_setting(plugin, "ai-provider",
			                            "ollama");
			gowl_bar_plugin_set_setting(plugin, "ai-model", selected);
			g_mutex_lock(&as->lock);
			as->tab = AS_TAB_CHAT;
			g_mutex_unlock(&as->lock);
			as_apply_look(plugin, as);
			break;
		default:
			break;
		}
		return;
	}

	if (g_strcmp0(item_id, "approval") == 0) {
		if (index == 0) {
			as_approve(plugin, as);
		} else {
			g_mutex_lock(&as->lock);
			bar_assistant_approval_clear(&as->approval);
			g_mutex_unlock(&as->lock);
		}
		as_apply_look(plugin, as);
		return;
	}

	if (g_strcmp0(item_id, "pull-cancel") == 0) {
		as_cancel_pull(as);
		as_apply_look(plugin, as);
		return;
	}

	if (g_strcmp0(item_id, "free-memory") == 0) {
		as_arm(plugin, as, BAR_ASSISTANT_OP_UNLOAD_ALL, NULL);
		return;
	}

	if (g_strcmp0(item_id, "refresh") == 0) {
		as_queue_refresh(plugin, as, TRUE);
		return;
	}
}

static void
as_panel_text(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
              const gchar *text, gboolean submitted)
{
	Assistant *as = data;
	g_autofree gchar *trimmed = NULL;

	if (g_strcmp0(item_id, "prompt") == 0) {
		if (!submitted) {
			/* Kept so the Send button sends what is in the field. */
			g_mutex_lock(&as->lock);
			g_free(as->draft);
			as->draft = g_strdup(text);
			g_mutex_unlock(&as->lock);
			return;
		}
		as_send(plugin, as, text);
		as_apply_look(plugin, as);
		return;
	}

	if (!submitted)
		return;
	trimmed = g_strstrip(g_strdup((text != NULL) ? text : ""));

	if (g_strcmp0(item_id, "model-entry") == 0) {
		if (trimmed[0] == '\0' || bar_assistant_valid_model(trimmed)) {
			gowl_bar_plugin_set_setting(plugin, "ai-model",
				(trimmed[0] != '\0') ? trimmed : NULL);
			g_mutex_lock(&as->lock);
			as->pick = AS_PICK_NONE;
			g_mutex_unlock(&as->lock);
			as_apply_look(plugin, as);
		} else {
			as_note(as, "That is not a model id: letters, digits and "
			        ". _ - : / @ + only.", TRUE);
		}
		return;
	}

	if (g_strcmp0(item_id, "pull-entry") == 0) {
		if (!bar_assistant_valid_model(trimmed)) {
			g_mutex_lock(&as->lock);
			g_free(as->op_status);
			as->op_status = g_strdup("That is not a model name: use "
				"name:tag, e.g. qwen3:8b.");
			as->op_failed = TRUE;
			g_mutex_unlock(&as->lock);
			return;
		}
		/* Downloads are never started by Return alone. */
		as_arm(plugin, as, BAR_ASSISTANT_OP_PULL, trimmed);
		return;
	}
}

static void
as_panel_opened(GowlBarPlugin *plugin, gpointer data)
{
	Assistant *as = data;
	gboolean needs;

	g_mutex_lock(&as->lock);
	as->panel_open = TRUE;
	needs = (as->tab != AS_TAB_CHAT);
	g_mutex_unlock(&as->lock);

	/* The lists are only kept current while somebody looks at them;
	   catch up now rather than at the next interval. */
	if (needs || g_strcmp0(as_provider(plugin), "ollama") == 0)
		as_queue_refresh(plugin, as, FALSE);
}

static void
as_panel_closed(GowlBarPlugin *plugin, gpointer data)
{
	Assistant *as = data;

	(void)plugin;
	g_mutex_lock(&as->lock);
	as->panel_open = FALSE;
	as->pick = AS_PICK_NONE;
	as->clear_armed_at = 0;
	g_mutex_unlock(&as->lock);
}

static const GowlBarPluginVTable assistant_vtable = {
	sizeof(GowlBarPluginVTable),
	as_create, as_destroy,
	NULL, NULL, as_configure,
	as_interval, as_poll, as_poll_async,
	as_measure, as_draw,
	NULL, NULL,
	as_panel, as_action,
	as_panel_opened, as_panel_closed,
	NULL,
	as_signature,
	as_panel_text, NULL
};

/**
 * bar_register_assistant_plugins:
 * @registry: the bar's plugin registry
 *
 * Registers `ai-assistant', with `assistant' and `nova' as aliases.
 */
void
bar_register_assistant_plugins(GowlBarRegistry *registry)
{
	gowl_bar_registry_register_vtable(registry, AS_WIDGET, "AI assistant",
		"Chat with any provider, manage local ollama models, see the "
		"machine", &assistant_vtable);
	gowl_bar_registry_register_alias(registry, "assistant", AS_WIDGET);
	gowl_bar_registry_register_alias(registry, "nova", AS_WIDGET);
}
