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

#ifndef GOWL_PROCESS_INFO_H
#define GOWL_PROCESS_INFO_H

#include "gowl-types.h"

G_BEGIN_DECLS

#define GOWL_TYPE_PROCESS_INFO (gowl_process_info_get_type())

/**
 * GowlProcessInfo:
 * @pid: Process ID.
 * @comm: Process name from /proc/PID/comm.
 * @cmdline: Full command line from /proc/PID/cmdline.
 * @cwd: Current working directory from /proc/PID/cwd.
 *
 * Information about the process behind a Wayland client.
 */
typedef struct _GowlProcessInfo GowlProcessInfo;

struct _GowlProcessInfo {
	pid_t    pid;
	gchar   *comm;
	gchar   *cmdline;
	gchar   *cwd;
};

GType             gowl_process_info_get_type (void) G_GNUC_CONST;

GowlProcessInfo * gowl_process_info_new      (pid_t pid);

GowlProcessInfo * gowl_process_info_copy     (const GowlProcessInfo *self);

void              gowl_process_info_free     (GowlProcessInfo *self);

G_END_DECLS

#endif /* GOWL_PROCESS_INFO_H */
