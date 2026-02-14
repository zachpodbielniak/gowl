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

#include "gowl-log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static GLogLevelFlags gowl_min_log_level = G_LOG_LEVEL_WARNING;
static FILE          *gowl_log_fp        = NULL;
static gboolean       gowl_log_writer_set = FALSE;

/**
 * level_name:
 * @log_level: a #GLogLevelFlags value
 *
 * Returns a short human-readable label for a log level.
 *
 * Returns: a static string like "ERROR", "WARNING", etc.
 */
static const gchar *
level_name(GLogLevelFlags log_level)
{
	if (log_level & G_LOG_LEVEL_ERROR)
		return "ERROR";
	if (log_level & G_LOG_LEVEL_CRITICAL)
		return "CRITICAL";
	if (log_level & G_LOG_LEVEL_WARNING)
		return "WARNING";
	if (log_level & G_LOG_LEVEL_MESSAGE)
		return "MESSAGE";
	if (log_level & G_LOG_LEVEL_INFO)
		return "INFO";
	if (log_level & G_LOG_LEVEL_DEBUG)
		return "DEBUG";
	return "LOG";
}

/**
 * gowl_log_writer:
 *
 * GLib structured log writer callback.  Filters by the configured
 * minimum log level, then writes to the log file if one is open,
 * otherwise delegates to the default writer (stderr / journal).
 */
static GLogWriterOutput
gowl_log_writer(
	GLogLevelFlags   log_level,
	const GLogField *fields,
	gsize            n_fields,
	gpointer         user_data
){
	const gchar *message;
	const gchar *domain;
	gsize i;
	time_t now;
	struct tm tm_buf;
	gchar time_str[64];

	(void)user_data;

	/* Filter messages below our configured level */
	if (log_level > gowl_min_log_level)
		return G_LOG_WRITER_HANDLED;

	/* If no file is open, use the default writer (stderr / journal) */
	if (gowl_log_fp == NULL)
		return g_log_writer_default(log_level, fields, n_fields, user_data);

	/* Extract message and domain from structured fields */
	message = NULL;
	domain  = "gowl";
	for (i = 0; i < n_fields; i++) {
		if (g_strcmp0(fields[i].key, "MESSAGE") == 0)
			message = (const gchar *)fields[i].value;
		else if (g_strcmp0(fields[i].key, "GLIB_DOMAIN") == 0)
			domain = (const gchar *)fields[i].value;
	}

	if (message == NULL)
		return G_LOG_WRITER_HANDLED;

	/* Format timestamp */
	now = time(NULL);
	localtime_r(&now, &tm_buf);
	strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

	/* Write to log file */
	fprintf(gowl_log_fp, "%s [%s] %s: %s\n",
	        time_str, level_name(log_level), domain, message);
	fflush(gowl_log_fp);

	return G_LOG_WRITER_HANDLED;
}

void
gowl_log_init(const gchar *level, const gchar *log_file)
{
	/* Parse log level */
	if (level == NULL || g_ascii_strcasecmp(level, "warning") == 0) {
		gowl_min_log_level = G_LOG_LEVEL_WARNING;
	} else if (g_ascii_strcasecmp(level, "debug") == 0) {
		gowl_min_log_level = G_LOG_LEVEL_DEBUG;
	} else if (g_ascii_strcasecmp(level, "info") == 0) {
		gowl_min_log_level = G_LOG_LEVEL_INFO;
	} else if (g_ascii_strcasecmp(level, "error") == 0) {
		gowl_min_log_level = G_LOG_LEVEL_ERROR;
	} else {
		gowl_min_log_level = G_LOG_LEVEL_WARNING;
	}

	/*
	 * Open log file if specified and not "stderr".
	 * Expand leading "~" to $HOME.
	 */
	if (log_file != NULL &&
	    g_ascii_strcasecmp(log_file, "stderr") != 0 &&
	    log_file[0] != '\0') {
		g_autofree gchar *expanded = NULL;
		g_autofree gchar *dir = NULL;

		/* Expand ~ to home directory */
		if (log_file[0] == '~' && log_file[1] == G_DIR_SEPARATOR) {
			expanded = g_build_filename(
				g_get_home_dir(), log_file + 2, NULL);
		} else {
			expanded = g_strdup(log_file);
		}

		/* Ensure the parent directory exists */
		dir = g_path_get_dirname(expanded);
		if (g_mkdir_with_parents(dir, 0755) == 0) {
			gowl_log_fp = fopen(expanded, "a");
			if (gowl_log_fp == NULL) {
				g_printerr("gowl: failed to open log file: %s\n",
				           expanded);
			}
		} else {
			g_printerr("gowl: failed to create log directory: %s\n",
			           dir);
		}
	}

	/* Only register the writer function once; GLib forbids repeat calls */
	if (!gowl_log_writer_set) {
		g_log_set_writer_func(gowl_log_writer, NULL, NULL);
		gowl_log_writer_set = TRUE;
	}
}
