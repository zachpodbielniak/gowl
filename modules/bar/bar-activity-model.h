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

#ifndef BAR_ACTIVITY_MODEL_H
#define BAR_ACTIVITY_MODEL_H

#include <glib.h>

#include "barkit/gowl-bar-theme.h"

G_BEGIN_DECLS

/*
 * The pure half of the cpu widget's activity mode: parsing /proc and
 * sysfs, the creature's arithmetic, the journal's pruning and the
 * signal guard.  Nothing here touches a plugin, a panel or a spawn, so
 * tests/test-bar-cpu-activity.c calls every function directly against
 * fixture trees.  Every reader takes its /proc or /sys ROOT as an
 * argument for exactly that reason.
 */

/**
 * BAR_ACTIVITY_CREATURE_ROWS:
 *
 * How many load rows the creature's body holds.
 */
#define BAR_ACTIVITY_CREATURE_ROWS 6

/**
 * BAR_ACTIVITY_CREATURE_FLAMES:
 *
 * How many flames burn above a hot creature's head.
 */
#define BAR_ACTIVITY_CREATURE_FLAMES 3

/* --- Settings ------------------------------------------------------ */

/**
 * bar_activity_sampling_interval:
 * @name: (nullable): `efficient', `balanced' or `fast'
 *
 * Returns: the poll interval in seconds: 4, 2 or 1; anything else
 *   (including %NULL) is `balanced'
 */
gint bar_activity_sampling_interval (const gchar *name);

/**
 * bar_activity_parse_fahrenheit:
 * @unit: (nullable): a `temperature-unit' setting
 *
 * Returns: %TRUE for `fahrenheit' (or `f'), %FALSE for anything else
 */
gboolean bar_activity_parse_fahrenheit (const gchar *unit);

/**
 * bar_activity_c_to_f:
 * @celsius: a temperature
 *
 * Returns: @celsius in degrees Fahrenheit
 */
gdouble bar_activity_c_to_f (gdouble celsius);

/**
 * bar_activity_format_temp:
 * @millicelsius: a sensor reading in thousandths of a degree Celsius
 * @fahrenheit: whether to show Fahrenheit
 * @buf: (out caller-allocates): the destination
 * @bufsz: its size
 *
 * Formats a whole-degree reading with its unit, e.g. `52°C' or
 * `126°F'.
 */
void bar_activity_format_temp (gint millicelsius, gboolean fahrenheit,
                               gchar *buf, gsize bufsz);

/**
 * bar_activity_parse_style_creature:
 * @style: (nullable): a `style' setting
 *
 * Returns: %TRUE when @style is `creature'; `label', unset and
 *   anything unrecognised keep the plain label
 */
gboolean bar_activity_parse_style_creature (const gchar *style);

/**
 * bar_activity_clamp_setting:
 * @value: (nullable): a setting's text
 * @fallback: used when @value is unset or not a number
 * @min: the smallest accepted value
 * @max: the largest accepted value
 *
 * Returns: @value as an integer, clamped to @min..@max
 */
gint bar_activity_clamp_setting (const gchar *value, gint fallback,
                                 gint min, gint max);

/* --- The creature -------------------------------------------------- */

/**
 * BarActivityCreature:
 * @left_eye: CPU utilisation as a 0..1 fill
 * @right_eye: memory in use as a 0..1 fill
 * @rows: load rows lit, 0..%BAR_ACTIVITY_CREATURE_ROWS
 * @calm: load is under the threshold, so no row is lit
 * @hot: CPU is at or over the heat threshold, so flames burn
 *
 * What the creature shows, worked out once from the readings.
 */
typedef struct {
	gdouble  left_eye;
	gdouble  right_eye;
	guint    rows;
	gboolean calm;
	gboolean hot;
} BarActivityCreature;

/**
 * bar_activity_creature_compute:
 * @cpu_percent: CPU utilisation, 0..100
 * @mem_percent: memory in use, 0..100
 * @load_percent: the one-minute load per core, as a percent
 * @load_threshold: the load percent under which no row is lit
 * @heat_threshold: the CPU percent at which flames appear
 * @out: (out caller-allocates): the result
 *
 * The rows are the load ABOVE the threshold, scaled over what is left
 * of the range: with a threshold of 50, a load of 50% lights none, 51%
 * lights one and 100% lights all six.  Scaling the whole range and
 * blanking below the threshold instead would jump from nothing to
 * three rows the moment the threshold is crossed.
 */
void bar_activity_creature_compute (gint cpu_percent, gint mem_percent,
                                    gint load_percent,
                                    gint load_threshold,
                                    gint heat_threshold,
                                    BarActivityCreature *out);

/**
 * BarActivityRect:
 * @x: left edge
 * @y: top edge
 * @w: width
 * @h: height
 */
typedef struct {
	gdouble x, y, w, h;
} BarActivityRect;

/**
 * BarActivityCreatureGeom:
 * @side: the square's side, in pixels
 * @body: the body, below the flame band
 * @eye: the two eye sockets, left then right
 * @eye_fill: the filled part of each socket, from its bottom
 * @row: the load row slots, @row[0] the BOTTOM one
 * @flame: the flame boxes above the head (each drawn as a tongue)
 *
 * Where everything goes, relative to the square's top-left corner.
 */
typedef struct {
	gint            side;
	BarActivityRect body;
	BarActivityRect eye[2];
	BarActivityRect eye_fill[2];
	BarActivityRect row[BAR_ACTIVITY_CREATURE_ROWS];
	BarActivityRect flame[BAR_ACTIVITY_CREATURE_FLAMES];
} BarActivityCreatureGeom;

/**
 * bar_activity_creature_side:
 * @bar_height: the bar's height in pixels
 *
 * Returns: the side of the square the creature is drawn in: the bar's
 *   height less a small margin, never under 10 pixels
 */
gint bar_activity_creature_side (gint bar_height);

/**
 * bar_activity_creature_layout:
 * @bar_height: the bar's height in pixels
 * @state: what the creature shows
 * @out: (out caller-allocates): the geometry
 */
void bar_activity_creature_layout (gint bar_height,
                                   const BarActivityCreature *state,
                                   BarActivityCreatureGeom *out);

/**
 * bar_activity_wiggle_offset:
 * @monotonic_us: g_get_monotonic_time()
 * @interval_s: the poll interval in seconds
 * @hot: whether the creature is hot
 * @motion: whether motion is enabled
 *
 * The creature's horizontal shake.  It alternates once per poll tick,
 * derived from the clock rather than a timer: a widget may not arm a
 * GLib timer (under cmacs it would fire on the editor's thread), and
 * the poll already runs once per interval.
 *
 * Returns: -1 or +1 while hot and moving, 0 otherwise
 */
gint bar_activity_wiggle_offset (gint64 monotonic_us, gint interval_s,
                                 gboolean hot, gboolean motion);

/**
 * bar_activity_resolve_color:
 * @theme: the active theme
 * @spec: (nullable): a `color-*' setting
 * @fallback: the role used when @spec is unset or unrecognised
 * @rgba: (out caller-allocates) (array fixed-size=4): the colour
 *
 * The three-way colour rule (role name, `#rrggbb', else @fallback) for
 * something painted directly with cairo rather than through the
 * plugin's label colour.
 *
 * Returns: %TRUE when @spec was understood, %FALSE when @fallback was
 *   used
 */
gboolean bar_activity_resolve_color (const GowlBarTheme *theme,
                                     const gchar *spec,
                                     GowlBarColor fallback,
                                     gdouble rgba[4]);

/* --- /proc/stat ----------------------------------------------------- */

/**
 * BarActivityCpuTimes:
 * @idle: idle plus iowait jiffies
 * @total: every jiffy the line counts
 */
typedef struct {
	guint64 idle;
	guint64 total;
} BarActivityCpuTimes;

/**
 * bar_activity_parse_proc_stat:
 * @text: the contents of /proc/stat
 * @aggregate: (out) (optional): the `cpu ' line
 * @cores: (out) (optional) (transfer full): a #GArray of
 *   #BarActivityCpuTimes, one per `cpuN' line, indexed by N; a core
 *   missing from the file (offline) is left zero
 *
 * Returns: %TRUE when the aggregate line was found
 */
gboolean bar_activity_parse_proc_stat (const gchar *text,
                                       BarActivityCpuTimes *aggregate,
                                       GArray **cores);

/**
 * bar_activity_cpu_percent:
 * @prev: the earlier sample
 * @now: the later sample
 *
 * Returns: utilisation between the two, 0..100, or -1 when no time
 *   passed or the counters went backwards (a core came back online)
 */
gint bar_activity_cpu_percent (const BarActivityCpuTimes *prev,
                               const BarActivityCpuTimes *now);

/**
 * bar_activity_read_frequencies:
 * @sys_root: the sysfs root, normally `/sys'
 *
 * Reads every logical core's current clock from
 * `devices/system/cpu/cpuN/cpufreq/scaling_cur_freq'.  On some
 * machines each read interrupts the core it asks about, so this is
 * only ever called from a worker.
 *
 * Returns: (transfer full): a #GArray of #gint MHz indexed by core;
 *   -1 for a core without cpufreq, and an empty array when none has
 */
GArray *bar_activity_read_frequencies (const gchar *sys_root);

/**
 * bar_activity_core_band:
 * @percent: a core's utilisation
 *
 * Returns: 0 (idle, under 25%), 1 (light), 2 (busy, 50%+), 3 (high,
 *   75%+) or 4 (saturated, 90%+) --- the CPU die's colour key
 */
guint bar_activity_core_band (gint percent);

/**
 * bar_activity_die_grid:
 * @n_cores: logical cores
 * @columns: (out): columns in the grid
 * @rows: (out): rows in the grid
 *
 * A near-square grid with at most 16 columns.
 */
void bar_activity_die_grid (guint n_cores, guint *columns, guint *rows);

/* --- Processes ------------------------------------------------------ */

/**
 * BarActivityProc:
 * @pid: the process id
 * @name: the command name (`comm'), valid UTF-8
 * @state: the state letter (R, S, D, Z, ...)
 * @ticks: user plus system time, in clock ticks
 * @starttime: when it started, in clock ticks since boot; with @pid
 *   this names ONE process, where a pid alone may be reused
 * @uid: the real user id
 * @rss_kb: resident memory in KiB
 * @cpu: CPU over the last sampling window, in percent of ONE core
 *   (so 250 means two and a half cores), -1 until a second sample
 */
typedef struct {
	gint     pid;
	gchar   *name;
	gchar    state;
	guint64  ticks;
	guint64  starttime;
	guint    uid;
	guint64  rss_kb;
	gdouble  cpu;
} BarActivityProc;

/**
 * bar_activity_proc_free:
 * @proc: (nullable) (transfer full): a process record
 */
void bar_activity_proc_free (BarActivityProc *proc);

/**
 * bar_activity_parse_pid_stat:
 * @text: the contents of /proc/PID/stat
 * @out: (out caller-allocates): filled with pid, name, state, ticks
 *   and starttime; @out->name is newly allocated on success
 *
 * The command name is the text between the FIRST `(' and the LAST
 * `)': a process may call itself `(foo) bar)', and splitting on spaces
 * or on the first `)' would read its fields from inside its name.
 *
 * Returns: %TRUE when every needed field was present
 */
gboolean bar_activity_parse_pid_stat (const gchar *text,
                                      BarActivityProc *out);

/**
 * bar_activity_parse_pid_status:
 * @text: the contents of /proc/PID/status
 * @uid: (out) (optional): the real uid
 * @rss_kb: (out) (optional): VmRSS in KiB, 0 for a kernel thread
 *
 * Returns: %TRUE when a Uid line was found
 */
gboolean bar_activity_parse_pid_status (const gchar *text, guint *uid,
                                        guint64 *rss_kb);

/**
 * bar_activity_read_processes:
 * @proc_root: the procfs root, normally `/proc'
 * @only_uid: keep only processes whose real uid is this, or -1 for all
 *
 * Reads every numeric directory under @proc_root.  A process that
 * exits between the listing and the read is skipped, not an error.
 * Worker-thread only: a busy machine has thousands.
 *
 * Returns: (transfer full) (element-type BarActivityProc): the
 *   processes, @cpu still -1
 */
GPtrArray *bar_activity_read_processes (const gchar *proc_root,
                                        gint only_uid);

/**
 * bar_activity_proc_key:
 * @pid: a pid
 * @starttime: its start time
 *
 * Returns: (transfer full): the key bar_activity_compute_cpu() uses,
 *   which a reused pid cannot collide with
 */
gchar *bar_activity_proc_key (gint pid, guint64 starttime);

/**
 * bar_activity_compute_cpu:
 * @procs: (element-type BarActivityProc): this sample
 * @prev_ticks: (element-type utf8 guint64): the last sample's ticks by
 *   bar_activity_proc_key(); replaced with this sample's on return
 * @elapsed_s: seconds between the two samples
 * @clock_ticks: `sysconf(_SC_CLK_TCK)'
 *
 * Fills @cpu for every process seen in both samples.  A process new
 * since the last one (or the first sample of all) keeps -1: one sample
 * says nothing about a rate.
 */
void bar_activity_compute_cpu (GPtrArray *procs, GHashTable *prev_ticks,
                               gdouble elapsed_s, glong clock_ticks);

/**
 * bar_activity_sort_processes:
 * @procs: (element-type BarActivityProc): the processes
 *
 * Busiest first; then by resident memory; then by pid, so the order is
 * stable between samples that tie.
 */
void bar_activity_sort_processes (GPtrArray *procs);

/**
 * bar_activity_proc_matches:
 * @proc: a process
 * @filter: (nullable): what the user typed
 *
 * Returns: %TRUE when @filter is empty, or names part of the command
 *   (ignoring case) or the pid's digits
 */
gboolean bar_activity_proc_matches (const BarActivityProc *proc,
                                    const gchar *filter);

/**
 * BarActivityKillCheck:
 * @BAR_ACTIVITY_KILL_OK: safe to signal
 * @BAR_ACTIVITY_KILL_PROTECTED: pid 1 or lower, or the compositor
 * @BAR_ACTIVITY_KILL_GONE: the process has exited
 * @BAR_ACTIVITY_KILL_REUSED: the pid now names a different process
 * @BAR_ACTIVITY_KILL_NOT_OWNER: it is not the user's own process
 */
typedef enum {
	BAR_ACTIVITY_KILL_OK = 0,
	BAR_ACTIVITY_KILL_PROTECTED,
	BAR_ACTIVITY_KILL_GONE,
	BAR_ACTIVITY_KILL_REUSED,
	BAR_ACTIVITY_KILL_NOT_OWNER
} BarActivityKillCheck;

/**
 * bar_activity_kill_check:
 * @proc_root: the procfs root
 * @pid: the pid the panel showed
 * @starttime: the start time sampled with it
 * @sampled_uid: the uid sampled with it
 * @my_uid: getuid()
 * @self_pid: getpid() --- the compositor, which must never be signalled
 *
 * Re-reads the process immediately before a signal.  The panel's list
 * is up to one sampling interval old; in that time the process can
 * exit and its pid be handed to something else, and a kill aimed at
 * the old one would land on the new one.
 *
 * Returns: why it may not be signalled, or %BAR_ACTIVITY_KILL_OK
 */
BarActivityKillCheck bar_activity_kill_check (const gchar *proc_root,
                                              gint pid,
                                              guint64 starttime,
                                              guint sampled_uid,
                                              guint my_uid,
                                              gint self_pid);

/**
 * bar_activity_kill_check_reason:
 * @check: a check result
 *
 * Returns: (transfer none): a sentence for the toast
 */
const gchar *bar_activity_kill_check_reason (BarActivityKillCheck check);

/**
 * bar_activity_parse_proc_id:
 * @id: a panel item id, `PREFIX:PID:STARTTIME:UID'
 * @prefix: the expected prefix, without its colon
 * @pid: (out): the pid
 * @starttime: (out): the start time
 * @uid: (out): the uid
 *
 * Returns: %TRUE when @id is well formed
 */
gboolean bar_activity_parse_proc_id (const gchar *id, const gchar *prefix,
                                     gint *pid, guint64 *starttime,
                                     guint *uid);

/* --- GPU (nvidia-smi) and filesystems -------------------------------- */

/**
 * BarActivityGpu:
 * @name: the board's name
 * @util: utilisation in percent, -1 when not reported
 * @mem_used_mb: memory in use, -1 when not reported
 * @mem_total_mb: memory fitted, -1 when not reported
 * @temp_c: temperature in Celsius, -1 when not reported
 */
typedef struct {
	gchar *name;
	gint   util;
	gint   mem_used_mb;
	gint   mem_total_mb;
	gint   temp_c;
} BarActivityGpu;

/**
 * bar_activity_gpu_free:
 * @gpu: (nullable) (transfer full): a GPU record
 */
void bar_activity_gpu_free (BarActivityGpu *gpu);

/**
 * bar_activity_nvidia_argv:
 * @command: (nullable): the nvidia-smi to run; %NULL for `nvidia-smi'
 * @timeout_s: wrap it in coreutils `timeout' for this many seconds, or
 *   0 for no wrapper
 *
 * nvidia-smi hangs outright when the driver is wedged, and the worker
 * thread that ran it would hang with it; the wrapper bounds that.
 *
 * Returns: (transfer full): the argv that asks for name, utilisation,
 *   memory and temperature as bare CSV
 */
gchar **bar_activity_nvidia_argv (const gchar *command, gint timeout_s);

/**
 * bar_activity_parse_nvidia_csv:
 * @text: (nullable): `nvidia-smi --query-gpu=name,utilization.gpu,
 *   memory.used,memory.total,temperature.gpu --format=csv,noheader,
 *   nounits' output
 *
 * A field nvidia-smi cannot report reads `[N/A]' or `[Not Supported]';
 * it becomes -1 rather than failing the line.
 *
 * Returns: (transfer full) (element-type BarActivityGpu): one per
 *   board; empty for empty or unparseable input
 */
GPtrArray *bar_activity_parse_nvidia_csv (const gchar *text);

/**
 * BarActivityMount:
 * @mount: the mount point
 * @fstype: the filesystem type
 * @used_bytes: bytes in use
 * @total_bytes: bytes in total
 */
typedef struct {
	gchar   *mount;
	gchar   *fstype;
	guint64  used_bytes;
	guint64  total_bytes;
} BarActivityMount;

/**
 * bar_activity_mount_free:
 * @mount: (nullable) (transfer full): a mount record
 */
void bar_activity_mount_free (BarActivityMount *mount);

/**
 * bar_activity_parse_mounts:
 * @text: the contents of /proc/mounts
 *
 * Keeps local disk filesystems only (ext4, xfs, btrfs, f2fs, vfat,
 * exfat, ntfs3, zfs, bcachefs), and one mount per device: a btrfs
 * volume mounted as /, /home and /var is one disk, not three, and is
 * named by its most recognisable mount point (/, then /var, /home,
 * /sysroot, then the shortest), not by whichever came first.  Network
 * filesystems are left out on purpose --- a statvfs on a hung NFS
 * server blocks for minutes.
 *
 * Returns: (transfer full) (element-type BarActivityMount): mounts with
 *   no sizes filled in
 */
GPtrArray *bar_activity_parse_mounts (const gchar *text);

/* --- The journal ---------------------------------------------------- */

/**
 * BAR_ACTIVITY_JOURNAL_MAX_AGE:
 *
 * Seven days, in seconds.
 */
#define BAR_ACTIVITY_JOURNAL_MAX_AGE (7 * 24 * 3600)

/**
 * BAR_ACTIVITY_JOURNAL_MAX_BYTES:
 *
 * 20 MiB.
 */
#define BAR_ACTIVITY_JOURNAL_MAX_BYTES (20 * 1024 * 1024)

/**
 * bar_activity_journal_line:
 * @epoch: seconds since 1970
 * @cpu: CPU percent
 * @mem: memory percent
 * @temp_mc: temperature in millidegrees Celsius, 0 when unknown
 *
 * Returns: (transfer full): `EPOCH,CPU,MEM,TEMP\n', temperature in
 *   whole degrees Celsius or empty when unknown
 */
gchar *bar_activity_journal_line (gint64 epoch, gint cpu, gint mem,
                                  gint temp_mc);

/**
 * bar_activity_journal_append:
 * @path: the journal
 * @line: one line, newline included
 * @error: return location for a #GError
 *
 * Appends with O_APPEND, creating the file mode 0600 (and its
 * directory 0700) when absent.
 *
 * Returns: %TRUE on success
 */
gboolean bar_activity_journal_append (const gchar *path, const gchar *line,
                                      GError **error);

/**
 * bar_activity_journal_prune:
 * @path: the journal
 * @now_epoch: the current time
 * @max_age_s: drop lines older than this
 * @max_bytes: then keep only the newest lines that fit in this
 * @error: return location for a #GError
 *
 * Rewrites the journal atomically when anything is dropped; a missing
 * journal is not an error.  A line that does not start with a number is
 * dropped too: it can only be a torn write.
 *
 * Returns: %TRUE on success
 */
gboolean bar_activity_journal_prune (const gchar *path, gint64 now_epoch,
                                     gint64 max_age_s, gsize max_bytes,
                                     GError **error);

/**
 * bar_activity_journal_export:
 * @path: the journal
 * @dest: the CSV to write
 * @error: return location for a #GError
 *
 * Writes `timestamp,cpu_percent,memory_percent,temperature_c' then one
 * row per journal line, the timestamp as local ISO 8601.
 *
 * Returns: %TRUE on success
 */
gboolean bar_activity_journal_export (const gchar *path, const gchar *dest,
                                      GError **error);

/**
 * bar_activity_export_path:
 * @home: the home directory
 * @now: (nullable): the time to name the file after; %NULL for now
 *
 * Returns: (transfer full): `HOME/activity-journal-YYYY-MM-DD.csv'
 */
gchar *bar_activity_export_path (const gchar *home, GDateTime *now);

G_END_DECLS

#endif /* BAR_ACTIVITY_MODEL_H */
