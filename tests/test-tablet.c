/* test-tablet.c -- tablet lifetime guards
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A tablet needs a started compositor to do anything, so what a unit
 * test can reach is the NOT-started case -- which is precisely where a
 * crash would be worst, because it is the shutdown path.  gowl tears
 * down on every exit, including exits that happened before any device
 * arrived, and the device lists are wl_lists that are garbage until
 * wl_list_init runs.
 *
 * The real routing is covered by tests/test-input-device-guard.sh (that
 * tablets reach a handler at all) and by driving a pen at a live
 * compositor, which no unit test can do.
 */

#include <glib-object.h>

#include "core/gowl-compositor.h"
#include "core/gowl-tablet.h"

#include <wlr/types/wlr_input_device.h>

/*
 * Shutdown before any device arrived.  The lists were never
 * initialised, so a wl_list_for_each over them would walk a NULL next
 * pointer.  gowl_tablet_finish has to notice and do nothing.
 */
static void
test_finish_without_init(void)
{
	GowlCompositor *c = gowl_compositor_new();

	gowl_tablet_finish(c);       /* must not crash */
	gowl_tablet_finish(c);       /* twice, for good measure */

	g_object_unref(c);
}

/* No tablet is attached to a compositor that never started. */
static void
test_no_devices_before_start(void)
{
	GowlCompositor *c = gowl_compositor_new();

	g_assert_false(gowl_tablet_has_devices(c));

	g_object_unref(c);
}

/*
 * The handler is documented as safe to hand every device, so a pointer
 * or a keyboard must come back FALSE rather than being half-adopted.
 * The caller relies on this: it sits in the `default:' arm of the
 * device switch and sees whatever the other arms did not take.
 */
static void
test_non_tablet_device_is_refused(void)
{
	GowlCompositor *c = gowl_compositor_new();
	struct wlr_input_device pointer = { 0 };
	struct wlr_input_device keyboard = { 0 };

	pointer.type = WLR_INPUT_DEVICE_POINTER;
	keyboard.type = WLR_INPUT_DEVICE_KEYBOARD;

	g_assert_false(gowl_tablet_new_device(c, &pointer));
	g_assert_false(gowl_tablet_new_device(c, &keyboard));

	g_object_unref(c);
}

/*
 * A tablet offered before startup is refused too: there is no manager
 * yet, so adopting it would leave a GowlTablet pointing at a NULL
 * protocol object that every later event would dereference.
 */
static void
test_tablet_refused_without_manager(void)
{
	GowlCompositor *c = gowl_compositor_new();
	struct wlr_input_device tablet = { 0 };

	tablet.type = WLR_INPUT_DEVICE_TABLET;

	g_assert_false(gowl_tablet_new_device(c, &tablet));

	g_object_unref(c);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/tablet/finish-without-init",
	                test_finish_without_init);
	g_test_add_func("/tablet/no-devices-before-start",
	                test_no_devices_before_start);
	g_test_add_func("/tablet/non-tablet-refused",
	                test_non_tablet_device_is_refused);
	g_test_add_func("/tablet/tablet-refused-without-manager",
	                test_tablet_refused_without_manager);

	return g_test_run();
}
