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
 * The `ai-rewrite' widget: rewrite the selected text with a language
 * model, anywhere on the desktop.
 *
 * Select text in any window, press the keybind (`gowl bar-panel
 * ai-rewrite'), pick an action, look at the word diff, press Enter:
 * the result goes onto the clipboard and is pasted over the selection,
 * and the clipboard is put back as it was a moment later.
 *
 * The flow, and which thread each step is on:
 *
 *   panel opens (compositor)  remember the focused window; queue the
 *                             capture
 *   capture (worker)          read ~/.config/gowl/ai-rewrite.yaml if it
 *                             changed; `wl-paste --primary', else the
 *                             clipboard, else nothing (the panel then
 *                             offers a text field)
 *   action (compositor)       compose the prompt, bar_ai_start(): the
 *                             model runs on a worker (`ai') or in cmacs
 *   answer (compositor)       picked up in poll and configure; diffed
 *   paste (compositor)        close the panel, queue the copy
 *   copy (worker)             save the old clipboard, `wl-copy' the
 *                             result (on STDIN, never argv)
 *   inject (compositor)       a wl_event_loop timer (or, without a
 *                             compositor, the poll) sees the copy done,
 *                             refocuses the window if focus moved and
 *                             taps the paste chord into it
 *   restore (worker)          a second later, `wl-copy' the old text
 *
 * Nothing blocks the compositor thread: every subprocess runs on a
 * worker, every wait is a timestamp checked from a callback.  The pure
 * decisions -- the action list and its formats, the prompt, the diff,
 * the paste chord, the paste/restore sequence -- live in
 * bar-airewrite-model.c, where tests reach them directly.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <wayland-server-core.h>

#include "gowl-enums.h"
#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "barkit/gowl-bar-host.h"
#include "barkit/gowl-bar-panel.h"
#include "barkit/gowl-bar-theme.h"
#include "barkit/gowl-bar-toast.h"

#include "bar-internal.h"
#include "bar-ai.h"
#include "bar-airewrite-model.h"
#include "bar-assistant-model.h"

/* The model spells modifiers itself so it needs no compositor header;
   these are what make handing them straight to the compositor right. */
G_STATIC_ASSERT(BAR_AIREWRITE_MOD_SHIFT == GOWL_KEY_MOD_SHIFT);
G_STATIC_ASSERT(BAR_AIREWRITE_MOD_CTRL  == GOWL_KEY_MOD_CTRL);
G_STATIC_ASSERT(BAR_AIREWRITE_MOD_ALT   == GOWL_KEY_MOD_ALT);
G_STATIC_ASSERT(BAR_AIREWRITE_MOD_LOGO  == GOWL_KEY_MOD_LOGO);

#define AIREWRITE_NAME            "ai-rewrite"
#define AIREWRITE_DEFAULT_WIDTH   560
#define AIREWRITE_DEFAULT_CONFIG  "~/.config/gowl/ai-rewrite.yaml"
#define AIREWRITE_DEFAULT_TERMS   \
	"gst,foot,footclient,kitty,alacritty,org.wezfurlong.wezterm," \
	"com.mitchellh.ghostty"
#define AIREWRITE_MAX_CONFIG      (256 * 1024)
#define AIREWRITE_MAX_CLIP_BYTES  (4 * 1024 * 1024)
#define AIREWRITE_PREVIEW_CHARS   400
#define AIREWRITE_PREVIEW_LINES   6
#define AIREWRITE_MAX_STEPS       32
#define AIREWRITE_PACKAGE         "wl-clipboard"
#define AIREWRITE_CHOICE_FILE     "ai-rewrite.choice"

/* ----------------------------------------------------------------
 * State
 * ---------------------------------------------------------------- */

typedef enum {
	VIEW_CAPTURING = 0,   /* reading the selection */
	VIEW_ENTER_TEXT,      /* nothing selected: type the text */
	VIEW_ACTIONS,         /* pick what to do */
	VIEW_INPUT,           /* an action that asks for a line first */
	VIEW_RUNNING,         /* the model is answering */
	VIEW_RESULT,          /* the diff, and what to do with it */
	VIEW_ERROR            /* it failed; why */
} AirewriteView;

typedef enum {
	ORIGIN_NONE = 0,
	ORIGIN_PRIMARY,       /* the selection: paste goes over it */
	ORIGIN_CLIPBOARD,     /* nothing selected; the clipboard was used */
	ORIGIN_TYPED          /* typed into the panel: clipboard only */
} AirewriteOrigin;

/*
 * One version of the text.  steps[0] is what was captured; each answer
 * is pushed on top, so chaining an action works on the top and
 * Backspace pops it.
 */
typedef struct {
	gchar     *text;
	gchar     *action_id;   /* what produced it; NULL for the source */
	gchar     *input;       /* what that action was told, if anything */
	GPtrArray *ops;         /* BarAirewriteDiffOp against the step below */
} AirewriteStep;

typedef struct {
	GowlBarPlugin *plugin;  /* owns us; not a reference */
	BarAi         *ai;
	GMutex         lock;    /* guards everything a worker writes */

	/* --- Compositor thread only ----------------------------------- */
	AirewriteView  view;
	AirewriteOrigin origin;
	GPtrArray     *steps;           /* AirewriteStep* */
	gchar         *note;            /* where the text came from, etc. */
	gchar         *status;          /* one line: "Copied", ... */
	gchar         *error;           /* VIEW_ERROR's text */
	gchar         *run_action;      /* the action running / asking */
	gchar         *run_input;
	gboolean       run_paste;       /* Shift+key: paste when done */
	gboolean       paste_due;       /* an auto-paste answer is in */
	gboolean       hints;           /* `?' */
	guint          entry_rev;
	GWeakRef       target;          /* the window to paste into */
	GPtrArray     *actions;         /* the effective list */
	gchar         *actions_key;     /* what it was built from */

	/* --- Written by workers, under the lock ------------------------ */
	guint          capture_gen;
	gboolean       capture_done;
	gchar         *captured;
	AirewriteOrigin captured_origin;
	gchar         *captured_note;
	gboolean       have_clipboard_tools;
	gboolean       backend_none;
	gchar         *backend_why;

	GPtrArray     *yaml_actions;    /* BarAirewriteAction*, or NULL */
	gboolean       yaml_replace;
	gchar         *yaml_error;
	gint64         yaml_mtime;
	guint          yaml_serial;

	BarAirewritePaste paste;
	guint          paste_gen;
	gchar         *paste_previous;  /* the clipboard before; secret-ish */

	/* --- The provider and model picked in the panel ----------------
	 * Compositor thread, except where marked.  Saved to the host's
	 * state directory, so the pick survives a restart; NULL means "the
	 * widget's ai-provider / ai-model settings" (under cmacs, the
	 * cmacs-ai default). */
	gboolean       settings_open;    /* the choice is on show */
	gboolean       settings_on_open; /* right-click: open on it */
	gboolean       choice_loaded;
	gchar         *choice_provider;
	gchar         *choice_model;
	guint          model_rev;        /* the model field's revision */
	gchar        **cli_providers;    /* lock: from `ai --list-providers' */
	gboolean       cli_listed;       /* lock: asked once this session */

	/* --- The paste chord, fixed when the paste starts -------------- */
	gchar         *paste_keys;
	gchar         *paste_app_id;
	gchar         *paste_copy_cmd;
	struct wl_event_source *timer;
} AiRewrite;

static void airewrite_paste_tick (AiRewrite *self);
static void update_label         (AiRewrite *self, GowlBarPlugin *plugin);
static void choice_ensure        (AiRewrite *self, GowlBarPlugin *plugin);

static void
step_free(gpointer data)
{
	AirewriteStep *step = data;

	if (step == NULL)
		return;
	g_free(step->text);
	g_free(step->action_id);
	g_free(step->input);
	if (step->ops != NULL)
		g_ptr_array_unref(step->ops);
	g_free(step);
}

static AirewriteStep *
step_top(AiRewrite *self)
{
	if (self->steps->len == 0)
		return NULL;
	return g_ptr_array_index(self->steps, self->steps->len - 1);
}

/* A setting, or @fallback when it is unset or empty. */
static const gchar *
setting(GowlBarPlugin *plugin, const gchar *key, const gchar *fallback)
{
	const gchar *v = gowl_bar_plugin_get_setting(plugin, key);

	return (v != NULL && v[0] != '\0') ? v : fallback;
}

/* Whether @text is empty or only white space. */
static gboolean
blank(const gchar *text)
{
	const gchar *p;

	for (p = text; *p != '\0'; p = g_utf8_next_char(p))
		if (!g_unichar_isspace(g_utf8_get_char(p)))
			return FALSE;
	return TRUE;
}

static void
set_str(gchar **slot, const gchar *value)
{
	g_free(*slot);
	*slot = g_strdup(value);
}

/* ----------------------------------------------------------------
 * Colour roles for the panel
 *
 * Panel items take a ROLE, so a `color-key' that is a role name is
 * honoured there and a hex literal falls back to the default (the
 * canvas, which paints itself, honours both).  The role name is looked
 * up by walking the roles, which keeps colour interpretation out of
 * this file in the way guard 9 asks.
 * ---------------------------------------------------------------- */

static GowlBarColor
role_setting(GowlBarPlugin *plugin, const gchar *key, GowlBarColor fallback)
{
	const gchar *spec = gowl_bar_plugin_get_setting(plugin, key);
	gint r;

	if (spec == NULL || spec[0] == '\0')
		return fallback;
	for (r = 0; r < (gint)GOWL_BAR_COLOR_COUNT; r++)
		if (g_ascii_strcasecmp(spec,
		        gowl_bar_theme_color_name((GowlBarColor)r)) == 0)
			return (GowlBarColor)r;
	return fallback;
}

/*
 * The colour a canvas paints with: a `#rrggbb' literal as written, a
 * role name through the theme, else @fallback through the theme.  The
 * role's components are read back off the cairo source so the
 * palette's own alpha comes along.
 */
static void
canvas_color(GowlBarPlugin *plugin, const GowlBarTheme *theme, cairo_t *cr,
             const gchar *key, GowlBarColor fallback, gdouble rgba[4])
{
	const gchar *spec = gowl_bar_plugin_get_setting(plugin, key);

	rgba[0] = rgba[1] = rgba[2] = 0.5;
	rgba[3] = 1.0;
	if (spec != NULL && spec[0] == '#' && gowl_bar_color_parse(spec, rgba))
		return;

	cairo_save(cr);
	gowl_bar_theme_cairo_set(theme, cr,
	                         role_setting(plugin, key, fallback));
	cairo_pattern_get_rgba(cairo_get_source(cr),
	                       &rgba[0], &rgba[1], &rgba[2], &rgba[3]);
	cairo_restore(cr);
}

/* ----------------------------------------------------------------
 * Running a command on a worker
 * ---------------------------------------------------------------- */

static void
set_nonblock(gint fd)
{
	gint flags = fcntl(fd, F_GETFL);

	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * Run @argv to completion on a worker thread, with an optional @input
 * on its stdin and, when @out is non-NULL, its stdout collected (up to
 * %AIREWRITE_MAX_CLIP_BYTES).  Killed -- this PID only -- at
 * @timeout_ms: a selection owner that never writes would otherwise hold
 * wl-paste, and this worker, forever.
 *
 * stdout goes to /dev/null when it is not wanted, and that matters for
 * wl-copy: it forks a child that serves the clipboard and keeps
 * whatever stdout it was given, so a pipe here would never see EOF.
 * The PARENT exits once the selection is set, and that is the exit
 * waited for.
 *
 * Reaped here, synchronously, never through a GLib child watch: under
 * cmacs that would fire on Emacs's thread.
 *
 * Returns: %TRUE when the command ran and exited 0
 */
static gboolean
run_command(gchar **argv, const gchar *input, gint timeout_ms, gchar **out,
            gboolean *too_big)
{
	GPid pid = 0;
	gint in_fd = -1, out_fd = -1;
	GSpawnFlags flags;
	GString *buf = NULL;
	gsize written = 0, in_len;
	gint64 deadline;
	gint status = 0;
	gboolean killed = FALSE;
	pid_t done;

	if (out != NULL)
		*out = NULL;
	if (too_big != NULL)
		*too_big = FALSE;
	if (argv == NULL || argv[0] == NULL)
		return FALSE;

	flags = G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
	        G_SPAWN_STDERR_TO_DEV_NULL;
	if (out == NULL)
		flags |= G_SPAWN_STDOUT_TO_DEV_NULL;
	if (input == NULL)
		flags |= G_SPAWN_STDIN_FROM_DEV_NULL;

	if (!g_spawn_async_with_pipes(NULL, argv, NULL, flags, NULL, NULL,
	                              &pid,
	                              (input != NULL) ? &in_fd : NULL,
	                              (out != NULL) ? &out_fd : NULL,
	                              NULL, NULL))
		return FALSE;

	in_len   = (input != NULL) ? strlen(input) : 0;
	deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
	if (out != NULL)
		buf = g_string_new(NULL);
	if (in_fd >= 0) {
		set_nonblock(in_fd);
		if (in_len == 0) {
			close(in_fd);
			in_fd = -1;
		}
	}
	if (out_fd >= 0)
		set_nonblock(out_fd);

	/* Feed stdin and drain stdout together, so a child that fills
	   one pipe while we wait on the other cannot deadlock us. */
	while (in_fd >= 0 || out_fd >= 0) {
		struct pollfd fds[2];
		nfds_t n = 0;
		gint64 left = (deadline - g_get_monotonic_time()) / 1000;
		gint rc;
		nfds_t k;

		if (left <= 0) {
			killed = TRUE;
			break;
		}
		if (in_fd >= 0) {
			fds[n].fd = in_fd;
			fds[n].events = POLLOUT;
			fds[n].revents = 0;
			n++;
		}
		if (out_fd >= 0) {
			fds[n].fd = out_fd;
			fds[n].events = POLLIN;
			fds[n].revents = 0;
			n++;
		}
		rc = poll(fds, n, (gint)MIN(left, 200));
		if (rc < 0 && errno != EINTR)
			break;
		for (k = 0; k < n && rc > 0; k++) {
			if (fds[k].revents == 0)
				continue;
			if (fds[k].fd == in_fd) {
				gssize w = write(in_fd, input + written,
				                 in_len - written);

				if (w > 0)
					written += (gsize)w;
				if ((w < 0 && errno != EAGAIN &&
				     errno != EINTR) || written >= in_len) {
					close(in_fd);
					in_fd = -1;
				}
			} else {
				gchar chunk[8192];
				gssize r = read(out_fd, chunk, sizeof(chunk));

				if (r > 0) {
					if (buf->len + (gsize)r >
					    AIREWRITE_MAX_CLIP_BYTES) {
						if (too_big != NULL)
							*too_big = TRUE;
						killed = TRUE;
						break;
					}
					g_string_append_len(buf, chunk, r);
				} else if (r == 0 || (errno != EAGAIN &&
				                      errno != EINTR)) {
					close(out_fd);
					out_fd = -1;
				}
			}
		}
		if (killed)
			break;
	}
	if (in_fd >= 0)
		close(in_fd);
	if (out_fd >= 0)
		close(out_fd);

	/* Wait for the exit within what is left of the deadline. */
	while (!killed) {
		done = waitpid((pid_t)pid, &status, WNOHANG);
		if (done == (pid_t)pid)
			break;
		if (done < 0 && errno != EINTR) {
			status = 1;
			break;
		}
		if (g_get_monotonic_time() >= deadline) {
			killed = TRUE;
			break;
		}
		g_usleep(10 * 1000);
	}
	if (killed) {
		kill((pid_t)pid, SIGTERM);
		while (waitpid((pid_t)pid, &status, 0) < 0 && errno == EINTR)
			;
	}
	g_spawn_close_pid(pid);

	if (buf != NULL) {
		if (!killed && out != NULL)
			*out = g_string_free(buf, FALSE);
		else
			g_string_free(buf, TRUE);
	}
	return !killed && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/*
 * A command setting split into words, with the widget's own arguments
 * after.  A user who points `paste-command' at a wrapper still gets
 * `--primary' and friends appended, which is what the wrapper is for.
 */
static gchar **
command_argv(const gchar *cmdline, const gchar *const *extra)
{
	g_auto(GStrv) words = NULL;
	GPtrArray *argv;
	guint i;

	if (!g_shell_parse_argv(cmdline, NULL, &words, NULL) ||
	    words == NULL || words[0] == NULL)
		return NULL;

	argv = g_ptr_array_new();
	for (i = 0; words[i] != NULL; i++)
		g_ptr_array_add(argv, (i == 0) ? bar_expand_tilde(words[i])
		                               : g_strdup(words[i]));
	for (i = 0; extra != NULL && extra[i] != NULL; i++)
		g_ptr_array_add(argv, g_strdup(extra[i]));
	g_ptr_array_add(argv, NULL);
	return (gchar **)g_ptr_array_free(argv, FALSE);
}

static gboolean
command_installed(const gchar *cmdline)
{
	g_auto(GStrv) words = NULL;
	g_autofree gchar *exe = NULL;
	g_autofree gchar *found = NULL;

	if (!g_shell_parse_argv(cmdline, NULL, &words, NULL) ||
	    words == NULL || words[0] == NULL)
		return FALSE;
	exe = bar_expand_tilde(words[0]);
	if (strchr(exe, '/') != NULL)
		return g_file_test(exe, G_FILE_TEST_IS_EXECUTABLE);
	found = g_find_program_in_path(exe);
	return found != NULL;
}

/* ----------------------------------------------------------------
 * Capture: the action file and the selected text, on a worker
 * ---------------------------------------------------------------- */

typedef struct {
	AiRewrite *self;
	guint      gen;
	gchar     *paste_cmd;
	gchar     *config;
	gint       timeout_ms;
	gint       max_chars;
} CaptureJob;

static void
capture_job_free(gpointer data)
{
	CaptureJob *job = data;

	g_free(job->paste_cmd);
	g_free(job->config);
	g_free(job);
}

/*
 * Re-read the action file when its mtime moved.  Bounded, because it
 * is a file read; on the worker, because it is a file read at all.
 */
static void
capture_load_config(AiRewrite *self, const gchar *config)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;
	GPtrArray *parsed = NULL;
	gboolean replace = FALSE;
	gsize len = 0;
	struct stat st;
	gint64 mtime;

	path = bar_expand_tilde(config);
	mtime = (stat(path, &st) == 0)
	        ? (gint64)st.st_mtime * G_USEC_PER_SEC + 1 : 0;

	g_mutex_lock(&self->lock);
	if (mtime == self->yaml_mtime) {
		g_mutex_unlock(&self->lock);
		return;
	}
	g_mutex_unlock(&self->lock);

	if (mtime != 0) {
		if (st.st_size > AIREWRITE_MAX_CONFIG) {
			g_set_error(&error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
			            "%s is larger than %d KiB", path,
			            AIREWRITE_MAX_CONFIG / 1024);
		} else if (g_file_get_contents(path, &text, &len, &error)) {
			parsed = bar_airewrite_actions_from_yaml(text, &replace,
			                                         &error);
		}
	}

	g_mutex_lock(&self->lock);
	if (self->yaml_actions != NULL)
		g_ptr_array_unref(self->yaml_actions);
	self->yaml_actions = parsed;
	self->yaml_replace = (parsed != NULL) && replace;
	g_free(self->yaml_error);
	self->yaml_error = (error != NULL)
	        ? g_strdup_printf("%s: %s", config, error->message) : NULL;
	self->yaml_mtime = mtime;
	self->yaml_serial++;
	g_mutex_unlock(&self->lock);
}

/* One read through `paste-command'.  NULL for nothing, empty, not
   text, or too large; @why says which when it is worth saying. */
static gchar *
capture_read(const gchar *paste_cmd, gboolean primary, gint timeout_ms,
             gint max_chars, gchar **why)
{
	static const gchar *const primary_args[] = {
		"--primary", "--no-newline", "--type", "text", NULL
	};
	static const gchar *const clip_args[] = {
		"--no-newline", "--type", "text", NULL
	};
	g_auto(GStrv) argv = NULL;
	gchar *out = NULL;
	gboolean too_big = FALSE;

	argv = command_argv(paste_cmd, primary ? primary_args : clip_args);
	if (argv == NULL)
		return NULL;
	if (!run_command(argv, NULL, timeout_ms, &out, &too_big)) {
		if (too_big && why != NULL && *why == NULL)
			*why = g_strdup("the selection is too large to rewrite");
		g_free(out);
		return NULL;
	}
	if (out == NULL || out[0] == '\0' ||
	    !g_utf8_validate(out, -1, NULL)) {
		g_free(out);
		return NULL;
	}
	if (g_utf8_strlen(out, -1) > (glong)max_chars) {
		if (why != NULL && *why == NULL)
			*why = g_strdup_printf(
				"the %s holds %ld characters; the limit is %d"
				" (ai-rewrite.max-chars)",
				primary ? "selection" : "clipboard",
				g_utf8_strlen(out, -1), max_chars);
		g_free(out);
		return NULL;
	}
	/* Nothing but white space is nothing to rewrite. */
	if (blank(out)) {
		g_free(out);
		return NULL;
	}
	return out;
}

static void
capture_work(GowlBarPlugin *plugin, gpointer user_data)
{
	CaptureJob *job = user_data;
	AiRewrite *self = job->self;
	g_autofree gchar *text = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *backend_why = NULL;
	AirewriteOrigin origin = ORIGIN_NONE;
	gboolean tools;
	gboolean backend_none;

	capture_load_config(self, job->config);

	/* Which backend: bar_ai_backend_for() looks for `ai' on PATH,
	   which is a filesystem walk, so it is asked here and not in the
	   panel builder. */
	backend_none = (bar_ai_backend_for(plugin) == BAR_AI_BACKEND_NONE);
	if (backend_none)
		backend_why = g_strdup(
			"No AI backend: install ai-glib's `ai' command, or set"
			" ai-rewrite.ai-backend to `cmacs' under cmacs --gowl.");

	tools = command_installed(job->paste_cmd);
	if (tools) {
		text = capture_read(job->paste_cmd, TRUE, job->timeout_ms,
		                    job->max_chars, &why);
		if (text != NULL) {
			origin = ORIGIN_PRIMARY;
		} else {
			text = capture_read(job->paste_cmd, FALSE,
			                    job->timeout_ms, job->max_chars,
			                    &why);
			if (text != NULL) {
				origin = ORIGIN_CLIPBOARD;
				if (why == NULL)
					why = g_strdup(
						"Nothing was selected, so this"
						" is the clipboard.");
			}
		}
	} else {
		why = g_strdup_printf("%s is not installed (Fedora package:"
		                      " %s), so the selection cannot be"
		                      " read.  Type or paste the text instead.",
		                      job->paste_cmd, AIREWRITE_PACKAGE);
	}

	g_mutex_lock(&self->lock);
	if (job->gen == self->capture_gen) {
		set_str(&self->captured, text);
		set_str(&self->captured_note, why);
		self->captured_origin = origin;
		self->capture_done = TRUE;
		self->have_clipboard_tools = tools;
		self->backend_none = backend_none;
		set_str(&self->backend_why, backend_why);
	}
	g_mutex_unlock(&self->lock);

	gowl_bar_plugin_request_panel_refresh(plugin);
}

/* ----------------------------------------------------------------
 * The action list
 * ---------------------------------------------------------------- */

/*
 * Rebuild the effective list when anything it is built from moved:
 * the built-ins, the YAML file's actions, and the `actions' setting
 * cmacs pushes.  The YAML list is copied out from under the lock, so
 * the worker can replace it at any time.
 */
static void
actions_refresh(AiRewrite *self, GowlBarPlugin *plugin)
{
	const gchar *pushed = gowl_bar_plugin_get_setting(plugin, "actions");
	const gchar *mode = setting(plugin, "actions-mode", "extend");
	g_autoptr(GPtrArray) builtin = NULL;
	g_autoptr(GPtrArray) yaml = NULL;
	g_autoptr(GPtrArray) step1 = NULL;
	g_autoptr(GPtrArray) extra = NULL;
	g_autofree gchar *key = NULL;
	gboolean yaml_replace;
	guint serial;
	guint i;

	g_mutex_lock(&self->lock);
	serial = self->yaml_serial;
	yaml_replace = self->yaml_replace;
	if (self->yaml_actions != NULL) {
		yaml = g_ptr_array_new_with_free_func(
			(GDestroyNotify)bar_airewrite_action_free);
		for (i = 0; i < self->yaml_actions->len; i++)
			g_ptr_array_add(yaml, bar_airewrite_action_copy(
				g_ptr_array_index(self->yaml_actions, i)));
	}
	g_mutex_unlock(&self->lock);

	key = g_strdup_printf("%u|%s|%s", serial, mode,
	                      (pushed != NULL) ? pushed : "");
	if (self->actions != NULL && g_strcmp0(key, self->actions_key) == 0)
		return;

	builtin = bar_airewrite_builtin_actions();
	step1 = bar_airewrite_actions_merge(builtin, yaml, yaml_replace);
	extra = bar_airewrite_actions_parse(pushed, NULL);

	if (self->actions != NULL)
		g_ptr_array_unref(self->actions);
	/* A pushed list only replaces when it has something in it: an
	   empty push must not leave the user with no actions at all. */
	self->actions = bar_airewrite_actions_merge(step1, extra,
		g_ascii_strcasecmp(mode, "replace") == 0 && extra->len > 0);
	g_free(self->actions_key);
	self->actions_key = g_steal_pointer(&key);
}

/* ----------------------------------------------------------------
 * The flow
 * ---------------------------------------------------------------- */

static void
set_view_error(AiRewrite *self, const gchar *message)
{
	set_str(&self->error, message);
	self->view = VIEW_ERROR;
}

/* Move whatever the capture worker found into the session. */
static void
capture_absorb(AiRewrite *self)
{
	AirewriteStep *step;

	if (self->view != VIEW_CAPTURING)
		return;

	g_mutex_lock(&self->lock);
	if (!self->capture_done) {
		g_mutex_unlock(&self->lock);
		return;
	}
	set_str(&self->note, self->captured_note);
	self->origin = self->captured_origin;
	if (self->captured != NULL) {
		step = g_new0(AirewriteStep, 1);
		step->text = g_strdup(self->captured);
		g_ptr_array_add(self->steps, step);
	}
	g_mutex_unlock(&self->lock);

	if (self->steps->len > 0) {
		self->view = VIEW_ACTIONS;
	} else {
		self->view = VIEW_ENTER_TEXT;
		self->entry_rev++;
	}
}

/*
 * Start @action_id on the top step.  An action that asks for a line
 * goes to the input view first, unless the line is already here (Again
 * and the input view's own submit).
 */
static void
run_action(AiRewrite *self, GowlBarPlugin *plugin, const gchar *action_id,
           const gchar *input, gboolean paste_after)
{
	const BarAirewriteAction *a;
	AirewriteStep *top;
	g_autofree gchar *prompt = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *provider = NULL;
	const gchar *model = NULL;

	actions_refresh(self, plugin);
	a = bar_airewrite_actions_find_id(self->actions, action_id);
	top = step_top(self);
	if (a == NULL || top == NULL)
		return;

	set_str(&self->run_action, a->id);
	self->run_paste = paste_after;
	g_clear_pointer(&self->status, g_free);

	if (a->input != NULL && (input == NULL || input[0] == '\0')) {
		g_clear_pointer(&self->run_input, g_free);
		self->view = VIEW_INPUT;
		self->entry_rev++;
		return;
	}
	set_str(&self->run_input, input);

	prompt = bar_airewrite_compose_prompt(a, input, top->text);
	choice_ensure(self, plugin);
	bar_airewrite_effective_model(a->provider, a->model,
	                              self->choice_provider,
	                              self->choice_model, &provider, &model);
	if (!bar_ai_start_full(self->ai, plugin, a->system, prompt,
	                       provider, model, &error)) {
		set_view_error(self, (error != NULL) ? error->message
		                                     : "the model could not be asked");
		return;
	}
	self->view = VIEW_RUNNING;
}

/*
 * Bring the AI slot's state into the view.  Called from poll,
 * configure and the panel builder; an answer that is to be pasted
 * straight away only RAISES paste_due here, because the builder must
 * not close the panel it is building.
 */
static void
ai_absorb(AiRewrite *self)
{
	g_autofree gchar *text = NULL;
	g_autofree gchar *err = NULL;
	const BarAirewriteAction *a;
	AirewriteStep *base, *step;
	BarAiState st;

	if (self->view != VIEW_RUNNING)
		return;

	st = bar_ai_get(self->ai, &text, &err);
	if (st == BAR_AI_ERROR) {
		set_view_error(self, (err != NULL) ? err : "the model failed");
		return;
	}
	if (st != BAR_AI_DONE)
		return;

	base = step_top(self);
	if (base == NULL)
		return;
	if (text == NULL || blank(text)) {
		set_view_error(self, "the model returned nothing");
		return;
	}

	a = bar_airewrite_actions_find_id(self->actions, self->run_action);
	step = g_new0(AirewriteStep, 1);
	step->text = bar_airewrite_clean_answer(text, base->text);
	step->action_id = g_strdup(self->run_action);
	step->input = g_strdup(self->run_input);
	if (a == NULL || a->diff) {
		step->ops = bar_airewrite_diff(base->text, step->text);
	} else {
		BarAirewriteDiffOp *op = g_new0(BarAirewriteDiffOp, 1);

		step->ops = g_ptr_array_new_with_free_func(
			(GDestroyNotify)bar_airewrite_diff_op_free);
		op->kind = BAR_AIREWRITE_DIFF_EQUAL;
		op->text = g_strdup(step->text);
		g_ptr_array_add(step->ops, op);
	}

	/* The undo stack is small on purpose: each step holds a copy of
	   the text, and nobody walks back thirty rewrites. */
	if (self->steps->len >= AIREWRITE_MAX_STEPS)
		g_ptr_array_remove_index(self->steps, 1);
	g_ptr_array_add(self->steps, step);

	self->view = VIEW_RESULT;
	if (self->run_paste || (a != NULL && !a->preview))
		self->paste_due = TRUE;
}

/* ----------------------------------------------------------------
 * Paste-back
 * ---------------------------------------------------------------- */

typedef struct {
	AiRewrite *self;
	guint      gen;
	gchar    **copy_argv;
	gchar    **read_argv;   /* NULL: do not save the old clipboard */
	gchar     *text;
	gint       timeout_ms;
	gboolean   restoring;   /* this is the put-back, not the copy */
} PasteJob;

static void
paste_job_free(gpointer data)
{
	PasteJob *job = data;

	g_strfreev(job->copy_argv);
	g_strfreev(job->read_argv);
	if (job->text != NULL) {
		/* The old clipboard can be a password; do not leave it in
		   freed memory longer than it has to be. */
		memset(job->text, 0, strlen(job->text));
		g_free(job->text);
	}
	g_free(job);
}

static void
paste_work(GowlBarPlugin *plugin, gpointer user_data)
{
	PasteJob *job = user_data;
	AiRewrite *self = job->self;
	gchar *previous = NULL;
	gboolean ok;

	/* Save the clipboard BEFORE replacing it; afterwards there is
	   nothing left to save. */
	if (!job->restoring && job->read_argv != NULL) {
		if (!run_command(job->read_argv, NULL, job->timeout_ms,
		                 &previous, NULL) ||
		    (previous != NULL &&
		     !g_utf8_validate(previous, -1, NULL)))
			g_clear_pointer(&previous, g_free);
	}

	/* The text goes on stdin.  In argv it would be visible to every
	   user on the machine through `ps', and capped by ARG_MAX. */
	ok = run_command(job->copy_argv, job->text, job->timeout_ms, NULL,
	                 NULL);

	g_mutex_lock(&self->lock);
	if (job->gen == self->paste_gen) {
		if (job->restoring) {
			self->paste.phase = BAR_AIREWRITE_PASTE_IDLE;
		} else {
			self->paste.copied = ok;
			self->paste.have_previous = (previous != NULL);
			self->paste.phase = BAR_AIREWRITE_PASTE_COPIED;
			g_free(self->paste_previous);
			self->paste_previous = previous;
			previous = NULL;
		}
	}
	g_mutex_unlock(&self->lock);
	g_free(previous);

	gowl_bar_plugin_request_redraw(plugin);
}

/*
 * Tap the paste chord into the window the panel was opened over.
 *
 * Isolated because it is the one step a headless test cannot take: it
 * needs a compositor, a keymap and a client.  Everything it DECIDES --
 * which chord, whether to try at all -- is decided before it is called,
 * in tested code.
 *
 * gowl_compositor_send_key_to_client() reaches Wayland and X11 windows
 * alike (an X11 one through X focus, confirmed with the X server) and
 * refuses while the session is locked or a launcher or popup holds the
 * keyboard -- the same grabs the focus gate honours.  If focus moved
 * while the panel was up, the window is shown and focused first, so
 * the user sees the paste land.
 */
static gboolean
paste_inject(AiRewrite *self, gchar **why)
{
	const BarEnv *env = bar_env();
	GowlCompositor *comp;
	g_autoptr(GowlClient) target = NULL;
	g_autofree gchar *keyname = NULL;
	xkb_keysym_t sym;
	guint32 keycode;
	gboolean shift = FALSE;
	guint mods = 0;

	if (env == NULL || env->compositor == NULL) {
		*why = g_strdup("there is no compositor to paste through");
		return FALSE;
	}
	comp = GOWL_COMPOSITOR(env->compositor);

	target = g_weak_ref_get(&self->target);
	if (target == NULL ||
	    g_list_find(gowl_compositor_get_clients(comp), target) == NULL) {
		*why = g_strdup("the window it came from has closed");
		return FALSE;
	}
	if (gowl_compositor_keyboard_is_grabbed(comp)) {
		*why = g_strdup("something else holds the keyboard");
		return FALSE;
	}
	if (!bar_airewrite_parse_keys(self->paste_keys, &mods, &keyname)) {
		*why = g_strdup_printf("cannot read paste-keys `%s'",
		                       self->paste_keys);
		return FALSE;
	}
	sym = xkb_keysym_from_name(keyname, XKB_KEYSYM_NO_FLAGS);
	if (sym == XKB_KEY_NoSymbol)
		sym = xkb_keysym_from_name(keyname,
		                           XKB_KEYSYM_CASE_INSENSITIVE);
	keycode = gowl_compositor_keysym_to_keycode(comp, (guint32)sym,
	                                            &shift);
	if (keycode == 0) {
		*why = g_strdup_printf("no key in this layout types `%s'",
		                       keyname);
		return FALSE;
	}
	if (shift)
		mods |= BAR_AIREWRITE_MOD_SHIFT;

	if (gowl_compositor_get_focused_client(comp) != target)
		gowl_compositor_show_client(comp, target);

	if (!gowl_compositor_send_key_to_client(comp, target, keycode, mods)) {
		*why = g_strdup("the compositor refused the keys");
		return FALSE;
	}
	return TRUE;
}

/*
 * The compositor's timer: the only clock fine enough for "paste as
 * soon as the copy lands" (the poll ticks in whole seconds), and it
 * fires on the compositor thread, where the injection must happen.
 * GLib timeouts would fire on Emacs's thread under cmacs (guard 7).
 */
static gint
paste_timer_fire(gpointer data)
{
	airewrite_paste_tick((AiRewrite *)data);
	return 0;
}

static void
paste_rearm(AiRewrite *self)
{
	const BarEnv *env = bar_env();
	gint wake;

	if (env == NULL || env->compositor == NULL)
		return;

	g_mutex_lock(&self->lock);
	wake = bar_airewrite_paste_wake_ms(&self->paste,
	                                   g_get_monotonic_time());
	g_mutex_unlock(&self->lock);
	if (wake < 0)
		return;

	if (self->timer == NULL) {
		struct wl_event_loop *loop = gowl_compositor_get_event_loop(
			GOWL_COMPOSITOR(env->compositor));

		if (loop == NULL)
			return;
		self->timer = wl_event_loop_add_timer(loop, paste_timer_fire,
		                                      self);
		if (self->timer == NULL)
			return;
	}
	wl_event_source_timer_update(self->timer, MAX(wake, 1));
}

/* Advance the paste machine one step; compositor thread. */
static void
airewrite_paste_tick(AiRewrite *self)
{
	GowlBarPlugin *plugin = self->plugin;
	BarAirewritePasteStep step;
	g_autofree gchar *why = NULL;

	g_mutex_lock(&self->lock);
	step = bar_airewrite_paste_step(&self->paste, g_get_monotonic_time());
	g_mutex_unlock(&self->lock);

	switch (step) {
	case BAR_AIREWRITE_PASTE_INJECT:
		if (!paste_inject(self, &why)) {
			/* The result is on the clipboard either way; say so,
			   and leave it there rather than restoring over it. */
			g_mutex_lock(&self->lock);
			self->paste.restore = FALSE;
			g_mutex_unlock(&self->lock);
			gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
				"The rewrite is on the clipboard",
				why);
		}
		break;

	case BAR_AIREWRITE_PASTE_RESTORE: {
		PasteJob *job;
		gchar *previous;

		g_mutex_lock(&self->lock);
		previous = self->paste_previous;
		self->paste_previous = NULL;
		g_mutex_unlock(&self->lock);

		job = g_new0(PasteJob, 1);
		job->self       = self;
		job->restoring  = TRUE;
		job->text       = previous;
		job->timeout_ms = 3000;
		job->copy_argv  = command_argv(self->paste_copy_cmd, NULL);
		g_mutex_lock(&self->lock);
		job->gen = self->paste_gen;
		g_mutex_unlock(&self->lock);
		if (job->copy_argv == NULL || job->text == NULL) {
			g_mutex_lock(&self->lock);
			self->paste.phase = BAR_AIREWRITE_PASTE_IDLE;
			g_mutex_unlock(&self->lock);
			paste_job_free(job);
			break;
		}
		gowl_bar_plugin_queue_work(plugin, paste_work, job,
		                           paste_job_free);
		break;
	}

	case BAR_AIREWRITE_PASTE_FINISH:
		g_mutex_lock(&self->lock);
		if (self->paste_previous != NULL) {
			memset(self->paste_previous, 0,
			       strlen(self->paste_previous));
			g_clear_pointer(&self->paste_previous, g_free);
		}
		if (!self->paste.copied)
			why = g_strdup_printf("could not run %s (Fedora"
			                      " package: %s)",
			                      self->paste_copy_cmd,
			                      AIREWRITE_PACKAGE);
		g_mutex_unlock(&self->lock);
		if (why != NULL)
			gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
				"Could not copy the rewrite", why);
		break;

	case BAR_AIREWRITE_PASTE_WAIT:
	default:
		break;
	}

	paste_rearm(self);
}

/*
 * Put @text on the clipboard and, when @inject, paste it over the
 * selection it was made from.
 */
static void
paste_start(AiRewrite *self, GowlBarPlugin *plugin, const gchar *text,
            gboolean inject)
{
	static const gchar *const read_args[] = {
		"--no-newline", "--type", "text", NULL
	};
	g_autoptr(GowlClient) target = NULL;
	GowlBarHost *host;
	PasteJob *job;
	const gchar *copy_cmd = setting(plugin, "copy-command", "wl-copy");
	const gchar *paste_cmd = setting(plugin, "paste-command", "wl-paste");
	gboolean restore;

	g_mutex_lock(&self->lock);
	if (self->paste.phase != BAR_AIREWRITE_PASTE_IDLE) {
		g_mutex_unlock(&self->lock);
		set_str(&self->status, "Still pasting the last one.");
		return;
	}
	g_mutex_unlock(&self->lock);

	/* Typed text has nothing to paste over, and a window that has
	   gone has nothing to paste into: both are copy only. */
	target = g_weak_ref_get(&self->target);
	if (self->origin == ORIGIN_TYPED || target == NULL)
		inject = FALSE;
	restore = inject && gowl_bar_plugin_get_setting_bool(plugin,
	                                "restore-clipboard", TRUE);

	g_free(self->paste_app_id);
	self->paste_app_id = g_strdup((target != NULL)
	        ? gowl_client_get_app_id(target) : NULL);
	set_str(&self->paste_keys, bar_airewrite_paste_keys_for(
		self->paste_app_id,
		setting(plugin, "paste-keys", "ctrl+v"),
		setting(plugin, "paste-keys-terminal", "ctrl+shift+v"),
		setting(plugin, "terminal-app-ids", AIREWRITE_DEFAULT_TERMS)));
	set_str(&self->paste_copy_cmd, copy_cmd);

	job = g_new0(PasteJob, 1);
	job->self       = self;
	job->text       = g_strdup(text);
	job->timeout_ms = 3000;
	job->copy_argv  = command_argv(copy_cmd, NULL);
	job->read_argv  = restore ? command_argv(paste_cmd, read_args) : NULL;

	g_mutex_lock(&self->lock);
	self->paste_gen++;
	job->gen = self->paste_gen;
	memset(&self->paste, 0, sizeof(self->paste));
	self->paste.phase   = BAR_AIREWRITE_PASTE_COPYING;
	self->paste.inject  = inject;
	self->paste.restore = restore;
	self->paste.restore_delay = (gint64)CLAMP(
		gowl_bar_plugin_get_setting_int(plugin, "restore-delay-ms",
		                                1000), 100, 60000) * 1000;
	g_mutex_unlock(&self->lock);

	if (inject) {
		/* Out of the way first: the window has to be the one that
		   takes the keys, and the user wants to watch it land. */
		host = gowl_bar_plugin_get_host(plugin);
		if (host != NULL)
			gowl_bar_host_close_panel(host);
	} else {
		set_str(&self->status, (self->origin == ORIGIN_TYPED)
			? "Copied to the clipboard (typed text has nothing to"
			  " paste over)."
			: "Copied to the clipboard.");
	}

	if (job->copy_argv == NULL) {
		g_mutex_lock(&self->lock);
		self->paste.phase = BAR_AIREWRITE_PASTE_COPIED;
		self->paste.copied = FALSE;
		g_mutex_unlock(&self->lock);
		paste_job_free(job);
	} else {
		gowl_bar_plugin_queue_work(plugin, paste_work, job,
		                           paste_job_free);
	}
	paste_rearm(self);
}

static gboolean
paste_busy(AiRewrite *self)
{
	gboolean busy;

	g_mutex_lock(&self->lock);
	busy = (self->paste.phase != BAR_AIREWRITE_PASTE_IDLE);
	g_mutex_unlock(&self->lock);
	return busy;
}

/* The answer the user is looking at, pasted (or copied). */
static void
paste_result(AiRewrite *self, GowlBarPlugin *plugin, gboolean inject)
{
	AirewriteStep *top = step_top(self);

	if (top == NULL || self->steps->len < 2)
		return;
	paste_start(self, plugin, top->text, inject);
}

/* An auto-paste answer arrived: act on it, outside the builder. */
static void
handle_paste_due(AiRewrite *self, GowlBarPlugin *plugin)
{
	if (!self->paste_due)
		return;
	self->paste_due = FALSE;
	if (self->view == VIEW_RESULT)
		paste_result(self, plugin, TRUE);
}

/* ----------------------------------------------------------------
 * Session
 * ---------------------------------------------------------------- */

static void
session_reset(AiRewrite *self)
{
	bar_ai_cancel(self->ai);
	g_ptr_array_set_size(self->steps, 0);
	g_clear_pointer(&self->note, g_free);
	g_clear_pointer(&self->status, g_free);
	g_clear_pointer(&self->error, g_free);
	g_clear_pointer(&self->run_action, g_free);
	g_clear_pointer(&self->run_input, g_free);
	self->run_paste = FALSE;
	self->paste_due = FALSE;
	self->origin = ORIGIN_NONE;
	self->view = VIEW_CAPTURING;
}

/* ----------------------------------------------------------------
 * The provider / model choice
 * ---------------------------------------------------------------- */

/* Where the choice is kept, or NULL with no host state directory. */
static gchar *
choice_path(GowlBarPlugin *plugin)
{
	GowlBarHost *host;
	const gchar *dir;

	host = gowl_bar_plugin_get_host(plugin);
	dir = (host != NULL) ? gowl_bar_host_get_state_dir(host) : NULL;
	if (dir == NULL || dir[0] == '\0')
		return NULL;
	return g_build_filename(dir, AIREWRITE_CHOICE_FILE, NULL);
}

/* Read the saved choice the first time it is needed.  A few bytes of
   key file, so the compositor thread can afford it once. */
static void
choice_ensure(AiRewrite *self, GowlBarPlugin *plugin)
{
	g_autofree gchar *path = NULL;

	if (self->choice_loaded)
		return;
	path = choice_path(plugin);
	if (path == NULL)
		return;   /* no host yet: try again next time */
	self->choice_loaded = TRUE;
	g_clear_pointer(&self->choice_provider, g_free);
	g_clear_pointer(&self->choice_model, g_free);
	bar_airewrite_choice_load(path, &self->choice_provider,
	                          &self->choice_model);
}

typedef struct {
	gchar *path;
	gchar *provider;
	gchar *model;
} ChoiceSaveJob;

static void
choice_save_job_free(gpointer data)
{
	ChoiceSaveJob *job = data;

	g_free(job->path);
	g_free(job->provider);
	g_free(job->model);
	g_free(job);
}

static void
choice_save_work(GowlBarPlugin *plugin, gpointer user_data)
{
	ChoiceSaveJob *job = user_data;
	g_autoptr(GError) error = NULL;

	(void)plugin;
	if (!bar_airewrite_choice_save(job->path, job->provider, job->model,
	                               &error))
		g_warning("ai-rewrite: could not save the provider choice: %s",
		          error->message);
}

/* Save the choice on a worker: a write is a write, and the compositor
   thread does none. */
static void
choice_save(AiRewrite *self, GowlBarPlugin *plugin)
{
	ChoiceSaveJob *job;
	gchar *path;

	path = choice_path(plugin);
	if (path == NULL)
		return;
	job = g_new0(ChoiceSaveJob, 1);
	job->path     = path;
	job->provider = g_strdup(self->choice_provider);
	job->model    = g_strdup(self->choice_model);
	gowl_bar_plugin_queue_work(plugin, choice_save_work, job,
	                           choice_save_job_free);
}

/* The provider a request will reach when the panel picked none: the
   widget's setting, else the default cmacs said it uses. */
static const gchar *
default_provider(GowlBarPlugin *plugin)
{
	return setting(plugin, "ai-provider",
	               setting(plugin, "ai-default-provider", NULL));
}

/* What the next request asks, for the panel to say. */
static const gchar *
shown_provider(AiRewrite *self, GowlBarPlugin *plugin)
{
	return (self->choice_provider != NULL) ? self->choice_provider
	                                        : default_provider(plugin);
}

typedef struct {
	AiRewrite *self;
	gchar     *command;
} ListJob;

static void
list_job_free(gpointer data)
{
	ListJob *job = data;

	g_free(job->command);
	g_free(job);
}

/* Standalone, no provider list was pushed: ask ai-glib's own CLI. */
static void
list_providers_work(GowlBarPlugin *plugin, gpointer user_data)
{
	ListJob *job = user_data;
	const gchar *argv[3];
	g_autofree gchar *out = NULL;
	gchar **names = NULL;

	argv[0] = job->command;
	argv[1] = "--list-providers";
	argv[2] = NULL;
	out = bar_run_argv(argv);
	if (out != NULL)
		names = bar_assistant_parse_provider_listing(out);

	g_mutex_lock(&job->self->lock);
	g_strfreev(job->self->cli_providers);
	job->self->cli_providers = names;
	g_mutex_unlock(&job->self->lock);
	gowl_bar_plugin_request_panel_refresh(plugin);
}

/* The providers to offer: cmacs's pushed list, or the `ai' listing. */
static gchar **
provider_names(AiRewrite *self, GowlBarPlugin *plugin)
{
	const gchar *pushed;
	gchar **names;

	pushed = setting(plugin, "ai-providers", NULL);
	if (pushed != NULL && pushed[0] != '\0')
		return bar_airewrite_split_names(pushed);

	g_mutex_lock(&self->lock);
	names = g_strdupv(self->cli_providers);
	if (!self->cli_listed &&
	    bar_ai_backend_for(plugin) == BAR_AI_BACKEND_CLI) {
		ListJob *job;

		self->cli_listed = TRUE;
		g_mutex_unlock(&self->lock);
		job = g_new0(ListJob, 1);
		job->self    = self;
		job->command = g_strdup(setting(plugin, "ai-command", "ai"));
		gowl_bar_plugin_queue_work(plugin, list_providers_work, job,
		                           list_job_free);
	} else {
		g_mutex_unlock(&self->lock);
	}
	return (names != NULL) ? names : g_new0(gchar *, 1);
}

/* The footer every working view carries: what the next request asks,
   and the way to change it. */
static void
add_choice_row(GowlBarPanel *panel, GowlBarPlugin *plugin, AiRewrite *self)
{
	const gchar *prov = shown_provider(self, plugin);
	g_autofree gchar *title = NULL;
	GowlBarPanelItem *item;

	title = g_strdup_printf("%s%s%s",
		(prov != NULL) ? prov : "default provider",
		(self->choice_model != NULL) ? " \xc2\xb7 " : "",
		(self->choice_model != NULL) ? self->choice_model : "");
	item = gowl_bar_panel_add_row(panel, "settings", "\xe2\x9a\x99",
		title, "Change the provider or model");
	gowl_bar_panel_item_set_value(item, "\xe2\x80\xba");
}

/* The provider / model view. */
static void
build_settings(GowlBarPanel *panel, GowlBarPlugin *plugin, AiRewrite *self)
{
	g_auto(GStrv) names = NULL;
	g_auto(GStrv) models = NULL;
	g_autofree gchar *models_key = NULL;
	g_autofree gchar *default_title = NULL;
	GowlBarPanelItem *item;
	const gchar *def;
	const gchar *prov;
	gchar rev[16];
	guint i;

	def = default_provider(plugin);
	gowl_bar_panel_add_section(panel, "Provider");
	default_title = (def != NULL)
		? g_strdup_printf("Default (%s)", def)
		: g_strdup("Default");
	item = gowl_bar_panel_add_row(panel, "provider:", NULL, default_title,
		(bar_ai_backend_for(plugin) == BAR_AI_BACKEND_CMACS)
		? "Whatever cmacs-ai uses for the AI menu"
		: "ai-glib's saved default, or ai-provider");
	gowl_bar_panel_item_set_selected(item, self->choice_provider == NULL);

	names = provider_names(self, plugin);
	for (i = 0; names[i] != NULL; i++) {
		g_autofree gchar *id = g_strconcat("provider:", names[i], NULL);

		item = gowl_bar_panel_add_row(panel, id, NULL, names[i], NULL);
		gowl_bar_panel_item_set_selected(item,
			g_strcmp0(self->choice_provider, names[i]) == 0);
	}
	if (names[0] == NULL)
		gowl_bar_panel_add_label(panel,
			"No provider list yet.  Under cmacs it is pushed at"
			" startup; standalone it comes from `ai"
			" --list-providers'.");

	/* Models: what cmacs pushed for this provider, plus free text,
	   which always works. */
	prov = shown_provider(self, plugin);
	gowl_bar_panel_add_section(panel, "Model");
	item = gowl_bar_panel_add_row(panel, "model-default", NULL,
		"The provider's default", NULL);
	gowl_bar_panel_item_set_selected(item, self->choice_model == NULL);
	if (prov != NULL) {
		models_key = g_strconcat("models.", prov, NULL);
		models = bar_airewrite_split_names(setting(plugin, models_key,
		                                           NULL));
		for (i = 0; models[i] != NULL; i++) {
			g_autofree gchar *id = g_strconcat("model:", models[i],
			                                   NULL);

			item = gowl_bar_panel_add_row(panel, id, NULL, models[i],
			                              NULL);
			gowl_bar_panel_item_set_selected(item,
				g_strcmp0(self->choice_model, models[i]) == 0);
		}
	}
	item = gowl_bar_panel_add_entry(panel, "model",
		"Or type a model id, then Return", "");
	g_snprintf(rev, sizeof(rev), "%u", self->model_rev);
	gowl_bar_panel_item_set_prop(item, GOWL_BAR_ENTRY_PROP_REVISION, rev);

	gowl_bar_panel_add_label(panel,
		"Saved for next time.  An action that names its own provider"
		" still uses it.");
	item = gowl_bar_panel_add_buttons(panel, "settings-close");
	gowl_bar_panel_add_button(item, "Done", TRUE);
}

/* Act on a click in the provider / model view.  Returns TRUE when it
   was one of its controls. */
static gboolean
settings_action(AiRewrite *self, GowlBarPlugin *plugin, const gchar *item_id)
{
	if (g_str_has_prefix(item_id, "provider:")) {
		const gchar *name = item_id + strlen("provider:");
		gchar *next = (name[0] != '\0' &&
		               bar_airewrite_choice_name_ok(name))
			? g_strdup(name) : NULL;

		/* A model belongs to its provider: switching drops it. */
		if (g_strcmp0(next, self->choice_provider) != 0)
			g_clear_pointer(&self->choice_model, g_free);
		g_free(self->choice_provider);
		self->choice_provider = next;
		choice_save(self, plugin);
		update_label(self, plugin);
		return TRUE;
	}
	if (g_str_has_prefix(item_id, "model:")) {
		const gchar *name = item_id + strlen("model:");

		if (bar_airewrite_choice_name_ok(name)) {
			set_str(&self->choice_model, name);
			choice_save(self, plugin);
		}
		return TRUE;
	}
	if (strcmp(item_id, "model-default") == 0) {
		g_clear_pointer(&self->choice_model, g_free);
		self->model_rev++;
		choice_save(self, plugin);
		return TRUE;
	}
	if (strcmp(item_id, "settings-close") == 0) {
		self->settings_open = FALSE;
		return TRUE;
	}
	return FALSE;
}

static void
airewrite_panel_opened(GowlBarPlugin *plugin, gpointer data)
{
	AiRewrite *self = data;
	const BarEnv *env = bar_env();
	CaptureJob *job;
	GowlClient *focused = NULL;

	if (self == NULL)
		return;
	session_reset(self);
	choice_ensure(self, plugin);
	/* A right-click on the icon opens straight on the provider
	   choice; the selection is still read underneath it, so Done
	   lands on the actions as usual. */
	self->settings_open = self->settings_on_open;
	self->settings_on_open = FALSE;

	/* The window to paste back into is the one focused NOW, before
	   anything the panel does can move focus. */
	if (env != NULL && env->compositor != NULL)
		focused = gowl_compositor_get_focused_client(
			GOWL_COMPOSITOR(env->compositor));
	g_weak_ref_set(&self->target, focused);

	job = g_new0(CaptureJob, 1);
	job->self       = self;
	job->paste_cmd  = g_strdup(setting(plugin, "paste-command",
	                                   "wl-paste"));
	job->config     = g_strdup(setting(plugin, "config",
	                                   AIREWRITE_DEFAULT_CONFIG));
	job->timeout_ms = CLAMP(gowl_bar_plugin_get_setting_int(plugin,
	                        "capture-timeout", 3), 1, 30) * 1000;
	job->max_chars  = CLAMP(gowl_bar_plugin_get_setting_int(plugin,
	                        "max-chars", 20000), 1, 1000000);

	g_mutex_lock(&self->lock);
	self->capture_gen++;
	self->capture_done = FALSE;
	job->gen = self->capture_gen;
	g_mutex_unlock(&self->lock);

	gowl_bar_plugin_queue_work(plugin, capture_work, job,
	                           capture_job_free);
}

static void
airewrite_panel_closed(GowlBarPlugin *plugin, gpointer data)
{
	AiRewrite *self = data;

	(void)plugin;
	if (self == NULL)
		return;

	self->settings_open = FALSE;

	/* A capture still running belongs to a panel nobody is looking
	   at; a model still answering, likewise.  A paste in flight is
	   NOT cancelled: closing the panel is its first step. */
	g_mutex_lock(&self->lock);
	self->capture_gen++;
	g_mutex_unlock(&self->lock);
	if (self->view == VIEW_RUNNING) {
		bar_ai_cancel(self->ai);
		self->view = VIEW_ACTIONS;
	}
}

/* ----------------------------------------------------------------
 * The diff canvas
 * ---------------------------------------------------------------- */

static PangoLayout *
diff_layout(AiRewrite *self, GowlBarPlugin *plugin, cairo_t *cr,
            const GowlBarTheme *theme, gint width)
{
	AirewriteStep *top = step_top(self);
	PangoLayout *layout;
	PangoFontDescription *font;
	PangoAttrList *attrs;
	GString *text;
	gdouble del[4], ins[4];
	guint i;

	layout = pango_cairo_create_layout(cr);
	font = pango_font_description_from_string(
		(theme != NULL && gowl_bar_theme_get_font(theme) != NULL)
			? gowl_bar_theme_get_font(theme) : "Sans 11");
	pango_layout_set_font_description(layout, font);
	pango_font_description_free(font);
	pango_layout_set_width(layout, MAX(width, 1) * PANGO_SCALE);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);

	if (top == NULL || top->ops == NULL) {
		pango_layout_set_text(layout, "", -1);
		return layout;
	}

	canvas_color(plugin, theme, cr, "color-deleted",
	             GOWL_BAR_COLOR_RED, del);
	canvas_color(plugin, theme, cr, "color-inserted",
	             GOWL_BAR_COLOR_GREEN, ins);

	text = g_string_new(NULL);
	attrs = pango_attr_list_new();
	for (i = 0; i < top->ops->len; i++) {
		const BarAirewriteDiffOp *op = g_ptr_array_index(top->ops, i);
		guint start = (guint)text->len;
		PangoAttribute *at;
		const gdouble *c;

		g_string_append(text, op->text);
		if (op->kind == BAR_AIREWRITE_DIFF_EQUAL)
			continue;

		c = (op->kind == BAR_AIREWRITE_DIFF_DELETE) ? del : ins;
		at = pango_attr_foreground_new((guint16)(c[0] * 65535.0),
		                               (guint16)(c[1] * 65535.0),
		                               (guint16)(c[2] * 65535.0));
		at->start_index = start;
		at->end_index = (guint)text->len;
		pango_attr_list_insert(attrs, at);

		/* Colour is not the only signal: struck through for
		   deleted, underlined for inserted, so the diff reads for
		   someone who cannot tell red from green. */
		if (op->kind == BAR_AIREWRITE_DIFF_DELETE)
			at = pango_attr_strikethrough_new(TRUE);
		else
			at = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
		at->start_index = start;
		at->end_index = (guint)text->len;
		pango_attr_list_insert(attrs, at);
	}
	pango_layout_set_text(layout, text->str, (gint)text->len);
	pango_layout_set_attributes(layout, attrs);
	pango_attr_list_unref(attrs);
	g_string_free(text, TRUE);
	return layout;
}

/* The height the diff needs at the panel's width, measured with the
   same layout the canvas will draw. */
static gint
diff_height(AiRewrite *self, GowlBarPlugin *plugin, gint panel_width)
{
	GowlBarHost *host = gowl_bar_plugin_get_host(plugin);
	const GowlBarTheme *theme = NULL;
	cairo_surface_t *surface;
	cairo_t *cr;
	PangoLayout *layout;
	gint pad = 12, h = 0;

	if (host != NULL)
		theme = gowl_bar_host_get_theme(host);
	if (theme != NULL)
		pad = gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_PAD_X);

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cr = cairo_create(surface);
	layout = diff_layout(self, plugin, cr, theme, panel_width - 2 * pad);
	pango_layout_get_pixel_size(layout, NULL, &h);
	g_object_unref(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	return CLAMP(h + 8, 32, 2400);
}

static void
airewrite_panel_canvas(GowlBarPlugin *plugin, gpointer data,
                       const gchar *item_id, cairo_t *cr,
                       const GowlBarTheme *theme, gint width, gint height)
{
	AiRewrite *self = data;
	PangoLayout *layout;
	gdouble fg[4];

	(void)height;
	if (self == NULL || g_strcmp0(item_id, "diff") != 0)
		return;

	canvas_color(plugin, theme, cr, "color-text", GOWL_BAR_COLOR_TEXT, fg);
	layout = diff_layout(self, plugin, cr, theme, width);
	cairo_set_source_rgba(cr, fg[0], fg[1], fg[2], fg[3]);
	cairo_move_to(cr, 0, 4);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

/* ----------------------------------------------------------------
 * The panel
 * ---------------------------------------------------------------- */

static const gchar *
origin_title(AirewriteOrigin origin)
{
	switch (origin) {
	case ORIGIN_PRIMARY:   return "Selected text";
	case ORIGIN_CLIPBOARD: return "From the clipboard";
	case ORIGIN_TYPED:     return "Typed text";
	case ORIGIN_NONE:
	default:               return "Text";
	}
}

static void
add_hints(GowlBarPanel *panel, AiRewrite *self)
{
	if (!self->hints) {
		gowl_bar_panel_add_label(panel, "? keys");
		return;
	}
	gowl_bar_panel_add_label(panel,
		"letter: run that action on the text shown  \xc2\xb7  "
		"Shift+letter: run it and paste the answer  \xc2\xb7  "
		"v / Enter on the diff: paste  \xc2\xb7  y: copy  \xc2\xb7  "
		"Backspace: back one step  \xc2\xb7  j / k / Tab: move  "
		"\xc2\xb7  ?: hide keys  \xc2\xb7  Esc: close");
}

static void
add_action_rows(GowlBarPanel *panel, GowlBarPlugin *plugin, AiRewrite *self)
{
	GowlBarColor key_role;
	guint i;

	key_role = role_setting(plugin, "color-key", GOWL_BAR_COLOR_ACCENT);
	for (i = 0; i < self->actions->len; i++) {
		const BarAirewriteAction *a = g_ptr_array_index(self->actions, i);
		GowlBarPanelItem *row;
		g_autofree gchar *id = g_strdup_printf("act:%s", a->id);
		gchar key[2];

		key[0] = a->key;
		key[1] = '\0';
		row = gowl_bar_panel_add_row(panel, id,
			(a->key != '\0') ? key : "\xc2\xb7", a->label,
			self->hints && a->key != '\0'
				? "Shift+key runs it and pastes" : NULL);
		gowl_bar_panel_item_set_color(row, key_role);
		if (a->input != NULL)
			gowl_bar_panel_item_set_value(row, "asks\xe2\x80\xa6");
		else if (!a->preview)
			gowl_bar_panel_item_set_value(row, "pastes");
	}
}

static void
add_source(GowlBarPanel *panel, AiRewrite *self)
{
	AirewriteStep *top = step_top(self);
	g_autofree gchar *preview = NULL;

	if (top == NULL)
		return;
	gowl_bar_panel_add_section(panel, (self->steps->len > 1)
		? "Current text" : origin_title(self->origin));
	preview = bar_airewrite_preview(top->text, AIREWRITE_PREVIEW_CHARS,
	                                AIREWRITE_PREVIEW_LINES);
	gowl_bar_panel_add_label(panel, preview);
}

static GowlBarPanel *
airewrite_panel(GowlBarPlugin *plugin, gpointer data)
{
	AiRewrite *self = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	gint width;
	gchar rev[16];
	gboolean backend_none, tools;
	g_autofree gchar *backend_why = NULL;
	g_autofree gchar *yaml_error = NULL;

	panel = gowl_bar_panel_new();
	width = CLAMP(gowl_bar_plugin_get_setting_int(plugin, "width",
	              AIREWRITE_DEFAULT_WIDTH), 240, 2000);
	gowl_bar_panel_set_width(panel, width);
	gowl_bar_panel_set_centered(panel, TRUE);
	if (self == NULL)
		return panel;

	capture_absorb(self);
	actions_refresh(self, plugin);
	ai_absorb(self);
	choice_ensure(self, plugin);

	if (self->settings_open) {
		gowl_bar_panel_add_hero(panel, "\xe2\x9a\x99", "AI rewrite",
		                        "Which model rewrites your text");
		build_settings(panel, plugin, self);
		return panel;
	}

	g_mutex_lock(&self->lock);
	backend_none = self->backend_none;
	tools = self->have_clipboard_tools;
	backend_why = g_strdup(self->backend_why);
	yaml_error = g_strdup(self->yaml_error);
	g_mutex_unlock(&self->lock);

	gowl_bar_panel_add_hero(panel, "\xe2\x9c\x8e", "AI rewrite",
	                        (self->note != NULL) ? self->note : NULL);
	if (backend_none && backend_why != NULL)
		gowl_bar_panel_add_label(panel, backend_why);
	if (yaml_error != NULL)
		gowl_bar_panel_add_label(panel, yaml_error);
	g_snprintf(rev, sizeof(rev), "%u", self->entry_rev);

	switch (self->view) {
	case VIEW_CAPTURING:
		/* No id: a status row, not a control.  An id would make
		   it hover and click and do nothing. */
		item = gowl_bar_panel_add_row(panel, NULL, NULL,
		                              "Reading the selection\xe2\x80\xa6",
		                              NULL);
		gowl_bar_panel_item_set_busy(item, TRUE);
		break;

	case VIEW_ENTER_TEXT:
		gowl_bar_panel_add_section(panel, "Nothing selected");
		item = gowl_bar_panel_add_entry(panel, "text",
			"Type or paste the text to rewrite, then Ctrl+Return",
			"");
		gowl_bar_panel_item_set_prop(item,
			GOWL_BAR_ENTRY_PROP_MULTILINE, "true");
		gowl_bar_panel_item_set_prop(item,
			GOWL_BAR_ENTRY_PROP_AUTOFOCUS, "true");
		gowl_bar_panel_item_set_prop(item,
			GOWL_BAR_ENTRY_PROP_REVISION, rev);
		gowl_bar_panel_item_set_height(item, 140);
		if (!tools)
			gowl_bar_panel_add_label(panel,
				"The result can only be shown here until"
				" wl-clipboard is installed.");
		add_choice_row(panel, plugin, self);
		break;

	case VIEW_ACTIONS:
		add_source(panel, self);
		gowl_bar_panel_add_section(panel, "Rewrite");
		add_action_rows(panel, plugin, self);
		if (self->status != NULL)
			gowl_bar_panel_add_label(panel, self->status);
		add_choice_row(panel, plugin, self);
		add_hints(panel, self);
		break;

	case VIEW_INPUT: {
		const BarAirewriteAction *a;

		a = bar_airewrite_actions_find_id(self->actions,
		                                  self->run_action);
		add_source(panel, self);
		gowl_bar_panel_add_section(panel,
			(a != NULL) ? a->label : "Instruction");
		item = gowl_bar_panel_add_entry(panel, "input",
			(a != NULL && a->input != NULL) ? a->input
			                                : "What should it do?",
			"");
		gowl_bar_panel_item_set_prop(item,
			GOWL_BAR_ENTRY_PROP_AUTOFOCUS, "true");
		gowl_bar_panel_item_set_prop(item,
			GOWL_BAR_ENTRY_PROP_REVISION, rev);
		gowl_bar_panel_add_label(panel,
			"Return runs it  \xc2\xb7  Esc closes");
		break;
	}

	case VIEW_RUNNING: {
		const BarAirewriteAction *a;
		g_autofree gchar *text = NULL;
		g_autofree gchar *preview = NULL;
		g_autofree gchar *waiting = NULL;
		const gchar *provider;

		/* Name the provider when one is known: the widget's own
		   setting, else the default cmacs says it will use. */
		provider = shown_provider(self, plugin);
		waiting = (provider != NULL)
			? g_strdup_printf("Waiting for %s\xe2\x80\xa6", provider)
			: g_strdup("Waiting for the model\xe2\x80\xa6");
		a = bar_airewrite_actions_find_id(self->actions,
		                                  self->run_action);
		item = gowl_bar_panel_add_row(panel, "running", NULL,
			(a != NULL) ? a->label : "Asking", waiting);
		gowl_bar_panel_item_set_busy(item, TRUE);
		(void)bar_ai_get(self->ai, &text, NULL);
		if (text != NULL && text[0] != '\0') {
			/* The tail, not the head: it is the part changing. */
			glong n = g_utf8_strlen(text, -1);
			const gchar *from = (n > 600)
				? g_utf8_offset_to_pointer(text, n - 600) : text;

			preview = bar_airewrite_preview(from, 600, 12);
			gowl_bar_panel_add_label(panel, preview);
		}
		item = gowl_bar_panel_add_buttons(panel, "running");
		gowl_bar_panel_add_button(item, "Cancel", FALSE);
		break;
	}

	case VIEW_RESULT: {
		AirewriteStep *top = step_top(self);
		const BarAirewriteAction *a = NULL;
		g_autofree gchar *title = NULL;

		if (top != NULL)
			a = bar_airewrite_actions_find_id(self->actions,
			                                  top->action_id);
		title = (self->steps->len > 2)
			? g_strdup_printf("%s (step %u)",
			                  (a != NULL) ? a->label : "Result",
			                  self->steps->len - 1)
			: g_strdup((a != NULL) ? a->label : "Result");
		gowl_bar_panel_add_section(panel, title);
		gowl_bar_panel_add_canvas(panel, "diff",
		                          diff_height(self, plugin, width));
		if (self->status != NULL)
			gowl_bar_panel_add_label(panel, self->status);
		item = gowl_bar_panel_add_buttons(panel, "result");
		gowl_bar_panel_add_button(item,
			(self->origin == ORIGIN_TYPED) ? "Copy \xe2\x8f\x8e"
			                               : "Paste \xe2\x8f\x8e",
			TRUE);
		gowl_bar_panel_add_button(item, "Copy (y)", FALSE);
		gowl_bar_panel_add_button(item, "Again", FALSE);
		gowl_bar_panel_add_button(item, "Back \xe2\x8c\xab", FALSE);
		gowl_bar_panel_add_section(panel, "Then");
		add_action_rows(panel, plugin, self);
		add_choice_row(panel, plugin, self);
		add_hints(panel, self);
		break;
	}

	case VIEW_ERROR:
	default:
		gowl_bar_panel_add_section(panel, "It did not work");
		gowl_bar_panel_add_label(panel, (self->error != NULL)
			? self->error : "unknown error");
		item = gowl_bar_panel_add_buttons(panel, "error");
		gowl_bar_panel_add_button(item, "Again", TRUE);
		gowl_bar_panel_add_button(item, "Back", FALSE);
		/* A wrong provider is the usual reason; offer the fix. */
		add_choice_row(panel, plugin, self);
		break;
	}
	return panel;
}

/* ----------------------------------------------------------------
 * Interaction
 * ---------------------------------------------------------------- */

/* Backspace: one step back, whatever "back" means where we are. */
static void
go_back(AiRewrite *self)
{
	switch (self->view) {
	case VIEW_RUNNING:
		bar_ai_cancel(self->ai);
		self->view = (self->steps->len > 1) ? VIEW_RESULT : VIEW_ACTIONS;
		break;
	case VIEW_RESULT:
		if (self->steps->len > 1)
			g_ptr_array_remove_index(self->steps,
			                         self->steps->len - 1);
		self->view = (self->steps->len > 1) ? VIEW_RESULT : VIEW_ACTIONS;
		break;
	case VIEW_INPUT:
	case VIEW_ERROR:
		self->view = (self->steps->len > 1) ? VIEW_RESULT : VIEW_ACTIONS;
		break;
	case VIEW_CAPTURING:
	case VIEW_ENTER_TEXT:
	case VIEW_ACTIONS:
	default:
		break;
	}
	g_clear_pointer(&self->status, g_free);
}

/* Run the action that produced the top step again, on the text below. */
static void
run_again(AiRewrite *self, GowlBarPlugin *plugin)
{
	g_autofree gchar *action = NULL;
	g_autofree gchar *input = NULL;
	AirewriteStep *top = step_top(self);

	if (self->view == VIEW_ERROR) {
		action = g_strdup(self->run_action);
		input = g_strdup(self->run_input);
	} else if (top != NULL && top->action_id != NULL) {
		action = g_strdup(top->action_id);
		input = g_strdup(top->input);
		g_ptr_array_remove_index(self->steps, self->steps->len - 1);
	}
	if (action != NULL)
		run_action(self, plugin, action, input, FALSE);
}

static void
airewrite_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
                 gint index, gdouble value, guint button)
{
	AiRewrite *self = data;

	(void)value;
	(void)button;
	if (self == NULL || item_id == NULL)
		return;
	capture_absorb(self);
	ai_absorb(self);

	if (strcmp(item_id, "settings") == 0) {
		self->settings_open = TRUE;
		return;
	}
	if (settings_action(self, plugin, item_id))
		return;

	if (g_str_has_prefix(item_id, "act:")) {
		if (self->view == VIEW_ACTIONS || self->view == VIEW_RESULT ||
		    self->view == VIEW_ERROR)
			run_action(self, plugin, item_id + 4, NULL, FALSE);
	} else if (strcmp(item_id, "diff") == 0) {
		/* Return on the focused diff (index -1); a click on the
		   text itself does nothing. */
		if (index < 0 && self->view == VIEW_RESULT)
			paste_result(self, plugin, TRUE);
	} else if (strcmp(item_id, "result") == 0) {
		switch (index) {
		case 0:  paste_result(self, plugin, TRUE);  break;
		case 1:  paste_result(self, plugin, FALSE); break;
		case 2:  run_again(self, plugin);           break;
		case 3:  go_back(self);                     break;
		default: break;
		}
	} else if (strcmp(item_id, "error") == 0) {
		if (index == 0)
			run_again(self, plugin);
		else
			go_back(self);
	} else if (strcmp(item_id, "running") == 0) {
		go_back(self);
	}
	handle_paste_due(self, plugin);
}

static void
airewrite_panel_text(GowlBarPlugin *plugin, gpointer data,
                     const gchar *item_id, const gchar *text,
                     gboolean submitted)
{
	AiRewrite *self = data;
	g_autofree gchar *trimmed = NULL;
	gint max_chars;

	if (self == NULL || !submitted || text == NULL)
		return;
	trimmed = g_strstrip(g_strdup(text));
	if (trimmed[0] == '\0')
		return;

	if (g_strcmp0(item_id, "model") == 0 && self->settings_open) {
		if (bar_airewrite_choice_name_ok(trimmed)) {
			set_str(&self->choice_model, trimmed);
			choice_save(self, plugin);
		} else {
			set_str(&self->note, "That is not a model id.");
		}
		/* Clear the field: the pick shows in the rows above. */
		self->model_rev++;
		return;
	}

	if (g_strcmp0(item_id, "text") == 0 && self->view == VIEW_ENTER_TEXT) {
		AirewriteStep *step;

		max_chars = CLAMP(gowl_bar_plugin_get_setting_int(plugin,
		                  "max-chars", 20000), 1, 1000000);
		if (g_utf8_strlen(text, -1) > (glong)max_chars) {
			set_str(&self->note, "That is longer than max-chars.");
			return;
		}
		/* Kept exactly as typed: the trim was only to refuse an
		   empty submit. */
		step = g_new0(AirewriteStep, 1);
		step->text = g_strdup(text);
		g_ptr_array_add(self->steps, step);
		self->origin = ORIGIN_TYPED;
		set_str(&self->note, "The result goes to the clipboard.");
		self->view = VIEW_ACTIONS;
		return;
	}

	if (g_strcmp0(item_id, "input") == 0 && self->view == VIEW_INPUT &&
	    self->run_action != NULL) {
		g_autofree gchar *action = g_strdup(self->run_action);

		run_action(self, plugin, action, trimmed, self->run_paste);
	}
}

/*
 * Keys the panel's navigation did not take.  j, k, Tab, Space and
 * Escape are the host's, and Return is too once a row has the keyboard
 * (in the text views every printable key goes to the field); everything
 * else lands here.
 *
 * Every letter is claimed in the list and result views, bound or not:
 * an unclaimed key closes the panel, and a typo should not throw away
 * a result the model took twenty seconds to write.
 */
static gboolean
airewrite_panel_key(GowlBarPlugin *plugin, gpointer data, guint keysym,
                    guint modifiers, gint focused_item)
{
	AiRewrite *self = data;
	gboolean shift;
	gchar c = '\0';

	(void)focused_item;
	if (self == NULL)
		return FALSE;

	/* A key can beat the rebuild that follows the capture or the
	   answer: bring the state up to date before deciding anything. */
	capture_absorb(self);
	ai_absorb(self);

	/* The provider choice is a list to click, not a place for action
	   letters: BackSpace returns, other plain keys are kept from
	   closing the panel, and everything else is the host's. */
	if (self->settings_open) {
		if (keysym == XKB_KEY_BackSpace) {
			self->settings_open = FALSE;
			return TRUE;
		}
		return (modifiers & (GOWL_KEY_MOD_CTRL | GOWL_KEY_MOD_ALT |
		                     GOWL_KEY_MOD_LOGO)) == 0 &&
		       xkb_keysym_to_utf32(keysym) >= 0x20;
	}

	if (keysym == XKB_KEY_question) {
		self->hints = !self->hints;
		return TRUE;
	}
	if (keysym == XKB_KEY_BackSpace) {
		go_back(self);
		return TRUE;
	}

	/* Return on a result pastes it.  The host offers Return here when
	   nothing in the panel has the keyboard yet (and first when the
	   diff canvas has it), so Enter alone works the moment the answer
	   is in -- no Tab to reach a button first. */
	if ((keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter) &&
	    self->view == VIEW_RESULT &&
	    (modifiers & (GOWL_KEY_MOD_CTRL | GOWL_KEY_MOD_ALT |
	                  GOWL_KEY_MOD_LOGO)) == 0) {
		paste_result(self, plugin, TRUE);
		handle_paste_due(self, plugin);
		return TRUE;
	}

	if (keysym >= XKB_KEY_a && keysym <= XKB_KEY_z)
		c = (gchar)keysym;
	else if (keysym >= XKB_KEY_A && keysym <= XKB_KEY_Z)
		c = (gchar)(keysym - XKB_KEY_A + XKB_KEY_a);
	else if (keysym >= XKB_KEY_0 && keysym <= XKB_KEY_9)
		c = (gchar)keysym;
	if (c == '\0')
		return FALSE;

	/* Ctrl and Alt chords are not ours to swallow. */
	if ((modifiers & (GOWL_KEY_MOD_CTRL | GOWL_KEY_MOD_ALT |
	                  GOWL_KEY_MOD_LOGO)) != 0)
		return FALSE;
	shift = ((modifiers & GOWL_KEY_MOD_SHIFT) != 0) ||
	        (keysym >= XKB_KEY_A && keysym <= XKB_KEY_Z);

	switch (self->view) {
	case VIEW_RESULT:
		if (c == 'v' && !shift) {
			paste_result(self, plugin, TRUE);
			return TRUE;
		}
		if (c == 'y' && !shift) {
			paste_result(self, plugin, FALSE);
			return TRUE;
		}
		/* Any other letter chains an action on the result. */
		G_GNUC_FALLTHROUGH;
	case VIEW_ACTIONS:
	case VIEW_ERROR: {
		const BarAirewriteAction *a;

		actions_refresh(self, plugin);
		a = bar_airewrite_actions_find_key(self->actions, c);
		if (a != NULL) {
			g_autofree gchar *id = g_strdup(a->id);

			run_action(self, plugin, id, NULL, shift);
			handle_paste_due(self, plugin);
		}
		return TRUE;
	}
	case VIEW_CAPTURING:
	case VIEW_RUNNING:
		return TRUE;
	case VIEW_ENTER_TEXT:
	case VIEW_INPUT:
	default:
		return FALSE;
	}
}

/* ----------------------------------------------------------------
 * The bar side
 * ---------------------------------------------------------------- */

static gboolean
is_busy(AiRewrite *self)
{
	gboolean capturing;

	g_mutex_lock(&self->lock);
	capturing = (self->view == VIEW_CAPTURING && !self->capture_done);
	g_mutex_unlock(&self->lock);
	return capturing || bar_ai_is_busy(self->ai) || paste_busy(self);
}

static gint
airewrite_interval(GowlBarPlugin *plugin, gpointer data)
{
	AiRewrite *self = data;

	(void)plugin;
	/* Once a second while anything is in flight -- the CLI route's
	   answer and the poll-driven paste both ride on it -- and rarely
	   otherwise: the widget has nothing of its own to watch. */
	return (self != NULL && is_busy(self)) ? 1 : 30;
}

static void
update_label(AiRewrite *self, GowlBarPlugin *plugin)
{
	const gchar *label = setting(plugin, "label", "\xe2\x9c\x8e");
	gboolean busy = bar_ai_is_busy(self->ai);

	gowl_bar_plugin_set_visible(plugin,
		gowl_bar_plugin_get_setting_bool(plugin, "show-in-bar", TRUE));
	gowl_bar_plugin_set_label(plugin, label);
	bar_plugin_apply_color(plugin,
		gowl_bar_plugin_get_setting(plugin, "color"),
		busy ? GOWL_BAR_COLOR_PEACH : GOWL_BAR_COLOR_TEXT);
	gowl_bar_plugin_set_tooltip(plugin, busy
		? "AI rewrite: the model is answering"
		: "AI rewrite: select text anywhere, then open this"
		  " (gowl bar-panel ai-rewrite).  Right-click to pick the"
		  " provider and model.");
}

static void
airewrite_poll(GowlBarPlugin *plugin, gpointer data)
{
	AiRewrite *self = data;
	AirewriteView before;

	if (self == NULL)
		return;
	before = self->view;
	capture_absorb(self);
	ai_absorb(self);
	handle_paste_due(self, plugin);

	/* The compositor's timer drives the paste machine; the poll
	   drives it too, which is all there is without a compositor and a
	   safety net with one (a step taken twice is a no-op: each one
	   moves the phase on). */
	airewrite_paste_tick(self);

	update_label(self, plugin);
	if (self->view != before || self->view == VIEW_RUNNING)
		gowl_bar_plugin_request_panel_refresh(plugin);
}

static void
airewrite_configure(GowlBarPlugin *plugin, gpointer data,
                    GHashTable *settings)
{
	AiRewrite *self = data;

	(void)settings;
	if (self == NULL)
		return;

	/* cmacs pushes its answer as settings; every push lands here. */
	if (bar_ai_absorb_settings(self->ai, plugin))
		gowl_bar_plugin_request_panel_refresh(plugin);
	actions_refresh(self, plugin);
	ai_absorb(self);
	handle_paste_due(self, plugin);
	update_label(self, plugin);
}

static gpointer
airewrite_create(GowlBarPlugin *plugin)
{
	AiRewrite *self;

	self = g_new0(AiRewrite, 1);
	self->plugin = plugin;
	self->ai = bar_ai_new(AIREWRITE_NAME);
	g_mutex_init(&self->lock);
	g_weak_ref_init(&self->target, NULL);
	self->steps = g_ptr_array_new_with_free_func(step_free);
	self->view = VIEW_CAPTURING;
	self->have_clipboard_tools = TRUE;
	return self;
}

static void
airewrite_destroy(GowlBarPlugin *plugin, gpointer data)
{
	AiRewrite *self = data;

	(void)plugin;
	if (self == NULL)
		return;
	if (self->timer != NULL)
		wl_event_source_remove(self->timer);
	bar_ai_free(self->ai);
	g_weak_ref_clear(&self->target);
	g_ptr_array_unref(self->steps);
	if (self->actions != NULL)
		g_ptr_array_unref(self->actions);
	if (self->yaml_actions != NULL)
		g_ptr_array_unref(self->yaml_actions);
	if (self->paste_previous != NULL) {
		memset(self->paste_previous, 0, strlen(self->paste_previous));
		g_free(self->paste_previous);
	}
	g_free(self->actions_key);
	g_free(self->choice_provider);
	g_free(self->choice_model);
	g_strfreev(self->cli_providers);
	g_free(self->note);
	g_free(self->status);
	g_free(self->error);
	g_free(self->run_action);
	g_free(self->run_input);
	g_free(self->captured);
	g_free(self->captured_note);
	g_free(self->backend_why);
	g_free(self->yaml_error);
	g_free(self->paste_keys);
	g_free(self->paste_app_id);
	g_free(self->paste_copy_cmd);
	g_mutex_clear(&self->lock);
	g_free(self);
}

/*
 * Right-click on the icon opens straight on the provider and model
 * choice; a left click opens the panel as usual (on the selection).
 */
static gboolean
airewrite_click(GowlBarPlugin *plugin, gpointer data, guint button, gint x,
                gint y, guint modifiers)
{
	AiRewrite *self = data;
	GowlBarHost *host;

	(void)x;
	(void)y;
	(void)modifiers;
	if (self == NULL || button != BTN_RIGHT)
		return FALSE;
	host = gowl_bar_plugin_get_host(plugin);
	if (host == NULL)
		return FALSE;
	self->settings_on_open = TRUE;
	gowl_bar_host_open_panel(host, gowl_bar_plugin_get_id(plugin));
	return TRUE;
}

static const GowlBarPluginVTable airewrite_vtable = {
	sizeof(GowlBarPluginVTable),
	airewrite_create, airewrite_destroy,
	NULL, NULL, airewrite_configure,
	airewrite_interval, airewrite_poll, NULL,
	NULL, NULL,
	airewrite_click, NULL,
	airewrite_panel, airewrite_action,
	airewrite_panel_opened, airewrite_panel_closed,
	airewrite_panel_key,
	NULL,
	airewrite_panel_text, airewrite_panel_canvas
};

/**
 * bar_register_airewrite_plugins:
 * @registry: the registry
 *
 * Registers `ai-rewrite' and its aliases `ai-edit' and `rewrite'.
 */
void
bar_register_airewrite_plugins(GowlBarRegistry *registry)
{
	gowl_bar_registry_register_vtable(registry, AIREWRITE_NAME,
		"AI rewrite",
		"Rewrite the selected text with AI and paste it back",
		&airewrite_vtable);
	gowl_bar_registry_register_alias(registry, "ai-edit", AIREWRITE_NAME);
	gowl_bar_registry_register_alias(registry, "rewrite", AIREWRITE_NAME);
}
