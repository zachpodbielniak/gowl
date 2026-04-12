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
 * GowlModuleRecording:
 *
 * A compositor module that records screen content to video files
 * by piping raw frames to an ffmpeg subprocess.  Supports the same
 * four selection modes as the screenshot module:
 *
 *   - desktop:  record the current (or named) monitor
 *   - window:   record a specific client surface
 *   - area:     record a fixed rectangular region
 *   - all:      record all monitors stitched
 *
 * For area mode, the region coordinates must be supplied by the
 * caller (typically obtained from the screenshot module's
 * interactive area selection).
 *
 * Configuration (YAML):
 *   modules:
 *     recording:
 *       enabled: false
 *       save-directory: ~/Videos/Recordings
 *       framerate: 30
 *       codec: libx264
 *       crf: 23
 *       container: mp4
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-recording"

#include <glib-object.h>
#include <gio/gio.h>
#include <gmodule.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <wordexp.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

#include "gowl-enums.h"
#include "module/gowl-module.h"
#include "module/gowl-module-manager.h"
#include "interfaces/gowl-recording-provider.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"

/* ----------------------------------------------------------------
 * Module type declaration
 * ---------------------------------------------------------------- */

#define GOWL_TYPE_MODULE_RECORDING (gowl_module_recording_get_type())

G_DECLARE_FINAL_TYPE(GowlModuleRecording, gowl_module_recording,
                     GOWL, MODULE_RECORDING, GowlModule)

struct _GowlModuleRecording {
	GowlModule  parent_instance;

	/* Borrowed references */
	GowlCompositor *compositor;

	/* Recording state */
	gboolean    recording;
	GPid        ffmpeg_pid;
	GPid        ffmpeg_pid_to_reap;   /* pid handed off to finalize */
	gint        ffmpeg_stdin_fd;
	gulong      frame_rendered_id; /* signal handler id */
	gint64      last_frame_us;     /* for FPS throttling */
	gint64      frame_interval_us;

	/* Capture parameters (fixed for duration of recording) */
	GowlCaptureMode mode;
	gchar      *output_name;     /* for desktop mode */
	GowlClient *target_client;   /* for window mode (borrowed) */
	gint        region_x;        /* for area mode */
	gint        region_y;
	gint        region_w;
	gint        region_h;

	/* Output */
	gchar      *output_path;
	gint64      start_time_us;  /* g_get_real_time() microseconds */

	/* Configuration */
	gchar      *save_directory;
	gchar      *filename_format;
	gint        framerate;
	gchar      *codec;
	gint        crf;
	gchar      *container;
	gchar      *ffmpeg_path;
	gchar      *pixel_format;
};

/* Signal IDs */
enum {
	SIGNAL_RECORDING_STARTED,
	SIGNAL_RECORDING_STOPPED,
	SIGNAL_RECORDING_ERROR,
	SIGNAL_FRAME_DROPPED,
	N_SIGNALS
};

static guint recording_signals[N_SIGNALS] = { 0, };

/* ----------------------------------------------------------------
 * Interface forward declarations
 * ---------------------------------------------------------------- */

static void recording_provider_init  (GowlRecordingProviderInterface *iface);
static void recording_startup_init   (GowlStartupHandlerInterface *iface);
static void recording_shutdown_init  (GowlShutdownHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GowlModuleRecording, gowl_module_recording,
    GOWL_TYPE_MODULE,
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_RECORDING_PROVIDER, recording_provider_init)
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER, recording_startup_init)
    G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER, recording_shutdown_init))

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

static gchar *
expand_path(const gchar *path)
{
	wordexp_t we;
	gchar *result;

	if (wordexp(path, &we, WRDE_NOCMD) != 0)
		return g_strdup(path);

	result = g_strdup(we.we_wordv[0]);
	wordfree(&we);
	return result;
}

static gchar *
generate_filename(GowlModuleRecording *self)
{
	g_autofree gchar *dir = NULL;
	gchar timebuf[64];
	time_t t;
	struct tm tm;

	dir = expand_path(self->save_directory);
	g_mkdir_with_parents(dir, 0755);

	t = time(NULL);
	localtime_r(&t, &tm);
	strftime(timebuf, sizeof(timebuf), self->filename_format, &tm);

	return g_strdup_printf("%s/%s.%s", dir, timebuf, self->container);
}

/* ----------------------------------------------------------------
 * Frame capture: signal handler (runs on compositor dispatch thread)
 * ---------------------------------------------------------------- */

static void
on_frame_rendered(GowlCompositor *compositor, GObject *monitor,
                  gpointer user_data)
{
	GowlModuleRecording *self = GOWL_MODULE_RECORDING(user_data);
	GBytes *frame = NULL;
	const guint8 *pixels;
	gsize size;
	ssize_t written;
	gint w, h;
	gint64 now_us;

	(void)compositor;
	(void)monitor;

	if (!self->recording || self->ffmpeg_stdin_fd < 0)
		return;

	/* FPS throttle: skip if last capture was too recent */
	now_us = g_get_monotonic_time();
	if (self->last_frame_us > 0 &&
	    (now_us - self->last_frame_us) < self->frame_interval_us)
		return;
	self->last_frame_us = now_us;

	/* Capture frame based on mode */
	switch (self->mode) {
	case GOWL_CAPTURE_MODE_DESKTOP:
		frame = gowl_compositor_screenshot_output(self->compositor,
		            self->output_name, &w, &h, NULL);
		break;
	case GOWL_CAPTURE_MODE_WINDOW:
		if (self->target_client != NULL)
			frame = gowl_compositor_screenshot_client(
			            self->compositor,
			            self->target_client, &w, &h, NULL);
		break;
	case GOWL_CAPTURE_MODE_AREA:
		frame = gowl_compositor_screenshot_region(self->compositor,
		            NULL,
		            self->region_x, self->region_y,
		            self->region_w, self->region_h,
		            &w, &h, NULL);
		break;
	case GOWL_CAPTURE_MODE_ALL:
		frame = gowl_compositor_screenshot_all(self->compositor,
		            &w, &h, NULL);
		break;
	}

	if (frame == NULL)
		return;

	pixels = g_bytes_get_data(frame, &size);

	/* Blocking write — loop on EINTR only.  In blocking mode,
	 * write() either writes all bytes or fails. Short writes only
	 * happen on interrupt (EINTR) before any bytes are written,
	 * or when writing more than PIPE_BUF in some edge cases. */
	{
		gsize offset = 0;

		while (offset < size) {
			written = write(self->ffmpeg_stdin_fd,
			                pixels + offset, size - offset);
			if (written > 0) {
				offset += (gsize)written;
				continue;
			}
			if (written < 0 && errno == EINTR)
				continue;
			/* Fatal: EPIPE, ECONNRESET, etc. */
			g_warning("recording: write error: %s — stopping",
			          g_strerror(errno));
			self->recording = FALSE;
			if (self->ffmpeg_stdin_fd >= 0) {
				close(self->ffmpeg_stdin_fd);
				self->ffmpeg_stdin_fd = -1;
			}
			g_signal_emit(self,
			              recording_signals[SIGNAL_RECORDING_ERROR],
			              0, g_strerror(errno));
			break;
		}
	}

	g_bytes_unref(frame);
}

/* ----------------------------------------------------------------
 * Child setup: redirect stderr to log, restore signal dispositions
 * ---------------------------------------------------------------- */

static void
recording_child_setup(gpointer user_data)
{
	gint log_fd;

	(void)user_data;

	/* Restore default signal dispositions so ffmpeg behaves normally.
	 * Emacs sets SIGPIPE to SIG_IGN and SIGCHLD to a handler; children
	 * inherit these.  ffmpeg needs the defaults. */
	signal(SIGPIPE, SIG_DFL);
	signal(SIGCHLD, SIG_DFL);
	signal(SIGINT,  SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);

	/* Redirect stderr to log file for diagnostics */
	log_fd = open("/tmp/gowl-recording.log",
	              O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (log_fd >= 0) {
		dup2(log_fd, STDERR_FILENO);
		if (log_fd != STDERR_FILENO)
			close(log_fd);
	}
}

/* ----------------------------------------------------------------
 * Start / stop recording
 * ---------------------------------------------------------------- */

static gboolean
do_start(GowlModuleRecording *self,
         GowlCaptureMode      mode,
         const gchar         *output_name,
         gpointer             client,
         gint                 region_x,
         gint                 region_y,
         gint                 region_w,
         gint                 region_h,
         const gchar         *output_path,
         GError             **error)
{
	GBytes *probe;
	gint pw, ph;
	gchar size_str[32];
	gchar fps_str[16];
	gchar crf_str[16];
	gint stdin_fd;
	gchar *argv[32];
	gint argc;
	gboolean ok;

	if (self->recording) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
		                    "A recording is already in progress");
		return FALSE;
	}

	if (self->compositor == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
		                    "Compositor not available");
		return FALSE;
	}

	/* Store capture params */
	self->mode = mode;
	g_free(self->output_name);
	self->output_name = g_strdup(output_name);
	self->target_client = client ? GOWL_CLIENT(client) : NULL;
	self->region_x = region_x;
	self->region_y = region_y;
	self->region_w = region_w;
	self->region_h = region_h;

	/* Probe frame dimensions */
	switch (mode) {
	case GOWL_CAPTURE_MODE_DESKTOP:
		probe = gowl_compositor_screenshot_output(self->compositor,
		            output_name, &pw, &ph, error);
		break;
	case GOWL_CAPTURE_MODE_WINDOW:
		if (self->target_client != NULL) {
			probe = gowl_compositor_screenshot_client(
			            self->compositor,
			            self->target_client, &pw, &ph, error);
		} else {
			g_set_error_literal(error, G_IO_ERROR,
			    G_IO_ERROR_INVALID_ARGUMENT,
			    "No client specified for window recording");
			return FALSE;
		}
		break;
	case GOWL_CAPTURE_MODE_AREA:
		pw = region_w;
		ph = region_h;
		probe = (pw > 0 && ph > 0)
		      ? GINT_TO_POINTER(1) /* dummy non-NULL */
		      : NULL;
		if (probe == NULL) {
			g_set_error_literal(error, G_IO_ERROR,
			    G_IO_ERROR_INVALID_ARGUMENT,
			    "Invalid region dimensions");
			return FALSE;
		}
		probe = NULL; /* no actual bytes to unref */
		break;
	case GOWL_CAPTURE_MODE_ALL:
		probe = gowl_compositor_screenshot_all(self->compositor,
		            &pw, &ph, error);
		break;
	default:
		g_set_error_literal(error, G_IO_ERROR,
		    G_IO_ERROR_INVALID_ARGUMENT, "Unknown capture mode");
		return FALSE;
	}

	if (mode != GOWL_CAPTURE_MODE_AREA) {
		if (probe == NULL)
			return FALSE;
		g_bytes_unref(probe);
	}

	/* Generate output path */
	if (output_path != NULL && output_path[0] != '\0') {
		g_free(self->output_path);
		self->output_path = expand_path(output_path);
		/* Ensure parent directory exists */
		{
			g_autofree gchar *dir = g_path_get_dirname(self->output_path);
			g_mkdir_with_parents(dir, 0755);
		}
	} else {
		g_free(self->output_path);
		self->output_path = generate_filename(self);
	}

	/* Build ffmpeg command line */
	g_snprintf(size_str, sizeof(size_str), "%dx%d", pw, ph);
	g_snprintf(fps_str, sizeof(fps_str), "%d", self->framerate);
	g_snprintf(crf_str, sizeof(crf_str), "%d", self->crf);

	argc = 0;
	argv[argc++] = self->ffmpeg_path;
	argv[argc++] = "-y";
	argv[argc++] = "-f";
	argv[argc++] = "rawvideo";
	argv[argc++] = "-pixel_format";
	argv[argc++] = "bgra";
	argv[argc++] = "-video_size";
	argv[argc++] = size_str;
	argv[argc++] = "-framerate";
	argv[argc++] = fps_str;
	argv[argc++] = "-i";
	argv[argc++] = "pipe:0";
	argv[argc++] = "-c:v";
	argv[argc++] = self->codec;
	/* ultrafast + zerolatency: libx264 outputs frames immediately
	 * (no lookahead, no B-frames), encoding is faster than realtime.
	 * Without these, libx264 buffers 40+ frames before outputting
	 * anything, so short recordings never flush any data. */
	argv[argc++] = "-preset";
	argv[argc++] = "ultrafast";
	argv[argc++] = "-tune";
	argv[argc++] = "zerolatency";
	/* Force keyframes every 1 second (30 frames at 30fps) so
	 * fragmented MP4 flushes a new fragment every second. */
	argv[argc++] = "-g";
	argv[argc++] = fps_str;  /* keyframe interval = framerate */
	argv[argc++] = "-crf";
	argv[argc++] = crf_str;
	argv[argc++] = "-pix_fmt";
	argv[argc++] = self->pixel_format;
	/* Fragmented MP4: write moov atom at start, each keyframe
	 * starts a new fragment (mdat/moof pair).  File is playable
	 * and current even if ffmpeg is killed abruptly. */
	argv[argc++] = "-movflags";
	argv[argc++] = "+frag_keyframe+empty_moov+default_base_moof";
	argv[argc++] = self->output_path;
	argv[argc] = NULL;

	/* Spawn ffmpeg with stderr redirected to /tmp/gowl-recording.log
	 * via a child setup callback. */
	{
		g_autofree gchar *dup_argv_str = g_strjoinv(" ", argv);
		g_message("recording: spawn cmd: %s", dup_argv_str);
	}
	ok = g_spawn_async_with_pipes(
	         NULL, argv, NULL,
	         G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD
	             | G_SPAWN_STDOUT_TO_DEV_NULL,
	         recording_child_setup, NULL,
	         &self->ffmpeg_pid,
	         &stdin_fd,
	         NULL, NULL,
	         error);

	if (!ok)
		return FALSE;

	/* Use BLOCKING writes on the ffmpeg pipe. Non-blocking writes
	 * combined with 3-30+ MB frames would require us to either loop
	 * on EAGAIN (stalling anyway) or drop partial writes (corrupting
	 * the stream). Blocking writes handle any frame size correctly:
	 * the compositor thread briefly stalls when ffmpeg can't keep up,
	 * but libx264 at 30fps on modern hardware drains frames faster
	 * than they arrive, so stalls are typically sub-millisecond.
	 *
	 * As an optimization, grow the pipe buffer to the kernel's max
	 * (often 1MB on default systems, up to 16MB with permissions).
	 * A larger buffer reduces blocking frequency. */
	{
		gint desired_sizes[] = {
			16 * 1024 * 1024,
			 8 * 1024 * 1024,
			 4 * 1024 * 1024,
			 2 * 1024 * 1024,
			 1 * 1024 * 1024,
			 0
		};
		gint i;
		for (i = 0; desired_sizes[i] > 0; i++) {
			if (fcntl(stdin_fd, F_SETPIPE_SZ,
			          desired_sizes[i]) >= 0)
				break;
		}
	}

	self->ffmpeg_stdin_fd = stdin_fd;
	self->recording = TRUE;
	self->start_time_us = g_get_real_time();
	self->last_frame_us = 0;
	self->frame_interval_us = (gint64)(1000000 / self->framerate);

	/* Connect to compositor's frame-rendered signal, which fires on
	 * the dispatch thread AFTER each monitor renders.  EGL context
	 * is free at that point, so screenshot_output() works correctly. */
	self->frame_rendered_id = g_signal_connect(
	    self->compositor, "frame-rendered",
	    G_CALLBACK(on_frame_rendered), self);

	g_signal_emit(self, recording_signals[SIGNAL_RECORDING_STARTED],
	              0, self->output_path);

	g_message("recording: started → %s (%s, %sfps, crf %d)",
	          self->output_path, size_str, fps_str, self->crf);

	return TRUE;
}

static gboolean
do_stop(GowlModuleRecording  *self,
        gchar               **out_path,
        GError              **error)
{
	if (!self->recording && self->frame_rendered_id == 0 &&
	    self->ffmpeg_stdin_fd < 0 && self->ffmpeg_pid <= 0) {
		/* Already fully stopped and cleaned up */
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
		                    "No recording in progress");
		return FALSE;
	}

	self->recording = FALSE;

	/* Disconnect frame-rendered signal.  Any in-flight callback on
	 * the compositor dispatch thread is protected by cmacs_gowl_mutex
	 * which the caller holds; by the time this returns, no new
	 * callbacks will fire. */
	if (self->frame_rendered_id != 0 && self->compositor != NULL) {
		g_signal_handler_disconnect(self->compositor,
		                            self->frame_rendered_id);
		self->frame_rendered_id = 0;
	}

	/* Close our copy of stdin — ideally this would cause ffmpeg to
	 * see EOF and finalize cleanly.  However, in an Emacs process
	 * that spawns other subprocesses (vterm, shell-mode, etc.),
	 * the pipe's write end can be inherited by those subprocesses
	 * via fork() before CLOEXEC fires, keeping the pipe alive from
	 * ffmpeg's POV even after our close().  Closing our copy is
	 * still correct housekeeping but not sufficient for termination. */
	if (self->ffmpeg_stdin_fd >= 0) {
		close(self->ffmpeg_stdin_fd);
		self->ffmpeg_stdin_fd = -1;
	}

	/* Send SIGINT first for a chance at graceful shutdown.  If
	 * ffmpeg doesn't exit in finalize(), we escalate to SIGTERM
	 * then SIGKILL.  The fragmented MP4 output format ensures the
	 * file is playable even if ffmpeg is killed abruptly. */
	if (self->ffmpeg_pid > 0) {
		if (kill(self->ffmpeg_pid, SIGINT) < 0 && errno != ESRCH) {
			g_warning("recording: kill(SIGINT) failed: %s",
			          g_strerror(errno));
		}
	}

	/* Transfer the pid to ffmpeg_pid_to_reap so the caller can
	 * wait for ffmpeg after releasing its mutex. */
	self->ffmpeg_pid_to_reap = self->ffmpeg_pid;
	self->ffmpeg_pid = 0;

	g_signal_emit(self, recording_signals[SIGNAL_RECORDING_STOPPED],
	              0, self->output_path);

	g_message("recording: stopped → %s", self->output_path);

	if (out_path != NULL)
		*out_path = g_strdup(self->output_path);

	return TRUE;
}

/* ----------------------------------------------------------------
 * GowlRecordingProvider interface
 * ---------------------------------------------------------------- */

static gboolean
recording_start(GowlRecordingProvider *provider,
                GowlCaptureMode        mode,
                const gchar           *output_name,
                gpointer               client,
                gint                   region_x,
                gint                   region_y,
                gint                   region_w,
                gint                   region_h,
                const gchar           *output_path,
                GError               **error)
{
	return do_start(GOWL_MODULE_RECORDING(provider),
	                mode, output_name, client,
	                region_x, region_y, region_w, region_h,
	                output_path, error);
}

static gboolean
recording_stop(GowlRecordingProvider  *provider,
               gchar                 **output_path,
               GError                **error)
{
	return do_stop(GOWL_MODULE_RECORDING(provider), output_path, error);
}

static gboolean
recording_is_recording(GowlRecordingProvider *provider)
{
	return GOWL_MODULE_RECORDING(provider)->recording;
}

static void
recording_finalize(GowlRecordingProvider *provider)
{
	GowlModuleRecording *self = GOWL_MODULE_RECORDING(provider);
	GPid pid;
	int status;
	pid_t r;
	gint64 deadline_us;

	/* MUST be called WITHOUT holding cmacs_gowl_mutex — this
	 * function blocks for up to ~6 seconds waiting for ffmpeg
	 * to finalize its output file.  Holding the compositor mutex
	 * during that time deadlocks the dispatch thread. */
	pid = self->ffmpeg_pid_to_reap;
	if (pid <= 0)
		return;
	self->ffmpeg_pid_to_reap = 0;

	/* Stage 1: SIGINT was already sent in do_stop.  Wait a short
	 * time for ffmpeg to exit gracefully.  With fragmented MP4
	 * output, the file is already playable, so we don't need to
	 * wait long. */
	deadline_us = g_get_monotonic_time() + 500 * 1000; /* 500ms */
	while (g_get_monotonic_time() < deadline_us) {
		r = waitpid(pid, &status, WNOHANG);
		if (r > 0)
			goto reaped;
		if (r < 0 && errno != EINTR) {
			g_warning("recording: waitpid failed: %s",
			          g_strerror(errno));
			goto reaped;
		}
		g_usleep(50 * 1000); /* 50ms */
	}

	/* Stage 2: ffmpeg still alive.  Send SIGTERM and wait 500ms. */
	kill(pid, SIGTERM);
	deadline_us = g_get_monotonic_time() + 500 * 1000;
	while (g_get_monotonic_time() < deadline_us) {
		r = waitpid(pid, &status, WNOHANG);
		if (r > 0)
			goto reaped;
		if (r < 0 && errno != EINTR) {
			g_warning("recording: waitpid failed: %s",
			          g_strerror(errno));
			goto reaped;
		}
		g_usleep(50 * 1000);
	}

	/* Stage 3: still alive.  SIGKILL and reap.  Safe because we're
	 * using fragmented MP4 (moov atom written at start). */
	kill(pid, SIGKILL);
	do {
		r = waitpid(pid, &status, 0);
	} while (r < 0 && errno == EINTR);

reaped:
	if (r > 0) {
		if (WIFEXITED(status)) {
			int ec = WEXITSTATUS(status);
			if (ec != 0)
				g_warning("recording: ffmpeg exited with %d",
				          ec);
		} else if (WIFSIGNALED(status)) {
			g_warning("recording: ffmpeg killed by signal %d",
			          WTERMSIG(status));
		}
	}

	g_spawn_close_pid(pid);
}

static void
recording_provider_init(GowlRecordingProviderInterface *iface)
{
	iface->start        = recording_start;
	iface->stop         = recording_stop;
	iface->is_recording = recording_is_recording;
	iface->finalize     = recording_finalize;
}

/* ----------------------------------------------------------------
 * GowlStartupHandler interface
 * ---------------------------------------------------------------- */

static void
recording_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleRecording *self = GOWL_MODULE_RECORDING(handler);

	self->compositor = GOWL_COMPOSITOR(compositor);
}

static void
recording_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = recording_on_startup;
}

/* ----------------------------------------------------------------
 * GowlShutdownHandler interface
 * ---------------------------------------------------------------- */

static void
recording_on_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	GowlModuleRecording *self = GOWL_MODULE_RECORDING(handler);

	(void)compositor;

	if (self->recording)
		do_stop(self, NULL, NULL);

	self->compositor = NULL;
}

static void
recording_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = recording_on_shutdown;
}

/* ----------------------------------------------------------------
 * GowlModule virtual methods
 * ---------------------------------------------------------------- */

static gboolean
recording_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
recording_deactivate(GowlModule *mod)
{
	GowlModuleRecording *self = GOWL_MODULE_RECORDING(mod);

	if (self->recording)
		do_stop(self, NULL, NULL);
}

static const gchar *
recording_get_name(GowlModule *mod)
{
	(void)mod;
	return "recording";
}

static const gchar *
recording_get_description(GowlModule *mod)
{
	(void)mod;
	return "Screen recording via ffmpeg subprocess";
}

static const gchar *
recording_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.1.0";
}

static void
recording_configure(GowlModule *mod, gpointer config)
{
	GowlModuleRecording *self = GOWL_MODULE_RECORDING(mod);
	GHashTable *settings;
	const gchar *val;

	if (config == NULL)
		return;

	settings = (GHashTable *)config;

	val = g_hash_table_lookup(settings, "save-directory");
	if (val != NULL) {
		g_free(self->save_directory);
		self->save_directory = g_strdup(val);
	}

	val = g_hash_table_lookup(settings, "filename-format");
	if (val != NULL) {
		g_free(self->filename_format);
		self->filename_format = g_strdup(val);
	}

	val = g_hash_table_lookup(settings, "framerate");
	if (val != NULL) {
		gint fps = atoi(val);
		if (fps > 0 && fps <= 120)
			self->framerate = fps;
	}

	val = g_hash_table_lookup(settings, "codec");
	if (val != NULL) {
		g_free(self->codec);
		self->codec = g_strdup(val);
	}

	val = g_hash_table_lookup(settings, "crf");
	if (val != NULL) {
		gint c = atoi(val);
		if (c >= 0 && c <= 51)
			self->crf = c;
	}

	val = g_hash_table_lookup(settings, "container");
	if (val != NULL) {
		g_free(self->container);
		self->container = g_strdup(val);
	}

	val = g_hash_table_lookup(settings, "ffmpeg-path");
	if (val != NULL) {
		g_free(self->ffmpeg_path);
		self->ffmpeg_path = g_strdup(val);
	}

	val = g_hash_table_lookup(settings, "pixel-format");
	if (val != NULL) {
		g_free(self->pixel_format);
		self->pixel_format = g_strdup(val);
	}
}

/* ----------------------------------------------------------------
 * GObject lifecycle
 * ---------------------------------------------------------------- */

static void
gowl_module_recording_finalize(GObject *object)
{
	GowlModuleRecording *self = GOWL_MODULE_RECORDING(object);

	if (self->recording)
		do_stop(self, NULL, NULL);

	g_free(self->save_directory);
	g_free(self->filename_format);
	g_free(self->codec);
	g_free(self->container);
	g_free(self->ffmpeg_path);
	g_free(self->pixel_format);
	g_free(self->output_name);
	g_free(self->output_path);

	G_OBJECT_CLASS(gowl_module_recording_parent_class)->finalize(object);
}

static void
gowl_module_recording_class_init(GowlModuleRecordingClass *klass)
{
	GObjectClass    *object_class = G_OBJECT_CLASS(klass);
	GowlModuleClass *module_class = GOWL_MODULE_CLASS(klass);

	object_class->finalize = gowl_module_recording_finalize;

	module_class->activate       = recording_activate;
	module_class->deactivate     = recording_deactivate;
	module_class->get_name       = recording_get_name;
	module_class->get_description = recording_get_description;
	module_class->get_version    = recording_get_version;
	module_class->configure      = recording_configure;

	/**
	 * GowlModuleRecording::recording-started:
	 * @self: the recording module
	 * @path: the output file path
	 *
	 * Emitted when recording begins.
	 */
	recording_signals[SIGNAL_RECORDING_STARTED] =
		g_signal_new("recording-started",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, G_TYPE_STRING);

	/**
	 * GowlModuleRecording::recording-stopped:
	 * @self: the recording module
	 * @path: the output file path
	 *
	 * Emitted when recording stops.
	 */
	recording_signals[SIGNAL_RECORDING_STOPPED] =
		g_signal_new("recording-stopped",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, G_TYPE_STRING);

	/**
	 * GowlModuleRecording::recording-error:
	 * @self: the recording module
	 * @message: error description
	 *
	 * Emitted when a recording error occurs.
	 */
	recording_signals[SIGNAL_RECORDING_ERROR] =
		g_signal_new("recording-error",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, G_TYPE_STRING);

	/**
	 * GowlModuleRecording::frame-dropped:
	 * @self: the recording module
	 *
	 * Emitted when a frame is dropped due to pipe back-pressure.
	 */
	recording_signals[SIGNAL_FRAME_DROPPED] =
		g_signal_new("frame-dropped",
		             G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL,
		             G_TYPE_NONE, 0);
}

static void
gowl_module_recording_init(GowlModuleRecording *self)
{
	self->compositor       = NULL;
	self->recording        = FALSE;
	self->ffmpeg_pid       = 0;
	self->ffmpeg_pid_to_reap = 0;
	self->ffmpeg_stdin_fd  = -1;
	self->frame_rendered_id = 0;
	self->last_frame_us    = 0;
	self->frame_interval_us = 0;
	self->output_name      = NULL;
	self->target_client    = NULL;
	self->output_path      = NULL;
	self->save_directory   = g_strdup("~/Videos/Recordings");
	self->filename_format  = g_strdup("recording_%Y%m%d_%H%M%S");
	self->framerate        = 30;
	self->codec            = g_strdup("libx264");
	self->crf              = 23;
	self->container        = g_strdup("mp4");
	self->ffmpeg_path      = g_strdup("ffmpeg");
	self->pixel_format     = g_strdup("yuv420p");
}

/* ----------------------------------------------------------------
 * Module entry point
 * ---------------------------------------------------------------- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_RECORDING;
}
