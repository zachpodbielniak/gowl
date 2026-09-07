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

#define _GNU_SOURCE
#include "bar-sysinfo.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>

/**
 * SECTION:bar-sysinfo
 * @title: BarSysinfo
 * @short_description: the shared `/proc' reader behind the system plugins
 *
 * Every reading here comes from a small kernel file, never a
 * subprocess, so the whole object can be refreshed on the compositor's
 * dispatch thread without risking the deadlock that a blocking spawn
 * there would cause.  Anything that needs a process --- `wpctl',
 * `podman', `tailscale' --- belongs in a plugin's async poll, not in
 * this file.
 */

typedef struct {
	gdouble values[BAR_SYSINFO_HISTORY];
	guint   count;   /* how many slots are filled, capped at the size */
	guint   head;    /* next slot to write */
} SampleRing;

struct _BarSysinfo {
	/* CPU */
	glong   prev_cpu_idle;
	glong   prev_cpu_total;
	gint    cpu_percent;
	gint    cpu_cores;
	time_t  cpu_time;

	/* Load */
	gdouble load1, load5, load15;
	time_t  load_time;

	/* Memory and swap, from one /proc/meminfo pass */
	glong   mem_total_mb, mem_used_mb, mem_avail_mb;
	glong   swap_total_mb, swap_used_mb;
	time_t  mem_time;

	/* Battery */
	gint     battery_percent;
	gboolean battery_charging;
	gint     battery_minutes;
	time_t   battery_time;

	/* Thermal and GPU */
	gchar  *temp_path;
	gint    temp_mc;
	time_t  temp_time;
	gint    gpu_percent;
	time_t  gpu_time;

	/* Network rates.  Keyed on the interface last asked about, so
	   switching the widget's interface restarts the delta rather than
	   reporting one enormous spike. */
	gchar  *net_iface;
	glong   prev_net_rx, prev_net_tx;
	glong   net_rx_rate, net_tx_rate;
	time_t  net_time;

	/* Block IO */
	gchar  *io_device;
	glong   prev_io_rd, prev_io_wr;
	glong   io_rd_rate, io_wr_rate;
	time_t  io_time;

	/* Wireless */
	gchar   *wifi_iface;
	gint     wifi_dbm;
	gdouble  wifi_quality;
	gboolean wifi_up;
	time_t   wifi_time;

	/* Static */
	gchar  *hostname;
	gchar  *username;

	/* Sparkline history */
	SampleRing cpu_history;
	SampleRing mem_history;
	SampleRing net_history;
	glong      net_peak;

	/* Scratch buffer returned by the history getter, so the caller
	   sees oldest-to-newest without the ring's wrap. */
	gdouble    history_out[BAR_SYSINFO_HISTORY];
};

/* ----------------------------------------------------------------
 * Sample rings
 * ---------------------------------------------------------------- */

static void
ring_push(SampleRing *ring, gdouble value)
{
	if (value < 0.0)
		value = 0.0;
	if (value > 1.0)
		value = 1.0;

	ring->values[ring->head] = value;
	ring->head = (ring->head + 1) % BAR_SYSINFO_HISTORY;
	if (ring->count < BAR_SYSINFO_HISTORY)
		ring->count++;
}

/* Copy the ring out in chronological order. */
static guint
ring_read(const SampleRing *ring, gdouble *out)
{
	guint i, start;

	if (ring->count == 0)
		return 0;

	start = (ring->count == BAR_SYSINFO_HISTORY)
		? ring->head
		: 0;
	for (i = 0; i < ring->count; i++)
		out[i] = ring->values[(start + i) % BAR_SYSINFO_HISTORY];
	return ring->count;
}

/* ----------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------- */

/**
 * bar_sysinfo_new:
 *
 * Returns: (transfer full): a new reader
 */
BarSysinfo *
bar_sysinfo_new(void)
{
	BarSysinfo *self;

	self = g_new0(BarSysinfo, 1);
	self->battery_percent = -1;
	self->battery_minutes = -1;
	self->gpu_percent     = -1;
	self->wifi_dbm        = 0;
	self->hostname        = g_strdup(g_get_host_name());
	self->username        = g_strdup(g_get_user_name());
	self->cpu_cores       = (gint)g_get_num_processors();
	return self;
}

/**
 * bar_sysinfo_free:
 * @self: (nullable) (transfer full): a reader
 */
void
bar_sysinfo_free(BarSysinfo *self)
{
	if (self == NULL)
		return;
	g_free(self->temp_path);
	g_free(self->net_iface);
	g_free(self->io_device);
	g_free(self->wifi_iface);
	g_free(self->hostname);
	g_free(self->username);
	g_free(self);
}

/* ----------------------------------------------------------------
 * Readers
 * ---------------------------------------------------------------- */

static void
read_cpu(BarSysinfo *self, time_t now)
{
	FILE *f;
	glong user, nice, sys, idle, iowait, irq, softirq, steal;
	glong total, diff_idle, diff_total;

	if (now - self->cpu_time < 2)
		return;
	self->cpu_time = now;

	f = fopen("/proc/stat", "r");
	if (f == NULL)
		return;

	if (fscanf(f, "cpu %ld %ld %ld %ld %ld %ld %ld %ld",
	           &user, &nice, &sys, &idle, &iowait, &irq, &softirq,
	           &steal) != 8) {
		fclose(f);
		return;
	}
	fclose(f);

	idle  += iowait;
	total  = user + nice + sys + idle + irq + softirq + steal;

	if (self->prev_cpu_total > 0) {
		diff_idle  = idle - self->prev_cpu_idle;
		diff_total = total - self->prev_cpu_total;
		if (diff_total > 0) {
			self->cpu_percent = (gint)(100 *
				(diff_total - diff_idle) / diff_total);
			if (self->cpu_percent < 0)
				self->cpu_percent = 0;
			if (self->cpu_percent > 100)
				self->cpu_percent = 100;
			ring_push(&self->cpu_history,
			          (gdouble)self->cpu_percent / 100.0);
		}
	}

	self->prev_cpu_idle  = idle;
	self->prev_cpu_total = total;
}

static void
read_load(BarSysinfo *self, time_t now)
{
	FILE *f;

	if (now - self->load_time < 5)
		return;
	self->load_time = now;

	f = fopen("/proc/loadavg", "r");
	if (f == NULL)
		return;
	if (fscanf(f, "%lf %lf %lf", &self->load1, &self->load5,
	           &self->load15) != 3)
		self->load1 = self->load5 = self->load15 = 0.0;
	fclose(f);
}

static void
read_memory(BarSysinfo *self, time_t now)
{
	FILE *f;
	char line[256];
	glong total_kb, avail_kb, swap_total_kb, swap_free_kb;

	if (now - self->mem_time < 5)
		return;
	self->mem_time = now;

	total_kb = avail_kb = swap_total_kb = swap_free_kb = 0;

	f = fopen("/proc/meminfo", "r");
	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, "MemTotal:", 9) == 0)
			sscanf(line + 9, " %ld", &total_kb);
		else if (strncmp(line, "MemAvailable:", 13) == 0)
			sscanf(line + 13, " %ld", &avail_kb);
		else if (strncmp(line, "SwapTotal:", 10) == 0)
			sscanf(line + 10, " %ld", &swap_total_kb);
		else if (strncmp(line, "SwapFree:", 9) == 0)
			sscanf(line + 9, " %ld", &swap_free_kb);
	}
	fclose(f);

	self->mem_total_mb  = total_kb / 1024;
	self->mem_avail_mb  = avail_kb / 1024;
	self->mem_used_mb   = (total_kb - avail_kb) / 1024;
	self->swap_total_mb = swap_total_kb / 1024;
	self->swap_used_mb  = (swap_total_kb - swap_free_kb) / 1024;

	if (self->mem_total_mb > 0) {
		ring_push(&self->mem_history,
		          (gdouble)self->mem_used_mb /
		          (gdouble)self->mem_total_mb);
	}
}

/* Read one integer out of a sysfs file. */
static gboolean
read_int_file(const gchar *path, gint *out)
{
	FILE *f;
	gint value;

	f = fopen(path, "r");
	if (f == NULL)
		return FALSE;
	if (fscanf(f, "%d", &value) != 1) {
		fclose(f);
		return FALSE;
	}
	fclose(f);
	if (out != NULL)
		*out = value;
	return TRUE;
}

/* Read a short string out of a sysfs file, trimmed. */
static gchar *
read_str_file(const gchar *path)
{
	g_autofree gchar *contents = NULL;

	if (!g_file_get_contents(path, &contents, NULL, NULL))
		return NULL;
	g_strstrip(contents);
	if (contents[0] == '\0')
		return NULL;
	return g_steal_pointer(&contents);
}

static void
read_battery(BarSysinfo *self, time_t now)
{
	const gchar *names[] = { "BAT0", "BAT1", "BAT2", "macsmc-battery",
	                         NULL };
	gint i;

	if (now - self->battery_time < 20)
		return;
	self->battery_time = now;

	self->battery_percent  = -1;
	self->battery_minutes  = -1;
	self->battery_charging = FALSE;

	for (i = 0; names[i] != NULL; i++) {
		g_autofree gchar *base = NULL;
		g_autofree gchar *cap_path = NULL;
		g_autofree gchar *status_path = NULL;
		g_autofree gchar *status = NULL;
		gint pct;

		base = g_build_filename("/sys/class/power_supply", names[i],
		                        NULL);
		if (!g_file_test(base, G_FILE_TEST_IS_DIR))
			continue;

		cap_path = g_build_filename(base, "capacity", NULL);
		if (!read_int_file(cap_path, &pct))
			continue;
		self->battery_percent = pct;

		status_path = g_build_filename(base, "status", NULL);
		status = read_str_file(status_path);
		if (status != NULL &&
		    (g_ascii_strcasecmp(status, "Charging") == 0 ||
		     g_ascii_strcasecmp(status, "Full") == 0))
			self->battery_charging = TRUE;

		/* Runtime left, from whichever pair of units this battery
		   reports.  Charge (uAh) and energy (uWh) are both common
		   and a machine exposes exactly one of them. */
		{
			g_autofree gchar *now_path = NULL;
			g_autofree gchar *rate_path = NULL;
			gint level = 0, rate = 0;

			now_path  = g_build_filename(base, "charge_now", NULL);
			rate_path = g_build_filename(base, "current_now", NULL);
			if (!read_int_file(now_path, &level)) {
				g_free(now_path);
				g_free(rate_path);
				now_path  = g_build_filename(base,
					"energy_now", NULL);
				rate_path = g_build_filename(base,
					"power_now", NULL);
				if (!read_int_file(now_path, &level))
					level = 0;
			}
			if (level > 0 && read_int_file(rate_path, &rate) &&
			    rate > 0) {
				self->battery_minutes =
					(gint)(((gint64)level * 60) / rate);
			}
		}
		break;
	}
}

static void
read_temp(BarSysinfo *self, time_t now)
{
	gint mc;

	if (now - self->temp_time < 10)
		return;
	self->temp_time = now;

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

	if (read_int_file(self->temp_path, &mc))
		self->temp_mc = mc;
}

static void
read_gpu(BarSysinfo *self, time_t now)
{
	const gchar *paths[] = {
		"/sys/class/drm/card0/device/gpu_busy_percent",
		"/sys/class/drm/card1/device/gpu_busy_percent",
		"/sys/class/drm/card2/device/gpu_busy_percent",
		NULL
	};
	gint i, pct;

	if (now - self->gpu_time < 5)
		return;
	self->gpu_time = now;

	for (i = 0; paths[i] != NULL; i++) {
		if (read_int_file(paths[i], &pct)) {
			self->gpu_percent = pct;
			return;
		}
	}
	self->gpu_percent = -1;
}

/* Sum an interface's counters out of /proc/net/dev.  With @want NULL
   the busiest non-loopback interface wins, which is what "the network"
   means when the user has not said. */
static gboolean
scan_net_dev(const gchar *want, gchar **found, glong *rx_out, glong *tx_out)
{
	FILE *f;
	char line[512];
	glong best_rx, best_tx;
	char best_name[64];
	gboolean have;

	f = fopen("/proc/net/dev", "r");
	if (f == NULL)
		return FALSE;

	have = FALSE;
	best_rx = best_tx = 0;
	best_name[0] = '\0';

	/* Two header lines. */
	if (fgets(line, sizeof(line), f) == NULL ||
	    fgets(line, sizeof(line), f) == NULL) {
		fclose(f);
		return FALSE;
	}

	while (fgets(line, sizeof(line), f) != NULL) {
		char name[64];
		glong r, t;
		gchar *trimmed;

		if (sscanf(line, " %63[^:]:%ld %*d %*d %*d %*d %*d %*d %*d %ld",
		           name, &r, &t) != 3)
			continue;

		trimmed = g_strstrip(name);

		if (want != NULL) {
			if (strcmp(trimmed, want) != 0)
				continue;
			g_strlcpy(best_name, trimmed, sizeof(best_name));
			best_rx = r;
			best_tx = t;
			have = TRUE;
			break;
		}

		if (strcmp(trimmed, "lo") == 0)
			continue;
		if (r + t > best_rx + best_tx) {
			g_strlcpy(best_name, trimmed, sizeof(best_name));
			best_rx = r;
			best_tx = t;
			have = TRUE;
		}
	}
	fclose(f);

	if (!have)
		return FALSE;
	if (found != NULL)
		*found = g_strdup(best_name);
	if (rx_out != NULL)
		*rx_out = best_rx;
	if (tx_out != NULL)
		*tx_out = best_tx;
	return TRUE;
}

static void
read_net(BarSysinfo *self, const gchar *iface, time_t now)
{
	g_autofree gchar *found = NULL;
	glong rx, tx;
	gboolean restart;

	/* A different interface than last time invalidates the previous
	   counters entirely; without this the first sample after a switch
	   is the difference between two unrelated devices. */
	restart = (g_strcmp0(self->net_iface, iface) != 0);
	if (!restart && now - self->net_time < 2)
		return;

	if (!scan_net_dev(iface, &found, &rx, &tx)) {
		self->net_rx_rate = 0;
		self->net_tx_rate = 0;
		return;
	}

	if (restart) {
		g_free(self->net_iface);
		self->net_iface = g_strdup(iface);
		self->prev_net_rx = rx;
		self->prev_net_tx = tx;
		self->net_time = now;
		return;
	}

	if (self->net_time > 0) {
		time_t dt = now - self->net_time;
		glong drx, dtx;

		if (dt <= 0)
			dt = 1;
		drx = rx - self->prev_net_rx;
		dtx = tx - self->prev_net_tx;
		if (drx < 0)
			drx = 0;
		if (dtx < 0)
			dtx = 0;
		self->net_rx_rate = drx / dt;
		self->net_tx_rate = dtx / dt;

		/* The sparkline is scaled against the largest rate seen so
		   far rather than a fixed ceiling: a 100 Mb link and a 10 Gb
		   link both want a graph that uses its full height. */
		if (self->net_rx_rate + self->net_tx_rate > self->net_peak)
			self->net_peak = self->net_rx_rate + self->net_tx_rate;
		if (self->net_peak > 0) {
			ring_push(&self->net_history,
				(gdouble)(self->net_rx_rate + self->net_tx_rate)
				/ (gdouble)self->net_peak);
		}
	}

	self->prev_net_rx = rx;
	self->prev_net_tx = tx;
	self->net_time    = now;
}

static void
read_io(BarSysinfo *self, const gchar *device, time_t now)
{
	FILE *f;
	char line[512];
	glong rd_sect, wr_sect;
	gboolean restart, found;

	restart = (g_strcmp0(self->io_device, device) != 0);
	if (!restart && now - self->io_time < 2)
		return;

	rd_sect = wr_sect = 0;
	found = FALSE;

	f = fopen("/proc/diskstats", "r");
	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f) != NULL) {
		char name[64];
		glong rd, wr;

		if (sscanf(line, " %*d %*d %63s %*d %*d %ld %*d %*d %*d %ld",
		           name, &rd, &wr) != 3)
			continue;

		if (device != NULL) {
			if (strcmp(name, device) != 0)
				continue;
			rd_sect = rd;
			wr_sect = wr;
			found = TRUE;
			break;
		}

		/* Whole disks only: a partition's counters double-count
		   what its parent already reported. */
		if ((strncmp(name, "sd", 2) == 0 && strlen(name) == 3) ||
		    (strncmp(name, "vd", 2) == 0 && strlen(name) == 3) ||
		    (strncmp(name, "nvme", 4) == 0 &&
		     strchr(name, 'p') == NULL)) {
			rd_sect = rd;
			wr_sect = wr;
			found = TRUE;
			break;
		}
	}
	fclose(f);

	if (!found)
		return;

	if (restart) {
		g_free(self->io_device);
		self->io_device = g_strdup(device);
		self->prev_io_rd = rd_sect;
		self->prev_io_wr = wr_sect;
		self->io_time = now;
		return;
	}

	if (self->io_time > 0) {
		time_t dt = now - self->io_time;
		glong drd, dwr;

		if (dt <= 0)
			dt = 1;
		drd = rd_sect - self->prev_io_rd;
		dwr = wr_sect - self->prev_io_wr;
		if (drd < 0)
			drd = 0;
		if (dwr < 0)
			dwr = 0;
		/* /proc/diskstats counts 512-byte sectors. */
		self->io_rd_rate = (drd * 512) / dt;
		self->io_wr_rate = (dwr * 512) / dt;
	}

	self->prev_io_rd = rd_sect;
	self->prev_io_wr = wr_sect;
	self->io_time    = now;
}

static void
read_wifi(BarSysinfo *self, time_t now)
{
	FILE *f;
	char line[512];

	if (now - self->wifi_time < 5)
		return;
	self->wifi_time = now;

	self->wifi_up = FALSE;

	f = fopen("/proc/net/wireless", "r");
	if (f == NULL)
		return;

	if (fgets(line, sizeof(line), f) == NULL ||
	    fgets(line, sizeof(line), f) == NULL) {
		fclose(f);
		return;
	}

	while (fgets(line, sizeof(line), f) != NULL) {
		char iface[64];
		gdouble link, level;

		if (sscanf(line, " %63[^:]: %*d %lf %lf", iface, &link,
		           &level) < 3)
			continue;

		g_free(self->wifi_iface);
		self->wifi_iface   = g_strdup(g_strstrip(iface));
		self->wifi_dbm     = (gint)level;
		/* /proc/net/wireless reports link quality on a 0-70 scale
		   on the drivers that report it at all. */
		self->wifi_quality = CLAMP(link / 70.0, 0.0, 1.0);
		self->wifi_up      = TRUE;
		break;
	}
	fclose(f);
}

/**
 * bar_sysinfo_tick:
 * @self: the reader
 */
void
bar_sysinfo_tick(BarSysinfo *self)
{
	time_t now;

	g_return_if_fail(self != NULL);

	now = time(NULL);
	read_cpu(self, now);
	read_load(self, now);
	read_memory(self, now);
	read_battery(self, now);
	read_temp(self, now);
	read_gpu(self, now);
	read_wifi(self, now);
	read_net(self, self->net_iface, now);
	read_io(self, self->io_device, now);
}

/* ----------------------------------------------------------------
 * Getters
 * ---------------------------------------------------------------- */

/**
 * bar_sysinfo_cpu_percent:
 * @self: the reader
 *
 * Returns: total CPU utilisation, 0--100
 */
gint
bar_sysinfo_cpu_percent(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, 0);
	read_cpu(self, time(NULL));
	return self->cpu_percent;
}

/**
 * bar_sysinfo_cpu_cores:
 * @self: the reader
 *
 * Returns: the number of logical processors
 */
gint
bar_sysinfo_cpu_cores(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, 1);
	return self->cpu_cores;
}

/**
 * bar_sysinfo_load1:
 * @self: the reader
 *
 * Returns: the one-minute load average
 */
gdouble
bar_sysinfo_load1(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, 0.0);
	read_load(self, time(NULL));
	return self->load1;
}

/**
 * bar_sysinfo_load5:
 * @self: the reader
 *
 * Returns: the five-minute load average
 */
gdouble
bar_sysinfo_load5(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, 0.0);
	read_load(self, time(NULL));
	return self->load5;
}

/**
 * bar_sysinfo_load15:
 * @self: the reader
 *
 * Returns: the fifteen-minute load average
 */
gdouble
bar_sysinfo_load15(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, 0.0);
	read_load(self, time(NULL));
	return self->load15;
}

/**
 * bar_sysinfo_mem_used_mb:
 * @self: the reader
 *
 * Returns: used memory in MiB
 */
glong
bar_sysinfo_mem_used_mb(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, 0);
	read_memory(self, time(NULL));
	return self->mem_used_mb;
}

/**
 * bar_sysinfo_mem_total_mb:
 * @self: the reader
 *
 * Returns: total memory in MiB
 */
glong
bar_sysinfo_mem_total_mb(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, 0);
	read_memory(self, time(NULL));
	return self->mem_total_mb;
}

/**
 * bar_sysinfo_mem_avail_mb:
 * @self: the reader
 *
 * Returns: available memory in MiB
 */
glong
bar_sysinfo_mem_avail_mb(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, 0);
	read_memory(self, time(NULL));
	return self->mem_avail_mb;
}

/**
 * bar_sysinfo_swap_used_mb:
 * @self: the reader
 *
 * Returns: used swap in MiB
 */
glong
bar_sysinfo_swap_used_mb(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, 0);
	read_memory(self, time(NULL));
	return self->swap_used_mb;
}

/**
 * bar_sysinfo_swap_total_mb:
 * @self: the reader
 *
 * Returns: total swap in MiB
 */
glong
bar_sysinfo_swap_total_mb(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, 0);
	read_memory(self, time(NULL));
	return self->swap_total_mb;
}

/**
 * bar_sysinfo_disk:
 * @self: the reader
 * @mount: (nullable): a mount point
 * @free_gb: (out) (optional): free space in GiB
 * @total_gb: (out) (optional): total space in GiB
 *
 * Returns: %TRUE when @mount could be stat'd
 */
gboolean
bar_sysinfo_disk(BarSysinfo *self, const gchar *mount, glong *free_gb,
                 glong *total_gb)
{
	struct statvfs st;

	g_return_val_if_fail(self != NULL, FALSE);

	if (mount == NULL || mount[0] == '\0')
		mount = "/";
	if (statvfs(mount, &st) != 0)
		return FALSE;

	if (free_gb != NULL)
		*free_gb = (glong)(((guint64)st.f_bavail * st.f_frsize) /
		                   (1024ULL * 1024ULL * 1024ULL));
	if (total_gb != NULL)
		*total_gb = (glong)(((guint64)st.f_blocks * st.f_frsize) /
		                    (1024ULL * 1024ULL * 1024ULL));
	return TRUE;
}

/**
 * bar_sysinfo_battery_percent:
 * @self: the reader
 *
 * Returns: charge percentage, or -1 when there is no battery
 */
gint
bar_sysinfo_battery_percent(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, -1);
	read_battery(self, time(NULL));
	return self->battery_percent;
}

/**
 * bar_sysinfo_battery_charging:
 * @self: the reader
 *
 * Returns: %TRUE when the battery is charging or full
 */
gboolean
bar_sysinfo_battery_charging(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	read_battery(self, time(NULL));
	return self->battery_charging;
}

/**
 * bar_sysinfo_battery_minutes:
 * @self: the reader
 *
 * Returns: minutes of runtime left, or -1
 */
gint
bar_sysinfo_battery_minutes(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, -1);
	read_battery(self, time(NULL));
	return self->battery_minutes;
}

/**
 * bar_sysinfo_temp_mc:
 * @self: the reader
 *
 * Returns: the temperature in millidegrees Celsius, or 0
 */
gint
bar_sysinfo_temp_mc(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, 0);
	read_temp(self, time(NULL));
	return self->temp_mc;
}

/**
 * bar_sysinfo_gpu_percent:
 * @self: the reader
 *
 * Returns: GPU utilisation, or -1 when the driver does not report it
 */
gint
bar_sysinfo_gpu_percent(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, -1);
	read_gpu(self, time(NULL));
	return self->gpu_percent;
}

/**
 * bar_sysinfo_net:
 * @self: the reader
 * @iface: (nullable): an interface name
 * @rx_rate: (out) (optional): receive rate in bytes per second
 * @tx_rate: (out) (optional): transmit rate in bytes per second
 *
 * Returns: (transfer none) (nullable): the interface measured
 */
const gchar *
bar_sysinfo_net(BarSysinfo *self, const gchar *iface, glong *rx_rate,
                glong *tx_rate)
{
	g_return_val_if_fail(self != NULL, NULL);

	read_net(self, iface, time(NULL));
	if (rx_rate != NULL)
		*rx_rate = self->net_rx_rate;
	if (tx_rate != NULL)
		*tx_rate = self->net_tx_rate;
	return self->net_iface;
}

/**
 * bar_sysinfo_io:
 * @self: the reader
 * @device: (nullable): a block device name
 * @rd_rate: (out) (optional): read rate in bytes per second
 * @wr_rate: (out) (optional): write rate in bytes per second
 */
void
bar_sysinfo_io(BarSysinfo *self, const gchar *device, glong *rd_rate,
               glong *wr_rate)
{
	g_return_if_fail(self != NULL);

	read_io(self, device, time(NULL));
	if (rd_rate != NULL)
		*rd_rate = self->io_rd_rate;
	if (wr_rate != NULL)
		*wr_rate = self->io_wr_rate;
}

/**
 * bar_sysinfo_uptime_seconds:
 * @self: the reader
 *
 * Returns: seconds since boot
 */
glong
bar_sysinfo_uptime_seconds(BarSysinfo *self)
{
	FILE *f;
	gdouble up;

	g_return_val_if_fail(self != NULL, 0);

	f = fopen("/proc/uptime", "r");
	if (f == NULL)
		return 0;
	if (fscanf(f, "%lf", &up) != 1)
		up = 0.0;
	fclose(f);
	return (glong)up;
}

/**
 * bar_sysinfo_hostname:
 * @self: the reader
 *
 * Returns: (transfer none): the host name
 */
const gchar *
bar_sysinfo_hostname(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, "");
	return (self->hostname != NULL) ? self->hostname : "";
}

/**
 * bar_sysinfo_username:
 * @self: the reader
 *
 * Returns: (transfer none): the user name
 */
const gchar *
bar_sysinfo_username(BarSysinfo *self)
{
	g_return_val_if_fail(self != NULL, "");
	return (self->username != NULL) ? self->username : "";
}

/**
 * bar_sysinfo_wifi:
 * @self: the reader
 * @iface: (out) (optional) (transfer none): the wireless interface
 * @dbm: (out) (optional): signal level in dBm
 * @quality: (out) (optional): link quality, 0.0--1.0
 *
 * Returns: %TRUE when a wireless interface is associated
 */
gboolean
bar_sysinfo_wifi(BarSysinfo *self, const gchar **iface, gint *dbm,
                 gdouble *quality)
{
	g_return_val_if_fail(self != NULL, FALSE);

	read_wifi(self, time(NULL));
	if (iface != NULL)
		*iface = self->wifi_iface;
	if (dbm != NULL)
		*dbm = self->wifi_dbm;
	if (quality != NULL)
		*quality = self->wifi_quality;
	return self->wifi_up;
}

/**
 * bar_sysinfo_ipv4:
 * @self: the reader
 * @iface: (nullable): an interface name
 *
 * Returns: (transfer full) (nullable): the address
 */
gchar *
bar_sysinfo_ipv4(BarSysinfo *self, const gchar *iface)
{
	struct ifaddrs *ifaddr, *ifa;
	gchar *result;

	g_return_val_if_fail(self != NULL, NULL);

	if (getifaddrs(&ifaddr) != 0)
		return NULL;

	result = NULL;
	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		char host[INET_ADDRSTRLEN];

		if (ifa->ifa_addr == NULL ||
		    ifa->ifa_addr->sa_family != AF_INET)
			continue;

		if (iface != NULL) {
			if (g_strcmp0(ifa->ifa_name, iface) != 0)
				continue;
		} else if (g_strcmp0(ifa->ifa_name, "lo") == 0) {
			continue;
		}

		if (inet_ntop(AF_INET,
		              &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
		              host, sizeof(host)) != NULL) {
			result = g_strdup(host);
			break;
		}
	}
	freeifaddrs(ifaddr);
	return result;
}

/* Parse /proc/net/route for the default route.  The kernel writes the
   addresses little-endian hex, which is why this is a shift-and-mask
   rather than an inet_ntop. */
static gboolean
read_default_route(gchar **iface_out, gchar **gateway_out)
{
	FILE *f;
	char line[512];
	gboolean found;

	f = fopen("/proc/net/route", "r");
	if (f == NULL)
		return FALSE;

	found = FALSE;
	/* Header. */
	if (fgets(line, sizeof(line), f) == NULL) {
		fclose(f);
		return FALSE;
	}

	while (fgets(line, sizeof(line), f) != NULL) {
		char name[64];
		guint dest, gateway;

		if (sscanf(line, "%63s %x %x", name, &dest, &gateway) != 3)
			continue;
		if (dest != 0)
			continue;

		if (iface_out != NULL)
			*iface_out = g_strdup(name);
		if (gateway_out != NULL) {
			*gateway_out = g_strdup_printf("%u.%u.%u.%u",
				(gateway      ) & 0xFF,
				(gateway >>  8) & 0xFF,
				(gateway >> 16) & 0xFF,
				(gateway >> 24) & 0xFF);
		}
		found = TRUE;
		break;
	}
	fclose(f);
	return found;
}

/**
 * bar_sysinfo_default_gateway:
 * @self: the reader
 *
 * Returns: (transfer full) (nullable): the IPv4 default gateway
 */
gchar *
bar_sysinfo_default_gateway(BarSysinfo *self)
{
	gchar *gateway;

	g_return_val_if_fail(self != NULL, NULL);

	gateway = NULL;
	if (!read_default_route(NULL, &gateway))
		return NULL;
	return gateway;
}

/**
 * bar_sysinfo_default_route_iface:
 * @self: the reader
 *
 * Returns: (transfer full) (nullable): the default route's interface
 */
gchar *
bar_sysinfo_default_route_iface(BarSysinfo *self)
{
	gchar *iface;

	g_return_val_if_fail(self != NULL, NULL);

	iface = NULL;
	if (!read_default_route(&iface, NULL))
		return NULL;
	return iface;
}

/**
 * bar_sysinfo_iface_bytes:
 * @self: the reader
 * @iface: an interface name
 * @rx: (out) (optional): total bytes received
 * @tx: (out) (optional): total bytes sent
 *
 * Returns: %TRUE when @iface was found
 */
gboolean
bar_sysinfo_iface_bytes(BarSysinfo *self, const gchar *iface, glong *rx,
                        glong *tx)
{
	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(iface != NULL, FALSE);

	return scan_net_dev(iface, NULL, rx, tx);
}

/**
 * bar_sysinfo_iface_is_wireless:
 * @self: the reader
 * @iface: an interface name
 *
 * Returns: %TRUE when @iface is a wireless device
 */
gboolean
bar_sysinfo_iface_is_wireless(BarSysinfo *self, const gchar *iface)
{
	g_autofree gchar *path = NULL;

	g_return_val_if_fail(self != NULL, FALSE);

	if (iface == NULL)
		return FALSE;

	/* The presence of the `wireless' directory is the kernel's own
	   answer; a name-prefix guess gets systemd's predictable names
	   wrong in both directions. */
	path = g_strdup_printf("/sys/class/net/%s/wireless", iface);
	return g_file_test(path, G_FILE_TEST_IS_DIR);
}

/**
 * bar_sysinfo_history:
 * @self: the reader
 * @which: `cpu', `memory' or `net'
 * @n_samples: (out): how many samples were returned
 *
 * Returns: (array length=n_samples) (transfer none) (nullable): samples
 */
const gdouble *
bar_sysinfo_history(BarSysinfo *self, const gchar *which, guint *n_samples)
{
	const SampleRing *ring;
	guint n;

	if (n_samples != NULL)
		*n_samples = 0;
	g_return_val_if_fail(self != NULL, NULL);

	if (g_strcmp0(which, "cpu") == 0)
		ring = &self->cpu_history;
	else if (g_strcmp0(which, "memory") == 0)
		ring = &self->mem_history;
	else if (g_strcmp0(which, "net") == 0)
		ring = &self->net_history;
	else
		return NULL;

	n = ring_read(ring, self->history_out);
	if (n == 0)
		return NULL;
	if (n_samples != NULL)
		*n_samples = n;
	return self->history_out;
}

/**
 * bar_sysinfo_format_bytes:
 * @bytes: a byte count
 * @buf: (out caller-allocates): the destination
 * @bufsz: its size
 */
void
bar_sysinfo_format_bytes(glong bytes, gchar *buf, gsize bufsz)
{
	if (buf == NULL || bufsz == 0)
		return;

	if (bytes >= 1024L * 1024L * 1024L)
		g_snprintf(buf, bufsz, "%.1f GB",
		           (gdouble)bytes / (1024.0 * 1024.0 * 1024.0));
	else if (bytes >= 1024L * 1024L)
		g_snprintf(buf, bufsz, "%.1f MB",
		           (gdouble)bytes / (1024.0 * 1024.0));
	else if (bytes >= 1024L)
		g_snprintf(buf, bufsz, "%.1f KB", (gdouble)bytes / 1024.0);
	else
		g_snprintf(buf, bufsz, "%ld B", bytes);
}

/**
 * bar_sysinfo_format_rate:
 * @bytes_per_sec: a rate
 * @buf: (out caller-allocates): the destination
 * @bufsz: its size
 */
void
bar_sysinfo_format_rate(glong bytes_per_sec, gchar *buf, gsize bufsz)
{
	if (buf == NULL || bufsz == 0)
		return;

	if (bytes_per_sec >= 1024L * 1024L * 1024L)
		g_snprintf(buf, bufsz, "%.1fG",
		           (gdouble)bytes_per_sec /
		           (1024.0 * 1024.0 * 1024.0));
	else if (bytes_per_sec >= 1024L * 1024L)
		g_snprintf(buf, bufsz, "%.1fM",
		           (gdouble)bytes_per_sec / (1024.0 * 1024.0));
	else if (bytes_per_sec >= 1024L)
		g_snprintf(buf, bufsz, "%.1fK",
		           (gdouble)bytes_per_sec / 1024.0);
	else
		g_snprintf(buf, bufsz, "%ldB", bytes_per_sec);
}

/**
 * bar_sysinfo_format_duration:
 * @seconds: a duration
 * @buf: (out caller-allocates): the destination
 * @bufsz: its size
 */
void
bar_sysinfo_format_duration(glong seconds, gchar *buf, gsize bufsz)
{
	glong days, hours, mins;

	if (buf == NULL || bufsz == 0)
		return;

	if (seconds < 0)
		seconds = 0;
	days  = seconds / 86400;
	hours = (seconds / 3600) % 24;
	mins  = (seconds / 60) % 60;

	if (days > 0)
		g_snprintf(buf, bufsz, "%ldd %ldh", days, hours);
	else if (hours > 0)
		g_snprintf(buf, bufsz, "%ldh %ldm", hours, mins);
	else
		g_snprintf(buf, bufsz, "%ldm", mins);
}
