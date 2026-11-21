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

/*
 * gowl-inputremap-vpad.h - Virtual gamepads, the inputremap module's
 * output for stick, trigger and pad-button targets.
 *
 * Wayland has no gamepad protocol: a game reads controllers from
 * /dev/input itself.  So the only way to give a game stick motion is a
 * real (virtual) input device, made through /dev/uinput.  A rule's
 * `virtual-pad:' declares one; it is created on its first use, kept
 * while any rule still declares it, and destroyed when none does, when
 * the module is disabled and when the compositor goes.
 *
 * Every pad is named GOWL_INPUT_REMAP_PAD_NAME_PREFIX + its name, which
 * is how the core's controller reader knows never to read one back.
 */

#ifndef GOWL_INPUTREMAP_VPAD_H
#define GOWL_INPUTREMAP_VPAD_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include <wayland-server-core.h>

#include "boxed/gowl-input-remap-rule.h"

G_BEGIN_DECLS

typedef struct _GowlInputRemapVpads GowlInputRemapVpads;

GowlInputRemapVpads *gowl_inputremap_vpads_new         (void);
void                 gowl_inputremap_vpads_free        (GowlInputRemapVpads        *self);
void                 gowl_inputremap_vpads_set_loop    (GowlInputRemapVpads        *self,
                                                        struct wl_event_loop       *loop);
gboolean             gowl_inputremap_vpads_render      (GowlInputRemapVpads        *self,
                                                        GowlInputRemapRule         *rule,
                                                        const GowlInputRemapTarget *target,
                                                        const GowlInputRemapEvent  *event,
                                                        gdouble                     amount);
void                 gowl_inputremap_vpads_sync        (GowlInputRemapVpads        *self,
                                                        GPtrArray                  *rules);
void                 gowl_inputremap_vpads_destroy_all (GowlInputRemapVpads        *self);
guint                gowl_inputremap_vpads_count       (GowlInputRemapVpads        *self);
const gchar         *gowl_inputremap_vpads_get_error   (GowlInputRemapVpads        *self);
void                 gowl_inputremap_vpads_to_json     (GowlInputRemapVpads        *self,
                                                        JsonBuilder                *b);
gboolean             gowl_inputremap_vpads_available   (void);

G_END_DECLS

#endif /* GOWL_INPUTREMAP_VPAD_H */
