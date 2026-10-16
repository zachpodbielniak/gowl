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
 * The `meeting-notes' widget: record a meeting, transcribe it,
 * summarise it, file the notes.
 *
 * A microphone in the bar.  Left-click starts recording every selected
 * source --- by default what the speakers play (the default sink's
 * monitor) and the default microphone --- each into its own 16 kHz mono
 * recording in a session directory: Opus in Ogg by default, piped
 * from the recorder through ffmpeg, because a WAV of an hour-long
 * meeting is over 100 MB of audio nobody listens to twice.  Whisper
 * still wants a WAV; that one is made when it is needed and deleted
 * as soon as whisper is done (see `Audio files' below).  Left-click again stops them, and the
 * meeting is processed by one of two backends:
 *
 *   cmacs   `emacsctl eval (cmacs-gowl-bar-meeting-finish ...)' hands
 *           the session to cmacs, which mixes, transcribes with
 *           cmacs-transcribe, summarises and files the result (a
 *           directory, the second brain, the Org inbox or both).
 *           Progress comes back as `meeting-notes.*' settings, the same
 *           push model as bar-ai.c.  `cmacs --gowl' selects this.
 *   local   everything here: ffmpeg mixes, whisper-cli transcribes,
 *           bar-ai summarises, and an Org or Markdown file is written.
 *           Standalone gowl; it needs a whisper model configured.
 *
 * How the threads divide the work --- the one thing to get right here:
 *
 *   - Every child (a recorder, ffmpeg, whisper-cli) is spawned on a
 *     worker with G_SPAWN_DO_NOT_REAP_CHILD and collected with
 *     waitpid(WNOHANG) from the async poll, which the bar runs once a
 *     second while a meeting is active.  Nothing ever WAITS for a
 *     child: a forty-minute whisper run does not hold one of the bar's
 *     four worker threads, and nothing touches GLib's main context
 *     (which, under cmacs, is Emacs's thread).
 *   - Stopping sends SIGINT --- the signal pw-record and ffmpeg finish
 *     a WAV header on --- to the pids THIS widget started, never a
 *     pattern.  A recorder still alive after `stop-grace' seconds gets
 *     SIGTERM, then SIGKILL.
 *   - Toasts are raised only from the sync poll: the host's toast
 *     stack is not thread-safe, so a worker queues them and the next
 *     poll on the compositor thread raises them.
 *   - Everything the worker and the compositor thread share sits behind
 *     MeetData.lock; a worker builds fresh lists unlocked and swaps
 *     them in.  A generation counter, bumped by Cancel, makes a worker
 *     that finishes late drop its result instead of resurrecting a
 *     cancelled meeting.
 *
 * Audio files:
 *
 *   - A recorder template that names {output} writes the file itself
 *     (the WAV-only path, or ffmpeg's pulse input straight to Ogg).
 *     One that does NOT name it writes raw s16le 16 kHz mono PCM to
 *     stdout, and is piped into `encode-command' (ffmpeg -> Opus), two
 *     processes per source.  Stopping signals the recorder only; the
 *     encoder finishes the file when its input ends.
 *   - The local backend mixes or decodes the recordings into one
 *     transient `mixed.wav' for whisper-cli and deletes it when the
 *     transcription is over, whatever its outcome.  The cmacs backend
 *     never makes one: cmacs-transcribe makes and deletes its own.
 *
 * Speaker separation (diarisation) and recognising voices are
 * deliberately not here yet; see docs/bar-widgets/meeting-notes.org.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib/gstdio.h>
#include <glib-unix.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "bar-internal.h"
#include "bar-ai.h"
#include "bar-meeting-model.h"

#define MEET_WIDGET          "meeting-notes"
#define MEET_ICON            "\xef\x84\xb0"   /* nf-fa-microphone */
#define MEET_ICON_SOURCE_OUT "\xef\x80\xa8"   /* nf-fa-volume_up */
#define MEET_ICON_FILE       "\xef\x85\x9c"   /* nf-fa-file_text */
#define MEET_ICON_MODEL      "\xef\x83\xab"   /* nf-fa-lightbulb */

/* Raw PCM to stdout: no {output}, so it is piped into the encoder. */
#define MEET_DEFAULT_RECORD \
	"pw-record --target {source} --rate 16000 --channels 1 " \
	"--format s16 --raw -"
/* pw-record and no ffmpeg: the old uncompressed WAV, said once. */
#define MEET_WAV_RECORD \
	"pw-record --target {source} --rate 16000 --channels 1 " \
	"--format s16 {output}"
/* ffmpeg and no pw-record: ffmpeg records and encodes in one process. */
#define MEET_FALLBACK_RECORD \
	"{ffmpeg} -hide_banner -loglevel error -nostdin -f pulse " \
	"-i {source} -ac 1 -ar 16000 {codec} {output}"
#define MEET_FALLBACK_RECORD_WAV \
	"{ffmpeg} -hide_banner -loglevel error -nostdin -f pulse " \
	"-i {source} -ac 1 -ar 16000 {output}"
/* Raw PCM on stdin to Opus.  `voip' tunes the codec for speech. */
#define MEET_DEFAULT_ENCODE \
	"{ffmpeg} -hide_banner -loglevel error -y -f s16le -ar 16000 " \
	"-ac 1 -i pipe:0 -c:a libopus -b:a {bitrate} -application voip " \
	"{output}"
#define MEET_DEFAULT_BITRATE "32k"
#define MEET_DEFAULT_TRANSCRIBE \
	"whisper-cli -m {model} -l {language} -t {threads} -otxt " \
	"-of {output_base} {input}"

#define MEET_DEFAULT_RECORDINGS "~/Documents/recordings/meetings"
#define MEET_DEFAULT_OUTPUT     "~/Documents/notes/00_inbox"
#define MEET_DEFAULT_MODELS     "~/.local/share/cmacs/whisper-models"

/* A WAV header is 44 bytes; a file no bigger than that holds no audio.
   An Ogg file's headers are a few hundred, and under a kilobyte of
   Opus at 32 kb/s is a quarter of a second: nothing worth transcribing. */
#define MEET_WAV_HEADER      (44)
#define MEET_OGG_EMPTY       (1024)
#define MEET_TRANSCRIPT_MAX  (8 * 1024 * 1024)
#define MEET_PROMPT_MAX      (400 * 1024)
#define MEET_TITLE_MAX       (120)
#define MEET_RECENT_MAX      (5)
#define MEET_MODELS_MAX      (20)
#define MEET_SOURCES_TTL_US  (60 * G_USEC_PER_SEC)
#define MEET_KILL_AFTER_TERM (3)

/* ----------------------------------------------------------------
 * State
 * ---------------------------------------------------------------- */

/*
 * One source.  Piped, it is two processes: the recorder (pid) writing
 * PCM into the encoder (enc_pid).  Each pid is zeroed once collected,
 * and `running' lasts until both are --- a pid of 0 must never reach
 * kill(), which would signal our whole process group.
 */
typedef struct {
	GPid      pid;
	GPid      enc_pid;    /* 0: not piped, or collected */
	gchar    *source;
	gchar    *label;
	gchar    *path;
	gchar    *log;
	gboolean  running;
	gboolean  early;      /* died before it was asked to stop */
	gboolean  lost;       /* `early' was raised; never twice */
} MeetRecorder;

/* A child we stopped wanting --- a cancelled recorder, a transcriber
   for a meeting that moved on --- still ours to collect, and to
   escalate on if it ignores the first signal. */
typedef struct {
	GPid    pid;
	gint64  since;
	gint    stage;     /* 0 first signal sent, 1 TERM, 2 KILL */
} MeetOrphan;

typedef struct {
	GowlBarToastUrgency  urgency;
	gchar               *summary;
	gchar               *body;
	gchar               *command;
} MeetToast;

typedef struct {
	GMutex           lock;

	BarMeetingPhase  phase;
	gint64           phase_since;    /* monotonic us */
	guint            generation;     /* bumped by start and cancel */

	/* The meeting in hand. */
	GDateTime       *started_at;     /* wall clock, for names */
	gint64           rec_started;    /* monotonic us */
	gint64           rec_stopped;
	gint64           stop_sent;      /* when SIGINT went out, 0 none */
	gint             stop_stage;     /* 0 INT, 1 TERM, 2 KILL sent */
	gchar           *session_dir;
	gchar           *meeting_title;  /* frozen when recording starts */
	GPtrArray       *recorders;      /* MeetRecorder* */
	GPid             child;          /* ffmpeg or whisper-cli */
	gchar           *child_log;
	gchar           *mixed;          /* the WAV whisper is given */
	gboolean         mixed_transient; /* we made it: delete after */
	gchar           *transcript_base;
	gchar           *transcript;
	gchar           *summary_note;
	gchar           *job_id;         /* the cmacs route's job */
	gchar           *cmacs_seen;     /* last absorbed state+result */
	BarAi           *ai;
	GArray          *orphans;        /* MeetOrphan, still to reap */

	/* What the last meeting came to. */
	gchar           *error;
	gchar           *result;

	/* The title entry. */
	gchar           *title;
	guint            title_rev;

	/* Sources. */
	GPtrArray       *sources;        /* BarMeetingSource* */
	gboolean         sources_loaded;
	gboolean         pactl_missing;
	gchar           *pactl_error;
	gint64           sources_at;
	gchar          **selection;      /* NULL until loaded */
	gboolean         echo_warned;

	/* What the panel lists, refreshed on a worker when it opens. */
	gboolean         panel_open;
	gboolean         want_refresh;
	GPtrArray       *recent;         /* gchar* paths */
	GPtrArray       *models;         /* gchar* paths */
	gboolean         have_recorder;
	gboolean         have_ffmpeg;
	gboolean         have_whisper;
	gboolean         have_emacsctl;
	gboolean         deps_checked;

	GPtrArray       *toasts;         /* MeetToast*, raised by poll */
} MeetData;

static void
meet_recorder_free(gpointer data)
{
	MeetRecorder *r = data;

	g_free(r->source);
	g_free(r->label);
	g_free(r->path);
	g_free(r->log);
	g_free(r);
}

static void
meet_toast_free(gpointer data)
{
	MeetToast *t = data;

	g_free(t->summary);
	g_free(t->body);
	g_free(t->command);
	g_free(t);
}

static gpointer
meet_create(GowlBarPlugin *plugin)
{
	MeetData *md;

	(void)plugin;
	md = g_new0(MeetData, 1);
	g_mutex_init(&md->lock);
	md->recorders = g_ptr_array_new_with_free_func(meet_recorder_free);
	md->sources   = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_meeting_source_free);
	md->recent    = g_ptr_array_new_with_free_func(g_free);
	md->models    = g_ptr_array_new_with_free_func(g_free);
	md->toasts    = g_ptr_array_new_with_free_func(meet_toast_free);
	md->orphans   = g_array_new(FALSE, FALSE, sizeof(MeetOrphan));
	md->ai        = bar_ai_new(MEET_WIDGET);
	md->want_refresh = TRUE;
	return md;
}

/*
 * Collect @pid if it has exited.  Returns TRUE when it is gone (reaped
 * now, or not our child at all), filling @status when known.  Never
 * blocks: WNOHANG.
 */
static gboolean
meet_reap(GPid pid, gint *status)
{
	pid_t done;
	gint st = 0;

	if (pid <= 0)
		return TRUE;
	do {
		done = waitpid((pid_t)pid, &st, WNOHANG);
	} while (done < 0 && errno == EINTR);
	if (done == 0)
		return FALSE;
	if (status != NULL)
		*status = (done > 0) ? st : 0;
	g_spawn_close_pid(pid);
	return TRUE;
}

static void
meet_destroy(GowlBarPlugin *plugin, gpointer data)
{
	MeetData *md = data;
	guint i;

	(void)plugin;
	if (md == NULL)
		return;

	/*
	 * A recording outliving its widget is still a file being written:
	 * ask it to finish (SIGINT writes the WAV header) rather than
	 * orphan it mid-sample.  This runs on the compositor thread, so it
	 * does not wait; anything not yet exited is collected by nobody
	 * and stays a zombie until the process ends, which is the price of
	 * never blocking the desktop on a child.
	 */
	for (i = 0; i < md->recorders->len; i++) {
		MeetRecorder *r = g_ptr_array_index(md->recorders, i);

		if (r->running) {
			if (r->pid > 0) {
				kill((pid_t)r->pid, SIGINT);
				meet_reap(r->pid, NULL);
			}
			if (r->enc_pid > 0)
				meet_reap(r->enc_pid, NULL);
			r->running = FALSE;
		}
	}
	if (md->child > 0) {
		kill((pid_t)md->child, SIGTERM);
		meet_reap(md->child, NULL);
	}
	for (i = 0; i < md->orphans->len; i++) {
		MeetOrphan *o = &g_array_index(md->orphans, MeetOrphan, i);

		kill((pid_t)o->pid, SIGTERM);
		meet_reap(o->pid, NULL);
	}

	bar_ai_free(md->ai);
	g_ptr_array_unref(md->recorders);
	g_ptr_array_unref(md->sources);
	g_ptr_array_unref(md->recent);
	g_ptr_array_unref(md->models);
	g_ptr_array_unref(md->toasts);
	g_array_unref(md->orphans);
	g_clear_pointer(&md->started_at, g_date_time_unref);
	g_strfreev(md->selection);
	g_free(md->session_dir);
	g_free(md->meeting_title);
	g_free(md->child_log);
	g_free(md->mixed);
	g_free(md->transcript_base);
	g_free(md->transcript);
	g_free(md->summary_note);
	g_free(md->job_id);
	g_free(md->cmacs_seen);
	g_free(md->error);
	g_free(md->result);
	g_free(md->title);
	g_free(md->pactl_error);
	g_mutex_clear(&md->lock);
	g_free(md);
}

/* ----------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------- */

/* A setting, or its default when unset or empty. */
static const gchar *
meet_setting(GowlBarPlugin *plugin, const gchar *key, const gchar *fallback)
{
	const gchar *v = gowl_bar_plugin_get_setting(plugin, key);

	return (v != NULL && v[0] != '\0') ? v : fallback;
}

/* A path setting, tilde-expanded. */
static gchar *
meet_path_setting(GowlBarPlugin *plugin, const gchar *key,
                  const gchar *fallback)
{
	const gchar *v = meet_setting(plugin, key, fallback);

	return (v != NULL) ? bar_expand_tilde(v) : NULL;
}

/* Whether the cmacs backend is selected.  `auto' and anything else is
   local: cmacs --gowl pushes `backend: cmacs' itself, so a standalone
   bar never guesses its way into an emacsctl call. */
static gboolean
meet_backend_is_cmacs(GowlBarPlugin *plugin)
{
	return g_ascii_strcasecmp(meet_setting(plugin, "backend", "local"),
	                          "cmacs") == 0;
}

/* Must hold md->lock. */
static void
meet_set_phase_locked(MeetData *md, BarMeetingPhase phase)
{
	md->phase = phase;
	md->phase_since = g_get_monotonic_time();
}

/*
 * Signal @pid and hand it to the async poll to collect.  Must hold
 * md->lock.  A child that shrugs off @sig --- bash waiting on a
 * foreground command swallows SIGINT, and a wedged recorder may ignore
 * it --- is escalated to SIGTERM and SIGKILL there, so an abandoned
 * child can never outlive the meeting.
 */
static void
meet_orphan_locked(MeetData *md, GPid pid, gint sig)
{
	MeetOrphan o;

	if (pid <= 0)
		return;
	kill((pid_t)pid, sig);
	o.pid   = pid;
	o.since = g_get_monotonic_time();
	o.stage = (sig == SIGTERM) ? 1 : 0;
	g_array_append_val(md->orphans, o);
}

/* Must hold md->lock.  Queues a toast for the next sync poll. */
static void
meet_toast_locked(MeetData *md, GowlBarToastUrgency urgency,
                  const gchar *summary, const gchar *body,
                  const gchar *command)
{
	MeetToast *t;

	/* A queue nobody drains (no bar running polls) must not grow
	   without bound. */
	if (md->toasts->len >= 16)
		g_ptr_array_remove_index(md->toasts, 0);
	t = g_new0(MeetToast, 1);
	t->urgency = urgency;
	t->summary = g_strdup(summary);
	t->body    = g_strdup(body);
	t->command = g_strdup(command);
	g_ptr_array_add(md->toasts, t);
}

/* Must hold md->lock.  Ends the meeting in an error, toasted. */
static void
meet_fail_locked(MeetData *md, const gchar *message)
{
	g_free(md->error);
	md->error = g_strdup(message);
	meet_set_phase_locked(md, BAR_MEETING_ERROR);
	meet_toast_locked(md, GOWL_BAR_TOAST_NORMAL, "Meeting notes failed",
	                  message, NULL);
}

/*
 * The command a click on a saved file runs, or NULL.
 *
 * Under cmacs the notes open in cmacs itself, in the frame the user is
 * working in, over emacsctl: `cmacs-gowl-bar-meeting-open' only
 * schedules the visit and returns, because an eval runs on the
 * compositor's thread.  An `open-command' the user set wins either way.
 * Standalone, xdg-open.  The path is quoted for Elisp inside the form
 * and the form for the command line, so neither a quote nor a space in
 * it can split or end anything.
 */
static gchar *
meet_open_command(GowlBarPlugin *plugin, const gchar *path)
{
	g_autofree gchar *quoted = NULL;

	if (path == NULL || path[0] == '\0')
		return NULL;
	if (gowl_bar_plugin_get_setting(plugin, "open-command") == NULL &&
	    meet_backend_is_cmacs(plugin) && g_utf8_validate(path, -1, NULL) &&
	    strpbrk(path, "\n\r") == NULL) {
		g_autofree gchar *lisp = bar_elisp_quote(path);
		g_autofree gchar *form = NULL;
		g_autofree gchar *ctl = NULL;

		form = g_strdup_printf("(cmacs-gowl-bar-meeting-open \"%s\")",
		                       lisp);
		ctl = g_shell_quote(meet_setting(plugin, "emacsctl",
		                                 "emacsctl"));
		quoted = g_shell_quote(form);
		return g_strdup_printf("%s eval %s", ctl, quoted);
	}
	quoted = g_shell_quote(path);
	return g_strdup_printf("%s %s",
		meet_setting(plugin, "open-command", "xdg-open"), quoted);
}

/*
 * The last non-empty line of a log file, bounded.  Worker only: it
 * reads a file.  The reason a recorder or whisper died is almost
 * always the last thing it printed.
 */
static gchar *
meet_log_tail(const gchar *path)
{
	g_autofree gchar *text = NULL;
	g_auto(GStrv) lines = NULL;
	gsize len = 0;
	gint i;
	const gchar *from;

	if (path == NULL || !g_file_get_contents(path, &text, &len, NULL))
		return NULL;
	from = (len > 4096) ? text + len - 4096 : text;
	lines = g_strsplit(from, "\n", -1);
	for (i = (gint)g_strv_length(lines) - 1; i >= 0; i--) {
		gchar *line = g_strstrip(lines[i]);

		if (line[0] != '\0')
			return bar_meeting_clean_text(line, 300);
	}
	return NULL;
}

/* Whether @path holds more than its container's headers.  Worker
   only. */
static gboolean
meet_has_audio(const gchar *path)
{
	GStatBuf st;

	if (g_stat(path, &st) != 0)
		return FALSE;
	return st.st_size > (g_str_has_suffix(path, ".wav")
	                     ? MEET_WAV_HEADER : MEET_OGG_EMPTY);
}

/*
 * Spawn @argv detached from GLib's child watching, its output into
 * @log.  Worker only.  The child gets /dev/null on stdin so a recorder
 * that would prompt cannot hang on the compositor's terminal.
 */
static gboolean
meet_spawn(gchar **argv, const gchar *log, GPid *pid, GError **error)
{
	gint fd = -1;
	gboolean ok;

	if (log != NULL) {
		fd = g_open(log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
		            0600);
		if (fd < 0) {
			g_set_error(error, G_FILE_ERROR,
			            g_file_error_from_errno(errno),
			            "cannot write %s: %s", log,
			            g_strerror(errno));
			return FALSE;
		}
	}
	ok = g_spawn_async_with_fds(NULL, argv, NULL,
	                            G_SPAWN_SEARCH_PATH |
	                            G_SPAWN_DO_NOT_REAP_CHILD |
	                            G_SPAWN_STDIN_FROM_DEV_NULL,
	                            NULL, NULL, pid, -1, fd, fd, error);
	if (fd >= 0)
		close(fd);
	return ok;
}

/*
 * Spawn @rec_argv with its stdout piped into @enc_argv's stdin, both
 * logging into @log.  Worker only.  The encoder starts first so the
 * recorder never writes into a pipe with nobody on the other end; the
 * pipe's ends are close-on-exec, so neither child keeps the other's
 * end open and the encoder sees end-of-file when the recorder exits.
 */
static gboolean
meet_spawn_piped(gchar **rec_argv, gchar **enc_argv, const gchar *log,
                 GPid *rec_pid, GPid *enc_pid, GError **error)
{
	gint fds[2] = { -1, -1 };
	gint logfd;
	gboolean ok;

	*rec_pid = 0;
	*enc_pid = 0;
	logfd = g_open(log, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND |
	               O_CLOEXEC, 0600);
	if (logfd < 0) {
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "cannot write %s: %s", log, g_strerror(errno));
		return FALSE;
	}
	if (!g_unix_open_pipe(fds, FD_CLOEXEC, error)) {
		close(logfd);
		return FALSE;
	}

	ok = g_spawn_async_with_fds(NULL, enc_argv, NULL,
	                            G_SPAWN_SEARCH_PATH |
	                            G_SPAWN_DO_NOT_REAP_CHILD,
	                            NULL, NULL, enc_pid, fds[0], logfd, logfd,
	                            error);
	if (ok)
		ok = g_spawn_async_with_fds(NULL, rec_argv, NULL,
		                            G_SPAWN_SEARCH_PATH |
		                            G_SPAWN_DO_NOT_REAP_CHILD |
		                            G_SPAWN_STDIN_FROM_DEV_NULL,
		                            NULL, NULL, rec_pid, -1, fds[1],
		                            logfd, error);
	close(fds[0]);
	close(fds[1]);
	close(logfd);

	if (!ok && *enc_pid > 0) {
		/* No recorder: the encoder has just seen its input close and
		   is exiting.  This is a worker, so it can wait for it. */
		kill((pid_t)*enc_pid, SIGTERM);
		waitpid((pid_t)*enc_pid, NULL, 0);
		*enc_pid = 0;
	}
	return ok;
}

/* Signal a recorder's processes: the recorder, and the encoder too
   when @both.  Collected pids are 0 and skipped. */
static void
meet_recorder_kill(MeetRecorder *r, gint sig, gboolean both)
{
	if (r->pid > 0)
		kill((pid_t)r->pid, sig);
	if (both && r->enc_pid > 0)
		kill((pid_t)r->enc_pid, sig);
}

/* ----------------------------------------------------------------
 * The bar
 * ---------------------------------------------------------------- */

static gint
meet_interval(GowlBarPlugin *plugin, gpointer data)
{
	MeetData *md = data;
	gboolean busy;

	(void)plugin;
	g_mutex_lock(&md->lock);
	busy = bar_meeting_phase_is_busy(md->phase) || md->orphans->len > 0;
	g_mutex_unlock(&md->lock);
	/* Once a second while anything is happening: the timer ticks, and
	   the async poll is what notices a child exiting.  Idle, there is
	   nothing to notice. */
	return busy ? 1 : 30;
}

/*
 * Set the label, colour and tooltip from the cached state.  Compositor
 * thread; reads nothing but MeetData and settings.
 */
static void
meet_apply_look(GowlBarPlugin *plugin, MeetData *md)
{
	BarMeetingPhase phase;
	gint64 now, since, started, stopped;
	guint n_rec = 0, i;
	g_autofree gchar *label = NULL;
	g_autofree gchar *tooltip = NULL;
	g_autofree gchar *clock = NULL;
	g_autofree gchar *error = NULL;
	g_autofree gchar *result = NULL;

	now = g_get_monotonic_time();
	g_mutex_lock(&md->lock);
	phase   = md->phase;
	since   = md->phase_since;
	started = md->rec_started;
	stopped = md->rec_stopped;
	error   = g_strdup(md->error);
	result  = g_strdup(md->result);
	for (i = 0; i < md->recorders->len; i++)
		if (((MeetRecorder *)g_ptr_array_index(md->recorders,
		                                       i))->running)
			n_rec++;
	g_mutex_unlock(&md->lock);

	gowl_bar_plugin_set_icon(plugin, MEET_ICON);

	switch (phase) {
	case BAR_MEETING_RECORDING:
	case BAR_MEETING_STOPPING:
		clock = bar_meeting_format_clock(
			(((phase == BAR_MEETING_STOPPING && stopped > 0)
			  ? stopped : now) - started) / G_USEC_PER_SEC);
		label = (phase == BAR_MEETING_RECORDING)
			? g_strdup_printf("\xe2\x97\x8f %s", clock)
			: g_strdup("stopping");
		tooltip = g_strdup_printf(
			"Recording %u source%s, %s\n"
			"Left-click to stop, right-click for the panel",
			n_rec, (n_rec == 1) ? "" : "s", clock);
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-recording"),
			GOWL_BAR_COLOR_RED);
		break;
	case BAR_MEETING_STARTING:
	case BAR_MEETING_MIXING:
	case BAR_MEETING_TRANSCRIBING:
	case BAR_MEETING_SUMMARISING:
	case BAR_MEETING_FILING:
		clock = bar_meeting_format_clock((now - since) /
		                                 G_USEC_PER_SEC);
		if (phase == BAR_MEETING_TRANSCRIBING)
			label = g_strdup_printf("transcribing %s", clock);
		else
			label = g_strdup(bar_meeting_phase_name(phase));
		tooltip = g_strdup_printf(
			"Meeting notes: %s (%s)\n"
			"Right-click for the panel",
			bar_meeting_phase_name(phase), clock);
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-working"),
			GOWL_BAR_COLOR_YELLOW);
		break;
	case BAR_MEETING_ERROR:
		label = g_strdup("error");
		tooltip = g_strdup_printf(
			"Meeting notes failed: %s\n"
			"Right-click for the panel",
			(error != NULL) ? error : "unknown error");
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-error"),
			GOWL_BAR_COLOR_RED);
		break;
	case BAR_MEETING_IDLE:
	case BAR_MEETING_DONE:
	default:
		tooltip = (result != NULL)
			? g_strdup_printf(
				"Meeting notes: left-click to record, "
				"right-click for options\nLast: %s", result)
			: g_strdup("Meeting notes: left-click to record, "
			           "right-click for options");
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color-idle"),
			GOWL_BAR_COLOR_SUBTEXT);
		break;
	}

	gowl_bar_plugin_set_label(plugin, label);
	gowl_bar_plugin_set_tooltip(plugin, tooltip);
}

/* Raise the toasts a worker queued.  Compositor thread only. */
static void
meet_raise_toasts(GowlBarPlugin *plugin, MeetData *md)
{
	GPtrArray *pending;
	GowlBarHost *host;
	guint i;

	g_mutex_lock(&md->lock);
	pending = md->toasts;
	md->toasts = g_ptr_array_new_with_free_func(meet_toast_free);
	g_mutex_unlock(&md->lock);

	host = gowl_bar_plugin_get_host(plugin);
	for (i = 0; i < pending->len; i++) {
		MeetToast *t = g_ptr_array_index(pending, i);

		if (t->command != NULL && host != NULL) {
			GowlBarToast *toast;

			/* A finished meeting's toast opens the notes, so it is
			   built here rather than through
			   gowl_bar_plugin_notify(), which would wire it to the
			   panel instead. */
			toast = gowl_bar_toast_new(t->summary, t->body);
			gowl_bar_toast_set_urgency(toast, t->urgency);
			gowl_bar_toast_set_app(toast,
				gowl_bar_plugin_get_title(plugin));
			gowl_bar_toast_set_icon(toast, MEET_ICON);
			gowl_bar_toast_set_command(toast, t->command);
			gowl_bar_toast_set_hint(toast, "Click to open");
			gowl_bar_host_notify(host, toast);
		} else {
			gowl_bar_plugin_notify(plugin, t->urgency, t->summary,
			                       t->body);
		}
	}
	g_ptr_array_unref(pending);
}

static void
meet_poll(GowlBarPlugin *plugin, gpointer data)
{
	MeetData *md = data;

	meet_raise_toasts(plugin, md);
	meet_apply_look(plugin, md);
}

/* ----------------------------------------------------------------
 * Sources, models and recent notes (worker)
 * ---------------------------------------------------------------- */

static gchar *
meet_selection_path(GowlBarPlugin *plugin)
{
	GowlBarHost *host = gowl_bar_plugin_get_host(plugin);
	const gchar *dir;

	dir = (host != NULL) ? gowl_bar_host_get_state_dir(host) : NULL;
	if (dir == NULL)
		return NULL;
	return g_build_filename(dir, "meeting-notes.sources", NULL);
}

/* Write the selection to the state file.  Worker only. */
static void
meet_save_selection_work(GowlBarPlugin *plugin, gpointer user_data)
{
	MeetData *md = user_data;
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *dir = NULL;

	path = meet_selection_path(plugin);
	if (path == NULL)
		return;
	g_mutex_lock(&md->lock);
	text = bar_meeting_format_selection(
		(const gchar * const *)md->selection);
	g_mutex_unlock(&md->lock);

	dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	if (!g_file_set_contents(path, text, -1, NULL))
		g_warning("meeting-notes: cannot write %s", path);
}

/*
 * Re-read the source list, the defaults and (once) the saved
 * selection.  Worker only: it runs pactl.
 */
static void
meet_refresh_sources(GowlBarPlugin *plugin, MeetData *md)
{
	const gchar *pactl;
	g_autofree gchar *found = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *sink = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *sel_path = NULL;
	g_autofree gchar *sel_text = NULL;
	GPtrArray *parsed;
	gchar **loaded = NULL;
	gboolean have_file = FALSE;
	gboolean need_selection;

	pactl = meet_setting(plugin, "pactl", "pactl");
	found = g_find_program_in_path(pactl);

	g_mutex_lock(&md->lock);
	need_selection = (md->selection == NULL);
	g_mutex_unlock(&md->lock);

	if (need_selection) {
		sel_path = meet_selection_path(plugin);
		if (sel_path != NULL &&
		    g_file_get_contents(sel_path, &sel_text, NULL, NULL)) {
			have_file = TRUE;
			loaded = bar_meeting_parse_selection(sel_text);
		}
	}

	if (found == NULL) {
		g_mutex_lock(&md->lock);
		md->pactl_missing = TRUE;
		md->sources_loaded = TRUE;
		md->sources_at = g_get_monotonic_time();
		if (md->selection == NULL)
			md->selection = (loaded != NULL)
				? loaded : g_new0(gchar *, 1);
		else
			g_strfreev(loaded);
		g_mutex_unlock(&md->lock);
		return;
	}

	{
		const gchar *list_argv[] = { found, "-f", "json", "list",
		                             "sources", NULL };
		const gchar *sink_argv[] = { found, "get-default-sink", NULL };
		const gchar *src_argv[]  = { found, "get-default-source",
		                             NULL };

		json   = bar_run_argv(list_argv);
		sink   = bar_run_argv_line(sink_argv);
		source = bar_run_argv_line(src_argv);
	}
	parsed = bar_meeting_parse_sources(json);

	g_mutex_lock(&md->lock);
	md->pactl_missing = FALSE;
	g_clear_pointer(&md->pactl_error, g_free);
	if (json == NULL)
		md->pactl_error = g_strdup(
			"pactl gave no source list; is PipeWire (or "
			"PulseAudio) running?");
	g_ptr_array_unref(md->sources);
	md->sources = parsed;
	md->sources_loaded = TRUE;
	md->sources_at = g_get_monotonic_time();
	if (md->selection == NULL) {
		/* No state file: the defaults.  An EMPTY state file is a
		   choice --- the user turned everything off --- and is kept
		   as such. */
		md->selection = have_file
			? loaded
			: bar_meeting_default_selection(sink, source);
		loaded = NULL;
	}
	g_mutex_unlock(&md->lock);
	g_strfreev(loaded);
}

/* @text with every @from replaced by @to. */
static gchar *
meet_replace(const gchar *text, const gchar *from, const gchar *to)
{
	g_auto(GStrv) parts = g_strsplit(text, from, -1);

	return g_strjoinv(to, parts);
}

/* Whether @rate is a bitrate ffmpeg reads (`32k', `48000'): it lands
   inside the recorder's argument list, so nothing else is let in. */
static gboolean
meet_bitrate_ok(const gchar *rate)
{
	const gchar *p;

	if (rate == NULL || rate[0] == '\0' || strlen(rate) > 12)
		return FALSE;
	for (p = rate; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p) && *p != '.')
			return FALSE;
	return TRUE;
}

/* The program a command template runs, found on PATH, or NULL.
   Worker only: it walks PATH. */
static gchar *
meet_template_program(const gchar *tmpl)
{
	g_auto(GStrv) argv = bar_meeting_expand_template(tmpl, NULL, NULL);

	if (argv == NULL || argv[0] == NULL)
		return NULL;
	return g_find_program_in_path(argv[0]);
}

static gint
meet_compare_desc(gconstpointer a, gconstpointer b)
{
	return -g_strcmp0(*(const gchar * const *)a,
	                  *(const gchar * const *)b);
}

static gint
meet_compare_asc(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(*(const gchar * const *)a,
	                 *(const gchar * const *)b);
}

/* Recent notes and the model list, for the panel.  Worker only. */
static void
meet_refresh_lists(GowlBarPlugin *plugin, MeetData *md)
{
	g_autofree gchar *outdir = NULL;
	g_autofree gchar *modeldir = NULL;
	g_autofree gchar *last = NULL;
	GPtrArray *recent;
	GPtrArray *models;
	GDir *dir;
	const gchar *name;
	guint seen = 0;
	gboolean rec, ff, wh, ctl;

	outdir   = meet_path_setting(plugin, "output-dir",
	                             MEET_DEFAULT_OUTPUT);
	modeldir = meet_path_setting(plugin, "whisper-models-dir",
	                             MEET_DEFAULT_MODELS);
	recent = g_ptr_array_new_with_free_func(g_free);
	models = g_ptr_array_new_with_free_func(g_free);

	/* Notes are named YYYY-MM-DD_HH-MM_meeting..., so a reverse name
	   sort is newest first without a stat per file.  Bounded: an
	   inbox can hold thousands of files. */
	dir = (outdir != NULL) ? g_dir_open(outdir, 0, NULL) : NULL;
	while (dir != NULL && (name = g_dir_read_name(dir)) != NULL &&
	       seen++ < 5000) {
		if (strstr(name, "_meeting") == NULL)
			continue;
		if (!g_str_has_suffix(name, ".org") &&
		    !g_str_has_suffix(name, ".md"))
			continue;
		g_ptr_array_add(recent, g_build_filename(outdir, name, NULL));
	}
	if (dir != NULL)
		g_dir_close(dir);
	g_ptr_array_sort(recent, meet_compare_desc);

	seen = 0;
	dir = (modeldir != NULL) ? g_dir_open(modeldir, 0, NULL) : NULL;
	while (dir != NULL && (name = g_dir_read_name(dir)) != NULL &&
	       seen++ < 1000) {
		if (!g_str_has_suffix(name, ".bin") &&
		    !g_str_has_suffix(name, ".gguf"))
			continue;
		if (models->len < MEET_MODELS_MAX)
			g_ptr_array_add(models,
				g_build_filename(modeldir, name, NULL));
	}
	if (dir != NULL)
		g_dir_close(dir);
	g_ptr_array_sort(models, meet_compare_asc);

	{
		g_autofree gchar *p1 = NULL;
		g_autofree gchar *p2 = NULL;
		g_autofree gchar *p3 = NULL;
		g_autofree gchar *p4 = NULL;

		p1 = meet_template_program(meet_setting(plugin,
			"record-command", MEET_DEFAULT_RECORD));
		p2 = g_find_program_in_path(meet_setting(plugin, "ffmpeg",
		                                         "ffmpeg"));
		p3 = meet_template_program(meet_setting(plugin,
			"transcribe-command", MEET_DEFAULT_TRANSCRIBE));
		p4 = g_find_program_in_path(meet_setting(plugin, "emacsctl",
		                                         "emacsctl"));
		rec = (p1 != NULL);
		ff  = (p2 != NULL);
		wh  = (p3 != NULL);
		ctl = (p4 != NULL);
	}

	g_mutex_lock(&md->lock);
	/* A result cmacs filed elsewhere (the second brain) belongs in the
	   list too, ahead of the directory's. */
	last = g_strdup(md->result);
	if (last != NULL) {
		guint i;
		gboolean present = FALSE;

		for (i = 0; i < recent->len; i++)
			if (g_strcmp0(g_ptr_array_index(recent, i), last) == 0)
				present = TRUE;
		if (!present)
			g_ptr_array_insert(recent, 0, g_steal_pointer(&last));
	}
	if (recent->len > MEET_RECENT_MAX)
		g_ptr_array_set_size(recent, MEET_RECENT_MAX);
	g_ptr_array_unref(md->recent);
	md->recent = recent;
	g_ptr_array_unref(md->models);
	md->models = models;
	md->have_recorder = rec;
	md->have_ffmpeg   = ff;
	md->have_whisper  = wh;
	md->have_emacsctl = ctl;
	md->deps_checked  = TRUE;
	md->want_refresh  = FALSE;
	g_mutex_unlock(&md->lock);
}

static void
meet_refresh_work(GowlBarPlugin *plugin, gpointer user_data)
{
	MeetData *md = user_data;

	meet_refresh_sources(plugin, md);
	meet_refresh_lists(plugin, md);
	gowl_bar_plugin_request_panel_refresh(plugin);
}

/* ----------------------------------------------------------------
 * Starting (worker)
 * ---------------------------------------------------------------- */

typedef struct {
	MeetData  *md;
	guint      generation;
	gchar    **names;
	gchar    **labels;
	gchar     *template;
	gboolean   template_is_default;
	gchar     *encode;       /* encode-command, or NULL for ours */
	gchar     *ffmpeg;
	gchar     *bitrate;
	gboolean   wav;          /* audio-format: wav */
	gchar     *recordings_dir;
	gchar     *title;
} MeetStartJob;

static void
meet_start_job_free(gpointer data)
{
	MeetStartJob *job = data;

	g_strfreev(job->names);
	g_strfreev(job->labels);
	g_free(job->template);
	g_free(job->encode);
	g_free(job->ffmpeg);
	g_free(job->bitrate);
	g_free(job->recordings_dir);
	g_free(job->title);
	g_free(job);
}

/*
 * Spawn one recorder per selected source.  The template is split
 * first and the values substituted into single words after (see
 * bar_meeting_expand_template), so a source name or a recordings path
 * with spaces or quotes is one argument and never a shell.
 */
static void
meet_start_work(GowlBarPlugin *plugin, gpointer user_data)
{
	MeetStartJob *job = user_data;
	MeetData *md = job->md;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *session = NULL;
	g_autofree gchar *session_dir = NULL;
	g_autofree gchar *first_program = NULL;
	g_autofree gchar *ffmpeg_path = NULL;
	g_autofree gchar *codec = NULL;
	GPtrArray *recorders;
	GString *failures;
	const gchar *tmpl;
	const gchar *encode;
	const gchar *ext;
	gboolean piped;
	gboolean wav_fallback = FALSE;
	gboolean wav_file = FALSE;
	guint i;

	now = g_date_time_new_now_local();
	session = bar_meeting_session_name(now);
	session_dir = g_build_filename(job->recordings_dir, session, NULL);
	recorders = g_ptr_array_new_with_free_func(meet_recorder_free);
	failures = g_string_new(NULL);

	/* Recordings are private: 0700, whatever the umask says. */
	if (g_mkdir_with_parents(session_dir, 0700) != 0) {
		g_autofree gchar *msg = g_strdup_printf(
			"cannot create %s: %s", session_dir,
			g_strerror(errno));

		g_mutex_lock(&md->lock);
		if (md->generation == job->generation)
			meet_fail_locked(md, msg);
		g_mutex_unlock(&md->lock);
		g_ptr_array_unref(recorders);
		g_string_free(failures, TRUE);
		return;
	}

	/*
	 * Which recorder, and whether it is piped into an encoder.  A
	 * template that names {output} writes the file itself; one that
	 * does not writes raw PCM, which ffmpeg compresses.  With the
	 * defaults: pw-record into ffmpeg when both are installed, ffmpeg's
	 * pulse input alone without pw-record, and pw-record's own WAV
	 * without ffmpeg (said once in a toast, since that is the big
	 * file this is meant to avoid).  A user's recorder is theirs to
	 * get right.
	 */
	ffmpeg_path = g_find_program_in_path(job->ffmpeg);
	tmpl = job->template;
	if (job->template_is_default) {
		first_program = meet_template_program(tmpl);
		if (first_program == NULL && ffmpeg_path != NULL) {
			tmpl = job->wav ? MEET_FALLBACK_RECORD_WAV
			                : MEET_FALLBACK_RECORD;
			wav_file = job->wav;
		} else if (first_program != NULL &&
		           (ffmpeg_path == NULL || job->wav)) {
			tmpl = MEET_WAV_RECORD;
			wav_file = TRUE;
		}
		wav_fallback = (first_program != NULL &&
		                ffmpeg_path == NULL && !job->wav);
	}
	piped  = (strstr(tmpl, "{output}") == NULL);
	encode = (job->encode != NULL) ? job->encode : MEET_DEFAULT_ENCODE;
	/* The file is what the encoder or the recorder writes: Opus in Ogg
	   unless the recording is a plain WAV. */
	ext = (wav_file || (job->wav && job->encode == NULL && piped))
		? "wav" : "ogg";
	if (piped && job->wav && job->encode == NULL)
		encode = "{ffmpeg} -hide_banner -loglevel error -y -f s16le "
		         "-ar 16000 -ac 1 -i pipe:0 {output}";
	codec = g_strdup_printf("-c:a libopus -b:a %s -application voip",
	                        job->bitrate);

	for (i = 0; job->names[i] != NULL; i++) {
		g_autofree gchar *file = NULL;
		g_autofree gchar *path = NULL;
		g_autofree gchar *log = NULL;
		g_autofree gchar *rec_tmpl = NULL;
		g_auto(GStrv) argv = NULL;
		g_auto(GStrv) enc_argv = NULL;
		GError *error = NULL;
		GPid pid = 0;
		GPid enc_pid = 0;
		gboolean ok;
		const gchar *pairs[11];

		file = bar_meeting_recording_filename(i, job->names[i], ext);
		path = g_build_filename(session_dir, file, NULL);
		log  = g_strdup_printf("%.*s.log",
			(gint)(strlen(path) - strlen(ext) - 1), path);

		pairs[0] = "source";  pairs[1] = job->names[i];
		pairs[2] = "output";  pairs[3] = path;
		pairs[4] = "ffmpeg";  pairs[5] = job->ffmpeg;
		pairs[6] = "bitrate"; pairs[7] = job->bitrate;
		pairs[8] = "codec";   pairs[9] = codec;
		pairs[10] = NULL;
		/* {codec} is several words: substituted into the template
		   before it is split, unlike a value, which is one word. */
		rec_tmpl = meet_replace(tmpl, "{codec}", codec);
		argv = bar_meeting_expand_template(rec_tmpl, pairs, &error);
		if (argv != NULL && piped)
			enc_argv = bar_meeting_expand_template(encode, pairs,
			                                       &error);
		if (argv == NULL || (piped && enc_argv == NULL))
			ok = FALSE;
		else if (piped)
			ok = meet_spawn_piped(argv, enc_argv, log, &pid,
			                      &enc_pid, &error);
		else
			ok = meet_spawn(argv, log, &pid, &error);
		if (!ok) {
			g_string_append_printf(failures, "%s%s: %s",
				(failures->len > 0) ? "; " : "",
				job->labels[i],
				(error != NULL) ? error->message
				                : "cannot run the recorder");
			g_clear_error(&error);
			continue;
		}
		{
			MeetRecorder *r = g_new0(MeetRecorder, 1);

			r->pid     = pid;
			r->enc_pid = enc_pid;
			r->source  = g_strdup(job->names[i]);
			r->label   = g_strdup(job->labels[i]);
			r->path    = g_steal_pointer(&path);
			r->log     = g_steal_pointer(&log);
			r->running = TRUE;
			g_ptr_array_add(recorders, r);
		}
	}

	g_mutex_lock(&md->lock);
	if (md->generation != job->generation ||
	    md->phase != BAR_MEETING_STARTING) {
		/* Cancelled while we were spawning: stop what started and
		   leave it to the async poll to collect. */
		for (i = 0; i < recorders->len; i++) {
			MeetRecorder *r = g_ptr_array_index(recorders, i);

			meet_orphan_locked(md, r->pid, SIGINT);
			meet_orphan_locked(md, r->enc_pid, SIGINT);
		}
		g_mutex_unlock(&md->lock);
		g_ptr_array_unref(recorders);
		g_string_free(failures, TRUE);
		return;
	}

	if (recorders->len == 0) {
		g_autofree gchar *msg = g_strdup_printf(
			"no recorder started (%s). Install pipewire-utils "
			"for pw-record, or ffmpeg-free, or set "
			"meeting-notes.record-command",
			(failures->len > 0) ? failures->str : "no sources");

		meet_fail_locked(md, msg);
		g_mutex_unlock(&md->lock);
		g_ptr_array_unref(recorders);
		g_string_free(failures, TRUE);
		return;
	}

	g_ptr_array_unref(md->recorders);
	md->recorders = recorders;
	g_free(md->session_dir);
	md->session_dir = g_steal_pointer(&session_dir);
	g_clear_pointer(&md->started_at, g_date_time_unref);
	md->started_at = g_date_time_ref(now);
	g_free(md->meeting_title);
	md->meeting_title = g_strdup(job->title);
	md->rec_started = g_get_monotonic_time();
	md->rec_stopped = 0;
	md->stop_sent = 0;
	md->stop_stage = 0;
	g_clear_pointer(&md->error, g_free);
	meet_set_phase_locked(md, BAR_MEETING_RECORDING);
	if (wav_fallback)
		meet_toast_locked(md, GOWL_BAR_TOAST_NORMAL,
		                  "Recording uncompressed",
		                  "ffmpeg is not installed, so this meeting is "
		                  "being recorded as WAV, about 115 MB an hour "
		                  "per source.  Install ffmpeg-free for Opus.",
		                  NULL);
	if (failures->len > 0)
		meet_toast_locked(md, GOWL_BAR_TOAST_NORMAL,
		                  "Some sources are not being recorded",
		                  failures->str, NULL);
	g_mutex_unlock(&md->lock);
	g_string_free(failures, TRUE);
	gowl_bar_plugin_request_panel_refresh(plugin);
}

/* Begin a meeting.  Compositor thread: snapshots, then hands the
   spawning to a worker. */
static void
meet_start(GowlBarPlugin *plugin, MeetData *md)
{
	MeetStartJob *job;
	GPtrArray *names;
	GPtrArray *labels;
	GPtrArray *chosen;
	const gchar *tmpl;
	guint i, j;
	gboolean warn = FALSE;

	g_mutex_lock(&md->lock);
	if (bar_meeting_phase_is_busy(md->phase)) {
		g_mutex_unlock(&md->lock);
		return;
	}
	if (md->selection == NULL || md->selection[0] == NULL) {
		meet_fail_locked(md, md->selection == NULL
			? "the source list has not been read yet; try again "
			  "in a moment"
			: "no source is selected; choose one in the panel");
		g_mutex_unlock(&md->lock);
		meet_raise_toasts(plugin, md);
		meet_apply_look(plugin, md);
		return;
	}

	names  = g_ptr_array_new_with_free_func(g_free);
	labels = g_ptr_array_new_with_free_func(g_free);
	chosen = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_meeting_source_free);
	for (i = 0; md->selection[i] != NULL; i++) {
		const gchar *name = md->selection[i];
		const BarMeetingSource *known = NULL;

		for (j = 0; j < md->sources->len; j++) {
			BarMeetingSource *s = g_ptr_array_index(md->sources, j);

			if (strcmp(s->name, name) == 0)
				known = s;
		}
		/* A saved source that is not plugged in today is skipped
		   rather than handed to a recorder that will fail on it ---
		   unless pactl could not list anything, when trusting the
		   saved list is all there is. */
		if (known == NULL && md->sources->len > 0)
			continue;
		g_ptr_array_add(names, g_strdup(name));
		g_ptr_array_add(labels, g_strdup(known != NULL
			? known->description : name));
		g_ptr_array_add(chosen, bar_meeting_source_new(name,
			known != NULL ? known->description : name,
			known != NULL ? known->monitor
			              : g_str_has_suffix(name, ".monitor")));
	}
	if (names->len == 0) {
		meet_fail_locked(md, "none of the selected sources is "
		                     "available; choose one in the panel");
		g_mutex_unlock(&md->lock);
		g_ptr_array_unref(names);
		g_ptr_array_unref(labels);
		g_ptr_array_unref(chosen);
		meet_raise_toasts(plugin, md);
		meet_apply_look(plugin, md);
		return;
	}
	if (!md->echo_warned && bar_meeting_echo_risk(chosen)) {
		md->echo_warned = TRUE;
		warn = TRUE;
	}

	md->generation++;
	meet_set_phase_locked(md, BAR_MEETING_STARTING);
	g_clear_pointer(&md->error, g_free);
	g_clear_pointer(&md->job_id, g_free);
	g_clear_pointer(&md->cmacs_seen, g_free);
	g_clear_pointer(&md->transcript, g_free);
	g_clear_pointer(&md->summary_note, g_free);

	job = g_new0(MeetStartJob, 1);
	job->md = md;
	job->generation = md->generation;
	job->title = (md->title != NULL && md->title[0] != '\0')
		? g_strdup(md->title) : NULL;
	g_mutex_unlock(&md->lock);

	g_ptr_array_add(names, NULL);
	g_ptr_array_add(labels, NULL);
	job->names  = (gchar **)g_ptr_array_free(names, FALSE);
	job->labels = (gchar **)g_ptr_array_free(labels, FALSE);
	g_ptr_array_unref(chosen);

	tmpl = gowl_bar_plugin_get_setting(plugin, "record-command");
	job->template_is_default = (tmpl == NULL || tmpl[0] == '\0');
	job->template = g_strdup(job->template_is_default
	                         ? MEET_DEFAULT_RECORD : tmpl);
	tmpl = gowl_bar_plugin_get_setting(plugin, "encode-command");
	job->encode  = (tmpl != NULL && tmpl[0] != '\0') ? g_strdup(tmpl)
	                                                 : NULL;
	job->ffmpeg  = g_strdup(meet_setting(plugin, "ffmpeg", "ffmpeg"));
	job->bitrate = g_strdup(meet_bitrate_ok(meet_setting(plugin,
	                                 "audio-bitrate", MEET_DEFAULT_BITRATE))
		? meet_setting(plugin, "audio-bitrate", MEET_DEFAULT_BITRATE)
		: MEET_DEFAULT_BITRATE);
	job->wav     = (g_ascii_strcasecmp(meet_setting(plugin,
	                "audio-format", "ogg"), "wav") == 0);
	job->recordings_dir = meet_path_setting(plugin, "recordings-dir",
	                                        MEET_DEFAULT_RECORDINGS);

	if (warn)
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
			"Speech may be recorded twice",
			"A microphone and the speakers it can hear are both "
			"selected; with speakers rather than headphones the "
			"other side may appear twice in the transcript.");

	meet_apply_look(plugin, md);
	gowl_bar_plugin_queue_work(plugin, meet_start_work, job,
	                           meet_start_job_free);
	/* Again: a host that ran the work inline (a test's) has already
	   moved on to RECORDING or ERROR. */
	meet_apply_look(plugin, md);
}

/* Ask every running recorder to finish.  Must hold md->lock. */
static void
meet_stop_locked(MeetData *md)
{
	guint i;

	if (md->phase != BAR_MEETING_RECORDING)
		return;
	for (i = 0; i < md->recorders->len; i++) {
		MeetRecorder *r = g_ptr_array_index(md->recorders, i);

		/* The recorder only: its encoder finishes the file when
		   the pipe closes, and a SIGINT of its own could cut the
		   last buffer off. */
		if (r->running)
			meet_recorder_kill(r, SIGINT, FALSE);
	}
	md->rec_stopped = g_get_monotonic_time();
	md->stop_sent = md->rec_stopped;
	md->stop_stage = 0;
	meet_set_phase_locked(md, BAR_MEETING_STOPPING);
}

/*
 * Abandon whatever is in progress: recorders are stopped (their files
 * are kept), a running ffmpeg or whisper is terminated, a cmacs job is
 * forgotten.  Compositor thread.
 */
static void
meet_cancel(GowlBarPlugin *plugin, MeetData *md)
{
	guint i;

	g_mutex_lock(&md->lock);
	md->generation++;
	for (i = 0; i < md->recorders->len; i++) {
		MeetRecorder *r = g_ptr_array_index(md->recorders, i);

		if (r->running) {
			meet_orphan_locked(md, r->pid, SIGINT);
			meet_orphan_locked(md, r->enc_pid, SIGINT);
			r->pid = r->enc_pid = 0;
			r->running = FALSE;
		}
	}
	if (md->child > 0) {
		meet_orphan_locked(md, md->child, SIGTERM);
		md->child = 0;
	}
	g_clear_pointer(&md->job_id, g_free);
	g_clear_pointer(&md->error, g_free);
	meet_set_phase_locked(md, BAR_MEETING_IDLE);
	if (md->session_dir != NULL)
		meet_toast_locked(md, GOWL_BAR_TOAST_LOW, "Meeting cancelled",
		                  md->session_dir, NULL);
	g_mutex_unlock(&md->lock);
	bar_ai_cancel(md->ai);
	meet_raise_toasts(plugin, md);
	meet_apply_look(plugin, md);
}

/* ----------------------------------------------------------------
 * After the recording (worker, from the async poll)
 * ---------------------------------------------------------------- */

/*
 * The recordings that hold audio, as absolute paths.  Worker only.
 * Must NOT hold the lock: it stats files.
 */
static gchar **
meet_usable_recordings(MeetData *md)
{
	GPtrArray *paths;
	GPtrArray *out;
	guint i;

	paths = g_ptr_array_new_with_free_func(g_free);
	g_mutex_lock(&md->lock);
	for (i = 0; i < md->recorders->len; i++)
		g_ptr_array_add(paths, g_strdup(((MeetRecorder *)
			g_ptr_array_index(md->recorders, i))->path));
	g_mutex_unlock(&md->lock);

	out = g_ptr_array_new();
	for (i = 0; i < paths->len; i++)
		if (meet_has_audio(g_ptr_array_index(paths, i)))
			g_ptr_array_add(out,
				g_strdup(g_ptr_array_index(paths, i)));
	g_ptr_array_add(out, NULL);
	g_ptr_array_unref(paths);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

/*
 * Delete the WAV made for whisper once nothing can still want it: not
 * mixing or transcribing, and no child of ours left that might be
 * writing it (a cancelled ffmpeg is an orphan until collected).  The
 * recordings themselves stay.  Worker only.
 */
static void
meet_drop_transient(MeetData *md)
{
	g_autofree gchar *path = NULL;

	g_mutex_lock(&md->lock);
	if (md->mixed_transient && md->mixed != NULL && md->child == 0 &&
	    md->orphans->len == 0 &&
	    md->phase != BAR_MEETING_MIXING &&
	    md->phase != BAR_MEETING_TRANSCRIBING) {
		path = g_steal_pointer(&md->mixed);
		md->mixed_transient = FALSE;
	}
	g_mutex_unlock(&md->lock);
	if (path != NULL && g_unlink(path) != 0 && errno != ENOENT)
		g_warning("meeting-notes: could not delete %s: %s", path,
		          g_strerror(errno));
}

/* Hand the session to cmacs.  Worker only (one emacsctl round trip). */
static void
meet_send_to_cmacs(GowlBarPlugin *plugin, MeetData *md, guint gen,
                   gchar **files)
{
	g_autofree gchar *session = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *out = NULL;
	GPtrArray *rel;
	const gchar *argv[4];
	guint i;

	g_mutex_lock(&md->lock);
	session = g_strdup(md->session_dir);
	title = g_strdup(md->meeting_title);
	id = g_strdup_printf("mn-%u-%08x", gen, g_random_int());
	g_free(md->job_id);
	md->job_id = g_strdup(id);
	g_clear_pointer(&md->cmacs_seen, g_free);
	g_mutex_unlock(&md->lock);

	/* Names relative to the session: cmacs expands them there, and
	   the form stays short. */
	rel = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; files[i] != NULL; i++)
		g_ptr_array_add(rel, g_path_get_basename(files[i]));
	g_ptr_array_add(rel, NULL);
	form = bar_meeting_cmacs_form(MEET_WIDGET, id, session,
		(const gchar * const *)rel->pdata, title,
		gowl_bar_plugin_get_setting(plugin, "destination"));
	g_ptr_array_unref(rel);

	if (form == NULL) {
		g_mutex_lock(&md->lock);
		if (md->generation == gen)
			meet_fail_locked(md, "the session path or title is "
			                     "not valid UTF-8 text");
		g_mutex_unlock(&md->lock);
		return;
	}

	argv[0] = meet_setting(plugin, "emacsctl", "emacsctl");
	argv[1] = "eval";
	argv[2] = form;
	argv[3] = NULL;
	out = bar_run_argv(argv);

	g_mutex_lock(&md->lock);
	if (md->generation == gen) {
		if (out == NULL) {
			g_autofree gchar *msg = g_strdup_printf(
				"could not reach cmacs (is emacsctl on PATH "
				"and cmacs running?); the recordings are in %s",
				session);

			meet_fail_locked(md, msg);
		} else if (md->phase == BAR_MEETING_FILING) {
			/* cmacs will say mixing/transcribing/...; until the
			   first push, say what it is about to do. */
			meet_set_phase_locked(md, BAR_MEETING_MIXING);
		}
	}
	g_mutex_unlock(&md->lock);
}

/* Start whisper on the mixed file.  Worker only. */
static void
meet_start_transcription(GowlBarPlugin *plugin, MeetData *md, guint gen)
{
	g_autofree gchar *model = NULL;
	g_autofree gchar *session = NULL;
	g_autofree gchar *input = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *log = NULL;
	g_autofree gchar *threads = NULL;
	g_auto(GStrv) argv = NULL;
	GError *error = NULL;
	GPid pid = 0;
	const gchar *pairs[13];

	/* Set in the config, or by a model row in the panel. */
	if (gowl_bar_plugin_get_setting(plugin, "whisper-model") != NULL)
		model = meet_path_setting(plugin, "whisper-model", NULL);
	g_mutex_lock(&md->lock);
	session = g_strdup(md->session_dir);
	input   = g_strdup(md->mixed);
	g_mutex_unlock(&md->lock);

	if (model == NULL || !g_file_test(model, G_FILE_TEST_EXISTS)) {
		g_autofree gchar *msg = g_strdup_printf(
			"%s: set meeting-notes.whisper-model to a whisper.cpp "
			"model (or pick one in the panel); the recordings are "
			"in %s",
			(model == NULL) ? "no whisper model is configured"
			                : "the whisper model does not exist",
			session);

		g_mutex_lock(&md->lock);
		if (md->generation == gen)
			meet_fail_locked(md, msg);
		g_mutex_unlock(&md->lock);
		return;
	}

	base    = g_build_filename(session, "transcript", NULL);
	log     = g_build_filename(session, "transcribe.log", NULL);
	threads = g_strdup_printf("%d", gowl_bar_plugin_get_setting_int(
		plugin, "whisper-threads", 4));
	pairs[0]  = "model";       pairs[1]  = model;
	pairs[2]  = "language";    pairs[3]  = meet_setting(plugin,
		"language", "auto");
	pairs[4]  = "threads";     pairs[5]  = threads;
	pairs[6]  = "output_base"; pairs[7]  = base;
	pairs[8]  = "input";       pairs[9]  = input;
	pairs[10] = "output";      pairs[11] = base;
	pairs[12] = NULL;

	argv = bar_meeting_expand_template(
		meet_setting(plugin, "transcribe-command",
		             MEET_DEFAULT_TRANSCRIBE), pairs, &error);
	if (argv == NULL || !meet_spawn(argv, log, &pid, &error)) {
		g_autofree gchar *msg = g_strdup_printf(
			"cannot run the transcriber (%s); install whisper-cli "
			"(whisper.cpp) or set meeting-notes.transcribe-command",
			(error != NULL) ? error->message : "bad template");

		g_clear_error(&error);
		g_mutex_lock(&md->lock);
		if (md->generation == gen)
			meet_fail_locked(md, msg);
		g_mutex_unlock(&md->lock);
		return;
	}

	g_mutex_lock(&md->lock);
	if (md->generation != gen) {
		meet_orphan_locked(md, pid, SIGTERM);
	} else {
		md->child = pid;
		g_free(md->child_log);
		md->child_log = g_steal_pointer(&log);
		g_free(md->transcript_base);
		md->transcript_base = g_steal_pointer(&base);
		meet_set_phase_locked(md, BAR_MEETING_TRANSCRIBING);
	}
	g_mutex_unlock(&md->lock);
}

/* The recorders are done: mix or decode them into the WAV whisper
   reads, or go straight to whisper when the one recording already is a
   WAV.  Worker only. */
static void
meet_process_local(GowlBarPlugin *plugin, MeetData *md, guint gen,
                   gchar **files)
{
	g_autofree gchar *session = NULL;
	g_autofree gchar *mixed = NULL;
	g_autofree gchar *log = NULL;
	g_auto(GStrv) argv = NULL;
	GError *error = NULL;
	GPid pid = 0;

	g_mutex_lock(&md->lock);
	session = g_strdup(md->session_dir);
	g_mutex_unlock(&md->lock);

	if (files[1] == NULL && g_str_has_suffix(files[0], ".wav")) {
		/* One WAV recording is already what whisper wants --- and
		   it is the recording, so it is not ours to delete. */
		g_mutex_lock(&md->lock);
		g_free(md->mixed);
		md->mixed = g_strdup(files[0]);
		md->mixed_transient = FALSE;
		g_mutex_unlock(&md->lock);
		meet_start_transcription(plugin, md, gen);
		return;
	}

	mixed = g_build_filename(session, "mixed.wav", NULL);
	log   = g_build_filename(session, "mix.log", NULL);
	argv  = bar_meeting_mix_argv(meet_setting(plugin, "ffmpeg", "ffmpeg"),
	                             (const gchar * const *)files, mixed);
	if (argv == NULL || !meet_spawn(argv, log, &pid, &error)) {
		g_autofree gchar *msg = g_strdup_printf(
			"cannot mix the recordings (%s); install ffmpeg-free; "
			"the recordings are in %s",
			(error != NULL) ? error->message : "no inputs", session);

		g_clear_error(&error);
		g_mutex_lock(&md->lock);
		if (md->generation == gen)
			meet_fail_locked(md, msg);
		g_mutex_unlock(&md->lock);
		return;
	}

	g_mutex_lock(&md->lock);
	if (md->generation != gen) {
		meet_orphan_locked(md, pid, SIGTERM);
	} else {
		md->child = pid;
		g_free(md->child_log);
		md->child_log = g_steal_pointer(&log);
		g_free(md->mixed);
		md->mixed = g_steal_pointer(&mixed);
		md->mixed_transient = TRUE;
		meet_set_phase_locked(md, BAR_MEETING_MIXING);
	}
	g_mutex_unlock(&md->lock);
}

/* What the summary prompt asks for; the notes builder relies on the
   `Action items' heading to give them their own section. */
static gchar *
meet_summary_system(GowlBarPlugin *plugin)
{
	const gchar *custom = gowl_bar_plugin_get_setting(plugin,
	                                                  "summary-system");
	gboolean md_fmt;

	if (custom != NULL && custom[0] != '\0')
		return g_strdup(custom);
	md_fmt = (g_strcmp0(meet_setting(plugin, "format", "org"), "md")
	          == 0);
	return g_strdup_printf(
		"You summarise meeting transcripts for the notes of someone "
		"who attended. Reply in %s with exactly these headings: "
		"Summary (three to six sentences), Decisions, Action items "
		"(one per line as \"- [ ] OWNER: task\", naming the owner "
		"when the transcript makes it clear), Open questions. Leave "
		"out a heading that would be empty. No preamble, no code "
		"fences.",
		md_fmt ? "Markdown" : "Org-mode markup");
}

/* Write the notes and finish.  Worker only. */
static void
meet_file_notes(GowlBarPlugin *plugin, MeetData *md, guint gen,
                const gchar *summary, const gchar *note)
{
	BarMeetingDoc doc;
	g_autofree gchar *outdir = NULL;
	g_autofree gchar *fname = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *raw_title = NULL;
	g_autofree gchar *transcript = NULL;
	g_autofree gchar *session = NULL;
	g_autofree gchar *open = NULL;
	g_autoptr(GDateTime) when = NULL;
	const gchar *format;
	const gchar *author;
	gint64 duration;
	guint n_sources;
	GError *error = NULL;

	format = (g_strcmp0(meet_setting(plugin, "format", "org"), "md") == 0)
		? "md" : "org";
	outdir = meet_path_setting(plugin, "output-dir", MEET_DEFAULT_OUTPUT);

	g_mutex_lock(&md->lock);
	when = (md->started_at != NULL) ? g_date_time_ref(md->started_at)
	                                : g_date_time_new_now_local();
	raw_title = g_strdup(md->meeting_title);
	transcript = g_strdup(md->transcript);
	session = g_strdup(md->session_dir);
	duration = (md->rec_stopped - md->rec_started) / G_USEC_PER_SEC;
	n_sources = md->recorders->len;
	meet_set_phase_locked(md, BAR_MEETING_FILING);
	g_mutex_unlock(&md->lock);

	title = (raw_title != NULL) ? g_strdup(raw_title)
	                            : bar_meeting_default_title(when);
	author = g_get_real_name();
	if (author == NULL || g_strcmp0(author, "Unknown") == 0)
		author = g_get_user_name();

	memset(&doc, 0, sizeof(doc));
	doc.format       = format;
	doc.title        = title;
	doc.created      = when;
	doc.author       = author;
	doc.summary      = summary;
	doc.summary_note = note;
	doc.transcript   = transcript;
	doc.duration_s   = duration;
	doc.n_sources    = n_sources;
	doc.recordings   = session;
	text = bar_meeting_document(&doc);

	fname = bar_meeting_notes_filename(when, raw_title, format);
	g_mkdir_with_parents(outdir, 0755);
	path = g_build_filename(outdir, fname, NULL);
	{
		guint n = 2;

		/* Never overwrite: two meetings in one minute with the same
		   title are two files. */
		while (g_file_test(path, G_FILE_TEST_EXISTS) && n < 100) {
			g_autofree gchar *stem = g_strndup(fname,
				strlen(fname) - strlen(format) - 1);

			g_free(path);
			path = g_strdup_printf("%s/%s_%u.%s", outdir, stem, n++,
			                       format);
		}
	}

	if (!g_file_set_contents(path, text, -1, &error)) {
		g_mutex_lock(&md->lock);
		if (md->generation == gen)
			meet_fail_locked(md, error->message);
		g_mutex_unlock(&md->lock);
		g_clear_error(&error);
		return;
	}

	open = meet_open_command(plugin, path);
	g_mutex_lock(&md->lock);
	if (md->generation == gen) {
		g_free(md->result);
		md->result = g_strdup(path);
		meet_set_phase_locked(md, BAR_MEETING_DONE);
		meet_toast_locked(md, GOWL_BAR_TOAST_NORMAL,
			(note != NULL) ? "Meeting notes saved (no summary)"
			               : "Meeting notes saved",
			path, open);
		g_clear_pointer(&md->title, g_free);
		md->title_rev++;
		md->want_refresh = TRUE;
	}
	g_mutex_unlock(&md->lock);
}

/* whisper finished: read its transcript and summarise or file.
   Worker only. */
static void
meet_after_transcription(GowlBarPlugin *plugin, MeetData *md, guint gen,
                         gint status)
{
	g_autofree gchar *base = NULL;
	g_autofree gchar *log = NULL;
	g_autofree gchar *txt = NULL;
	g_autofree gchar *raw = NULL;
	g_autofree gchar *clean = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *system = NULL;
	g_autofree gchar *prompt = NULL;
	GStatBuf st;
	GError *error = NULL;

	g_mutex_lock(&md->lock);
	base = g_strdup(md->transcript_base);
	log  = g_strdup(md->child_log);
	g_mutex_unlock(&md->lock);

	txt = g_strdup_printf("%s.txt", base);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
	    g_stat(txt, &st) != 0) {
		g_autofree gchar *tail = meet_log_tail(log);
		g_autofree gchar *msg = g_strdup_printf(
			"transcription failed%s%s",
			(tail != NULL) ? ": " : "",
			(tail != NULL) ? tail : "");

		g_mutex_lock(&md->lock);
		if (md->generation == gen)
			meet_fail_locked(md, msg);
		g_mutex_unlock(&md->lock);
		return;
	}
	if (st.st_size > MEET_TRANSCRIPT_MAX ||
	    !g_file_get_contents(txt, &raw, NULL, NULL)) {
		g_mutex_lock(&md->lock);
		if (md->generation == gen)
			meet_fail_locked(md, "the transcript could not be "
			                     "read, or is implausibly large");
		g_mutex_unlock(&md->lock);
		return;
	}
	clean = bar_meeting_clean_text(g_strstrip(raw), MEET_TRANSCRIPT_MAX);

	g_mutex_lock(&md->lock);
	if (md->generation != gen) {
		g_mutex_unlock(&md->lock);
		return;
	}
	g_free(md->transcript);
	md->transcript = g_strdup(clean);
	g_mutex_unlock(&md->lock);

	if (clean[0] == '\0') {
		meet_file_notes(plugin, md, gen, NULL,
		                "Nothing was said, or nothing whisper could "
		                "hear.");
		return;
	}
	if (!gowl_bar_plugin_get_setting_bool(plugin, "summary", TRUE)) {
		meet_file_notes(plugin, md, gen, NULL,
		                "Summaries are turned off "
		                "(meeting-notes.summary).");
		return;
	}

	system = meet_summary_system(plugin);
	{
		g_autofree gchar *cut = bar_meeting_clean_text(clean,
		                                               MEET_PROMPT_MAX);

		prompt = g_strdup_printf(
			"Summarise this meeting transcript.\n\n%s", cut);
	}
	g_mutex_lock(&md->lock);
	if (md->generation == gen)
		meet_set_phase_locked(md, BAR_MEETING_SUMMARISING);
	g_mutex_unlock(&md->lock);

	if (!bar_ai_start(md->ai, plugin, system, prompt, &error)) {
		why = g_strdup_printf("No summary: %s.", error->message);
		g_clear_error(&error);
		meet_file_notes(plugin, md, gen, NULL, why);
	}
}

/* ----------------------------------------------------------------
 * The async poll: the state machine
 * ---------------------------------------------------------------- */

/*
 * Collect the recorders.  Must hold md->lock; never blocks.  A recorder
 * that exits while the phase is still RECORDING died on its own, and
 * says so.
 */
static guint
meet_reap_recorders_locked(MeetData *md)
{
	guint i, alive = 0;

	for (i = 0; i < md->recorders->len; i++) {
		MeetRecorder *r = g_ptr_array_index(md->recorders, i);

		gboolean lost = FALSE;

		if (!r->running)
			continue;
		if (r->pid > 0 && meet_reap(r->pid, NULL)) {
			r->pid = 0;
			lost = TRUE;
		}
		if (r->enc_pid > 0 && meet_reap(r->enc_pid, NULL)) {
			r->enc_pid = 0;
			lost = TRUE;
		}
		/* Either half dying while we still record is an early
		   death.  A recorder whose encoder died is writing into a
		   closed pipe: stop it rather than wait for SIGPIPE. */
		if (lost && md->phase == BAR_MEETING_RECORDING && !r->lost) {
			r->lost  = TRUE;
			r->early = TRUE;
			meet_recorder_kill(r, SIGINT, FALSE);
		}
		if (r->pid > 0 || r->enc_pid > 0) {
			alive++;
			continue;
		}
		r->running = FALSE;
	}
	return alive;
}

static void
meet_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	MeetData *md = data;
	BarMeetingPhase phase;
	gboolean refresh_sources, refresh_lists;
	guint gen, i, alive;
	gint64 now;
	gdouble max_min;
	gint grace;
	GPtrArray *early;
	GPid child;
	gint status = 0;
	gboolean child_done = FALSE;
	gboolean recorded = FALSE;

	now = g_get_monotonic_time();

	/* Children we gave up on (cancel, a late start) are still ours to
	   collect, or they linger as zombies. */
	grace = gowl_bar_plugin_get_setting_int(plugin, "stop-grace", 5);
	g_mutex_lock(&md->lock);
	for (i = 0; i < md->orphans->len; ) {
		MeetOrphan *o = &g_array_index(md->orphans, MeetOrphan, i);
		gint64 waited = now - o->since;

		if (meet_reap(o->pid, NULL)) {
			g_array_remove_index_fast(md->orphans, i);
			continue;
		}
		if (o->stage == 0 &&
		    waited >= (gint64)grace * G_USEC_PER_SEC) {
			kill((pid_t)o->pid, SIGTERM);
			o->stage = 1;
		} else if (o->stage == 1 &&
		           waited >= (gint64)(grace + MEET_KILL_AFTER_TERM) *
		                      G_USEC_PER_SEC) {
			kill((pid_t)o->pid, SIGKILL);
			o->stage = 2;
		}
		i++;
	}
	refresh_sources = !md->sources_loaded ||
	                  (md->panel_open &&
	                   now - md->sources_at > MEET_SOURCES_TTL_US);
	refresh_lists = md->want_refresh && md->panel_open;
	g_mutex_unlock(&md->lock);

	if (refresh_sources)
		meet_refresh_sources(plugin, md);
	if (refresh_lists)
		meet_refresh_lists(plugin, md);
	if (refresh_sources || refresh_lists)
		gowl_bar_plugin_request_panel_refresh(plugin);

	max_min = gowl_bar_plugin_get_setting_double(plugin, "max-minutes",
	                                             240.0);
	early   = g_ptr_array_new_with_free_func(g_free);

	g_mutex_lock(&md->lock);
	phase = md->phase;
	gen   = md->generation;

	if (phase == BAR_MEETING_RECORDING) {
		alive = meet_reap_recorders_locked(md);
		for (i = 0; i < md->recorders->len; i++) {
			MeetRecorder *r = g_ptr_array_index(md->recorders, i);

			if (r->early) {
				r->early = FALSE;
				g_ptr_array_add(early, g_strdup(r->label));
				g_ptr_array_add(early, g_strdup(r->log));
			}
		}
		if (alive == 0) {
			/* Every recorder is gone: process what there is. */
			md->rec_stopped = now;
			md->stop_sent = now;
			meet_set_phase_locked(md, BAR_MEETING_STOPPING);
		} else if (max_min > 0.0 &&
		           (gdouble)(now - md->rec_started) >=
		           max_min * 60.0 * (gdouble)G_USEC_PER_SEC) {
			g_autofree gchar *limit = bar_meeting_format_clock(
				(gint64)(max_min * 60.0));
			g_autofree gchar *body = g_strdup_printf(
				"The recording reached its limit of %s "
				"(meeting-notes.max-minutes) and was stopped.",
				limit);

			meet_stop_locked(md);
			meet_toast_locked(md, GOWL_BAR_TOAST_NORMAL,
			                  "Meeting recording stopped", body,
			                  NULL);
		}
	}

	if (md->phase == BAR_MEETING_STOPPING) {
		gint64 waited = now - md->stop_sent;

		alive = meet_reap_recorders_locked(md);
		if (alive > 0 && md->stop_stage == 0 &&
		    waited >= (gint64)grace * G_USEC_PER_SEC) {
			/* SIGINT was not enough.  SIGTERM loses the last
			   buffer but keeps what is on disk. */
			for (i = 0; i < md->recorders->len; i++) {
				MeetRecorder *r = g_ptr_array_index(
					md->recorders, i);

				if (r->running)
					meet_recorder_kill(r, SIGTERM, TRUE);
			}
			md->stop_stage = 1;
		} else if (alive > 0 && md->stop_stage == 1 &&
		           waited >= (gint64)(grace + MEET_KILL_AFTER_TERM) *
		                      G_USEC_PER_SEC) {
			for (i = 0; i < md->recorders->len; i++) {
				MeetRecorder *r = g_ptr_array_index(
					md->recorders, i);

				if (r->running)
					meet_recorder_kill(r, SIGKILL, TRUE);
			}
			md->stop_stage = 2;
		}
		if (alive == 0) {
			/* Claim the next step so nothing else starts it.
			   FILING is a placeholder the backend replaces
			   within this call. */
			meet_set_phase_locked(md, BAR_MEETING_FILING);
			recorded = TRUE;
		}
	} else if (md->phase == BAR_MEETING_MIXING ||
	           md->phase == BAR_MEETING_TRANSCRIBING) {
		child = md->child;
		if (child > 0 && meet_reap(child, &status)) {
			md->child = 0;
			child_done = TRUE;
		}
	}
	phase = md->phase;
	g_mutex_unlock(&md->lock);

	/* Early deaths, said once each, with the recorder's last words. */
	for (i = 0; i + 1 < early->len; i += 2) {
		g_autofree gchar *tail = meet_log_tail(
			g_ptr_array_index(early, i + 1));
		g_autofree gchar *body = g_strdup_printf("%s%s%s",
			(const gchar *)g_ptr_array_index(early, i),
			(tail != NULL) ? ": " : "",
			(tail != NULL) ? tail : "");

		g_mutex_lock(&md->lock);
		meet_toast_locked(md, GOWL_BAR_TOAST_NORMAL,
		                  "A recorder stopped early", body, NULL);
		g_mutex_unlock(&md->lock);
	}
	g_ptr_array_unref(early);

	if (recorded) {
		g_auto(GStrv) files = meet_usable_recordings(md);

		if (files[0] == NULL) {
			g_mutex_lock(&md->lock);
			if (md->generation == gen)
				meet_fail_locked(md, "nothing was recorded; "
					"check the sources and the recorder logs "
					"in the session directory");
			g_mutex_unlock(&md->lock);
		} else if (meet_backend_is_cmacs(plugin)) {
			meet_send_to_cmacs(plugin, md, gen, files);
		} else {
			meet_process_local(plugin, md, gen, files);
		}
	} else if (phase == BAR_MEETING_MIXING && child_done) {
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			meet_start_transcription(plugin, md, gen);
		} else {
			g_autofree gchar *log = NULL;
			g_autofree gchar *tail = NULL;
			g_autofree gchar *msg = NULL;

			g_mutex_lock(&md->lock);
			log = g_strdup(md->child_log);
			g_mutex_unlock(&md->lock);
			tail = meet_log_tail(log);
			msg = g_strdup_printf("mixing failed%s%s",
				(tail != NULL) ? ": " : "",
				(tail != NULL) ? tail : "");
			g_mutex_lock(&md->lock);
			if (md->generation == gen)
				meet_fail_locked(md, msg);
			g_mutex_unlock(&md->lock);
		}
	} else if (phase == BAR_MEETING_TRANSCRIBING && child_done) {
		meet_after_transcription(plugin, md, gen, status);
	}
	meet_drop_transient(md);

	/* A summary in flight (local backend): file once it is in, with or
	   without it --- the transcript is never lost to a model error. */
	g_mutex_lock(&md->lock);
	phase = md->phase;
	gen   = md->generation;
	g_mutex_unlock(&md->lock);
	if (phase == BAR_MEETING_SUMMARISING && !meet_backend_is_cmacs(plugin)) {
		g_autofree gchar *text = NULL;
		g_autofree gchar *err = NULL;
		BarAiState st;

		st = bar_ai_get(md->ai, &text, &err);
		if (st == BAR_AI_DONE) {
			meet_file_notes(plugin, md, gen, text, NULL);
		} else if (st == BAR_AI_ERROR || st == BAR_AI_IDLE) {
			g_autofree gchar *why = g_strdup_printf(
				"No summary: %s.", (err != NULL) ? err
				: "the request was abandoned");

			meet_file_notes(plugin, md, gen, NULL, why);
		}
	}

	/* The label follows on the next sync poll, a second away at most:
	   bar_plugin_apply_color() may write a setting, which is not a
	   worker's to do. */
	gowl_bar_plugin_request_panel_refresh(plugin);
}

/* ----------------------------------------------------------------
 * cmacs pushes
 * ---------------------------------------------------------------- */

/*
 * Take cmacs's progress for OUR job: `meeting-notes.job-id', `.state',
 * `.result' and `.error'.  A push for another job (an earlier meeting
 * cmacs is still finishing, or a cancelled one) is ignored, and the
 * same push seen twice --- every reconfigure re-delivers the settings
 * --- changes nothing and toasts nothing.  Compositor thread.
 */
static void
meet_absorb_cmacs(GowlBarPlugin *plugin, MeetData *md)
{
	const gchar *id, *state, *result, *error;
	g_autofree gchar *seen = NULL;
	g_autofree gchar *open = NULL;
	BarMeetingPhase phase;

	id     = gowl_bar_plugin_get_setting(plugin, "job-id");
	state  = gowl_bar_plugin_get_setting(plugin, "state");
	result = gowl_bar_plugin_get_setting(plugin, "result");
	error  = gowl_bar_plugin_get_setting(plugin, "error");
	if (id == NULL || !bar_meeting_phase_from_name(state, &phase))
		return;

	seen = g_strdup_printf("%s|%s|%s", state,
	                       (result != NULL) ? result : "",
	                       (error != NULL) ? error : "");
	if (phase == BAR_MEETING_DONE && result != NULL)
		open = meet_open_command(plugin, result);

	g_mutex_lock(&md->lock);
	if (md->job_id == NULL || strcmp(md->job_id, id) != 0 ||
	    g_strcmp0(md->cmacs_seen, seen) == 0) {
		g_mutex_unlock(&md->lock);
		return;
	}
	g_free(md->cmacs_seen);
	md->cmacs_seen = g_steal_pointer(&seen);

	if (phase == BAR_MEETING_DONE) {
		g_autofree gchar *path = bar_meeting_clean_text(result, 4096);

		g_free(md->result);
		md->result = (path[0] != '\0') ? g_strdup(path) : NULL;
		meet_set_phase_locked(md, BAR_MEETING_DONE);
		meet_toast_locked(md, GOWL_BAR_TOAST_NORMAL,
		                  "Meeting notes filed",
		                  (md->result != NULL) ? md->result
		                                       : "cmacs filed the notes",
		                  open);
		g_clear_pointer(&md->title, g_free);
		md->title_rev++;
		md->want_refresh = TRUE;
	} else if (phase == BAR_MEETING_ERROR) {
		g_autofree gchar *msg = bar_meeting_clean_text(
			(error != NULL) ? error : "cmacs reported an error",
			400);

		meet_fail_locked(md, msg);
	} else if (bar_meeting_phase_is_busy(md->phase)) {
		meet_set_phase_locked(md, phase);
	}
	g_mutex_unlock(&md->lock);
}

static void
meet_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	MeetData *md = data;

	(void)settings;
	meet_absorb_cmacs(plugin, md);
	/* The local backend may itself summarise through cmacs
	   (ai-backend: cmacs); that answer arrives as ai-* settings. */
	bar_ai_absorb_settings(md->ai, plugin);
	meet_raise_toasts(plugin, md);
	meet_apply_look(plugin, md);
}

/* ----------------------------------------------------------------
 * Clicks, keys and the panel
 * ---------------------------------------------------------------- */

/* Start, or stop, depending on where the meeting is. */
static void
meet_toggle(GowlBarPlugin *plugin, MeetData *md)
{
	BarMeetingPhase phase;

	g_mutex_lock(&md->lock);
	phase = md->phase;
	if (phase == BAR_MEETING_RECORDING)
		meet_stop_locked(md);
	g_mutex_unlock(&md->lock);

	if (phase == BAR_MEETING_RECORDING) {
		meet_apply_look(plugin, md);
		gowl_bar_plugin_request_panel_refresh(plugin);
		return;
	}
	if (!bar_meeting_phase_is_busy(phase))
		meet_start(plugin, md);
}

static gboolean
meet_click(GowlBarPlugin *plugin, gpointer data, guint button, gint x,
           gint y, guint modifiers)
{
	MeetData *md = data;
	GowlBarHost *host;

	(void)x;
	(void)y;
	(void)modifiers;

	/* Left is the one thing this widget is for: record or stop, with
	   no panel in the way.  Right opens the panel, which the bar
	   itself only does for a left-click. */
	if (button == BTN_LEFT) {
		meet_toggle(plugin, md);
		return TRUE;
	}
	if (button == BTN_RIGHT) {
		host = gowl_bar_plugin_get_host(plugin);
		if (host != NULL)
			gowl_bar_host_open_panel(host,
				gowl_bar_plugin_get_id(plugin));
		return TRUE;
	}
	return FALSE;
}

static const gchar *const meet_destinations[] = {
	"directory", "secondbrain", "org-inbox", "both"
};
static const gchar *const meet_destination_labels[] = {
	"Directory", "Second brain", "Org inbox", "Brain + dir"
};

static GowlBarPanel *
meet_panel(GowlBarPlugin *plugin, gpointer data)
{
	MeetData *md = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	BarMeetingPhase phase;
	gboolean cmacs, busy;
	gchar rev[16];
	guint i, j;
	gint64 now;
	const gchar *dest;
	g_autofree gchar *outdir = NULL;
	g_autofree gchar *model = NULL;

	cmacs  = meet_backend_is_cmacs(plugin);
	outdir = meet_path_setting(plugin, "output-dir", MEET_DEFAULT_OUTPUT);
	model  = meet_path_setting(plugin, "whisper-model", NULL);
	dest   = meet_setting(plugin, "destination", NULL);
	now    = g_get_monotonic_time();

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 440);

	/* Held across the build: every string below belongs to lists the
	   worker swaps. */
	g_mutex_lock(&md->lock);
	phase = md->phase;
	busy  = bar_meeting_phase_is_busy(phase);

	{
		g_autofree gchar *title = NULL;
		g_autofree gchar *sub = NULL;
		g_autofree gchar *clock = NULL;

		if (phase == BAR_MEETING_RECORDING) {
			clock = bar_meeting_format_clock(
				(now - md->rec_started) / G_USEC_PER_SEC);
			title = g_strdup_printf("Recording %s", clock);
			sub = g_strdup_printf("%u source%s into %s",
				md->recorders->len,
				(md->recorders->len == 1) ? "" : "s",
				(md->session_dir != NULL) ? md->session_dir
				                          : "");
		} else if (busy) {
			clock = bar_meeting_format_clock(
				(now - md->phase_since) / G_USEC_PER_SEC);
			title = g_strdup_printf("%c%s",
				g_ascii_toupper(
					bar_meeting_phase_name(phase)[0]),
				bar_meeting_phase_name(phase) + 1);
			sub = g_strdup_printf("%s, %s", clock,
				cmacs ? "in cmacs" : "on this machine");
		} else if (phase == BAR_MEETING_ERROR) {
			title = g_strdup("Meeting notes");
			sub = g_strdup("The last meeting failed");
		} else {
			title = g_strdup("Meeting notes");
			sub = g_strdup((md->result != NULL)
				? "Ready; the last notes are below"
				: "Ready to record");
		}
		item = gowl_bar_panel_add_hero(panel, MEET_ICON, title, sub);
		/* The hero's id makes its switch live: on = recording. */
		gowl_bar_panel_item_set_id(item, "record");
		gowl_bar_panel_item_set_active(item,
			phase == BAR_MEETING_RECORDING);
		gowl_bar_panel_item_set_busy(item,
			busy && phase != BAR_MEETING_RECORDING);
		gowl_bar_panel_item_set_color(item,
			(phase == BAR_MEETING_RECORDING ||
			 phase == BAR_MEETING_ERROR)
				? GOWL_BAR_COLOR_RED
				: busy ? GOWL_BAR_COLOR_YELLOW
				       : GOWL_BAR_COLOR_SUBTEXT);
	}

	if (phase == BAR_MEETING_ERROR && md->error != NULL) {
		item = gowl_bar_panel_add_label(panel, md->error);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_RED);
	}

	item = gowl_bar_panel_add_entry(panel, "title",
	                                "Meeting title (optional)",
	                                (md->title != NULL) ? md->title : "");
	g_snprintf(rev, sizeof(rev), "%u", md->title_rev);
	gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_REVISION, rev);
	gowl_bar_panel_item_set_title(item, "Title");

	/* Sources. */
	gowl_bar_panel_add_section(panel, "Sources");
	if (md->pactl_missing) {
		item = gowl_bar_panel_add_label(panel,
			"pactl is not installed, so sources cannot be listed. "
			"Install pulseaudio-utils (it works with PipeWire).");
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
	} else if (md->pactl_error != NULL) {
		item = gowl_bar_panel_add_label(panel, md->pactl_error);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
	} else if (!md->sources_loaded) {
		gowl_bar_panel_add_label(panel, "Listing sources...");
	}
	for (i = 0; i < md->sources->len; i++) {
		BarMeetingSource *s = g_ptr_array_index(md->sources, i);
		g_autofree gchar *id = g_strdup_printf("src:%s", s->name);
		g_autofree gchar *label = g_strdup_printf("%s %s",
			s->monitor ? "Output:" : "Mic:", s->description);
		gboolean on = FALSE;

		for (j = 0; md->selection != NULL &&
		     md->selection[j] != NULL; j++)
			if (strcmp(md->selection[j], s->name) == 0)
				on = TRUE;
		item = gowl_bar_panel_add_toggle(panel, id, label, on);
		gowl_bar_panel_item_set_disabled(item, busy);
	}
	/* A saved choice not plugged in today still shows, so it can be
	   turned off. */
	for (j = 0; md->selection != NULL && md->selection[j] != NULL; j++) {
		gboolean listed = FALSE;

		for (i = 0; i < md->sources->len; i++)
			if (strcmp(((BarMeetingSource *)g_ptr_array_index(
				md->sources, i))->name, md->selection[j]) == 0)
				listed = TRUE;
		if (!listed) {
			g_autofree gchar *id = g_strdup_printf("src:%s",
			                                       md->selection[j]);
			g_autofree gchar *label = g_strdup_printf(
				"Not present: %s", md->selection[j]);

			item = gowl_bar_panel_add_toggle(panel, id, label,
			                                 TRUE);
			gowl_bar_panel_item_set_disabled(item, busy);
		}
	}
	item = gowl_bar_panel_add_row(panel, "sources-refresh", "\xef\x80\xa1",
	                              "Refresh sources", NULL);
	gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_SUBTEXT);
	if (md->deps_checked && !md->have_recorder) {
		item = gowl_bar_panel_add_label(panel,
			"The recorder is not installed: install pipewire-utils "
			"for pw-record (ffmpeg's pulse input is used meanwhile, "
			"if ffmpeg is installed).");
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
	}

	/* Output. */
	gowl_bar_panel_add_section(panel, "Output");
	gowl_bar_panel_add_field(panel, "Backend",
		cmacs ? "cmacs (cmacs-transcribe)" : "local (whisper-cli)");
	if (cmacs) {
		item = gowl_bar_panel_add_buttons(panel, "dest");
		for (i = 0; i < G_N_ELEMENTS(meet_destinations); i++)
			gowl_bar_panel_add_button(item,
				meet_destination_labels[i],
				g_strcmp0(dest, meet_destinations[i]) == 0);
		if (dest == NULL)
			gowl_bar_panel_add_label(panel,
				"Unset: cmacs-gowl-bar-meeting-destination "
				"decides.");
		if (md->deps_checked && !md->have_emacsctl) {
			item = gowl_bar_panel_add_label(panel,
				"emacsctl is not on PATH; cmacs cannot be "
				"reached.");
			gowl_bar_panel_item_set_color(item,
			                              GOWL_BAR_COLOR_RED);
		}
	} else {
		gowl_bar_panel_add_field(panel, "Notes",
		                         (outdir != NULL) ? outdir : "");
		gowl_bar_panel_add_field(panel, "Format",
			(g_strcmp0(meet_setting(plugin, "format", "org"), "md")
			 == 0) ? "Markdown" : "Org");
		item = gowl_bar_panel_add_buttons(panel, "dest");
		gowl_bar_panel_add_button(item, "Directory", TRUE);

		gowl_bar_panel_add_section(panel, "Whisper model");
		if (md->models->len == 0) {
			g_autofree gchar *dir = meet_path_setting(plugin,
				"whisper-models-dir", MEET_DEFAULT_MODELS);
			g_autofree gchar *text = g_strdup_printf(
				"No *.bin or *.gguf models in %s. Set "
				"meeting-notes.whisper-model to a model file, "
				"or meeting-notes.whisper-models-dir.", dir);

			item = gowl_bar_panel_add_label(panel, text);
			gowl_bar_panel_item_set_color(item,
				(model == NULL) ? GOWL_BAR_COLOR_YELLOW
				                : GOWL_BAR_COLOR_MUTED);
		}
		for (i = 0; i < md->models->len; i++) {
			const gchar *path = g_ptr_array_index(md->models, i);
			g_autofree gchar *id = g_strdup_printf("model:%s",
			                                       path);
			g_autofree gchar *base = g_path_get_basename(path);

			item = gowl_bar_panel_add_row(panel, id,
				MEET_ICON_MODEL, base, NULL);
			gowl_bar_panel_item_set_active(item,
				g_strcmp0(model, path) == 0);
		}
		if (md->deps_checked && !md->have_whisper) {
			item = gowl_bar_panel_add_label(panel,
				"whisper-cli is not on PATH. Fedora's "
				"whisper-cpp package ships only the library; "
				"install the CLI (e.g. brew install whisper-cpp) "
				"or set meeting-notes.transcribe-command.");
			gowl_bar_panel_item_set_color(item,
			                              GOWL_BAR_COLOR_YELLOW);
		}
		if (md->deps_checked && !md->have_ffmpeg) {
			item = gowl_bar_panel_add_label(panel,
				"ffmpeg is not installed: mixing two or more "
				"sources needs it (ffmpeg-free).");
			gowl_bar_panel_item_set_color(item,
			                              GOWL_BAR_COLOR_YELLOW);
		}
	}

	/* Recent. */
	if (md->recent->len > 0) {
		gowl_bar_panel_add_section(panel, "Recent");
		for (i = 0; i < md->recent->len; i++) {
			const gchar *path = g_ptr_array_index(md->recent, i);
			g_autofree gchar *id = g_strdup_printf("open:%s", path);
			g_autofree gchar *base = g_path_get_basename(path);
			g_autofree gchar *dir = g_path_get_dirname(path);

			gowl_bar_panel_add_row(panel, id, MEET_ICON_FILE, base,
			                       dir);
		}
	}

	gowl_bar_panel_add_separator(panel);
	item = gowl_bar_panel_add_buttons(panel, "control");
	if (phase == BAR_MEETING_RECORDING) {
		gowl_bar_panel_add_button(item, "Stop", TRUE);
		gowl_bar_panel_add_button(item, "Cancel", FALSE);
	} else if (busy) {
		gowl_bar_panel_add_button(item, "Cancel", FALSE);
	} else {
		gowl_bar_panel_add_button(item, "Start recording", FALSE);
		if (phase == BAR_MEETING_ERROR || phase == BAR_MEETING_DONE)
			gowl_bar_panel_add_button(item, "Dismiss", FALSE);
	}
	g_mutex_unlock(&md->lock);
	return panel;
}

/* Toggle one source in the selection and save it. */
static void
meet_toggle_source(GowlBarPlugin *plugin, MeetData *md, const gchar *name)
{
	GPtrArray *next;
	gboolean found = FALSE;
	guint i;

	if (!bar_meeting_name_is_valid(name))
		return;
	g_mutex_lock(&md->lock);
	if (bar_meeting_phase_is_busy(md->phase)) {
		g_mutex_unlock(&md->lock);
		return;
	}
	next = g_ptr_array_new();
	for (i = 0; md->selection != NULL && md->selection[i] != NULL; i++) {
		if (strcmp(md->selection[i], name) == 0)
			found = TRUE;
		else
			g_ptr_array_add(next, g_strdup(md->selection[i]));
	}
	if (!found)
		g_ptr_array_add(next, g_strdup(name));
	g_ptr_array_add(next, NULL);
	g_strfreev(md->selection);
	md->selection = (gchar **)g_ptr_array_free(next, FALSE);
	md->echo_warned = FALSE;
	g_mutex_unlock(&md->lock);

	gowl_bar_plugin_queue_work(plugin, meet_save_selection_work, md, NULL);
}

static void
meet_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
            gint index, gdouble value, guint button)
{
	MeetData *md = data;
	BarMeetingPhase phase;

	(void)value;
	(void)button;
	if (item_id == NULL)
		return;

	g_mutex_lock(&md->lock);
	phase = md->phase;
	g_mutex_unlock(&md->lock);

	if (strcmp(item_id, "record") == 0) {
		meet_toggle(plugin, md);
		return;
	}
	if (strcmp(item_id, "control") == 0) {
		if (phase == BAR_MEETING_RECORDING) {
			if (index == 0)
				meet_toggle(plugin, md);
			else
				meet_cancel(plugin, md);
		} else if (bar_meeting_phase_is_busy(phase)) {
			meet_cancel(plugin, md);
		} else if (index == 0) {
			meet_start(plugin, md);
		} else {
			g_mutex_lock(&md->lock);
			g_clear_pointer(&md->error, g_free);
			meet_set_phase_locked(md, BAR_MEETING_IDLE);
			g_mutex_unlock(&md->lock);
			meet_apply_look(plugin, md);
		}
		return;
	}
	if (g_str_has_prefix(item_id, "src:")) {
		meet_toggle_source(plugin, md, item_id + strlen("src:"));
		return;
	}
	if (strcmp(item_id, "sources-refresh") == 0) {
		g_mutex_lock(&md->lock);
		md->want_refresh = TRUE;
		g_mutex_unlock(&md->lock);
		gowl_bar_plugin_queue_work(plugin, meet_refresh_work, md, NULL);
		return;
	}
	if (strcmp(item_id, "dest") == 0) {
		/* Standalone has one destination; the row is informational. */
		if (meet_backend_is_cmacs(plugin) && index >= 0 &&
		    (guint)index < G_N_ELEMENTS(meet_destinations))
			gowl_bar_plugin_set_setting(plugin, "destination",
			                            meet_destinations[index]);
		return;
	}
	if (g_str_has_prefix(item_id, "model:")) {
		const gchar *path = item_id + strlen("model:");

		if (path[0] == '/')
			gowl_bar_plugin_set_setting(plugin, "whisper-model",
			                            path);
		return;
	}
	if (g_str_has_prefix(item_id, "open:")) {
		g_autofree gchar *cmd = meet_open_command(plugin,
			item_id + strlen("open:"));

		if (cmd != NULL)
			gowl_bar_plugin_spawn(plugin, cmd);
		return;
	}
}

static void
meet_panel_text(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
                const gchar *text, gboolean submitted)
{
	MeetData *md = data;

	(void)plugin;
	if (g_strcmp0(item_id, "title") != 0)
		return;
	g_mutex_lock(&md->lock);
	g_free(md->title);
	md->title = bar_meeting_clean_text(text, MEET_TITLE_MAX);
	g_strstrip(md->title);
	g_mutex_unlock(&md->lock);

	/* Return in the title starts the meeting: the natural end of
	   typing a name for it. */
	if (submitted)
		meet_toggle(plugin, md);
}

static gboolean
meet_panel_key(GowlBarPlugin *plugin, gpointer data, guint keysym,
               guint modifiers, gint focused_item)
{
	(void)modifiers;
	(void)focused_item;
	if (keysym == XKB_KEY_r) {
		meet_toggle(plugin, data);
		gowl_bar_plugin_request_panel_refresh(plugin);
		return TRUE;
	}
	return FALSE;
}

static void
meet_panel_opened(GowlBarPlugin *plugin, gpointer data)
{
	MeetData *md = data;

	g_mutex_lock(&md->lock);
	md->panel_open = TRUE;
	md->want_refresh = TRUE;
	g_mutex_unlock(&md->lock);
	/* The source list and the recent notes, read fresh off-thread. */
	gowl_bar_plugin_queue_work(plugin, meet_refresh_work, md, NULL);
}

static void
meet_panel_closed(GowlBarPlugin *plugin, gpointer data)
{
	MeetData *md = data;

	(void)plugin;
	g_mutex_lock(&md->lock);
	md->panel_open = FALSE;
	g_mutex_unlock(&md->lock);
}

static const GowlBarPluginVTable meeting_vtable = {
	sizeof(GowlBarPluginVTable),
	meet_create, meet_destroy,
	NULL, NULL, meet_configure,
	meet_interval, meet_poll, meet_poll_async,
	NULL, NULL,
	meet_click, NULL,
	meet_panel, meet_action,
	meet_panel_opened, meet_panel_closed,
	meet_panel_key,
	NULL,
	meet_panel_text, NULL
};

/* ----------------------------------------------------------------
 * Registration
 * ---------------------------------------------------------------- */

void
bar_register_meeting_plugins(GowlBarRegistry *registry)
{
	gowl_bar_registry_register_vtable(registry, MEET_WIDGET,
		"Meeting notes",
		"Record a meeting, transcribe and summarise it, file the notes",
		&meeting_vtable);
	gowl_bar_registry_register_alias(registry, "meeting", MEET_WIDGET);
	gowl_bar_registry_register_alias(registry, "notetaker", MEET_WIDGET);
}
