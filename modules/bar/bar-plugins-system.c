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

#include <linux/input-event-codes.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include "bar-internal.h"
#include "bar-activity-model.h"

/**
 * SECTION:bar-plugins-system
 * @title: System bar plugins
 * @short_description: the machine's own readings, and the panel behind them
 *
 * Every one of these puts a number in the bar and opens the same
 * system panel: a click on the memory widget and a click on the
 * temperature widget should both take you to the place where you can
 * see what the machine is doing, rather than to two different
 * single-reading dropdowns.  The panel's hero changes to match
 * whichever widget was clicked, which is enough to make it feel like
 * that widget's own.
 */

static BarSysinfo *
sys_info(void)
{
	const BarEnv *env = bar_env();

	return (env != NULL) ? env->sysinfo : NULL;
}

/* Pick a colour for a 0..1 utilisation figure.  One rule for every
   reading so a busy machine looks consistently busy. */
static GowlBarColor
sys_pressure_color(gdouble fraction)
{
	if (fraction >= 0.90)
		return GOWL_BAR_COLOR_RED;
	if (fraction >= 0.75)
		return GOWL_BAR_COLOR_PEACH;
	return GOWL_BAR_COLOR_TEXT;
}

/*
 * The widget's colour for a 0..1 pressure reading.
 *
 * A `color' setting is the CALM colour -- what the widget wears while
 * the reading is ordinary -- and the pressure ladder still fires over
 * it.  It used to win outright, which with the shipped layout
 * colouring cpu green, memory blue and disk yellow meant no shipped
 * bar ever showed a pressure warning: a processor pinned at 100% was
 * exactly as green as an idle one.  A user who wants a widget that
 * never changes colour has the readings that do not carry pressure.
 */
static void
sys_set_color(GowlBarPlugin *plugin, gdouble fraction,
              GowlBarColor fallback)
{
	if (fraction >= 0.75) {
		gowl_bar_plugin_set_color(plugin,
		                          sys_pressure_color(fraction));
		return;
	}
	bar_plugin_apply_color(plugin,
	                       gowl_bar_plugin_get_setting(plugin, "color"),
	                       fallback);
}

/* Whether the widget should show its icon.  Off by default because the
   shipped configuration uses a plain monospace font, and a Nerd Font
   glyph in a font that lacks it draws a replacement box. */
static void
sys_set_icon(GowlBarPlugin *plugin, const gchar *glyph)
{
	if (gowl_bar_plugin_get_setting_bool(plugin, "icons", FALSE))
		gowl_bar_plugin_set_icon(plugin, glyph);
	else
		gowl_bar_plugin_set_icon(plugin, NULL);
}

/* ----------------------------------------------------------------
 * The shared system panel
 * ---------------------------------------------------------------- */

/*
 * Which filesystem "the disk" means on this machine.
 *
 * On an ostree system -- Silverblue, Immutablue and friends -- `/' is a
 * read-only overlay of the deployment, typically tens of megabytes with
 * ZERO available.  Reporting it is not merely unhelpful, it is alarming
 * and wrong: the bar reads "/ 0G" on a host with terabytes free, because
 * everything writable lives under /var.
 *
 * /run/ostree-booted is the marker ostree itself sets, so this asks the
 * system rather than guessing from a distro name.  An explicit
 * `disk:<mount>' in the configuration still wins.
 */
static const gchar *
disk_default_mount(void)
{
	static gint cached = -1;

	if (cached < 0)
		cached = g_file_test("/run/ostree-booted",
		                     G_FILE_TEST_EXISTS) ? 1 : 0;
	return cached ? "/var" : "/";
}

/*
 * What one widget may change in the shared panel.  Every field has a
 * neutral value, and a NULL options pointer means all of them, which is
 * exactly the panel every system widget showed before the cpu widget
 * grew its activity mode.
 */
typedef struct {
	gboolean       fahrenheit;     /* temperatures in Fahrenheit */
	const gdouble *cpu_samples;    /* the Processor graph, or NULL for
	                                  the shared reader's */
	guint          n_cpu_samples;
	const gchar   *clock_avg;      /* a Clock row, or NULL for none */
	const gchar   *clock_range;
} SysPanelOpts;

/* Build the body of the panel every system widget opens --- everything
   but the tool buttons, so a widget can add its own sections above
   them.  @focus names the reading the clicked widget shows, which
   becomes the hero. */
static GowlBarPanel *
sys_build_panel_body(GowlBarPlugin *plugin, const gchar *focus,
                     const SysPanelOpts *opts)
{
	BarSysinfo *info = sys_info();
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	const gdouble *samples;
	guint n_samples;
	gchar buf[128], buf2[128];
	glong used_mb, total_mb, swap_used, swap_total;
	glong free_gb, total_gb;
	gint cpu, temp_mc, gpu, battery;
	gdouble load1, load5, load15;
	gboolean fahrenheit;

	if (info == NULL)
		return NULL;
	fahrenheit = (opts != NULL) && opts->fahrenheit;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 430);

	cpu       = bar_sysinfo_cpu_percent(info);
	used_mb   = bar_sysinfo_mem_used_mb(info);
	total_mb  = bar_sysinfo_mem_total_mb(info);
	swap_used = bar_sysinfo_swap_used_mb(info);
	swap_total = bar_sysinfo_swap_total_mb(info);
	load1     = bar_sysinfo_load1(info);
	load5     = bar_sysinfo_load5(info);
	load15    = bar_sysinfo_load15(info);
	temp_mc   = bar_sysinfo_temp_mc(info);
	gpu       = bar_sysinfo_gpu_percent(info);
	battery   = bar_sysinfo_battery_percent(info);

	/* Hero: whichever reading the user clicked. */
	if (g_strcmp0(focus, "memory") == 0) {
		g_snprintf(buf, sizeof(buf), "%.1f GB",
		           (gdouble)used_mb / 1024.0);
		gowl_bar_panel_add_hero(panel, "\xef\x94\xb8", buf,
		                        "Memory in use");
	} else if (g_strcmp0(focus, "temp") == 0) {
		bar_activity_format_temp(temp_mc, fahrenheit, buf,
		                         sizeof(buf));
		gowl_bar_panel_add_hero(panel, "\xef\x8b\x88", buf,
		                        "Package temperature");
	} else if (g_strcmp0(focus, "disk") == 0) {
		const gchar *mount;

		mount = gowl_bar_plugin_get_setting(plugin, "param");
		if (mount == NULL || mount[0] == '\0')
			mount = disk_default_mount();
		if (bar_sysinfo_disk(info, mount, &free_gb, &total_gb))
			g_snprintf(buf, sizeof(buf), "%ld GB", free_gb);
		else
			g_strlcpy(buf, "--", sizeof(buf));
		g_snprintf(buf2, sizeof(buf2), "Free on %s", mount);
		gowl_bar_panel_add_hero(panel, "\xef\x82\xa0", buf, buf2);
	} else {
		g_snprintf(buf, sizeof(buf), "%d%%", cpu);
		gowl_bar_panel_add_hero(panel, "\xef\x8b\x9b", buf,
		                        "Processor load");
	}

	/* CPU */
	gowl_bar_panel_add_section(panel, "Processor");
	if (opts != NULL && opts->cpu_samples != NULL) {
		samples   = opts->cpu_samples;
		n_samples = opts->n_cpu_samples;
	} else {
		samples = bar_sysinfo_history(info, "cpu", &n_samples);
	}
	if (samples != NULL && n_samples > 0) {
		g_snprintf(buf, sizeof(buf), "%d%%", cpu);
		item = gowl_bar_panel_add_graph(panel, "Utilisation", samples,
		                                n_samples);
		gowl_bar_panel_item_set_value(item, buf);
		gowl_bar_panel_item_set_color(item,
			sys_pressure_color((gdouble)cpu / 100.0));
	} else {
		g_snprintf(buf, sizeof(buf), "%d%%", cpu);
		gowl_bar_panel_add_field(panel, "Utilisation", buf);
	}

	g_snprintf(buf, sizeof(buf), "%.2f", load1);
	g_snprintf(buf2, sizeof(buf2), "%.2f / %.2f", load5, load15);
	item = gowl_bar_panel_add_field_pair(panel, "Load", buf, "5m / 15m",
	                                     buf2);
	{
		gint cores = bar_sysinfo_cpu_cores(info);

		if (cores > 0) {
			gowl_bar_panel_item_set_value_color(item,
				sys_pressure_color(load1 / (gdouble)cores));
		}
	}

	g_snprintf(buf, sizeof(buf), "%d", bar_sysinfo_cpu_cores(info));
	if (temp_mc > 0) {
		bar_activity_format_temp(temp_mc, fahrenheit, buf2,
		                         sizeof(buf2));
		gowl_bar_panel_add_field_pair(panel, "Cores", buf,
		                              "Temperature", buf2);
	} else {
		gowl_bar_panel_add_field(panel, "Cores", buf);
	}
	if (opts != NULL && opts->clock_avg != NULL) {
		gowl_bar_panel_add_field_pair(panel, "Clock", opts->clock_avg,
			"Range", (opts->clock_range != NULL)
			         ? opts->clock_range : "--");
	}

	if (gpu >= 0) {
		g_snprintf(buf, sizeof(buf), "%d%%", gpu);
		gowl_bar_panel_add_field(panel, "GPU", buf);
	}

	/* Memory */
	gowl_bar_panel_add_separator(panel);
	gowl_bar_panel_add_section(panel, "Memory");
	if (total_mb > 0) {
		g_snprintf(buf, sizeof(buf), "%.1f / %.1f GB",
		           (gdouble)used_mb / 1024.0,
		           (gdouble)total_mb / 1024.0);
		item = gowl_bar_panel_add_progress(panel, "In use",
			(gdouble)used_mb / (gdouble)total_mb);
		gowl_bar_panel_item_set_value(item, buf);
		gowl_bar_panel_item_set_color(item,
			sys_pressure_color((gdouble)used_mb /
			                   (gdouble)total_mb));

		g_snprintf(buf, sizeof(buf), "%.1f GB",
		           (gdouble)bar_sysinfo_mem_avail_mb(info) / 1024.0);
		if (swap_total > 0) {
			g_snprintf(buf2, sizeof(buf2), "%.1f / %.1f GB",
			           (gdouble)swap_used / 1024.0,
			           (gdouble)swap_total / 1024.0);
			gowl_bar_panel_add_field_pair(panel, "Available", buf,
			                              "Swap", buf2);
		} else {
			gowl_bar_panel_add_field(panel, "Available", buf);
		}
	}

	/* Storage.  The widget's own mount first, then root when they
	   differ --- the two questions a disk widget is asked. */
	gowl_bar_panel_add_separator(panel);
	gowl_bar_panel_add_section(panel, "Storage");
	{
		const gchar *mount;

		mount = gowl_bar_plugin_get_setting(plugin, "param");
		if (mount == NULL || mount[0] == '\0')
			mount = disk_default_mount();

		if (bar_sysinfo_disk(info, mount, &free_gb, &total_gb) &&
		    total_gb > 0) {
			g_snprintf(buf, sizeof(buf), "%ld / %ld GB",
			           total_gb - free_gb, total_gb);
			item = gowl_bar_panel_add_progress(panel, mount,
				(gdouble)(total_gb - free_gb) /
				(gdouble)total_gb);
			gowl_bar_panel_item_set_value(item, buf);
			gowl_bar_panel_item_set_color(item,
				sys_pressure_color(
					(gdouble)(total_gb - free_gb) /
					(gdouble)total_gb));
		}
		if (strcmp(mount, "/") != 0 &&
		    bar_sysinfo_disk(info, "/", &free_gb, &total_gb) &&
		    total_gb > 0) {
			g_snprintf(buf, sizeof(buf), "%ld / %ld GB",
			           total_gb - free_gb, total_gb);
			item = gowl_bar_panel_add_progress(panel, "/",
				(gdouble)(total_gb - free_gb) /
				(gdouble)total_gb);
			gowl_bar_panel_item_set_value(item, buf);
		}
	}

	{
		glong rd, wr;

		bar_sysinfo_io(info, NULL, &rd, &wr);
		bar_sysinfo_format_rate(rd, buf, sizeof(buf));
		bar_sysinfo_format_rate(wr, buf2, sizeof(buf2));
		{
			gchar rdb[64], wrb[64];

			g_snprintf(rdb, sizeof(rdb), "%s/s", buf);
			g_snprintf(wrb, sizeof(wrb), "%s/s", buf2);
			gowl_bar_panel_add_field_pair(panel, "Read", rdb,
			                              "Write", wrb);
		}
	}

	/* Machine */
	gowl_bar_panel_add_separator(panel);
	gowl_bar_panel_add_section(panel, "Machine");
	bar_sysinfo_format_duration(bar_sysinfo_uptime_seconds(info), buf,
	                            sizeof(buf));
	gowl_bar_panel_add_field_pair(panel, "Uptime", buf, "Host",
	                              bar_sysinfo_hostname(info));
	if (battery >= 0) {
		gint minutes;

		minutes = bar_sysinfo_battery_minutes(info);
		g_snprintf(buf, sizeof(buf), "%d%%%s", battery,
		           bar_sysinfo_battery_charging(info) ? " (charging)"
		                                              : "");
		if (minutes > 0) {
			bar_sysinfo_format_duration((glong)minutes * 60, buf2,
			                            sizeof(buf2));
			gowl_bar_panel_add_field_pair(panel, "Battery", buf,
			                              "Remaining", buf2);
		} else {
			gowl_bar_panel_add_field(panel, "Battery", buf);
		}
	}

	return panel;
}

/* The tool buttons at the foot of every system panel. */
static void
sys_add_tools(GowlBarPanel *panel)
{
	GowlBarPanelItem *item;

	gowl_bar_panel_add_separator(panel);
	item = gowl_bar_panel_add_buttons(panel, "tool");
	gowl_bar_panel_add_button(item, "Monitor", FALSE);
	gowl_bar_panel_add_button(item, "Processes", FALSE);
	gowl_bar_panel_add_button(item, "Disks", FALSE);
}

/* The whole shared panel, as every system widget but cpu opens it. */
static GowlBarPanel *
sys_build_panel(GowlBarPlugin *plugin, const gchar *focus)
{
	GowlBarPanel *panel;

	panel = sys_build_panel_body(plugin, focus, NULL);
	if (panel != NULL)
		sys_add_tools(panel);
	return panel;
}

/* The buttons at the foot of the system panel.  Each runs whatever the
   user configured, falling back to a terminal tool that is probably
   installed --- and doing nothing, visibly, rather than silently, when
   it is not. */
static void
sys_panel_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
                 gint index, gdouble value, guint button)
{
	const gchar *command;
	const gchar *keys[3] = { "monitor-command", "processes-command",
	                         "disks-command" };
	const gchar *defaults[3] = { "btop", "btop", "gdu" };

	(void)data;
	(void)value;
	(void)button;

	if (g_strcmp0(item_id, "tool") != 0 || index < 0 || index > 2)
		return;

	command = gowl_bar_plugin_get_setting(plugin, keys[index]);
	if (command == NULL) {
		if (!bar_have_command(defaults[index])) {
			g_autofree gchar *body = NULL;

			body = g_strdup_printf("%s is not installed.  Set the "
				"widget's %s setting to the program you want "
				"this button to run.", defaults[index],
				keys[index]);
			gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
				"No tool configured", body);
			return;
		}
		/* A terminal tool needs a terminal; the session's own is
		   the only one worth guessing at. */
		{
			g_autofree gchar *line = NULL;
			const gchar *term;

			term = g_getenv("TERMINAL");
			if (term == NULL || term[0] == '\0')
				term = "gst";
			line = g_strdup_printf("%s -e %s", term,
			                       defaults[index]);
			gowl_bar_plugin_spawn(plugin, line);
		}
		return;
	}
	gowl_bar_plugin_spawn(plugin, command);
}

/* ----------------------------------------------------------------
 * cpu, and its activity mode
 *
 * By default this is the widget it always was: `CPU 23%' in the bar
 * and the shared system panel behind it.  Everything the activity mode
 * adds is behind a setting --- the creature (style: creature), the
 * sampling speed, Fahrenheit, clocks, the expanded panel with its
 * process list, and the journal --- so a configuration that never
 * names one sees no change at all.
 *
 * Three threads of state, kept apart deliberately:
 *
 *   - the compositor thread (poll, draw, measure, signature, panel,
 *     actions, canvas) owns the creature, the history ring, the
 *     expand/select/arm state and the filter text;
 *   - the worker (poll_async, and the sample queued when the panel
 *     opens) reads every process, the clocks, nvidia-smi and the
 *     filesystems --- reading /proc for every process is thousands of
 *     small files, not something the compositor may do per tick ---
 *     and swaps the results in under `lock';
 *   - `work_lock' serialises the two ways a worker sample can start,
 *     so the per-process tick table is never walked twice at once.
 * ---------------------------------------------------------------- */

static gint
sys_interval_fast(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 2;
}

/* A second click on Force kill must come within this of the first. */
#define CPU_ARM_US          (5 * G_USEC_PER_SEC)
/* nvidia-smi is a process start and a driver query; every few seconds
   is plenty for a panel someone is reading. */
#define CPU_GPU_EVERY_US    (4 * G_USEC_PER_SEC)
#define CPU_GPU_TIMEOUT_S   5
#define CPU_MOUNTS_EVERY_US (10 * G_USEC_PER_SEC)
/* The journal is pruned at most hourly unless it outgrows its cap. */
#define CPU_PRUNE_EVERY_US  ((gint64)3600 * G_USEC_PER_SEC)
/* The CPU die: one cell's height, the gap, and the legend strip. */
#define CPU_DIE_CELL        26
#define CPU_DIE_GAP         4
#define CPU_DIE_LEGEND      20
#define CPU_FILTER_MAX      64

typedef enum {
	CPU_GPU_UNKNOWN = 0,   /* not asked yet */
	CPU_GPU_NO_TOOL,       /* nvidia-smi is not installed */
	CPU_GPU_FAILED,        /* installed, but it did not answer */
	CPU_GPU_OK
} CpuGpuState;

typedef struct {
	/* --- compositor thread only ------------------------------- */
	gboolean            creature;
	gboolean            fahrenheit;
	gint                interval;
	gint                process_count;
	BarActivityCreature state;
	gint                wiggle;

	/* The Processor graph's history, `history-samples' long. */
	gdouble            *ring;
	gdouble            *ring_out;
	guint               ring_size;
	guint               ring_count;
	guint               ring_head;

	gboolean            expanded;
	gint                open_expanded_applied;  /* -1 before the first */
	gchar              *filter;
	gint                selected_pid;
	guint64             selected_start;
	gint                armed_pid;
	guint64             armed_start;
	gint64              armed_at;

	/* --- shared with the worker, under `lock' ------------------ */
	GMutex              lock;
	gint                cpu, mem, temp_mc;
	gint                sample_interval;
	gchar              *journal_path;   /* NULL while the journal is off */
	gchar              *nvidia_cmd;
	gboolean            want_freq;
	GPtrArray          *procs;          /* BarActivityProc, sorted */
	gboolean            procs_ready;    /* CPU figures are rates */
	GPtrArray          *gpus;           /* BarActivityGpu */
	CpuGpuState         gpu_state;
	GPtrArray          *mounts;         /* BarActivityMount, sized */
	GArray             *freqs;          /* gint MHz per core */
	gchar              *toast_summary;  /* for the compositor to raise */
	gchar              *toast_body;

	/* --- worker only, under `work_lock' ------------------------- */
	GMutex              work_lock;
	GHashTable         *prev_ticks;
	gint64              prev_sample_us;
	gint64              last_journal_us;
	gint64              last_gpu_us;
	gint64              last_mounts_us;
	gint64              last_prune_us;
	gboolean            journal_warned;

	/* --- read by the worker, written by the compositor --------- */
	volatile gint       panel_open;
	volatile gint       expanded_flag;
} CpuData;

static void
cpu_ring_resize(CpuData *cd, guint size)
{
	if (size == cd->ring_size)
		return;
	g_free(cd->ring);
	g_free(cd->ring_out);
	cd->ring       = g_new0(gdouble, size);
	cd->ring_out   = g_new0(gdouble, size);
	cd->ring_size  = size;
	cd->ring_count = 0;
	cd->ring_head  = 0;
}

static void
cpu_ring_push(CpuData *cd, gdouble value)
{
	if (cd->ring_size == 0)
		return;
	cd->ring[cd->ring_head] = CLAMP(value, 0.0, 1.0);
	cd->ring_head = (cd->ring_head + 1) % cd->ring_size;
	if (cd->ring_count < cd->ring_size)
		cd->ring_count++;
}

/* The ring oldest-first, in `ring_out'; valid until the next push. */
static guint
cpu_ring_read(CpuData *cd)
{
	guint i, start;

	start = (cd->ring_count == cd->ring_size) ? cd->ring_head : 0;
	for (i = 0; i < cd->ring_count; i++)
		cd->ring_out[i] = cd->ring[(start + i) % cd->ring_size];
	return cd->ring_count;
}

static gpointer
cpu_create(GowlBarPlugin *plugin)
{
	CpuData *cd = g_new0(CpuData, 1);

	(void)plugin;
	g_mutex_init(&cd->lock);
	g_mutex_init(&cd->work_lock);
	cd->interval              = 2;
	cd->sample_interval       = 2;
	cd->process_count         = 15;
	cd->open_expanded_applied = -1;
	cd->prev_ticks = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                       g_free, g_free);
	cpu_ring_resize(cd, 60);
	return cd;
}

static void
cpu_destroy(GowlBarPlugin *plugin, gpointer data)
{
	CpuData *cd = data;

	(void)plugin;
	if (cd == NULL)
		return;
	g_free(cd->ring);
	g_free(cd->ring_out);
	g_free(cd->filter);
	g_free(cd->journal_path);
	g_free(cd->nvidia_cmd);
	g_clear_pointer(&cd->procs, g_ptr_array_unref);
	g_clear_pointer(&cd->gpus, g_ptr_array_unref);
	g_clear_pointer(&cd->mounts, g_ptr_array_unref);
	g_clear_pointer(&cd->freqs, g_array_unref);
	g_free(cd->toast_summary);
	g_free(cd->toast_body);
	g_hash_table_unref(cd->prev_ticks);
	g_mutex_clear(&cd->lock);
	g_mutex_clear(&cd->work_lock);
	g_free(cd);
}

/*
 * Read every setting into the plugin's state.  Called from configure
 * (a setting changed) and from every poll, which is what makes a value
 * pushed before the host was attached --- or a state directory that
 * only exists once it is --- take effect within one interval.
 */
static void
cpu_apply_settings(GowlBarPlugin *plugin, CpuData *cd)
{
	g_autofree gchar *journal = NULL;
	const gchar *cmd;
	gboolean open_expanded;

	cd->creature = bar_activity_parse_style_creature(
		gowl_bar_plugin_get_setting(plugin, "style"));
	cd->fahrenheit = bar_activity_parse_fahrenheit(
		gowl_bar_plugin_get_setting(plugin, "temperature-unit"));
	cd->interval = bar_activity_sampling_interval(
		gowl_bar_plugin_get_setting(plugin, "sampling"));
	cd->process_count = bar_activity_clamp_setting(
		gowl_bar_plugin_get_setting(plugin, "process-count"),
		15, 1, 100);
	cpu_ring_resize(cd, (guint)bar_activity_clamp_setting(
		gowl_bar_plugin_get_setting(plugin, "history-samples"),
		60, 10, 600));

	/* `open-expanded' is the starting state, applied when it first
	   arrives and again only when it CHANGES --- re-applying it every
	   poll would undo the Compact button a second later. */
	open_expanded = gowl_bar_plugin_get_setting_bool(plugin,
	                                                 "open-expanded",
	                                                 FALSE);
	if ((gint)open_expanded != cd->open_expanded_applied) {
		cd->open_expanded_applied = (gint)open_expanded;
		cd->expanded = open_expanded;
		g_atomic_int_set(&cd->expanded_flag, open_expanded ? 1 : 0);
	}

	if (gowl_bar_plugin_get_setting_bool(plugin, "journal", FALSE)) {
		GowlBarHost *host = gowl_bar_plugin_get_host(plugin);
		const gchar *dir = NULL;
		g_autofree gchar *fallback = NULL;

		if (host != NULL)
			dir = gowl_bar_host_get_state_dir(host);
		if (dir == NULL) {
			fallback = g_build_filename(g_get_user_state_dir(),
			                            "gowl", NULL);
			dir = fallback;
		}
		journal = g_build_filename(dir, "activity.journal", NULL);
	}

	cmd = gowl_bar_plugin_get_setting(plugin, "nvidia-smi");

	g_mutex_lock(&cd->lock);
	if (g_strcmp0(cd->journal_path, journal) != 0) {
		g_free(cd->journal_path);
		cd->journal_path = g_steal_pointer(&journal);
	}
	if (g_strcmp0(cd->nvidia_cmd, cmd) != 0) {
		g_free(cd->nvidia_cmd);
		cd->nvidia_cmd = g_strdup(cmd);
	}
	cd->want_freq = gowl_bar_plugin_get_setting_bool(plugin,
	                                                 "show-frequencies",
	                                                 FALSE);
	cd->sample_interval = cd->interval;
	g_mutex_unlock(&cd->lock);
}

static void
cpu_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	(void)settings;
	if (data != NULL)
		cpu_apply_settings(plugin, data);
}

static gint
cpu_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	return bar_activity_sampling_interval(
		gowl_bar_plugin_get_setting(plugin, "sampling"));
}

/* A worker cannot raise a toast (only the label/icon/colour/tooltip
   setters are safe off the compositor thread), so it leaves one here
   and the next poll or panel build --- both compositor-thread --- puts
   it on screen. */
static void
cpu_flush_toast(GowlBarPlugin *plugin, CpuData *cd)
{
	g_autofree gchar *summary = NULL;
	g_autofree gchar *body = NULL;

	g_mutex_lock(&cd->lock);
	summary = g_steal_pointer(&cd->toast_summary);
	body    = g_steal_pointer(&cd->toast_body);
	g_mutex_unlock(&cd->lock);

	if (summary != NULL)
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL, summary,
		                       body);
}

static void
cpu_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = sys_info();
	CpuData *cd = data;
	gchar buf[32];
	glong used, total;
	gint pct, mem, load, cores, temp_mc;

	if (info == NULL)
		return;

	/* The label is set in both styles: in `label' style it is what is
	   drawn, exactly as before; under the creature it still names the
	   reading for anything that reads labels (and the signature). */
	pct = bar_sysinfo_cpu_percent(info);
	g_snprintf(buf, sizeof(buf), "CPU %d%%", pct);
	gowl_bar_plugin_set_label(plugin, buf);

	/* No instance data until the plugin is activated or configured;
	   until then it is the plain widget it always was. */
	if (cd == NULL) {
		sys_set_icon(plugin, "\xef\x8b\x9b");
		sys_set_color(plugin, (gdouble)pct / 100.0,
		              GOWL_BAR_COLOR_TEXT);
		return;
	}

	cpu_apply_settings(plugin, cd);
	bar_sysinfo_set_cpu_period(info, cd->interval);
	if (cd->creature)
		gowl_bar_plugin_set_icon(plugin, NULL);
	else
		sys_set_icon(plugin, "\xef\x8b\x9b");
	sys_set_color(plugin, (gdouble)pct / 100.0, GOWL_BAR_COLOR_TEXT);

	used    = bar_sysinfo_mem_used_mb(info);
	total   = bar_sysinfo_mem_total_mb(info);
	mem     = (total > 0) ? (gint)(used * 100 / total) : 0;
	cores   = bar_sysinfo_cpu_cores(info);
	load    = (cores > 0)
	          ? (gint)(bar_sysinfo_load1(info) * 100.0 / (gdouble)cores)
	          : 0;
	temp_mc = bar_sysinfo_temp_mc(info);

	cpu_ring_push(cd, (gdouble)pct / 100.0);

	g_mutex_lock(&cd->lock);
	cd->cpu     = pct;
	cd->mem     = mem;
	cd->temp_mc = temp_mc;
	g_mutex_unlock(&cd->lock);

	bar_activity_creature_compute(pct, mem, load,
		bar_activity_clamp_setting(
			gowl_bar_plugin_get_setting(plugin, "load-threshold"),
			50, 0, 99),
		bar_activity_clamp_setting(
			gowl_bar_plugin_get_setting(plugin, "heat-threshold"),
			80, 1, 100),
		&cd->state);
	cd->wiggle = bar_activity_wiggle_offset(g_get_monotonic_time(),
		cd->interval, cd->state.hot,
		gowl_bar_plugin_get_setting_bool(plugin, "motion", TRUE));

	if (cd->creature) {
		g_autofree gchar *tip = NULL;
		gchar temp[24] = "";

		if (temp_mc > 0) {
			gchar t[16];

			bar_activity_format_temp(temp_mc, cd->fahrenheit, t,
			                         sizeof(t));
			g_snprintf(temp, sizeof(temp), " \xc2\xb7 %s", t);
		}
		tip = g_strdup_printf("CPU %d%% \xc2\xb7 Memory %d%% \xc2\xb7 "
		                      "Load %d%%%s%s", pct, mem,
		                      CLAMP(load, 0, 999), temp,
		                      cd->state.hot ? " \xc2\xb7 running hot"
		                                    : "");
		gowl_bar_plugin_set_tooltip(plugin, tip);
	} else {
		gowl_bar_plugin_set_tooltip(plugin, NULL);
	}

	cpu_flush_toast(plugin, cd);
}

/* The creature draws something that is not its label, so it says what
   it draws here or the bar would skip its repaints (bar.org, "Saying
   so in the signature").  The wiggle is in it on purpose: that is what
   makes a hot creature move once per tick with no timer. */
static void
cpu_signature(GowlBarPlugin *plugin, gpointer data, GString *out)
{
	CpuData *cd = data;

	(void)plugin;
	if (cd == NULL || !cd->creature)
		return;
	g_string_append_printf(out, "creature:%u:%d:%d:%d:%d",
	                       cd->state.rows,
	                       (gint)(cd->state.left_eye * 100.0),
	                       (gint)(cd->state.right_eye * 100.0),
	                       cd->state.hot ? 1 : 0, cd->wiggle);
}

static gint
cpu_measure(GowlBarPlugin *plugin, gpointer data, PangoLayout *layout,
            const GowlBarTheme *theme, gint height)
{
	CpuData *cd = data;

	if (cd == NULL || !cd->creature)
		return gowl_bar_plugin_measure_label(plugin, layout, theme,
		                                     height);
	return bar_activity_creature_side(height) +
	       2 * gowl_bar_theme_metric(theme, GOWL_BAR_METRIC_ITEM_PAD);
}

static void
cpu_fill_rect(cairo_t *cr, gdouble ox, gdouble oy,
              const BarActivityRect *r, gdouble radius)
{
	gowl_bar_cairo_rounded_rect(cr, ox + r->x, oy + r->y, r->w, r->h,
	                            radius);
}

/* One tongue of flame in its box: a point at the top, a round base. */
static void
cpu_flame_path(cairo_t *cr, gdouble ox, gdouble oy,
               const BarActivityRect *r)
{
	gdouble x = ox + r->x, y = oy + r->y;

	cairo_move_to(cr, x + r->w * 0.5, y);
	cairo_curve_to(cr, x + r->w * 1.05, y + r->h * 0.45,
	               x + r->w * 1.0, y + r->h,
	               x + r->w * 0.5, y + r->h);
	cairo_curve_to(cr, x, y + r->h, x - r->w * 0.05, y + r->h * 0.45,
	               x + r->w * 0.5, y);
	cairo_close_path(cr);
}

static void
cpu_draw(GowlBarPlugin *plugin, gpointer data, cairo_t *cr,
         PangoLayout *layout, const GowlBarTheme *theme,
         gint x, gint y, gint width, gint height,
         gboolean hovered, gboolean panel_open)
{
	CpuData *cd = data;
	BarActivityCreatureGeom g;
	gdouble body[4], eye[4], load[4], hot[4], flame[4], ox, oy, lw;
	guint i;

	if (cd == NULL || !cd->creature) {
		gowl_bar_plugin_draw_label(plugin, cr, layout, theme, x, y,
		                           width, height, hovered,
		                           panel_open);
		return;
	}

	/* The default drawing with nothing to say still paints the hover
	   wash and the open-panel rule, so the creature behaves like every
	   other widget under the pointer. */
	gowl_bar_plugin_draw_text(plugin, cr, layout, theme, x, y, width,
	                          height, hovered, panel_open, NULL);

	bar_activity_resolve_color(theme,
		gowl_bar_plugin_get_setting(plugin, "color-body"),
		GOWL_BAR_COLOR_SUBTEXT, body);
	bar_activity_resolve_color(theme,
		gowl_bar_plugin_get_setting(plugin, "color-eye"),
		GOWL_BAR_COLOR_ACCENT, eye);
	bar_activity_resolve_color(theme,
		gowl_bar_plugin_get_setting(plugin, "color-load"),
		GOWL_BAR_COLOR_GREEN, load);
	bar_activity_resolve_color(theme,
		gowl_bar_plugin_get_setting(plugin, "color-hot"),
		GOWL_BAR_COLOR_RED, hot);
	bar_activity_resolve_color(theme,
		gowl_bar_plugin_get_setting(plugin, "color-flame"),
		GOWL_BAR_COLOR_PEACH, flame);

	bar_activity_creature_layout(height, &cd->state, &g);
	ox = (gdouble)x + (gdouble)(width - g.side) / 2.0 +
	     (gdouble)cd->wiggle;
	oy = (gdouble)y + (gdouble)(height - g.side) / 2.0;
	lw = MAX(1.0, (gdouble)g.side / 14.0);

	cairo_save(cr);

	/* Body: a faint fill under a firm outline. */
	cpu_fill_rect(cr, ox, oy, &g.body, (gdouble)g.side * 0.2);
	cairo_set_source_rgba(cr, body[0], body[1], body[2], 0.18);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, body[0], body[1], body[2], body[3]);
	cairo_set_line_width(cr, lw);
	cairo_stroke(cr);

	/* Eyes: a socket each, filled from the bottom like a gauge ---
	   left the processor, right the memory. */
	for (i = 0; i < 2; i++) {
		cpu_fill_rect(cr, ox, oy, &g.eye[i], g.eye[i].w * 0.3);
		cairo_set_source_rgba(cr, eye[0], eye[1], eye[2], 0.35);
		cairo_set_line_width(cr, MAX(1.0, lw * 0.6));
		cairo_stroke(cr);
		if (g.eye_fill[i].h > 0.0) {
			cairo_rectangle(cr, ox + g.eye_fill[i].x,
			                oy + g.eye_fill[i].y, g.eye_fill[i].w,
			                g.eye_fill[i].h);
			cairo_set_source_rgba(cr, eye[0], eye[1], eye[2],
			                      eye[3]);
			cairo_fill(cr);
		}
	}

	/* Rows: lit from the bottom with the load; the unlit slots stay
	   faintly visible, so a calm creature still has a belly. */
	for (i = 0; i < BAR_ACTIVITY_CREATURE_ROWS; i++) {
		const gdouble *c = cd->state.hot ? hot : load;

		cpu_fill_rect(cr, ox, oy, &g.row[i], g.row[i].h * 0.4);
		if (i < cd->state.rows)
			cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);
		else
			cairo_set_source_rgba(cr, body[0], body[1], body[2],
			                      0.15);
		cairo_fill(cr);
	}

	if (cd->state.hot) {
		for (i = 0; i < BAR_ACTIVITY_CREATURE_FLAMES; i++) {
			cpu_flame_path(cr, ox, oy, &g.flame[i]);
			cairo_set_source_rgba(cr, flame[0], flame[1],
			                      flame[2], flame[3]);
			cairo_fill(cr);
		}
	}

	cairo_restore(cr);
}

/* ---- The CPU die ----------------------------------------------- */

static const GowlBarColor cpu_band_roles[5] = {
	GOWL_BAR_COLOR_OVERLAY, GOWL_BAR_COLOR_GREEN, GOWL_BAR_COLOR_YELLOW,
	GOWL_BAR_COLOR_PEACH, GOWL_BAR_COLOR_RED
};
static const gchar *const cpu_band_names[5] = {
	"idle", "25%+", "50%+", "75%+", "90%+"
};

static gint
cpu_die_height(guint n_cores)
{
	guint cols, rows;

	bar_activity_die_grid(n_cores, &cols, &rows);
	if (rows == 0)
		return 0;
	return (gint)(rows * (CPU_DIE_CELL + CPU_DIE_GAP)) + CPU_DIE_LEGEND;
}

static PangoLayout *
cpu_small_layout(cairo_t *cr, const GowlBarTheme *theme)
{
	PangoLayout *layout;
	PangoFontDescription *desc;
	const gchar *font;
	gint size;

	layout = pango_cairo_create_layout(cr);
	font = gowl_bar_theme_get_font(theme);
	desc = pango_font_description_from_string(
		(font != NULL) ? font : "monospace 10");
	size = pango_font_description_get_size(desc);
	if (size > 0)
		pango_font_description_set_size(desc, size * 3 / 4);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	return layout;
}

static void
cpu_panel_canvas(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
                 cairo_t *cr, const GowlBarTheme *theme, gint width,
                 gint height)
{
	BarSysinfo *info = sys_info();
	CpuData *cd = data;
	g_autoptr(GArray) freqs = NULL;
	PangoLayout *layout;
	const gint *cores;
	guint n = 0, cols, rows, i;
	gdouble cell_w, lx;
	gint legend_y;

	(void)plugin;
	(void)height;

	if (g_strcmp0(item_id, "die") != 0 || info == NULL || cd == NULL)
		return;
	cores = bar_sysinfo_core_percents(info, &n);
	if (cores == NULL || n == 0)
		return;
	bar_activity_die_grid(n, &cols, &rows);
	cell_w = ((gdouble)width - (gdouble)(cols - 1) * CPU_DIE_GAP) /
	         (gdouble)cols;

	/* A copy, so the worker can swap a fresh array in mid-paint. */
	g_mutex_lock(&cd->lock);
	if (cd->want_freq && cd->freqs != NULL)
		freqs = g_array_copy(cd->freqs);
	g_mutex_unlock(&cd->lock);

	layout = cpu_small_layout(cr, theme);
	for (i = 0; i < n; i++) {
		gdouble cx, cy;
		guint band;
		gchar text[32];
		PangoRectangle ext;

		cx = (gdouble)(i % cols) * (cell_w + CPU_DIE_GAP);
		cy = (gdouble)(i / cols) * (CPU_DIE_CELL + CPU_DIE_GAP);
		band = bar_activity_core_band(cores[i]);

		gowl_bar_cairo_rounded_rect(cr, cx, cy, cell_w, CPU_DIE_CELL,
		                            4.0);
		gowl_bar_theme_cairo_set_alpha(theme, cr, cpu_band_roles[band],
		                               band == 0 ? 0.6 : 0.9);
		cairo_fill(cr);

		/* The figure, if it fits; with clocks on and room for them,
		   the clock too. */
		if (cell_w < 22.0)
			continue;
		if (freqs != NULL && i < freqs->len &&
		    g_array_index(freqs, gint, i) > 0 && cell_w >= 64.0)
			g_snprintf(text, sizeof(text), "%d%% %.1fG", cores[i],
			           (gdouble)g_array_index(freqs, gint, i) /
			           1000.0);
		else
			g_snprintf(text, sizeof(text), "%d", cores[i]);
		pango_layout_set_text(layout, text, -1);
		pango_layout_get_pixel_extents(layout, NULL, &ext);
		gowl_bar_theme_cairo_set(theme, cr, band == 0
		                         ? GOWL_BAR_COLOR_TEXT
		                         : GOWL_BAR_COLOR_BASE);
		cairo_move_to(cr, cx + (cell_w - ext.width) / 2.0,
		              cy + (CPU_DIE_CELL - ext.height) / 2.0);
		pango_cairo_show_layout(cr, layout);
	}

	/* The key, under the grid. */
	legend_y = (gint)(rows * (CPU_DIE_CELL + CPU_DIE_GAP)) + 4;
	lx = 0.0;
	for (i = 0; i < G_N_ELEMENTS(cpu_band_roles); i++) {
		PangoRectangle ext;

		gowl_bar_cairo_rounded_rect(cr, lx, legend_y + 2, 10, 10, 2.0);
		gowl_bar_theme_cairo_set(theme, cr, cpu_band_roles[i]);
		cairo_fill(cr);
		pango_layout_set_text(layout, cpu_band_names[i], -1);
		pango_layout_get_pixel_extents(layout, NULL, &ext);
		gowl_bar_theme_cairo_set(theme, cr, GOWL_BAR_COLOR_SUBTEXT);
		cairo_move_to(cr, lx + 14, legend_y + 7 - ext.height / 2);
		pango_cairo_show_layout(cr, layout);
		lx += 14 + ext.width + 12;
	}
	g_object_unref(layout);
}

/* ---- The panel ------------------------------------------------- */

static void
cpu_add_die(GowlBarPanel *panel, BarSysinfo *info)
{
	const gint *cores;
	guint n = 0;

	gowl_bar_panel_add_separator(panel);
	gowl_bar_panel_add_section(panel, "CPU die");
	cores = bar_sysinfo_core_percents(info, &n);
	if (cores == NULL || n == 0) {
		gowl_bar_panel_add_label(panel,
			"Per-core load appears after the next sample.");
		return;
	}
	gowl_bar_panel_add_canvas(panel, "die", cpu_die_height(n));
}

static void
cpu_add_gpu(GowlBarPanel *panel, BarSysinfo *info, CpuData *cd)
{
	GowlBarPanelItem *item;
	gchar buf[128], temp[24];
	gint amd;
	guint i;
	gboolean any = FALSE;

	gowl_bar_panel_add_separator(panel);
	gowl_bar_panel_add_section(panel, "GPU");

	/* amdgpu reports through sysfs, which the shared reader already
	   watches; nothing to run for it. */
	amd = bar_sysinfo_gpu_percent(info);
	if (amd >= 0) {
		g_snprintf(buf, sizeof(buf), "%d%%", amd);
		item = gowl_bar_panel_add_progress(panel, "Utilisation",
		                                   (gdouble)amd / 100.0);
		gowl_bar_panel_item_set_value(item, buf);
		gowl_bar_panel_item_set_color(item,
			sys_pressure_color((gdouble)amd / 100.0));
		any = TRUE;
	}

	g_mutex_lock(&cd->lock);
	for (i = 0; cd->gpus != NULL && i < cd->gpus->len; i++) {
		BarActivityGpu *gpu = g_ptr_array_index(cd->gpus, i);
		gdouble frac = (gpu->util >= 0) ? gpu->util / 100.0 : 0.0;

		temp[0] = '\0';
		if (gpu->temp_c >= 0) {
			gchar t[16];

			bar_activity_format_temp(gpu->temp_c * 1000,
			                         cd->fahrenheit, t, sizeof(t));
			g_snprintf(temp, sizeof(temp), " \xc2\xb7 %s", t);
		}
		if (gpu->mem_total_mb > 0)
			g_snprintf(buf, sizeof(buf), "%d%% \xc2\xb7 %d / %d MB%s",
			           MAX(gpu->util, 0), MAX(gpu->mem_used_mb, 0),
			           gpu->mem_total_mb, temp);
		else
			g_snprintf(buf, sizeof(buf), "%d%%%s",
			           MAX(gpu->util, 0), temp);
		item = gowl_bar_panel_add_progress(panel, gpu->name, frac);
		gowl_bar_panel_item_set_value(item, buf);
		gowl_bar_panel_item_set_color(item, sys_pressure_color(frac));
		any = TRUE;
	}
	if (!any) {
		switch (cd->gpu_state) {
		case CPU_GPU_NO_TOOL:
			gowl_bar_panel_add_label(panel,
				"No GPU reports its load.  AMD cards report "
				"through the amdgpu driver on their own; for an "
				"NVIDIA card install nvidia-smi (Fedora: "
				"xorg-x11-drv-nvidia-cuda, from RPM Fusion).");
			break;
		case CPU_GPU_FAILED:
			gowl_bar_panel_add_label(panel,
				"nvidia-smi is installed but did not answer --- "
				"is the NVIDIA driver loaded?");
			break;
		default:
			gowl_bar_panel_add_label(panel, "Asking the GPU...");
			break;
		}
	}
	g_mutex_unlock(&cd->lock);
}

static void
cpu_add_filesystems(GowlBarPanel *panel, CpuData *cd)
{
	guint i;

	gowl_bar_panel_add_separator(panel);
	gowl_bar_panel_add_section(panel, "Filesystems");

	g_mutex_lock(&cd->lock);
	if (cd->mounts == NULL) {
		gowl_bar_panel_add_label(panel, "Reading mounts...");
	} else if (cd->mounts->len == 0) {
		gowl_bar_panel_add_label(panel, "No local disk is mounted.");
	}
	for (i = 0; cd->mounts != NULL && i < cd->mounts->len; i++) {
		BarActivityMount *m = g_ptr_array_index(cd->mounts, i);
		GowlBarPanelItem *item;
		gchar used[32], tot[32], val[80];
		gdouble frac;

		frac = (gdouble)m->used_bytes / (gdouble)m->total_bytes;
		bar_sysinfo_format_bytes((glong)m->used_bytes, used,
		                         sizeof(used));
		bar_sysinfo_format_bytes((glong)m->total_bytes, tot,
		                         sizeof(tot));
		g_snprintf(val, sizeof(val), "%s / %s", used, tot);
		item = gowl_bar_panel_add_progress(panel, m->mount, frac);
		gowl_bar_panel_item_set_value(item, val);
		gowl_bar_panel_item_set_color(item, sys_pressure_color(frac));
	}
	g_mutex_unlock(&cd->lock);
}

static gboolean
cpu_is_armed(CpuData *cd, gint pid, guint64 start)
{
	return cd->armed_pid == pid && cd->armed_start == start &&
	       g_get_monotonic_time() - cd->armed_at < CPU_ARM_US;
}

static void
cpu_add_processes(GowlBarPanel *panel, CpuData *cd)
{
	GowlBarPanelItem *item;
	guint i, shown = 0;
	gboolean ready;

	gowl_bar_panel_add_separator(panel);
	gowl_bar_panel_add_section(panel, "Processes");
	gowl_bar_panel_add_entry(panel, "filter", "Filter by name or pid",
	                         (cd->filter != NULL) ? cd->filter : "");

	g_mutex_lock(&cd->lock);
	if (cd->procs == NULL) {
		g_mutex_unlock(&cd->lock);
		gowl_bar_panel_add_label(panel, "Sampling your processes...");
		return;
	}
	ready = cd->procs_ready;
	for (i = 0; i < cd->procs->len; i++) {
		BarActivityProc *p = g_ptr_array_index(cd->procs, i);
		g_autofree gchar *id = NULL;
		gchar rss[32], sub[96], val[24];

		if (!bar_activity_proc_matches(p, cd->filter))
			continue;
		if ((gint)shown >= cd->process_count)
			break;
		shown++;

		id = g_strdup_printf("proc:%d:%" G_GUINT64_FORMAT ":%u",
		                     p->pid, p->starttime, p->uid);
		bar_sysinfo_format_bytes((glong)(p->rss_kb * 1024), rss,
		                         sizeof(rss));
		g_snprintf(sub, sizeof(sub), "pid %d \xc2\xb7 %s \xc2\xb7 %c",
		           p->pid, rss, p->state);
		if (p->cpu >= 0.0)
			g_snprintf(val, sizeof(val), "%.1f%%", p->cpu);
		else
			g_strlcpy(val, "--", sizeof(val));

		item = gowl_bar_panel_add_row(panel, id, NULL, p->name, sub);
		gowl_bar_panel_item_set_value(item, val);
		if (p->cpu >= 0.0)
			gowl_bar_panel_item_set_value_color(item,
				sys_pressure_color(p->cpu / 100.0));

		if (p->pid == cd->selected_pid &&
		    p->starttime == cd->selected_start) {
			g_autofree gchar *sig_id = NULL;

			gowl_bar_panel_item_set_selected(item, TRUE);
			sig_id = g_strdup_printf("sig:%d:%" G_GUINT64_FORMAT
			                         ":%u", p->pid, p->starttime,
			                         p->uid);
			item = gowl_bar_panel_add_buttons(panel, sig_id);
			gowl_bar_panel_add_button(item, "Terminate", FALSE);
			gowl_bar_panel_add_button(item,
				cpu_is_armed(cd, p->pid, p->starttime)
				? "Click again to force kill" : "Force kill",
				FALSE);
		}
	}
	g_mutex_unlock(&cd->lock);

	if (shown == 0) {
		gowl_bar_panel_add_label(panel,
			(cd->filter != NULL && cd->filter[0] != '\0')
			? "None of your processes match the filter."
			: "No processes of yours were found.");
	} else if (!ready) {
		gowl_bar_panel_add_label(panel,
			"CPU figures appear after the next sample.");
	}
}

/* Clock summary for the Processor section, from the worker's last
   cpufreq read.  @avg and @range are left empty when there is none. */
static void
cpu_clock_summary(CpuData *cd, gchar *avg, gsize avg_sz, gchar *range,
                  gsize range_sz)
{
	guint i, n = 0;
	gint lo = G_MAXINT, hi = 0;
	gint64 sum = 0;

	avg[0]   = '\0';
	range[0] = '\0';
	g_mutex_lock(&cd->lock);
	if (!cd->want_freq) {
		g_mutex_unlock(&cd->lock);
		return;
	}
	if (cd->freqs == NULL) {
		g_mutex_unlock(&cd->lock);
		g_strlcpy(avg, "reading...", avg_sz);
		return;
	}
	for (i = 0; i < cd->freqs->len; i++) {
		gint mhz = g_array_index(cd->freqs, gint, i);

		if (mhz <= 0)
			continue;
		sum += mhz;
		lo = MIN(lo, mhz);
		hi = MAX(hi, mhz);
		n++;
	}
	g_mutex_unlock(&cd->lock);

	if (n == 0) {
		g_strlcpy(avg, "not reported", avg_sz);
		return;
	}
	g_snprintf(avg, avg_sz, "%d MHz", (gint)(sum / n));
	g_snprintf(range, range_sz, "%d\xe2\x80\x93%d MHz", lo, hi);
}

static GowlBarPanel *
cpu_panel(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = sys_info();
	CpuData *cd = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	SysPanelOpts opts;
	gchar avg[48], range[48];
	gboolean journal;

	if (cd == NULL)
		return sys_build_panel(plugin, "cpu");
	if (info == NULL)
		return NULL;

	cpu_clock_summary(cd, avg, sizeof(avg), range, sizeof(range));
	memset(&opts, 0, sizeof(opts));
	opts.fahrenheit    = cd->fahrenheit;
	opts.n_cpu_samples = cpu_ring_read(cd);
	opts.cpu_samples   = (opts.n_cpu_samples > 0) ? cd->ring_out : NULL;
	opts.clock_avg     = (avg[0] != '\0') ? avg : NULL;
	opts.clock_range   = (range[0] != '\0') ? range : NULL;

	panel = sys_build_panel_body(plugin, "cpu", &opts);
	if (panel == NULL)
		return NULL;
	cpu_flush_toast(plugin, cd);

	cpu_add_die(panel, info);
	item = gowl_bar_panel_add_buttons(panel, "view");
	gowl_bar_panel_add_button(item, cd->expanded ? "Compact" : "Expand",
	                          FALSE);

	if (cd->expanded) {
		cpu_add_gpu(panel, info, cd);
		cpu_add_filesystems(panel, cd);
		cpu_add_processes(panel, cd);
	}

	g_mutex_lock(&cd->lock);
	journal = (cd->journal_path != NULL);
	g_mutex_unlock(&cd->lock);
	if (journal) {
		gowl_bar_panel_add_separator(panel);
		item = gowl_bar_panel_add_buttons(panel, "journal");
		gowl_bar_panel_add_button(item, "Export journal", FALSE);
	}

	sys_add_tools(panel);
	return panel;
}

/* ---- The worker ------------------------------------------------ */

static void
cpu_sample_gpu(const gchar *cmd, GPtrArray **gpus, CpuGpuState *state)
{
	g_auto(GStrv) probe = NULL;
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *out = NULL;

	*gpus = NULL;
	probe = bar_activity_nvidia_argv(cmd, 0);
	if (!bar_have_command(probe[0])) {
		*state = CPU_GPU_NO_TOOL;
		return;
	}
	argv = bar_activity_nvidia_argv(cmd,
		bar_have_command("timeout") ? CPU_GPU_TIMEOUT_S : 0);
	out = bar_run_argv((const gchar * const *)argv);
	*gpus = bar_activity_parse_nvidia_csv(out);
	*state = ((*gpus)->len > 0) ? CPU_GPU_OK : CPU_GPU_FAILED;
}

static GPtrArray *
cpu_sample_mounts(void)
{
	g_autofree gchar *text = NULL;
	GPtrArray *mounts;
	guint i;

	if (!g_file_get_contents("/proc/mounts", &text, NULL, NULL))
		return bar_activity_parse_mounts(NULL);
	mounts = bar_activity_parse_mounts(text);
	for (i = 0; i < mounts->len; ) {
		BarActivityMount *m = g_ptr_array_index(mounts, i);
		struct statvfs st;

		if (statvfs(m->mount, &st) != 0 || st.f_blocks == 0) {
			g_ptr_array_remove_index(mounts, i);
			continue;
		}
		m->total_bytes = (guint64)st.f_blocks * st.f_frsize;
		m->used_bytes  = (guint64)(st.f_blocks - st.f_bfree) *
		                 st.f_frsize;
		i++;
	}
	return mounts;
}

/* Append this sample to the journal, pruning it now and then. */
static void
cpu_sample_journal(CpuData *cd, const gchar *path, gint cpu, gint mem,
                   gint temp_mc, gint interval, gint64 now)
{
	g_autofree gchar *line = NULL;
	GError *error = NULL;
	GStatBuf st;

	/* A sample queued when the panel opens must not double a line
	   the interval poll is about to write. */
	if (cd->last_journal_us != 0 &&
	    now - cd->last_journal_us <
	    (gint64)interval * G_USEC_PER_SEC - G_USEC_PER_SEC / 4)
		return;
	cd->last_journal_us = now;

	line = bar_activity_journal_line(g_get_real_time() / G_USEC_PER_SEC,
	                                 cpu, mem, temp_mc);
	if (!bar_activity_journal_append(path, line, &error)) {
		if (!cd->journal_warned)
			g_warning("cpu: journal: %s", error->message);
		cd->journal_warned = TRUE;
		g_clear_error(&error);
		return;
	}

	if (cd->last_prune_us == 0 ||
	    now - cd->last_prune_us >= CPU_PRUNE_EVERY_US ||
	    (g_stat(path, &st) == 0 &&
	     st.st_size > BAR_ACTIVITY_JOURNAL_MAX_BYTES)) {
		cd->last_prune_us = now;
		if (!bar_activity_journal_prune(path,
			    g_get_real_time() / G_USEC_PER_SEC,
			    BAR_ACTIVITY_JOURNAL_MAX_AGE,
			    BAR_ACTIVITY_JOURNAL_MAX_BYTES, &error)) {
			g_warning("cpu: journal prune: %s", error->message);
			g_clear_error(&error);
		}
	}
}

/*
 * One worker sample.  Everything here may block: the journal write, a
 * walk of every /proc/PID, the cpufreq reads (each may interrupt the
 * core it asks about), nvidia-smi and statvfs.  None of it is done
 * unless something will show it --- the process list, GPU and mounts
 * only while the panel is open AND expanded --- so an idle bar costs a
 * journal line at most.
 */
static void
cpu_sample(GowlBarPlugin *plugin, CpuData *cd)
{
	g_autofree gchar *journal = NULL;
	g_autofree gchar *nvidia = NULL;
	gboolean want_freq;
	gint cpu, mem, temp_mc, interval;
	gint64 now;

	g_mutex_lock(&cd->work_lock);

	g_mutex_lock(&cd->lock);
	journal   = g_strdup(cd->journal_path);
	nvidia    = g_strdup(cd->nvidia_cmd);
	want_freq = cd->want_freq;
	cpu       = cd->cpu;
	mem       = cd->mem;
	temp_mc   = cd->temp_mc;
	interval  = cd->sample_interval;
	g_mutex_unlock(&cd->lock);

	now = g_get_monotonic_time();
	if (journal != NULL)
		cpu_sample_journal(cd, journal, cpu, mem, temp_mc, interval,
		                   now);

	if (!g_atomic_int_get(&cd->panel_open)) {
		g_mutex_unlock(&cd->work_lock);
		return;
	}

	if (want_freq) {
		GArray *freqs = bar_activity_read_frequencies("/sys");

		g_mutex_lock(&cd->lock);
		g_clear_pointer(&cd->freqs, g_array_unref);
		cd->freqs = freqs;
		g_mutex_unlock(&cd->lock);
	}

	if (g_atomic_int_get(&cd->expanded_flag)) {
		GPtrArray *procs;
		gdouble elapsed;
		gboolean had_prev;

		/* Two samples a few milliseconds apart (the panel opened
		   just before a poll) say nothing about a rate; keep the
		   list shown and the baseline as they are. */
		elapsed  = (cd->prev_sample_us > 0)
		           ? (gdouble)(now - cd->prev_sample_us) / 1e6 : 0.0;
		had_prev = g_hash_table_size(cd->prev_ticks) > 0;
		if (!had_prev || elapsed >= 0.5) {
			procs = bar_activity_read_processes("/proc",
			                                    (gint)getuid());
			bar_activity_compute_cpu(procs, cd->prev_ticks, elapsed,
			                         sysconf(_SC_CLK_TCK));
			bar_activity_sort_processes(procs);
			cd->prev_sample_us = now;

			g_mutex_lock(&cd->lock);
			g_clear_pointer(&cd->procs, g_ptr_array_unref);
			cd->procs       = procs;
			cd->procs_ready = had_prev;
			g_mutex_unlock(&cd->lock);
		}

		if (cd->last_gpu_us == 0 ||
		    now - cd->last_gpu_us >= CPU_GPU_EVERY_US) {
			GPtrArray *gpus = NULL;
			CpuGpuState state = CPU_GPU_UNKNOWN;

			cd->last_gpu_us = now;
			cpu_sample_gpu(nvidia, &gpus, &state);
			g_mutex_lock(&cd->lock);
			g_clear_pointer(&cd->gpus, g_ptr_array_unref);
			cd->gpus      = gpus;
			cd->gpu_state = state;
			g_mutex_unlock(&cd->lock);
		}

		if (cd->last_mounts_us == 0 ||
		    now - cd->last_mounts_us >= CPU_MOUNTS_EVERY_US) {
			GPtrArray *mounts;

			cd->last_mounts_us = now;
			mounts = cpu_sample_mounts();
			g_mutex_lock(&cd->lock);
			g_clear_pointer(&cd->mounts, g_ptr_array_unref);
			cd->mounts = mounts;
			g_mutex_unlock(&cd->lock);
		}
	}

	g_mutex_unlock(&cd->work_lock);
	gowl_bar_plugin_request_panel_refresh(plugin);
}

static void
cpu_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	if (data != NULL)
		cpu_sample(plugin, data);
}

static CpuData *
cpu_data_of(GowlBarPlugin *plugin)
{
	if (!GOWL_IS_BAR_PLUGIN_PROXY(plugin))
		return NULL;
	return gowl_bar_plugin_proxy_get_data(GOWL_BAR_PLUGIN_PROXY(plugin));
}

/* Queued when the panel opens or expands, so it fills now rather than
   at the next interval. */
static void
cpu_sample_work(GowlBarPlugin *plugin, gpointer user_data)
{
	CpuData *cd = cpu_data_of(plugin);

	(void)user_data;
	if (cd != NULL)
		cpu_sample(plugin, cd);
}

/* Runs on the worker: the journal may be 20 MiB. */
static void
cpu_export_work(GowlBarPlugin *plugin, gpointer user_data)
{
	const gchar *path = user_data;
	CpuData *cd = cpu_data_of(plugin);
	g_autofree gchar *dest = NULL;
	GError *error = NULL;
	gchar *summary, *body;

	if (cd == NULL)
		return;
	dest = bar_activity_export_path(g_get_home_dir(), NULL);
	if (bar_activity_journal_export(path, dest, &error)) {
		summary = g_strdup("Activity journal exported");
		body    = g_strdup(dest);
	} else {
		summary = g_strdup("Activity journal not exported");
		body    = g_error_matches(error, G_FILE_ERROR,
		                          G_FILE_ERROR_NOENT)
		          ? g_strdup("Nothing has been recorded yet.")
		          : g_strdup(error->message);
		g_clear_error(&error);
	}

	g_mutex_lock(&cd->lock);
	g_free(cd->toast_summary);
	g_free(cd->toast_body);
	cd->toast_summary = summary;
	cd->toast_body    = body;
	g_mutex_unlock(&cd->lock);
	gowl_bar_plugin_request_panel_refresh(plugin);
}

/* ---- Actions --------------------------------------------------- */

/*
 * Signal one of the user's processes.  The list is up to one interval
 * old, so the process is re-read first: its pid could belong to a
 * different program by now, and a signal is not something to send on
 * a guess.  Init and the compositor itself are refused outright.
 */
static void
cpu_signal(GowlBarPlugin *plugin, CpuData *cd, gint pid, guint64 start,
           guint uid, gint signo)
{
	BarActivityKillCheck check;
	g_autofree gchar *name = NULL;
	g_autofree gchar *summary = NULL;
	guint i;

	g_mutex_lock(&cd->lock);
	for (i = 0; cd->procs != NULL && i < cd->procs->len; i++) {
		BarActivityProc *p = g_ptr_array_index(cd->procs, i);

		if (p->pid == pid && p->starttime == start) {
			name = g_strdup(p->name);
			break;
		}
	}
	g_mutex_unlock(&cd->lock);
	if (name == NULL)
		name = g_strdup_printf("process %d", pid);

	check = bar_activity_kill_check("/proc", pid, start, uid,
	                                (guint)getuid(), (gint)getpid());
	if (check != BAR_ACTIVITY_KILL_OK) {
		summary = g_strdup_printf("%s was not signalled", name);
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL, summary,
		                       bar_activity_kill_check_reason(check));
		return;
	}
	if (kill((pid_t)pid, signo) != 0) {
		gint e = errno;

		summary = g_strdup_printf("Could not signal %s", name);
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL, summary,
		                       g_strerror(e));
		return;
	}
	summary = g_strdup_printf("Sent %s to %s (%d)",
	                          signo == SIGKILL ? "SIGKILL" : "SIGTERM",
	                          name, pid);
	gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW, summary, NULL);
	cd->selected_pid = 0;
	cd->armed_pid    = 0;
}

static void
cpu_panel_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
                 gint index, gdouble value, guint button)
{
	CpuData *cd = data;
	gint pid;
	guint64 start;
	guint uid;

	if (g_strcmp0(item_id, "tool") == 0 || cd == NULL) {
		sys_panel_action(plugin, data, item_id, index, value, button);
		return;
	}

	if (g_strcmp0(item_id, "view") == 0) {
		cd->expanded = !cd->expanded;
		g_atomic_int_set(&cd->expanded_flag, cd->expanded ? 1 : 0);
		if (cd->expanded)
			gowl_bar_plugin_queue_work(plugin, cpu_sample_work,
			                           NULL, NULL);
		return;
	}

	if (g_strcmp0(item_id, "journal") == 0) {
		gchar *path;

		g_mutex_lock(&cd->lock);
		path = g_strdup(cd->journal_path);
		g_mutex_unlock(&cd->lock);
		if (path != NULL)
			gowl_bar_plugin_queue_work(plugin, cpu_export_work,
			                           path, g_free);
		return;
	}

	/* A row selects its process (and a second click deselects it);
	   the actions appear under the selected row only, so a stray
	   click in a long list never signals anything. */
	if (bar_activity_parse_proc_id(item_id, "proc", &pid, &start, &uid)) {
		if (cd->selected_pid == pid && cd->selected_start == start) {
			cd->selected_pid = 0;
		} else {
			cd->selected_pid   = pid;
			cd->selected_start = start;
		}
		cd->armed_pid = 0;
		return;
	}

	if (bar_activity_parse_proc_id(item_id, "sig", &pid, &start, &uid)) {
		if (index == 0) {
			cpu_signal(plugin, cd, pid, start, uid, SIGTERM);
		} else if (index == 1) {
			/* Two steps: the first click arms, the second ---
			   within five seconds --- kills. */
			if (cpu_is_armed(cd, pid, start)) {
				cd->armed_pid = 0;
				cpu_signal(plugin, cd, pid, start, uid,
				           SIGKILL);
			} else {
				cd->armed_pid   = pid;
				cd->armed_start = start;
				cd->armed_at    = g_get_monotonic_time();
			}
		}
	}
}

static void
cpu_panel_text(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
               const gchar *text, gboolean submitted)
{
	CpuData *cd = data;

	(void)plugin;
	(void)submitted;
	if (cd == NULL || g_strcmp0(item_id, "filter") != 0)
		return;
	g_free(cd->filter);
	cd->filter = (text != NULL)
	             ? g_utf8_substring(text, 0,
	                                MIN(g_utf8_strlen(text, -1),
	                                    CPU_FILTER_MAX))
	             : NULL;
}

static void
cpu_panel_opened(GowlBarPlugin *plugin, gpointer data)
{
	CpuData *cd = data;

	if (cd == NULL)
		return;
	g_atomic_int_set(&cd->panel_open, 1);
	gowl_bar_plugin_queue_work(plugin, cpu_sample_work, NULL, NULL);
}

static void
cpu_panel_closed(GowlBarPlugin *plugin, gpointer data)
{
	CpuData *cd = data;

	(void)plugin;
	if (cd == NULL)
		return;
	g_atomic_int_set(&cd->panel_open, 0);
	cd->armed_pid = 0;
}

static const GowlBarPluginVTable cpu_vtable = {
	sizeof(GowlBarPluginVTable),
	cpu_create, cpu_destroy, NULL, NULL, cpu_configure,
	cpu_interval, cpu_poll, cpu_poll_async,
	cpu_measure, cpu_draw, NULL, NULL,
	cpu_panel, cpu_panel_action,
	cpu_panel_opened, cpu_panel_closed,
	NULL,
	cpu_signature,
	cpu_panel_text, cpu_panel_canvas
};

/* ----------------------------------------------------------------
 * memory and swap
 * ---------------------------------------------------------------- */

static void
memory_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = sys_info();
	gchar buf[48];
	glong used, total;

	(void)data;

	if (info == NULL)
		return;
	used  = bar_sysinfo_mem_used_mb(info);
	total = bar_sysinfo_mem_total_mb(info);

	if (used >= 1024)
		g_snprintf(buf, sizeof(buf), "MEM %.1fG",
		           (gdouble)used / 1024.0);
	else
		g_snprintf(buf, sizeof(buf), "MEM %ldM", used);
	gowl_bar_plugin_set_label(plugin, buf);
	sys_set_icon(plugin, "\xef\x94\xb8");
	sys_set_color(plugin,
	              (total > 0) ? (gdouble)used / (gdouble)total : -1.0,
	              GOWL_BAR_COLOR_TEXT);
}

static GowlBarPanel *
memory_panel(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	return sys_build_panel(plugin, "memory");
}

static const GowlBarPluginVTable memory_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL, NULL, NULL, NULL,
	NULL, memory_poll, NULL,
	NULL, NULL, NULL, NULL,
	memory_panel, sys_panel_action,
	NULL, NULL,
	NULL,
	NULL,
	NULL, NULL
};

static void
swap_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = sys_info();
	gchar buf[64];
	glong used, total;

	(void)data;

	if (info == NULL)
		return;
	used  = bar_sysinfo_swap_used_mb(info);
	total = bar_sysinfo_swap_total_mb(info);

	if (total <= 0) {
		/* A machine with no swap should not show an empty widget;
		   hiding it keeps the bar honest about what exists. */
		gowl_bar_plugin_set_label(plugin, NULL);
		return;
	}
	if (used >= 1024)
		g_snprintf(buf, sizeof(buf), "SWAP %.1fG/%.1fG",
		           (gdouble)used / 1024.0, (gdouble)total / 1024.0);
	else
		g_snprintf(buf, sizeof(buf), "SWAP %ldM/%ldM", used, total);
	gowl_bar_plugin_set_label(plugin, buf);
	sys_set_color(plugin, (gdouble)used / (gdouble)total,
	              GOWL_BAR_COLOR_TEXT);
}

static GowlBarPanel *
swap_panel(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	return sys_build_panel(plugin, "memory");
}

static const GowlBarPluginVTable swap_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL, NULL, NULL, NULL,
	NULL, swap_poll, NULL,
	NULL, NULL, NULL, NULL,
	swap_panel, sys_panel_action,
	NULL, NULL,
	NULL,
	NULL,
	NULL, NULL
};

/* ----------------------------------------------------------------
 * disk
 * ---------------------------------------------------------------- */

static gint
sys_interval_slow(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 30;
}

static void
disk_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = sys_info();
	const gchar *mount;
	gchar buf[128];
	glong free_gb, total_gb;

	(void)data;

	if (info == NULL)
		return;
	mount = gowl_bar_plugin_get_setting(plugin, "param");
	if (mount == NULL || mount[0] == '\0')
		mount = disk_default_mount();

	if (!bar_sysinfo_disk(info, mount, &free_gb, &total_gb)) {
		g_snprintf(buf, sizeof(buf), "%s ?", mount);
		gowl_bar_plugin_set_label(plugin, buf);
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_MUTED);
		return;
	}

	g_snprintf(buf, sizeof(buf), "%s %ldG", mount, free_gb);
	gowl_bar_plugin_set_label(plugin, buf);
	sys_set_icon(plugin, "\xef\x82\xa0");
	/* Pressure is how full it is, not how free: a disk at 95% should
	   read red whether that is 5 GB or 500. */
	sys_set_color(plugin,
	              (total_gb > 0)
	              	? (gdouble)(total_gb - free_gb) / (gdouble)total_gb
	              	: -1.0,
	              GOWL_BAR_COLOR_TEXT);
}

static GowlBarPanel *
disk_panel(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	return sys_build_panel(plugin, "disk");
}

static const GowlBarPluginVTable disk_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL, NULL, NULL, NULL,
	sys_interval_slow, disk_poll, NULL,
	NULL, NULL, NULL, NULL,
	disk_panel, sys_panel_action,
	NULL, NULL,
	NULL,
	NULL,
	NULL, NULL
};

/* ----------------------------------------------------------------
 * load, temp, gpu, io, uptime, host, user
 * ---------------------------------------------------------------- */

static void
load_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = sys_info();
	gchar buf[32];
	gdouble load;
	gint cores;

	(void)data;

	if (info == NULL)
		return;
	load  = bar_sysinfo_load1(info);
	cores = bar_sysinfo_cpu_cores(info);
	g_snprintf(buf, sizeof(buf), "LOAD %.2f", load);
	gowl_bar_plugin_set_label(plugin, buf);
	sys_set_color(plugin,
	              (cores > 0) ? load / (gdouble)cores : -1.0,
	              GOWL_BAR_COLOR_TEXT);
}

static GowlBarPanel *
load_panel(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	return sys_build_panel(plugin, "cpu");
}

static const GowlBarPluginVTable load_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL, NULL, NULL, NULL,
	NULL, load_poll, NULL,
	NULL, NULL, NULL, NULL,
	load_panel, sys_panel_action,
	NULL, NULL,
	NULL,
	NULL,
	NULL, NULL
};

static void
temp_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = sys_info();
	gchar buf[32];
	gint mc;

	(void)data;

	if (info == NULL)
		return;
	mc = bar_sysinfo_temp_mc(info);
	if (mc <= 0) {
		gowl_bar_plugin_set_label(plugin, NULL);
		return;
	}
	g_snprintf(buf, sizeof(buf), "%d\xc2\xb0""C", mc / 1000);
	gowl_bar_plugin_set_label(plugin, buf);
	sys_set_icon(plugin, "\xef\x8b\x88");
	/* 40 C is idle and 90 C is throttling, so that is the band the
	   colour should span rather than 0..100. */
	sys_set_color(plugin, ((gdouble)mc / 1000.0 - 40.0) / 50.0,
	              GOWL_BAR_COLOR_TEXT);
}

static GowlBarPanel *
temp_panel(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	return sys_build_panel(plugin, "temp");
}

static const GowlBarPluginVTable temp_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL, NULL, NULL, NULL,
	NULL, temp_poll, NULL,
	NULL, NULL, NULL, NULL,
	temp_panel, sys_panel_action,
	NULL, NULL,
	NULL,
	NULL,
	NULL, NULL
};

static void
gpu_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = sys_info();
	gchar buf[32];
	gint pct;

	(void)data;

	if (info == NULL)
		return;
	pct = bar_sysinfo_gpu_percent(info);
	if (pct < 0) {
		gowl_bar_plugin_set_label(plugin, NULL);
		return;
	}
	g_snprintf(buf, sizeof(buf), "GPU %d%%", pct);
	gowl_bar_plugin_set_label(plugin, buf);
	sys_set_color(plugin, (gdouble)pct / 100.0, GOWL_BAR_COLOR_TEXT);
}

static GowlBarPanel *
gpu_panel(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	return sys_build_panel(plugin, "cpu");
}

static const GowlBarPluginVTable gpu_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL, NULL, NULL, NULL,
	sys_interval_fast, gpu_poll, NULL,
	NULL, NULL, NULL, NULL,
	gpu_panel, sys_panel_action,
	NULL, NULL,
	NULL,
	NULL,
	NULL, NULL
};

static void
io_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = sys_info();
	gchar rd_buf[24], wr_buf[24], buf[64];
	glong rd, wr;

	(void)data;

	if (info == NULL)
		return;
	bar_sysinfo_io(info, gowl_bar_plugin_get_setting(plugin, "param"),
	               &rd, &wr);
	bar_sysinfo_format_rate(rd, rd_buf, sizeof(rd_buf));
	bar_sysinfo_format_rate(wr, wr_buf, sizeof(wr_buf));
	g_snprintf(buf, sizeof(buf), "R:%s W:%s", rd_buf, wr_buf);
	gowl_bar_plugin_set_label(plugin, buf);
}

static GowlBarPanel *
io_panel(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	return sys_build_panel(plugin, "disk");
}

static const GowlBarPluginVTable io_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL, NULL, NULL, NULL,
	sys_interval_fast, io_poll, NULL,
	NULL, NULL, NULL, NULL,
	io_panel, sys_panel_action,
	NULL, NULL,
	NULL,
	NULL,
	NULL, NULL
};

static void
uptime_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = sys_info();
	gchar buf[64], out[80];

	(void)data;

	if (info == NULL)
		return;
	bar_sysinfo_format_duration(bar_sysinfo_uptime_seconds(info), buf,
	                            sizeof(buf));
	g_snprintf(out, sizeof(out), "UP %s", buf);
	gowl_bar_plugin_set_label(plugin, out);
}

static GowlBarPanel *
uptime_panel(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	return sys_build_panel(plugin, "cpu");
}

static const GowlBarPluginVTable uptime_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL, NULL, NULL, NULL,
	sys_interval_slow, uptime_poll, NULL,
	NULL, NULL, NULL, NULL,
	uptime_panel, sys_panel_action,
	NULL, NULL,
	NULL,
	NULL,
	NULL, NULL
};

static void
host_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = sys_info();

	(void)data;

	if (info == NULL)
		return;
	gowl_bar_plugin_set_label(plugin, bar_sysinfo_hostname(info));
}

static const GowlBarPluginVTable host_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL, NULL, NULL, NULL,
	sys_interval_slow, host_poll, NULL,
	NULL, NULL, NULL, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL,
	NULL,
	NULL, NULL
};

static void
user_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = sys_info();

	(void)data;

	if (info == NULL)
		return;
	gowl_bar_plugin_set_label(plugin, bar_sysinfo_username(info));
}

static const GowlBarPluginVTable user_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL, NULL, NULL, NULL,
	sys_interval_slow, user_poll, NULL,
	NULL, NULL, NULL, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL,
	NULL,
	NULL, NULL
};

/* ----------------------------------------------------------------
 * battery / power
 * ---------------------------------------------------------------- */

/* A glyph that matches the charge, so the widget is readable at a
   glance even with the percentage turned off. */
static const gchar *
battery_glyph(gint percent, gboolean charging)
{
	if (charging)
		return "\xef\x83\xa7";        /* U+F0E7 bolt */
	if (percent >= 90)
		return "\xef\x89\x80";        /* U+F240 battery-full */
	if (percent >= 65)
		return "\xef\x89\x81";
	if (percent >= 40)
		return "\xef\x89\x82";
	if (percent >= 15)
		return "\xef\x89\x83";
	return "\xef\x89\x84";                /* U+F244 battery-empty */
}

static void
battery_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = sys_info();
	gchar buf[48];
	gint pct;
	gboolean charging;

	(void)data;

	if (info == NULL)
		return;
	pct = bar_sysinfo_battery_percent(info);
	if (pct < 0) {
		/* No battery: a desktop should not carry a dead widget. */
		gowl_bar_plugin_set_label(plugin, NULL);
		gowl_bar_plugin_set_icon(plugin, NULL);
		return;
	}
	charging = bar_sysinfo_battery_charging(info);

	g_snprintf(buf, sizeof(buf), "BAT %d%%%s", pct, charging ? "+" : "");
	gowl_bar_plugin_set_label(plugin, buf);
	sys_set_icon(plugin, battery_glyph(pct, charging));

	/* Low is a warning and wins; charging is green; otherwise the
	   configured colour -- the shipped `battery-color: teal' was
	   silently ignored here while every other system widget honoured
	   its own. */
	if (!charging && pct <= 10)
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_RED);
	else if (!charging && pct <= 25)
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_PEACH);
	else if (charging && !bar_sysinfo_battery_full(info))
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_GREEN);
	else
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color"),
			GOWL_BAR_COLOR_TEXT);
}

/* ----------------------------------------------------------------
 * The power profile, off the compositor thread
 *
 * Asking for it is a subprocess, and the panel that shows it is built
 * on the compositor's dispatch thread while it holds cmacs_gowl_mutex:
 * powerprofilesctl talks to a daemon over D-Bus, so the editor froze
 * for that round trip every time the power panel opened.  The async
 * poll reads it into this cache and the panel reads the cache; a
 * change made from the panel is run on the worker and re-read there.
 * ---------------------------------------------------------------- */

typedef struct {
	GMutex  lock;
	gchar  *profile;     /* the active profile, or NULL when unknown */
	gboolean answered;   /* whether either interface has ever replied */
} PowerData;

static gpointer
power_create(GowlBarPlugin *plugin)
{
	PowerData *pd = g_new0(PowerData, 1);

	(void)plugin;
	g_mutex_init(&pd->lock);
	return pd;
}

static void
power_destroy(GowlBarPlugin *plugin, gpointer data)
{
	PowerData *pd = data;

	(void)plugin;
	if (pd == NULL)
		return;
	g_free(pd->profile);
	g_mutex_clear(&pd->lock);
	g_free(pd);
}

static gchar *power_profile_get(void);

static void
power_read_profile(PowerData *pd)
{
	gchar *profile;

	if (pd == NULL)
		return;
	profile = power_profile_get();
	g_mutex_lock(&pd->lock);
	g_free(pd->profile);
	pd->profile  = profile;
	pd->answered = (profile != NULL);
	g_mutex_unlock(&pd->lock);
}

static void
power_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	power_read_profile(data);
}

static gchar *
power_dup_profile(PowerData *pd, gboolean *answered)
{
	gchar *copy;

	if (answered != NULL)
		*answered = FALSE;
	if (pd == NULL)
		return NULL;
	g_mutex_lock(&pd->lock);
	copy = g_strdup(pd->profile);
	if (answered != NULL)
		*answered = pd->answered;
	g_mutex_unlock(&pd->lock);
	return copy;
}

/*
 * The active power profile, and how to change it.
 *
 * Asked over D-Bus rather than through powerprofilesctl, because that
 * binary is not always installed even where power profiles work.
 * Fedora 44 ships tuned-ppd in the base image, which CONFLICTS with
 * power-profiles-daemon -- only one can be present -- and tuned-ppd
 * provides the same net.hadess.PowerProfiles interface without
 * providing powerprofilesctl.  Keying on the binary meant the panel
 * reported "not installed" on a machine whose power profiles were
 * working perfectly.
 *
 * busctl comes from systemd, so it is there on any host that has either
 * implementation.  powerprofilesctl is still preferred when present:
 * it is the documented interface and it is what a user will have read
 * about.
 */
static gchar *
power_profile_get(void)
{
	const gchar *ppd[] = { "powerprofilesctl", "get", NULL };
	const gchar *bus[] = { "busctl", "--system", "get-property",
	                       "net.hadess.PowerProfiles",
	                       "/net/hadess/PowerProfiles",
	                       "net.hadess.PowerProfiles",
	                       "ActiveProfile", NULL };
	g_autofree gchar *out = NULL;
	gchar *start, *end;

	if (bar_have_command("powerprofilesctl"))
		return bar_run_argv_line(ppd);

	if (!bar_have_command("busctl"))
		return NULL;

	/* busctl prints a typed value: s "balanced" */
	out = bar_run_argv_line(bus);
	if (out == NULL)
		return NULL;
	start = strchr(out, '"');
	if (start == NULL)
		return NULL;
	start++;
	end = strchr(start, '"');
	if (end == NULL)
		return NULL;
	return g_strndup(start, (gsize)(end - start));
}

static gchar *
power_profile_set_command(const gchar *name)
{
	if (bar_have_command("powerprofilesctl"))
		return g_strdup_printf("powerprofilesctl set %s", name);
	if (bar_have_command("busctl"))
		return g_strdup_printf(
			"busctl --system set-property "
			"net.hadess.PowerProfiles /net/hadess/PowerProfiles "
			"net.hadess.PowerProfiles ActiveProfile s %s", name);
	return NULL;
}

static GowlBarPanel *
power_panel(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = sys_info();
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	gchar buf[64], buf2[64];
	gint pct, minutes;
	gboolean charging;

	(void)data;

	if (info == NULL)
		return NULL;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 400);

	pct      = bar_sysinfo_battery_percent(info);
	charging = bar_sysinfo_battery_charging(info);
	minutes  = bar_sysinfo_battery_minutes(info);

	if (pct >= 0) {
		g_snprintf(buf, sizeof(buf), "%d%%", pct);
		gowl_bar_panel_add_hero(panel, battery_glyph(pct, charging),
			buf, bar_sysinfo_battery_full(info) ? "Full"
			     : charging ? "Charging" : "On battery");

		item = gowl_bar_panel_add_progress(panel, "Charge",
		                                   (gdouble)pct / 100.0);
		if (pct <= 15 && !charging)
			gowl_bar_panel_item_set_color(item,
			                              GOWL_BAR_COLOR_RED);
		else if (charging)
			gowl_bar_panel_item_set_color(item,
			                              GOWL_BAR_COLOR_GREEN);

		if (minutes > 0) {
			bar_sysinfo_format_duration((glong)minutes * 60, buf,
			                            sizeof(buf));
			gowl_bar_panel_add_field(panel,
				charging ? "Until full" : "Remaining", buf);
		}
	} else {
		gowl_bar_panel_add_hero(panel, "\xef\x87\xa6", "Power",
		                        "No battery");
	}

	bar_sysinfo_format_duration(bar_sysinfo_uptime_seconds(info), buf,
	                            sizeof(buf));
	g_snprintf(buf2, sizeof(buf2), "%.2f", bar_sysinfo_load1(info));
	gowl_bar_panel_add_field_pair(panel, "Uptime", buf, "Load", buf2);

	gowl_bar_panel_add_separator(panel);
	gowl_bar_panel_add_section(panel, "Power profile");
	{
		g_autofree gchar *profile = NULL;
		const gchar *names[3] = { "power-saver", "balanced",
		                          "performance" };
		gboolean answered = FALSE;
		gint i;

		/* From the cache the async poll fills, never a subprocess
		   here: this runs on the compositor thread. */
		profile = power_dup_profile(data, &answered);
		item = gowl_bar_panel_add_buttons(panel, "profile");
		for (i = 0; i < 3; i++) {
			gowl_bar_panel_add_button(item, names[i],
				g_strcmp0(profile, names[i]) == 0);
		}
		if (profile == NULL) {
			/* Nothing answers on either interface, so say so
			   rather than offering three controls that do
			   nothing -- or, before the first poll has come
			   back, say that instead. */
			gowl_bar_panel_add_label(panel, answered
				? "no power-profiles service is running"
				: "asking for the active profile...");
		}
	}

	gowl_bar_panel_add_separator(panel);
	item = gowl_bar_panel_add_buttons(panel, "action");
	gowl_bar_panel_add_button(item, "Lock", FALSE);
	gowl_bar_panel_add_button(item, "Suspend", FALSE);
	gowl_bar_panel_add_button(item, "Reboot", FALSE);
	gowl_bar_panel_add_button(item, "Power off", FALSE);

	return panel;
}

/* Runs on the worker: apply the profile, wait for it, and re-read so
   the panel shows what the daemon actually did rather than what was
   asked.  The button is lit optimistically in the meantime. */
static void
power_set_profile_work(GowlBarPlugin *plugin, gpointer user_data)
{
	const gchar *line = user_data;
	g_autofree gchar *out = NULL;
	PowerData *pd = NULL;

	if (GOWL_IS_BAR_PLUGIN_PROXY(plugin))
		pd = gowl_bar_plugin_proxy_get_data(GOWL_BAR_PLUGIN_PROXY(plugin));

	out = bar_run_shell_line(line);
	power_read_profile(pd);
	gowl_bar_plugin_request_panel_refresh(plugin);
}

static void
power_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
             gint index, gdouble value, guint button)
{
	PowerData *pd = data;

	(void)value;
	(void)button;

	if (g_strcmp0(item_id, "profile") == 0) {
		const gchar *names[3] = { "power-saver", "balanced",
		                          "performance" };
		gchar *line;

		if (index < 0 || index > 2)
			return;
		line = power_profile_set_command(names[index]);
		if (line == NULL)
			return;
		if (pd != NULL) {
			g_mutex_lock(&pd->lock);
			g_free(pd->profile);
			pd->profile = g_strdup(names[index]);
			g_mutex_unlock(&pd->lock);
		}
		gowl_bar_plugin_queue_work(plugin, power_set_profile_work,
		                           line, g_free);
		gowl_bar_plugin_request_panel_refresh(plugin);
		return;
	}

	if (g_strcmp0(item_id, "action") != 0)
		return;

	switch (index) {
	case 0: {
		const gchar *cmd;

		/* The session's own locker first: under gowl that is the
		   compositor's, not whatever loginctl would pick. */
		cmd = gowl_bar_plugin_get_setting(plugin, "lock-command");
		gowl_bar_plugin_spawn(plugin,
			(cmd != NULL) ? cmd : "loginctl lock-session");
		break;
	}
	case 1:
		gowl_bar_plugin_spawn(plugin, "systemctl suspend");
		break;
	case 2:
		gowl_bar_plugin_spawn(plugin, "systemctl reboot");
		break;
	case 3:
		gowl_bar_plugin_spawn(plugin, "systemctl poweroff");
		break;
	default:
		break;
	}
}

static const GowlBarPluginVTable battery_vtable = {
	sizeof(GowlBarPluginVTable),
	power_create, power_destroy, NULL, NULL, NULL,
	sys_interval_slow, battery_poll, power_poll_async,
	NULL, NULL, NULL, NULL,
	power_panel, power_action,
	NULL, NULL,
	NULL,
	NULL,
	NULL, NULL
};

/* A power widget with no battery still wants its panel: the session
   actions live there. */
static void
power_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = sys_info();
	gint pct;

	(void)data;

	if (info == NULL)
		return;
	pct = bar_sysinfo_battery_percent(info);
	if (pct >= 0) {
		battery_poll(plugin, data);
		return;
	}
	gowl_bar_plugin_set_label(plugin, NULL);
	gowl_bar_plugin_set_icon(plugin, "\xef\x80\x91");   /* U+F011 power */
	gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_SUBTEXT);
}

static const GowlBarPluginVTable power_vtable = {
	sizeof(GowlBarPluginVTable),
	power_create, power_destroy, NULL, NULL, NULL,
	sys_interval_slow, power_poll, power_poll_async,
	NULL, NULL, NULL, NULL,
	power_panel, power_action,
	NULL, NULL,
	NULL,
	NULL,
	NULL, NULL
};

/* ----------------------------------------------------------------
 * Registration
 * ---------------------------------------------------------------- */

/**
 * bar_register_system_plugins:
 * @registry: the registry to populate
 */
void
bar_register_system_plugins(GowlBarRegistry *registry)
{
	gowl_bar_registry_register_vtable(registry, "cpu", "CPU",
		"Processor utilisation, with an optional activity mode",
		&cpu_vtable);
	gowl_bar_registry_register_vtable(registry, "memory", "Memory",
		"Memory in use", &memory_vtable);
	gowl_bar_registry_register_vtable(registry, "swap", "Swap",
		"Swap in use", &swap_vtable);
	gowl_bar_registry_register_vtable(registry, "disk", "Disk",
		"Free space on a mount point", &disk_vtable);
	gowl_bar_registry_register_vtable(registry, "load", "Load average",
		"One-minute load average", &load_vtable);
	gowl_bar_registry_register_vtable(registry, "temp", "Temperature",
		"Package temperature", &temp_vtable);
	gowl_bar_registry_register_vtable(registry, "gpu", "GPU",
		"GPU utilisation", &gpu_vtable);
	gowl_bar_registry_register_vtable(registry, "io", "Disk IO",
		"Block device read and write rates", &io_vtable);
	gowl_bar_registry_register_vtable(registry, "uptime", "Uptime",
		"Time since boot", &uptime_vtable);
	gowl_bar_registry_register_vtable(registry, "host", "Host name",
		"This machine's host name", &host_vtable);
	gowl_bar_registry_register_vtable(registry, "user", "User name",
		"The logged-in user", &user_vtable);
	gowl_bar_registry_register_vtable(registry, "battery", "Battery",
		"Charge level, with a power panel", &battery_vtable);
	gowl_bar_registry_register_vtable(registry, "power", "Power",
		"Session power actions and profiles", &power_vtable);

	gowl_bar_registry_register_alias(registry, "mem", "memory");
	gowl_bar_registry_register_alias(registry, "bat", "battery");
	gowl_bar_registry_register_alias(registry, "hostname", "host");
	gowl_bar_registry_register_alias(registry, "temperature", "temp");
	gowl_bar_registry_register_alias(registry, "activity", "cpu");
}
