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

#ifndef GOWL_LOG_H
#define GOWL_LOG_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * gowl_log_init:
 * @level: log level string ("debug", "info", "warning", "error")
 * @log_file: (nullable): path to log file, "stderr" for stderr only,
 *            or %NULL for stderr only.  The path may contain "~" which
 *            is expanded to $HOME.
 * @truncate: if %TRUE, the log file is truncated (overwritten) instead
 *            of appended to.  Useful for debug sessions where a fresh
 *            log is desired on each launch.
 *
 * Initialize the gowl logging system.  When @log_file is a valid path,
 * log messages are written to the file.  When set to "stderr" or %NULL,
 * messages go to stderr (or the systemd journal if available).
 */
void
gowl_log_init(const gchar *level, const gchar *log_file, gboolean truncate);

G_END_DECLS

#endif /* GOWL_LOG_H */
