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

#ifndef GOWL_BARKIT_TYPES_H
#define GOWL_BARKIT_TYPES_H

#include <glib-object.h>

G_BEGIN_DECLS

/*
 * Forward declarations shared by the barkit headers.  A plugin needs
 * to name its host and a host needs to name its plugins, so one of the
 * two references has to arrive before the type that declares it.
 */

typedef struct _GowlBarPlugin   GowlBarPlugin;
typedef struct _GowlBarHost     GowlBarHost;
typedef struct _GowlBarRegistry GowlBarRegistry;

G_END_DECLS

#endif /* GOWL_BARKIT_TYPES_H */
