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

#include "gowl-input-remapper.h"

G_DEFINE_INTERFACE(GowlInputRemapper, gowl_input_remapper, G_TYPE_OBJECT)

static void
gowl_input_remapper_default_init(GowlInputRemapperInterface *iface)
{
	/* Default implementation - claims nothing, maps nothing */
	(void)iface;
}

/**
 * gowl_input_remapper_claims_device:
 * @self: a #GowlInputRemapper
 * @info: a connected device's identity
 *
 * Returns: %TRUE when @self wants the device's input routed to it
 */
gboolean
gowl_input_remapper_claims_device(
	GowlInputRemapper         *self,
	const GowlInputDeviceInfo *info
){
	GowlInputRemapperInterface *iface;

	g_return_val_if_fail(GOWL_IS_INPUT_REMAPPER(self), FALSE);
	g_return_val_if_fail(info != NULL, FALSE);

	iface = GOWL_INPUT_REMAPPER_GET_IFACE(self);
	if (iface->claims_device == NULL)
		return FALSE;
	return iface->claims_device(self, info);
}

/**
 * gowl_input_remapper_map_event:
 * @self: a #GowlInputRemapper
 * @info: the claimed device
 * @event: one physical input from it
 * @out_target: (out) (transfer none) (nullable): the target the input
 *   maps to, owned by the returned rule
 *
 * Returns: (transfer full) (nullable): the rule that decided the input,
 *   or %NULL to pass it through
 */
GowlInputRemapRule *
gowl_input_remapper_map_event(
	GowlInputRemapper           *self,
	const GowlInputDeviceInfo   *info,
	const GowlInputRemapEvent   *event,
	const GowlInputRemapTarget **out_target
){
	GowlInputRemapperInterface *iface;

	g_return_val_if_fail(out_target != NULL, NULL);
	*out_target = NULL;
	g_return_val_if_fail(GOWL_IS_INPUT_REMAPPER(self), NULL);
	g_return_val_if_fail(info != NULL && event != NULL, NULL);

	iface = GOWL_INPUT_REMAPPER_GET_IFACE(self);
	if (iface->map_event == NULL)
		return NULL;
	return iface->map_event(self, info, event, out_target);
}

/**
 * gowl_input_remapper_device_changed:
 * @self: a #GowlInputRemapper
 * @info: the device, with @claimed already updated
 * @claimed: %TRUE when the core just claimed it, %FALSE when released
 */
void
gowl_input_remapper_device_changed(
	GowlInputRemapper         *self,
	const GowlInputDeviceInfo *info,
	gboolean                   claimed
){
	GowlInputRemapperInterface *iface;

	g_return_if_fail(GOWL_IS_INPUT_REMAPPER(self));
	g_return_if_fail(info != NULL);

	iface = GOWL_INPUT_REMAPPER_GET_IFACE(self);
	if (iface->device_changed != NULL)
		iface->device_changed(self, info, claimed);
}
