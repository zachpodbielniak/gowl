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

#ifndef GOWL_H
#define GOWL_H

/* Version */
#include "gowl-version.h"

/* Type system */
#include "gowl-types.h"
#include "gowl-enums.h"

/* Boxed types */
#include "boxed/gowl-geometry.h"
#include "boxed/gowl-color.h"
#include "boxed/gowl-key-combo.h"
#include "boxed/gowl-tag-mask.h"
#include "boxed/gowl-gaps.h"
#include "boxed/gowl-border-spec.h"
#include "boxed/gowl-rule.h"
#include "boxed/gowl-output-mode.h"
#include "boxed/gowl-process-info.h"
#include "boxed/gowl-capture-source.h"

/* Module system */
#include "module/gowl-module.h"
#include "module/gowl-module-manager.h"
#include "module/gowl-module-info.h"

/* Interfaces */
#include "interfaces/gowl-layout-provider.h"
#include "interfaces/gowl-keybind-handler.h"
#include "interfaces/gowl-mouse-handler.h"
#include "interfaces/gowl-client-decorator.h"
#include "interfaces/gowl-client-placer.h"
#include "interfaces/gowl-focus-policy.h"
#include "interfaces/gowl-monitor-configurator.h"
#include "interfaces/gowl-rule-provider.h"
#include "interfaces/gowl-startup-handler.h"
#include "interfaces/gowl-shutdown-handler.h"
#include "interfaces/gowl-ipc-handler.h"
#include "interfaces/gowl-tag-manager.h"
#include "interfaces/gowl-gap-provider.h"
#include "interfaces/gowl-bar-provider.h"
#include "interfaces/gowl-scratchpad-handler.h"
#include "interfaces/gowl-dropdown-provider.h"
#include "interfaces/gowl-swallow-handler.h"
#include "interfaces/gowl-sticky-handler.h"
#include "interfaces/gowl-cursor-provider.h"
#include "interfaces/gowl-wallpaper-provider.h"
#include "interfaces/gowl-lock-handler.h"
#include "interfaces/gowl-screenshot-provider.h"
#include "interfaces/gowl-recording-provider.h"
#include "interfaces/gowl-prefix-key-policy.h"
#include "interfaces/gowl-session-provider.h"
#include "interfaces/gowl-embed-renderer.h"
#include "interfaces/gowl-workspace-provider.h"
#include "interfaces/gowl-capture-provider.h"

/* Boxed types */
#include "boxed/gowl-capture-result.h"
#include "boxed/gowl-focus-token.h"
#include "boxed/gowl-workspace-id.h"

/* Configuration */
#include "config/gowl-config.h"
#include "config/gowl-config-compiler.h"
#include "config/gowl-keybind.h"

/* Layouts */
#include "layout/gowl-layout-tile.h"
#include "layout/gowl-layout-monocle.h"
#include "layout/gowl-layout-float.h"

/* Core */
#include "core/gowl-compositor.h"
#include "core/gowl-monitor.h"
#include "core/gowl-client.h"
#include "core/gowl-seat.h"
#include "core/gowl-keyboard-group.h"
#include "core/gowl-cursor.h"
#include "core/gowl-layer-surface.h"
#include "core/gowl-bar.h"
#include "core/gowl-session-lock.h"
#include "core/gowl-idle-manager.h"
#include "core/gowl-static-prefix-key-policy.h"
#include "core/gowl-session-default.h"
#include "core/gowl-mirror.h"
#include "core/gowl-workspace.h"
#include "core/gowl-frame-workspace-manager.h"
#include "core/gowl-cairo-embed-renderer.h"
#include "core/gowl-gl-embed-renderer.h"

/* IPC */
#include "ipc/gowl-ipc.h"

/* Utilities */
#include "util/gowl-log.h"

#endif /* GOWL_H */
