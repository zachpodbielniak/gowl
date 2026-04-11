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
	/* Per-widget output cache (for subprocess/IPC widgets) */
	gchar        *cached_output;
	time_t        output_read_time;
	gint          output_interval; /* seconds, default varies by type */
} BarWidget;

/* ----------------------------------------------------------------
 * Module type
 * ---------------------------------------------------------------- */

#define GOWL_TYPE_MODULE_BAR (gowl_module_bar_get_type())
G_DECLARE_FINAL_TYPE(GowlModuleBar, gowl_module_bar,
                     GOWL, MODULE_BAR, GowlModule)

typedef struct {
	struct wlr_scene_buffer *scene_buf;
	gint width;
	gint height;
	gint mon_x;
	gint mon_y;
} BarSurface;

struct _GowlModuleBar {
	GowlModule parent_instance;

	/* Config */
	gint     bar_height;
	gdouble  bg_color[4];
	gdouble  fg_color[4];
	gchar   *font_desc;

	/* Title colorization */
	gchar   *title_delimiters;            /* chars that split segments */
	gdouble  title_delimiter_color[4];    /* color for delimiter chars */
	gdouble  title_palette[8][4];         /* segment color cycle */
	gint     title_palette_size;          /* 0 = disabled, use fg_color */

	/* Widgets */
	BarWidget widgets[BAR_MAX_WIDGETS];
	gint      n_widgets;

	/* State */
	GHashTable *surfaces;
	gpointer    compositor;
	gchar      *custom_title;
	gulong      focus_handler_id;
	gulong      client_added_id;
	gulong      client_removed_id;
	struct wl_event_source *tick_timer;

	/* Cached system data */
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

	/* Widget data (Elisp-driven values) */
	GHashTable *widget_data;
};

static void bar_provider_iface_init(GowlBarProviderInterface *iface);
static void bar_startup_init(GowlStartupHandlerInterface *iface);
static void bar_shutdown_init(GowlShutdownHandlerInterface *iface);

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
read_gpu(BarWidget *w)
{
	FILE *f;
	gint pct;
	time_t now;

	now = time(NULL);
	if (now - w->output_read_time < 5)
		return;
	w->output_read_time = now;

	/* AMD amdgpu sysfs */
	f = fopen("/sys/class/drm/card0/device/gpu_busy_percent", "r");
	if (f == NULL)
		f = fopen("/sys/class/drm/card1/device/gpu_busy_percent", "r");
	if (f == NULL) {
		g_free(w->cached_output);
		w->cached_output = NULL;
		return;
	}

	if (fscanf(f, "%d", &pct) == 1) {
		g_free(w->cached_output);
		w->cached_output = g_strdup_printf("GPU %d%%", pct);
	}
	fclose(f);
}

static void
read_wifi(BarWidget *w)
{
	FILE *f;
	char line[512];
	time_t now;

	now = time(NULL);
	if (now - w->output_read_time < 10)
		return;
	w->output_read_time = now;

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
			g_free(w->cached_output);
			w->cached_output = g_strdup_printf("WiFi %ddBm",
			                                   (gint)level);
		}
	}
	fclose(f);
}

static void
read_ip(BarWidget *w)
{
	struct ifaddrs *ifaddr, *ifa;
	const gchar *target_iface;
	time_t now;

	now = time(NULL);
	if (now - w->output_read_time < 30)
		return;
	w->output_read_time = now;

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
		g_free(w->cached_output);
		w->cached_output = g_strdup(host);
		break;
	}

	freeifaddrs(ifaddr);
}

static void
read_vpn(BarWidget *w)
{
	time_t now;

	now = time(NULL);
	if (now - w->output_read_time < 10)
		return;
	w->output_read_time = now;

	g_free(w->cached_output);
	if (g_file_test("/sys/class/net/tun0", G_FILE_TEST_IS_DIR) ||
	    g_file_test("/sys/class/net/wg0", G_FILE_TEST_IS_DIR))
		w->cached_output = g_strdup("VPN \xe2\x9c\x93");
	else
		w->cached_output = g_strdup("VPN \xe2\x9c\x97");
}

static void
read_volume(BarWidget *w)
{
	gchar *out = NULL;
	time_t now;

	now = time(NULL);
	if (now - w->output_read_time < 2)
		return;
	w->output_read_time = now;

	if (g_spawn_command_line_sync(
	        "wpctl get-volume @DEFAULT_AUDIO_SINK@",
	        &out, NULL, NULL, NULL) && out != NULL) {
		gdouble vol;
		gboolean muted;

		muted = (strstr(out, "[MUTED]") != NULL);
		g_free(w->cached_output);

		if (muted) {
			w->cached_output = g_strdup("VOL MUTE");
		} else if (sscanf(out, "Volume: %lf", &vol) == 1) {
			w->cached_output = g_strdup_printf("VOL %d%%",
			                                   (gint)(vol * 100));
		} else {
			w->cached_output = g_strdup("VOL ?");
		}
	}
	g_free(out);
}

static void
read_media(BarWidget *w)
{
	gchar *out = NULL;
	time_t now;

	now = time(NULL);
	if (now - w->output_read_time < 2)
		return;
	w->output_read_time = now;

	/* Use playerctl for MPRIS — simpler than raw D-Bus from a module */
	if (g_spawn_command_line_sync(
	        "playerctl metadata --format '{{artist}} - {{title}}'",
	        &out, NULL, NULL, NULL) && out != NULL) {
		g_strstrip(out);
		if (out[0] != '\0' && strcmp(out, " - ") != 0) {
			g_free(w->cached_output);
			/* Truncate to ~40 chars */
			if (strlen(out) > 40)
				out[40] = '\0';
			w->cached_output = g_strdup_printf("\xe2\x99\xab %s", out);
		} else {
			g_free(w->cached_output);
			w->cached_output = NULL;
		}
	}
	g_free(out);
}

static void
read_git(GowlModuleBar *self, BarWidget *w)
{
	GowlClient *focused;
	GowlProcessInfo *info;
	gchar head_path[PATH_MAX];
	gchar dir[PATH_MAX];
	FILE *f;
	char ref_line[512];
	gchar *out = NULL;
	gchar *branch;
	gint dirty;
	time_t now;

	now = time(NULL);
	if (now - w->output_read_time < 5)
		return;
	w->output_read_time = now;

	if (self->compositor == NULL)
		return;

	focused = gowl_compositor_get_focused_client(
	              GOWL_COMPOSITOR(self->compositor));
	if (focused == NULL)
		return;

	info = gowl_client_get_process_info(focused);
	if (info == NULL || info->cwd == NULL) {
		if (info != NULL) gowl_process_info_free(info);
		return;
	}

	/* Walk up from CWD to find .git/ */
	g_strlcpy(dir, info->cwd, sizeof(dir));
	gowl_process_info_free(info);

	while (dir[0] != '\0') {
		g_snprintf(head_path, sizeof(head_path), "%s/.git/HEAD", dir);
		if (g_file_test(head_path, G_FILE_TEST_EXISTS))
			break;
		/* Go up one level */
		{
			gchar *slash = strrchr(dir, '/');
			if (slash == NULL || slash == dir) {
				dir[0] = '\0';
				break;
			}
			*slash = '\0';
		}
	}

	if (dir[0] == '\0') {
		g_free(w->cached_output);
		w->cached_output = NULL;
		return;
	}

	/* Read branch from .git/HEAD */
	f = fopen(head_path, "r");
	if (f == NULL) {
		g_free(w->cached_output);
		w->cached_output = NULL;
		return;
	}
	if (fgets(ref_line, sizeof(ref_line), f) == NULL) {
		fclose(f);
		g_free(w->cached_output);
		w->cached_output = NULL;
		return;
	}
	fclose(f);

	g_strstrip(ref_line);
	if (strncmp(ref_line, "ref: refs/heads/", 16) == 0)
		branch = ref_line + 16;
	else
		branch = ref_line; /* detached HEAD */

	/* Check dirty count */
	dirty = 0;
	{
		gchar *cmd = g_strdup_printf(
			"git -C '%s' status --porcelain 2>/dev/null | wc -l",
			dir);
		if (g_spawn_command_line_sync(cmd, &out, NULL, NULL, NULL)
		    && out != NULL) {
			dirty = atoi(out);
		}
		g_free(cmd);
		g_free(out);
	}

	g_free(w->cached_output);
	if (dirty > 0)
		w->cached_output = g_strdup_printf("%s*%d", branch, dirty);
	else
		w->cached_output = g_strdup(branch);
}

static void
read_podman(BarWidget *w)
{
	gchar *out = NULL;
	time_t now;

	now = time(NULL);
	if (now - w->output_read_time < 30)
		return;
	w->output_read_time = now;

	if (g_spawn_command_line_sync("podman ps -q",
	        &out, NULL, NULL, NULL) && out != NULL) {
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
		g_free(w->cached_output);
		w->cached_output = g_strdup_printf("POD %d", count);
	}
	g_free(out);
}

static void
read_weather(BarWidget *w)
{
	gchar *out = NULL;
	gchar *cmd;
	const gchar *loc;
	time_t now;

	now = time(NULL);
	if (now - w->output_read_time < 900) /* 15 min cache */
		return;
	w->output_read_time = now;

	loc = (w->param != NULL) ? w->param : "";
	cmd = g_strdup_printf("curl -sf 'wttr.in/%s?format=%%c+%%t' 2>/dev/null",
	                      loc);

	if (g_spawn_command_line_sync(cmd, &out, NULL, NULL, NULL)
	    && out != NULL) {
		g_strstrip(out);
		if (out[0] != '\0') {
			g_free(w->cached_output);
			w->cached_output = g_strdup(out);
		}
	}

	g_free(cmd);
	g_free(out);
}

static void
read_cmd(BarWidget *w)
{
	gchar *out = NULL;
	gchar *nl;
	time_t now;
	gint interval;

	interval = (w->output_interval > 0) ? w->output_interval : 10;
	now = time(NULL);
	if (now - w->output_read_time < interval)
		return;
	w->output_read_time = now;

	if (w->param == NULL || w->param[0] == '\0')
		return;

	if (g_spawn_command_line_sync(w->param,
	        &out, NULL, NULL, NULL) && out != NULL) {
		/* Take only the first line */
		nl = strchr(out, '\n');
		if (nl != NULL)
			*nl = '\0';
		/* Trim trailing whitespace */
		g_strchomp(out);
		g_free(w->cached_output);
		w->cached_output = g_strdup(out);
	}
	g_free(out);
}

static void
read_all_data(GowlModuleBar *self)
{
	gint i;

	for (i = 0; i < self->n_widgets; i++) {
		switch (self->widgets[i].type) {
		case BAR_WIDGET_CPU:     read_cpu(self);                 break;
		case BAR_WIDGET_MEMORY:  read_memory(self);              break;
		case BAR_WIDGET_DISK:    read_disk(self);                break;
		case BAR_WIDGET_BATTERY: read_battery(self);             break;
		case BAR_WIDGET_LOAD:    read_load(self);                break;
		case BAR_WIDGET_SWAP:    read_swap(self);                break;
		case BAR_WIDGET_NET:     read_net(self, &self->widgets[i]); break;
		case BAR_WIDGET_IO:      read_io(self, &self->widgets[i]); break;
		case BAR_WIDGET_TEMP:    read_temp(self);                break;
		case BAR_WIDGET_GPU:     read_gpu(&self->widgets[i]);    break;
		case BAR_WIDGET_WIFI:    read_wifi(&self->widgets[i]);   break;
		case BAR_WIDGET_IP:      read_ip(&self->widgets[i]);     break;
		case BAR_WIDGET_VPN:     read_vpn(&self->widgets[i]);    break;
		case BAR_WIDGET_VOLUME:  read_volume(&self->widgets[i]); break;
		case BAR_WIDGET_MEDIA:   read_media(&self->widgets[i]);  break;
		case BAR_WIDGET_GIT:     read_git(self, &self->widgets[i]); break;
		case BAR_WIDGET_PODMAN:  read_podman(&self->widgets[i]); break;
		case BAR_WIDGET_WEATHER: read_weather(&self->widgets[i]); break;
		case BAR_WIDGET_CMD:     read_cmd(&self->widgets[i]);    break;
		default: break;
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
		/* These use per-widget cached_output */
		if (w->cached_output != NULL)
			snprintf(buf, bufsz, "%s", w->cached_output);
		else
			buf[0] = '\0';
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
parse_widget_list(GowlModuleBar *self, const gchar *spec)
{
	gchar **parts;
	gint i;

	parts = g_strsplit(spec, " ", -1);
	self->n_widgets = 0;

	/* Free old widget params */
	for (i = 0; i < self->n_widgets; i++)
		g_free(self->widgets[i].param);

	self->n_widgets = 0;

	for (i = 0; parts[i] != NULL && self->n_widgets < BAR_MAX_WIDGETS; i++) {
		BarWidgetType t;
		gchar *name;
		gchar *param;
		gchar *colon;

		if (parts[i][0] == '\0')
			continue;

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

		self->widgets[self->n_widgets].type = t;
		self->widgets[self->n_widgets].has_color = FALSE;
		self->widgets[self->n_widgets].param =
			(param != NULL && param[0] != '\0') ? g_strdup(param) : NULL;
		self->widgets[self->n_widgets].cached_output = NULL;
		self->widgets[self->n_widgets].output_read_time = 0;
		/* Default refresh intervals per type */
		switch (t) {
		case BAR_WIDGET_CMD:     self->widgets[self->n_widgets].output_interval = 10; break;
		case BAR_WIDGET_WEATHER: self->widgets[self->n_widgets].output_interval = 900; break;
		case BAR_WIDGET_PODMAN:  self->widgets[self->n_widgets].output_interval = 30; break;
		default:                 self->widgets[self->n_widgets].output_interval = 0; break;
		}
		self->n_widgets++;
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

static BarBuffer *
bar_render(GowlModuleBar *self, gint width, gint height)
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
	gint i;
	char wtext[128];
	gint separator_w;

	cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	cr = cairo_create(cs);

	/* Background */
	cairo_set_source_rgba(cr, self->bg_color[0], self->bg_color[1],
	                      self->bg_color[2], self->bg_color[3]);
	cairo_paint(cr);

	/* Set up pango */
	layout = pango_cairo_create_layout(cr);
	font = pango_font_description_from_string(self->font_desc);
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

	/* Left: title */
	if (self->custom_title != NULL && self->custom_title[0] != '\0') {
		title = self->custom_title;
	} else {
		focused = (self->compositor != NULL) ?
			gowl_compositor_get_focused_client(
				GOWL_COMPOSITOR(self->compositor)) : NULL;
		title = (focused != NULL) ? gowl_client_get_title(focused) : "cmacs";
		if (title == NULL)
			title = "cmacs";
	}

	pango_layout_set_text(layout, title, -1);
	pango_layout_set_width(layout, (width / 2 - padding * 2) * PANGO_SCALE);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

	/* Colorize title: split on delimiters, cycle palette colors */
	if (self->title_palette_size > 0 && self->title_delimiters != NULL) {
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

			if (strchr(self->title_delimiters, title[pos]) != NULL) {
				/* Delimiter character */
				c = self->title_delimiter_color;
			} else {
				/* Regular character — find end of segment */
				gint start = pos;
				while (pos < len &&
				       strchr(self->title_delimiters, title[pos]) == NULL)
					pos++;
				c = self->title_palette[seg_idx % self->title_palette_size];
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
		cairo_set_source_rgba(cr, self->fg_color[0], self->fg_color[1],
		                      self->fg_color[2], self->fg_color[3]);
	}

	cairo_move_to(cr, padding, text_y);
	/* When using pango attributes, set cairo source to white so
	 * the attribute colors are used directly. */
	if (self->title_palette_size > 0)
		cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
	pango_cairo_show_layout(cr, layout);
	pango_layout_set_attributes(layout, NULL);

	/* Right: widgets, rendered right-to-left */
	right_x = width - padding;
	pango_layout_set_width(layout, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);

	/* Read fresh system data */
	read_all_data(self);

	for (i = self->n_widgets - 1; i >= 0; i--) {
		const gdouble *c;

		widget_text(self, &self->widgets[i], wtext, sizeof(wtext));
		if (wtext[0] == '\0')
			continue;

		/* Use per-widget color if set, otherwise fg_color */
		c = self->widgets[i].has_color ? self->widgets[i].color
		                               : self->fg_color;

		/* CMD widgets: ANSI color rendering */
		if (self->widgets[i].type == BAR_WIDGET_CMD &&
		    self->widgets[i].cached_output != NULL &&
		    strchr(self->widgets[i].cached_output, '\033') != NULL) {
			GString *plain;
			PangoAttrList *cmd_attrs;

			plain = g_string_new(NULL);
			cmd_attrs = render_ansi_text(self->widgets[i].cached_output,
			                             c, plain);
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

static void
bar_create_surface(GowlModuleBar *self, GowlMonitor *monitor)
{
	GowlCompositor *comp;
	struct wlr_scene_tree *top_layer;
	BarSurface *surface;
	BarBuffer *buf;
	const gchar *name;
	gint mon_x, mon_y, mon_w, mon_h;

	comp = GOWL_COMPOSITOR(self->compositor);
	name = gowl_monitor_get_name(monitor);
	gowl_monitor_get_geometry(monitor, &mon_x, &mon_y, &mon_w, &mon_h);

	if (mon_w <= 0 || mon_h <= 0)
		return;

	/* Remove existing surface for this monitor */
	surface = (BarSurface *)g_hash_table_lookup(self->surfaces, name);
	if (surface != NULL) {
		if (surface->scene_buf != NULL)
			wlr_scene_node_destroy(&surface->scene_buf->node);
		g_hash_table_remove(self->surfaces, name);
		g_free(surface);
	}

	top_layer = gowl_compositor_get_scene_layer(comp,
	                                            GOWL_SCENE_LAYER_TOP);
	if (top_layer == NULL)
		return;

	buf = bar_render(self, mon_w, self->bar_height);

	surface = g_new0(BarSurface, 1);
	surface->scene_buf = wlr_scene_buffer_create(top_layer, &buf->base);
	surface->width  = mon_w;
	surface->height = self->bar_height;
	surface->mon_x  = mon_x;
	surface->mon_y  = mon_y;

	wlr_scene_node_set_position(&surface->scene_buf->node, mon_x, mon_y);
	wlr_buffer_drop(&buf->base);

	g_hash_table_insert(self->surfaces, g_strdup(name), surface);
}

static void
bar_redraw_all(GowlModuleBar *self)
{
	GowlCompositor *comp;
	GList *monitors, *l;

	if (self->compositor == NULL)
		return;

	comp = GOWL_COMPOSITOR(self->compositor);
	monitors = gowl_compositor_get_monitors(comp);

	for (l = monitors; l != NULL; l = l->next) {
		GowlMonitor *mon = GOWL_MONITOR(l->data);
		const gchar *name = gowl_monitor_get_name(mon);
		BarSurface *surface;
		BarBuffer *buf;

		surface = (BarSurface *)g_hash_table_lookup(self->surfaces, name);
		if (surface == NULL || surface->scene_buf == NULL) {
			bar_create_surface(self, mon);
			continue;
		}

		buf = bar_render(self, surface->width, surface->height);
		wlr_scene_buffer_set_buffer(surface->scene_buf, &buf->base);
		wlr_buffer_drop(&buf->base);
	}
}

static void
bar_destroy_all(GowlModuleBar *self)
{
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init(&iter, self->surfaces);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		BarSurface *surface = (BarSurface *)value;
		if (surface->scene_buf != NULL)
			wlr_scene_node_destroy(&surface->scene_buf->node);
		g_free(surface);
	}
	g_hash_table_remove_all(self->surfaces);
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

static int
bar_tick(void *data)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(data);
	bar_redraw_all(self);

	if (self->tick_timer != NULL)
		wl_event_source_timer_update(self->tick_timer, 2 * 1000);

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
	const gchar *val;
	gint i;

	self = GOWL_MODULE_BAR(mod);

	if (config == NULL)
		return;

	settings = (GHashTable *)config;

	val = (const gchar *)g_hash_table_lookup(settings, "height");
	if (val != NULL)
		self->bar_height = (gint)g_ascii_strtoll(val, NULL, 10);

	val = (const gchar *)g_hash_table_lookup(settings, "bg-color");
	if (val != NULL)
		parse_hex_color(val, self->bg_color);

	val = (const gchar *)g_hash_table_lookup(settings, "fg-color");
	if (val != NULL)
		parse_hex_color(val, self->fg_color);

	val = (const gchar *)g_hash_table_lookup(settings, "font");
	if (val != NULL) {
		g_free(self->font_desc);
		self->font_desc = g_strdup(val);
	}

	val = (const gchar *)g_hash_table_lookup(settings, "font-size");
	if (val != NULL) {
		gdouble sz = g_ascii_strtod(val, NULL);
		g_free(self->font_desc);
		self->font_desc = g_strdup_printf("monospace %.0f", sz);
	}

	val = (const gchar *)g_hash_table_lookup(settings, "title");
	if (val != NULL) {
		g_free(self->custom_title);
		self->custom_title = g_strdup(val);
	}

	/* Title colorization */
	val = (const gchar *)g_hash_table_lookup(settings, "title-delimiters");
	if (val != NULL) {
		g_free(self->title_delimiters);
		self->title_delimiters = g_strdup(val);
	}

	val = (const gchar *)g_hash_table_lookup(settings, "title-delimiter-color");
	if (val != NULL)
		parse_hex_color(val, self->title_delimiter_color);

	val = (const gchar *)g_hash_table_lookup(settings, "title-palette");
	if (val != NULL) {
		gchar **colors = g_strsplit(val, " ", -1);
		gint ci;

		self->title_palette_size = 0;
		for (ci = 0; colors[ci] != NULL && ci < 8; ci++) {
			if (colors[ci][0] == '\0')
				continue;
			parse_hex_color(colors[ci],
			                self->title_palette[self->title_palette_size]);
			self->title_palette_size++;
		}
		g_strfreev(colors);
	}

	val = (const gchar *)g_hash_table_lookup(settings, "widgets");
	if (val != NULL)
		parse_widget_list(self, val);

	/* Per-widget colors */
	for (i = 0; i < self->n_widgets; i++) {
		const gchar *key = widget_color_key(self->widgets[i].type);
		if (key != NULL) {
			val = (const gchar *)g_hash_table_lookup(settings, key);
			if (val != NULL) {
				parse_hex_color(val, self->widgets[i].color);
				self->widgets[i].has_color = TRUE;
			}
		}
	}

	/* CMD widget interval */
	val = (const gchar *)g_hash_table_lookup(settings, "cmd-interval");
	if (val != NULL) {
		gint interval = (gint)g_ascii_strtoll(val, NULL, 10);
		if (interval > 0) {
			for (i = 0; i < self->n_widgets; i++) {
				if (self->widgets[i].type == BAR_WIDGET_CMD)
					self->widgets[i].output_interval = interval;
			}
		}
	}

	/* Widget data (for Elisp-driven widgets like todo) */
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

	if (self->compositor != NULL)
		bar_redraw_all(self);
}

/* ----------------------------------------------------------------
 * GowlBarProvider
 * ---------------------------------------------------------------- */

static gint
bar_get_bar_height(GowlBarProvider *provider, gpointer monitor)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(provider);
	(void)monitor;
	return self->bar_height;
}

static void
bar_render_bar(GowlBarProvider *provider, gpointer monitor)
{
	GowlModuleBar *self = GOWL_MODULE_BAR(provider);

	if (monitor != NULL) {
		GowlMonitor *mon = GOWL_MONITOR(monitor);
		const gchar *name = gowl_monitor_get_name(mon);
		gint mon_x, mon_y, mon_w, mon_h;
		BarSurface *surface;

		gowl_monitor_get_geometry(mon, &mon_x, &mon_y, &mon_w, &mon_h);
		surface = (BarSurface *)g_hash_table_lookup(self->surfaces, name);

		if (surface == NULL || surface->width != mon_w
		    || surface->mon_x != mon_x || surface->mon_y != mon_y) {
			bar_create_surface(self, mon);
			return;
		}
	}

	bar_redraw_all(self);
}

static void
bar_provider_iface_init(GowlBarProviderInterface *iface)
{
	iface->get_bar_height = bar_get_bar_height;
	iface->render_bar     = bar_render_bar;
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
	for (l = monitors; l != NULL; l = l->next)
		bar_create_surface(self, GOWL_MONITOR(l->data));

	/* 2-second tick timer for system data updates */
	loop = wl_display_get_event_loop(
		gowl_compositor_get_wl_display(comp));
	if (loop != NULL) {
		self->tick_timer = wl_event_loop_add_timer(loop,
			bar_tick, self);
		if (self->tick_timer != NULL)
			wl_event_source_timer_update(self->tick_timer, 2 * 1000);
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

	{
		gint i;
		for (i = 0; i < self->n_widgets; i++) {
			g_free(self->widgets[i].param);
			g_free(self->widgets[i].cached_output);
		}
	}
	g_free(self->font_desc);
	g_free(self->custom_title);
	g_free(self->title_delimiters);
	g_free(self->temp_path);
	g_free(self->cached_hostname);
	g_free(self->cached_username);
	g_free(self->cached_keymap);
	if (self->widget_data != NULL)
		g_hash_table_unref(self->widget_data);
	g_hash_table_unref(self->surfaces);

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

static void
gowl_module_bar_init(GowlModuleBar *self)
{
	self->bar_height = 28;
	/* Semi-transparent Catppuccin Mocha base */
	self->bg_color[0] = 0.118;
	self->bg_color[1] = 0.118;
	self->bg_color[2] = 0.180;
	self->bg_color[3] = 0.8;
	/* Catppuccin text */
	self->fg_color[0] = 0.804;
	self->fg_color[1] = 0.839;
	self->fg_color[2] = 0.957;
	self->fg_color[3] = 1.0;

	self->font_desc = g_strdup("monospace 13");
	self->title_delimiters   = NULL;  /* disabled by default */
	self->title_palette_size = 0;
	/* dim delimiter color default */
	self->title_delimiter_color[0] = 0.45;
	self->title_delimiter_color[1] = 0.46;
	self->title_delimiter_color[2] = 0.50;
	self->title_delimiter_color[3] = 1.0;

	self->surfaces  = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                        g_free, NULL);
	self->compositor       = NULL;
	self->custom_title     = NULL;
	self->focus_handler_id = 0;
	self->client_added_id  = 0;
	self->client_removed_id = 0;
	self->tick_timer       = NULL;

	/* Default widgets: cpu memory disk battery clock */
	self->n_widgets = 5;
	self->widgets[0].type = BAR_WIDGET_CPU;
	self->widgets[0].has_color = FALSE;
	self->widgets[0].param = NULL;
	self->widgets[1].type = BAR_WIDGET_MEMORY;
	self->widgets[1].has_color = FALSE;
	self->widgets[1].param = NULL;
	self->widgets[2].type = BAR_WIDGET_DISK;
	self->widgets[2].has_color = FALSE;
	self->widgets[2].param = NULL;
	self->widgets[3].type = BAR_WIDGET_BATTERY;
	self->widgets[3].has_color = FALSE;
	self->widgets[3].param = NULL;
	self->widgets[4].type = BAR_WIDGET_CLOCK;
	self->widgets[4].has_color = FALSE;
	self->widgets[4].param = NULL;

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
}

/* ----------------------------------------------------------------
 * Shared-object entry point
 * ---------------------------------------------------------------- */

G_MODULE_EXPORT GType
gowl_module_register(void)
{
	return GOWL_TYPE_MODULE_BAR;
}
