/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * bar-bt-devices.h -- turning bluetoothctl's listings into one list of
 * devices that says which are connected, which are paired, and which
 * are merely in the air right now.
 *
 * Split out from the plugin because it is pure text handling, and
 * because the bug it exists for was a question of WHICH listings get
 * asked for: the widget read `devices Paired' and `devices Connected'
 * and nothing else, so a device found by a scan could never appear in
 * it.  Turning scanning on did start a scan and then had nowhere to
 * put what it found.
 */

#ifndef BAR_BT_DEVICES_H
#define BAR_BT_DEVICES_H

#include <glib.h>

typedef struct {
	gchar    *mac;
	gchar    *name;
	gboolean  paired;
	gboolean  connected;
} BarBtDevice;

/**
 * bar_bt_device_free:
 * @data: a #BarBtDevice
 */
void bar_bt_device_free (gpointer data);

/**
 * bar_bt_devices_parse:
 * @out: (element-type BarBtDevice): filled in, cleared first
 * @all: stdout of `bluetoothctl devices' --- everything the adapter
 *   knows about, which after a scan includes what the scan found
 * @paired: stdout of `bluetoothctl devices Paired', or %NULL
 * @connected: stdout of `bluetoothctl devices Connected', or %NULL
 * @limit: keep at most this many devices
 *
 * Every listing prints one device per line as
 *
 *   Device AA:BB:CC:DD:EE:FF Some Name
 *
 * and the name is the rest of the line, so it may contain spaces and is
 * taken whole rather than tokenised.
 *
 * The three listings are one set of devices seen three ways, not three
 * sets: @all establishes who exists and @paired and @connected say what
 * is true of them.  A device in @paired or @connected that @all somehow
 * missed is still kept --- a device you own must not vanish from the
 * panel because two listings disagreed.
 *
 * Ordering is the part that matters when @limit bites: connected first,
 * then paired, then whatever the scan turned up.  In a room full of
 * other people's headphones the discovered devices are the ones that
 * fall off the end, never yours.
 */
void bar_bt_devices_parse (GPtrArray   *out,
                           const gchar *all,
                           const gchar *paired,
                           const gchar *connected,
                           guint        limit);

#endif /* BAR_BT_DEVICES_H */
