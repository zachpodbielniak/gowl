/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * bar-wifi-scan.h -- turning `nmcli device wifi list' into a list of
 * networks.
 *
 * Split out from the plugin because it is pure text handling and it got
 * two things wrong that only show up with real output: nmcli reports one
 * row per BSS, so a dual-band router or a mesh repeats an SSID several
 * times, and the truncation that claimed to keep "the strongest twenty"
 * kept whichever twenty came first.
 */

#ifndef BAR_WIFI_SCAN_H
#define BAR_WIFI_SCAN_H

#include <glib.h>

/**
 * bar_wifi_scan_parse:
 * @out: (element-type utf8): array the rows are appended to, cleared
 *   first; each entry is "ssid\tsignal\tsecurity\tactive"
 * @nmcli_output: the stdout of
 *   `nmcli -t -f ACTIVE,SSID,SIGNAL,SECURITY device wifi list'
 * @limit: keep at most this many networks, strongest first
 *
 * Parses terse nmcli output into one entry per NETWORK, strongest
 * signal first.
 *
 * Terse output escapes colons inside fields with a backslash, so the
 * split has to respect that rather than using g_strsplit().
 */
void bar_wifi_scan_parse (GPtrArray   *out,
                          const gchar *nmcli_output,
                          guint        limit);

#endif /* BAR_WIFI_SCAN_H */
