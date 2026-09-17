/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Turning bluetoothctl's listings into the list the panel shows.
 *
 * The bug this exists for: the widget asked for `devices Paired' and
 * `devices Connected' and nothing else.  Between them those are every
 * device EXCEPT the ones a scan exists to find, so turning scanning on
 * started a scan and then had nowhere to put what it found -- which
 * from the outside is a scan that does not scan.
 *
 * The other half is what happens when a scan works too well.  In a cafe
 * a scan turns up dozens of other people's headphones, and a list that
 * truncates in listing order loses YOUR headphones to somebody else's.
 * Standing decides the order, and only then does the limit bite.
 */

#include <glib.h>
#include <string.h>

#include "../modules/bar/bar-bt-devices.c"

#define DEVICES_LIMIT (12)

static GPtrArray *
parse(const gchar *all, const gchar *paired, const gchar *connected,
      guint limit)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(bar_bt_device_free);

	bar_bt_devices_parse(out, all, paired, connected, limit);
	return out;
}

static const BarBtDevice *
by_mac(GPtrArray *devices, const gchar *mac)
{
	guint i;

	for (i = 0; i < devices->len; i++) {
		const BarBtDevice *d = g_ptr_array_index(devices, i);

		if (g_strcmp0(d->mac, mac) == 0)
			return d;
	}
	return NULL;
}

/* ------------------------------------------------------------------ *
 * The reported bug
 * ------------------------------------------------------------------ */

static void
test_a_discovered_device_is_listed(void)
{
	g_autoptr(GPtrArray) devices = NULL;
	const BarBtDevice *d;

	/*
	 * One paired device and one the scan just turned up.  The second
	 * is in `devices' and in neither filtered listing -- which is
	 * exactly the shape the old code could not see.
	 */
	devices = parse("Device AA:AA:AA:AA:AA:AA My Headphones\n"
	                "Device BB:BB:BB:BB:BB:BB Somebody Speaker\n",
	                "Device AA:AA:AA:AA:AA:AA My Headphones\n",
	                NULL, DEVICES_LIMIT);

	g_assert_cmpuint(devices->len, ==, 2);

	d = by_mac(devices, "BB:BB:BB:BB:BB:BB");
	g_assert_nonnull(d);
	g_assert_cmpstr(d->name, ==, "Somebody Speaker");
	g_assert_false(d->paired);
	g_assert_false(d->connected);
}

static void
test_paired_and_connected_are_marked(void)
{
	g_autoptr(GPtrArray) devices = NULL;
	const BarBtDevice *d;

	devices = parse("Device AA:AA:AA:AA:AA:AA Headphones\n"
	                "Device BB:BB:BB:BB:BB:BB Keyboard\n"
	                "Device CC:CC:CC:CC:CC:CC Stranger\n",
	                "Device AA:AA:AA:AA:AA:AA Headphones\n"
	                "Device BB:BB:BB:BB:BB:BB Keyboard\n",
	                "Device AA:AA:AA:AA:AA:AA Headphones\n",
	                DEVICES_LIMIT);

	d = by_mac(devices, "AA:AA:AA:AA:AA:AA");
	g_assert_true(d->paired);
	g_assert_true(d->connected);

	d = by_mac(devices, "BB:BB:BB:BB:BB:BB");
	g_assert_true(d->paired);
	g_assert_false(d->connected);

	d = by_mac(devices, "CC:CC:CC:CC:CC:CC");
	g_assert_false(d->paired);
	g_assert_false(d->connected);
}

static void
test_connected_implies_paired(void)
{
	g_autoptr(GPtrArray) devices = NULL;
	const BarBtDevice *d;

	/*
	 * Connected but absent from the Paired listing.  Being connected
	 * to something unpaired is not a state bluetooth has, and a row
	 * that claimed it would offer to pair a device already in use.
	 */
	devices = parse("Device AA:AA:AA:AA:AA:AA Headphones\n", NULL,
	                "Device AA:AA:AA:AA:AA:AA Headphones\n",
	                DEVICES_LIMIT);

	d = by_mac(devices, "AA:AA:AA:AA:AA:AA");
	g_assert_nonnull(d);
	g_assert_true(d->connected);
	g_assert_true(d->paired);
}

/* ------------------------------------------------------------------ *
 * Order, and what the limit drops
 * ------------------------------------------------------------------ */

static void
test_standing_decides_the_order(void)
{
	g_autoptr(GPtrArray) devices = NULL;
	const BarBtDevice *first, *second, *third;

	/* Listed worst-first on purpose: the order out is not the order
	 * in. */
	devices = parse("Device CC:CC:CC:CC:CC:CC Stranger\n"
	                "Device BB:BB:BB:BB:BB:BB Keyboard\n"
	                "Device AA:AA:AA:AA:AA:AA Headphones\n",
	                "Device BB:BB:BB:BB:BB:BB Keyboard\n"
	                "Device AA:AA:AA:AA:AA:AA Headphones\n",
	                "Device AA:AA:AA:AA:AA:AA Headphones\n",
	                DEVICES_LIMIT);

	first  = g_ptr_array_index(devices, 0);
	second = g_ptr_array_index(devices, 1);
	third  = g_ptr_array_index(devices, 2);

	g_assert_cmpstr(first->mac,  ==, "AA:AA:AA:AA:AA:AA"); /* connected */
	g_assert_cmpstr(second->mac, ==, "BB:BB:BB:BB:BB:BB"); /* paired */
	g_assert_cmpstr(third->mac,  ==, "CC:CC:CC:CC:CC:CC"); /* found */
}

static void
test_the_limit_drops_strangers_not_yours(void)
{
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GString) all = g_string_new(NULL);
	guint i;

	/*
	 * A cafe: twenty devices in the air, and yours is the last one
	 * the adapter happens to print.  Truncating in listing order
	 * would drop exactly the device the panel exists to offer.
	 */
	for (i = 0; i < 20; i++)
		g_string_append_printf(all,
			"Device 00:00:00:00:00:%02X Stranger %u\n", i, i);
	g_string_append(all, "Device AA:AA:AA:AA:AA:AA My Headphones\n");

	devices = parse(all->str, "Device AA:AA:AA:AA:AA:AA My Headphones\n",
	                NULL, 5);

	g_assert_cmpuint(devices->len, ==, 5);
	g_assert_nonnull(by_mac(devices, "AA:AA:AA:AA:AA:AA"));
	g_assert_cmpstr(((BarBtDevice *)g_ptr_array_index(devices, 0))->mac,
	                ==, "AA:AA:AA:AA:AA:AA");
}

static void
test_order_within_a_group_is_the_listing_order(void)
{
	g_autoptr(GPtrArray) devices = NULL;

	/* Nothing reorders devices of equal standing: the adapter's own
	 * order is as good as any and a list that shuffled itself between
	 * polls would be unusable. */
	devices = parse("Device 11:11:11:11:11:11 One\n"
	                "Device 22:22:22:22:22:22 Two\n"
	                "Device 33:33:33:33:33:33 Three\n",
	                NULL, NULL, DEVICES_LIMIT);

	g_assert_cmpstr(((BarBtDevice *)g_ptr_array_index(devices, 0))->name,
	                ==, "One");
	g_assert_cmpstr(((BarBtDevice *)g_ptr_array_index(devices, 1))->name,
	                ==, "Two");
	g_assert_cmpstr(((BarBtDevice *)g_ptr_array_index(devices, 2))->name,
	                ==, "Three");
}

/* ------------------------------------------------------------------ *
 * The listings disagreeing, and malformed input
 * ------------------------------------------------------------------ */

static void
test_a_device_only_the_filters_know_is_kept(void)
{
	g_autoptr(GPtrArray) devices = NULL;
	const BarBtDevice *d;

	/* `devices' missed it somehow.  Dropping a device you own because
	 * two listings disagreed is worse than the disagreement. */
	devices = parse("Device BB:BB:BB:BB:BB:BB Stranger\n",
	                "Device AA:AA:AA:AA:AA:AA Headphones\n",
	                NULL, DEVICES_LIMIT);

	d = by_mac(devices, "AA:AA:AA:AA:AA:AA");
	g_assert_nonnull(d);
	g_assert_true(d->paired);
	g_assert_cmpstr(d->name, ==, "Headphones");
}

static void
test_a_device_in_every_listing_appears_once(void)
{
	g_autoptr(GPtrArray) devices = NULL;

	devices = parse("Device AA:AA:AA:AA:AA:AA Headphones\n",
	                "Device AA:AA:AA:AA:AA:AA Headphones\n",
	                "Device AA:AA:AA:AA:AA:AA Headphones\n",
	                DEVICES_LIMIT);

	g_assert_cmpuint(devices->len, ==, 1);
}

static void
test_a_name_with_spaces_is_kept_whole(void)
{
	g_autoptr(GPtrArray) devices = NULL;
	const BarBtDevice *d;

	devices = parse("Device AA:AA:AA:AA:AA:AA Zach's WH-1000XM5 (LE)\n",
	                NULL, NULL, DEVICES_LIMIT);

	d = g_ptr_array_index(devices, 0);
	g_assert_cmpstr(d->name, ==, "Zach's WH-1000XM5 (LE)");
	g_assert_cmpstr(d->mac, ==, "AA:AA:AA:AA:AA:AA");
}

static void
test_noise_is_ignored(void)
{
	g_autoptr(GPtrArray) devices = NULL;

	/*
	 * bluetoothctl prints more than device lines -- agent messages,
	 * a prompt, a controller line -- and a device line with no name
	 * is not one this can use.
	 */
	devices = parse("Agent registered\n"
	                "Controller 8C:B8:7E:C9:EE:A5 lt-zach [default]\n"
	                "Device \n"
	                "Device AA:AA:AA:AA:AA:AA Real One\n"
	                "\n",
	                NULL, NULL, DEVICES_LIMIT);

	g_assert_cmpuint(devices->len, ==, 1);
	g_assert_cmpstr(((BarBtDevice *)g_ptr_array_index(devices, 0))->name,
	                ==, "Real One");
}

static void
test_a_device_with_no_address_is_ignored(void)
{
	g_autoptr(GPtrArray) devices = NULL;
	guint i;

	/*
	 * "Device" then straight to the name: the address is empty.  A row
	 * built from that has nothing to pair or connect to, and every
	 * such line would collapse into ONE device because they all match
	 * each other on the empty address.
	 */
	devices = parse("Device  Nameless One\n"
	                "Device  Nameless Two\n"
	                "Device AA:AA:AA:AA:AA:AA Real One\n",
	                NULL, NULL, DEVICES_LIMIT);

	g_assert_cmpuint(devices->len, ==, 1);
	for (i = 0; i < devices->len; i++) {
		const BarBtDevice *d = g_ptr_array_index(devices, i);

		g_assert_cmpuint(strlen(d->mac), >, 0);
	}
}

static void
test_nothing_at_all(void)
{
	g_autoptr(GPtrArray) devices = NULL;

	/* The adapter is off, or bluetoothctl returned nothing: an empty
	 * list, not a crash. */
	devices = parse(NULL, NULL, NULL, DEVICES_LIMIT);
	g_assert_cmpuint(devices->len, ==, 0);

	bar_bt_devices_parse(NULL, "Device AA:AA:AA:AA:AA:AA X\n",
	                     NULL, NULL, DEVICES_LIMIT);
}

static void
test_the_array_is_cleared_first(void)
{
	g_autoptr(GPtrArray) devices = NULL;

	/* Polled every few seconds into the same array: a device that has
	 * gone has to leave the list. */
	devices = parse("Device AA:AA:AA:AA:AA:AA Gone\n"
	                "Device BB:BB:BB:BB:BB:BB Staying\n",
	                NULL, NULL, DEVICES_LIMIT);
	g_assert_cmpuint(devices->len, ==, 2);

	bar_bt_devices_parse(devices, "Device BB:BB:BB:BB:BB:BB Staying\n",
	                     NULL, NULL, DEVICES_LIMIT);
	g_assert_cmpuint(devices->len, ==, 1);
	g_assert_null(by_mac(devices, "AA:AA:AA:AA:AA:AA"));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-bt/a-discovered-device-is-listed",
	                test_a_discovered_device_is_listed);
	g_test_add_func("/bar-bt/paired-and-connected-are-marked",
	                test_paired_and_connected_are_marked);
	g_test_add_func("/bar-bt/connected-implies-paired",
	                test_connected_implies_paired);
	g_test_add_func("/bar-bt/standing-decides-the-order",
	                test_standing_decides_the_order);
	g_test_add_func("/bar-bt/the-limit-drops-strangers-not-yours",
	                test_the_limit_drops_strangers_not_yours);
	g_test_add_func("/bar-bt/order-within-a-group-is-listing-order",
	                test_order_within_a_group_is_the_listing_order);
	g_test_add_func("/bar-bt/a-device-only-the-filters-know-is-kept",
	                test_a_device_only_the_filters_know_is_kept);
	g_test_add_func("/bar-bt/a-device-in-every-listing-appears-once",
	                test_a_device_in_every_listing_appears_once);
	g_test_add_func("/bar-bt/a-name-with-spaces-is-kept-whole",
	                test_a_name_with_spaces_is_kept_whole);
	g_test_add_func("/bar-bt/noise-is-ignored", test_noise_is_ignored);
	g_test_add_func("/bar-bt/a-device-with-no-address-is-ignored",
	                test_a_device_with_no_address_is_ignored);
	g_test_add_func("/bar-bt/nothing-at-all", test_nothing_at_all);
	g_test_add_func("/bar-bt/the-array-is-cleared-first",
	                test_the_array_is_cleared_first);

	return g_test_run();
}
