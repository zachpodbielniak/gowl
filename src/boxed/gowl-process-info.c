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

#include "gowl-process-info.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

G_DEFINE_BOXED_TYPE(GowlProcessInfo, gowl_process_info,
                    gowl_process_info_copy, gowl_process_info_free)

/**
 * gowl_process_info_new:
 * @pid: the process ID to query
 *
 * Creates a new #GowlProcessInfo by reading /proc/@pid.
 * Fields that cannot be read are set to %NULL.
 *
 * Returns: (transfer full): a newly allocated #GowlProcessInfo.
 *          Free with gowl_process_info_free().
 */
GowlProcessInfo *
gowl_process_info_new(pid_t pid)
{
	GowlProcessInfo *self;
	gchar path[256];
	gchar buf[4096];
	gchar *contents;
	gsize len;
	ssize_t n;

	self = g_slice_new0(GowlProcessInfo);
	self->pid = pid;

	/* Read /proc/PID/comm */
	g_snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	if (g_file_get_contents(path, &contents, &len, NULL)) {
		g_strstrip(contents);
		self->comm = contents;
	}

	/* Read /proc/PID/cmdline (NUL-separated → space-separated) */
	g_snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
	if (g_file_get_contents(path, &contents, &len, NULL)) {
		gsize i;

		for (i = 0; i < len; i++) {
			if (contents[i] == '\0')
				contents[i] = ' ';
		}
		g_strstrip(contents);
		self->cmdline = contents;
	}

	/* Read /proc/PID/cwd (symlink) */
	g_snprintf(path, sizeof(path), "/proc/%d/cwd", (int)pid);
	n = readlink(path, buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		self->cwd = g_strdup(buf);
	}

	return self;
}

/**
 * gowl_process_info_copy:
 * @self: (not nullable): a #GowlProcessInfo to copy
 *
 * Creates a deep copy of @self.
 *
 * Returns: (transfer full): a newly allocated copy.
 */
GowlProcessInfo *
gowl_process_info_copy(const GowlProcessInfo *self)
{
	GowlProcessInfo *copy;

	g_return_val_if_fail(self != NULL, NULL);

	copy = g_slice_new0(GowlProcessInfo);
	copy->pid     = self->pid;
	copy->comm    = g_strdup(self->comm);
	copy->cmdline = g_strdup(self->cmdline);
	copy->cwd     = g_strdup(self->cwd);

	return copy;
}

/**
 * gowl_process_info_free:
 * @self: (nullable): a #GowlProcessInfo to free
 *
 * Releases all memory associated with @self.
 */
void
gowl_process_info_free(GowlProcessInfo *self)
{
	if (self == NULL)
		return;

	g_free(self->comm);
	g_free(self->cmdline);
	g_free(self->cwd);
	g_slice_free(GowlProcessInfo, self);
}
