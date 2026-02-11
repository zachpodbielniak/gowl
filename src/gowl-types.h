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

#ifndef GOWL_TYPES_H
#define GOWL_TYPES_H

#include <glib-object.h>

G_BEGIN_DECLS

/* Forward declarations for core types */
typedef struct _GowlCompositor      GowlCompositor;
typedef struct _GowlMonitor         GowlMonitor;
typedef struct _GowlClient          GowlClient;
typedef struct _GowlSeat            GowlSeat;
typedef struct _GowlKeyboardGroup   GowlKeyboardGroup;
typedef struct _GowlCursor          GowlCursor;
typedef struct _GowlLayerSurface    GowlLayerSurface;
typedef struct _GowlBar             GowlBar;
typedef struct _GowlSessionLock     GowlSessionLock;
typedef struct _GowlIdleManager     GowlIdleManager;

/* Forward declarations for config types */
typedef struct _GowlConfig          GowlConfig;
typedef struct _GowlConfigCompiler  GowlConfigCompiler;

/* Forward declarations for module types */
typedef struct _GowlModule          GowlModule;
typedef struct _GowlModuleClass     GowlModuleClass;
typedef struct _GowlModuleManager   GowlModuleManager;

/* Forward declarations for boxed types */
typedef struct _GowlGeometry        GowlGeometry;
typedef struct _GowlColor           GowlColor;
typedef struct _GowlKeyCombo        GowlKeyCombo;
typedef struct _GowlTagMask         GowlTagMask;
typedef struct _GowlGaps            GowlGaps;
typedef struct _GowlBorderSpec      GowlBorderSpec;
typedef struct _GowlRule            GowlRule;
typedef struct _GowlOutputMode      GowlOutputMode;
typedef struct _GowlModuleInfo      GowlModuleInfo;

/* Forward declaration for IPC */
typedef struct _GowlIpc             GowlIpc;

/* Maximum number of tags (bitmask-based, 1-31) */
#define GOWL_MAX_TAGS (31)

/* Default tag count */
#define GOWL_DEFAULT_TAG_COUNT (9)

/* Macro to create tag bitmask from index (0-based) */
#define GOWL_TAGMASK(n) ((guint32)(1 << (n)))

/* Macro to check if tag is set in bitmask */
#define GOWL_TAG_IS_SET(mask, n) (((mask) & GOWL_TAGMASK(n)) != 0)

/* All tags bitmask */
#define GOWL_TAGMASK_ALL(count) ((guint32)((1 << (count)) - 1))

/* Clean modifier mask (remove Caps Lock bit 1 and Num Lock bit 4) */
#define GOWL_CLEANMASK(mask) ((mask) & ~((guint)(1 << 1)) & ~((guint)(1 << 4)))

G_END_DECLS

#endif /* GOWL_TYPES_H */
