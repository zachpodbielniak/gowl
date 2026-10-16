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

#include "bar-activity-model.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * SECTION:bar-activity-model
 * @title: Activity model
 * @short_description: the cpu widget's activity mode, minus the widget
 *
 * Parsing, arithmetic and file handling for the cpu widget's activity
 * mode (the creature, the CPU die, the process list, the journal).
 * Nothing here knows about plugins or panels; every reader takes the
 * procfs or sysfs root it reads from, so the tests point it at a
 * fixture tree rather than at the machine running them.
 */

/* Command names longer than this are cut.  The kernel's own limit is
   15 bytes; anything longer came from a fixture or a hostile tree, and
   should not get to stretch a panel row. */
#define ACTIVITY_NAME_MAX 64

/* No /proc file this reads is anywhere near this; a bigger one is not
   a procfs file and is refused rather than read into memory. */
#define ACTIVITY_PROC_FILE_MAX (64 * 1024)

/* ----------------------------------------------------------------
 * Settings
 * ---------------------------------------------------------------- */

/**
 * bar_activity_sampling_interval:
 * @name: (nullable): the setting
 *
 * Returns: seconds between samples
 */
gint
bar_activity_sampling_interval(const gchar *name)
{
	if (g_strcmp0(name, "efficient") == 0)
		return 4;
	if (g_strcmp0(name, "fast") == 0)
		return 1;
	return 2;
}

/**
 * bar_activity_parse_fahrenheit:
 * @unit: (nullable): the setting
 *
 * Returns: whether Fahrenheit was asked for
 */
gboolean
bar_activity_parse_fahrenheit(const gchar *unit)
{
	if (unit == NULL)
		return FALSE;
	return g_ascii_strcasecmp(unit, "fahrenheit") == 0 ||
	       g_ascii_strcasecmp(unit, "f") == 0;
}

/**
 * bar_activity_c_to_f:
 * @celsius: degrees Celsius
 *
 * Returns: degrees Fahrenheit
 */
gdouble
bar_activity_c_to_f(gdouble celsius)
{
	return celsius * 9.0 / 5.0 + 32.0;
}

/**
 * bar_activity_format_temp:
 * @millicelsius: the reading
 * @fahrenheit: the unit
 * @buf: (out caller-allocates): the destination
 * @bufsz: its size
 */
void
bar_activity_format_temp(gint millicelsius, gboolean fahrenheit,
                         gchar *buf, gsize bufsz)
{
	gdouble c;

	c = (gdouble)millicelsius / 1000.0;
	if (fahrenheit)
		g_snprintf(buf, bufsz, "%d\xc2\xb0""F",
		           (gint)floor(bar_activity_c_to_f(c) + 0.5));
	else
		g_snprintf(buf, bufsz, "%d\xc2\xb0""C", millicelsius / 1000);
}

/**
 * bar_activity_parse_style_creature:
 * @style: (nullable): the setting
 *
 * Returns: whether to draw the creature
 */
gboolean
bar_activity_parse_style_creature(const gchar *style)
{
	return style != NULL && g_ascii_strcasecmp(style, "creature") == 0;
}

/**
 * bar_activity_clamp_setting:
 * @value: (nullable): the setting
 * @fallback: the default
 * @min: lower bound
 * @max: upper bound
 *
 * Returns: the clamped value
 */
gint
bar_activity_clamp_setting(const gchar *value, gint fallback, gint min,
                           gint max)
{
	gchar *end = NULL;
	gint64 v;

	if (value == NULL || value[0] == '\0')
		return CLAMP(fallback, min, max);
	v = g_ascii_strtoll(value, &end, 10);
	if (end == value || (end != NULL && *end != '\0'))
		return CLAMP(fallback, min, max);
	return (gint)CLAMP(v, (gint64)min, (gint64)max);
}

/* ----------------------------------------------------------------
 * The creature
 * ---------------------------------------------------------------- */

/**
 * bar_activity_creature_compute:
 * @cpu_percent: CPU
 * @mem_percent: memory
 * @load_percent: load per core
 * @load_threshold: rows start above this
 * @heat_threshold: flames from this
 * @out: (out caller-allocates): the result
 */
void
bar_activity_creature_compute(gint cpu_percent, gint mem_percent,
                              gint load_percent, gint load_threshold,
                              gint heat_threshold,
                              BarActivityCreature *out)
{
	gint cpu, mem, load, threshold;

	g_return_if_fail(out != NULL);

	cpu       = CLAMP(cpu_percent, 0, 100);
	mem       = CLAMP(mem_percent, 0, 100);
	load      = CLAMP(load_percent, 0, 100);
	threshold = CLAMP(load_threshold, 0, 99);

	out->left_eye  = (gdouble)cpu / 100.0;
	out->right_eye = (gdouble)mem / 100.0;
	out->hot       = cpu >= CLAMP(heat_threshold, 1, 100);

	if (load <= threshold) {
		out->rows = 0;
		out->calm = TRUE;
		return;
	}

	/* Round UP: one percent past the threshold is visibly one row,
	   so crossing it never looks like nothing happened. */
	out->calm = FALSE;
	out->rows = (guint)ceil((gdouble)(load - threshold) *
	                        BAR_ACTIVITY_CREATURE_ROWS /
	                        (gdouble)(100 - threshold));
	if (out->rows < 1)
		out->rows = 1;
	if (out->rows > BAR_ACTIVITY_CREATURE_ROWS)
		out->rows = BAR_ACTIVITY_CREATURE_ROWS;
}

/**
 * bar_activity_creature_side:
 * @bar_height: the bar's height
 *
 * Returns: the creature's square side
 */
gint
bar_activity_creature_side(gint bar_height)
{
	return MAX(10, bar_height - 6);
}

static void
rect_set(BarActivityRect *r, gdouble side, gdouble x, gdouble y,
         gdouble w, gdouble h)
{
	r->x = x * side;
	r->y = y * side;
	r->w = w * side;
	r->h = h * side;
}

/**
 * bar_activity_creature_layout:
 * @bar_height: the bar's height
 * @state: what to show
 * @out: (out caller-allocates): the geometry
 *
 * Proportions of a unit square, so the creature is the same shape at
 * any bar height: the top fifth is the flame band, then the body with
 * two eye sockets near its top and six row slots filling its bottom
 * half.
 */
void
bar_activity_creature_layout(gint bar_height,
                             const BarActivityCreature *state,
                             BarActivityCreatureGeom *out)
{
	gdouble s, rows_top, rows_h, slot_h, row_h;
	guint i;

	g_return_if_fail(state != NULL);
	g_return_if_fail(out != NULL);

	memset(out, 0, sizeof(*out));
	out->side = bar_activity_creature_side(bar_height);
	s = (gdouble)out->side;

	rect_set(&out->body,   s, 0.08, 0.24, 0.84, 0.74);
	rect_set(&out->eye[0], s, 0.22, 0.32, 0.20, 0.20);
	rect_set(&out->eye[1], s, 0.58, 0.32, 0.20, 0.20);

	/* Each socket fills from its bottom, like a gauge. */
	for (i = 0; i < 2; i++) {
		gdouble fill = (i == 0) ? state->left_eye : state->right_eye;

		fill = CLAMP(fill, 0.0, 1.0);
		out->eye_fill[i]   = out->eye[i];
		out->eye_fill[i].h = out->eye[i].h * fill;
		out->eye_fill[i].y = out->eye[i].y + out->eye[i].h -
		                     out->eye_fill[i].h;
	}

	/* Six equal slots in the lower body, row 0 at the bottom.  Each
	   bar takes 70% of its slot so neighbours stay distinct at 20
	   pixels tall. */
	rows_top = 0.58 * s;
	rows_h   = 0.34 * s;
	slot_h   = rows_h / BAR_ACTIVITY_CREATURE_ROWS;
	row_h    = slot_h * 0.7;
	for (i = 0; i < BAR_ACTIVITY_CREATURE_ROWS; i++) {
		out->row[i].x = 0.20 * s;
		out->row[i].w = 0.60 * s;
		out->row[i].h = row_h;
		out->row[i].y = rows_top + rows_h - (gdouble)(i + 1) * slot_h +
		                (slot_h - row_h) / 2.0;
	}

	for (i = 0; i < BAR_ACTIVITY_CREATURE_FLAMES; i++)
		rect_set(&out->flame[i], s, 0.17 + 0.25 * (gdouble)i, 0.01,
		         0.16, 0.21);
}

/**
 * bar_activity_wiggle_offset:
 * @monotonic_us: now
 * @interval_s: the poll interval
 * @hot: whether hot
 * @motion: whether motion is on
 *
 * Returns: the horizontal offset
 */
gint
bar_activity_wiggle_offset(gint64 monotonic_us, gint interval_s,
                           gboolean hot, gboolean motion)
{
	gint64 tick;

	if (!hot || !motion)
		return 0;
	tick = monotonic_us / ((gint64)MAX(interval_s, 1) * G_USEC_PER_SEC);
	return (tick % 2 == 0) ? -1 : 1;
}

/**
 * bar_activity_resolve_color:
 * @theme: the theme
 * @spec: (nullable): the setting
 * @fallback: the default role
 * @rgba: (out caller-allocates) (array fixed-size=4): the result
 *
 * Returns: whether @spec was understood
 */
gboolean
bar_activity_resolve_color(const GowlBarTheme *theme, const gchar *spec,
                           GowlBarColor fallback, gdouble rgba[4])
{
	GowlBarColor role;
	const gdouble *c;

	if (spec != NULL && spec[0] != '\0') {
		if (gowl_bar_theme_color_from_name(spec, &role)) {
			c = gowl_bar_theme_color(theme, role);
			memcpy(rgba, c, 4 * sizeof(gdouble));
			return TRUE;
		}
		if (gowl_bar_color_parse(spec, rgba))
			return TRUE;
	}
	c = gowl_bar_theme_color(theme, fallback);
	memcpy(rgba, c, 4 * sizeof(gdouble));
	return FALSE;
}

/* ----------------------------------------------------------------
 * /proc/stat and cpufreq
 * ---------------------------------------------------------------- */

/* One `cpu' or `cpuN' line, after its label.  Guest time is already
   counted in user time, so it is not added again. */
static gboolean
parse_cpu_fields(const gchar *p, BarActivityCpuTimes *out)
{
	guint64 v[8];
	gchar *end;
	guint i;

	for (i = 0; i < 8; i++) {
		while (*p == ' ')
			p++;
		if (!g_ascii_isdigit(*p)) {
			/* Kernels before 2.6.11 stop at seven; steal is 0. */
			if (i == 7) {
				v[7] = 0;
				break;
			}
			return FALSE;
		}
		v[i] = g_ascii_strtoull(p, &end, 10);
		p = end;
	}
	out->idle  = v[3] + v[4];
	out->total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
	return TRUE;
}

/**
 * bar_activity_parse_proc_stat:
 * @text: the file
 * @aggregate: (out) (optional): the total line
 * @cores: (out) (optional) (transfer full): per-core lines
 *
 * Returns: whether the total line was found
 */
gboolean
bar_activity_parse_proc_stat(const gchar *text,
                             BarActivityCpuTimes *aggregate,
                             GArray **cores)
{
	g_auto(GStrv) lines = NULL;
	GArray *arr;
	gboolean found = FALSE;
	guint i;

	if (cores != NULL)
		*cores = NULL;
	if (text == NULL)
		return FALSE;

	arr = g_array_new(FALSE, TRUE, sizeof(BarActivityCpuTimes));
	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		const gchar *l = lines[i];
		BarActivityCpuTimes t;

		if (!g_str_has_prefix(l, "cpu"))
			continue;
		if (l[3] == ' ') {
			if (parse_cpu_fields(l + 3, &t)) {
				if (aggregate != NULL)
					*aggregate = t;
				found = TRUE;
			}
		} else if (g_ascii_isdigit(l[3])) {
			gchar *end;
			guint64 n;

			n = g_ascii_strtoull(l + 3, &end, 10);
			/* 8192 is the kernel's NR_CPUS ceiling; a number
			   past it is not a core, and must not size an
			   array. */
			if (*end != ' ' || n >= 8192)
				continue;
			if (!parse_cpu_fields(end, &t))
				continue;
			if (arr->len <= n)
				g_array_set_size(arr, (guint)n + 1);
			g_array_index(arr, BarActivityCpuTimes, n) = t;
		}
	}

	if (cores != NULL)
		*cores = arr;
	else
		g_array_unref(arr);
	return found;
}

/**
 * bar_activity_cpu_percent:
 * @prev: earlier
 * @now: later
 *
 * Returns: utilisation, or -1
 */
gint
bar_activity_cpu_percent(const BarActivityCpuTimes *prev,
                         const BarActivityCpuTimes *now)
{
	guint64 d_total, d_idle;

	if (prev == NULL || now == NULL)
		return -1;
	if (now->total <= prev->total || now->idle < prev->idle)
		return -1;
	d_total = now->total - prev->total;
	d_idle  = now->idle - prev->idle;
	if (d_idle > d_total)
		return 0;
	return (gint)(100 * (d_total - d_idle) / d_total);
}

static gboolean
read_small_file(const gchar *path, gchar **out)
{
	gchar *text = NULL;
	gsize len = 0;

	*out = NULL;
	if (!g_file_get_contents(path, &text, &len, NULL))
		return FALSE;
	if (len > ACTIVITY_PROC_FILE_MAX) {
		g_free(text);
		return FALSE;
	}
	*out = text;
	return TRUE;
}

/**
 * bar_activity_read_frequencies:
 * @sys_root: the sysfs root
 *
 * Returns: (transfer full): MHz by core
 */
GArray *
bar_activity_read_frequencies(const gchar *sys_root)
{
	g_autofree gchar *base = NULL;
	g_autoptr(GDir) dir = NULL;
	GArray *arr;
	const gchar *entry;
	gboolean any = FALSE;
	gint unknown = -1;

	arr  = g_array_new(FALSE, FALSE, sizeof(gint));
	base = g_build_filename(sys_root != NULL ? sys_root : "/sys",
	                        "devices", "system", "cpu", NULL);
	dir = g_dir_open(base, 0, NULL);
	if (dir == NULL)
		return arr;

	while ((entry = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *path = NULL;
		g_autofree gchar *text = NULL;
		gchar *end;
		guint64 n, khz;

		if (!g_str_has_prefix(entry, "cpu") ||
		    !g_ascii_isdigit(entry[3]))
			continue;
		n = g_ascii_strtoull(entry + 3, &end, 10);
		if (*end != '\0' || n >= 8192)
			continue;

		path = g_build_filename(base, entry, "cpufreq",
		                        "scaling_cur_freq", NULL);
		if (!read_small_file(path, &text))
			continue;
		khz = g_ascii_strtoull(g_strstrip(text), &end, 10);
		if (end == text || *end != '\0')
			continue;

		while (arr->len <= n)
			g_array_append_val(arr, unknown);
		g_array_index(arr, gint, n) = (gint)(khz / 1000);
		any = TRUE;
	}

	if (!any)
		g_array_set_size(arr, 0);
	return arr;
}

/**
 * bar_activity_core_band:
 * @percent: utilisation
 *
 * Returns: the colour band
 */
guint
bar_activity_core_band(gint percent)
{
	if (percent >= 90)
		return 4;
	if (percent >= 75)
		return 3;
	if (percent >= 50)
		return 2;
	if (percent >= 25)
		return 1;
	return 0;
}

/**
 * bar_activity_die_grid:
 * @n_cores: cores
 * @columns: (out): columns
 * @rows: (out): rows
 *
 * One row up to eight cores, eight wide up to 32, sixteen wide past
 * that: a laptop's die reads as a strip, a workstation's as a block.
 */
void
bar_activity_die_grid(guint n_cores, guint *columns, guint *rows)
{
	guint cols;

	if (n_cores == 0) {
		*columns = 0;
		*rows    = 0;
		return;
	}
	if (n_cores <= 8)
		cols = n_cores;
	else if (n_cores <= 32)
		cols = 8;
	else
		cols = 16;
	*columns = cols;
	*rows    = (n_cores + cols - 1) / cols;
}

/* ----------------------------------------------------------------
 * Processes
 * ---------------------------------------------------------------- */

/**
 * bar_activity_proc_free:
 * @proc: (nullable) (transfer full): the record
 */
void
bar_activity_proc_free(BarActivityProc *proc)
{
	if (proc == NULL)
		return;
	g_free(proc->name);
	g_free(proc);
}

/* Valid UTF-8, no control characters, bounded.  A process names
   itself; that name goes straight into a pango layout. */
static gchar *
clean_name(const gchar *raw, gssize len)
{
	g_autofree gchar *valid = NULL;
	GString *out;
	const gchar *p;

	valid = g_utf8_make_valid(raw, len);
	out = g_string_new(NULL);
	for (p = valid; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (g_unichar_iscntrl(c))
			c = ' ';
		if (g_utf8_strlen(out->str, -1) >= ACTIVITY_NAME_MAX)
			break;
		g_string_append_unichar(out, c);
	}
	return g_string_free(out, FALSE);
}

/**
 * bar_activity_parse_pid_stat:
 * @text: the file
 * @out: (out caller-allocates): the record
 *
 * Returns: whether it parsed
 */
gboolean
bar_activity_parse_pid_stat(const gchar *text, BarActivityProc *out)
{
	const gchar *open, *close, *p;
	gchar *end;
	guint64 fields[20];
	gint64 pid;
	guint i;

	if (text == NULL || out == NULL)
		return FALSE;

	open  = strchr(text, '(');
	close = strrchr(text, ')');
	if (open == NULL || close == NULL || close < open)
		return FALSE;

	pid = g_ascii_strtoll(text, &end, 10);
	if (end == text || pid <= 0 || pid > G_MAXINT)
		return FALSE;

	/* After the name: state, then fields 4.. as numbers.  utime is
	   field 14, stime 15, starttime 22 (proc(5), 1-based), which is
	   index 11, 12 and 19 counting the state as 0. */
	p = close + 1;
	while (*p == ' ')
		p++;
	if (*p == '\0' || *p == ' ')
		return FALSE;
	out->state = *p;
	p++;
	for (i = 1; i < G_N_ELEMENTS(fields); i++) {
		while (*p == ' ')
			p++;
		if (*p == '-')
			p++;  /* priority and nice may be negative */
		if (!g_ascii_isdigit(*p))
			return FALSE;
		fields[i] = g_ascii_strtoull(p, &end, 10);
		p = end;
	}

	out->pid       = (gint)pid;
	out->ticks     = fields[11] + fields[12];
	out->starttime = fields[19];
	out->name      = clean_name(open + 1, close - (open + 1));
	return TRUE;
}

/**
 * bar_activity_parse_pid_status:
 * @text: the file
 * @uid: (out) (optional): the real uid
 * @rss_kb: (out) (optional): resident KiB
 *
 * Returns: whether a Uid line was found
 */
gboolean
bar_activity_parse_pid_status(const gchar *text, guint *uid,
                              guint64 *rss_kb)
{
	g_auto(GStrv) lines = NULL;
	gboolean found = FALSE;
	guint i;

	if (rss_kb != NULL)
		*rss_kb = 0;
	if (text == NULL)
		return FALSE;

	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		gchar *p, *end;
		guint64 v;

		if (g_str_has_prefix(lines[i], "Uid:")) {
			p = lines[i] + 4;
			while (*p == ' ' || *p == '\t')
				p++;
			v = g_ascii_strtoull(p, &end, 10);
			if (end == p || v > G_MAXUINT)
				continue;
			if (uid != NULL)
				*uid = (guint)v;
			found = TRUE;
		} else if (g_str_has_prefix(lines[i], "VmRSS:")) {
			p = lines[i] + 6;
			while (*p == ' ' || *p == '\t')
				p++;
			v = g_ascii_strtoull(p, &end, 10);
			if (end != p && rss_kb != NULL)
				*rss_kb = v;
		}
	}
	return found;
}

static gboolean
read_one_process(const gchar *proc_root, const gchar *name,
                 BarActivityProc *out)
{
	g_autofree gchar *stat_path = NULL;
	g_autofree gchar *status_path = NULL;
	g_autofree gchar *stat_text = NULL;
	g_autofree gchar *status_text = NULL;

	stat_path   = g_build_filename(proc_root, name, "stat", NULL);
	status_path = g_build_filename(proc_root, name, "status", NULL);
	if (!read_small_file(stat_path, &stat_text) ||
	    !read_small_file(status_path, &status_text))
		return FALSE;
	if (!bar_activity_parse_pid_stat(stat_text, out))
		return FALSE;
	if (!bar_activity_parse_pid_status(status_text, &out->uid,
	                                   &out->rss_kb)) {
		g_clear_pointer(&out->name, g_free);
		return FALSE;
	}
	return TRUE;
}

/**
 * bar_activity_read_processes:
 * @proc_root: the procfs root
 * @only_uid: a uid, or -1
 *
 * Returns: (transfer full) (element-type BarActivityProc): processes
 */
GPtrArray *
bar_activity_read_processes(const gchar *proc_root, gint only_uid)
{
	g_autoptr(GDir) dir = NULL;
	GPtrArray *out;
	const gchar *entry;

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_activity_proc_free);
	if (proc_root == NULL)
		proc_root = "/proc";
	dir = g_dir_open(proc_root, 0, NULL);
	if (dir == NULL)
		return out;

	while ((entry = g_dir_read_name(dir)) != NULL) {
		BarActivityProc rec;
		const gchar *c;

		for (c = entry; *c != '\0'; c++)
			if (!g_ascii_isdigit(*c))
				break;
		if (*c != '\0' || entry[0] == '\0')
			continue;

		memset(&rec, 0, sizeof(rec));
		/* Gone between the listing and the read: not an error,
		   just a process that finished. */
		if (!read_one_process(proc_root, entry, &rec))
			continue;
		if (only_uid >= 0 && rec.uid != (guint)only_uid) {
			g_free(rec.name);
			continue;
		}
		rec.cpu = -1.0;
		g_ptr_array_add(out, g_memdup2(&rec, sizeof(rec)));
	}
	return out;
}

/**
 * bar_activity_proc_key:
 * @pid: the pid
 * @starttime: its start time
 *
 * Returns: (transfer full): the key
 */
gchar *
bar_activity_proc_key(gint pid, guint64 starttime)
{
	return g_strdup_printf("%d:%" G_GUINT64_FORMAT, pid, starttime);
}

/**
 * bar_activity_compute_cpu:
 * @procs: (element-type BarActivityProc): this sample
 * @prev_ticks: (element-type utf8 guint64): the last sample, keyed by
 *   bar_activity_proc_key() and owning both keys and values (create it
 *   with g_str_hash, g_str_equal, g_free, g_free)
 * @elapsed_s: seconds between samples
 * @clock_ticks: ticks per second
 */
void
bar_activity_compute_cpu(GPtrArray *procs, GHashTable *prev_ticks,
                         gdouble elapsed_s, glong clock_ticks)
{
	guint i;

	g_return_if_fail(procs != NULL);
	g_return_if_fail(prev_ticks != NULL);

	for (i = 0; i < procs->len; i++) {
		BarActivityProc *p = g_ptr_array_index(procs, i);
		g_autofree gchar *key = NULL;
		const guint64 *before;

		key = bar_activity_proc_key(p->pid, p->starttime);
		before = g_hash_table_lookup(prev_ticks, key);
		if (before == NULL || elapsed_s <= 0.0 || clock_ticks <= 0 ||
		    p->ticks < *before) {
			p->cpu = -1.0;
			continue;
		}
		p->cpu = (gdouble)(p->ticks - *before) /
		         (gdouble)clock_ticks / elapsed_s * 100.0;
	}

	/* This sample becomes the next one's baseline.  Rebuilt rather
	   than updated so a process that exited does not stay in the
	   table for the rest of the session. */
	g_hash_table_remove_all(prev_ticks);
	for (i = 0; i < procs->len; i++) {
		BarActivityProc *p = g_ptr_array_index(procs, i);
		guint64 *v = g_new(guint64, 1);

		*v = p->ticks;
		g_hash_table_insert(prev_ticks,
		                    bar_activity_proc_key(p->pid,
		                                          p->starttime), v);
	}
}

static gint
proc_compare(gconstpointer a, gconstpointer b)
{
	const BarActivityProc *pa = *(const BarActivityProc * const *)a;
	const BarActivityProc *pb = *(const BarActivityProc * const *)b;

	if (pa->cpu != pb->cpu)
		return (pa->cpu > pb->cpu) ? -1 : 1;
	if (pa->rss_kb != pb->rss_kb)
		return (pa->rss_kb > pb->rss_kb) ? -1 : 1;
	return (pa->pid < pb->pid) ? -1 : (pa->pid > pb->pid) ? 1 : 0;
}

/**
 * bar_activity_sort_processes:
 * @procs: (element-type BarActivityProc): the processes
 */
void
bar_activity_sort_processes(GPtrArray *procs)
{
	g_return_if_fail(procs != NULL);
	g_ptr_array_sort(procs, proc_compare);
}

/**
 * bar_activity_proc_matches:
 * @proc: the process
 * @filter: (nullable): the filter
 *
 * Returns: whether it matches
 */
gboolean
bar_activity_proc_matches(const BarActivityProc *proc, const gchar *filter)
{
	g_autofree gchar *want = NULL;
	g_autofree gchar *have = NULL;
	gchar pid[16];

	if (filter == NULL || filter[0] == '\0')
		return TRUE;
	if (proc == NULL || proc->name == NULL)
		return FALSE;

	g_snprintf(pid, sizeof(pid), "%d", proc->pid);
	if (strstr(pid, filter) != NULL)
		return TRUE;

	want = g_utf8_casefold(filter, -1);
	have = g_utf8_casefold(proc->name, -1);
	return strstr(have, want) != NULL;
}

/**
 * bar_activity_kill_check:
 * @proc_root: the procfs root
 * @pid: the pid
 * @starttime: its sampled start time
 * @sampled_uid: its sampled uid
 * @my_uid: ours
 * @self_pid: ours
 *
 * Returns: the verdict
 */
BarActivityKillCheck
bar_activity_kill_check(const gchar *proc_root, gint pid, guint64 starttime,
                        guint sampled_uid, guint my_uid, gint self_pid)
{
	g_autofree gchar *name = NULL;
	BarActivityProc now;

	/* Before touching /proc: these are refused whatever it says. */
	if (pid <= 1 || pid == self_pid)
		return BAR_ACTIVITY_KILL_PROTECTED;
	if (sampled_uid != my_uid)
		return BAR_ACTIVITY_KILL_NOT_OWNER;

	memset(&now, 0, sizeof(now));
	name = g_strdup_printf("%d", pid);
	if (!read_one_process(proc_root != NULL ? proc_root : "/proc", name,
	                      &now))
		return BAR_ACTIVITY_KILL_GONE;
	g_free(now.name);

	if (now.pid != pid || now.starttime != starttime)
		return BAR_ACTIVITY_KILL_REUSED;
	if (now.uid != my_uid)
		return BAR_ACTIVITY_KILL_NOT_OWNER;
	return BAR_ACTIVITY_KILL_OK;
}

/**
 * bar_activity_kill_check_reason:
 * @check: the verdict
 *
 * Returns: (transfer none): why
 */
const gchar *
bar_activity_kill_check_reason(BarActivityKillCheck check)
{
	switch (check) {
	case BAR_ACTIVITY_KILL_OK:
		return "It can be signalled.";
	case BAR_ACTIVITY_KILL_PROTECTED:
		return "That is init or the compositor itself; signalling it "
		       "would end the session.";
	case BAR_ACTIVITY_KILL_GONE:
		return "It has already exited.";
	case BAR_ACTIVITY_KILL_REUSED:
		return "It exited and its process id now belongs to a "
		       "different program, so nothing was sent.";
	case BAR_ACTIVITY_KILL_NOT_OWNER:
		return "It belongs to another user; only your own processes "
		       "can be signalled from here.";
	default:
		break;
	}
	return "Unknown.";
}

/**
 * bar_activity_parse_proc_id:
 * @id: the item id
 * @prefix: the prefix
 * @pid: (out): pid
 * @starttime: (out): start time
 * @uid: (out): uid
 *
 * Returns: whether @id parsed
 */
gboolean
bar_activity_parse_proc_id(const gchar *id, const gchar *prefix, gint *pid,
                           guint64 *starttime, guint *uid)
{
	g_auto(GStrv) parts = NULL;
	gint64 p;
	guint64 s, u;

	if (id == NULL || prefix == NULL)
		return FALSE;
	parts = g_strsplit(id, ":", 5);
	if (g_strv_length(parts) != 4 || strcmp(parts[0], prefix) != 0)
		return FALSE;
	if (!g_ascii_string_to_signed(parts[1], 10, 1, G_MAXINT, &p, NULL) ||
	    !g_ascii_string_to_unsigned(parts[2], 10, 0, G_MAXUINT64, &s,
	                                NULL) ||
	    !g_ascii_string_to_unsigned(parts[3], 10, 0, G_MAXUINT, &u,
	                                NULL))
		return FALSE;
	*pid       = (gint)p;
	*starttime = s;
	*uid       = (guint)u;
	return TRUE;
}

/* ----------------------------------------------------------------
 * GPU and filesystems
 * ---------------------------------------------------------------- */

/**
 * bar_activity_gpu_free:
 * @gpu: (nullable) (transfer full): the record
 */
void
bar_activity_gpu_free(BarActivityGpu *gpu)
{
	if (gpu == NULL)
		return;
	g_free(gpu->name);
	g_free(gpu);
}

/**
 * bar_activity_nvidia_argv:
 * @command: (nullable): the binary
 * @timeout_s: seconds, or 0
 *
 * Returns: (transfer full): the argv
 */
gchar **
bar_activity_nvidia_argv(const gchar *command, gint timeout_s)
{
	GPtrArray *argv;

	argv = g_ptr_array_new();
	if (timeout_s > 0) {
		g_ptr_array_add(argv, g_strdup("timeout"));
		g_ptr_array_add(argv, g_strdup_printf("%d", timeout_s));
	}
	g_ptr_array_add(argv, g_strdup((command != NULL &&
	                                command[0] != '\0')
	                               ? command : "nvidia-smi"));
	g_ptr_array_add(argv, g_strdup("--query-gpu=name,utilization.gpu,"
	                               "memory.used,memory.total,"
	                               "temperature.gpu"));
	g_ptr_array_add(argv, g_strdup("--format=csv,noheader,nounits"));
	g_ptr_array_add(argv, NULL);
	return (gchar **)g_ptr_array_free(argv, FALSE);
}

static gint
csv_int(const gchar *field)
{
	g_autofree gchar *t = g_strstrip(g_strdup(field));
	gint64 v;

	if (!g_ascii_string_to_signed(t, 10, 0, G_MAXINT, &v, NULL))
		return -1;
	return (gint)v;
}

/**
 * bar_activity_parse_nvidia_csv:
 * @text: (nullable): the output
 *
 * The last four fields are numbers; everything before them is the
 * name, so a board name with a comma in it does not shift the rest.
 *
 * Returns: (transfer full) (element-type BarActivityGpu): the boards
 */
GPtrArray *
bar_activity_parse_nvidia_csv(const gchar *text)
{
	g_auto(GStrv) lines = NULL;
	GPtrArray *out;
	guint i;

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_activity_gpu_free);
	if (text == NULL)
		return out;

	lines = g_strsplit(text, "\n", 64);
	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) f = NULL;
		g_autofree gchar *name = NULL;
		BarActivityGpu *gpu;
		guint n;

		g_strstrip(lines[i]);
		if (lines[i][0] == '\0')
			continue;
		f = g_strsplit(lines[i], ",", -1);
		n = g_strv_length(f);
		if (n < 5)
			continue;

		{
			gchar *saved = f[n - 4];

			f[n - 4] = NULL;
			name = g_strjoinv(",", f);
			f[n - 4] = saved;
		}
		gpu = g_new0(BarActivityGpu, 1);
		gpu->name = clean_name(g_strstrip(name), -1);
		gpu->util         = csv_int(f[n - 4]);
		gpu->mem_used_mb  = csv_int(f[n - 3]);
		gpu->mem_total_mb = csv_int(f[n - 2]);
		gpu->temp_c       = csv_int(f[n - 1]);
		g_ptr_array_add(out, gpu);
	}
	return out;
}

/**
 * bar_activity_mount_free:
 * @mount: (nullable) (transfer full): the record
 */
void
bar_activity_mount_free(BarActivityMount *mount)
{
	if (mount == NULL)
		return;
	g_free(mount->mount);
	g_free(mount->fstype);
	g_free(mount);
}

/* /proc/mounts escapes space, tab, newline and backslash as \ooo. */
static gchar *
unescape_mount(const gchar *s)
{
	GString *out = g_string_new(NULL);

	while (*s != '\0') {
		if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' &&
		    s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
			g_string_append_c(out, (gchar)(((s[1] - '0') << 6) |
			                               ((s[2] - '0') << 3) |
			                               (s[3] - '0')));
			s += 4;
			continue;
		}
		g_string_append_c(out, *s++);
	}
	return g_string_free(out, FALSE);
}

/*
 * Which of a device's mount points names it.  One disk is commonly
 * mounted several times --- btrfs subvolumes, and on an ostree system
 * the same volume appears as /sysroot, /etc, /var and /var/home --- and
 * the first in /proc/mounts is not the one a person would call it.  On
 * ostree `/' is a read-only composefs that is not in the list at all,
 * so /var (where everything writable lives) is the next best name.
 */
static guint
mount_rank(const gchar *mount)
{
	static const gchar *const preferred[] = {
		"/", "/var", "/home", "/sysroot", NULL
	};
	guint i;

	for (i = 0; preferred[i] != NULL; i++)
		if (strcmp(mount, preferred[i]) == 0)
			return i;
	return 16 + (guint)MIN(strlen(mount), 1000);
}

/**
 * bar_activity_parse_mounts:
 * @text: the file
 *
 * Returns: (transfer full) (element-type BarActivityMount): mounts
 */
GPtrArray *
bar_activity_parse_mounts(const gchar *text)
{
	static const gchar *const local[] = {
		"ext4", "ext3", "xfs", "btrfs", "f2fs", "vfat", "exfat",
		"ntfs3", "zfs", "bcachefs", NULL
	};
	g_auto(GStrv) lines = NULL;
	g_autoptr(GHashTable) by_device = NULL;
	GPtrArray *out;
	guint i;

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_activity_mount_free);
	if (text == NULL)
		return out;

	/* device -> the BarActivityMount kept for it (owned by @out) */
	by_device = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                  NULL);
	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) f = NULL;
		g_autofree gchar *raw = NULL;
		g_autofree gchar *mount = NULL;
		BarActivityMount *m;

		f = g_strsplit(lines[i], " ", 5);
		if (g_strv_length(f) < 3)
			continue;
		if (!g_strv_contains(local, f[2]))
			continue;
		raw   = unescape_mount(f[1]);
		mount = clean_name(raw, -1);

		m = g_hash_table_lookup(by_device, f[0]);
		if (m != NULL) {
			/* Seen this disk: keep whichever name ranks first. */
			if (mount_rank(mount) < mount_rank(m->mount)) {
				g_free(m->mount);
				m->mount = g_steal_pointer(&mount);
			}
			continue;
		}
		if (out->len >= 16)
			continue;

		m = g_new0(BarActivityMount, 1);
		m->mount  = g_steal_pointer(&mount);
		m->fstype = g_strdup(f[2]);
		g_ptr_array_add(out, m);
		g_hash_table_insert(by_device, g_strdup(f[0]), m);
	}
	return out;
}

/* ----------------------------------------------------------------
 * The journal
 * ---------------------------------------------------------------- */

/**
 * bar_activity_journal_line:
 * @epoch: seconds
 * @cpu: CPU percent
 * @mem: memory percent
 * @temp_mc: millidegrees, 0 for unknown
 *
 * Returns: (transfer full): the line
 */
gchar *
bar_activity_journal_line(gint64 epoch, gint cpu, gint mem, gint temp_mc)
{
	gchar temp[16] = "";

	if (temp_mc > 0)
		g_snprintf(temp, sizeof(temp), "%d", temp_mc / 1000);
	return g_strdup_printf("%" G_GINT64_FORMAT ",%d,%d,%s\n", epoch,
	                       CLAMP(cpu, 0, 100), CLAMP(mem, 0, 100), temp);
}

/**
 * bar_activity_journal_append:
 * @path: the journal
 * @line: the line
 * @error: return location for a #GError
 *
 * Returns: whether it was written
 */
gboolean
bar_activity_journal_append(const gchar *path, const gchar *line,
                            GError **error)
{
	g_autofree gchar *dir = NULL;
	gsize len, done = 0;
	gint fd;

	g_return_val_if_fail(path != NULL && line != NULL, FALSE);

	dir = g_path_get_dirname(path);
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		gint e = errno;

		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(e),
		            "cannot create %s: %s", dir, g_strerror(e));
		return FALSE;
	}
	fd = g_open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0) {
		gint e = errno;

		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(e),
		            "cannot open %s: %s", path, g_strerror(e));
		return FALSE;
	}
	len = strlen(line);
	while (done < len) {
		gssize n = write(fd, line + done, len - done);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			gint e = errno;

			close(fd);
			g_set_error(error, G_FILE_ERROR,
			            g_file_error_from_errno(e),
			            "cannot write %s: %s", path, g_strerror(e));
			return FALSE;
		}
		done += (gsize)n;
	}
	close(fd);
	return TRUE;
}

/* The epoch at the start of a journal line, or -1 for a line that does
   not have one (a write torn by a crash). */
static gint64
line_epoch(const gchar *line)
{
	gchar *end;
	gint64 v;

	if (!g_ascii_isdigit(line[0]))
		return -1;
	v = g_ascii_strtoll(line, &end, 10);
	if (*end != ',')
		return -1;
	return v;
}

/**
 * bar_activity_journal_prune:
 * @path: the journal
 * @now_epoch: now
 * @max_age_s: oldest kept
 * @max_bytes: largest kept
 * @error: return location for a #GError
 *
 * Returns: whether it succeeded
 */
gboolean
bar_activity_journal_prune(const gchar *path, gint64 now_epoch,
                           gint64 max_age_s, gsize max_bytes,
                           GError **error)
{
	g_autofree gchar *text = NULL;
	g_auto(GStrv) lines = NULL;
	g_autoptr(GPtrArray) keep = NULL;
	g_autoptr(GString) out = NULL;
	GError *local = NULL;
	gsize len = 0, kept_bytes = 0;
	gboolean dropped = FALSE;
	guint i, first;

	if (!g_file_get_contents(path, &text, &len, &local)) {
		if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
			g_error_free(local);
			return TRUE;
		}
		g_propagate_error(error, local);
		return FALSE;
	}

	keep  = g_ptr_array_new();
	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		gint64 e;

		/* The empty string after the final newline. */
		if (lines[i][0] == '\0' && lines[i + 1] == NULL)
			break;
		e = line_epoch(lines[i]);
		if (e < 0 || e < now_epoch - max_age_s) {
			dropped = TRUE;
			continue;
		}
		g_ptr_array_add(keep, lines[i]);
		kept_bytes += strlen(lines[i]) + 1;
	}

	/* Still too big: drop the oldest until the rest fits.  Whole
	   lines only, so the file always starts at a record. */
	first = 0;
	while (kept_bytes > max_bytes && first < keep->len) {
		kept_bytes -= strlen(g_ptr_array_index(keep, first)) + 1;
		first++;
		dropped = TRUE;
	}

	if (!dropped)
		return TRUE;

	out = g_string_sized_new(kept_bytes);
	for (i = first; i < keep->len; i++) {
		g_string_append(out, g_ptr_array_index(keep, i));
		g_string_append_c(out, '\n');
	}
	return g_file_set_contents_full(path, out->str, (gssize)out->len,
	                                G_FILE_SET_CONTENTS_CONSISTENT, 0600,
	                                error);
}

/**
 * bar_activity_journal_export:
 * @path: the journal
 * @dest: the CSV
 * @error: return location for a #GError
 *
 * Returns: whether it was written
 */
gboolean
bar_activity_journal_export(const gchar *path, const gchar *dest,
                            GError **error)
{
	g_autofree gchar *text = NULL;
	g_auto(GStrv) lines = NULL;
	g_autoptr(GString) out = NULL;
	guint i;

	if (!g_file_get_contents(path, &text, NULL, error))
		return FALSE;

	out = g_string_new("timestamp,cpu_percent,memory_percent,"
	                   "temperature_c\n");
	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		g_autoptr(GDateTime) when = NULL;
		g_autofree gchar *iso = NULL;
		const gchar *rest;
		gint64 e;

		e = line_epoch(lines[i]);
		if (e < 0)
			continue;
		when = g_date_time_new_from_unix_local(e);
		if (when == NULL)
			continue;
		iso  = g_date_time_format_iso8601(when);
		rest = strchr(lines[i], ',');
		g_string_append_printf(out, "%s%s\n", iso, rest);
	}
	return g_file_set_contents_full(dest, out->str, (gssize)out->len,
	                                G_FILE_SET_CONTENTS_CONSISTENT, 0600,
	                                error);
}

/**
 * bar_activity_export_path:
 * @home: the home directory
 * @now: (nullable): when
 *
 * Returns: (transfer full): the path
 */
gchar *
bar_activity_export_path(const gchar *home, GDateTime *now)
{
	g_autoptr(GDateTime) local = NULL;
	g_autofree gchar *day = NULL;
	g_autofree gchar *name = NULL;

	if (now == NULL)
		local = g_date_time_new_now_local();
	else
		local = g_date_time_ref(now);
	day  = g_date_time_format(local, "%Y-%m-%d");
	name = g_strdup_printf("activity-journal-%s.csv", day);
	return g_build_filename(home, name, NULL);
}
