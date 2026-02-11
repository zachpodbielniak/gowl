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
#include <string.h>

static GLogLevelFlags gowl_min_log_level = G_LOG_LEVEL_WARNING;

static GLogWriterOutput
gowl_log_writer(
	GLogLevelFlags   log_level,
	const GLogField *fields,
	gsize            n_fields,
	gpointer         user_data
){
	(void)user_data;

	/* Filter messages below our configured level */
	if (log_level > gowl_min_log_level)
		return G_LOG_WRITER_HANDLED;

	/* Use the default structured log writer */
	return g_log_writer_default(log_level, fields, n_fields, user_data);
}

void
gowl_log_init(const gchar *level)
{
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

	g_log_set_writer_func(gowl_log_writer, NULL, NULL);
}
