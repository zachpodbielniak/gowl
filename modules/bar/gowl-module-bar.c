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

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-bar"

#include <glib-object.h>
#include <gmodule.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/statvfs.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <unistd.h>
#include <dirent.h>

#include <cairo.h>
#include <pango/pangocairo.h>

#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

#include "gowl-enums.h"
#include "module/gowl-module.h"
#include "interfaces/gowl-bar-provider.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"
#include "core/gowl-bar.h"
#include "config/gowl-config.h"
#include "boxed/gowl-process-info.h"

/* ----------------------------------------------------------------
 * Custom wlr_buffer for bar pixel data
 * ---------------------------------------------------------------- */

typedef struct {
	struct wlr_buffer base;
	guchar *pixels;
	gsize   size;
	gint    stride;
} BarBuffer;

static void
bar_buffer_destroy(struct wlr_buffer *wlr_buf)
{
	BarBuffer *buf = wl_container_of(wlr_buf, buf, base);
	g_free(buf->pixels);
	g_free(buf);
}

static bool
bar_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buf,
                                 uint32_t flags,
                                 void **data,
                                 uint32_t *format,
                                 size_t *stride)
{
	BarBuffer *buf = wl_container_of(wlr_buf, buf, base);
	*data   = buf->pixels;
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)buf->stride;
	return true;
}

static void
bar_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buf)
{
	(void)wlr_buf;
}

static const struct wlr_buffer_impl bar_buffer_impl = {
	.destroy = bar_buffer_destroy,
	.begin_data_ptr_access = bar_buffer_begin_data_ptr_access,
	.end_data_ptr_access = bar_buffer_end_data_ptr_access,
};

/* ----------------------------------------------------------------
 * Hex color parsing
 * ---------------------------------------------------------------- */

static void
parse_hex_color(const gchar *hex, gdouble rgba[4])
{
	guint r, g, b, a;

	a = 255;
	if (hex == NULL || hex[0] != '#') {
		rgba[0] = rgba[1] = rgba[2] = 0.0;
		rgba[3] = 1.0;
		return;
	}

	if (strlen(hex) == 9) {
		sscanf(hex, "#%2x%2x%2x%2x", &r, &g, &b, &a);
	} else if (strlen(hex) == 7) {
		sscanf(hex, "#%2x%2x%2x", &r, &g, &b);
	} else {
		rgba[0] = rgba[1] = rgba[2] = 0.0;
		rgba[3] = 1.0;
		return;
	}

	rgba[0] = (gdouble)r / 255.0;
	rgba[1] = (gdouble)g / 255.0;
	rgba[2] = (gdouble)b / 255.0;
	rgba[3] = (gdouble)a / 255.0;
}

/* ----------------------------------------------------------------
 * Widget types
 * ---------------------------------------------------------------- */

typedef enum {
	BAR_WIDGET_CPU,
	BAR_WIDGET_MEMORY,
	BAR_WIDGET_DISK,
	BAR_WIDGET_BATTERY,
	BAR_WIDGET_CLOCK,
	BAR_WIDGET_LOAD,
	BAR_WIDGET_CMD,
	BAR_WIDGET_NET,
	BAR_WIDGET_UPTIME,
	BAR_WIDGET_HOST,
	BAR_WIDGET_TEMP,
	BAR_WIDGET_GPU,
	BAR_WIDGET_SWAP,
	BAR_WIDGET_IO,
	BAR_WIDGET_VOLUME,
	BAR_WIDGET_MEDIA,
	BAR_WIDGET_WIFI,
	BAR_WIDGET_IP,
	BAR_WIDGET_VPN,
	BAR_WIDGET_GIT,
	BAR_WIDGET_TODO,
	BAR_WIDGET_PODMAN,
	BAR_WIDGET_USER,
	BAR_WIDGET_KEYMAP,
	BAR_WIDGET_WEATHER,
	BAR_WIDGET_COUNT
} BarWidgetType;

#define BAR_MAX_WIDGETS 32

typedef struct {
	BarWidgetType type;
	gdouble       color[4];  /* (0,0,0,0) = use fg_color */
	gboolean      has_color;
	gchar        *param;     /* type-specific: disk mount, net iface, cmd string */
	/* Per-widget output cache (for subprocess/IPC widgets).
	   cached_output is written by worker threads and read by the
	   dispatch thread's render; guarded by bar->output_mutex. */
	gchar        *cached_output;
	time_t        output_read_time;
	gint          output_interval;  /* seconds, default varies by type */
	gboolean      interval_explicit; /* TRUE if set via per-widget @N syntax */
	/* Set to 1 while a worker thread is running a subprocess for this
	   widget; gated atomically to prevent duplicate spawns. */
	volatile gint spawn_in_flight;
} BarWidget;

/* ----------------------------------------------------------------
 * Module type
 * ---------------------------------------------------------------- */

#define GOWL_TYPE_MODULE_BAR (gowl_module_bar_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleBar, gowl_module_bar,
                     GOWL, MODULE_BAR, GowlModule)

/* Position of a bar instance on the monitor.  TOP sits flush with
   the monitor's top edge, BOTTOM sits flush with the bottom.  Two
   instances per module is the intentional limit -- stacking more
   bars on one edge doesn't fit the screen budget of a typical
   tiling WM. */
typedef enum {
	GOWL_BAR_POSITION_TOP = 0,
	GOWL_BAR_POSITION_BOTTOM = 1,
	GOWL_BAR_POSITION_COUNT
} GowlBarPosition;

typedef struct {
	struct wlr_scene_buffer *scene_buf;
	gint width;
	gint height;
	gint mon_x;
	gint mon_y;
	/* Signature of the last rendered content (title + widget texts +
	   colors + geometry). bar_redraw_all compares the freshly-computed
	   signature against this one and skips bar_render +
	   wlr_scene_buffer_set_buffer when nothing that affects the visual
	   output has changed -- avoiding a cairo surface allocation, a
	   Pango layout pass, a memcpy, and a scene-graph damage event
	   (which would force a full compositor re-render at vblank). */
	gchar *last_signature;
} BarSurface;

/* Per-bar instance state.  A module owns two of these: one for
   the top slot, one for the bottom. */
typedef struct {
	GowlBarPosition position;
	gboolean        enabled;   /* has been configured OR explicitly shown */
	gboolean        visible;   /* hide/show toggle, independent of enabled */

	/* Config */
	gint     bar_height;
	gdouble  bg_color[4];
	gdouble  fg_color[4];
	gchar   *font_desc;

	/* Title colorization (per slot -- top bar typically carries the
	   focused-window title, but a bottom bar can have its own) */
	gchar   *custom_title;
	gchar   *title_delimiters;            /* chars that split segments */
	gdouble  title_delimiter_color[4];    /* color for delimiter chars */
	gdouble  title_palette[8][4];         /* segment color cycle */
	gint     title_palette_size;          /* 0 = disabled, use fg_color */

	/* dwm-style tag indicator (left edge, before the title).  States:
	   active = selected on this output, urgent = has urgent client,
	   occupied = has clients, empty = none. */
	gboolean show_tags;                   /* draw the tag row */
	gdouble  tag_active_bg[4];            /* selected tag fill */
	gdouble  tag_active_fg[4];            /* selected tag number */
	gdouble  tag_occupied_fg[4];          /* occupied, non-selected */
	gdouble  tag_urgent_bg[4];            /* urgent tag fill */
	gdouble  tag_urgent_fg[4];            /* urgent tag number */
	gdouble  tag_empty_fg[4];             /* empty tag number */

	/* Widgets */
	BarWidget widgets[BAR_MAX_WIDGETS];
	gint      n_widgets;

	/* One BarSurface per monitor (key = monitor name, owned by hash) */
	GHashTable *surfaces;
} GowlBarInstance;

struct _GowlModuleBar {
	GowlModule parent_instance;

	/* Per-slot state */
	GowlBarInstance bars[GOWL_BAR_POSITION_COUNT];

	/* Shared compositor state */
	gpointer    compositor;
	gulong      focus_handler_id;
	gulong      client_added_id;
	gulong      client_removed_id;
	struct wl_event_source *tick_timer;

	/* Cached system data (read once per tick, shared by both bars) */
	glong   prev_cpu_idle;
	glong   prev_cpu_total;
	gint    cached_cpu_pct;
	time_t  cpu_read_time;
	glong   cached_mem_used_mb;
	glong   cached_mem_total_mb;
	time_t  mem_read_time;
	glong   cached_disk_free_gb;
	glong   cached_disk_total_gb;
	time_t  disk_read_time;
	gint    cached_battery;
	time_t  bat_read_time;

	/* Load */
	gdouble cached_load_1;
	time_t  load_read_time;

	/* Swap (read alongside memory) */
	glong   cached_swap_used_mb;
	glong   cached_swap_total_mb;

	/* Net */
	glong   prev_net_rx;
	glong   prev_net_tx;
	glong   cached_net_rx_rate;
	glong   cached_net_tx_rate;
	time_t  net_read_time;

	/* IO */
	glong   prev_io_rd;
	glong   prev_io_wr;
	glong   cached_io_rd_rate;
	glong   cached_io_wr_rate;
	time_t  io_read_time;

	/* Temp */
	gint    cached_temp_mc;        /* millidegrees C */
	time_t  temp_read_time;
	gchar  *temp_path;             /* auto-detected hwmon path */

	/* Static (read once) */
	gchar  *cached_hostname;
	gchar  *cached_username;

	/* Keymap */
	gchar  *cached_keymap;
	time_t  keymap_read_time;

	/* Widget data (Elisp-driven values) -- shared across bars so
	   widgets like "todo" see the same values wherever they're
	   placed. */
	GHashTable *widget_data;

	/* Async subprocess dispatch.  Subprocess-based widget reads run
	   on this pool so they never block the compositor dispatch
	   thread (which holds cmacs_gowl_mutex during wl_event_loop
	   iteration -- blocking there deadlocks every main-thread DEFUN
	   that needs the mutex). */
	GThreadPool *worker_pool;
	GMutex       output_mutex; /* protects BarWidget::cached_output */
};

/* Resolve a #GowlBarPosition to its slot pointer.  NULL on invalid
   position. */
static inline GowlBarInstance *
bar_slot(GowlModuleBar *self, GowlBarPosition pos)
{
	if ((guint)pos >= GOWL_BAR_POSITION_COUNT)
		return NULL;
	return &self->bars[pos];
}

/* Parse a "position" string from config into a #GowlBarPosition.
   Accepts "top" / "bottom" (case-insensitive).  Returns the default
   (TOP) if @s is NULL or unrecognised, so existing single-bar
   configs keep working. */
static GowlBarPosition
bar_position_from_string(const gchar *s)
{
	if (s == NULL)
		return GOWL_BAR_POSITION_TOP;
	if (g_ascii_strcasecmp(s, "bottom") == 0)
		return GOWL_BAR_POSITION_BOTTOM;
	return GOWL_BAR_POSITION_TOP;
}

static void bar_provider_iface_init(GowlBarProviderInterface *iface);
static void bar_startup_init(GowlStartupHandlerInterface *iface);
static void bar_shutdown_init(GowlShutdownHandlerInterface *iface);

/* ----------------------------------------------------------------
 * Async subprocess worker infrastructure
 *
 * Subprocess-based widgets (cmd, volume, media, git, podman, weather)
 * used to call g_spawn_sync() directly from bar_tick on the compositor
 * dispatch thread.  That thread holds cmacs_gowl_mutex for the entire
 * wl_event_loop iteration, so any slow subprocess would block every
 * main-thread DEFUN that needs the mutex (deadlocking the editor).
 *
 * We now push those reads to a GThreadPool.  Workers run the spawn,
 * parse the output, and swap in a new cached_output under
 * bar->output_mutex.  The dispatch thread never blocks on I/O.
 * ---------------------------------------------------------------- */

typedef enum {
	WORK_KIND_CMD,
	WORK_KIND_VOLUME,
	WORK_KIND_MEDIA,
	WORK_KIND_GIT,
	WORK_KIND_PODMAN,
	WORK_KIND_WEATHER,
} WorkKind;

typedef struct {
	GowlModuleBar *bar;       /* borrowed; outlives work items */
	BarWidget     *widget;    /* borrowed; spawn_in_flight pins lifetime */
	WorkKind       kind;
	gchar         *param;     /* copy -- worker must not touch widget->param */
	gchar         *git_cwd;   /* for WORK_KIND_GIT only */
} WorkItem;

/* Store a freshly-built cached_output string. Takes ownership of
   new_output (even if NULL).  Safe to call from any thread. */
static void
bar_widget_store_output(GowlModuleBar *self, BarWidget *w, gchar *new_output)
{
	g_mutex_lock(&self->output_mutex);
	g_free(w->cached_output);
	w->cached_output = new_output;
	g_mutex_unlock(&self->output_mutex);
}

/* Run an argv-style command synchronously and return the first line
   of stdout, trimmed. Returns NULL on failure or empty output. */
static gchar *
run_command_first_line(const gchar * const *argv)
{
	gchar *out = NULL;
	gchar *nl;

	if (!g_spawn_sync(NULL, (gchar **)argv, NULL,
	                  G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
	                  NULL, NULL, &out, NULL, NULL, NULL)) {
		g_free(out);
		return NULL;
	}
	if (out == NULL)
		return NULL;

	nl = strchr(out, '\n');
	if (nl != NULL)
		*nl = '\0';
	g_strchomp(out);
	if (out[0] == '\0') {
		g_free(out);
		return NULL;
	}
	return out;
}

/* Argument to g_strsplit a shell-parsed command, honoring tilde.
   Callers must g_strfreev() the result. Returns NULL on parse
   failure. */
static gchar **
parse_shell_argv(const gchar *cmdline)
{
	gchar      **argv = NULL;
	gint         argc = 0;
	const gchar *home;
	gint         i;

	if (!g_shell_parse_argv(cmdline, &argc, &argv, NULL))
		return NULL;

	home = g_get_home_dir();
	if (home != NULL) {
		for (i = 0; i < argc; i++) {
			if (argv[i][0] == '~' && argv[i][1] == '/') {
				gchar *expanded =
					g_strdup_printf("%s%s", home, argv[i] + 1);
				g_free(argv[i]);
				argv[i] = expanded;
			}
		}
	}
	return argv;
}

/* Worker body: runs on a GThreadPool thread, never holds any
   compositor mutex.  Must only touch: self->output_mutex and the
   widget's cached_output / spawn_in_flight fields. */
static void
bar_worker_func(gpointer data, gpointer user_data)
{
	WorkItem      *item = data;
	GowlModuleBar *self = item->bar;
	BarWidget     *w    = item->widget;
	gchar         *new_output = NULL;
	gchar         *out = NULL;

	(void)user_data;

	switch (item->kind) {
	case WORK_KIND_CMD:
		{
			gchar **argv = parse_shell_argv(item->param);
			if (argv != NULL) {
				new_output =
					run_command_first_line((const gchar * const *)argv);
				g_strfreev(argv);
			}
		}
		break;

	case WORK_KIND_VOLUME:
		{
			const gchar *argv[] = {
				"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL
			};
			if (g_spawn_sync(NULL, (gchar **)argv, NULL,
			        G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
			        NULL, NULL, &out, NULL, NULL, NULL) && out != NULL) {
				gdouble vol;
				gboolean muted = (strstr(out, "[MUTED]") != NULL);
				if (muted) {
					new_output = g_strdup("VOL MUTE");
				} else if (sscanf(out, "Volume: %lf", &vol) == 1) {
					new_output = g_strdup_printf("VOL %d%%",
					    (gint)(vol * 100));
				} else {
					new_output = g_strdup("VOL ?");
				}
			}
			g_free(out);
		}
		break;

	case WORK_KIND_MEDIA:
		{
			const gchar *argv[] = {
				"playerctl", "metadata", "--format",
				"{{artist}} - {{title}}", NULL
			};
			if (g_spawn_sync(NULL, (gchar **)argv, NULL,
			        G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
			        NULL, NULL, &out, NULL, NULL, NULL) && out != NULL) {
				g_strstrip(out);
				if (out[0] != '\0' && strcmp(out, " - ") != 0) {
					if (strlen(out) > 40)
						out[40] = '\0';
					new_output = g_strdup_printf("\xe2\x99\xab %s", out);
				}
			}
			g_free(out);
		}
		break;

	case WORK_KIND_PODMAN:
		{
			const gchar *argv[] = { "podman", "ps", "-q", NULL };
			if (g_spawn_sync(NULL, (gchar **)argv, NULL,
			        G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
			        NULL, NULL, &out, NULL, NULL, NULL) && out != NULL) {
				gint count;
				gchar **lines;

				g_strstrip(out);
				if (out[0] == '\0') {
					count = 0;
				} else {
					lines = g_strsplit(out, "\n", -1);
					count = (gint)g_strv_length(lines);
					g_strfreev(lines);
				}
				new_output = g_strdup_printf("POD %d", count);
			}
			g_free(out);
		}
		break;

	case WORK_KIND_WEATHER:
		{
			const gchar *loc = (item->param != NULL) ? item->param : "";
			gchar *url = g_strdup_printf("wttr.in/%s?format=%%c+%%t", loc);
			const gchar *argv[] = { "curl", "-sf", url, NULL };
			if (g_spawn_sync(NULL, (gchar **)argv, NULL,
			        G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
			        NULL, NULL, &out, NULL, NULL, NULL) && out != NULL) {
				g_strstrip(out);
				if (out[0] != '\0')
					new_output = g_strdup(out);
			}
			g_free(out);
			g_free(url);
		}
		break;

	case WORK_KIND_GIT:
		if (item->git_cwd != NULL) {
			gchar  head_path[PATH_MAX];
			gchar  dir[PATH_MAX];
			FILE  *f;
			char   ref_line[512];
			gchar *branch;
			gint   dirty = 0;

			g_strlcpy(dir, item->git_cwd, sizeof(dir));
			/* Walk up to find .git/HEAD */
			while (dir[0] != '\0') {
				g_snprintf(head_path, sizeof(head_path),
				           "%s/.git/HEAD", dir);
				if (g_file_test(head_path, G_FILE_TEST_EXISTS))
					break;
				{
					gchar *slash = strrchr(dir, '/');
					if (slash == NULL || slash == dir) {
						dir[0] = '\0';
						break;
					}
					*slash = '\0';
				}
			}

			if (dir[0] != '\0' &&
			    (f = fopen(head_path, "r")) != NULL) {
				if (fgets(ref_line, sizeof(ref_line), f) != NULL) {
					g_strstrip(ref_line);
					if (strncmp(ref_line, "ref: refs/heads/", 16) == 0)
						branch = ref_line + 16;
					else
						branch = ref_line;

					{
						const gchar *argv[] = {
							"git", "-C", dir, "status", "--porcelain", NULL
						};
						gchar *status_out = NULL;
						if (g_spawn_sync(NULL, (gchar **)argv, NULL,
						        G_SPAWN_SEARCH_PATH
						        | G_SPAWN_STDERR_TO_DEV_NULL,
						        NULL, NULL, &status_out, NULL, NULL, NULL)
						    && status_out != NULL) {
							gchar *p;
							for (p = status_out; *p != '\0'; p++)
								if (*p == '\n')
									dirty++;
						}
						g_free(status_out);
					}

					if (dirty > 0)
						new_output =
							g_strdup_printf("%s*%d", branch, dirty);
					else
						new_output = g_strdup(branch);
				}
				fclose(f);
			}
		}
		break;
	}

	bar_widget_store_output(self, w, new_output);

	/* Release the lifetime pin so the widget can be freed and so the
	   next tick can schedule another read. */
	g_atomic_int_set(&w->spawn_in_flight, 0);

	g_free(item->param);
	g_free(item->git_cwd);
	g_free(item);
}

/* Schedule a worker if one isn't already running for this widget.
   Called from the dispatch thread. Never blocks. */
static void
bar_schedule_work(GowlModuleBar *self, BarWidget *w, WorkKind kind,
                  const gchar *param, const gchar *git_cwd)
{
	WorkItem *item;

	if (self->worker_pool == NULL)
		return;
	if (!g_atomic_int_compare_and_exchange(&w->spawn_in_flight, 0, 1))
		return; /* already running */

	item = g_new0(WorkItem, 1);
	item->bar     = self;
	item->widget  = w;
	item->kind    = kind;
	item->param   = (param   != NULL) ? g_strdup(param)   : NULL;
	item->git_cwd = (git_cwd != NULL) ? g_strdup(git_cwd) : NULL;

	if (!g_thread_pool_push(self->worker_pool, item, NULL)) {
		g_atomic_int_set(&w->spawn_in_flight, 0);
		g_free(item->param);
		g_free(item->git_cwd);
		g_free(item);
	}
}

G_DEFINE_TYPE_WITH_CODE(GowlModuleBar, gowl_module_bar,
	GOWL_TYPE_MODULE,
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_BAR_PROVIDER,
		bar_provider_iface_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_STARTUP_HANDLER,
		bar_startup_init)
	G_IMPLEMENT_INTERFACE(GOWL_TYPE_SHUTDOWN_HANDLER,
		bar_shutdown_init))

/* ----------------------------------------------------------------
 * System data readers
 * ---------------------------------------------------------------- */

static void
read_cpu(GowlModuleBar *self)
{
	FILE *f;
	glong user, nice, sys, idle, iowait, irq, softirq, steal;
	glong total, diff_idle, diff_total;
	time_t now;

	now = time(NULL);
	if (now - self->cpu_read_time < 2)
		return;
	self->cpu_read_time = now;

	f = fopen("/proc/stat", "r");
	if (f == NULL)
		return;

	if (fscanf(f, "cpu %ld %ld %ld %ld %ld %ld %ld %ld",
	           &user, &nice, &sys, &idle, &iowait,
	           &irq, &softirq, &steal) != 8) {
		fclose(f);
		return;
	}
	fclose(f);

	total = user + nice + sys + idle + iowait + irq + softirq + steal;
	diff_idle  = (idle + iowait) - self->prev_cpu_idle;
	diff_total = total - self->prev_cpu_total;

	if (diff_total > 0)
		self->cached_cpu_pct = (gint)(100 * (diff_total - diff_idle) / diff_total);
	else
		self->cached_cpu_pct = 0;

	self->prev_cpu_idle  = idle + iowait;
	self->prev_cpu_total = total;
}

static void
read_memory(GowlModuleBar *self)
{
	FILE *f;
	char line[256];
	glong total_kb, avail_kb, used_kb;
	time_t now;

	now = time(NULL);
	if (now - self->mem_read_time < 5)
		return;
	self->mem_read_time = now;

	total_kb = 0;
	avail_kb = 0;

	f = fopen("/proc/meminfo", "r");
	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, "MemTotal:", 9) == 0)
			sscanf(line + 9, " %ld", &total_kb);
		else if (strncmp(line, "MemAvailable:", 13) == 0)
			sscanf(line + 13, " %ld", &avail_kb);
	}
	fclose(f);

	used_kb = total_kb - avail_kb;
	self->cached_mem_used_mb  = used_kb / 1024;
	self->cached_mem_total_mb = total_kb / 1024;
}

static void
read_disk(GowlModuleBar *self)
{
	struct statvfs st;
	time_t now;

	now = time(NULL);
	if (now - self->disk_read_time < 30)
		return;
	self->disk_read_time = now;

	if (statvfs("/", &st) != 0)
		return;

	self->cached_disk_free_gb  = (glong)((st.f_bavail * st.f_frsize) /
	                             (1024UL * 1024UL * 1024UL));
	self->cached_disk_total_gb = (glong)((st.f_blocks * st.f_frsize) /
	                             (1024UL * 1024UL * 1024UL));
}

static void
read_battery(GowlModuleBar *self)
{
	FILE *f;
	gint pct;
	time_t now;

	now = time(NULL);
	if (now - self->bat_read_time < 60)
		return;
	self->bat_read_time = now;

	f = fopen("/sys/class/power_supply/BAT0/capacity", "r");
	if (f == NULL)
		f = fopen("/sys/class/power_supply/BAT1/capacity", "r");
	if (f == NULL) {
		self->cached_battery = -1;
		return;
	}

	if (fscanf(f, "%d", &pct) != 1)
		pct = -1;
	fclose(f);
	self->cached_battery = pct;
}

/* --- Auto-scale helper --- */

static void
format_rate(glong bytes_per_sec, char *buf, gsize bufsz)
{
	if (bytes_per_sec >= 1024L * 1024L * 1024L)
		snprintf(buf, bufsz, "%.1fG",
		         (gdouble)bytes_per_sec / (1024.0 * 1024.0 * 1024.0));
	else if (bytes_per_sec >= 1024L * 1024L)
		snprintf(buf, bufsz, "%.1fM",
		         (gdouble)bytes_per_sec / (1024.0 * 1024.0));
	else if (bytes_per_sec >= 1024L)
		snprintf(buf, bufsz, "%.1fK",
		         (gdouble)bytes_per_sec / 1024.0);
	else
		snprintf(buf, bufsz, "%ldB", bytes_per_sec);
}

/* --- New data readers --- */

static void
read_load(GowlModuleBar *self)
{
	FILE *f;
	time_t now;

	now = time(NULL);
	if (now - self->load_read_time < 5)
		return;
	self->load_read_time = now;

	f = fopen("/proc/loadavg", "r");
	if (f == NULL)
		return;

	if (fscanf(f, "%lf", &self->cached_load_1) != 1)
		self->cached_load_1 = 0.0;
	fclose(f);
}

static void
read_swap(GowlModuleBar *self)
{
	/* Swap is read alongside memory from /proc/meminfo.
	 * Extend read_memory to also parse SwapTotal/SwapFree. */
	FILE *f;
	char line[256];
	glong swap_total_kb, swap_free_kb;
	time_t now;

	now = time(NULL);
	if (now - self->mem_read_time < 5)
		return;
	/* Don't update mem_read_time here — read_memory handles it */

	swap_total_kb = 0;
	swap_free_kb  = 0;

	f = fopen("/proc/meminfo", "r");
	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, "SwapTotal:", 10) == 0)
			sscanf(line + 10, " %ld", &swap_total_kb);
		else if (strncmp(line, "SwapFree:", 9) == 0)
			sscanf(line + 9, " %ld", &swap_free_kb);
	}
	fclose(f);

	self->cached_swap_total_mb = swap_total_kb / 1024;
	self->cached_swap_used_mb  = (swap_total_kb - swap_free_kb) / 1024;
}

static void
read_net(GowlModuleBar *self, BarWidget *w)
{
	FILE *f;
	char line[512];
	glong rx, tx, diff_rx, diff_tx;
	time_t now;
	const gchar *iface;

	now = time(NULL);
	if (now - self->net_read_time < 2)
		return;

	iface = (w->param != NULL) ? w->param : NULL;
	rx = tx = 0;

	f = fopen("/proc/net/dev", "r");
	if (f == NULL)
		return;

	/* Skip header lines */
	if (fgets(line, sizeof(line), f) == NULL) { fclose(f); return; }
	if (fgets(line, sizeof(line), f) == NULL) { fclose(f); return; }

	while (fgets(line, sizeof(line), f) != NULL) {
		char name[64];
		glong r, t;

		if (sscanf(line, " %63[^:]:%ld %*d %*d %*d %*d %*d %*d %*d %ld",
		           name, &r, &t) == 3) {
			/* If specific interface requested, match it */
			if (iface != NULL) {
				if (strcmp(name, iface) == 0) {
					rx = r; tx = t;
					break;
				}
			} else {
				/* Auto: first non-lo with traffic */
				if (strcmp(name, "lo") != 0 && r > 0) {
					rx = r; tx = t;
					break;
				}
			}
		}
	}
	fclose(f);

	if (self->net_read_time > 0) {
		time_t dt = now - self->net_read_time;
		if (dt <= 0) dt = 1;
		diff_rx = rx - self->prev_net_rx;
		diff_tx = tx - self->prev_net_tx;
		if (diff_rx < 0) diff_rx = 0;
		if (diff_tx < 0) diff_tx = 0;
		self->cached_net_rx_rate = diff_rx / dt;
		self->cached_net_tx_rate = diff_tx / dt;
	}

	self->prev_net_rx = rx;
	self->prev_net_tx = tx;
	self->net_read_time = now;
}

static void
read_io(GowlModuleBar *self, BarWidget *w)
{
	FILE *f;
	char line[512];
	glong rd_sect, wr_sect, diff_rd, diff_wr;
	time_t now;
	const gchar *device;

	now = time(NULL);
	if (now - self->io_read_time < 2)
		return;

	device = (w->param != NULL) ? w->param : NULL;
	rd_sect = wr_sect = 0;

	f = fopen("/proc/diskstats", "r");
	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f) != NULL) {
		char name[64];
		glong rd, wr;

		/* Fields: major minor name rd_ios rd_merges rd_sect rd_ticks
		 *         wr_ios wr_merges wr_sect ... */
		if (sscanf(line, " %*d %*d %63s %*d %*d %ld %*d %*d %*d %ld",
		           name, &rd, &wr) == 3) {
			if (device != NULL) {
				if (strcmp(name, device) == 0) {
					rd_sect = rd; wr_sect = wr;
					break;
				}
			} else {
				/* Auto: find sda, nvme0n1, or vda */
				if (strncmp(name, "sd", 2) == 0 && strlen(name) == 3) {
					rd_sect = rd; wr_sect = wr;
					break;
				}
				if (strncmp(name, "nvme", 4) == 0 &&
				    strchr(name, 'p') == NULL) {
					rd_sect = rd; wr_sect = wr;
					break;
				}
				if (strncmp(name, "vd", 2) == 0 && strlen(name) == 3) {
					rd_sect = rd; wr_sect = wr;
					break;
				}
			}
		}
	}
	fclose(f);

	if (self->io_read_time > 0) {
		time_t dt = now - self->io_read_time;
		if (dt <= 0) dt = 1;
		diff_rd = rd_sect - self->prev_io_rd;
		diff_wr = wr_sect - self->prev_io_wr;
		if (diff_rd < 0) diff_rd = 0;
		if (diff_wr < 0) diff_wr = 0;
		/* sectors are 512 bytes */
		self->cached_io_rd_rate = (diff_rd * 512) / dt;
		self->cached_io_wr_rate = (diff_wr * 512) / dt;
	}

	self->prev_io_rd = rd_sect;
	self->prev_io_wr = wr_sect;
	self->io_read_time = now;
}

static void
read_temp(GowlModuleBar *self)
{
	FILE *f;
	gint mc;
	time_t now;

	now = time(NULL);
	if (now - self->temp_read_time < 10)
		return;
	self->temp_read_time = now;

	/* Auto-detect temp path on first call */
	if (self->temp_path == NULL) {
		const gchar *paths[] = {
			"/sys/class/hwmon/hwmon0/temp1_input",
			"/sys/class/hwmon/hwmon1/temp1_input",
			"/sys/class/hwmon/hwmon2/temp1_input",
			"/sys/class/thermal/thermal_zone0/temp",
			NULL
		};
		gint i;
		for (i = 0; paths[i] != NULL; i++) {
			if (g_file_test(paths[i], G_FILE_TEST_EXISTS)) {
				self->temp_path = g_strdup(paths[i]);
				break;
			}
		}
		if (self->temp_path == NULL)
			return;
	}

	f = fopen(self->temp_path, "r");
	if (f == NULL)
		return;

	if (fscanf(f, "%d", &mc) == 1)
		self->cached_temp_mc = mc;
	fclose(f);
}

static void
read_gpu(GowlModuleBar *self, BarWidget *w)
{
	FILE *f;
	gint pct;

	/* AMD amdgpu sysfs */
	f = fopen("/sys/class/drm/card0/device/gpu_busy_percent", "r");
	if (f == NULL)
		f = fopen("/sys/class/drm/card1/device/gpu_busy_percent", "r");
	if (f == NULL) {
		bar_widget_store_output(self, w, NULL);
		return;
	}

	if (fscanf(f, "%d", &pct) == 1)
		bar_widget_store_output(self, w,
		    g_strdup_printf("GPU %d%%", pct));
	fclose(f);
}

static void
read_wifi(GowlModuleBar *self, BarWidget *w)
{
	FILE *f;
	char line[512];

	f = fopen("/proc/net/wireless", "r");
	if (f == NULL)
		return;

	/* Skip 2 header lines */
	if (fgets(line, sizeof(line), f) == NULL) { fclose(f); return; }
	if (fgets(line, sizeof(line), f) == NULL) { fclose(f); return; }

	if (fgets(line, sizeof(line), f) != NULL) {
		char iface[64];
		gdouble link, level;

		if (sscanf(line, " %63[^:]: %*d %lf %lf",
		           iface, &link, &level) >= 3) {
			bar_widget_store_output(self, w,
			    g_strdup_printf("WiFi %ddBm", (gint)level));
		}
	}
	fclose(f);
}

static void
read_ip(GowlModuleBar *self, BarWidget *w)
{
	struct ifaddrs *ifaddr, *ifa;
	const gchar *target_iface;

	target_iface = (w->param != NULL) ? w->param : NULL;

	if (getifaddrs(&ifaddr) != 0)
		return;

	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		char host[INET_ADDRSTRLEN];

		if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET)
			continue;

		if (target_iface != NULL) {
			if (strcmp(ifa->ifa_name, target_iface) != 0)
				continue;
		} else {
			if (strcmp(ifa->ifa_name, "lo") == 0)
				continue;
		}

		inet_ntop(AF_INET,
		          &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
		          host, sizeof(host));
		bar_widget_store_output(self, w, g_strdup(host));
		break;
	}

	freeifaddrs(ifaddr);
}

static void
read_vpn(GowlModuleBar *self, BarWidget *w)
{
	const gchar *text;

	if (g_file_test("/sys/class/net/tun0", G_FILE_TEST_IS_DIR) ||
	    g_file_test("/sys/class/net/wg0", G_FILE_TEST_IS_DIR))
		text = "VPN \xe2\x9c\x93";
	else
		text = "VPN \xe2\x9c\x97";

	bar_widget_store_output(self, w, g_strdup(text));
}

static void
read_volume(GowlModuleBar *self, BarWidget *w)
{
	bar_schedule_work(self, w, WORK_KIND_VOLUME, NULL, NULL);
}

static void
read_media(GowlModuleBar *self, BarWidget *w)
{
	bar_schedule_work(self, w, WORK_KIND_MEDIA, NULL, NULL);
}

static void
read_git(GowlModuleBar *self, BarWidget *w)
{
	GowlClient      *focused;
	GowlProcessInfo *info;
	gchar           *cwd_copy = NULL;

	/* The focused-client lookup must happen on the dispatch thread
	   (it's guarded by cmacs_gowl_mutex, which this thread holds).
	   We snapshot the CWD and hand it to the worker thread which
	   then walks the directory tree and runs `git status` without
	   touching compositor state. */
	if (self->compositor == NULL)
		return;

	focused = gowl_compositor_get_focused_client(
	              GOWL_COMPOSITOR(self->compositor));
	if (focused == NULL)
		return;

	info = gowl_client_get_process_info(focused);
	if (info == NULL || info->cwd == NULL) {
		if (info != NULL)
			gowl_process_info_free(info);
		return;
	}
	cwd_copy = g_strdup(info->cwd);
	gowl_process_info_free(info);

	bar_schedule_work(self, w, WORK_KIND_GIT, NULL, cwd_copy);
	g_free(cwd_copy);
}

static void
read_podman(GowlModuleBar *self, BarWidget *w)
{
	bar_schedule_work(self, w, WORK_KIND_PODMAN, NULL, NULL);
}

static void
read_weather(GowlModuleBar *self, BarWidget *w)
{
	bar_schedule_work(self, w, WORK_KIND_WEATHER, w->param, NULL);
}

static void
read_cmd(GowlModuleBar *self, BarWidget *w)
{
	if (w->param == NULL || w->param[0] == '\0')
		return;
	bar_schedule_work(self, w, WORK_KIND_CMD, w->param, NULL);
}

static void
read_all_data(GowlModuleBar *self)
{
	BarWidget *w;
	time_t     now;
	gint       bi, i;

	now = time(NULL);

	/* Walk every widget in every configured slot.  Module-level
	   caches (cpu, memory, disk, load, swap, net, io, temp) mean
	   that reading the same stat twice in one tick just bumps the
	   same cached_* counters -- cheap and correct. */
	for (bi = 0; bi < GOWL_BAR_POSITION_COUNT; bi++) {
		GowlBarInstance *bar = &self->bars[bi];

		if (!bar->enabled)
			continue;

		for (i = 0; i < bar->n_widgets; i++) {
			w = &bar->widgets[i];

			/* Unified throttle: if the widget has an interval set,
			   only re-read when the interval has elapsed.  Widgets
			   with output_interval == 0 run on every tick. */
			if (w->output_interval > 0 &&
			    now - w->output_read_time < w->output_interval)
				continue;
			w->output_read_time = now;

			switch (w->type) {
			case BAR_WIDGET_CPU:     read_cpu(self);                 break;
			case BAR_WIDGET_MEMORY:  read_memory(self);              break;
			case BAR_WIDGET_DISK:    read_disk(self);                break;
			case BAR_WIDGET_BATTERY: read_battery(self);             break;
			case BAR_WIDGET_LOAD:    read_load(self);                break;
			case BAR_WIDGET_SWAP:    read_swap(self);                break;
			case BAR_WIDGET_NET:     read_net(self, w);              break;
			case BAR_WIDGET_IO:      read_io(self, w);               break;
			case BAR_WIDGET_TEMP:    read_temp(self);                break;
			case BAR_WIDGET_GPU:     read_gpu(self, w);              break;
			case BAR_WIDGET_WIFI:    read_wifi(self, w);             break;
			case BAR_WIDGET_IP:      read_ip(self, w);               break;
			case BAR_WIDGET_VPN:     read_vpn(self, w);              break;
			case BAR_WIDGET_VOLUME:  read_volume(self, w);           break;
			case BAR_WIDGET_MEDIA:   read_media(self, w);            break;
			case BAR_WIDGET_GIT:     read_git(self, w);              break;
			case BAR_WIDGET_PODMAN:  read_podman(self, w);           break;
			case BAR_WIDGET_WEATHER: read_weather(self, w);          break;
			case BAR_WIDGET_CMD:     read_cmd(self, w);              break;
			default: break;
			}
		}
	}
}

/* ----------------------------------------------------------------
 * Widget text formatting
 * ---------------------------------------------------------------- */

static void
widget_text(GowlModuleBar *self, BarWidget *w, char *buf, gsize bufsz)
{
	time_t now;
	struct tm *tm;

	switch (w->type) {
	case BAR_WIDGET_CPU:
		snprintf(buf, bufsz, "CPU %d%%", self->cached_cpu_pct);
		break;
	case BAR_WIDGET_MEMORY:
		if (self->cached_mem_used_mb >= 1024)
			snprintf(buf, bufsz, "MEM %.1fG",
			         (gdouble)self->cached_mem_used_mb / 1024.0);
		else
			snprintf(buf, bufsz, "MEM %ldM", self->cached_mem_used_mb);
		break;
	case BAR_WIDGET_DISK:
		{
			const gchar *path;
			struct statvfs st;
			glong free_gb;

			path = (w->param != NULL) ? w->param : "/";
			if (statvfs(path, &st) == 0) {
				free_gb = (glong)((st.f_bavail * st.f_frsize) /
				          (1024UL * 1024UL * 1024UL));
				g_snprintf(buf, bufsz, "%s %ldG", path, free_gb);
			} else {
				snprintf(buf, bufsz, "%s ?", path);
			}
		}
		break;
	case BAR_WIDGET_BATTERY:
		if (self->cached_battery >= 0)
			snprintf(buf, bufsz, "BAT %d%%", self->cached_battery);
		else
			buf[0] = '\0';
		break;
	case BAR_WIDGET_CLOCK:
		now = time(NULL);
		tm = localtime(&now);
		strftime(buf, bufsz, "%a %b %d  %H:%M", tm);
		break;
	case BAR_WIDGET_LOAD:
		snprintf(buf, bufsz, "LOAD %.2f", self->cached_load_1);
		break;
	case BAR_WIDGET_SWAP:
		if (self->cached_swap_total_mb > 0) {
			if (self->cached_swap_used_mb >= 1024)
				snprintf(buf, bufsz, "SWAP %.1fG/%.1fG",
				         (gdouble)self->cached_swap_used_mb / 1024.0,
				         (gdouble)self->cached_swap_total_mb / 1024.0);
			else
				snprintf(buf, bufsz, "SWAP %ldM/%ldM",
				         self->cached_swap_used_mb,
				         self->cached_swap_total_mb);
		} else {
			buf[0] = '\0';
		}
		break;
	case BAR_WIDGET_NET:
		{
			char rxb[16], txb[16];
			format_rate(self->cached_net_rx_rate, rxb, sizeof(rxb));
			format_rate(self->cached_net_tx_rate, txb, sizeof(txb));
			snprintf(buf, bufsz, "\xe2\x86\x93%s \xe2\x86\x91%s",
			         rxb, txb);
		}
		break;
	case BAR_WIDGET_IO:
		{
			char rdb[16], wrb[16];
			format_rate(self->cached_io_rd_rate, rdb, sizeof(rdb));
			format_rate(self->cached_io_wr_rate, wrb, sizeof(wrb));
			snprintf(buf, bufsz, "R:%s W:%s", rdb, wrb);
		}
		break;
	case BAR_WIDGET_TEMP:
		if (self->cached_temp_mc > 0)
			snprintf(buf, bufsz, "%d\xc2\xb0""C",
			         self->cached_temp_mc / 1000);
		else
			buf[0] = '\0';
		break;
	case BAR_WIDGET_UPTIME:
		{
			FILE *uf;
			gdouble up_sec;

			uf = fopen("/proc/uptime", "r");
			if (uf != NULL && fscanf(uf, "%lf", &up_sec) == 1) {
				glong total_min, hours, days, mins;
				total_min = (glong)(up_sec / 60.0);
				days  = total_min / (60 * 24);
				hours = (total_min / 60) % 24;
				mins  = total_min % 60;
				if (days > 0)
					snprintf(buf, bufsz, "UP %ldd %ldh",
					         days, hours);
				else
					snprintf(buf, bufsz, "UP %ldh %ldm",
					         hours, mins);
			} else {
				buf[0] = '\0';
			}
			if (uf != NULL) fclose(uf);
		}
		break;
	case BAR_WIDGET_HOST:
		if (self->cached_hostname != NULL)
			snprintf(buf, bufsz, "%s", self->cached_hostname);
		else
			buf[0] = '\0';
		break;
	case BAR_WIDGET_USER:
		if (self->cached_username != NULL)
			snprintf(buf, bufsz, "%s", self->cached_username);
		else
			buf[0] = '\0';
		break;
	case BAR_WIDGET_KEYMAP:
		if (self->cached_keymap != NULL)
			snprintf(buf, bufsz, "%s", self->cached_keymap);
		else
			buf[0] = '\0';
		break;
	case BAR_WIDGET_GPU:
	case BAR_WIDGET_WIFI:
	case BAR_WIDGET_IP:
	case BAR_WIDGET_VPN:
	case BAR_WIDGET_VOLUME:
	case BAR_WIDGET_MEDIA:
	case BAR_WIDGET_GIT:
	case BAR_WIDGET_PODMAN:
	case BAR_WIDGET_WEATHER:
	case BAR_WIDGET_CMD:
		/* These use per-widget cached_output, which worker threads
		   may be updating concurrently. Copy under the mutex. */
		g_mutex_lock(&self->output_mutex);
		if (w->cached_output != NULL)
			g_strlcpy(buf, w->cached_output, bufsz);
		else
			buf[0] = '\0';
		g_mutex_unlock(&self->output_mutex);
		break;
	case BAR_WIDGET_TODO:
		{
			const gchar *val = NULL;
			if (self->widget_data != NULL)
				val = g_hash_table_lookup(self->widget_data, "todo");
			if (val != NULL)
				snprintf(buf, bufsz, "%s", val);
			else
				buf[0] = '\0';
		}
		break;
	default:
		buf[0] = '\0';
		break;
	}
}

/* ----------------------------------------------------------------
 * Widget config parsing
 * ---------------------------------------------------------------- */

static BarWidgetType
widget_type_from_name(const gchar *name)
{
	if (strcmp(name, "cpu") == 0)     return BAR_WIDGET_CPU;
	if (strcmp(name, "memory") == 0)  return BAR_WIDGET_MEMORY;
	if (strcmp(name, "mem") == 0)     return BAR_WIDGET_MEMORY;
	if (strcmp(name, "disk") == 0)    return BAR_WIDGET_DISK;
	if (strcmp(name, "battery") == 0) return BAR_WIDGET_BATTERY;
	if (strcmp(name, "bat") == 0)     return BAR_WIDGET_BATTERY;
	if (strcmp(name, "clock") == 0)    return BAR_WIDGET_CLOCK;
	if (strcmp(name, "time") == 0)     return BAR_WIDGET_CLOCK;
	if (strcmp(name, "load") == 0)     return BAR_WIDGET_LOAD;
	if (strcmp(name, "cmd") == 0)      return BAR_WIDGET_CMD;
	if (strcmp(name, "net") == 0)      return BAR_WIDGET_NET;
	if (strcmp(name, "uptime") == 0)   return BAR_WIDGET_UPTIME;
	if (strcmp(name, "host") == 0)     return BAR_WIDGET_HOST;
	if (strcmp(name, "hostname") == 0) return BAR_WIDGET_HOST;
	if (strcmp(name, "temp") == 0)     return BAR_WIDGET_TEMP;
	if (strcmp(name, "gpu") == 0)      return BAR_WIDGET_GPU;
	if (strcmp(name, "swap") == 0)     return BAR_WIDGET_SWAP;
	if (strcmp(name, "io") == 0)       return BAR_WIDGET_IO;
	if (strcmp(name, "volume") == 0)   return BAR_WIDGET_VOLUME;
	if (strcmp(name, "vol") == 0)      return BAR_WIDGET_VOLUME;
	if (strcmp(name, "media") == 0)    return BAR_WIDGET_MEDIA;
	if (strcmp(name, "wifi") == 0)     return BAR_WIDGET_WIFI;
	if (strcmp(name, "ip") == 0)       return BAR_WIDGET_IP;
	if (strcmp(name, "vpn") == 0)      return BAR_WIDGET_VPN;
	if (strcmp(name, "git") == 0)      return BAR_WIDGET_GIT;
	if (strcmp(name, "todo") == 0)     return BAR_WIDGET_TODO;
	if (strcmp(name, "podman") == 0)   return BAR_WIDGET_PODMAN;
	if (strcmp(name, "pod") == 0)      return BAR_WIDGET_PODMAN;
	if (strcmp(name, "user") == 0)     return BAR_WIDGET_USER;
	if (strcmp(name, "keymap") == 0)   return BAR_WIDGET_KEYMAP;
	if (strcmp(name, "weather") == 0)  return BAR_WIDGET_WEATHER;
	return BAR_WIDGET_COUNT; /* invalid */
}

static const gchar *
widget_color_key(BarWidgetType type)
{
	switch (type) {
	case BAR_WIDGET_CPU:     return "cpu-color";
	case BAR_WIDGET_MEMORY:  return "memory-color";
	case BAR_WIDGET_DISK:    return "disk-color";
	case BAR_WIDGET_BATTERY: return "battery-color";
	case BAR_WIDGET_CLOCK:   return "clock-color";
	case BAR_WIDGET_LOAD:    return "load-color";
	case BAR_WIDGET_CMD:     return "cmd-color";
	case BAR_WIDGET_NET:     return "net-color";
	case BAR_WIDGET_UPTIME:  return "uptime-color";
	case BAR_WIDGET_HOST:    return "host-color";
	case BAR_WIDGET_TEMP:    return "temp-color";
	case BAR_WIDGET_GPU:     return "gpu-color";
	case BAR_WIDGET_SWAP:    return "swap-color";
	case BAR_WIDGET_IO:      return "io-color";
	case BAR_WIDGET_VOLUME:  return "volume-color";
	case BAR_WIDGET_MEDIA:   return "media-color";
	case BAR_WIDGET_WIFI:    return "wifi-color";
	case BAR_WIDGET_IP:      return "ip-color";
	case BAR_WIDGET_VPN:     return "vpn-color";
	case BAR_WIDGET_GIT:     return "git-color";
	case BAR_WIDGET_TODO:    return "todo-color";
	case BAR_WIDGET_PODMAN:  return "podman-color";
	case BAR_WIDGET_USER:    return "user-color";
	case BAR_WIDGET_KEYMAP:  return "keymap-color";
	case BAR_WIDGET_WEATHER: return "weather-color";
	default: return NULL;
	}
}

static void
parse_widget_list(GowlModuleBar *self, GowlBarInstance *bar,
                  const gchar *spec)
{
	gchar **parts;
	gint i;

	parts = g_strsplit(spec, " ", -1);

	/* Free old widget params and cached output. The widgets array
	   is stable storage inside the bar struct, so a worker that's
	   still running against an old slot can safely update it under
	   output_mutex -- the worst case is one tick of stale text in
	   the new widget occupying that slot, corrected on the next
	   read_all_data pass. */
	for (i = 0; i < bar->n_widgets; i++) {
		g_free(bar->widgets[i].param);
		bar->widgets[i].param = NULL;
		g_mutex_lock(&self->output_mutex);
		g_free(bar->widgets[i].cached_output);
		bar->widgets[i].cached_output = NULL;
		g_mutex_unlock(&self->output_mutex);
	}

	bar->n_widgets = 0;

	for (i = 0; parts[i] != NULL && bar->n_widgets < BAR_MAX_WIDGETS; i++) {
		BarWidgetType t;
		gchar *name;
		gchar *param;
		gchar *colon;
		gchar *at;
		gint   interval_override;

		if (parts[i][0] == '\0')
			continue;

		/* Per-widget "@N" refresh interval suffix (works for any
		   widget). Uses the LAST '@' so commands that legitimately
		   contain '@' still parse -- e.g. "cmd:ssh user@host@5" is
		   ("ssh user@host", interval 5). If the text after the final
		   '@' isn't a positive integer, the whole token stands. */
		interval_override = 0;
		at = strrchr(parts[i], '@');
		if (at != NULL && at[1] != '\0') {
			gchar *endptr = NULL;
			glong  n = g_ascii_strtoll(at + 1, &endptr, 10);
			if (n > 0 && endptr != NULL && *endptr == '\0') {
				*at = '\0';
				interval_override = (gint)n;
			}
		}

		/* Support "disk:/home" syntax for parameterized widgets */
		name  = parts[i];
		param = NULL;
		colon = strchr(name, ':');
		if (colon != NULL) {
			*colon = '\0';
			param = colon + 1;
		}

		t = widget_type_from_name(name);
		if (t == BAR_WIDGET_COUNT)
			continue;

		bar->widgets[bar->n_widgets].type = t;
		bar->widgets[bar->n_widgets].has_color = FALSE;
		bar->widgets[bar->n_widgets].param =
			(param != NULL && param[0] != '\0') ? g_strdup(param) : NULL;
		bar->widgets[bar->n_widgets].cached_output = NULL;
		bar->widgets[bar->n_widgets].output_read_time = 0;
		bar->widgets[bar->n_widgets].interval_explicit = FALSE;
		bar->widgets[bar->n_widgets].spawn_in_flight = 0;
		/* Default refresh intervals per type (seconds). 0 means
		   "run every tick". These defaults are picked to match the
		   cost of each read path. */
		switch (t) {
		case BAR_WIDGET_GPU:     bar->widgets[bar->n_widgets].output_interval = 5;   break;
		case BAR_WIDGET_WIFI:    bar->widgets[bar->n_widgets].output_interval = 10;  break;
		case BAR_WIDGET_IP:      bar->widgets[bar->n_widgets].output_interval = 30;  break;
		case BAR_WIDGET_VPN:     bar->widgets[bar->n_widgets].output_interval = 10;  break;
		case BAR_WIDGET_VOLUME:  bar->widgets[bar->n_widgets].output_interval = 2;   break;
		case BAR_WIDGET_MEDIA:   bar->widgets[bar->n_widgets].output_interval = 2;   break;
		case BAR_WIDGET_GIT:     bar->widgets[bar->n_widgets].output_interval = 5;   break;
		case BAR_WIDGET_CMD:     bar->widgets[bar->n_widgets].output_interval = 10;  break;
		case BAR_WIDGET_WEATHER: bar->widgets[bar->n_widgets].output_interval = 900; break;
		case BAR_WIDGET_PODMAN:  bar->widgets[bar->n_widgets].output_interval = 30;  break;
		default:                 bar->widgets[bar->n_widgets].output_interval = 0;   break;
		}
		if (interval_override > 0) {
			bar->widgets[bar->n_widgets].output_interval =
				interval_override;
			bar->widgets[bar->n_widgets].interval_explicit = TRUE;
		}
		bar->n_widgets++;
	}
	g_strfreev(parts);
}

/* ----------------------------------------------------------------
 * ANSI color parsing for CMD widget
 * ---------------------------------------------------------------- */

/* Standard ANSI 8-color palette (30-37) */
static const gdouble ansi_colors[8][3] = {
	{ 0.0,   0.0,   0.0   },  /* black */
	{ 0.8,   0.2,   0.2   },  /* red */
	{ 0.2,   0.8,   0.2   },  /* green */
	{ 0.8,   0.8,   0.2   },  /* yellow */
	{ 0.2,   0.4,   0.8   },  /* blue */
	{ 0.8,   0.2,   0.8   },  /* magenta */
	{ 0.2,   0.8,   0.8   },  /* cyan */
	{ 0.8,   0.8,   0.8   },  /* white */
};

/* Bright 8-color palette (90-97) */
static const gdouble ansi_bright[8][3] = {
	{ 0.5,   0.5,   0.5   },  /* bright black (gray) */
	{ 1.0,   0.3,   0.3   },  /* bright red */
	{ 0.3,   1.0,   0.3   },  /* bright green */
	{ 1.0,   1.0,   0.3   },  /* bright yellow */
	{ 0.4,   0.6,   1.0   },  /* bright blue */
	{ 1.0,   0.3,   1.0   },  /* bright magenta */
	{ 0.3,   1.0,   1.0   },  /* bright cyan */
	{ 1.0,   1.0,   1.0   },  /* bright white */
};

/**
 * render_ansi_text:
 *
 * Renders text with ANSI escape codes to a PangoLayout.
 * ANSI colors apply ONLY to ASCII printable characters (0x20-0x7E).
 * Non-ASCII (emoji, CJK, etc.) uses the base widget color.
 *
 * Returns the PangoAttrList (caller must unref) and writes the
 * stripped plain text into @out_text.
 */
static PangoAttrList *
render_ansi_text(const gchar *input, const gdouble base_color[4],
                 GString *out_text)
{
	PangoAttrList *attrs;
	const gchar *p;
	gdouble cur_r, cur_g, cur_b;
	gboolean color_active;

	attrs = pango_attr_list_new();
	cur_r = base_color[0];
	cur_g = base_color[1];
	cur_b = base_color[2];
	color_active = FALSE;

	p = input;
	while (*p != '\0') {
		/* Check for ESC [ */
		if (p[0] == '\033' && p[1] == '[') {
			gint params[8];
			gint n_params;
			const gchar *q;

			p += 2; /* skip ESC [ */
			n_params = 0;
			q = p;

			/* Parse semicolon-separated numbers */
			while (*q >= '0' && *q <= '9') {
				gint val = 0;
				while (*q >= '0' && *q <= '9')
					val = val * 10 + (*q++ - '0');
				if (n_params < 8)
					params[n_params++] = val;
				if (*q == ';')
					q++;
			}

			if (*q == 'm') {
				q++; /* skip 'm' */
				p = q;

				/* Process the parameters */
				if (n_params == 0 || params[0] == 0) {
					/* Reset */
					cur_r = base_color[0];
					cur_g = base_color[1];
					cur_b = base_color[2];
					color_active = FALSE;
				} else if (n_params >= 1 && params[0] >= 30
				           && params[0] <= 37) {
					gint idx = params[0] - 30;
					cur_r = ansi_colors[idx][0];
					cur_g = ansi_colors[idx][1];
					cur_b = ansi_colors[idx][2];
					color_active = TRUE;
				} else if (n_params >= 1 && params[0] >= 90
				           && params[0] <= 97) {
					gint idx = params[0] - 90;
					cur_r = ansi_bright[idx][0];
					cur_g = ansi_bright[idx][1];
					cur_b = ansi_bright[idx][2];
					color_active = TRUE;
				} else if (n_params >= 3 && params[0] == 38
				           && params[1] == 2) {
					/* 24-bit: ESC[38;2;R;G;Bm */
					if (n_params >= 5) {
						cur_r = (gdouble)params[2] / 255.0;
						cur_g = (gdouble)params[3] / 255.0;
						cur_b = (gdouble)params[4] / 255.0;
						color_active = TRUE;
					}
				} else if (n_params >= 3 && params[0] == 38
				           && params[1] == 5) {
					/* 256-color: ESC[38;5;Nm */
					gint n = params[2];
					if (n >= 0 && n < 8) {
						cur_r = ansi_colors[n][0];
						cur_g = ansi_colors[n][1];
						cur_b = ansi_colors[n][2];
					} else if (n >= 8 && n < 16) {
						cur_r = ansi_bright[n - 8][0];
						cur_g = ansi_bright[n - 8][1];
						cur_b = ansi_bright[n - 8][2];
					} else if (n >= 16 && n < 232) {
						/* 6x6x6 color cube */
						gint idx = n - 16;
						cur_r = (gdouble)((idx / 36) % 6) / 5.0;
						cur_g = (gdouble)((idx / 6) % 6) / 5.0;
						cur_b = (gdouble)(idx % 6) / 5.0;
					} else if (n >= 232 && n < 256) {
						gdouble gray = (gdouble)(n - 232) / 23.0;
						cur_r = cur_g = cur_b = gray;
					}
					color_active = TRUE;
				}
				/* Skip bold/dim/italic (1,2,3) — just continue */
				continue;
			} else {
				/* Unknown sequence — skip to next alpha or end */
				p = q;
				if (*p != '\0') p++;
				continue;
			}
			continue;
		}

		/* Regular character */
		{
			guint start = (guint)out_text->len;
			gboolean is_ascii;

			/* Append the character (could be multi-byte UTF-8) */
			if (((guchar)*p & 0x80) == 0) {
				/* ASCII */
				is_ascii = (*p >= 0x20 && *p <= 0x7E);
				g_string_append_c(out_text, *p);
				p++;
			} else {
				/* UTF-8 multi-byte: copy entire sequence */
				guchar lead = (guchar)*p;
				gint nbytes;

				is_ascii = FALSE;
				if ((lead & 0xE0) == 0xC0)      nbytes = 2;
				else if ((lead & 0xF0) == 0xE0)  nbytes = 3;
				else if ((lead & 0xF8) == 0xF0)  nbytes = 4;
				else                              nbytes = 1;

				g_string_append_len(out_text, p, nbytes);
				p += nbytes;
			}

			/* Apply color attribute */
			{
				PangoAttribute *attr;
				gdouble r, g, b;

				if (is_ascii && color_active) {
					r = cur_r; g = cur_g; b = cur_b;
				} else {
					r = base_color[0];
					g = base_color[1];
					b = base_color[2];
				}

				attr = pango_attr_foreground_new(
					(guint16)(r * 65535),
					(guint16)(g * 65535),
					(guint16)(b * 65535));
				attr->start_index = start;
				attr->end_index   = (guint)out_text->len;
				pango_attr_list_insert(attrs, attr);
			}
		}
	}

	return attrs;
}


/* ----------------------------------------------------------------
 * Rendering
 * ---------------------------------------------------------------- */

/* Number of tags to display, from the compositor config (9 when
   unavailable). */
static gint
bar_tag_count(GowlModuleBar *self)
{
	GowlConfig *config;

	if (self->compositor == NULL)
		return 9;
	config = gowl_compositor_get_config(GOWL_COMPOSITOR(self->compositor));
	if (config == NULL)
		return 9;
	return gowl_config_get_tag_count(config);
}

/* Compute the selected / occupied / urgent tag bitmasks for @monitor.
   @selected is the monitor's viewed tag set; @occupied / @urgent are
   the union of tags across that monitor's clients.  Shared by the
   renderer and the redraw-signature so the two never drift. */
static void
bar_compute_tag_masks(GowlModuleBar *self, GowlMonitor *monitor,
                      guint32 *selected, guint32 *occupied,
                      guint32 *urgent)
{
	GowlCompositor *comp;
	GList *clients, *l;

	*selected = 0;
	*occupied = 0;
	*urgent   = 0;

	if (monitor == NULL || self->compositor == NULL)
		return;

	comp = GOWL_COMPOSITOR(self->compositor);
	*selected = gowl_monitor_get_tags(monitor);

	clients = gowl_compositor_get_clients(comp);
	for (l = clients; l != NULL; l = l->next) {
		GowlClient *c = GOWL_CLIENT(l->data);
		guint32 ct;

		if (gowl_client_get_monitor(c) != monitor)
			continue;
		ct = gowl_client_get_tags(c);
		*occupied |= ct;
		if (gowl_client_get_urgent(c))
			*urgent |= ct;
	}
}

/* Draw the dwm-style tag indicator at the left edge of @bar for
   @monitor.  Returns the x coordinate just past the row, where the
   title should start.  Reads the selected tag set from @monitor and
   scans the compositor's clients for occupied / urgent tags. */
static gint
bar_render_tags(GowlModuleBar *self, GowlBarInstance *bar,
                GowlMonitor *monitor, cairo_t *cr,
                PangoLayout *layout, gint height,
                gint start_x, gint text_y)
{
	guint32 sel_mask, occ_mask, urg_mask;
	gint tag_count, box_w, x, i;

	if (!bar->show_tags || monitor == NULL || self->compositor == NULL)
		return start_x;

	bar_compute_tag_masks(self, monitor, &sel_mask, &occ_mask, &urg_mask);

	tag_count = bar_tag_count(self);
	box_w = height;          /* square boxes matching the bar height */
	x = start_x;

	for (i = 0; i < tag_count; i++) {
		guint32 bit = (guint32)1u << i;
		gboolean is_sel = (sel_mask & bit) != 0;
		gboolean is_urg = (urg_mask & bit) != 0;
		gboolean is_occ = (occ_mask & bit) != 0;
		const gdouble *fg;
		char label[8];
		PangoRectangle lg;

		if (is_urg) {
			cairo_set_source_rgba(cr, bar->tag_urgent_bg[0],
				bar->tag_urgent_bg[1], bar->tag_urgent_bg[2],
				bar->tag_urgent_bg[3]);
			cairo_rectangle(cr, x, 0, box_w, height);
			cairo_fill(cr);
			fg = bar->tag_urgent_fg;
		} else if (is_sel) {
			cairo_set_source_rgba(cr, bar->tag_active_bg[0],
				bar->tag_active_bg[1], bar->tag_active_bg[2],
				bar->tag_active_bg[3]);
			cairo_rectangle(cr, x, 0, box_w, height);
			cairo_fill(cr);
			fg = bar->tag_active_fg;
		} else if (is_occ) {
			fg = bar->tag_occupied_fg;
		} else {
			fg = bar->tag_empty_fg;
		}

		g_snprintf(label, sizeof(label), "%d", i + 1);
		pango_layout_set_width(layout, -1);
		pango_layout_set_text(layout, label, -1);
		pango_layout_get_pixel_extents(layout, NULL, &lg);
		cairo_set_source_rgba(cr, fg[0], fg[1], fg[2], fg[3]);
		cairo_move_to(cr, x + (box_w - lg.width) / 2, text_y);
		pango_cairo_show_layout(cr, layout);

		/* Small corner marker for occupied, non-selected tags
		   (dwm convention). */
		if (is_occ && !is_sel && !is_urg) {
			cairo_set_source_rgba(cr, fg[0], fg[1], fg[2], fg[3]);
			cairo_rectangle(cr, x + 2.0, 2.0, 3.0, 3.0);
			cairo_fill(cr);
		}

		x += box_w;
	}

	return x + 8;            /* gap before the title */
}

static BarBuffer *
bar_render(GowlModuleBar *self, GowlBarInstance *bar,
           GowlMonitor *monitor, gint width, gint height)
{
	cairo_surface_t *cs;
	cairo_t *cr;
	PangoLayout *layout;
	PangoFontDescription *font;
	BarBuffer *buf;
	guchar *pixels;
	gint stride;
	gint padding;
	gint text_y;
	PangoRectangle ink, logical;
	GowlClient *focused;
	const gchar *title;
	gint right_x;
	gint left_x;
	gint title_w;
	gint i;
	char wtext[128];
	gint separator_w;

	cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	cr = cairo_create(cs);

	/* Background */
	cairo_set_source_rgba(cr, bar->bg_color[0], bar->bg_color[1],
	                      bar->bg_color[2], bar->bg_color[3]);
	cairo_paint(cr);

	/* Set up pango */
	layout = pango_cairo_create_layout(cr);
	font = pango_font_description_from_string(bar->font_desc);
	pango_layout_set_font_description(layout, font);

	padding = 10;
	text_y = (height - pango_font_description_get_size(font) /
	          PANGO_SCALE) / 2;
	if (text_y < 2)
		text_y = 2;

	/* Measure separator width */
	pango_layout_set_text(layout, "  ", -1);
	pango_layout_get_pixel_extents(layout, &ink, &logical);
	separator_w = logical.width;

	/* Left edge: dwm-style tag indicator, then the title. */
	left_x = bar_render_tags(self, bar, monitor, cr, layout, height,
	                         padding, text_y);

	/* Left: title */
	if (bar->custom_title != NULL && bar->custom_title[0] != '\0') {
		title = bar->custom_title;
	} else {
		focused = (self->compositor != NULL) ?
			gowl_compositor_get_focused_client(
				GOWL_COMPOSITOR(self->compositor)) : NULL;
		title = (focused != NULL) ? gowl_client_get_title(focused) : "cmacs";
		if (title == NULL)
			title = "cmacs";
	}

	pango_layout_set_text(layout, title, -1);
	/* Title occupies from the tag row's right edge to mid-bar. */
	title_w = width / 2 - left_x - padding;
	if (title_w < 0)
		title_w = 0;
	pango_layout_set_width(layout, title_w * PANGO_SCALE);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

	/* Colorize title: split on delimiters, cycle palette colors */
	if (bar->title_palette_size > 0 && bar->title_delimiters != NULL) {
		PangoAttrList *attrs;
		gint seg_idx;
		gint pos;
		gint len;

		attrs = pango_attr_list_new();
		seg_idx = 0;
		len = (gint)strlen(title);

		for (pos = 0; pos < len; pos++) {
			PangoAttribute *attr;
			const gdouble *c;

			if (strchr(bar->title_delimiters, title[pos]) != NULL) {
				/* Delimiter character */
				c = bar->title_delimiter_color;
			} else {
				/* Regular character — find end of segment */
				gint start = pos;
				while (pos < len &&
				       strchr(bar->title_delimiters, title[pos]) == NULL)
					pos++;
				c = bar->title_palette[seg_idx % bar->title_palette_size];
				attr = pango_attr_foreground_new(
					(guint16)(c[0] * 65535),
					(guint16)(c[1] * 65535),
					(guint16)(c[2] * 65535));
				attr->start_index = (guint)start;
				attr->end_index   = (guint)pos;
				pango_attr_list_insert(attrs, attr);
				seg_idx++;
				pos--; /* re-process delimiter in next iteration */
				continue;
			}
			/* Single delimiter char */
			attr = pango_attr_foreground_new(
				(guint16)(c[0] * 65535),
				(guint16)(c[1] * 65535),
				(guint16)(c[2] * 65535));
			attr->start_index = (guint)pos;
			attr->end_index   = (guint)(pos + 1);
			pango_attr_list_insert(attrs, attr);
		}
		pango_layout_set_attributes(layout, attrs);
		pango_attr_list_unref(attrs);
	} else {
		cairo_set_source_rgba(cr, bar->fg_color[0], bar->fg_color[1],
		                      bar->fg_color[2], bar->fg_color[3]);
	}

	cairo_move_to(cr, left_x, text_y);
	/* When using pango attributes, set cairo source to white so
	 * the attribute colors are used directly. */
	if (bar->title_palette_size > 0)
		cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
	pango_cairo_show_layout(cr, layout);
	pango_layout_set_attributes(layout, NULL);

	/* Right: widgets, rendered right-to-left */
	right_x = width - padding;
	pango_layout_set_width(layout, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);

	for (i = bar->n_widgets - 1; i >= 0; i--) {
		const gdouble *c;

		widget_text(self, &bar->widgets[i], wtext, sizeof(wtext));
		if (wtext[0] == '\0')
			continue;

		/* Use per-widget color if set, otherwise fg_color */
		c = bar->widgets[i].has_color ? bar->widgets[i].color
		                              : bar->fg_color;

		/* CMD widgets: ANSI color rendering. Work from the local
		   `wtext` copy since cached_output may be swapped by a
		   worker thread at any moment. */
		if (bar->widgets[i].type == BAR_WIDGET_CMD &&
		    strchr(wtext, '\033') != NULL) {
			GString *plain;
			PangoAttrList *cmd_attrs;

			plain = g_string_new(NULL);
			cmd_attrs = render_ansi_text(wtext, c, plain);
			pango_layout_set_text(layout, plain->str, (gint)plain->len);
			pango_layout_set_attributes(layout, cmd_attrs);
			pango_layout_get_pixel_extents(layout, &ink, &logical);
			right_x -= logical.width;
			cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
			cairo_move_to(cr, right_x, text_y);
			pango_cairo_show_layout(cr, layout);
			pango_layout_set_attributes(layout, NULL);
			pango_attr_list_unref(cmd_attrs);
			g_string_free(plain, TRUE);
		} else {
			cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);
			pango_layout_set_text(layout, wtext, -1);
			pango_layout_get_pixel_extents(layout, &ink, &logical);
			right_x -= logical.width;
			cairo_move_to(cr, right_x, text_y);
			pango_cairo_show_layout(cr, layout);
		}

		/* Add separator between widgets */
		right_x -= separator_w;
	}

	pango_font_description_free(font);
	g_object_unref(layout);

	/* Copy to pixel buffer */
	cairo_surface_flush(cs);
	stride = cairo_image_surface_get_stride(cs);
	pixels = (guchar *)g_malloc((gsize)(stride * height));
	memcpy(pixels, cairo_image_surface_get_data(cs), (gsize)(stride * height));
	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	/* Create wlr_buffer */
	buf = g_new0(BarBuffer, 1);
	buf->pixels = pixels;
	buf->size   = (gsize)(stride * height);
	buf->stride = stride;
	wlr_buffer_init(&buf->base, &bar_buffer_impl, width, height);

	return buf;
}

/* ----------------------------------------------------------------
 * Scene buffer management
 * ---------------------------------------------------------------- */

/* Compute the y-origin of @bar on @monitor given its rendered
   height.  TOP bars sit flush with the monitor's top edge; BOTTOM
   bars sit flush with the bottom. */
static gint
bar_surface_y(GowlBarInstance *bar, gint mon_y, gint mon_h)
{
	if (bar->position == GOWL_BAR_POSITION_BOTTOM)
		return mon_y + mon_h - bar->bar_height;
	return mon_y;
}

static void
bar_create_surface(GowlModuleBar *self, GowlBarInstance *bar,
                   GowlMonitor *monitor)
{
	GowlCompositor *comp;
	struct wlr_scene_tree *top_layer;
	BarSurface *surface;
	BarBuffer *buf;
	const gchar *name;
	gint mon_x, mon_y, mon_w, mon_h;
	gint surf_y;

	if (!bar->enabled || !bar->visible || bar->bar_height <= 0)
		return;

	comp = GOWL_COMPOSITOR(self->compositor);
	name = gowl_monitor_get_name(monitor);
	gowl_monitor_get_geometry(monitor, &mon_x, &mon_y, &mon_w, &mon_h);

	if (mon_w <= 0 || mon_h <= 0)
		return;

	/* Remove existing surface for this monitor */
	surface = (BarSurface *)g_hash_table_lookup(bar->surfaces, name);
	if (surface != NULL) {
		if (surface->scene_buf != NULL)
			wlr_scene_node_destroy(&surface->scene_buf->node);
		g_free(surface->last_signature);
		g_hash_table_remove(bar->surfaces, name);
		g_free(surface);
	}

	top_layer = gowl_compositor_get_scene_layer(comp,
	                                            GOWL_SCENE_LAYER_TOP);
	if (top_layer == NULL)
		return;

	buf = bar_render(self, bar, monitor, mon_w, bar->bar_height);

	surf_y = bar_surface_y(bar, mon_y, mon_h);

	surface = g_new0(BarSurface, 1);
	surface->scene_buf = wlr_scene_buffer_create(top_layer, &buf->base);
	surface->width  = mon_w;
	surface->height = bar->bar_height;
	surface->mon_x  = mon_x;
	surface->mon_y  = surf_y;

	wlr_scene_node_set_position(&surface->scene_buf->node, mon_x, surf_y);
	/* Force the new node to the top of the layer's z-stack.
	   wlroots' scene damage-tracking otherwise leaves the new
	   sibling silently invisible when another bar surface
	   already exists in GOWL_SCENE_LAYER_TOP -- the bottom-bar
	   surface gets added but never rendered until something
	   re-traverses the layer.  raise_to_top forces that
	   re-traversal. */
	wlr_scene_node_raise_to_top(&surface->scene_buf->node);
	wlr_buffer_drop(&buf->base);

	g_hash_table_insert(bar->surfaces, g_strdup(name), surface);
}

/* Build a canonical signature string capturing everything bar_render
   reads: geometry, background/foreground/palette colors, font, title
   (custom or focused client), and each widget's text + color. Callers
   free the returned string with g_free().

   Used by bar_redraw_all to skip expensive cairo/pango rendering (and
   the scene-graph buffer swap that would force the compositor to
   redamage the output) when nothing visible has changed since the
   previous tick. */
static gchar *
bar_build_signature(GowlModuleBar *self, GowlBarInstance *bar,
                    GowlMonitor *monitor, gint width, gint height)
{
	GString *s;
	GowlClient *focused;
	const gchar *title;
	gint i, pi;
	char wtext[128];

	s = g_string_sized_new(256);

	g_string_append_printf(s, "g:%dx%d;", width, height);

	/* Tag indicator state: a change to selected / occupied / urgent
	   tags must force a repaint even when nothing else changed. */
	if (bar->show_tags) {
		guint32 sel_mask, occ_mask, urg_mask;

		bar_compute_tag_masks(self, monitor, &sel_mask, &occ_mask,
		                      &urg_mask);
		g_string_append_printf(s, "tg:%u,%u,%u;",
		                       sel_mask, occ_mask, urg_mask);
	}
	g_string_append_printf(s, "bg:%.3f,%.3f,%.3f,%.3f;",
	                       bar->bg_color[0], bar->bg_color[1],
	                       bar->bg_color[2], bar->bg_color[3]);
	g_string_append_printf(s, "fg:%.3f,%.3f,%.3f,%.3f;",
	                       bar->fg_color[0], bar->fg_color[1],
	                       bar->fg_color[2], bar->fg_color[3]);
	g_string_append_printf(s, "font:%s;",
	                       bar->font_desc != NULL ? bar->font_desc : "");

	/* Title palette state: size, delimiters, delimiter color, palette
	   entries. Config changes on any of these must re-render even if
	   the title string itself is unchanged. */
	g_string_append_printf(s, "tps:%d;", bar->title_palette_size);
	g_string_append_printf(s, "tdl:%s;",
	                       bar->title_delimiters != NULL
	                       ? bar->title_delimiters : "");
	g_string_append_printf(s, "tdc:%.3f,%.3f,%.3f,%.3f;",
	                       bar->title_delimiter_color[0],
	                       bar->title_delimiter_color[1],
	                       bar->title_delimiter_color[2],
	                       bar->title_delimiter_color[3]);
	for (pi = 0; pi < bar->title_palette_size && pi < 8; pi++) {
		g_string_append_printf(s, "tp%d:%.3f,%.3f,%.3f,%.3f;",
		                       pi,
		                       bar->title_palette[pi][0],
		                       bar->title_palette[pi][1],
		                       bar->title_palette[pi][2],
		                       bar->title_palette[pi][3]);
	}

	/* Title text (custom override, or focused client title, or
	   fallback). Matches the selection logic in bar_render. */
	if (bar->custom_title != NULL && bar->custom_title[0] != '\0') {
		title = bar->custom_title;
	} else {
		focused = (self->compositor != NULL)
			? gowl_compositor_get_focused_client(
				GOWL_COMPOSITOR(self->compositor))
			: NULL;
		title = (focused != NULL)
			? gowl_client_get_title(focused)
			: "cmacs";
		if (title == NULL)
			title = "cmacs";
	}
	g_string_append(s, "t:");
	g_string_append(s, title);
	g_string_append_c(s, ';');

	g_string_append_printf(s, "nw:%d;", bar->n_widgets);
	for (i = 0; i < bar->n_widgets; i++) {
		BarWidget *w = &bar->widgets[i];
		widget_text(self, w, wtext, sizeof(wtext));
		g_string_append_printf(s, "w%d:t=%d,c=%d,", i,
		                       (gint)w->type, w->has_color ? 1 : 0);
		if (w->has_color) {
			g_string_append_printf(s, "%.3f,%.3f,%.3f,%.3f,",
			                       w->color[0], w->color[1],
			                       w->color[2], w->color[3]);
		}
		g_string_append(s, "|");
		g_string_append(s, wtext);
		g_string_append_c(s, ';');
	}

	return g_string_free(s, FALSE);
}

/* Destroy the bar surface for monitor @name (if any) and drop its hash
   entry.  Mirrors the inline teardown in bar_create_surface(); used to
   reclaim surfaces for disabled or unplugged monitors so their bars do
   not linger in the shared TOP layer over the surviving outputs. */
static void
bar_destroy_surface_for(GowlBarInstance *bar, const gchar *name)
{
	BarSurface *surface;

	if (bar->surfaces == NULL || name == NULL)
		return;
	surface = (BarSurface *)g_hash_table_lookup(bar->surfaces, name);
	if (surface == NULL)
		return;
	if (surface->scene_buf != NULL)
		wlr_scene_node_destroy(&surface->scene_buf->node);
	g_free(surface->last_signature);
	g_hash_table_remove(bar->surfaces, name);
	g_free(surface);
}

static void
bar_redraw_all(GowlModuleBar *self)
{
	GowlCompositor *comp;
	GList *monitors, *l;
	gint bi;

	if (self->compositor == NULL)
		return;

	comp = GOWL_COMPOSITOR(self->compositor);

	/* Refresh cached system data once per tick; both bars read from
	   the same cached_* fields. */
	read_all_data(self);

	monitors = gowl_compositor_get_monitors(comp);

	for (bi = 0; bi < GOWL_BAR_POSITION_COUNT; bi++) {
		GowlBarInstance *bar = &self->bars[bi];

		if (!bar->enabled || !bar->visible || bar->bar_height <= 0)
			continue;

		for (l = monitors; l != NULL; l = l->next) {
			GowlMonitor *mon = GOWL_MONITOR(l->data);
			const gchar *name = gowl_monitor_get_name(mon);
			BarSurface *surface;
			BarBuffer *buf;
			gchar *sig;
			gint mon_x, mon_y, mon_w, mon_h;
			gint surf_y;

			/* A disabled monitor (e.g. a lid-shut internal panel)
			   keeps its GowlMonitor; drop any bar surface it still
			   owns and never create one, so its bar does not linger
			   stacked over the surviving output. */
			if (!gowl_monitor_get_enabled(mon)) {
				bar_destroy_surface_for(bar, name);
				continue;
			}

			surface = (BarSurface *)g_hash_table_lookup(bar->surfaces,
			                                             name);
			if (surface == NULL || surface->scene_buf == NULL) {
				bar_create_surface(self, bar, mon);
				continue;
			}

			/* Refresh cached position in case the monitor moved or
			   the bar toggled between TOP and BOTTOM. */
			gowl_monitor_get_geometry(mon, &mon_x, &mon_y,
			                          &mon_w, &mon_h);

			/* Monitor resized: the cached surface buffer is the
			   wrong width (signature and render below both use the
			   stale surface->width).  Rebuild it at the new
			   geometry -- bar_create_surface() destroys the old
			   surface and renders a fresh buffer sized to mon_w. */
			if (surface->width != mon_w ||
			    surface->height != bar->bar_height) {
				bar_create_surface(self, bar, mon);
				continue;
			}

			surf_y = bar_surface_y(bar, mon_y, mon_h);
			if (surface->mon_x != mon_x || surface->mon_y != surf_y) {
				wlr_scene_node_set_position(
					&surface->scene_buf->node, mon_x, surf_y);
				surface->mon_x = mon_x;
				surface->mon_y = surf_y;
			}

			/* Skip the cairo/pango render + scene buffer swap when
			   nothing visible has changed. */
			sig = bar_build_signature(self, bar, mon,
			                          surface->width, surface->height);
			if (surface->last_signature != NULL &&
			    strcmp(sig, surface->last_signature) == 0) {
				g_free(sig);
				continue;
			}
			g_free(surface->last_signature);
			surface->last_signature = sig;

			buf = bar_render(self, bar, mon,
			                  surface->width, surface->height);
			wlr_scene_buffer_set_buffer(surface->scene_buf, &buf->base);
			wlr_buffer_drop(&buf->base);
		}
	}
}

static void
bar_instance_destroy_surfaces(GowlBarInstance *bar)
{
	GHashTableIter iter;
	gpointer key, value;

	if (bar->surfaces == NULL)
		return;

	g_hash_table_iter_init(&iter, bar->surfaces);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		BarSurface *surface = (BarSurface *)value;
		if (surface->scene_buf != NULL)
			wlr_scene_node_destroy(&surface->scene_buf->node);
		g_free(surface->last_signature);
		g_free(surface);
	}
	g_hash_table_remove_all(bar->surfaces);
}

static void
bar_destroy_all(GowlModuleBar *self)
{
	gint i;
	for (i = 0; i < GOWL_BAR_POSITION_COUNT; i++)
		bar_instance_destroy_surfaces(&self->bars[i]);
}

/* ----------------------------------------------------------------
 * Signal callbacks
 * ---------------------------------------------------------------- */

static void
bar_on_focus_changed(GowlCompositor *comp, GObject *client,
                     gpointer user_data)
{
	(void)comp;
	(void)client;
	bar_redraw_all(GOWL_MODULE_BAR(user_data));
}

static void
bar_on_client_changed(GowlCompositor *comp, GObject *client,
                      gpointer user_data)
{
	(void)comp;
	(void)client;
	bar_redraw_all(GOWL_MODULE_BAR(user_data));
}

/* Compute tick period in ms. Defaults to 5s for the dynamic system
   widgets (cpu/memory/clock/...); a cmd widget with a smaller explicit
   @N interval speeds up the tick so it can actually reach that rate.
   Slow cmd widgets (e.g. weather@900) do not slow the tick down --
   they just use their cached output across the extra redraws.  The
   min runs across widgets in every enabled bar slot so fast widgets
   on either bar pull the tick.

   The 5 s default is a battery / idle-CPU tradeoff: at 2 s the bar
   ticked often enough that cpu/memory deltas produced scene damage
   every second tick, forcing the compositor to render a frame and
   burning ~1-2% idle CPU for widgets nobody reads sub-second anyway.
   Widgets that genuinely need faster updates can still pull the tick
   via @N. */
static int
bar_tick_ms(GowlModuleBar *self)
{
	gint min_s = 5;
	gint bi, i;

	for (bi = 0; bi < GOWL_BAR_POSITION_COUNT; bi++) {
		GowlBarInstance *bar = &self->bars[bi];
		if (!bar->enabled)
			continue;
		for (i = 0; i < bar->n_widgets; i++) {
			gint ival = bar->widgets[i].output_interval;
			if (ival > 0 && ival < min_s)
				min_s = ival;
		}
	}
	if (min_s < 1)
		min_s = 1;
	return min_s * 1000;
}

static int
bar_tick(void *data)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(data);
	bar_redraw_all(self);

	if (self->tick_timer != NULL)
		wl_event_source_timer_update(self->tick_timer, bar_tick_ms(self));

	return 0;
}

/* ----------------------------------------------------------------
 * GowlModule virtual methods
 * ---------------------------------------------------------------- */

static gboolean
bar_activate(GowlModule *mod)
{
	(void)mod;
	return TRUE;
}

static void
bar_deactivate(GowlModule *mod)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(mod);

	if (self->focus_handler_id != 0 && self->compositor != NULL) {
		g_signal_handler_disconnect(self->compositor,
		                            self->focus_handler_id);
		self->focus_handler_id = 0;
	}
	if (self->client_added_id != 0 && self->compositor != NULL) {
		g_signal_handler_disconnect(self->compositor,
		                            self->client_added_id);
		self->client_added_id = 0;
	}
	if (self->client_removed_id != 0 && self->compositor != NULL) {
		g_signal_handler_disconnect(self->compositor,
		                            self->client_removed_id);
		self->client_removed_id = 0;
	}

	if (self->tick_timer != NULL) {
		wl_event_source_remove(self->tick_timer);
		self->tick_timer = NULL;
	}

	bar_destroy_all(self);
	self->compositor = NULL;
}

static const gchar *
bar_get_name(GowlModule *mod)
{
	(void)mod;
	return "bar";
}

static const gchar *
bar_get_description(GowlModule *mod)
{
	(void)mod;
	return "Compositor status bar with configurable system widgets";
}

static const gchar *
bar_get_version(GowlModule *mod)
{
	(void)mod;
	return "0.2.0";
}

static void
bar_configure(GowlModule *mod, gpointer config)
{
	GowlModuleBar *self;
	GHashTable *settings;
	GowlBarInstance *bar;
	GowlBarPosition pos;
	const gchar *val;
	gint i;

	self = GOWL_MODULE_BAR(mod);

	if (config == NULL)
		return;

	settings = (GHashTable *)config;

	/* Dispatch to a specific slot based on the optional "position"
	   key.  Missing / unknown = top, so a single-bar config keeps
	   targeting the same slot it always has. */
	val = (const gchar *)g_hash_table_lookup(settings, "position");
	pos = bar_position_from_string(val);
	bar = &self->bars[pos];
	/* Any configure call marks the slot as "in play". visible stays
	   at its current value so repeat configures don't un-hide a
	   slot the user explicitly hid. */
	bar->enabled = TRUE;

	val = (const gchar *)g_hash_table_lookup(settings, "height");
	if (val != NULL)
		bar->bar_height = (gint)g_ascii_strtoll(val, NULL, 10);

	/* Visibility toggle.  Accepts "true"/"false"/"t"/"nil"/"1"/"0".
	   Hiding releases scene surfaces and triggers a fresh
	   arrangelayers so the tiling area grows back; the slot's
	   configuration is preserved across hide/show cycles. */
	val = (const gchar *)g_hash_table_lookup(settings, "visible");
	if (val != NULL) {
		gboolean want;
		want = (g_ascii_strcasecmp(val, "true") == 0
		    || g_ascii_strcasecmp(val, "t")    == 0
		    || g_ascii_strcasecmp(val, "1")    == 0
		    || g_ascii_strcasecmp(val, "yes")  == 0);
		bar->visible = want;
		if (!want)
			bar_instance_destroy_surfaces(bar);
	}

	val = (const gchar *)g_hash_table_lookup(settings, "bg-color");
	if (val != NULL)
		parse_hex_color(val, bar->bg_color);

	val = (const gchar *)g_hash_table_lookup(settings, "fg-color");
	if (val != NULL)
		parse_hex_color(val, bar->fg_color);

	/* Tag indicator: visibility toggle + per-state colours. */
	val = (const gchar *)g_hash_table_lookup(settings, "show-tags");
	if (val != NULL)
		bar->show_tags = (g_ascii_strcasecmp(val, "true") == 0
		    || g_ascii_strcasecmp(val, "t")   == 0
		    || g_ascii_strcasecmp(val, "1")   == 0
		    || g_ascii_strcasecmp(val, "yes") == 0);

	val = (const gchar *)g_hash_table_lookup(settings, "tag-active-bg");
	if (val != NULL)
		parse_hex_color(val, bar->tag_active_bg);
	val = (const gchar *)g_hash_table_lookup(settings, "tag-active-fg");
	if (val != NULL)
		parse_hex_color(val, bar->tag_active_fg);
	val = (const gchar *)g_hash_table_lookup(settings, "tag-occupied-fg");
	if (val != NULL)
		parse_hex_color(val, bar->tag_occupied_fg);
	val = (const gchar *)g_hash_table_lookup(settings, "tag-urgent-bg");
	if (val != NULL)
		parse_hex_color(val, bar->tag_urgent_bg);
	val = (const gchar *)g_hash_table_lookup(settings, "tag-urgent-fg");
	if (val != NULL)
		parse_hex_color(val, bar->tag_urgent_fg);
	val = (const gchar *)g_hash_table_lookup(settings, "tag-empty-fg");
	if (val != NULL)
		parse_hex_color(val, bar->tag_empty_fg);

	val = (const gchar *)g_hash_table_lookup(settings, "font");
	if (val != NULL) {
		g_free(bar->font_desc);
		bar->font_desc = g_strdup(val);
	}

	val = (const gchar *)g_hash_table_lookup(settings, "font-size");
	if (val != NULL) {
		gdouble sz = g_ascii_strtod(val, NULL);
		g_free(bar->font_desc);
		bar->font_desc = g_strdup_printf("monospace %.0f", sz);
	}

	val = (const gchar *)g_hash_table_lookup(settings, "title");
	if (val != NULL) {
		g_free(bar->custom_title);
		bar->custom_title = g_strdup(val);
	}

	/* Title colorization */
	val = (const gchar *)g_hash_table_lookup(settings, "title-delimiters");
	if (val != NULL) {
		g_free(bar->title_delimiters);
		bar->title_delimiters = g_strdup(val);
	}

	val = (const gchar *)g_hash_table_lookup(settings, "title-delimiter-color");
	if (val != NULL)
		parse_hex_color(val, bar->title_delimiter_color);

	val = (const gchar *)g_hash_table_lookup(settings, "title-palette");
	if (val != NULL) {
		gchar **colors = g_strsplit(val, " ", -1);
		gint ci;

		bar->title_palette_size = 0;
		for (ci = 0; colors[ci] != NULL && ci < 8; ci++) {
			if (colors[ci][0] == '\0')
				continue;
			parse_hex_color(colors[ci],
			                bar->title_palette[bar->title_palette_size]);
			bar->title_palette_size++;
		}
		g_strfreev(colors);
	}

	val = (const gchar *)g_hash_table_lookup(settings, "widgets");
	if (val != NULL)
		parse_widget_list(self, bar, val);

	/* Per-widget colors */
	for (i = 0; i < bar->n_widgets; i++) {
		const gchar *key = widget_color_key(bar->widgets[i].type);
		if (key != NULL) {
			val = (const gchar *)g_hash_table_lookup(settings, key);
			if (val != NULL) {
				parse_hex_color(val, bar->widgets[i].color);
				bar->widgets[i].has_color = TRUE;
			}
		}
	}

	/* CMD widget interval (global default; per-widget @N wins).
	   The key applies only to the slot being configured. */
	val = (const gchar *)g_hash_table_lookup(settings, "cmd-interval");
	if (val != NULL) {
		gint interval = (gint)g_ascii_strtoll(val, NULL, 10);
		if (interval > 0) {
			for (i = 0; i < bar->n_widgets; i++) {
				if (bar->widgets[i].type == BAR_WIDGET_CMD &&
				    !bar->widgets[i].interval_explicit)
					bar->widgets[i].output_interval = interval;
			}
		}
	}

	/* Widget data is shared across slots (Elisp-driven values like
	   todo). */
	{
		GHashTableIter iter;
		gpointer k, v;

		g_hash_table_iter_init(&iter, settings);
		while (g_hash_table_iter_next(&iter, &k, &v)) {
			const gchar *key = (const gchar *)k;
			if (strncmp(key, "widget-data-", 12) == 0) {
				g_hash_table_insert(self->widget_data,
				                    g_strdup(key + 12),
				                    g_strdup((const gchar *)v));
			}
		}
	}

	/* Re-adjust tick to match the new fastest widget interval. */
	if (self->tick_timer != NULL)
		wl_event_source_timer_update(self->tick_timer,
		                             bar_tick_ms(self));

	if (self->compositor != NULL) {
		/* Ask the compositor to re-run arrangelayers so the usable
		   area reflects the new bar height, then redraw. */
		GList *monitors =
			gowl_compositor_get_monitors(GOWL_COMPOSITOR(self->compositor));
		GList *l;
		for (l = monitors; l != NULL; l = l->next)
			gowl_compositor_arrangelayers(
				GOWL_COMPOSITOR(self->compositor),
				GOWL_MONITOR(l->data));
		bar_redraw_all(self);
	}
}

/* ----------------------------------------------------------------
 * GowlBarProvider
 * ---------------------------------------------------------------- */

static gint
bar_get_bar_height(GowlBarProvider *provider, gpointer monitor)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(provider);
	GowlBarInstance *top = &self->bars[GOWL_BAR_POSITION_TOP];
	(void)monitor;
	/* Back-compat scalar: report the TOP slot's height only.  The
	   insets API below reports both slots. */
	if (top->enabled && top->visible)
		return top->bar_height;
	return 0;
}

static void
bar_get_bar_insets_impl(GowlBarProvider *provider, gpointer monitor,
                        gint *top_out, gint *bottom_out)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(provider);
	GowlBarInstance *top = &self->bars[GOWL_BAR_POSITION_TOP];
	GowlBarInstance *bot = &self->bars[GOWL_BAR_POSITION_BOTTOM];
	gint t = 0, b = 0;

	(void)monitor;

	if (top->enabled && top->visible && top->bar_height > 0)
		t = top->bar_height;
	if (bot->enabled && bot->visible && bot->bar_height > 0)
		b = bot->bar_height;

	if (top_out    != NULL) *top_out    = t;
	if (bottom_out != NULL) *bottom_out = b;
}

static void
bar_render_bar(GowlBarProvider *provider, gpointer monitor)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(provider);
	(void)monitor;
	/* bar_redraw_all walks both slots and every monitor, creating
	   surfaces on demand if geometry changed. */
	bar_redraw_all(self);
}

/* Hit-test monitor-local (@x, @y) to the 0-based tag index whose
   indicator box contains it, or -1.  Computes the per-slot tag-bar
   heights (a slot contributes only when enabled, visible and drawing
   tags) and defers the geometry to gowl_bar_tag_hit(), which the unit
   tests exercise directly.  The padding (10) MUST match bar_render. */
static gint
bar_tag_at(GowlBarProvider *provider, gpointer monitor, gint x, gint y)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(provider);
	GowlMonitor *mon = (GowlMonitor *)monitor;
	GowlBarInstance *top = &self->bars[GOWL_BAR_POSITION_TOP];
	GowlBarInstance *bot = &self->bars[GOWL_BAR_POSITION_BOTTOM];
	gint mx, my, mw, mh;
	gint top_h = 0, bot_h = 0;

	if (mon == NULL)
		return -1;

	gowl_monitor_get_geometry(mon, &mx, &my, &mw, &mh);

	if (top->enabled && top->visible && top->show_tags &&
	    top->bar_height > 0)
		top_h = top->bar_height;
	if (bot->enabled && bot->visible && bot->show_tags &&
	    bot->bar_height > 0)
		bot_h = bot->bar_height;

	return gowl_bar_tag_hit(x, y, mh, top_h, bot_h, 10,
	                        bar_tag_count(self));
}

static void
bar_provider_iface_init(GowlBarProviderInterface *iface)
{
	iface->get_bar_height = bar_get_bar_height;
	iface->get_bar_insets = bar_get_bar_insets_impl;
	iface->render_bar     = bar_render_bar;
	iface->tag_at         = bar_tag_at;
}

/* ----------------------------------------------------------------
 * GowlStartupHandler
 * ---------------------------------------------------------------- */

static void
bar_on_startup(GowlStartupHandler *handler, gpointer compositor)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(handler);
	GowlCompositor *comp = GOWL_COMPOSITOR(compositor);
	GList *monitors, *l;
	struct wl_event_loop *loop;

	self->compositor = compositor;

	self->focus_handler_id =
		g_signal_connect(compositor, "focus-changed",
		                 G_CALLBACK(bar_on_focus_changed), self);
	self->client_added_id =
		g_signal_connect(compositor, "client-added",
		                 G_CALLBACK(bar_on_client_changed), self);
	self->client_removed_id =
		g_signal_connect(compositor, "client-removed",
		                 G_CALLBACK(bar_on_client_changed), self);

	monitors = gowl_compositor_get_monitors(comp);
	for (l = monitors; l != NULL; l = l->next) {
		GowlMonitor *mon = GOWL_MONITOR(l->data);
		gint bi;
		/* Skip disabled monitors (e.g. a lid-shut internal panel) so
		   their bars are not created off-screen / stacked over the
		   surviving output. */
		if (!gowl_monitor_get_enabled(mon))
			continue;
		for (bi = 0; bi < GOWL_BAR_POSITION_COUNT; bi++)
			bar_create_surface(self, &self->bars[bi], mon);
	}

	/* Tick timer for system data updates. Period adapts to the
	   fastest per-widget interval; 2s default, 1s floor. */
	loop = wl_display_get_event_loop(
		gowl_compositor_get_wl_display(comp));
	if (loop != NULL) {
		self->tick_timer = wl_event_loop_add_timer(loop,
			bar_tick, self);
		if (self->tick_timer != NULL)
			wl_event_source_timer_update(self->tick_timer,
			                             bar_tick_ms(self));
	}

	monitors = gowl_compositor_get_monitors(comp);
	for (l = monitors; l != NULL; l = l->next)
		gowl_compositor_arrangelayers(comp, GOWL_MONITOR(l->data));
}

static void
bar_startup_init(GowlStartupHandlerInterface *iface)
{
	iface->on_startup = bar_on_startup;
}

/* ----------------------------------------------------------------
 * GowlShutdownHandler
 * ---------------------------------------------------------------- */

static void
bar_on_shutdown(GowlShutdownHandler *handler, gpointer compositor)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(handler);

	(void)compositor;

	if (self->focus_handler_id != 0 && self->compositor != NULL) {
		g_signal_handler_disconnect(self->compositor,
		                            self->focus_handler_id);
		self->focus_handler_id = 0;
	}
	if (self->client_added_id != 0 && self->compositor != NULL) {
		g_signal_handler_disconnect(self->compositor,
		                            self->client_added_id);
		self->client_added_id = 0;
	}
	if (self->client_removed_id != 0 && self->compositor != NULL) {
		g_signal_handler_disconnect(self->compositor,
		                            self->client_removed_id);
		self->client_removed_id = 0;
	}

	if (self->tick_timer != NULL) {
		wl_event_source_remove(self->tick_timer);
		self->tick_timer = NULL;
	}

	bar_destroy_all(self);
	self->compositor = NULL;
}

static void
bar_shutdown_init(GowlShutdownHandlerInterface *iface)
{
	iface->on_shutdown = bar_on_shutdown;
}

/* ----------------------------------------------------------------
 * GObject lifecycle
 * ---------------------------------------------------------------- */

static void
gowl_module_bar_finalize(GObject *object)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(object);

	/* Drain in-flight subprocess workers before touching widgets --
	   they hold raw pointers into the widgets array. immediate=FALSE
	   lets queued items complete; wait=TRUE blocks until they all
	   finish. */
	if (self->worker_pool != NULL) {
		g_thread_pool_free(self->worker_pool, FALSE, TRUE);
		self->worker_pool = NULL;
	}

	{
		gint bi, i;
		for (bi = 0; bi < GOWL_BAR_POSITION_COUNT; bi++) {
			GowlBarInstance *bar = &self->bars[bi];
			for (i = 0; i < bar->n_widgets; i++) {
				g_free(bar->widgets[i].param);
				g_free(bar->widgets[i].cached_output);
			}
			g_free(bar->font_desc);
			g_free(bar->custom_title);
			g_free(bar->title_delimiters);
			if (bar->surfaces != NULL)
				g_hash_table_unref(bar->surfaces);
		}
	}
	g_free(self->temp_path);
	g_free(self->cached_hostname);
	g_free(self->cached_username);
	g_free(self->cached_keymap);
	if (self->widget_data != NULL)
		g_hash_table_unref(self->widget_data);
	g_mutex_clear(&self->output_mutex);

	G_OBJECT_CLASS(gowl_module_bar_parent_class)->finalize(object);
}

static void
gowl_module_bar_class_init(GowlModuleBarClass *klass)
{
	GObjectClass *obj_class;
	GowlModuleClass *mod_class;

	obj_class = G_OBJECT_CLASS(klass);
	obj_class->finalize = gowl_module_bar_finalize;

	mod_class = GOWL_MODULE_CLASS(klass);
	mod_class->activate        = bar_activate;
	mod_class->deactivate      = bar_deactivate;
	mod_class->get_name        = bar_get_name;
	mod_class->get_description = bar_get_description;
	mod_class->get_version     = bar_get_version;
	mod_class->configure       = bar_configure;
}

/* Populate a fresh #GowlBarInstance with defaults. */
static void
bar_instance_init_defaults(GowlBarInstance *bar, GowlBarPosition pos)
{
	memset(bar, 0, sizeof(*bar));

	bar->position    = pos;
	bar->enabled     = FALSE;
	bar->visible     = TRUE;
	bar->bar_height  = 28;

	/* Semi-transparent Catppuccin Mocha base */
	bar->bg_color[0] = 0.118;
	bar->bg_color[1] = 0.118;
	bar->bg_color[2] = 0.180;
	bar->bg_color[3] = 0.8;
	/* Catppuccin text */
	bar->fg_color[0] = 0.804;
	bar->fg_color[1] = 0.839;
	bar->fg_color[2] = 0.957;
	bar->fg_color[3] = 1.0;

	bar->font_desc          = g_strdup("monospace 13");
	bar->title_delimiters   = NULL;  /* disabled by default */
	bar->title_palette_size = 0;
	/* dim delimiter color default */
	bar->title_delimiter_color[0] = 0.45;
	bar->title_delimiter_color[1] = 0.46;
	bar->title_delimiter_color[2] = 0.50;
	bar->title_delimiter_color[3] = 1.0;

	/* Tag indicator: on by default, Catppuccin-ish palette. */
	bar->show_tags = TRUE;
	/* active fill: blue (#89b4fa) */
	bar->tag_active_bg[0] = 0.537; bar->tag_active_bg[1] = 0.706;
	bar->tag_active_bg[2] = 0.980; bar->tag_active_bg[3] = 1.0;
	/* active number: base/dark (#1e1e2e) */
	bar->tag_active_fg[0] = 0.118; bar->tag_active_fg[1] = 0.118;
	bar->tag_active_fg[2] = 0.180; bar->tag_active_fg[3] = 1.0;
	/* occupied (non-selected) number: text (#cdd6f4) */
	bar->tag_occupied_fg[0] = 0.804; bar->tag_occupied_fg[1] = 0.839;
	bar->tag_occupied_fg[2] = 0.957; bar->tag_occupied_fg[3] = 1.0;
	/* urgent fill: red (#f38ba8) */
	bar->tag_urgent_bg[0] = 0.953; bar->tag_urgent_bg[1] = 0.545;
	bar->tag_urgent_bg[2] = 0.659; bar->tag_urgent_bg[3] = 1.0;
	/* urgent number: base/dark */
	bar->tag_urgent_fg[0] = 0.118; bar->tag_urgent_fg[1] = 0.118;
	bar->tag_urgent_fg[2] = 0.180; bar->tag_urgent_fg[3] = 1.0;
	/* empty number: dim/overlay (#6c7086) */
	bar->tag_empty_fg[0] = 0.424; bar->tag_empty_fg[1] = 0.439;
	bar->tag_empty_fg[2] = 0.525; bar->tag_empty_fg[3] = 1.0;

	bar->surfaces  = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                        g_free, NULL);
	bar->custom_title = NULL;
	bar->n_widgets    = 0;
}

static void
gowl_module_bar_init(GowlModuleBar *self)
{
	GowlBarInstance *top;

	/* Both slots start with default styling; only TOP is enabled by
	   default so existing single-bar users get the same behavior. */
	bar_instance_init_defaults(&self->bars[GOWL_BAR_POSITION_TOP],
	                            GOWL_BAR_POSITION_TOP);
	bar_instance_init_defaults(&self->bars[GOWL_BAR_POSITION_BOTTOM],
	                            GOWL_BAR_POSITION_BOTTOM);

	/* Default top-bar widgets: cpu memory disk battery clock */
	top = &self->bars[GOWL_BAR_POSITION_TOP];
	top->enabled    = TRUE;
	top->n_widgets  = 5;
	top->widgets[0].type = BAR_WIDGET_CPU;
	top->widgets[1].type = BAR_WIDGET_MEMORY;
	top->widgets[2].type = BAR_WIDGET_DISK;
	top->widgets[3].type = BAR_WIDGET_BATTERY;
	top->widgets[4].type = BAR_WIDGET_CLOCK;

	self->compositor       = NULL;
	self->focus_handler_id = 0;
	self->client_added_id  = 0;
	self->client_removed_id = 0;
	self->tick_timer       = NULL;

	/* Zero cached data */
	self->prev_cpu_idle  = 0;
	self->prev_cpu_total = 0;
	self->cached_cpu_pct = 0;
	self->cpu_read_time  = 0;
	self->cached_mem_used_mb  = 0;
	self->cached_mem_total_mb = 0;
	self->mem_read_time  = 0;
	self->cached_disk_free_gb  = 0;
	self->cached_disk_total_gb = 0;
	self->disk_read_time = 0;
	self->cached_battery = -1;
	self->bat_read_time  = 0;

	/* New widget cached data */
	self->cached_load_1       = 0.0;
	self->load_read_time      = 0;
	self->cached_swap_used_mb  = 0;
	self->cached_swap_total_mb = 0;
	self->prev_net_rx          = 0;
	self->prev_net_tx          = 0;
	self->cached_net_rx_rate   = 0;
	self->cached_net_tx_rate   = 0;
	self->net_read_time        = 0;
	self->prev_io_rd           = 0;
	self->prev_io_wr           = 0;
	self->cached_io_rd_rate    = 0;
	self->cached_io_wr_rate    = 0;
	self->io_read_time         = 0;
	self->cached_temp_mc       = 0;
	self->temp_read_time       = 0;
	self->temp_path            = NULL;
	self->cached_hostname      = g_strdup(g_get_host_name());
	self->cached_username      = g_strdup(g_get_user_name());
	self->cached_keymap        = NULL;
	self->keymap_read_time     = 0;
	self->widget_data = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                          g_free, g_free);

	/* Async subprocess dispatch. Pool cap of 4 is plenty for a
	   handful of bar widgets. Exclusive=FALSE so threads are shared
	   with other glib thread pools. */
	g_mutex_init(&self->output_mutex);
	self->worker_pool = g_thread_pool_new(bar_worker_func, NULL,
	                                      4, FALSE, NULL);
}

/* ----------------------------------------------------------------
 * Shared-object entry point
 * ---------------------------------------------------------------- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_BAR;
}
