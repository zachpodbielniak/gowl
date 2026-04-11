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
	gint        ffmpeg_stdin_fd;
	struct wl_event_source *frame_timer;

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
 * Frame capture timer callback
 * ---------------------------------------------------------------- */

static int
frame_timer_callback(void *data)
{
	GowlModuleRecording *self = data;
	GBytes *frame = NULL;
	const guint8 *pixels;
	gsize size;
	ssize_t written;
	gint w, h;

	if (!self->recording)
		return 0;

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
		goto rearm;

	pixels = g_bytes_get_data(frame, &size);

	/* Non-blocking write to ffmpeg stdin */
	written = write(self->ffmpeg_stdin_fd, pixels, size);
	if (written < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			g_signal_emit(self,
			              recording_signals[SIGNAL_FRAME_DROPPED],
			              0);
		} else {
			g_warning("recording: write error: %s",
			          g_strerror(errno));
		}
	}

	g_bytes_unref(frame);

rearm:
	/* Re-arm the timer */
	if (self->recording && self->frame_timer != NULL) {
		wl_event_source_timer_update(self->frame_timer,
		                             1000 / self->framerate);
	}

	return 0;
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
	struct wl_event_loop *loop;
	GBytes *probe;
	gint pw, ph;
	gchar size_str[32];
	gchar fps_str[16];
	gchar crf_str[16];
	gint stdin_fd;
	gchar *argv[20];
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
	argv[argc++] = "-crf";
	argv[argc++] = crf_str;
	argv[argc++] = "-pix_fmt";
	argv[argc++] = self->pixel_format;
	argv[argc++] = self->output_path;
	argv[argc] = NULL;

	/* Spawn ffmpeg */
	ok = g_spawn_async_with_pipes(
	         NULL, argv, NULL,
	         G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD
	             | G_SPAWN_STDOUT_TO_DEV_NULL
	             | G_SPAWN_STDERR_TO_DEV_NULL,
	         NULL, NULL,
	         &self->ffmpeg_pid,
	         &stdin_fd,
	         NULL, NULL,
	         error);

	if (!ok)
		return FALSE;

	/* Set non-blocking on the pipe */
	{
		gint flags = fcntl(stdin_fd, F_GETFL, 0);
		if (flags >= 0)
			fcntl(stdin_fd, F_SETFL, flags | O_NONBLOCK);
	}

	self->ffmpeg_stdin_fd = stdin_fd;
	self->recording = TRUE;
	self->start_time_us = g_get_real_time();

	/* Start frame capture timer */
	loop = gowl_compositor_get_event_loop(self->compositor);
	self->frame_timer = wl_event_loop_add_timer(loop,
	                        frame_timer_callback, self);
	wl_event_source_timer_update(self->frame_timer,
	                             1000 / self->framerate);

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
	if (!self->recording) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
		                    "No recording in progress");
		return FALSE;
	}

	self->recording = FALSE;

	/* Remove frame timer */
	if (self->frame_timer != NULL) {
		wl_event_source_remove(self->frame_timer);
		self->frame_timer = NULL;
	}

	/* Close stdin pipe — causes ffmpeg to finalize the file */
	if (self->ffmpeg_stdin_fd >= 0) {
		close(self->ffmpeg_stdin_fd);
		self->ffmpeg_stdin_fd = -1;
	}

	/* Reap the child process */
	if (self->ffmpeg_pid > 0) {
		g_spawn_close_pid(self->ffmpeg_pid);
		self->ffmpeg_pid = 0;
	}

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
recording_provider_init(GowlRecordingProviderInterface *iface)
{
	iface->start        = recording_start;
	iface->stop         = recording_stop;
	iface->is_recording = recording_is_recording;
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
	self->ffmpeg_stdin_fd  = -1;
	self->frame_timer      = NULL;
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
