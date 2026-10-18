/* test-touchpad-defaults.c -- what a touchpad gets before any config
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A two-finger click on the touchpad did not right click.  libinput's
 * own default click method on a clickpad (a touchpad whose whole
 * surface is the button) is "button areas": the right button is the
 * bottom-right corner, and a press with two fingers anywhere else is a
 * LEFT click.  GNOME and KDE switch such touchpads to "clickfinger",
 * where the number of fingers decides the button; gowl's defaults
 * covered tap, scrolling and disable-while-typing, and left the click
 * method where libinput put it.
 *
 * No touchpad is needed to test that, and none could be relied on:
 * creating one takes /dev/uinput, which is root's.  The libinput
 * configuration calls are defined here instead -- the test is linked
 * with --export-dynamic, so libgowl's calls into libinput resolve to
 * these -- and record what the defaults asked for.  The device is a
 * fake whose capabilities each case chooses.
 */

#include <glib.h>
#include <libinput.h>
#include <string.h>

#include "core/gowl-core-private.h"

/* ── A pretend libinput device ───────────────────────────────────── */

typedef struct {
	/* what the device can do */
	gint     tap_fingers;
	guint32  click_methods;
	guint32  scroll_methods;
	gboolean natural_scroll;
	gboolean dwt;
	/* what it was told */
	gboolean set_click;
	enum libinput_config_click_method click;
	gboolean set_tap;
	gboolean set_scroll;
	gboolean set_natural;
	gboolean set_dwt;
} FakeDevice;

#define FAKE(li) ((FakeDevice *)(gpointer)(li))

int
libinput_device_config_tap_get_finger_count(struct libinput_device *li)
{
	return FAKE(li)->tap_fingers;
}

enum libinput_config_status
libinput_device_config_tap_set_enabled(struct libinput_device *li,
                                       enum libinput_config_tap_state s)
{
	FAKE(li)->set_tap = s == LIBINPUT_CONFIG_TAP_ENABLED;
	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

enum libinput_config_status
libinput_device_config_tap_set_drag_enabled(struct libinput_device *li,
                                            enum libinput_config_drag_state s)
{
	(void)li;
	(void)s;
	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

int
libinput_device_config_scroll_has_natural_scroll(struct libinput_device *li)
{
	return FAKE(li)->natural_scroll;
}

enum libinput_config_status
libinput_device_config_scroll_set_natural_scroll_enabled(
	struct libinput_device *li, int enable)
{
	FAKE(li)->set_natural = enable != 0;
	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

uint32_t
libinput_device_config_scroll_get_methods(struct libinput_device *li)
{
	return FAKE(li)->scroll_methods;
}

enum libinput_config_status
libinput_device_config_scroll_set_method(struct libinput_device *li,
                                         enum libinput_config_scroll_method m)
{
	FAKE(li)->set_scroll = m == LIBINPUT_CONFIG_SCROLL_2FG;
	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

int
libinput_device_config_dwt_is_available(struct libinput_device *li)
{
	return FAKE(li)->dwt;
}

enum libinput_config_status
libinput_device_config_dwt_set_enabled(struct libinput_device *li,
                                       enum libinput_config_dwt_state s)
{
	FAKE(li)->set_dwt = s == LIBINPUT_CONFIG_DWT_ENABLED;
	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

uint32_t
libinput_device_config_click_get_methods(struct libinput_device *li)
{
	return FAKE(li)->click_methods;
}

enum libinput_config_status
libinput_device_config_click_set_method(struct libinput_device *li,
                                        enum libinput_config_click_method m)
{
	FAKE(li)->set_click = TRUE;
	FAKE(li)->click = m;
	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

/* ── The cases ───────────────────────────────────────────────────── */

/* A clickpad that can do both: two fingers is a right click. */
static void
test_clickpad_gets_clickfinger(void)
{
	FakeDevice d;

	memset(&d, 0, sizeof(d));
	d.tap_fingers = 3;
	d.click_methods = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS
	                | LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER;
	d.scroll_methods = LIBINPUT_CONFIG_SCROLL_2FG
	                 | LIBINPUT_CONFIG_SCROLL_EDGE;
	d.natural_scroll = TRUE;
	d.dwt = TRUE;

	gowl_input_config_touchpad_defaults((struct libinput_device *)&d);

	g_assert_true(d.set_click);
	g_assert_cmpint(d.click, ==, LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER);
	/* and the defaults it always had are still there */
	g_assert_true(d.set_tap);
	g_assert_true(d.set_scroll);
	g_assert_true(d.set_natural);
	g_assert_true(d.set_dwt);
}

/* A touchpad that cannot clickfinger is left on whatever it can do,
 * rather than being asked for a method libinput would refuse. */
static void
test_no_clickfinger_left_alone(void)
{
	FakeDevice d;

	memset(&d, 0, sizeof(d));
	d.tap_fingers = 2;
	d.click_methods = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;

	gowl_input_config_touchpad_defaults((struct libinput_device *)&d);

	g_assert_false(d.set_click);
	g_assert_true(d.set_tap);
}

/* A mouse reports no tap fingers and is not a touchpad: nothing about
 * it changes, its wheel direction least of all. */
static void
test_mouse_untouched(void)
{
	FakeDevice d;

	memset(&d, 0, sizeof(d));
	d.tap_fingers = 0;
	d.click_methods = LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER;
	d.scroll_methods = LIBINPUT_CONFIG_SCROLL_2FG;
	d.natural_scroll = TRUE;

	gowl_input_config_touchpad_defaults((struct libinput_device *)&d);

	g_assert_false(d.set_click);
	g_assert_false(d.set_tap);
	g_assert_false(d.set_natural);
	g_assert_false(d.set_scroll);
}

/* Not a libinput device at all (a virtual pointer, a nested backend's). */
static void
test_null_device(void)
{
	gowl_input_config_touchpad_defaults(NULL);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/input/touchpad-defaults/clickpad-gets-clickfinger",
	                test_clickpad_gets_clickfinger);
	g_test_add_func("/input/touchpad-defaults/no-clickfinger-left-alone",
	                test_no_clickfinger_left_alone);
	g_test_add_func("/input/touchpad-defaults/mouse-untouched",
	                test_mouse_untouched);
	g_test_add_func("/input/touchpad-defaults/null-device",
	                test_null_device);
	return g_test_run();
}
