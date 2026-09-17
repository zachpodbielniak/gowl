/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "bar-bt-devices.h"

#include <string.h>

void
bar_bt_device_free(gpointer data)
{
	BarBtDevice *d = data;

	if (d == NULL)
		return;
	g_free(d->mac);
	g_free(d->name);
	g_free(d);
}

/* The device with this address, or NULL. */
static BarBtDevice *
find_mac(GPtrArray *devices, const gchar *mac)
{
	guint i;

	for (i = 0; i < devices->len; i++) {
		BarBtDevice *d = g_ptr_array_index(devices, i);

		if (g_ascii_strcasecmp(d->mac, mac) == 0)
			return d;
	}
	return NULL;
}

/*
 * Walk one listing.  @add says whether a device that is not in the list
 * yet should be added or only marked: the `all' listing brings devices
 * in, the other two describe the ones it brought.
 *
 * A device only the filtered listings know about is still added,
 * because losing one you own is worse than the disagreement that
 * caused it.
 */
static void
scan_listing(GPtrArray *out, const gchar *listing,
             gboolean paired, gboolean connected)
{
	g_auto(GStrv) lines = NULL;
	gint i;

	if (listing == NULL)
		return;

	lines = g_strsplit(listing, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		const gchar *mac, *space;
		g_autofree gchar *addr = NULL;
		BarBtDevice *d;

		if (!g_str_has_prefix(lines[i], "Device "))
			continue;
		mac = lines[i] + strlen("Device ");
		space = strchr(mac, ' ');
		if (space == NULL || space == mac)
			continue;
		addr = g_strndup(mac, (gsize)(space - mac));

		d = find_mac(out, addr);
		if (d == NULL) {
			d = g_new0(BarBtDevice, 1);
			d->mac = g_steal_pointer(&addr);
			d->name = g_strdup(space + 1);
			g_ptr_array_add(out, d);
		}

		/* Being connected to something you have not paired with is
		 * not a state bluetooth has, and a panel that showed one as
		 * unpaired would offer to pair it again. */
		if (paired || connected)
			d->paired = TRUE;
		if (connected)
			d->connected = TRUE;
	}
}

/* Connected, then paired, then discovered; stable within each group. */
static gint
by_standing(gconstpointer a, gconstpointer b)
{
	const BarBtDevice *x = *(const BarBtDevice * const *)a;
	const BarBtDevice *y = *(const BarBtDevice * const *)b;
	gint rx = x->connected ? 0 : (x->paired ? 1 : 2);
	gint ry = y->connected ? 0 : (y->paired ? 1 : 2);

	return rx - ry;
}

void
bar_bt_devices_parse(GPtrArray *out, const gchar *all, const gchar *paired,
                     const gchar *connected, guint limit)
{
	if (out == NULL)
		return;

	g_ptr_array_set_size(out, 0);

	scan_listing(out, all, FALSE, FALSE);
	scan_listing(out, paired, TRUE, FALSE);
	scan_listing(out, connected, FALSE, TRUE);

	/* Sorted before the truncation, so what is dropped is the tail of
	 * the discovered devices rather than whatever happened to be
	 * printed last. */
	g_ptr_array_sort(out, by_standing);

	if (limit > 0 && out->len > limit)
		g_ptr_array_set_size(out, limit);
}
