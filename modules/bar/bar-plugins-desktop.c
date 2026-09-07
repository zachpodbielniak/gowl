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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/gowl-compositor.h"
#include "core/gowl-client.h"
#include "core/gowl-monitor.h"
#include "boxed/gowl-process-info.h"

#include "bar-internal.h"

/**
 * SECTION:bar-plugins-desktop
 * @title: Desktop bar plugins
 * @short_description: audio, media, containers, weather and the rest
 *
 * The plugins here all talk to something outside the compositor --- a
 * PipeWire session, a media player, podman, a weather service --- and
 * so all of them do their work in @poll_async.  That is not an
 * optimisation: the compositor's dispatch thread holds the lock every
 * editor primitive needs, and one `podman ps' on it would freeze the
 * editor as well as the bar.
 */

/* ----------------------------------------------------------------
 * audio
 * ---------------------------------------------------------------- */

typedef struct {
	gdouble    volume;         /* 0.0--1.0, may exceed 1 on boost */
	gboolean   muted;
	gdouble    source_volume;
	gboolean   source_muted;
	gchar     *sink_name;
	gchar     *source_name;
	GPtrArray *sinks;          /* `id\tname\tactive' rows */
	GPtrArray *sources;
	gboolean   have_wpctl;
} AudioData;

static gpointer
audio_create(GowlBarPlugin *plugin)
{
	AudioData *ad;

	(void)plugin;
	ad = g_new0(AudioData, 1);
	ad->sinks      = g_ptr_array_new_with_free_func(g_free);
	ad->sources    = g_ptr_array_new_with_free_func(g_free);
	ad->have_wpctl = bar_have_command("wpctl");
	return ad;
}

static void
audio_destroy(GowlBarPlugin *plugin, gpointer data)
{
	AudioData *ad = data;

	(void)plugin;
	if (ad == NULL)
		return;
	g_free(ad->sink_name);
	g_free(ad->source_name);
	g_ptr_array_unref(ad->sinks);
	g_ptr_array_unref(ad->sources);
	g_free(ad);
}

static gint
audio_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 2;
}

/* Read one wpctl volume line: `Volume: 0.40' or `Volume: 0.40
   [MUTED]'. */
static gboolean
audio_read_volume(const gchar *target, gdouble *volume, gboolean *muted)
{
	const gchar *argv[] = { "wpctl", "get-volume", NULL, NULL };
	g_autofree gchar *out = NULL;
	gdouble parsed;

	argv[2] = target;
	out = bar_run_argv_line(argv);
	if (out == NULL)
		return FALSE;

	if (sscanf(out, "Volume: %lf", &parsed) != 1)
		return FALSE;

	*volume = parsed;
	*muted  = (strstr(out, "[MUTED]") != NULL);
	return TRUE;
}

/* Collect the devices of one kind out of `wpctl status'.  The output
   is a tree drawn with box characters, so this looks for the section
   heading and then reads the indented rows until the blank line. */
static void
audio_read_devices(AudioData *ad, const gchar *heading, GPtrArray *out,
                   gchar **active_name)
{
	const gchar *argv[] = { "wpctl", "status", NULL };
	g_autofree gchar *status = NULL;
	g_auto(GStrv) lines = NULL;
	gboolean in_section;
	gint i;

	g_ptr_array_set_size(out, 0);

	status = bar_run_argv(argv);
	if (status == NULL)
		return;

	lines = g_strsplit(status, "\n", -1);
	in_section = FALSE;
	for (i = 0; lines[i] != NULL; i++) {
		const gchar *p;
		const gchar *dot;
		gboolean active;
		gint id;

		if (strstr(lines[i], heading) != NULL) {
			in_section = TRUE;
			continue;
		}
		if (!in_section)
			continue;

		/* A section ends at the next heading or at a line with no
		   device id in it. */
		if (strstr(lines[i], "Sources:") != NULL ||
		    strstr(lines[i], "Sinks:") != NULL ||
		    strstr(lines[i], "Devices:") != NULL ||
		    strstr(lines[i], "Filters:") != NULL ||
		    strstr(lines[i], "Streams:") != NULL) {
			if (strstr(lines[i], heading) == NULL)
				break;
		}

		/* Rows look like `  |  *   45. Generic Analog  [vol: 0.40]'
		   with a star marking the default. */
		active = (strchr(lines[i], '*') != NULL);
		p = lines[i];
		while (*p != '\0' && (*p < '0' || *p > '9'))
			p++;
		if (*p == '\0')
			continue;
		id = (gint)g_ascii_strtoll(p, NULL, 10);
		dot = strchr(p, '.');
		if (dot == NULL)
			continue;
		dot++;
		while (*dot == ' ')
			dot++;

		{
			g_autofree gchar *name = NULL;
			gchar *bracket;

			name = g_strdup(dot);
			bracket = strstr(name, "[vol:");
			if (bracket != NULL)
				*bracket = '\0';
			g_strstrip(name);
			if (name[0] == '\0')
				continue;

			g_ptr_array_add(out, g_strdup_printf("%d\t%s\t%d", id,
			                                     name,
			                                     active ? 1 : 0));
			if (active && active_name != NULL) {
				g_free(*active_name);
				*active_name = g_strdup(name);
			}
		}

		if (out->len >= 12)
			break;
	}
}

/* A speaker glyph matching the level, so the widget is legible without
   the percentage. */
static const gchar *
audio_glyph(gdouble volume, gboolean muted)
{
	if (muted)
		return "\xef\x9a\xa9";       /* U+F6A9 volume-mute */
	if (volume <= 0.01)
		return "\xef\x80\xa6";       /* U+F026 volume-off */
	if (volume < 0.5)
		return "\xef\x80\xa7";       /* U+F027 volume-down */
	return "\xef\x80\xa8";               /* U+F028 volume-up */
}

static void
audio_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	AudioData *ad = data;
	gchar buf[48];

	ad->have_wpctl = bar_have_command("wpctl");
	if (!ad->have_wpctl) {
		gowl_bar_plugin_set_label(plugin, NULL);
		gowl_bar_plugin_set_icon(plugin, NULL);
		return;
	}

	if (!audio_read_volume("@DEFAULT_AUDIO_SINK@", &ad->volume,
	                       &ad->muted)) {
		gowl_bar_plugin_set_label(plugin, "VOL ?");
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_MUTED);
		return;
	}
	audio_read_volume("@DEFAULT_AUDIO_SOURCE@", &ad->source_volume,
	                  &ad->source_muted);

	/* Device enumeration costs a `wpctl status' parse, which is only
	   worth doing while the panel that shows it is open. */
	if (gowl_bar_plugin_get_setting_bool(plugin, "panel-open", FALSE)) {
		audio_read_devices(ad, "Sinks:", ad->sinks, &ad->sink_name);
		audio_read_devices(ad, "Sources:", ad->sources,
		                   &ad->source_name);
	}

	if (ad->muted)
		g_strlcpy(buf, "VOL MUTE", sizeof(buf));
	else
		g_snprintf(buf, sizeof(buf), "VOL %d%%",
		           (gint)(ad->volume * 100.0 + 0.5));
	gowl_bar_plugin_set_label(plugin, buf);
	gowl_bar_plugin_set_icon(plugin, audio_glyph(ad->volume, ad->muted));
	gowl_bar_plugin_set_color(plugin,
		ad->muted ? GOWL_BAR_COLOR_MUTED : GOWL_BAR_COLOR_TEXT);
}

static void
audio_panel_opened(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	gowl_bar_plugin_set_setting(plugin, "panel-open", "true");
	gowl_bar_plugin_request_redraw(plugin);
}

/*
 * And stop when it closes.  Without this the flag is one-way: the first
 * open leaves the async poll enumerating audio devices every interval
 * for the rest of the session.
 */
static void
audio_panel_closed(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	gowl_bar_plugin_set_setting(plugin, "panel-open", "false");
}

static GowlBarPanel *
audio_panel(GowlBarPlugin *plugin, gpointer data)
{
	AudioData *ad = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	gchar buf[64];
	guint i;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 420);

	if (!ad->have_wpctl) {
		gowl_bar_panel_add_hero(panel, "\xef\x80\xa8", "Audio",
		                        "wpctl not found");
		gowl_bar_panel_add_label(panel,
			"Install pipewire's wireplumber tools to control "
			"audio from here.");
		return panel;
	}

	item = gowl_bar_panel_add_hero(panel, audio_glyph(ad->volume,
	                                                  ad->muted),
	                               "Audio", "Easy listening");
	gowl_bar_panel_item_set_id(item, "mute");
	gowl_bar_panel_item_set_active(item, !ad->muted);

	g_snprintf(buf, sizeof(buf), "%d%%",
	           (gint)(ad->volume * 100.0 + 0.5));
	item = gowl_bar_panel_add_slider(panel, "output", "Output",
	                                 ad->volume);
	gowl_bar_panel_item_set_value(item, buf);
	gowl_bar_panel_item_set_step(item, 0.05);
	if (ad->muted)
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_OVERLAY);

	for (i = 0; i < ad->sinks->len; i++) {
		g_auto(GStrv) fields = NULL;
		g_autofree gchar *row_id = NULL;

		fields = g_strsplit(g_ptr_array_index(ad->sinks, i), "\t", 3);
		if (g_strv_length(fields) < 3)
			continue;
		row_id = g_strdup_printf("sink:%s", fields[0]);
		item = gowl_bar_panel_add_row(panel, row_id, "\xef\x80\xa8",
		                              fields[1], NULL);
		gowl_bar_panel_item_set_active(item,
			g_strcmp0(fields[2], "1") == 0);
	}

	gowl_bar_panel_add_separator(panel);

	g_snprintf(buf, sizeof(buf), "%d%%",
	           (gint)(ad->source_volume * 100.0 + 0.5));
	item = gowl_bar_panel_add_slider(panel, "input", "Input",
	                                 ad->source_volume);
	gowl_bar_panel_item_set_value(item, buf);
	gowl_bar_panel_item_set_step(item, 0.05);
	if (ad->source_muted)
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_OVERLAY);

	for (i = 0; i < ad->sources->len; i++) {
		g_auto(GStrv) fields = NULL;
		g_autofree gchar *row_id = NULL;

		fields = g_strsplit(g_ptr_array_index(ad->sources, i), "\t",
		                    3);
		if (g_strv_length(fields) < 3)
			continue;
		row_id = g_strdup_printf("source:%s", fields[0]);
		item = gowl_bar_panel_add_row(panel, row_id, "\xef\x84\xb0",
		                              fields[1], NULL);
		gowl_bar_panel_item_set_active(item,
			g_strcmp0(fields[2], "1") == 0);
	}

	gowl_bar_panel_add_separator(panel);
	item = gowl_bar_panel_add_buttons(panel, "tool");
	gowl_bar_panel_add_button(item, "Mute output", ad->muted);
	gowl_bar_panel_add_button(item, "Mute input", ad->source_muted);
	gowl_bar_panel_add_button(item, "Mixer", FALSE);

	return panel;
}

static void
audio_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
             gint index, gdouble value, guint button)
{
	AudioData *ad = data;
	g_autofree gchar *line = NULL;

	(void)button;

	if (g_strcmp0(item_id, "output") == 0) {
		line = g_strdup_printf("wpctl set-volume @DEFAULT_AUDIO_SINK@ "
		                       "%.2f", value);
		gowl_bar_plugin_spawn(plugin, line);
		ad->volume = value;
		return;
	}
	if (g_strcmp0(item_id, "input") == 0) {
		line = g_strdup_printf("wpctl set-volume "
		                       "@DEFAULT_AUDIO_SOURCE@ %.2f", value);
		gowl_bar_plugin_spawn(plugin, line);
		ad->source_volume = value;
		return;
	}
	if (g_strcmp0(item_id, "mute") == 0) {
		gowl_bar_plugin_spawn(plugin,
			"wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
		ad->muted = !ad->muted;
		return;
	}
	if (item_id != NULL && g_str_has_prefix(item_id, "sink:")) {
		line = g_strdup_printf("wpctl set-default %s",
		                       item_id + strlen("sink:"));
		gowl_bar_plugin_spawn(plugin, line);
		return;
	}
	if (item_id != NULL && g_str_has_prefix(item_id, "source:")) {
		line = g_strdup_printf("wpctl set-default %s",
		                       item_id + strlen("source:"));
		gowl_bar_plugin_spawn(plugin, line);
		return;
	}
	if (g_strcmp0(item_id, "tool") == 0) {
		switch (index) {
		case 0:
			gowl_bar_plugin_spawn(plugin,
				"wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
			break;
		case 1:
			gowl_bar_plugin_spawn(plugin,
				"wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle");
			break;
		case 2: {
			const gchar *cmd;

			cmd = gowl_bar_plugin_get_setting(plugin,
			                                  "mixer-command");
			gowl_bar_plugin_spawn(plugin,
				(cmd != NULL) ? cmd : "pavucontrol");
			break;
		}
		default:
			break;
		}
	}
}

static gboolean
audio_scroll(GowlBarPlugin *plugin, gpointer data, gdouble delta,
             gint discrete, guint modifiers)
{
	AudioData *ad = data;
	g_autofree gchar *line = NULL;
	gint steps;

	(void)modifiers;

	if (!ad->have_wpctl)
		return FALSE;

	steps = (discrete != 0) ? -discrete : ((delta > 0.0) ? -1 : 1);
	if (steps == 0)
		return FALSE;

	/* The wheel over a volume widget is the one scroll a bar may
	   claim without a modifier: the pointer is over the widget, so
	   there is no ambiguity about what it means. */
	line = g_strdup_printf("wpctl set-volume -l 1.5 "
	                       "@DEFAULT_AUDIO_SINK@ %d%%%c",
	                       (steps > 0) ? steps * 5 : -steps * 5,
	                       (steps > 0) ? '+' : '-');
	gowl_bar_plugin_spawn(plugin, line);
	return TRUE;
}

static gboolean
audio_click(GowlBarPlugin *plugin, gpointer data, guint button, gint x,
            gint y, guint modifiers)
{
	(void)data;
	(void)x;
	(void)y;
	(void)modifiers;

	if (button == BTN_MIDDLE) {
		gowl_bar_plugin_spawn(plugin,
			"wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
		return TRUE;
	}
	return FALSE;
}

static const GowlBarPluginVTable audio_vtable = {
	sizeof(GowlBarPluginVTable),
	audio_create, audio_destroy,
	NULL, NULL, NULL,
	audio_interval, NULL, audio_poll_async,
	NULL, NULL,
	audio_click, audio_scroll,
	audio_panel, audio_action,
	audio_panel_opened, audio_panel_closed
};

/* ----------------------------------------------------------------
 * media -- whatever is playing
 * ---------------------------------------------------------------- */

typedef struct {
	gchar   *artist;
	gchar   *title;
	gchar   *album;
	gchar   *status;
	gchar   *player;
	gdouble  position;
	gboolean have_playerctl;
} MediaData;

static gpointer
media_create(GowlBarPlugin *plugin)
{
	MediaData *md;

	(void)plugin;
	md = g_new0(MediaData, 1);
	md->have_playerctl = bar_have_command("playerctl");
	return md;
}

static void
media_destroy(GowlBarPlugin *plugin, gpointer data)
{
	MediaData *md = data;

	(void)plugin;
	if (md == NULL)
		return;
	g_free(md->artist);
	g_free(md->title);
	g_free(md->album);
	g_free(md->status);
	g_free(md->player);
	g_free(md);
}

static gint
media_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 3;
}

static void
media_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	MediaData *md = data;
	const gchar *argv[] = {
		"playerctl", "metadata", "--format",
		"{{status}}\t{{artist}}\t{{title}}\t{{album}}\t"
		"{{playerName}}", NULL
	};
	g_autofree gchar *out = NULL;
	g_auto(GStrv) fields = NULL;
	gchar buf[160];
	gint max_len;

	md->have_playerctl = bar_have_command("playerctl");
	if (!md->have_playerctl) {
		gowl_bar_plugin_set_label(plugin, NULL);
		gowl_bar_plugin_set_icon(plugin, NULL);
		return;
	}

	out = bar_run_argv_line(argv);
	if (out == NULL) {
		g_clear_pointer(&md->title, g_free);
		gowl_bar_plugin_set_label(plugin, NULL);
		gowl_bar_plugin_set_icon(plugin, NULL);
		return;
	}

	fields = g_strsplit(out, "\t", 5);
	g_free(md->status);
	md->status = g_strdup((fields[0] != NULL) ? fields[0] : "");
	g_free(md->artist);
	md->artist = g_strdup((fields[1] != NULL) ? fields[1] : "");
	g_free(md->title);
	md->title = g_strdup((fields[2] != NULL) ? fields[2] : "");
	g_free(md->album);
	md->album = g_strdup((fields[3] != NULL) ? fields[3] : "");
	g_free(md->player);
	md->player = g_strdup((fields[4] != NULL) ? fields[4] : "");

	if (md->title[0] == '\0') {
		gowl_bar_plugin_set_label(plugin, NULL);
		gowl_bar_plugin_set_icon(plugin, NULL);
		return;
	}

	max_len = gowl_bar_plugin_get_setting_int(plugin, "max-length", 40);
	if (md->artist[0] != '\0')
		g_snprintf(buf, sizeof(buf), "%s - %s", md->artist, md->title);
	else
		g_snprintf(buf, sizeof(buf), "%s", md->title);

	/* Truncate on a character boundary: a byte-wise cut through a
	   multi-byte glyph renders as a replacement box. */
	if (max_len > 0 && g_utf8_strlen(buf, -1) > max_len) {
		gchar *cut;

		cut = g_utf8_offset_to_pointer(buf, max_len);
		*cut = '\0';
	}

	gowl_bar_plugin_set_label(plugin, buf);
	gowl_bar_plugin_set_icon(plugin,
		(g_strcmp0(md->status, "Playing") == 0)
			? "\xef\x81\x8b" : "\xef\x81\x8c");
	gowl_bar_plugin_set_color(plugin,
		(g_strcmp0(md->status, "Playing") == 0)
			? GOWL_BAR_COLOR_MAUVE : GOWL_BAR_COLOR_MUTED);
}

static GowlBarPanel *
media_panel(GowlBarPlugin *plugin, gpointer data)
{
	MediaData *md = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;

	(void)plugin;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 400);

	if (!md->have_playerctl) {
		gowl_bar_panel_add_hero(panel, "\xef\x80\x81", "Media",
		                        "playerctl not found");
		return panel;
	}
	if (md->title == NULL || md->title[0] == '\0') {
		gowl_bar_panel_add_hero(panel, "\xef\x80\x81", "Media",
		                        "Nothing playing");
		return panel;
	}

	gowl_bar_panel_add_hero(panel, "\xef\x80\x81", md->title,
		(md->artist[0] != '\0') ? md->artist : "Unknown artist");
	if (md->album[0] != '\0')
		gowl_bar_panel_add_field(panel, "Album", md->album);
	gowl_bar_panel_add_field_pair(panel, "Player",
		(md->player[0] != '\0') ? md->player : "--", "Status",
		(md->status[0] != '\0') ? md->status : "--");

	gowl_bar_panel_add_separator(panel);
	item = gowl_bar_panel_add_buttons(panel, "transport");
	gowl_bar_panel_add_button(item, "\xe2\x8f\xae", FALSE);
	gowl_bar_panel_add_button(item,
		(g_strcmp0(md->status, "Playing") == 0) ? "\xe2\x8f\xb8"
		                                        : "\xe2\x96\xb6",
		FALSE);
	gowl_bar_panel_add_button(item, "\xe2\x8f\xad", FALSE);

	return panel;
}

static void
media_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
             gint index, gdouble value, guint button)
{
	(void)data;
	(void)value;
	(void)button;

	if (g_strcmp0(item_id, "transport") != 0)
		return;

	switch (index) {
	case 0:
		gowl_bar_plugin_spawn(plugin, "playerctl previous");
		break;
	case 1:
		gowl_bar_plugin_spawn(plugin, "playerctl play-pause");
		break;
	case 2:
		gowl_bar_plugin_spawn(plugin, "playerctl next");
		break;
	default:
		break;
	}
}

static gboolean
media_click(GowlBarPlugin *plugin, gpointer data, guint button, gint x,
            gint y, guint modifiers)
{
	(void)data;
	(void)x;
	(void)y;
	(void)modifiers;

	if (button == BTN_MIDDLE) {
		gowl_bar_plugin_spawn(plugin, "playerctl play-pause");
		return TRUE;
	}
	return FALSE;
}

static gboolean
media_scroll(GowlBarPlugin *plugin, gpointer data, gdouble delta,
             gint discrete, guint modifiers)
{
	(void)data;
	(void)delta;
	(void)modifiers;

	gowl_bar_plugin_spawn(plugin,
		(discrete > 0) ? "playerctl next" : "playerctl previous");
	return TRUE;
}

static const GowlBarPluginVTable media_vtable = {
	sizeof(GowlBarPluginVTable),
	media_create, media_destroy,
	NULL, NULL, NULL,
	media_interval, NULL, media_poll_async,
	NULL, NULL,
	media_click, media_scroll,
	media_panel, media_action,
	NULL, NULL
};

/* ----------------------------------------------------------------
 * podman
 * ---------------------------------------------------------------- */

typedef struct {
	GPtrArray *running;    /* `id\tname\timage\tstatus' rows */
	gint       total;
	gboolean   have_podman;
} PodmanData;

static gpointer
podman_create(GowlBarPlugin *plugin)
{
	PodmanData *pd;

	(void)plugin;
	pd = g_new0(PodmanData, 1);
	pd->running = g_ptr_array_new_with_free_func(g_free);
	return pd;
}

static void
podman_destroy(GowlBarPlugin *plugin, gpointer data)
{
	PodmanData *pd = data;

	(void)plugin;
	if (pd == NULL)
		return;
	g_ptr_array_unref(pd->running);
	g_free(pd);
}

static gint
podman_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 30;
}

static void
podman_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	PodmanData *pd = data;
	const gchar *argv[] = {
		"podman", "ps", "--all", "--format",
		"{{.ID}}\t{{.Names}}\t{{.Image}}\t{{.Status}}", NULL
	};
	g_autofree gchar *out = NULL;
	g_auto(GStrv) lines = NULL;
	gchar buf[32];
	gint running, i;

	pd->have_podman = bar_have_command("podman");
	if (!pd->have_podman) {
		gowl_bar_plugin_set_label(plugin, NULL);
		gowl_bar_plugin_set_icon(plugin, NULL);
		return;
	}

	out = bar_run_argv(argv);
	g_ptr_array_set_size(pd->running, 0);
	running = 0;
	pd->total = 0;

	if (out != NULL) {
		lines = g_strsplit(out, "\n", -1);
		for (i = 0; lines[i] != NULL; i++) {
			if (lines[i][0] == '\0')
				continue;
			pd->total++;
			if (strncmp(lines[i], "\t", 1) == 0)
				continue;
			g_ptr_array_add(pd->running, g_strdup(lines[i]));
			if (strstr(lines[i], "\tUp ") != NULL)
				running++;
			if (pd->running->len >= 24)
				break;
		}
	}

	g_snprintf(buf, sizeof(buf), "POD %d", running);
	gowl_bar_plugin_set_label(plugin, buf);
	gowl_bar_plugin_set_icon(plugin, "\xef\x8c\x88");
	gowl_bar_plugin_set_color(plugin,
		(running > 0) ? GOWL_BAR_COLOR_PEACH : GOWL_BAR_COLOR_MUTED);
}

static GowlBarPanel *
podman_panel(GowlBarPlugin *plugin, gpointer data)
{
	PodmanData *pd = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	gchar buf[32];
	guint i, running;

	(void)plugin;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 460);

	if (!pd->have_podman) {
		gowl_bar_panel_add_hero(panel, "\xef\x8c\x88", "Containers",
		                        "podman not found");
		return panel;
	}

	running = 0;
	for (i = 0; i < pd->running->len; i++) {
		if (strstr(g_ptr_array_index(pd->running, i), "\tUp ") != NULL)
			running++;
	}

	g_snprintf(buf, sizeof(buf), "%u", running);
	gowl_bar_panel_add_hero(panel, "\xef\x8c\x88", buf,
	                        "Containers running");

	g_snprintf(buf, sizeof(buf), "%d", pd->total);
	gowl_bar_panel_add_field(panel, "Defined", buf);

	if (pd->running->len == 0) {
		gowl_bar_panel_add_label(panel, "No containers");
		return panel;
	}

	gowl_bar_panel_add_separator(panel);
	gowl_bar_panel_add_section(panel, "Containers");

	for (i = 0; i < pd->running->len; i++) {
		g_auto(GStrv) fields = NULL;
		g_autofree gchar *row_id = NULL;
		gboolean up;

		fields = g_strsplit(g_ptr_array_index(pd->running, i), "\t",
		                    4);
		if (g_strv_length(fields) < 4)
			continue;
		up = (strncmp(fields[3], "Up", 2) == 0);

		row_id = g_strdup_printf("container:%s", fields[1]);
		item = gowl_bar_panel_add_row(panel, row_id,
			up ? "\xef\x81\x98" : "\xef\x81\x97",
			fields[1], fields[2]);
		gowl_bar_panel_item_set_value(item, fields[3]);
		gowl_bar_panel_item_set_value_color(item,
			up ? GOWL_BAR_COLOR_GREEN : GOWL_BAR_COLOR_MUTED);
		gowl_bar_panel_item_set_color(item,
			up ? GOWL_BAR_COLOR_GREEN : GOWL_BAR_COLOR_OVERLAY);
	}

	gowl_bar_panel_add_separator(panel);
	item = gowl_bar_panel_add_buttons(panel, "tool");
	gowl_bar_panel_add_button(item, "Refresh", FALSE);
	gowl_bar_panel_add_button(item, "Prune", FALSE);

	return panel;
}

static void
podman_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
              gint index, gdouble value, guint button)
{
	PodmanData *pd = data;
	g_autofree gchar *quoted = NULL;
	g_autofree gchar *line = NULL;

	(void)pd;
	(void)value;

	if (item_id != NULL && g_str_has_prefix(item_id, "container:")) {
		const gchar *name = item_id + strlen("container:");

		quoted = g_shell_quote(name);
		/* Left starts or stops, right opens the logs.  Two verbs
		   is as much as a bar panel should offer for something
		   this consequential. */
		if (button == BTN_RIGHT) {
			const gchar *term;

			term = g_getenv("TERMINAL");
			if (term == NULL || term[0] == '\0')
				term = "gst";
			line = g_strdup_printf("%s -e podman logs -f %s", term,
			                       quoted);
		} else {
			line = g_strdup_printf(
				"sh -c 'podman start %s || podman stop %s'",
				quoted, quoted);
		}
		gowl_bar_plugin_spawn(plugin, line);
		gowl_bar_plugin_refresh_panel(plugin);
		return;
	}

	if (g_strcmp0(item_id, "tool") != 0)
		return;

	if (index == 0) {
		gowl_bar_plugin_refresh_panel(plugin);
	} else if (index == 1) {
		gowl_bar_plugin_spawn(plugin, "podman container prune -f");
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
			"Pruning containers",
			"Stopped containers are being removed.");
	}
}

static const GowlBarPluginVTable podman_vtable = {
	sizeof(GowlBarPluginVTable),
	podman_create, podman_destroy,
	NULL, NULL, NULL,
	podman_interval, NULL, podman_poll_async,
	NULL, NULL,
	NULL, NULL,
	podman_panel, podman_action,
	NULL, NULL
};

/* ----------------------------------------------------------------
 * weather
 * ---------------------------------------------------------------- */

typedef struct {
	gchar *current;     /* the bar's short form */
	gchar *location;
	gchar *condition;
	gchar *temp;
	gchar *feels_like;
	gchar *wind;
	gchar *humidity;
	GPtrArray *forecast;  /* `day\tcond\thigh\tlow' rows */
} WeatherData;

static gpointer
weather_create(GowlBarPlugin *plugin)
{
	WeatherData *wd;

	(void)plugin;
	wd = g_new0(WeatherData, 1);
	wd->forecast = g_ptr_array_new_with_free_func(g_free);
	return wd;
}

static void
weather_destroy(GowlBarPlugin *plugin, gpointer data)
{
	WeatherData *wd = data;

	(void)plugin;
	if (wd == NULL)
		return;
	g_free(wd->current);
	g_free(wd->location);
	g_free(wd->condition);
	g_free(wd->temp);
	g_free(wd->feels_like);
	g_free(wd->wind);
	g_free(wd->humidity);
	g_ptr_array_unref(wd->forecast);
	g_free(wd);
}

static gint
weather_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	/* Fifteen minutes: the service is somebody else's, and a bar that
	   polls it every tick is a bar that gets rate limited. */
	return gowl_bar_plugin_get_setting_int(plugin, "refresh", 900);
}

static void
weather_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	WeatherData *wd = data;
	const gchar *location;
	const gchar *units;
	g_autofree gchar *url = NULL;
	g_autofree gchar *out = NULL;
	g_auto(GStrv) lines = NULL;
	const gchar *argv[] = { "curl", "-sf", "--max-time", "8", NULL, NULL };
	gint i;

	if (!bar_have_command("curl")) {
		gowl_bar_plugin_set_label(plugin, NULL);
		return;
	}

	location = gowl_bar_plugin_get_setting(plugin, "param");
	if (location == NULL)
		location = gowl_bar_plugin_get_setting(plugin, "location");
	if (location == NULL)
		location = "";

	units = gowl_bar_plugin_get_setting(plugin, "units");
	if (units == NULL)
		units = "u";   /* wttr.in: u = imperial, m = metric */

	/* One request, several fields, tab separated: the forecast rows
	   come from the same call so the panel never has to make a second
	   round trip while the user is looking at it. */
	url = g_strdup_printf(
		"wttr.in/%s?format=%%c\\t%%t\\t%%f\\t%%w\\t%%h\\t%%l&%s",
		location, units);
	argv[4] = url;

	out = bar_run_argv_line(argv);
	if (out != NULL) {
		g_auto(GStrv) fields = NULL;

		fields = g_strsplit(out, "\t", 6);
		if (g_strv_length(fields) >= 6) {
			g_free(wd->condition);
			wd->condition = g_strdup(g_strstrip(fields[0]));
			g_free(wd->temp);
			wd->temp = g_strdup(g_strstrip(fields[1]));
			g_free(wd->feels_like);
			wd->feels_like = g_strdup(g_strstrip(fields[2]));
			g_free(wd->wind);
			wd->wind = g_strdup(g_strstrip(fields[3]));
			g_free(wd->humidity);
			wd->humidity = g_strdup(g_strstrip(fields[4]));
			g_free(wd->location);
			wd->location = g_strdup(g_strstrip(fields[5]));

			g_free(wd->current);
			wd->current = g_strdup_printf("%s %s", wd->condition,
			                              wd->temp);
			gowl_bar_plugin_set_label(plugin, wd->current);
		}
	}

	/* The three-day outlook, as one more line-oriented request. */
	{
		g_autofree gchar *furl = NULL;
		g_autofree gchar *fout = NULL;
		const gchar *fargv[] = { "curl", "-sf", "--max-time", "8",
		                         NULL, NULL };

		furl = g_strdup_printf(
			"wttr.in/%s?format=j1&%s", location, units);
		fargv[4] = furl;
		fout = bar_run_argv(fargv);

		g_ptr_array_set_size(wd->forecast, 0);
		if (fout != NULL) {
			const gchar *p = fout;

			/* Only three fields per day are needed, and each
			   appears once per day object in order, so a
			   forward scan is enough --- no JSON parser for
			   three strings. */
			for (i = 0; i < 3; i++) {
				const gchar *date, *maxt, *mint;
				g_autofree gchar *d = NULL;
				g_autofree gchar *hi = NULL;
				g_autofree gchar *lo = NULL;

				date = strstr(p, "\"date\":\"");
				if (date == NULL)
					break;
				maxt = strstr(date,
					(units[0] == 'm') ? "\"maxtempC\":\""
					                  : "\"maxtempF\":\"");
				mint = strstr(date,
					(units[0] == 'm') ? "\"mintempC\":\""
					                  : "\"mintempF\":\"");
				if (maxt == NULL || mint == NULL)
					break;

				{
					const gchar *s = date + 8;
					const gchar *e = strchr(s, '"');

					if (e == NULL)
						break;
					d = g_strndup(s, (gsize)(e - s));
				}
				{
					const gchar *s = strchr(maxt + 12, '"');
					const gchar *e;

					if (s == NULL)
						break;
					s++;
					e = strchr(s, '"');
					if (e == NULL)
						break;
					hi = g_strndup(s, (gsize)(e - s));
				}
				{
					const gchar *s = strchr(mint + 12, '"');
					const gchar *e;

					if (s == NULL)
						break;
					s++;
					e = strchr(s, '"');
					if (e == NULL)
						break;
					lo = g_strndup(s, (gsize)(e - s));
				}

				g_ptr_array_add(wd->forecast,
					g_strdup_printf("%s\t%s\t%s", d, hi,
					                lo));
				p = mint;
			}
		}
	}
}

static GowlBarPanel *
weather_panel(GowlBarPlugin *plugin, gpointer data)
{
	WeatherData *wd = data;
	GowlBarPanel *panel;
	guint i;

	(void)plugin;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 420);

	if (wd->temp == NULL) {
		gowl_bar_panel_add_hero(panel, "\xef\x83\x82", "Weather",
		                        "No reading yet");
		return panel;
	}

	gowl_bar_panel_add_hero(panel,
		(wd->condition != NULL) ? wd->condition : "\xef\x83\x82",
		wd->temp,
		(wd->location != NULL) ? wd->location : "Here");

	gowl_bar_panel_add_field_pair(panel, "Feels",
		(wd->feels_like != NULL) ? wd->feels_like : "--", "Wind",
		(wd->wind != NULL) ? wd->wind : "--");
	gowl_bar_panel_add_field(panel, "Humidity",
		(wd->humidity != NULL) ? wd->humidity : "--");

	if (wd->forecast->len > 0) {
		gowl_bar_panel_add_separator(panel);
		gowl_bar_panel_add_section(panel, "Forecast");

		for (i = 0; i < wd->forecast->len; i++) {
			g_auto(GStrv) fields = NULL;
			g_autofree gchar *range = NULL;

			fields = g_strsplit(
				g_ptr_array_index(wd->forecast, i), "\t", 3);
			if (g_strv_length(fields) < 3)
				continue;
			range = g_strdup_printf("%s\xc2\xb0 / %s\xc2\xb0",
			                        fields[1], fields[2]);
			gowl_bar_panel_add_field(panel, fields[0], range);
		}
	}

	return panel;
}

static const GowlBarPluginVTable weather_vtable = {
	sizeof(GowlBarPluginVTable),
	weather_create, weather_destroy,
	NULL, NULL, NULL,
	weather_interval, NULL, weather_poll_async,
	NULL, NULL,
	NULL, NULL,
	weather_panel, NULL,
	NULL, NULL
};

/* ----------------------------------------------------------------
 * git -- the focused window's repository
 * ---------------------------------------------------------------- */

typedef struct {
	gchar *cwd;      /* written on the dispatch thread, read by the
	                    worker; only ever a whole-pointer swap */
	gchar *branch;
	gint   dirty;
} GitData;

static gpointer
git_create(GowlBarPlugin *plugin)
{
	(void)plugin;
	return g_new0(GitData, 1);
}

static void
git_destroy(GowlBarPlugin *plugin, gpointer data)
{
	GitData *gd = data;

	(void)plugin;
	if (gd == NULL)
		return;
	g_free(gd->cwd);
	g_free(gd->branch);
	g_free(gd);
}

static gint
git_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 5;
}

/* The focused client's working directory has to be read on the
   dispatch thread --- it walks compositor state --- so the sync poll
   snapshots it and the async poll does the git work. */
static void
git_poll(GowlBarPlugin *plugin, gpointer data)
{
	GitData *gd = data;
	const BarEnv *env = bar_env();
	GowlClient *focused;
	GowlProcessInfo *info;

	(void)plugin;

	if (env == NULL || env->compositor == NULL)
		return;

	focused = gowl_compositor_get_focused_client(
		GOWL_COMPOSITOR(env->compositor));
	if (focused == NULL)
		return;

	info = gowl_client_get_process_info(focused);
	if (info == NULL)
		return;
	if (info->cwd != NULL) {
		g_free(gd->cwd);
		gd->cwd = g_strdup(info->cwd);
	}
	gowl_process_info_free(info);
}

static void
git_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	GitData *gd = data;
	gchar dir[PATH_MAX];
	gchar head_path[PATH_MAX];
	g_autofree gchar *head = NULL;
	const gchar *branch;
	gchar buf[160];

	if (gd->cwd == NULL) {
		gowl_bar_plugin_set_label(plugin, NULL);
		return;
	}

	/* Walk up to the repository root the cheap way; `git rev-parse'
	   would be a subprocess per tick just to find the directory. */
	g_strlcpy(dir, gd->cwd, sizeof(dir));
	while (dir[0] != '\0') {
		gchar *slash;

		g_snprintf(head_path, sizeof(head_path), "%s/.git/HEAD", dir);
		if (g_file_test(head_path, G_FILE_TEST_EXISTS))
			break;
		slash = strrchr(dir, '/');
		if (slash == NULL || slash == dir) {
			dir[0] = '\0';
			break;
		}
		*slash = '\0';
	}

	if (dir[0] == '\0') {
		gowl_bar_plugin_set_label(plugin, NULL);
		return;
	}

	if (!g_file_get_contents(head_path, &head, NULL, NULL)) {
		gowl_bar_plugin_set_label(plugin, NULL);
		return;
	}
	g_strstrip(head);
	branch = (strncmp(head, "ref: refs/heads/", 16) == 0)
		? head + 16 : head;

	{
		const gchar *argv[] = { "git", "-C", NULL, "status",
		                        "--porcelain", NULL };
		g_autofree gchar *status = NULL;
		const gchar *p;

		argv[2] = dir;
		status = bar_run_argv(argv);
		gd->dirty = 0;
		if (status != NULL) {
			for (p = status; *p != '\0'; p++) {
				if (*p == '\n')
					gd->dirty++;
			}
		}
	}

	g_free(gd->branch);
	gd->branch = g_strdup(branch);

	if (gd->dirty > 0)
		g_snprintf(buf, sizeof(buf), "%s*%d", branch, gd->dirty);
	else
		g_snprintf(buf, sizeof(buf), "%s", branch);
	gowl_bar_plugin_set_label(plugin, buf);
	gowl_bar_plugin_set_icon(plugin, "\xef\x90\x98");
	gowl_bar_plugin_set_color(plugin,
		(gd->dirty > 0) ? GOWL_BAR_COLOR_YELLOW : GOWL_BAR_COLOR_TEXT);
}

static const GowlBarPluginVTable git_vtable = {
	sizeof(GowlBarPluginVTable),
	git_create, git_destroy,
	NULL, NULL, NULL,
	git_interval, git_poll, git_poll_async,
	NULL, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL, NULL
};

/* ----------------------------------------------------------------
 * keymap
 * ---------------------------------------------------------------- */

static gint
keymap_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 10;
}

static void
keymap_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	g_autofree gchar *out = NULL;

	(void)data;

	if (!bar_have_command("localectl"))
		return;
	{
		const gchar *argv[] = { "localectl", "status", NULL };

		out = bar_run_argv(argv);
	}
	if (out == NULL)
		return;

	{
		const gchar *p;

		p = strstr(out, "X11 Layout:");
		if (p == NULL)
			p = strstr(out, "VC Keymap:");
		if (p == NULL)
			return;
		p = strchr(p, ':');
		if (p == NULL)
			return;
		p++;
		while (*p == ' ')
			p++;
		{
			g_autofree gchar *layout = NULL;
			const gchar *nl = strchr(p, '\n');

			layout = (nl != NULL) ? g_strndup(p, (gsize)(nl - p))
			                      : g_strdup(p);
			g_strstrip(layout);
			gowl_bar_plugin_set_label(plugin, layout);
		}
	}
}

static const GowlBarPluginVTable keymap_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL, NULL, NULL, NULL,
	keymap_interval, NULL, keymap_poll_async,
	NULL, NULL, NULL, NULL,
	NULL, NULL,
	NULL, NULL
};

/* ----------------------------------------------------------------
 * display -- scale and brightness
 * ---------------------------------------------------------------- */

typedef struct {
	gint     brightness;    /* 0--100, or -1 when there is no backlight */
	gchar   *backlight;     /* the sysfs device name */
	gint     max_brightness;
} DisplayData;

static gpointer
display_create(GowlBarPlugin *plugin)
{
	DisplayData *dd;

	(void)plugin;
	dd = g_new0(DisplayData, 1);
	dd->brightness = -1;
	return dd;
}

static void
display_destroy(GowlBarPlugin *plugin, gpointer data)
{
	DisplayData *dd = data;

	(void)plugin;
	if (dd == NULL)
		return;
	g_free(dd->backlight);
	g_free(dd);
}

static gint
display_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 5;
}

/* Find the first backlight device.  A laptop has one; a desktop has
   none, and the panel then offers scale alone rather than a slider
   that does nothing. */
static void
display_find_backlight(DisplayData *dd)
{
	g_autoptr(GDir) dir = NULL;
	const gchar *name;

	if (dd->backlight != NULL)
		return;

	dir = g_dir_open("/sys/class/backlight", 0, NULL);
	if (dir == NULL)
		return;
	name = g_dir_read_name(dir);
	if (name == NULL)
		return;
	dd->backlight = g_strdup(name);
}

static void
display_poll(GowlBarPlugin *plugin, gpointer data)
{
	DisplayData *dd = data;
	g_autofree gchar *path = NULL;
	g_autofree gchar *max_path = NULL;
	g_autofree gchar *value = NULL;
	g_autofree gchar *max_value = NULL;

	display_find_backlight(dd);
	if (dd->backlight == NULL) {
		gowl_bar_plugin_set_icon(plugin, "\xef\x84\x88");
		gowl_bar_plugin_set_label(plugin, NULL);
		return;
	}

	path = g_strdup_printf("/sys/class/backlight/%s/brightness",
	                       dd->backlight);
	max_path = g_strdup_printf("/sys/class/backlight/%s/max_brightness",
	                           dd->backlight);
	if (!g_file_get_contents(path, &value, NULL, NULL) ||
	    !g_file_get_contents(max_path, &max_value, NULL, NULL))
		return;

	dd->max_brightness = (gint)g_ascii_strtoll(max_value, NULL, 10);
	if (dd->max_brightness <= 0)
		return;
	dd->brightness = (gint)((g_ascii_strtoll(value, NULL, 10) * 100) /
	                        dd->max_brightness);

	if (gowl_bar_plugin_get_setting_bool(plugin, "labels", FALSE)) {
		gchar buf[32];

		g_snprintf(buf, sizeof(buf), "%d%%", dd->brightness);
		gowl_bar_plugin_set_label(plugin, buf);
	} else {
		gowl_bar_plugin_set_label(plugin, NULL);
	}
	gowl_bar_plugin_set_icon(plugin, "\xef\x84\x88");
}

/*
 * The output scales offered, and the only place they are listed.  The
 * panel builds its buttons from this and the action indexes back into
 * it, so the two cannot drift into offering one scale and applying
 * another.
 */
static const struct {
	const gchar *label;
	gdouble      value;
} display_scales[] = {
	{ "1x",    1.0  },
	{ "1.25x", 1.25 },
	{ "1.6x",  1.6  },
	{ "2x",    2.0  },
	{ "3.2x",  3.2  },
	{ "4x",    4.0  }
};

static GowlBarPanel *
display_panel(GowlBarPlugin *plugin, gpointer data)
{
	DisplayData *dd = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	const BarEnv *env = bar_env();
	gchar buf[32];

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 420);

	gowl_bar_panel_add_hero(panel, "\xef\x84\x88", "Display",
		(dd->brightness >= 0) ? "Adjustable brightness"
		                      : "Fixed brightness");

	if (dd->brightness >= 0) {
		g_snprintf(buf, sizeof(buf), "%d%%", dd->brightness);
		item = gowl_bar_panel_add_slider(panel, "brightness",
			"Brightness", (gdouble)dd->brightness / 100.0);
		gowl_bar_panel_item_set_value(item, buf);
		gowl_bar_panel_item_set_step(item, 0.05);
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
	}

	if (env != NULL && env->compositor != NULL) {
		GowlMonitor *mon;

		mon = gowl_compositor_get_selected_monitor(
			GOWL_COMPOSITOR(env->compositor));
		if (mon != NULL) {
			gint mx, my, mw, mh;

			gowl_monitor_get_geometry(mon, &mx, &my, &mw, &mh);
			g_snprintf(buf, sizeof(buf), "%dx%d", mw, mh);
			gowl_bar_panel_add_field_pair(panel, "Output",
				gowl_monitor_get_name(mon), "Size", buf);
		}
	}

	gowl_bar_panel_add_separator(panel);
	gowl_bar_panel_add_section(panel, "Text size");
	item = gowl_bar_panel_add_buttons(panel, "text-size");
	gowl_bar_panel_add_button(item, "S", FALSE);
	gowl_bar_panel_add_button(item, "M", TRUE);
	gowl_bar_panel_add_button(item, "L", FALSE);
	gowl_bar_panel_add_button(item, "XL", FALSE);

	/*
	 * Output scale.  These apply immediately through
	 * gowl_monitor_set_scale(); the row is built from the same table
	 * the action reads, so the button that is lit is the scale the
	 * output is actually running at rather than a guess.
	 */
	if (env != NULL && env->compositor != NULL) {
		GowlMonitor *mon;

		mon = gowl_compositor_get_selected_monitor(
			GOWL_COMPOSITOR(env->compositor));
		if (mon != NULL) {
			gdouble cur = gowl_monitor_get_scale(mon);
			gsize   i;

			gowl_bar_panel_add_separator(panel);
			gowl_bar_panel_add_section(panel, "Scale");
			item = gowl_bar_panel_add_buttons(panel, "scale");
			for (i = 0; i < G_N_ELEMENTS(display_scales); i++) {
				gowl_bar_panel_add_button(item,
					display_scales[i].label,
					ABS(cur - display_scales[i].value) < 0.01);
			}
		}
	}

	gowl_bar_panel_add_separator(panel);
	item = gowl_bar_panel_add_buttons(panel, "tool");
	gowl_bar_panel_add_button(item, "Night light", FALSE);
	gowl_bar_panel_add_button(item, "Outputs", FALSE);

	return panel;
}

static void
display_set_brightness(DisplayData *dd, gdouble fraction)
{
	g_autofree gchar *line = NULL;

	if (dd->backlight == NULL || dd->max_brightness <= 0)
		return;

	/* brightnessctl if it is there; writing the sysfs file directly
	   needs root, and silently failing is worse than not offering. */
	if (!bar_have_command("brightnessctl"))
		return;

	line = g_strdup_printf("brightnessctl -d %s set %d%%", dd->backlight,
	                       (gint)(fraction * 100.0 + 0.5));
	bar_spawn_shell(line);
	dd->brightness = (gint)(fraction * 100.0 + 0.5);
}

static void
display_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
               gint index, gdouble value, guint button)
{
	DisplayData *dd = data;
	const GowlBarTheme *theme;

	(void)button;
	(void)theme;

	if (g_strcmp0(item_id, "brightness") == 0) {
		display_set_brightness(dd, value);
		return;
	}

	if (g_strcmp0(item_id, "text-size") == 0) {
		const gdouble scales[4] = { 0.85, 1.0, 1.2, 1.45 };
		g_autofree gchar *scale = NULL;

		if (index < 0 || index > 3)
			return;
		/* The theme scale is a bar setting, so this goes back
		   through the module's own configuration path rather than
		   reaching into the theme: a change made here should look
		   exactly like one made in the config file. */
		scale = g_strdup_printf("%.2f", scales[index]);
		gowl_bar_plugin_set_setting(plugin, "requested-scale", scale);
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW,
			"Bar text size",
			"Set theme-scale in the bar configuration to make "
			"this permanent.");
		return;
	}

	if (g_strcmp0(item_id, "scale") == 0) {
		const BarEnv *e = bar_env();
		GowlMonitor  *mon;

		if (index < 0 || (gsize)index >= G_N_ELEMENTS(display_scales))
			return;
		if (e == NULL || e->compositor == NULL)
			return;
		mon = gowl_compositor_get_selected_monitor(
			GOWL_COMPOSITOR(e->compositor));
		if (mon == NULL)
			return;
		if (gowl_monitor_set_scale(mon, display_scales[index].value)) {
			gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW,
				"Display scale", display_scales[index].label);
			gowl_bar_plugin_request_redraw(plugin);
		} else {
			gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
				"Display scale",
				"The output refused that scale.");
		}
		return;
	}

	if (g_strcmp0(item_id, "tool") == 0) {
		if (index == 0) {
			const gchar *cmd;

			cmd = gowl_bar_plugin_get_setting(plugin,
			                                  "nightlight-command");
			gowl_bar_plugin_spawn(plugin,
				(cmd != NULL) ? cmd
				              : "sh -c 'pkill gammastep || "
				                "gammastep -O 4000'");
		} else if (index == 1) {
			const gchar *cmd;

			cmd = gowl_bar_plugin_get_setting(plugin,
			                                  "outputs-command");
			gowl_bar_plugin_spawn(plugin,
				(cmd != NULL) ? cmd : "wdisplays");
		}
	}
}

static gboolean
display_scroll(GowlBarPlugin *plugin, gpointer data, gdouble delta,
               gint discrete, guint modifiers)
{
	DisplayData *dd = data;
	gint steps;

	(void)plugin;
	(void)modifiers;

	if (dd->brightness < 0)
		return FALSE;

	steps = (discrete != 0) ? -discrete : ((delta > 0.0) ? -1 : 1);
	display_set_brightness(dd,
		CLAMP((gdouble)(dd->brightness + steps * 5) / 100.0, 0.0, 1.0));
	return TRUE;
}

static const GowlBarPluginVTable display_vtable = {
	sizeof(GowlBarPluginVTable),
	display_create, display_destroy,
	NULL, NULL, NULL,
	display_interval, display_poll, NULL,
	NULL, NULL,
	NULL, display_scroll,
	display_panel, display_action,
	NULL, NULL
};


/* ----------------------------------------------------------------
 * recorder
 *
 * A screen recorder that asks WHAT to record before it starts.
 *
 * The button it replaces ran one fixed command --- typically
 * `wf-recorder -g "$(slurp)"' --- which quietly degrades into recording
 * everything when slurp is not installed, because an empty -g is not an
 * error.  Offering the choice explicitly also lets the compositor
 * supply the geometry it already knows: for a window there is no reason
 * to make the user draw a rectangle around something gowl can measure.
 *
 * Every command is a setting, so a different recorder can be dropped in
 * without touching this.
 * ---------------------------------------------------------------- */

typedef struct {
	gint64    started;      /* g_get_monotonic_time(), 0 when idle */
	gchar    *scope;        /* what the running recording covers */
} RecorderData;

static gpointer
recorder_create(GowlBarPlugin *plugin)
{
	(void)plugin;
	return g_new0(RecorderData, 1);
}

static void
recorder_destroy(GowlBarPlugin *plugin, gpointer data)
{
	RecorderData *rd = data;

	(void)plugin;
	if (rd == NULL)
		return;
	g_free(rd->scope);
	g_free(rd);
}

/* Whether a recording this plugin started is still running.  Asking the
   process table rather than trusting our own flag: the recorder can be
   stopped from anywhere, and a button that says "Stop" for a process
   that already exited is worse than no button. */
static gboolean
recorder_running(GowlBarPlugin *plugin)
{
	const gchar      *proc;
	g_autofree gchar *line = NULL;
	g_autofree gchar *out = NULL;

	proc = gowl_bar_plugin_get_setting(plugin, "process");
	if (proc == NULL || *proc == '\0')
		proc = "wf-recorder";
	if (!bar_have_command("pidof"))
		return FALSE;
	line = g_strdup_printf("pidof %s", proc);
	out = bar_run_shell_line(line);
	return out != NULL && *out != '\0';
}

static void
recorder_poll(GowlBarPlugin *plugin, gpointer data)
{
	RecorderData *rd = data;
	gchar buf[64];

	if (rd == NULL)
		return;

	if (!recorder_running(plugin)) {
		if (rd->started != 0) {
			rd->started = 0;
			g_clear_pointer(&rd->scope, g_free);
		}
		gowl_bar_plugin_set_label(plugin, NULL);
		gowl_bar_plugin_set_icon(plugin, "\xef\x8f\x9b");
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_TEXT);
		gowl_bar_plugin_set_tooltip(plugin, "Record the screen");
		return;
	}

	if (rd->started == 0)
		rd->started = g_get_monotonic_time();

	g_snprintf(buf, sizeof(buf), "%02d:%02d",
	           (gint)((g_get_monotonic_time() - rd->started) / 60000000),
	           (gint)(((g_get_monotonic_time() - rd->started) / 1000000) % 60));
	gowl_bar_plugin_set_label(plugin, buf);
	gowl_bar_plugin_set_icon(plugin, "\xef\x8f\x9b");
	gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_RED);
	gowl_bar_plugin_set_tooltip(plugin,
		rd->scope != NULL ? rd->scope : "Recording");
}

static gint
recorder_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 1;
}

static GowlBarPanel *
recorder_panel(GowlBarPlugin *plugin, gpointer data)
{
	RecorderData     *rd = data;
	GowlBarPanel     *panel;
	GowlBarPanelItem *item;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 380);

	if (recorder_running(plugin)) {
		gowl_bar_panel_add_hero(panel, "\xef\x8f\x9b", "Recording",
			rd != NULL && rd->scope != NULL ? rd->scope : NULL);
		gowl_bar_panel_add_separator(panel);
		item = gowl_bar_panel_add_buttons(panel, "stop");
		gowl_bar_panel_add_button(item, "Stop", TRUE);
		return panel;
	}

	gowl_bar_panel_add_hero(panel, "\xef\x8f\x9b", "Record",
	                        "Choose what to capture");
	gowl_bar_panel_add_separator(panel);
	gowl_bar_panel_add_section(panel, "Capture");
	item = gowl_bar_panel_add_buttons(panel, "start");
	gowl_bar_panel_add_button(item, "Screen", FALSE);
	gowl_bar_panel_add_button(item, "Window", FALSE);
	gowl_bar_panel_add_button(item, "Region", FALSE);

	if (!bar_have_command("wf-recorder"))
		gowl_bar_panel_add_field(panel, "Missing", "wf-recorder");
	else if (!bar_have_command("slurp"))
		gowl_bar_panel_add_field(panel, "Note",
			"slurp is missing; Region is unavailable");

	return panel;
}

/* The geometry for each scope, or NULL to let the recorder decide.
   Window geometry comes from the compositor rather than from the user
   drawing a box round something it already knows the bounds of. */
static gchar *
recorder_geometry(gint index, gchar **scope_out)
{
	const BarEnv *env = bar_env();
	GowlClient   *c;
	gint          x, y, w, h;

	if (index == 1 && env != NULL && env->compositor != NULL) {
		c = gowl_compositor_get_focused_client(
			GOWL_COMPOSITOR(env->compositor));
		if (c != NULL) {
			gowl_client_get_geometry(c, &x, &y, &w, &h);
			if (w > 0 && h > 0) {
				*scope_out = g_strdup("Focused window");
				return g_strdup_printf("%d,%d %dx%d",
				                       x, y, w, h);
			}
		}
		return NULL;
	}

	if (index == 2) {
		*scope_out = g_strdup("Region");
		return g_strdup("$(slurp)");
	}

	*scope_out = g_strdup("Whole screen");
	return NULL;
}

static void
recorder_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
                gint index, gdouble value, guint button)
{
	RecorderData     *rd = data;
	g_autofree gchar *geom = NULL;
	g_autofree gchar *scope = NULL;
	g_autofree gchar *line = NULL;
	const gchar      *dir;
	const gchar      *proc;

	(void)value;
	(void)button;

	if (g_strcmp0(item_id, "stop") == 0) {
		proc = gowl_bar_plugin_get_setting(plugin, "process");
		if (proc == NULL || *proc == '\0')
			proc = "wf-recorder";
		line = g_strdup_printf("pkill -INT %s", proc);
		gowl_bar_plugin_spawn(plugin, line);
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW,
			"Recording stopped", NULL);
		return;
	}

	if (g_strcmp0(item_id, "start") != 0)
		return;

	if (!bar_have_command("wf-recorder")) {
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
			"Cannot record", "wf-recorder is not installed.");
		return;
	}
	if (index == 2 && !bar_have_command("slurp")) {
		/* Without this the command becomes -g "" and wf-recorder
		   records the whole screen, which is not what was asked
		   for and gives no hint that anything went wrong. */
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
			"Cannot record a region",
			"slurp is not installed.");
		return;
	}

	geom = recorder_geometry(index, &scope);
	if (index == 1 && geom == NULL) {
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
			"Cannot record a window", "Nothing is focused.");
		return;
	}

	dir = gowl_bar_plugin_get_setting(plugin, "directory");
	if (dir == NULL || *dir == '\0')
		dir = "~/Videos";

	if (geom != NULL)
		line = g_strdup_printf(
			"mkdir -p %s && wf-recorder -g \"%s\" "
			"-f %s/rec-$(date +%%F-%%H%%M%%S).mp4",
			dir, geom, dir);
	else
		line = g_strdup_printf(
			"mkdir -p %s && wf-recorder "
			"-f %s/rec-$(date +%%F-%%H%%M%%S).mp4",
			dir, dir);

	gowl_bar_plugin_spawn(plugin, line);
	if (rd != NULL) {
		g_free(rd->scope);
		rd->scope = g_steal_pointer(&scope);
		rd->started = 0;   /* poll starts the clock once it is up */
	}
	gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW,
		"Recording", rd != NULL ? rd->scope : NULL);
}

static const GowlBarPluginVTable recorder_vtable = {
	sizeof(GowlBarPluginVTable),
	recorder_create, recorder_destroy,
	NULL, NULL, NULL,
	recorder_interval, recorder_poll, NULL,
	NULL, NULL,
	NULL, NULL,
	recorder_panel, recorder_action,
	NULL, NULL
};


/* ----------------------------------------------------------------
 * Registration
 * ---------------------------------------------------------------- */

/**
 * bar_register_desktop_plugins:
 * @registry: the registry to populate
 */
void
bar_register_desktop_plugins(GowlBarRegistry *registry)
{
	gowl_bar_registry_register_vtable(registry, "audio", "Audio",
		"Output and input volume, with a device panel",
		&audio_vtable);
	gowl_bar_registry_register_vtable(registry, "media", "Media",
		"What is playing, with transport controls", &media_vtable);
	gowl_bar_registry_register_vtable(registry, "podman", "Containers",
		"Running containers, with start and stop", &podman_vtable);
	gowl_bar_registry_register_vtable(registry, "weather", "Weather",
		"Current conditions and a three-day outlook",
		&weather_vtable);
	gowl_bar_registry_register_vtable(registry, "git", "Git branch",
		"The focused window's repository", &git_vtable);
	gowl_bar_registry_register_vtable(registry, "keymap", "Keyboard layout",
		"The active keyboard layout", &keymap_vtable);
	gowl_bar_registry_register_vtable(registry, "display", "Display",
		"Brightness and output settings", &display_vtable);

	gowl_bar_registry_register_alias(registry, "volume", "audio");
	gowl_bar_registry_register_alias(registry, "vol", "audio");
	gowl_bar_registry_register_alias(registry, "pod", "podman");
	gowl_bar_registry_register_vtable(registry, "recorder",
		"Screen recorder",
		"Record the screen, a window or a region", &recorder_vtable);

	gowl_bar_registry_register_alias(registry, "brightness", "display");
	gowl_bar_registry_register_alias(registry, "record", "recorder");
}
