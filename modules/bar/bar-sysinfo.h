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

#ifndef BAR_SYSINFO_H
#define BAR_SYSINFO_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * BAR_SYSINFO_HISTORY:
 *
 * How many samples the sparklines in the system panel plot.  Sized so
 * a minute of five-second ticks fits, which is enough to see a spike
 * you just caused without holding a meaningful amount of memory.
 */
#define BAR_SYSINFO_HISTORY 48

/**
 * BarSysinfo:
 *
 * The shared reader behind every system plugin.
 *
 * One object rather than one per plugin because the readings overlap:
 * memory and swap come out of the same `/proc/meminfo' pass, and a bar
 * with both a cpu widget and a cpu panel open must not compute two
 * different utilisation figures from two different sampling windows.
 * Every getter is throttled internally, so calling one every tick from
 * several plugins costs one read.
 */
typedef struct _BarSysinfo BarSysinfo;

BarSysinfo *bar_sysinfo_new  (void);
void        bar_sysinfo_free (BarSysinfo *self);

/**
 * bar_sysinfo_tick:
 * @self: the reader
 *
 * Refreshes everything whose throttle has expired.  Called once per
 * bar tick, on the compositor thread; every read here is a small
 * `/proc' or `sysfs' file, never a subprocess.
 */
void bar_sysinfo_tick (BarSysinfo *self);

gint    bar_sysinfo_cpu_percent   (BarSysinfo *self);
gint    bar_sysinfo_cpu_cores     (BarSysinfo *self);
gdouble bar_sysinfo_load1         (BarSysinfo *self);
gdouble bar_sysinfo_load5         (BarSysinfo *self);
gdouble bar_sysinfo_load15        (BarSysinfo *self);

glong   bar_sysinfo_mem_used_mb   (BarSysinfo *self);
glong   bar_sysinfo_mem_total_mb  (BarSysinfo *self);
glong   bar_sysinfo_mem_avail_mb  (BarSysinfo *self);
glong   bar_sysinfo_swap_used_mb  (BarSysinfo *self);
glong   bar_sysinfo_swap_total_mb (BarSysinfo *self);

/**
 * bar_sysinfo_disk:
 * @self: the reader
 * @mount: (nullable): a mount point, or %NULL for `/'
 * @free_gb: (out) (optional): free space in GiB
 * @total_gb: (out) (optional): total space in GiB
 *
 * Returns: %TRUE when @mount could be stat'd
 */
gboolean bar_sysinfo_disk (BarSysinfo *self, const gchar *mount,
                            glong *free_gb, glong *total_gb);

gint    bar_sysinfo_battery_percent (BarSysinfo *self);
gboolean bar_sysinfo_battery_charging (BarSysinfo *self);
/**
 * bar_sysinfo_battery_minutes:
 * @self: the reader
 *
 * Returns: minutes of runtime left, or -1 when the kernel does not
 *   report enough to work it out
 */
gint    bar_sysinfo_battery_minutes (BarSysinfo *self);

gint    bar_sysinfo_temp_mc  (BarSysinfo *self);
gint    bar_sysinfo_gpu_percent (BarSysinfo *self);

/**
 * bar_sysinfo_net:
 * @self: the reader
 * @iface: (nullable): an interface name, or %NULL to pick the busiest
 * @rx_rate: (out) (optional): receive rate in bytes per second
 * @tx_rate: (out) (optional): transmit rate in bytes per second
 *
 * Returns: (transfer none) (nullable): the interface actually measured
 */
const gchar *bar_sysinfo_net (BarSysinfo *self, const gchar *iface,
                               glong *rx_rate, glong *tx_rate);

/**
 * bar_sysinfo_io:
 * @self: the reader
 * @device: (nullable): a block device name, or %NULL to auto-detect
 * @rd_rate: (out) (optional): read rate in bytes per second
 * @wr_rate: (out) (optional): write rate in bytes per second
 */
void bar_sysinfo_io (BarSysinfo *self, const gchar *device,
                      glong *rd_rate, glong *wr_rate);

/**
 * bar_sysinfo_uptime_seconds:
 * @self: the reader
 *
 * Returns: seconds since boot
 */
glong bar_sysinfo_uptime_seconds (BarSysinfo *self);

const gchar *bar_sysinfo_hostname (BarSysinfo *self);
const gchar *bar_sysinfo_username (BarSysinfo *self);

/**
 * bar_sysinfo_wifi:
 * @self: the reader
 * @iface: (out) (optional) (transfer none): the wireless interface
 * @dbm: (out) (optional): signal level in dBm
 * @quality: (out) (optional): link quality, 0.0--1.0
 *
 * Returns: %TRUE when a wireless interface is associated
 */
gboolean bar_sysinfo_wifi (BarSysinfo *self, const gchar **iface,
                            gint *dbm, gdouble *quality);

/**
 * bar_sysinfo_ipv4:
 * @self: the reader
 * @iface: (nullable): an interface name, or %NULL for the first
 *   non-loopback one
 *
 * Returns: (transfer full) (nullable): the address, or %NULL
 */
gchar *bar_sysinfo_ipv4 (BarSysinfo *self, const gchar *iface);

/**
 * bar_sysinfo_default_gateway:
 * @self: the reader
 *
 * Returns: (transfer full) (nullable): the IPv4 default gateway
 */
gchar *bar_sysinfo_default_gateway (BarSysinfo *self);

/**
 * bar_sysinfo_default_route_iface:
 * @self: the reader
 *
 * Returns: (transfer full) (nullable): the interface carrying the
 *   default route --- what "the network" means on this machine
 */
gchar *bar_sysinfo_default_route_iface (BarSysinfo *self);

/**
 * bar_sysinfo_iface_bytes:
 * @self: the reader
 * @iface: an interface name
 * @rx: (out) (optional): total bytes received since boot
 * @tx: (out) (optional): total bytes sent since boot
 *
 * Returns: %TRUE when @iface was found
 */
gboolean bar_sysinfo_iface_bytes (BarSysinfo *self, const gchar *iface,
                                   glong *rx, glong *tx);

/**
 * bar_sysinfo_iface_is_wireless:
 * @self: the reader
 * @iface: an interface name
 *
 * Returns: %TRUE when @iface is a wireless device
 */
gboolean bar_sysinfo_iface_is_wireless (BarSysinfo *self,
                                         const gchar *iface);

/**
 * bar_sysinfo_history:
 * @self: the reader
 * @which: `cpu', `memory' or `net'
 * @n_samples: (out): how many samples were returned
 *
 * Returns: (array length=n_samples) (transfer none) (nullable): the
 *   sample ring in oldest-to-newest order, each 0.0--1.0
 */
const gdouble *bar_sysinfo_history (BarSysinfo *self, const gchar *which,
                                     guint *n_samples);

/**
 * bar_sysinfo_format_bytes:
 * @bytes: a byte count
 * @buf: (out caller-allocates): the destination
 * @bufsz: its size
 *
 * Formats @bytes with a binary unit suffix.
 */
void bar_sysinfo_format_bytes (glong bytes, gchar *buf, gsize bufsz);

/**
 * bar_sysinfo_format_rate:
 * @bytes_per_sec: a rate
 * @buf: (out caller-allocates): the destination
 * @bufsz: its size
 *
 * Formats @bytes_per_sec with a binary unit suffix and no `/s'; the
 * caller supplies the units it wants to show.
 */
void bar_sysinfo_format_rate (glong bytes_per_sec, gchar *buf, gsize bufsz);

/**
 * bar_sysinfo_format_duration:
 * @seconds: a duration
 * @buf: (out caller-allocates): the destination
 * @bufsz: its size
 *
 * Formats @seconds as the largest two units that apply, e.g. `3d 4h'.
 */
void bar_sysinfo_format_duration (glong seconds, gchar *buf, gsize bufsz);

G_END_DECLS

#endif /* BAR_SYSINFO_H */
