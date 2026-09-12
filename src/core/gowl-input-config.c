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
 * Per-device input settings: the `input:' section applied through
 * libinput.
 *
 * The compositor keeps a list of every input device it has been
 * handed, so that a settings change on reload reaches the devices
 * already plugged in and not only the next one.  A device's settings
 * are looked up by class (`touchpad', `pointer', `keyboard'), by a
 * glob on its name, or `*'; the last block that sets a key wins.
 *
 * Every setting is optional and only touched when set, on top of the
 * GNOME-like touchpad defaults create_pointer() applies first (tap,
 * tap-drag, natural scroll, two-finger scroll, disable-while-typing).
 * A setting a device cannot do is skipped with a debug line rather
 * than a warning: a config shared between a laptop and a desktop
 * names touchpad settings the desktop's mouse has no use for.
 */

#include "gowl-core-private.h"
#include <wlr/backend/libinput.h>

typedef struct {
	GowlCompositor          *compositor;
	struct wlr_input_device *device;
	struct wl_listener       destroy;
} GowlTrackedDevice;

static gboolean
setting_bool(const gchar *v, gboolean fallback)
{
	if (v == NULL)
		return fallback;
	return g_ascii_strcasecmp(v, "true") == 0
	       || g_ascii_strcasecmp(v, "yes") == 0
	       || g_ascii_strcasecmp(v, "on") == 0
	       || g_ascii_strcasecmp(v, "enabled") == 0
	       || g_strcmp0(v, "1") == 0;
}

/* Which class a device belongs to for the `input:' match. */
static const gchar *
device_class(struct wlr_input_device *dev, struct libinput_device *li)
{
	switch (dev->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		return "keyboard";
	case WLR_INPUT_DEVICE_POINTER:
		if (li != NULL
		    && libinput_device_config_tap_get_finger_count(li) > 0)
			return "touchpad";
		return "pointer";
	case WLR_INPUT_DEVICE_TOUCH:
		return "touch";
	case WLR_INPUT_DEVICE_TABLET:
	case WLR_INPUT_DEVICE_TABLET_PAD:
		return "tablet";
	default:
		return "other";
	}
}

#define LOOKUP(key) \
	gowl_config_lookup_input_setting(self->config, dev->name, cls, (key))

/**
 * gowl_input_config_apply_device:
 * @self: the compositor
 * @dev: an input device
 *
 * Applies every `input:' setting that names this device.  Safe for a
 * device that is not libinput's (a virtual pointer, a nested
 * backend's): nothing applies.
 */
void
gowl_input_config_apply_device(
	GowlCompositor          *self,
	struct wlr_input_device *dev
){
	struct libinput_device *li;
	const gchar *cls;
	const gchar *v;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(dev != NULL);

	if (self->config == NULL || !wlr_input_device_is_libinput(dev))
		return;
	li = wlr_libinput_get_device_handle(dev);
	if (li == NULL)
		return;
	cls = device_class(dev, li);

	/* enabled: true | false | disabled-on-external-mouse */
	v = LOOKUP("enabled");
	if (v != NULL && libinput_device_config_send_events_get_modes(li)) {
		guint32 mode;

		if (g_ascii_strcasecmp(v, "disabled-on-external-mouse") == 0)
			mode = LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE;
		else
			mode = setting_bool(v, TRUE)
			       ? LIBINPUT_CONFIG_SEND_EVENTS_ENABLED
			       : LIBINPUT_CONFIG_SEND_EVENTS_DISABLED;
		libinput_device_config_send_events_set_mode(li, mode);
	}

	if (libinput_device_config_tap_get_finger_count(li) > 0) {
		v = LOOKUP("tap");
		if (v != NULL)
			libinput_device_config_tap_set_enabled(li,
				setting_bool(v, TRUE) ? LIBINPUT_CONFIG_TAP_ENABLED
				                      : LIBINPUT_CONFIG_TAP_DISABLED);
		v = LOOKUP("tap-drag");
		if (v != NULL)
			libinput_device_config_tap_set_drag_enabled(li,
				setting_bool(v, TRUE) ? LIBINPUT_CONFIG_DRAG_ENABLED
				                      : LIBINPUT_CONFIG_DRAG_DISABLED);
		v = LOOKUP("tap-drag-lock");
		if (v != NULL)
			libinput_device_config_tap_set_drag_lock_enabled(li,
				setting_bool(v, FALSE)
				? LIBINPUT_CONFIG_DRAG_LOCK_ENABLED
				: LIBINPUT_CONFIG_DRAG_LOCK_DISABLED);
		v = LOOKUP("tap-button-map");
		if (v != NULL)
			libinput_device_config_tap_set_button_map(li,
				g_ascii_strcasecmp(v, "lmr") == 0
				? LIBINPUT_CONFIG_TAP_MAP_LMR
				: LIBINPUT_CONFIG_TAP_MAP_LRM);
	}

	v = LOOKUP("natural-scroll");
	if (v != NULL && libinput_device_config_scroll_has_natural_scroll(li))
		libinput_device_config_scroll_set_natural_scroll_enabled(li,
			setting_bool(v, FALSE));

	v = LOOKUP("scroll-method");
	if (v != NULL) {
		guint32 methods = libinput_device_config_scroll_get_methods(li);
		enum libinput_config_scroll_method m = LIBINPUT_CONFIG_SCROLL_NO_SCROLL;

		if (g_ascii_strcasecmp(v, "two-finger") == 0
		    || g_ascii_strcasecmp(v, "2fg") == 0)
			m = LIBINPUT_CONFIG_SCROLL_2FG;
		else if (g_ascii_strcasecmp(v, "edge") == 0)
			m = LIBINPUT_CONFIG_SCROLL_EDGE;
		else if (g_ascii_strcasecmp(v, "button") == 0
		         || g_ascii_strcasecmp(v, "on-button-down") == 0)
			m = LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;
		if (m == LIBINPUT_CONFIG_SCROLL_NO_SCROLL || (methods & m))
			libinput_device_config_scroll_set_method(li, m);
		else
			g_debug("input: '%s' cannot scroll by %s", dev->name, v);
	}

	v = LOOKUP("scroll-button");
	if (v != NULL)
		libinput_device_config_scroll_set_button(li, (guint32)atoi(v));

	v = LOOKUP("click-method");
	if (v != NULL) {
		guint32 methods = libinput_device_config_click_get_methods(li);
		enum libinput_config_click_method m = LIBINPUT_CONFIG_CLICK_METHOD_NONE;

		if (g_ascii_strcasecmp(v, "button-areas") == 0)
			m = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
		else if (g_ascii_strcasecmp(v, "clickfinger") == 0)
			m = LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER;
		if (m == LIBINPUT_CONFIG_CLICK_METHOD_NONE || (methods & m))
			libinput_device_config_click_set_method(li, m);
		else
			g_debug("input: '%s' cannot click by %s", dev->name, v);
	}

	v = LOOKUP("accel-profile");
	if (v != NULL) {
		guint32 profiles = libinput_device_config_accel_get_profiles(li);
		enum libinput_config_accel_profile p =
			g_ascii_strcasecmp(v, "flat") == 0
			? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
			: LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;

		if (profiles & p)
			libinput_device_config_accel_set_profile(li, p);
	}

	v = LOOKUP("accel-speed");
	if (v != NULL && libinput_device_config_accel_is_available(li))
		libinput_device_config_accel_set_speed(li,
			CLAMP(g_ascii_strtod(v, NULL), -1.0, 1.0));

	v = LOOKUP("left-handed");
	if (v != NULL && libinput_device_config_left_handed_is_available(li))
		libinput_device_config_left_handed_set(li, setting_bool(v, FALSE));

	v = LOOKUP("middle-emulation");
	if (v != NULL
	    && libinput_device_config_middle_emulation_is_available(li))
		libinput_device_config_middle_emulation_set_enabled(li,
			setting_bool(v, FALSE)
			? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED
			: LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED);

	v = LOOKUP("dwt");
	if (v != NULL && libinput_device_config_dwt_is_available(li))
		libinput_device_config_dwt_set_enabled(li,
			setting_bool(v, TRUE) ? LIBINPUT_CONFIG_DWT_ENABLED
			                      : LIBINPUT_CONFIG_DWT_DISABLED);

	v = LOOKUP("dwtp");
	if (v != NULL && libinput_device_config_dwtp_is_available(li))
		libinput_device_config_dwtp_set_enabled(li,
			setting_bool(v, TRUE) ? LIBINPUT_CONFIG_DWTP_ENABLED
			                      : LIBINPUT_CONFIG_DWTP_DISABLED);

	v = LOOKUP("rotation");
	if (v != NULL && libinput_device_config_rotation_is_available(li))
		libinput_device_config_rotation_set_angle(li,
			(guint)(atoi(v) % 360 + 360) % 360);
}

#undef LOOKUP

/**
 * gowl_input_config_apply_all:
 * @self: the compositor
 *
 * Re-applies the settings to every device known: what a config
 * reload calls.
 */
void
gowl_input_config_apply_all(GowlCompositor *self)
{
	GList *l;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));

	for (l = self->input_devices; l != NULL; l = l->next) {
		GowlTrackedDevice *t = (GowlTrackedDevice *)l->data;

		gowl_input_config_apply_device(self, t->device);
	}
}

void
gowl_compositor_apply_input_config(GowlCompositor *self)
{
	gowl_input_config_apply_all(self);
}

static void
on_tracked_device_destroy(struct wl_listener *listener, void *data)
{
	GowlTrackedDevice *t = wl_container_of(listener, t, destroy);
	(void)data;

	wl_list_remove(&t->destroy.link);
	t->compositor->input_devices =
		g_list_remove(t->compositor->input_devices, t);
	g_free(t);
}

/**
 * gowl_input_config_track_device:
 * @self: the compositor
 * @dev: a device that just appeared
 *
 * Remembers the device until it goes, and applies its settings now.
 */
void
gowl_input_config_track_device(
	GowlCompositor          *self,
	struct wlr_input_device *dev
){
	GowlTrackedDevice *t;

	g_return_if_fail(GOWL_IS_COMPOSITOR(self));
	g_return_if_fail(dev != NULL);

	t = g_new0(GowlTrackedDevice, 1);
	t->compositor = self;
	t->device = dev;
	t->destroy.notify = on_tracked_device_destroy;
	wl_signal_add(&dev->events.destroy, &t->destroy);
	self->input_devices = g_list_prepend(self->input_devices, t);

	gowl_input_config_apply_device(self, dev);
}

/**
 * gowl_input_config_finish:
 * @self: the compositor, during teardown, before the backend destroys
 *   its devices
 */
void
gowl_input_config_finish(GowlCompositor *self)
{
	GList *l;

	for (l = self->input_devices; l != NULL; l = l->next) {
		GowlTrackedDevice *t = (GowlTrackedDevice *)l->data;

		wl_list_remove(&t->destroy.link);
		g_free(t);
	}
	g_clear_pointer(&self->input_devices, g_list_free);
}

/**
 * gowl_input_config_foreach_keyboard:
 * @self: the compositor
 * @func: called with each keyboard device
 * @user_data: passed through
 *
 * Every keyboard the compositor has been handed, for a keymap change
 * that has to reach the members of the group and not only the group.
 */
void
gowl_input_config_foreach_keyboard(
	GowlCompositor *self,
	void          (*func)(struct wlr_keyboard *kb, gpointer user_data),
	gpointer        user_data
){
	GList *l;

	for (l = self->input_devices; l != NULL; l = l->next) {
		GowlTrackedDevice *t = (GowlTrackedDevice *)l->data;

		if (t->device->type == WLR_INPUT_DEVICE_KEYBOARD)
			func(wlr_keyboard_from_input_device(t->device), user_data);
	}
}
