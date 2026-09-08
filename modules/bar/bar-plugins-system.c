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
#include <string.h>

#include "bar-internal.h"

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

/* Apply a `color' setting if the user set one, otherwise a computed
   pressure colour.  An explicit choice always wins: a user who
   coloured their cpu widget green wants it green. */
static void
sys_set_color(GowlBarPlugin *plugin, gdouble fraction,
              GowlBarColor fallback)
{
	const gchar *spec;
	GowlBarColor role;

	spec = gowl_bar_plugin_get_setting(plugin, "color");
	if (spec != NULL && gowl_bar_theme_color_from_name(spec, &role)) {
		gowl_bar_plugin_set_color(plugin, role);
		return;
	}
	if (fraction >= 0.0) {
		gowl_bar_plugin_set_color(plugin,
		                          sys_pressure_color(fraction));
		return;
	}
	gowl_bar_plugin_set_color(plugin, fallback);
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

/* Build the panel every system widget opens.  @focus names the reading
   the clicked widget shows, which becomes the hero. */
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

static GowlBarPanel *
sys_build_panel(GowlBarPlugin *plugin, const gchar *focus)
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

	if (info == NULL)
		return NULL;

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
		g_snprintf(buf, sizeof(buf), "%d\xc2\xb0""C", temp_mc / 1000);
		gowl_bar_panel_add_hero(panel, "\xef\x8b\x88", buf,
		                        "Package temperature");
	} else if (g_strcmp0(focus, "disk") == 0) {
		const gchar *mount;

		mount = gowl_bar_plugin_get_setting(plugin, "param");
		if (mount == NULL)
			mount = disk_default_mount();
		bar_sysinfo_disk(info, mount, &free_gb, &total_gb);
		g_snprintf(buf, sizeof(buf), "%ld GB", free_gb);
		g_snprintf(buf2, sizeof(buf2), "Free on %s", mount);
		gowl_bar_panel_add_hero(panel, "\xef\x82\xa0", buf, buf2);
	} else {
		g_snprintf(buf, sizeof(buf), "%d%%", cpu);
		gowl_bar_panel_add_hero(panel, "\xef\x8b\x9b", buf,
		                        "Processor load");
	}

	/* CPU */
	gowl_bar_panel_add_section(panel, "Processor");
	samples = bar_sysinfo_history(info, "cpu", &n_samples);
	if (samples != NULL) {
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
		g_snprintf(buf2, sizeof(buf2), "%d\xc2\xb0""C",
		           temp_mc / 1000);
		gowl_bar_panel_add_field_pair(panel, "Cores", buf,
		                              "Temperature", buf2);
	} else {
		gowl_bar_panel_add_field(panel, "Cores", buf);
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

	gowl_bar_panel_add_separator(panel);
	item = gowl_bar_panel_add_buttons(panel, "tool");
	gowl_bar_panel_add_button(item, "Monitor", FALSE);
	gowl_bar_panel_add_button(item, "Processes", FALSE);
	gowl_bar_panel_add_button(item, "Disks", FALSE);

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
			gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
				"No tool configured",
				"Set the widget's monitor-command setting to "
				"the program you want this button to run.");
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
 * cpu
 * ---------------------------------------------------------------- */

static gint
sys_interval_fast(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 2;
}

static void
cpu_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = sys_info();
	gchar buf[32];
	gint pct;

	(void)data;

	if (info == NULL)
		return;
	pct = bar_sysinfo_cpu_percent(info);
	g_snprintf(buf, sizeof(buf), "CPU %d%%", pct);
	gowl_bar_plugin_set_label(plugin, buf);
	sys_set_icon(plugin, "\xef\x8b\x9b");
	sys_set_color(plugin, (gdouble)pct / 100.0, GOWL_BAR_COLOR_TEXT);
}

static GowlBarPanel *
cpu_panel(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	return sys_build_panel(plugin, "cpu");
}

static const GowlBarPluginVTable cpu_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL, NULL, NULL, NULL,
	sys_interval_fast, cpu_poll, NULL,
	NULL, NULL, NULL, NULL,
	cpu_panel, sys_panel_action,
	NULL, NULL
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

	if (!charging && pct <= 10)
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_RED);
	else if (!charging && pct <= 25)
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_PEACH);
	else if (charging)
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_GREEN);
	else
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_TEXT);
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
			buf, charging ? "Charging" : "On battery");

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
		gint i;

		profile = power_profile_get();
		item = gowl_bar_panel_add_buttons(panel, "profile");
		for (i = 0; i < 3; i++) {
			gowl_bar_panel_add_button(item, names[i],
				g_strcmp0(profile, names[i]) == 0);
		}
		if (profile == NULL) {
			/* Nothing answers on either interface, so say so
			   rather than offering three controls that do
			   nothing. */
			gowl_bar_panel_add_label(panel,
				"no power-profiles service is running");
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

static void
power_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
             gint index, gdouble value, guint button)
{
	(void)data;
	(void)value;
	(void)button;

	if (g_strcmp0(item_id, "profile") == 0) {
		const gchar *names[3] = { "power-saver", "balanced",
		                          "performance" };
		g_autofree gchar *line = NULL;

		if (index < 0 || index > 2)
			return;
		line = power_profile_set_command(names[index]);
		if (line == NULL)
			return;
		gowl_bar_plugin_spawn(plugin, line);
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
	NULL, NULL, NULL, NULL, NULL,
	sys_interval_slow, battery_poll, NULL,
	NULL, NULL, NULL, NULL,
	power_panel, power_action,
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
	NULL, NULL, NULL, NULL, NULL,
	sys_interval_slow, power_poll, NULL,
	NULL, NULL, NULL, NULL,
	power_panel, power_action,
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
		"Processor utilisation", &cpu_vtable);
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
}
