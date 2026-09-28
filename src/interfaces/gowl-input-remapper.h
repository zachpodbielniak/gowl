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

#ifndef GOWL_INPUT_REMAPPER_H
#define GOWL_INPUT_REMAPPER_H

#include <glib-object.h>

#include "boxed/gowl-input-remap-rule.h"

G_BEGIN_DECLS

#define GOWL_TYPE_INPUT_REMAPPER (gowl_input_remapper_get_type())

G_DECLARE_INTERFACE(GowlInputRemapper, gowl_input_remapper, GOWL, INPUT_REMAPPER, GObject)

/**
 * GowlInputRemapperInterface:
 * @parent_iface: the parent interface
 * @claims_device: whether a connected device should be taken out of
 *   the shared keyboard group (keyboards) or have its buttons and wheel
 *   offered to @map_event (pointers).  Asked on hotplug and again each
 *   time the module calls gowl_compositor_input_remap_reevaluate().
 * @map_event: for one input on a claimed device, the rule that decides
 *   it and the ONE target it maps to.  Return %NULL to pass the input
 *   through unchanged.  Returning a rule with a %NULL target means the
 *   rule does not mention the input: the core then drops or passes it
 *   per gowl_input_remap_rule_get_drop_unmatched().
 * @device_changed: (nullable): told when the core claims or releases a
 *   device, for logging and IPC events.  Optional.
 *
 * Per-device input remapping.  The core owns the devices and the event
 * delivery; the implementation owns the rules.  Only the first active
 * implementation is consulted, and with none loaded the core's input
 * path is exactly what it is without this interface.
 *
 * Called on the compositor thread, in the middle of input handling:
 * implementations must answer from memory, never block.
 */
struct _GowlInputRemapperInterface {
	GTypeInterface parent_iface;

	gboolean             (*claims_device)  (GowlInputRemapper          *self,
	                                        const GowlInputDeviceInfo  *info);
	GowlInputRemapRule  *(*map_event)      (GowlInputRemapper          *self,
	                                        const GowlInputDeviceInfo  *info,
	                                        const GowlInputRemapEvent  *event,
	                                        const GowlInputRemapTarget **out_target);
	void                 (*device_changed) (GowlInputRemapper          *self,
	                                        const GowlInputDeviceInfo  *info,
	                                        gboolean                    claimed);
};

gboolean            gowl_input_remapper_claims_device  (GowlInputRemapper           *self,
                                                        const GowlInputDeviceInfo   *info);
GowlInputRemapRule *gowl_input_remapper_map_event      (GowlInputRemapper           *self,
                                                        const GowlInputDeviceInfo   *info,
                                                        const GowlInputRemapEvent   *event,
                                                        const GowlInputRemapTarget **out_target);
void                gowl_input_remapper_device_changed (GowlInputRemapper           *self,
                                                        const GowlInputDeviceInfo   *info,
                                                        gboolean                     claimed);

G_END_DECLS

#endif /* GOWL_INPUT_REMAPPER_H */
