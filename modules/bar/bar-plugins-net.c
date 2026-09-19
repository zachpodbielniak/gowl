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

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-bar"

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>

#include "barkit/gowl-bar-json.h"

#include "bar-internal.h"
#include "bar-wifi-scan.h"
#include "bar-bt-devices.h"

/**
 * SECTION:bar-plugins-net
 * @title: Network bar plugins
 * @short_description: link state, the network panel and Tailscale
 *
 * The network panel is built from what the kernel already knows ---
 * the `/proc/net' files and `/sys/class/net' --- plus `nmcli' when it
 * is present.
 * That split matters: the readings still work on a machine with no
 * NetworkManager, and only the actions that genuinely need it are
 * hidden when it is missing, rather than the whole panel going blank.
 */

static BarSysinfo *
net_info(void)
{
	const BarEnv *env = bar_env();

	return (env != NULL) ? env->sysinfo : NULL;
}

/* A short sysfs string, trimmed, or NULL. */
static gchar *
read_str_file_trim(const gchar *path)
{
	g_autofree gchar *contents = NULL;

	if (!g_file_get_contents(path, &contents, NULL, NULL))
		return NULL;
	g_strstrip(contents);
	if (contents[0] == '\0')
		return NULL;
	return g_steal_pointer(&contents);
}

/* ----------------------------------------------------------------
 * Shared network state
 *
 * One struct per widget instance.  Everything in it is written by the
 * async poll on a worker and read by the sync poll, the panel builder
 * and clicks on the dispatch thread, and the host serialises NONE of
 * that: its async_inflight flag only stops a plugin's async poll
 * overlapping itself.  So every pointer in here is behind `lock'.  The
 * worker gathers into locals -- subprocesses and all -- and swaps them
 * in under the lock in one short critical section; the readers take
 * the same lock for the few microseconds a panel takes to build.  This
 * struct used to free and rewrite its strings on the worker while the
 * panel builder was reading them, which is the heap corruption the
 * plugin documentation warns every third-party widget about.
 * ---------------------------------------------------------------- */

typedef struct {
	GMutex   lock;

	gchar   *iface;
	gchar   *ipv4;
	gchar   *gateway;
	gchar   *ssid;
	gchar   *connection;    /* NetworkManager's name for the link */
	gboolean wireless;
	gboolean online;
	gint     signal_dbm;
	gdouble  quality;
	gdouble  ping_ms;       /* < 0 when unknown or timed out */
	gint     packet_loss;   /* 0, 100, or -1 when ping was not run */
	glong    rx_total, tx_total;
	glong    rx_rate, tx_rate;

	/* Nearby and known networks, as `SSID\tSIGNAL\tSECURITY\tACTIVE'
	   rows straight out of nmcli's terse output. */
	GPtrArray *scan;
	gboolean   have_nmcli;
	gboolean   have_ping;
	gchar     *dns_mode;    /* dhcp, cloudflare, google, quad9, custom */
} NetData;

static gpointer
net_create(GowlBarPlugin *plugin)
{
	NetData *nd;

	(void)plugin;
	nd = g_new0(NetData, 1);
	g_mutex_init(&nd->lock);
	nd->scan = g_ptr_array_new_with_free_func(g_free);
	nd->ping_ms = -1.0;
	nd->packet_loss = -1;
	nd->have_nmcli = bar_have_command("nmcli");
	nd->have_ping  = bar_have_command("ping");
	return nd;
}

static void
net_destroy(GowlBarPlugin *plugin, gpointer data)
{
	NetData *nd = data;

	(void)plugin;
	if (nd == NULL)
		return;
	g_free(nd->iface);
	g_free(nd->ipv4);
	g_free(nd->gateway);
	g_free(nd->ssid);
	g_free(nd->connection);
	g_free(nd->dns_mode);
	g_ptr_array_unref(nd->scan);
	g_mutex_clear(&nd->lock);
	g_free(nd);
}

static gint
net_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 5;
}

/* Measure the round trip to a well-known address.  One packet with a
   one-second deadline: the panel wants a number, not a latency study,
   and a longer wait would hold a worker thread open.  Returns the
   loss: 0, 100, or -1 when there is no ping to run. */
static gint
net_measure_ping(NetData *nd, const gchar *host, gdouble *ms_out)
{
	const gchar *argv[] = { "ping", "-c", "1", "-W", "1", NULL, NULL };
	g_autofree gchar *out = NULL;
	const gchar *p;

	*ms_out = -1.0;
	if (!nd->have_ping)
		return -1;

	argv[5] = (host != NULL && host[0] != '\0') ? host : "1.1.1.1";

	out = bar_run_argv(argv);
	if (out == NULL)
		return 100;

	p = strstr(out, "time=");
	if (p == NULL)
		return 100;
	*ms_out = g_ascii_strtod(p + 5, NULL);
	return 0;
}

/* Pull the visible networks out of nmcli.  The parsing lives in
   bar-wifi-scan.c so it can be tested against real output: that is
   where the duplicate-SSID and ranking bugs were.  Fills @out, which
   the caller swaps in under the lock. */
static void
net_scan_wifi(NetData *nd, GPtrArray *out)
{
	const gchar *argv[] = {
		"nmcli", "-t", "-f", "ACTIVE,SSID,SIGNAL,SECURITY",
		"device", "wifi", "list", NULL
	};
	g_autofree gchar *raw = NULL;

	if (!nd->have_nmcli) {
		g_ptr_array_set_size(out, 0);
		return;
	}

	raw = bar_run_argv(argv);
	bar_wifi_scan_parse(out, raw, 20);
}

/* NetworkManager's name for the active connection on @iface. */
static gchar *
net_connection_name(const gchar *iface)
{
	const gchar *argv[] = {
		"nmcli", "-t", "-f", "GENERAL.CONNECTION", "device", "show",
		NULL, NULL
	};
	g_autofree gchar *out = NULL;
	const gchar *colon;

	argv[6] = iface;
	out = bar_run_argv_line(argv);
	if (out == NULL)
		return NULL;
	colon = strchr(out, ':');
	return g_strdup((colon != NULL) ? colon + 1 : out);
}

/*
 * Which of the panel's DNS choices the connection is actually using.
 *
 * The panel used to light "DHCP" until a button was pressed and then
 * whatever was pressed, without ever asking -- so a connection set to
 * Cloudflare yesterday read as DHCP today.  Read from the connection
 * profile, which is where the panel writes it.
 */
static gchar *
net_read_dns_mode(const gchar *connection)
{
	const gchar *argv[] = {
		"nmcli", "-g", "ipv4.dns,ipv4.ignore-auto-dns", "connection",
		"show", NULL, NULL
	};
	g_autofree gchar *out = NULL;
	g_auto(GStrv) lines = NULL;
	const gchar *servers, *ignore_auto;

	if (connection == NULL || connection[0] == '\0')
		return NULL;
	argv[5] = connection;
	out = bar_run_argv(argv);
	if (out == NULL)
		return NULL;

	lines = g_strsplit(out, "\n", -1);
	servers     = (lines[0] != NULL) ? lines[0] : "";
	ignore_auto = (lines[0] != NULL && lines[1] != NULL) ? lines[1] : "";

	if (servers[0] == '\0' || g_strcmp0(ignore_auto, "yes") != 0)
		return g_strdup("dhcp");
	if (strstr(servers, "1.1.1.1") != NULL)
		return g_strdup("cloudflare");
	if (strstr(servers, "8.8.8.8") != NULL)
		return g_strdup("google");
	if (strstr(servers, "9.9.9.9") != NULL)
		return g_strdup("quad9");
	return g_strdup("custom");
}

/*
 * Everything the panel shows, gathered on the worker and swapped in.
 *
 * @with_ping is whether to measure latency this time: the `network'
 * widget's whole colour is the ping, so it always does; the lighter
 * widgets sharing the panel only ping while the panel is open, or
 * every one of them would be a packet every few seconds for a number
 * nobody is looking at.  The wifi scan and the DNS lookup are likewise
 * panel-only, and for the same reason the scan's own comment gives: a
 * background scan every interval disrupts the connection it measures.
 */
static void
net_gather(GowlBarPlugin *plugin, NetData *nd, gboolean with_ping)
{
	BarSysinfo *info = net_info();
	const gchar *configured;
	g_autofree gchar *iface = NULL;
	g_autofree gchar *ipv4 = NULL;
	g_autofree gchar *gateway = NULL;
	g_autofree gchar *ssid = NULL;
	g_autofree gchar *connection = NULL;
	g_autofree gchar *dns_mode = NULL;
	g_autoptr(GPtrArray) scan = NULL;
	gboolean wireless = FALSE, online, panel_open;
	gint signal_dbm = 0, packet_loss = -1;
	gdouble quality = 0.0, ping_ms = -1.0;
	glong rx_total = 0, tx_total = 0, rx_rate = 0, tx_rate = 0;

	if (info == NULL)
		return;

	panel_open = gowl_bar_plugin_get_setting_bool(plugin, "panel-open",
	                                              FALSE);

	configured = gowl_bar_plugin_get_setting(plugin, "param");
	if (configured != NULL && configured[0] != '\0')
		iface = g_strdup(configured);
	else
		iface = bar_sysinfo_default_route_iface(info);
	online = (iface != NULL);

	if (online) {
		wireless = bar_sysinfo_iface_is_wireless(info, iface);
		ipv4     = bar_sysinfo_ipv4(info, iface);
		gateway  = bar_sysinfo_default_gateway(info);
		bar_sysinfo_iface_bytes(info, iface, &rx_total, &tx_total);
		bar_sysinfo_net(info, iface, &rx_rate, &tx_rate);
	}

	if (wireless) {
		const gchar *wifi_iface = NULL;

		bar_sysinfo_wifi(info, &wifi_iface, &signal_dbm, &quality);
	}

	/* nmcli is only asked while it has something to answer that the
	   kernel cannot: the connection's name (which is the SSID on
	   wireless) and, while the panel is up, its DNS. */
	if (online && nd->have_nmcli && (wireless || panel_open)) {
		connection = net_connection_name(iface);
		if (wireless)
			ssid = g_strdup(connection);
		if (panel_open)
			dns_mode = net_read_dns_mode(connection);
	}

	if (with_ping || panel_open) {
		packet_loss = net_measure_ping(nd,
			gowl_bar_plugin_get_setting(plugin, "ping-host"),
			&ping_ms);
	}

	if (wireless && panel_open) {
		scan = g_ptr_array_new_with_free_func(g_free);
		net_scan_wifi(nd, scan);
	}

	g_mutex_lock(&nd->lock);
	g_free(nd->iface);      nd->iface      = g_steal_pointer(&iface);
	g_free(nd->ipv4);       nd->ipv4       = g_steal_pointer(&ipv4);
	g_free(nd->gateway);    nd->gateway    = g_steal_pointer(&gateway);
	g_free(nd->ssid);       nd->ssid       = g_steal_pointer(&ssid);
	g_free(nd->connection); nd->connection = g_steal_pointer(&connection);
	nd->wireless   = wireless;
	nd->online     = online;
	nd->signal_dbm = signal_dbm;
	nd->quality    = quality;
	nd->rx_total   = rx_total;
	nd->tx_total   = tx_total;
	nd->rx_rate    = rx_rate;
	nd->tx_rate    = tx_rate;
	if (with_ping || panel_open) {
		nd->ping_ms     = ping_ms;
		nd->packet_loss = packet_loss;
	}
	if (dns_mode != NULL) {
		g_free(nd->dns_mode);
		nd->dns_mode = g_steal_pointer(&dns_mode);
	}
	if (scan != NULL) {
		g_ptr_array_unref(nd->scan);
		nd->scan = g_steal_pointer(&scan);
	} else if (!panel_open && nd->scan->len > 0) {
		/* Not "empty the list" -- swap an empty one in, so a panel
		   that closed mid-build is not walking freed rows. */
		g_ptr_array_unref(nd->scan);
		nd->scan = g_ptr_array_new_with_free_func(g_free);
	}
	g_mutex_unlock(&nd->lock);
}

/* The `network' widget's label, from the state just gathered. */
static void
net_apply_label(GowlBarPlugin *plugin, NetData *nd)
{
	gchar buf[128];
	gchar rx_buf[32], tx_buf[32];
	gboolean online, wireless;
	gint packet_loss;
	g_autofree gchar *ssid = NULL;
	g_autofree gchar *ipv4 = NULL;
	g_autofree gchar *iface = NULL;
	glong rx_rate, tx_rate;

	g_mutex_lock(&nd->lock);
	online      = nd->online;
	wireless    = nd->wireless;
	packet_loss = nd->packet_loss;
	ssid        = g_strdup(nd->ssid);
	ipv4        = g_strdup(nd->ipv4);
	iface       = g_strdup(nd->iface);
	rx_rate     = nd->rx_rate;
	tx_rate     = nd->tx_rate;
	g_mutex_unlock(&nd->lock);

	if (!online) {
		gowl_bar_plugin_set_label(plugin, "offline");
		gowl_bar_plugin_set_icon(plugin, "\xef\x87\xab");
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_RED);
		gowl_bar_plugin_set_tooltip(plugin, "No default route");
		return;
	}

	if (gowl_bar_plugin_get_setting_bool(plugin, "show-rates", FALSE)) {
		bar_sysinfo_format_rate(rx_rate, rx_buf, sizeof(rx_buf));
		bar_sysinfo_format_rate(tx_rate, tx_buf, sizeof(tx_buf));
		g_snprintf(buf, sizeof(buf),
		           "\xe2\x86\x93%s \xe2\x86\x91%s", rx_buf, tx_buf);
	} else if (wireless && ssid != NULL) {
		g_snprintf(buf, sizeof(buf), "%s", ssid);
	} else if (ipv4 != NULL) {
		g_snprintf(buf, sizeof(buf), "%s", ipv4);
	} else {
		g_snprintf(buf, sizeof(buf), "%s", iface);
	}

	gowl_bar_plugin_set_label(plugin, buf);
	gowl_bar_plugin_set_icon(plugin,
		wireless ? "\xef\x87\xab" : "\xef\x9b\xbf");
	/* Peach means "the ping failed", so without a ping there is
	   nothing to be peach about: the widget used to sit peach for
	   ever on a host with no ping binary. */
	if (packet_loss > 0)
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_PEACH);
	else
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color"),
			GOWL_BAR_COLOR_TEXT);
	{
		g_autofree gchar *tip = NULL;

		tip = g_strdup_printf("%s%s%s", iface,
			(ipv4 != NULL) ? "  " : "",
			(ipv4 != NULL) ? ipv4 : "");
		gowl_bar_plugin_set_tooltip(plugin, tip);
	}
}

static void
net_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	NetData *nd = data;

	net_gather(plugin, nd, TRUE);
	net_apply_label(plugin, nd);
}

/* The widgets that only borrow the panel gather without pinging unless
   the panel is open, and set their own labels in their sync polls. */
static void
net_light_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	net_gather(plugin, data, FALSE);
}

/* Runs on the host's worker thread; see net_panel_opened(). */
static void
net_open_work(GowlBarPlugin *plugin, gpointer user_data)
{
	NetData *nd = user_data;

	if (nd == NULL)
		return;
	net_gather(plugin, nd, TRUE);
	/* A panel is built once when it opens, so a scan finishing later
	   has to ask for it to be rebuilt or the list never appears. */
	gowl_bar_plugin_request_panel_refresh(plugin);
}

static void
net_panel_opened(GowlBarPlugin *plugin, gpointer data)
{
	NetData *nd = data;

	/* The async poll reads this to decide whether to scan; setting it
	   here rather than scanning inline keeps the scan off the
	   compositor thread. */
	gowl_bar_plugin_set_setting(plugin, "panel-open", "true");

	/*
	 * Gather NOW, off the compositor thread, rather than waiting for
	 * the next poll: the lighter widgets poll every thirty seconds,
	 * and a panel that sits on "scanning" for half a minute reads as
	 * broken rather than slow.  queue_work runs on the host's worker,
	 * so nmcli and ping still never block the compositor.
	 */
	gowl_bar_plugin_queue_work(plugin, net_open_work, nd, NULL);
	gowl_bar_plugin_request_redraw(plugin);
}

/*
 * Stop scanning once the panel is gone.
 *
 * Without this the flag is one-way: the first time a panel opened, the
 * async poll kept rescanning every interval for the rest of the session
 * -- which is exactly what the comment at the scan call warns about,
 * since a rescan disrupts the connection it is measuring.
 */
static void
net_panel_closed(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	gowl_bar_plugin_set_setting(plugin, "panel-open", "false");
}

static GowlBarPanel *
net_panel(GowlBarPlugin *plugin, gpointer data)
{
	NetData *nd = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	gchar buf[128], buf2[128];
	guint i;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 440);

	/* Held for the whole build: every string below belongs to the
	   struct, and the worker may replace them at any moment.  The
	   build is a few hundred microseconds; the worker waits. */
	g_mutex_lock(&nd->lock);

	/* Hero: what you are connected through, and its whimsical
	   subtitle --- the panel is a utility, and a little character in
	   the one line that never carries data costs nothing. */
	if (nd->online && nd->wireless) {
		gowl_bar_panel_add_hero(panel, "\xef\x87\xab",
			(nd->ssid != NULL) ? nd->ssid : "Wi-Fi",
			"Routing crumbs");
	} else if (nd->online) {
		gowl_bar_panel_add_hero(panel, "\xef\x9b\xbf", "Ethernet",
		                        "Wiring bits");
	} else {
		gowl_bar_panel_add_hero(panel, "\xef\x87\xab", "Offline",
		                        "Nothing routing");
	}

	if (nd->packet_loss < 0) {
		g_strlcpy(buf, nd->have_ping ? "Measuring..." : "No ping",
		          sizeof(buf));
		g_strlcpy(buf2, "--", sizeof(buf2));
	} else {
		if (nd->ping_ms >= 0.0)
			g_snprintf(buf, sizeof(buf), "%.1f ms", nd->ping_ms);
		else
			g_strlcpy(buf, "Timeout", sizeof(buf));
		g_snprintf(buf2, sizeof(buf2), "%d%%", nd->packet_loss);
	}
	item = gowl_bar_panel_add_field_pair(panel, "Ping", buf,
	                                     "Packet Loss", buf2);
	gowl_bar_panel_item_set_value_color(item,
		(nd->packet_loss > 0) ? GOWL_BAR_COLOR_RED
		                      : GOWL_BAR_COLOR_SUBTEXT);

	{
		gchar rx_buf[32], tx_buf[32];

		bar_sysinfo_format_rate(nd->rx_rate, rx_buf, sizeof(rx_buf));
		bar_sysinfo_format_rate(nd->tx_rate, tx_buf, sizeof(tx_buf));
		g_snprintf(buf, sizeof(buf), "%s/s", rx_buf);
		g_snprintf(buf2, sizeof(buf2), "%s/s", tx_buf);
		gowl_bar_panel_add_field_pair(panel, "Receiving", buf,
		                              "Sending", buf2);

		bar_sysinfo_format_bytes(nd->rx_total, buf, sizeof(buf));
		bar_sysinfo_format_bytes(nd->tx_total, buf2, sizeof(buf2));
		gowl_bar_panel_add_field_pair(panel, "Downloaded", buf,
		                              "Uploaded", buf2);
	}

	gowl_bar_panel_add_field_pair(panel, "IP Address",
		(nd->ipv4 != NULL) ? nd->ipv4 : "--", "Gateway",
		(nd->gateway != NULL) ? nd->gateway : "--");
	if (nd->iface != NULL) {
		gowl_bar_panel_add_field_pair(panel, "Interface", nd->iface,
			"Connection",
			(nd->connection != NULL) ? nd->connection : "--");
	}

	if (nd->wireless && nd->signal_dbm != 0) {
		g_snprintf(buf, sizeof(buf), "%d dBm", nd->signal_dbm);
		item = gowl_bar_panel_add_progress(panel, "Signal",
		                                   nd->quality);
		gowl_bar_panel_item_set_value(item, buf);
		gowl_bar_panel_item_set_color(item,
			(nd->quality > 0.6) ? GOWL_BAR_COLOR_GREEN
			: (nd->quality > 0.3) ? GOWL_BAR_COLOR_YELLOW
			: GOWL_BAR_COLOR_RED);
	}

	{
		const gdouble *samples;
		guint n;
		BarSysinfo *info = net_info();

		if (info != NULL) {
			samples = bar_sysinfo_history(info, "net", &n);
			if (samples != NULL) {
				item = gowl_bar_panel_add_graph(panel,
					"Throughput", samples, n);
				gowl_bar_panel_item_set_color(item,
					GOWL_BAR_COLOR_SAPPHIRE);
			}
		}
	}

	if (nd->have_nmcli && nd->online) {
		gowl_bar_panel_add_separator(panel);
		gowl_bar_panel_add_section(panel, "DNS provider");
		item = gowl_bar_panel_add_buttons(panel, "dns");
		gowl_bar_panel_add_button(item, "DHCP",
			g_strcmp0(nd->dns_mode, "dhcp") == 0);
		gowl_bar_panel_add_button(item, "Cloudflare",
			g_strcmp0(nd->dns_mode, "cloudflare") == 0);
		gowl_bar_panel_add_button(item, "Google",
			g_strcmp0(nd->dns_mode, "google") == 0);
		gowl_bar_panel_add_button(item, "Quad9",
			g_strcmp0(nd->dns_mode, "quad9") == 0);
		if (nd->dns_mode == NULL)
			gowl_bar_panel_add_label(panel,
				"reading the connection's DNS...");
		else if (g_strcmp0(nd->dns_mode, "custom") == 0)
			gowl_bar_panel_add_label(panel,
				"custom servers are set on this connection");
	} else if (!nd->have_nmcli) {
		gowl_bar_panel_add_separator(panel);
		gowl_bar_panel_add_label(panel,
			"nmcli is not installed, so DNS and Wi-Fi cannot be "
			"changed from here");
	}

	if (nd->wireless && nd->have_nmcli) {
		gowl_bar_panel_add_separator(panel);
		gowl_bar_panel_add_section(panel, "Networks");

		if (nd->scan->len == 0) {
			gowl_bar_panel_add_label(panel, "Scanning...");
		}
		for (i = 0; i < nd->scan->len; i++) {
			g_auto(GStrv) fields = NULL;
			const gchar *row;
			gint signal_pct;

			row = g_ptr_array_index(nd->scan, i);
			fields = g_strsplit(row, "\t", 4);
			if (g_strv_length(fields) < 4)
				continue;

			signal_pct = (gint)g_ascii_strtoll(fields[1], NULL, 10);
			{
				g_autofree gchar *row_id = NULL;

				/* The action callback is given an item id
				   and nothing else, so the SSID travels in
				   the id.  A property would be invisible to
				   it. */
				row_id = g_strdup_printf("connect:%s",
				                         fields[0]);
				item = gowl_bar_panel_add_row(panel, row_id,
					(signal_pct > 60) ? "\xef\x87\xab"
					                  : "\xef\x87\xac",
					fields[0],
					(fields[2][0] != '\0') ? fields[2]
					                       : "Open");
			}
			g_snprintf(buf, sizeof(buf), "%d%%", signal_pct);
			gowl_bar_panel_item_set_value(item, buf);
			if (g_strcmp0(fields[3], "yes") == 0) {
				gowl_bar_panel_item_set_active(item, TRUE);
				gowl_bar_panel_item_set_subtitle(item,
				                                 "Connected");
			}
			if (fields[2][0] != '\0')
				gowl_bar_panel_item_set_badge(item,
					"\xef\x80\xa3");   /* U+F023 lock */
		}
	}

	g_mutex_unlock(&nd->lock);

	gowl_bar_panel_add_separator(panel);
	item = gowl_bar_panel_add_buttons(panel, "tool");
	gowl_bar_panel_add_button(item, "Rescan", FALSE);
	gowl_bar_panel_add_button(item, "Settings", FALSE);

	return panel;
}

static void
net_panel_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
                 gint index, gdouble value, guint button)
{
	NetData *nd = data;

	(void)value;
	(void)button;

	if (g_strcmp0(item_id, "dns") == 0) {
		const gchar *servers[4] = {
			"", "1.1.1.1,1.0.0.1", "8.8.8.8,8.8.4.4",
			"9.9.9.9,149.112.112.112"
		};
		const gchar *modes[4] = { "dhcp", "cloudflare", "google",
		                          "quad9" };
		g_autofree gchar *iface = NULL;
		g_autofree gchar *connection = NULL;
		g_autofree gchar *qconn = NULL;
		g_autofree gchar *qiface = NULL;
		g_autofree gchar *line = NULL;

		if (index < 0 || index > 3 || !nd->have_nmcli)
			return;

		g_mutex_lock(&nd->lock);
		iface      = g_strdup(nd->iface);
		connection = g_strdup(nd->connection);
		g_free(nd->dns_mode);
		nd->dns_mode = g_strdup(modes[index]);
		g_mutex_unlock(&nd->lock);

		if (iface == NULL || connection == NULL)
			return;

		/*
		 * On the CONNECTION PROFILE, then reapplied to the device.
		 * `nmcli device modify' changes the live device only, so the
		 * choice was gone the next time the link came up -- a DNS
		 * provider that silently reverted on every reconnect.
		 */
		qconn  = g_shell_quote(connection);
		qiface = g_shell_quote(iface);
		if (index == 0) {
			line = g_strdup_printf(
				"nmcli connection modify %s ipv4.dns '' "
				"ipv4.ignore-auto-dns no && "
				"nmcli device reapply %s", qconn, qiface);
		} else {
			line = g_strdup_printf(
				"nmcli connection modify %s ipv4.dns %s "
				"ipv4.ignore-auto-dns yes && "
				"nmcli device reapply %s",
				qconn, servers[index], qiface);
		}
		gowl_bar_plugin_spawn(plugin, line);
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW,
			"DNS provider changed", modes[index]);
		gowl_bar_plugin_request_panel_refresh(plugin);
		return;
	}

	if (item_id != NULL && g_str_has_prefix(item_id, "connect:")) {
		const gchar *ssid = item_id + strlen("connect:");
		g_autofree gchar *quoted = NULL;
		g_autofree gchar *line = NULL;

		if (!nd->have_nmcli || ssid[0] == '\0')
			return;
		/* The SSID is user-visible text from a scan, so it goes
		   through shell quoting rather than into a format string:
		   a network called `; rm -rf ~' is a thing an attacker can
		   broadcast. */
		quoted = g_shell_quote(ssid);
		line = g_strdup_printf("nmcli device wifi connect %s", quoted);
		gowl_bar_plugin_spawn(plugin, line);
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
			"Connecting", ssid);
		return;
	}

	if (g_strcmp0(item_id, "tool") == 0) {
		if (index == 0) {
			if (nd->have_nmcli)
				gowl_bar_plugin_spawn(plugin,
					"nmcli device wifi rescan");
			/* And look again once the scan has had a moment:
			   the list is read on the worker, not here. */
			gowl_bar_plugin_queue_work(plugin, net_open_work, nd,
			                           NULL);
		} else if (index == 1) {
			const gchar *cmd;

			cmd = gowl_bar_plugin_get_setting(plugin,
			                                  "settings-command");
			if (cmd == NULL)
				cmd = "nm-connection-editor";
			gowl_bar_plugin_spawn(plugin, cmd);
		}
		return;
	}
}

static const GowlBarPluginVTable network_vtable = {
	sizeof(GowlBarPluginVTable),
	net_create, net_destroy,
	NULL, NULL, NULL,
	net_interval, NULL, net_poll_async,
	NULL, NULL,
	NULL, NULL,
	net_panel, net_panel_action,
	net_panel_opened, net_panel_closed,
	NULL,
	NULL
};

/* ----------------------------------------------------------------
 * net -- bare throughput
 * ---------------------------------------------------------------- */

static gint
rate_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 2;
}

static void
rate_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = net_info();
	gchar rx_buf[32], tx_buf[32], buf[80];
	glong rx, tx;

	(void)data;

	if (info == NULL)
		return;
	bar_sysinfo_net(info, gowl_bar_plugin_get_setting(plugin, "param"),
	                &rx, &tx);
	bar_sysinfo_format_rate(rx, rx_buf, sizeof(rx_buf));
	bar_sysinfo_format_rate(tx, tx_buf, sizeof(tx_buf));
	g_snprintf(buf, sizeof(buf), "\xe2\x86\x93%s \xe2\x86\x91%s",
	           rx_buf, tx_buf);
	gowl_bar_plugin_set_label(plugin, buf);
	bar_plugin_apply_color(plugin,
		gowl_bar_plugin_get_setting(plugin, "color"),
		GOWL_BAR_COLOR_TEXT);
}

/*
 * The three light widgets share the network panel, so they share its
 * data too -- through net_light_poll_async, which gathers without
 * pinging.  They used to serve net_panel with a NetData nothing ever
 * filled: an `ip' widget's dropdown said "Offline", "Timeout" and
 * "100%" on a machine that was fine.
 */
static const GowlBarPluginVTable rate_vtable = {
	sizeof(GowlBarPluginVTable),
	net_create, net_destroy,
	NULL, NULL, NULL,
	rate_interval, rate_poll, net_light_poll_async,
	NULL, NULL,
	NULL, NULL,
	net_panel, net_panel_action,
	net_panel_opened, net_panel_closed,
	NULL,
	NULL
};

/* ----------------------------------------------------------------
 * ip and wifi
 * ---------------------------------------------------------------- */

static gint
ip_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 30;
}

static void
ip_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = net_info();
	const gchar *configured;
	g_autofree gchar *iface = NULL;
	g_autofree gchar *addr = NULL;

	(void)data;

	if (info == NULL)
		return;

	/* The default route's interface, not the first one with an
	   address: on a host with podman, libvirt or a tailnet that first
	   one is a bridge or a tunnel, and the widget showed 10.88.0.1. */
	configured = gowl_bar_plugin_get_setting(plugin, "param");
	if (configured != NULL && configured[0] != '\0')
		iface = g_strdup(configured);
	else
		iface = bar_sysinfo_default_route_iface(info);

	addr = bar_sysinfo_ipv4(info, iface);
	gowl_bar_plugin_set_label(plugin,
	                          (addr != NULL) ? addr : "no address");
	gowl_bar_plugin_set_tooltip(plugin, iface);
	if (addr != NULL)
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color"),
			GOWL_BAR_COLOR_TEXT);
	else
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_MUTED);
}

static const GowlBarPluginVTable ip_vtable = {
	sizeof(GowlBarPluginVTable),
	net_create, net_destroy,
	NULL, NULL, NULL,
	ip_interval, ip_poll, net_light_poll_async,
	NULL, NULL,
	NULL, NULL,
	net_panel, net_panel_action,
	net_panel_opened, net_panel_closed,
	NULL,
	NULL
};

static void
wifi_poll(GowlBarPlugin *plugin, gpointer data)
{
	BarSysinfo *info = net_info();
	const gchar *iface = NULL;
	gchar buf[64];
	gint dbm;
	gdouble quality;

	(void)data;

	if (info == NULL)
		return;
	if (!bar_sysinfo_wifi(info, &iface, &dbm, &quality)) {
		gowl_bar_plugin_set_label(plugin, NULL);
		return;
	}
	g_snprintf(buf, sizeof(buf), "WiFi %ddBm", dbm);
	gowl_bar_plugin_set_label(plugin, buf);
	gowl_bar_plugin_set_tooltip(plugin, iface);
	if (quality > 0.6)
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color"),
			GOWL_BAR_COLOR_TEXT);
	else
		gowl_bar_plugin_set_color(plugin,
			(quality > 0.3) ? GOWL_BAR_COLOR_YELLOW
			                : GOWL_BAR_COLOR_RED);
}

static const GowlBarPluginVTable wifi_vtable = {
	sizeof(GowlBarPluginVTable),
	net_create, net_destroy,
	NULL, NULL, NULL,
	ip_interval, wifi_poll, net_light_poll_async,
	NULL, NULL,
	NULL, NULL,
	net_panel, net_panel_action,
	net_panel_opened, net_panel_closed,
	NULL,
	NULL
};

/* ----------------------------------------------------------------
 * vpn
 * ---------------------------------------------------------------- */

/*
 * Whether @name is a tunnel: a WireGuard device by its DEVTYPE, or a
 * tun/tap device by the flags file only those have.  The kernel is the
 * authority, whatever created it -- the widget used to test for three
 * hard-coded names (tun0, wg0, proton0) and missed wg-home, tun1 and
 * every OpenVPN client that names its own device.
 */
static gboolean
vpn_iface_is_tunnel(const gchar *name)
{
	g_autofree gchar *uevent_path = NULL;
	g_autofree gchar *tun_path = NULL;
	g_autofree gchar *uevent = NULL;

	tun_path = g_build_filename("/sys/class/net", name, "tun_flags", NULL);
	if (g_file_test(tun_path, G_FILE_TEST_EXISTS))
		return TRUE;

	uevent_path = g_build_filename("/sys/class/net", name, "uevent", NULL);
	if (g_file_get_contents(uevent_path, &uevent, NULL, NULL) &&
	    strstr(uevent, "DEVTYPE=wireguard") != NULL)
		return TRUE;

	return g_str_has_prefix(name, "ppp") || g_str_has_prefix(name, "ipsec");
}

static void
vpn_poll(GowlBarPlugin *plugin, gpointer data)
{
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GString) names = g_string_new(NULL);
	g_auto(GStrv) ignore = NULL;
	const gchar *entry;
	const gchar *ignore_spec;
	gint n = 0;

	(void)data;

	/* Tailscale is a tunnel too, and has a widget of its own; a
	   "VPN" light that is on whenever the tailnet is would say
	   nothing.  The list is a setting because somebody's VPN IS
	   their tailnet. */
	ignore_spec = gowl_bar_plugin_get_setting(plugin, "ignore");
	ignore = g_strsplit_set((ignore_spec != NULL) ? ignore_spec
	                                              : "tailscale0", " ,", -1);

	dir = g_dir_open("/sys/class/net", 0, NULL);
	while (dir != NULL && (entry = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *oper_path = NULL;
		g_autofree gchar *oper = NULL;
		gint i;
		gboolean skip = FALSE;

		for (i = 0; ignore[i] != NULL; i++)
			if (strcmp(ignore[i], entry) == 0)
				skip = TRUE;
		if (skip || !vpn_iface_is_tunnel(entry))
			continue;

		/* A tunnel that exists but is down is a client that is
		   still connecting or has just failed: not "VPN on". */
		oper_path = g_build_filename("/sys/class/net", entry,
		                             "operstate", NULL);
		oper = read_str_file_trim(oper_path);
		if (oper != NULL && strcmp(oper, "down") == 0)
			continue;

		if (names->len > 0)
			g_string_append(names, ", ");
		g_string_append(names, entry);
		n++;
	}

	if (n == 0) {
		gowl_bar_plugin_set_label(plugin, NULL);
		gowl_bar_plugin_set_icon(plugin, NULL);
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_MUTED);
		gowl_bar_plugin_set_tooltip(plugin, "No VPN tunnel is up");
		return;
	}

	{
		const gchar *label = gowl_bar_plugin_get_setting(plugin, "label");

		gowl_bar_plugin_set_label(plugin,
			(label != NULL && label[0] != '\0') ? label : "VPN");
	}
	gowl_bar_plugin_set_icon(plugin, "\xef\x82\xa3");
	bar_plugin_apply_color(plugin,
		gowl_bar_plugin_get_setting(plugin, "color"),
		GOWL_BAR_COLOR_GREEN);
	gowl_bar_plugin_set_tooltip(plugin, names->str);
}

static const GowlBarPluginVTable vpn_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL, NULL, NULL, NULL,
	ip_interval, vpn_poll, NULL,
	NULL, NULL, NULL, NULL,
	NULL, NULL,
	NULL, NULL,
	NULL,
	NULL
};

/* ----------------------------------------------------------------
 * tailscale
 * ---------------------------------------------------------------- */

typedef struct {
	/* Guards every pointer and array below: the async poll rebuilds
	   them on a worker while the panel walks them on the dispatch
	   thread, and the host serialises none of that. */
	GMutex   lock;
	gboolean installed;
	/* The binary is there but `tailscale status' gets no answer:
	   tailscaled is not running.  Distinct from never-joined, which
	   it used to be shown as -- a yellow "set up" on a host that had
	   joined years ago and merely had the daemon stopped. */
	gboolean daemon_down;
	gboolean active;
	gboolean needs_login;
	/* Whether this host has ever joined a tailnet.  Installed is not
	   the same question --- Immutablue ships Tailscale on every host,
	   so a widget keyed on the binary being present would sit grey
	   and permanent on machines that do not use it. */
	gboolean joined;
	/* The sign-in URL tailscale publishes while a join is waiting for
	   the browser.  Empty except during that window. */
	gchar   *auth_url;
	gboolean busy;
	gchar   *self_name;
	gchar   *self_ip;
	gchar   *exit_node;
	gchar   *status_text;
	gchar   *last_error;
	GPtrArray *peers;      /* `name\tip\tos\tonline\texit' rows */
	GPtrArray *exit_nodes; /* `name\tip' rows */
} TailscaleData;

static gpointer
ts_create(GowlBarPlugin *plugin)
{
	TailscaleData *td;

	(void)plugin;
	td = g_new0(TailscaleData, 1);
	g_mutex_init(&td->lock);
	td->peers      = g_ptr_array_new_with_free_func(g_free);
	td->exit_nodes = g_ptr_array_new_with_free_func(g_free);
	return td;
}

static void
ts_destroy(GowlBarPlugin *plugin, gpointer data)
{
	TailscaleData *td = data;

	(void)plugin;
	if (td == NULL)
		return;
	g_free(td->self_name);
	g_free(td->self_ip);
	g_free(td->auth_url);
	g_free(td->exit_node);
	g_free(td->status_text);
	g_free(td->last_error);
	g_ptr_array_unref(td->peers);
	g_ptr_array_unref(td->exit_nodes);
	g_mutex_clear(&td->lock);
	g_free(td);
}

static gint
ts_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)data;
	return gowl_bar_plugin_get_setting_int(plugin, "refresh", 30);
}

/* The peer walk.  Each callback gets one peer object, brace-matched by
   the kit's reader, so a field missing from a peer cannot be answered
   from the next one. */
typedef struct {
	GPtrArray *peers;
	GPtrArray *exit_nodes;
	gchar     *exit_node;
} TsPeers;

static gboolean
ts_collect_peer(const gchar *obj, guint index, gpointer user_data)
{
	TsPeers *td = user_data;
	g_autofree gchar *name = NULL;
	g_autofree gchar *host = NULL;
	g_autofree gchar *os = NULL;
	g_autofree gchar *ip = NULL;
	g_auto(GStrv) ips = NULL;
	gboolean online, exit_option;

	(void)index;

	name = gowl_bar_json_string(obj, "DNSName");
	host = gowl_bar_json_string(obj, "HostName");
	os   = gowl_bar_json_string(obj, "OS");
	online      = gowl_bar_json_bool(obj, "Online", FALSE);
	exit_option = gowl_bar_json_bool(obj, "ExitNodeOption", FALSE);

	/* DNSName is fully qualified and can be empty --- a tailnet
	   service such as a funnel ingress has only a HostName.  Prefer
	   the host name when there is one, since that is what the admin
	   console shows. */
	if (name != NULL) {
		gchar *dot = strchr(name, '.');

		if (dot != NULL)
			*dot = '\0';
	}
	if (host != NULL && host[0] != '\0') {
		g_free(name);
		name = g_strdup(host);
	}
	if (name == NULL || name[0] == '\0')
		return TRUE;

	/* Prefer the 100.x address: a machine list showing fd7a:... for
	   some peers and 100.x for others is not one anybody can use. */
	ips = gowl_bar_json_string_array(obj, "TailscaleIPs", NULL);
	if (ips != NULL) {
		gint i;

		for (i = 0; ips[i] != NULL; i++) {
			if (g_str_has_prefix(ips[i], "100.")) {
				ip = g_strdup(ips[i]);
				break;
			}
			if (ip == NULL)
				ip = g_strdup(ips[i]);
		}
	}

	g_ptr_array_add(td->peers,
		g_strdup_printf("%s\t%s\t%s\t%d", name,
		                (ip != NULL) ? ip : "",
		                (os != NULL) ? os : "", online ? 1 : 0));
	if (exit_option) {
		g_ptr_array_add(td->exit_nodes,
			g_strdup_printf("%s\t%s", name,
			                (ip != NULL) ? ip : ""));
	}

	/* ExitNodeStatus is an object when set and null when not, so the
	   peer's own flag is the reliable way to know which node traffic
	   is leaving through. */
	if (gowl_bar_json_bool(obj, "ExitNode", FALSE)) {
		g_free(td->exit_node);
		td->exit_node = g_strdup(name);
	}

	return (td->peers->len < 60);
}

/* Fresh arrays, filled here and swapped into the plugin's struct
   under its lock by the caller. */
static void
ts_parse_peers(TsPeers *out, const gchar *json)
{
	out->peers      = g_ptr_array_new_with_free_func(g_free);
	out->exit_nodes = g_ptr_array_new_with_free_func(g_free);
	out->exit_node  = NULL;
	gowl_bar_json_foreach_object(json, "Peer", ts_collect_peer, out);
}

/*
 * Whether the widget belongs in this bar at all.
 *
 * `show' is `auto' (the default), `always' or `never'.  Under `auto'
 * the widget appears wherever Tailscale is installed --- including on
 * a host that has never joined a tailnet, where it carries the join
 * flow.  On a Tailscale-native image that is the case you most want a
 * prompt for: a machine that is one click from being on the tailnet
 * and simply has not been told to.
 *
 * Membership is not a visibility question, then; it is a presentation
 * one.  Never joined reads as ready-to-set-up, joined but down reads
 * as off, joined and up reads as on -- see ts_apply_state().
 *
 * The plugin keeps polling while invisible, so installing Tailscale
 * makes the widget appear on the next poll rather than at the next
 * login.
 */
static void
ts_apply_visibility(GowlBarPlugin *plugin, TailscaleData *td)
{
	const gchar *show;
	gboolean visible;

	show = gowl_bar_plugin_get_setting(plugin, "show");
	if (show == NULL) {
		/* `tailscale:always' reads better than a separate setting
		   for a three-value switch, so the spec's parameter is
		   accepted as the mode too. */
		show = gowl_bar_plugin_get_setting(plugin, "param");
	}
	if (g_strcmp0(show, "always") == 0)
		visible = TRUE;
	else if (g_strcmp0(show, "never") == 0)
		visible = FALSE;
	else
		visible = td->installed;

	gowl_bar_plugin_set_visible(plugin, visible);

	if (!visible) {
		gowl_bar_plugin_set_label(plugin, NULL);
		gowl_bar_plugin_set_icon(plugin, NULL);
	}
}

/* Apply the visibility rule as soon as the setting arrives, rather
   than waiting for a poll: `show: never' should take effect at once,
   and under `auto' this keeps the widget out of the bar until a poll
   confirms membership instead of showing it and taking it away. */
/* The bar's icon, label and colour for the state we are in.  Three
   states worth telling apart: on, off, and never set up. */
static void ts_apply_state_locked(GowlBarPlugin *plugin, TailscaleData *td);

static void
ts_apply_state(GowlBarPlugin *plugin, TailscaleData *td)
{
	if (!gowl_bar_plugin_get_visible(plugin))
		return;
	/* The strings read below are the worker's to replace; a refresh
	   queued from the panel can run beside the host's own poll. */
	g_mutex_lock(&td->lock);
	ts_apply_state_locked(plugin, td);
	g_mutex_unlock(&td->lock);
}

static void
ts_apply_state_locked(GowlBarPlugin *plugin, TailscaleData *td)
{
	gboolean labels;

	labels = gowl_bar_plugin_get_setting_bool(plugin, "labels", FALSE);
	gowl_bar_plugin_set_icon(plugin, "\xef\x95\x82");

	if (!td->installed) {
		gowl_bar_plugin_set_label(plugin, labels ? "n/a" : NULL);
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_OVERLAY);
		gowl_bar_plugin_set_tooltip(plugin, "Tailscale is not installed");
		return;
	}

	if (td->daemon_down) {
		gowl_bar_plugin_set_label(plugin, labels ? "down" : NULL);
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_MUTED);
		gowl_bar_plugin_set_tooltip(plugin,
			"Tailscale: tailscaled is not running");
		return;
	}

	if (!td->joined) {
		/* Not an error and not "off": this host has never been
		   told to join, and the panel behind the icon is where it
		   gets told.  Yellow rather than grey so it reads as
		   something to do rather than something broken. */
		gowl_bar_plugin_set_label(plugin, labels ? "set up" : NULL);
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_YELLOW);
		gowl_bar_plugin_set_tooltip(plugin,
			(td->auth_url != NULL) ? "Tailscale: waiting for sign-in"
			                       : "Tailscale: not set up yet");
		return;
	}

	if (td->active) {
		g_autofree gchar *tip = NULL;

		gowl_bar_plugin_set_label(plugin,
			labels ? ((td->self_name != NULL) ? td->self_name
			                                  : "tailscale")
			       : NULL);
		bar_plugin_apply_color(plugin,
			gowl_bar_plugin_get_setting(plugin, "color"),
			GOWL_BAR_COLOR_TEAL);
		tip = g_strdup_printf("Tailscale: %s%s%s",
			(td->self_name != NULL) ? td->self_name : "up",
			(td->self_ip != NULL) ? "  " : "",
			(td->self_ip != NULL) ? td->self_ip : "");
		gowl_bar_plugin_set_tooltip(plugin, tip);
		return;
	}

	gowl_bar_plugin_set_label(plugin, labels ? "off" : NULL);
	gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_MUTED);
	gowl_bar_plugin_set_tooltip(plugin, "Tailscale is down");
}

static void
ts_configure(GowlBarPlugin *plugin, gpointer data, GHashTable *settings)
{
	(void)settings;
	ts_apply_visibility(plugin, data);
}

static void
ts_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	TailscaleData *td = data;
	const gchar *argv[] = { "tailscale", "status", "--json", NULL };
	g_autofree gchar *json = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *auth_url = NULL;
	g_autofree gchar *self_name = NULL;
	g_autofree gchar *self_ip = NULL;
	TsPeers peers;
	gboolean active, needs_login, joined;

	td->installed = bar_have_command("tailscale");
	if (!td->installed) {
		td->joined = FALSE;
		td->daemon_down = FALSE;
		td->busy = FALSE;
		ts_apply_visibility(plugin, td);
		ts_apply_state(plugin, td);
		return;
	}

	json = bar_run_argv(argv);
	if (json == NULL) {
		/* The daemon is not answering.  Whether that is worth a
		   widget depends on whether this host uses Tailscale at
		   all, which the last successful poll already told us. */
		td->active = FALSE;
		td->daemon_down = TRUE;
		td->busy = FALSE;
		ts_apply_visibility(plugin, td);
		ts_apply_state(plugin, td);
		return;
	}
	td->daemon_down = FALSE;

	state = gowl_bar_json_string(json, "BackendState");
	active      = (g_strcmp0(state, "Running") == 0);
	needs_login = (g_strcmp0(state, "NeedsLogin") == 0);

	/* HaveNodeKey is the honest "is this a tailnet member" flag: it
	   stays true across `tailscale down' and goes false only on a
	   host that has never joined or has been logged out. */
	joined = gowl_bar_json_bool(json, "HaveNodeKey", FALSE) || active;

	/* Populated only while a join is waiting on the browser. */
	auth_url = gowl_bar_json_string(json, "AuthURL");
	if (auth_url != NULL && auth_url[0] == '\0')
		g_clear_pointer(&auth_url, g_free);

	{
		g_autofree gchar *self = NULL;

		self = gowl_bar_json_object(json, "Self");
		if (self != NULL) {
			g_autofree gchar *dns = NULL;
			g_autofree gchar *host = NULL;
			g_auto(GStrv) ips = NULL;

			dns  = gowl_bar_json_string(self, "DNSName");
			host = gowl_bar_json_string(self, "HostName");
			if (dns != NULL) {
				gchar *dot = strchr(dns, '.');

				if (dot != NULL)
					*dot = '\0';
			}
			self_name = (dns != NULL && dns[0] != '\0')
				? g_strdup(dns) : g_strdup(host);

			ips = gowl_bar_json_string_array(self, "TailscaleIPs",
			                                 NULL);
			if (ips != NULL) {
				gint i;

				for (i = 0; ips[i] != NULL; i++) {
					if (g_str_has_prefix(ips[i], "100.")) {
						g_free(self_ip);
						self_ip = g_strdup(ips[i]);
						break;
					}
					if (self_ip == NULL)
						self_ip = g_strdup(ips[i]);
				}
			}
		}
	}

	ts_parse_peers(&peers, json);

	/* Everything the panel reads, swapped in one go. */
	g_mutex_lock(&td->lock);
	td->active      = active;
	td->needs_login = needs_login;
	td->joined      = joined;
	g_free(td->auth_url);    td->auth_url    = g_steal_pointer(&auth_url);
	g_free(td->status_text);
	td->status_text = g_strdup((state != NULL) ? state : "Unknown");
	g_free(td->self_name);   td->self_name   = g_steal_pointer(&self_name);
	g_free(td->self_ip);     td->self_ip     = g_steal_pointer(&self_ip);
	g_free(td->exit_node);   td->exit_node   = peers.exit_node;
	g_ptr_array_unref(td->peers);       td->peers      = peers.peers;
	g_ptr_array_unref(td->exit_nodes);  td->exit_nodes = peers.exit_nodes;
	/* Whatever was asked for has either happened or not by now: a
	   spinner that never stopped is what this used to be. */
	td->busy = FALSE;
	g_mutex_unlock(&td->lock);

	ts_apply_visibility(plugin, td);
	if (!gowl_bar_plugin_get_visible(plugin))
		return;

	ts_apply_state(plugin, td);

	/*
	 * One toast, and only when it is actionable.
	 *
	 * A join that is waiting on the browser is time-sensitive and easy
	 * to miss, so it gets a notification wired to this widget's own
	 * panel -- click it and the sign-in link is right there.  A
	 * lapsed session gets the same treatment.
	 *
	 * A host that has simply never joined gets nothing: the widget is
	 * visible and yellow, which is the invitation.  A toast every
	 * login telling you that you could set up Tailscale is nagging,
	 * not helping.
	 */
	if (td->auth_url != NULL &&
	    !gowl_bar_plugin_get_setting_bool(plugin, "auth-notified",
	                                      FALSE)) {
		gowl_bar_plugin_set_setting(plugin, "auth-notified", "true");
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
			"Tailscale is waiting for sign-in",
			"Open the link to finish joining the tailnet.");
	} else if (td->auth_url == NULL) {
		gowl_bar_plugin_set_setting(plugin, "auth-notified", NULL);
	}

	if (td->needs_login && td->joined && td->auth_url == NULL &&
	    !gowl_bar_plugin_get_setting_bool(plugin, "login-notified",
	                                      FALSE)) {
		gowl_bar_plugin_set_setting(plugin, "login-notified", "true");
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
			"Tailscale needs signing in again",
			"This device is a tailnet member but its session has "
			"expired.");
	}
}

static GowlBarPanel *
ts_panel(GowlBarPlugin *plugin, gpointer data)
{
	TailscaleData *td = data;
	GowlBarPanel *panel;
	GowlBarPanelItem *item;
	guint i;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 430);

	if (!td->installed) {
		gowl_bar_panel_add_hero(panel, "\xef\x95\x82", "Tailscale",
		                        "Not installed");
		gowl_bar_panel_add_label(panel,
			"Install the tailscale package to use this widget.");
		return panel;
	}
	if (td->daemon_down) {
		gowl_bar_panel_add_hero(panel, "\xef\x95\x82", "Tailscale",
		                        "tailscaled is not running");
		gowl_bar_panel_add_label(panel,
			"The tailscale command is installed but its daemon "
			"gave no answer.  Start the tailscaled service.");
		return panel;
	}

	/* Held across the build: every string and row below belongs to
	   the struct the worker rebuilds. */
	g_mutex_lock(&td->lock);

	if (!td->joined) {
		/*
		 * The set-up panel.  This is the whole reason the widget is
		 * visible on a host that has never joined: the machine is
		 * one click from being on the tailnet and the bar is where
		 * that click should be.
		 */
		gowl_bar_panel_add_hero(panel, "\xef\x95\x82", "Tailscale",
			(td->auth_url != NULL) ? "Waiting for sign-in"
			                       : "Not set up yet");

		if (td->auth_url != NULL) {
			/* A join is already in flight and the browser has
			   not been sent yet.  Everything else can wait. */
			item = gowl_bar_panel_add_row(panel, "auth",
				"\xef\x82\x8e", "Open the sign-in page",
				"Finish joining in your browser");
			gowl_bar_panel_item_set_color(item,
			                              GOWL_BAR_COLOR_GREEN);

			item = gowl_bar_panel_add_label(panel, td->auth_url);
			gowl_bar_panel_item_set_color(item,
			                              GOWL_BAR_COLOR_MUTED);

			gowl_bar_panel_add_separator(panel);
			item = gowl_bar_panel_add_buttons(panel, "auth-tool");
			gowl_bar_panel_add_button(item, "Copy link", FALSE);
			gowl_bar_panel_add_button(item, "Cancel", FALSE);
			g_mutex_unlock(&td->lock);
			return panel;
		}

		item = gowl_bar_panel_add_row(panel, "join", "\xef\x82\x90",
			"Join a tailnet", "Opens a sign-in page in your "
			"browser");
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);

		gowl_bar_panel_add_separator(panel);
		gowl_bar_panel_add_section(panel, "If joining fails");
		gowl_bar_panel_add_label(panel,
			"Bringing a device up usually needs root. Granting "
			"your user the operator role does that once, and "
			"then joining works from here on its own.");
		item = gowl_bar_panel_add_row(panel, "operator",
			"\xef\x82\x84", "Allow without sudo",
			"Asks for your password once");
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_SUBTEXT);

		g_mutex_unlock(&td->lock);
		return panel;
	}

	item = gowl_bar_panel_add_hero(panel, "\xef\x95\x82",
		(td->self_name != NULL) ? td->self_name : "Tailscale",
		td->active ? "Braiding packets" : "Sealed ports");
	/* The hero's id is what makes its trailing switch live. */
	gowl_bar_panel_item_set_id(item, "power");
	gowl_bar_panel_item_set_active(item, td->active);
	gowl_bar_panel_item_set_busy(item, td->busy);

	if (td->needs_login) {
		item = gowl_bar_panel_add_row(panel, "login", "\xef\x82\x90",
			"Sign in", "This device has not joined a tailnet");
		gowl_bar_panel_item_set_color(item, GOWL_BAR_COLOR_YELLOW);
	}

	gowl_bar_panel_add_field_pair(panel, "State",
		(td->status_text != NULL) ? td->status_text : "Unknown",
		"Address",
		(td->self_ip != NULL) ? td->self_ip : "--");
	if (td->exit_node != NULL && td->exit_node[0] != '\0')
		gowl_bar_panel_add_field(panel, "Exit node", td->exit_node);

	if (td->active && td->exit_nodes->len > 0) {
		gowl_bar_panel_add_separator(panel);
		gowl_bar_panel_add_section(panel, "Exit nodes");

		item = gowl_bar_panel_add_row(panel, "exit-none", NULL,
		                              "None", "Route directly");
		gowl_bar_panel_item_set_active(item,
			(td->exit_node == NULL || td->exit_node[0] == '\0'));

		for (i = 0; i < td->exit_nodes->len && i < 8; i++) {
			g_auto(GStrv) fields = NULL;

			fields = g_strsplit(
				g_ptr_array_index(td->exit_nodes, i), "\t", 2);
			if (g_strv_length(fields) < 2)
				continue;
			{
				g_autofree gchar *row_id = NULL;

				row_id = g_strdup_printf("exit:%s", fields[0]);
				item = gowl_bar_panel_add_row(panel, row_id,
					"\xef\x82\xa3", fields[0], fields[1]);
			}
			gowl_bar_panel_item_set_active(item,
				g_strcmp0(td->exit_node, fields[0]) == 0);
		}
	}

	if (td->active && td->peers->len > 0) {
		gowl_bar_panel_add_separator(panel);
		gowl_bar_panel_add_section(panel, "Machines");

		for (i = 0; i < td->peers->len; i++) {
			g_auto(GStrv) fields = NULL;
			gboolean online;

			fields = g_strsplit(g_ptr_array_index(td->peers, i),
			                    "\t", 4);
			if (g_strv_length(fields) < 4)
				continue;
			online = (g_strcmp0(fields[3], "1") == 0);

			{
				g_autofree gchar *row_id = NULL;

				row_id = g_strdup_printf("peer:%s", fields[1]);
				item = gowl_bar_panel_add_row(panel, row_id,
					online ? "\xef\x84\x91"
					       : "\xef\x84\x8c",
					fields[0], fields[1]);
			}
			gowl_bar_panel_item_set_value(item,
				online ? "online" : "offline");
			gowl_bar_panel_item_set_value_color(item,
				online ? GOWL_BAR_COLOR_GREEN
				       : GOWL_BAR_COLOR_MUTED);
			gowl_bar_panel_item_set_disabled(item, !online);
		}
	}

	gowl_bar_panel_add_separator(panel);
	item = gowl_bar_panel_add_buttons(panel, "tool");
	gowl_bar_panel_add_button(item, td->active ? "Down" : "Up", FALSE);
	gowl_bar_panel_add_button(item, "Refresh", FALSE);
	gowl_bar_panel_add_button(item, "Admin", FALSE);

	g_mutex_unlock(&td->lock);
	return panel;
}

/* Re-read the status on the worker and rebuild the panel from it. */
static void
ts_refresh_work(GowlBarPlugin *plugin, gpointer user_data)
{
	ts_poll_async(plugin, user_data);
	gowl_bar_plugin_request_panel_refresh(plugin);
}

static void
ts_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
          gint index, gdouble value, guint button)
{
	TailscaleData *td = data;

	(void)value;
	(void)button;

	if (g_strcmp0(item_id, "power") == 0) {
		td->busy = TRUE;
		gowl_bar_plugin_spawn(plugin,
			td->active ? "tailscale down" : "tailscale up");
		return;
	}

	if (g_strcmp0(item_id, "login") == 0 ||
	    g_strcmp0(item_id, "join") == 0) {
		/* `tailscale up' publishes its sign-in URL through the
		   status reply, so the next poll picks it up and the panel
		   turns into the open-the-link view by itself.  Nothing
		   here has to parse the command's output. */
		td->busy = TRUE;
		gowl_bar_plugin_set_setting(plugin, "auth-notified", NULL);
		gowl_bar_plugin_spawn(plugin, "tailscale up");
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW,
			"Joining a tailnet",
			"A sign-in link will appear here shortly.");
		return;
	}

	if (g_strcmp0(item_id, "auth") == 0) {
		g_autofree gchar *quoted = NULL;
		g_autofree gchar *line = NULL;
		g_autofree gchar *url = NULL;

		g_mutex_lock(&td->lock);
		url = g_strdup(td->auth_url);
		g_mutex_unlock(&td->lock);
		if (url == NULL)
			return;
		quoted = g_shell_quote(url);
		line = g_strdup_printf("%s %s",
			(gowl_bar_plugin_get_setting(plugin, "browser-command")
			 != NULL)
				? gowl_bar_plugin_get_setting(plugin,
					"browser-command")
				: "xdg-open",
			quoted);
		gowl_bar_plugin_spawn(plugin, line);
		return;
	}

	if (g_strcmp0(item_id, "auth-tool") == 0) {
		g_autofree gchar *url = NULL;

		g_mutex_lock(&td->lock);
		url = g_strdup(td->auth_url);
		if (index == 1)
			g_clear_pointer(&td->auth_url, g_free);
		g_mutex_unlock(&td->lock);

		if (index == 0 && url != NULL) {
			g_autofree gchar *quoted = NULL;
			g_autofree gchar *line = NULL;
			const gchar *copy;

			copy = gowl_bar_plugin_get_setting(plugin,
			                                   "copy-command");
			quoted = g_shell_quote(url);
			/* A pipe, so the spawn helper gives it a shell. */
			line = g_strdup_printf("printf %%s %s | %s",
				quoted, (copy != NULL) ? copy : "wl-copy");
			gowl_bar_plugin_spawn(plugin, line);
			gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW,
				"Copied", "The sign-in link is on the "
				"clipboard.");
		} else if (index == 1) {
			gowl_bar_plugin_spawn(plugin, "tailscale logout");
		}
		return;
	}

	if (g_strcmp0(item_id, "operator") == 0) {
		g_autofree gchar *line = NULL;

		/* pkexec so the polkit prompt is the desktop's, not a
		   terminal the bar has no way to give you. */
		line = g_strdup_printf("pkexec tailscale set --operator=%s",
		                       g_get_user_name());
		gowl_bar_plugin_spawn(plugin, line);
		return;
	}

	if (g_strcmp0(item_id, "exit-none") == 0) {
		gowl_bar_plugin_spawn(plugin, "tailscale set --exit-node=");
		return;
	}

	if (item_id != NULL && g_str_has_prefix(item_id, "exit:")) {
		g_autofree gchar *quoted = NULL;
		g_autofree gchar *line = NULL;

		quoted = g_shell_quote(item_id + strlen("exit:"));
		line = g_strdup_printf("tailscale set --exit-node=%s", quoted);
		gowl_bar_plugin_spawn(plugin, line);
		return;
	}

	if (item_id != NULL && g_str_has_prefix(item_id, "peer:")) {
		const gchar *ip = item_id + strlen("peer:");
		g_autofree gchar *quoted = NULL;
		g_autofree gchar *line = NULL;

		/* Copying the address is what a machine list is for; the
		   clipboard tool is configurable because a session may
		   have wl-copy, cmacs, or neither. */
		if (ip[0] == '\0')
			return;
		quoted = g_shell_quote(ip);
		line = g_strdup_printf("%s %s",
			gowl_bar_plugin_get_setting(plugin, "copy-command")
				!= NULL
				? gowl_bar_plugin_get_setting(plugin,
					"copy-command")
				: "wl-copy",
			quoted);
		gowl_bar_plugin_spawn(plugin, line);
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW,
			"Copied", ip);
		return;
	}

	if (g_strcmp0(item_id, "tool") == 0) {
		switch (index) {
		case 0:
			td->busy = TRUE;
			gowl_bar_plugin_spawn(plugin,
				td->active ? "tailscale down" : "tailscale up");
			break;
		case 1:
			/* Ask again, on the worker: rebuilding the panel from
			   the cache is not a refresh. */
			gowl_bar_plugin_queue_work(plugin, ts_refresh_work, td,
			                           NULL);
			break;
		case 2:
			gowl_bar_plugin_spawn(plugin,
				"xdg-open https://login.tailscale.com/admin/machines");
			break;
		default:
			break;
		}
	}
}

static gboolean
ts_click(GowlBarPlugin *plugin, gpointer data, guint button, gint x, gint y,
         guint modifiers)
{
	TailscaleData *td = data;

	(void)x;
	(void)y;
	(void)modifiers;

	/* Middle-click toggles without opening the panel: the one thing
	   this widget is asked to do most. */
	if (button == BTN_MIDDLE) {
		gowl_bar_plugin_spawn(plugin,
			td->active ? "tailscale down" : "tailscale up");
		return TRUE;
	}
	return FALSE;
}

static const GowlBarPluginVTable tailscale_vtable = {
	sizeof(GowlBarPluginVTable),
	ts_create, ts_destroy,
	NULL, NULL, ts_configure,
	ts_interval, NULL, ts_poll_async,
	NULL, NULL,
	ts_click, NULL,
	ts_panel, ts_action,
	NULL, NULL,
	NULL,
	NULL
};

/* ----------------------------------------------------------------
 * Registration
 * ---------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * bluetooth
 *
 * Adapter power, what is connected, and one click to connect or
 * disconnect a known device.
 *
 * bluetoothctl rather than BlueZ over D-Bus: the state this widget
 * needs is four short lines of output, where the D-Bus equivalent is
 * GetManagedObjects and a walk of a nested variant for every device on
 * the machine.  Everything it runs is a setting, so a different tool
 * can be dropped in without touching this.
 *
 * Scanning is the part with a shape worth knowing about.  BlueZ ties a
 * discovery session to the D-Bus client that asked for it, so a
 * `bluetoothctl scan on' fired off and forgotten is a client that
 * connects, asks, and leaves --- and the far end has every reason to
 * stop scanning when it does.  The scan is held open for a bounded
 * window instead, by a bluetoothctl that stays alive for it.
 * ---------------------------------------------------------------- */

#define BT_MAX_DEVICES (12)

/* How long one press of the scan toggle scans for, in seconds.  Bounded
 * on purpose: the worker pool is drained on the way out, so an unbounded
 * scan is a session that takes as long to quit as the scan had left. */
#define BT_SCAN_SECONDS (10)

typedef BarBtDevice BtDevice;

typedef struct {
	gboolean  present;              /* an adapter exists at all */
	gboolean  tool_missing;         /* bluetoothctl is not installed */
	gboolean  powered;
	gboolean  scanning;             /* a scan WE are holding open */
	gboolean  discovering;          /* what the adapter reports */
	GPtrArray *devices;             /* element-type BtDevice */
	/*
	 * Filled by the ASYNC poll -- each listing costs a subprocess, and
	 * the sync poll, the panel and a click all run on the compositor
	 * thread, where that would not make the bar slow but freeze the
	 * editor.
	 *
	 * The device list is guarded because it is the one thing here that
	 * is FREED rather than overwritten.  Rebuilding it drops every
	 * BtDevice, and the dispatch thread is free to be walking the same
	 * array building a panel at that moment: the host only stops an
	 * async poll from overlapping itself.  A use-after-free here
	 * corrupts the heap and surfaces much later, in another thread,
	 * as an abort nothing can be traced back from.
	 */
	GMutex    lock;
} BtData;

static gpointer
bt_create(GowlBarPlugin *plugin)
{
	BtData *bd = g_new0(BtData, 1);

	(void)plugin;
	bd->devices = g_ptr_array_new_with_free_func(bar_bt_device_free);
	g_mutex_init(&bd->lock);
	return bd;
}

static void
bt_destroy(GowlBarPlugin *plugin, gpointer data)
{
	BtData *bd = data;

	(void)plugin;
	if (bd == NULL)
		return;
	g_ptr_array_unref(bd->devices);
	g_mutex_clear(&bd->lock);
	g_free(bd);
}

static const gchar *
bt_tool(GowlBarPlugin *plugin)
{
	const gchar *tool = gowl_bar_plugin_get_setting(plugin, "command");

	return (tool != NULL && *tool != '\0') ? tool : "bluetoothctl";
}

static void
bt_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	BtData           *bd = data;
	const gchar      *tool;
	g_autofree gchar *show = NULL;
	g_autofree gchar *all = NULL;
	g_autofree gchar *paired = NULL;
	g_autofree gchar *conn = NULL;
	g_autoptr(GPtrArray) fresh = NULL;
	const gchar      *argv[4];

	if (bd == NULL)
		return;

	tool = bt_tool(plugin);
	if (!bar_have_command(tool)) {
		bd->tool_missing = TRUE;
		bd->present = FALSE;
		return;
	}
	bd->tool_missing = FALSE;

	/*
	 * bar_run_argv, not bar_run_shell_line: these outputs are several
	 * lines and the `_line' helper returns only the first, which
	 * silently threw away every line that mattered.  There is no shell
	 * here either -- an argv is not parsed by one -- so a `2>/dev/null'
	 * would arrive as a literal argument to bluetoothctl rather than a
	 * redirection.  bar_run_argv already sends stderr to /dev/null.
	 */
	argv[0] = tool; argv[1] = "show"; argv[2] = NULL;
	show = bar_run_argv(argv);

	/* No adapter is not an error and not a state to report: the widget
	   hides itself, the way the battery does on a desktop. */
	bd->present = (show != NULL && *show != '\0'
	               && strstr(show, "No default controller") == NULL);
	if (!bd->present)
		return;

	bd->powered = (strstr(show, "Powered: yes") != NULL);

	/*
	 * What the ADAPTER says, which is not the same question as
	 * whether we are holding a scan open: another client can be
	 * discovering, and our own scan has a moment at each end where
	 * the two disagree.  The toggle shows either.
	 */
	bd->discovering = (strstr(show, "Discovering: yes") != NULL);

	/*
	 * Built into a fresh array and swapped in under the lock, rather
	 * than clearing the live one: the dispatch thread may be walking
	 * it right now, and freeing what it is reading is how a heap gets
	 * corrupted.
	 */
	fresh = g_ptr_array_new_with_free_func(bar_bt_device_free);
	if (bd->powered) {
		/*
		 * `devices' with no filter, FIRST.  The widget used to ask
		 * only for Paired and Connected, which is every device
		 * except the ones a scan exists to find -- so scanning
		 * worked and had nowhere to put what it found.  The two
		 * filtered listings then say which of these are already
		 * yours.
		 */
		argv[0] = tool; argv[1] = "devices"; argv[2] = NULL;
		argv[3] = NULL;
		all = bar_run_argv(argv);

		argv[2] = "Paired";
		paired = bar_run_argv(argv);

		argv[2] = "Connected";
		conn = bar_run_argv(argv);

		bar_bt_devices_parse(fresh, all, paired, conn,
		                     BT_MAX_DEVICES);
	}

	g_mutex_lock(&bd->lock);
	g_ptr_array_unref(bd->devices);
	bd->devices = g_steal_pointer(&fresh);
	g_mutex_unlock(&bd->lock);
}

/* The connected device, or NULL.  The first is enough for a label. */
static const BtDevice *
bt_first_connected(BtData *bd)
{
	guint i;

	if (bd == NULL)
		return NULL;
	for (i = 0; i < bd->devices->len; i++) {
		BtDevice *d = g_ptr_array_index(bd->devices, i);

		if (d->connected)
			return d;
	}
	return NULL;
}

static void
bt_poll(GowlBarPlugin *plugin, gpointer data)
{
	BtData         *bd = data;
	const BtDevice *conn;
	guint           n_conn = 0, i;

	if (bd != NULL && bd->tool_missing) {
		/*
		 * Visible but plainly not working.  Hiding is right for a
		 * machine with no adapter; hiding because the TOOL is
		 * absent looks identical to a widget that was never
		 * added, and leaves nothing to diagnose from.
		 */
		gowl_bar_plugin_set_visible(plugin, TRUE);
		gowl_bar_plugin_set_icon(plugin, "\xef\x8a\x94");
		gowl_bar_plugin_set_label(plugin, NULL);
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_MUTED);
		gowl_bar_plugin_set_tooltip(plugin,
			"Bluetooth: bluetoothctl is not installed");
		return;
	}

	if (bd == NULL || !bd->present) {
		/* Nothing to say and nothing to click: a machine with no
		   adapter should not carry a dead icon forever. */
		gowl_bar_plugin_set_visible(plugin, FALSE);
		return;
	}
	gowl_bar_plugin_set_visible(plugin, TRUE);

	g_mutex_lock(&bd->lock);
	for (i = 0; i < bd->devices->len; i++) {
		if (((BtDevice *)g_ptr_array_index(bd->devices, i))->connected)
			n_conn++;
	}
	conn = bt_first_connected(bd);

	/* Nerd Font: bluetooth (U+F293), and bluetooth-off (U+F294) so the
	   powered state reads from the shape as well as the colour. */
	gowl_bar_plugin_set_icon(plugin,
		bd->powered ? "\xef\x8a\x93" : "\xef\x8a\x94");

	if (!bd->powered) {
		gowl_bar_plugin_set_label(plugin, NULL);
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_MUTED);
		gowl_bar_plugin_set_tooltip(plugin, "Bluetooth is off");
	} else if (n_conn == 0) {
		gowl_bar_plugin_set_label(plugin, NULL);
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_TEXT);
		gowl_bar_plugin_set_tooltip(plugin,
			"Bluetooth on, nothing connected");
	} else {
		if (n_conn == 1) {
			gowl_bar_plugin_set_label(plugin, conn->name);
			gowl_bar_plugin_set_tooltip(plugin, conn->name);
		} else {
			gchar buf[32];

			g_snprintf(buf, sizeof(buf), "%u", n_conn);
			gowl_bar_plugin_set_label(plugin, buf);
			gowl_bar_plugin_set_tooltip(plugin,
				"Connected devices");
		}
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_BLUE);
	}
	g_mutex_unlock(&bd->lock);
}

static gint
bt_interval(GowlBarPlugin *plugin, gpointer data)
{
	(void)plugin;
	(void)data;
	return 5;
}

static GowlBarPanel *
bt_panel(GowlBarPlugin *plugin, gpointer data)
{
	BtData           *bd = data;
	GowlBarPanel     *panel;
	GowlBarPanelItem *item;
	guint             i;

	panel = gowl_bar_panel_new();
	gowl_bar_panel_set_width(panel, 380);

	if (bd == NULL || !bd->present) {
		gowl_bar_panel_add_hero(panel, "\xef\x8a\x94", "Bluetooth",
		                        "No adapter");
		if (!bar_have_command(bt_tool(plugin)))
			gowl_bar_panel_add_field(panel, "Missing",
			                         bt_tool(plugin));
		return panel;
	}

	gowl_bar_panel_add_hero(panel,
		bd->powered ? "\xef\x8a\x93" : "\xef\x8a\x94", "Bluetooth",
		bd->powered ? "On" : "Off");
	gowl_bar_panel_add_separator(panel);
	gowl_bar_panel_add_toggle(panel, "power", "Bluetooth", bd->powered);

	if (!bd->powered)
		return panel;

	{
		gboolean busy = bd->scanning || bd->discovering;

		gowl_bar_panel_add_toggle(panel, "scan", "Scan for devices",
		                          busy);
	}

	g_mutex_lock(&bd->lock);
	i = bd->devices->len;
	g_mutex_unlock(&bd->lock);
	if (i == 0) {
		gowl_bar_panel_add_separator(panel);
		gowl_bar_panel_add_field(panel, "Devices",
			bd->scanning ? "Scanning..." : "None paired");
		return panel;
	}

	gowl_bar_panel_add_separator(panel);
	gowl_bar_panel_add_section(panel, "Devices");
	g_mutex_lock(&bd->lock);
	for (i = 0; i < bd->devices->len; i++) {
		BtDevice         *d = g_ptr_array_index(bd->devices, i);
		g_autofree gchar *id = NULL;
		const gchar      *detail;

		/* The row id carries the index, so a click needs no lookup
		   by name -- two devices may share one. */
		id = g_strdup_printf("dev:%u", i);

		/*
		 * A device found by a scan says what clicking it will do.
		 * "Not paired" and an address are the same information to
		 * somebody who already knows bluetooth and a dead end to
		 * everybody else.
		 */
		detail = d->connected ? "Connected"
		       : (d->paired ? d->mac : "Tap to pair");
		item = gowl_bar_panel_add_row(panel, id,
			d->connected ? "\xef\x8a\x93" : "\xef\x8a\x94",
			d->name, detail);
		(void)item;
	}
	g_mutex_unlock(&bd->lock);

	return panel;
}

/*
 * Hold a scan open.
 *
 * BlueZ hands a discovery session to the D-Bus client that asked for
 * it and takes it back when that client goes away, so the scan lasts
 * exactly as long as something stays connected asking for it.  A
 * bluetoothctl told to scan and left to its own devices is not that:
 * `--timeout' is, and it bounds the scan into the bargain, which
 * matters because the worker pool is drained on the way out.
 *
 * Runs on a worker thread; everything it touches here is the plugin's
 * own and the array it refreshes is swapped under the lock.
 */
static void
bt_scan_work(GowlBarPlugin *plugin, gpointer user_data)
{
	BtData      *bd = user_data;
	const gchar *argv[6];
	gchar        secs[16];
	g_autofree gchar *out = NULL;

	if (bd == NULL)
		return;

	g_snprintf(secs, sizeof(secs), "%d", BT_SCAN_SECONDS);
	argv[0] = bt_tool(plugin);
	argv[1] = "--timeout";
	argv[2] = secs;
	argv[3] = "scan";
	argv[4] = "on";
	argv[5] = NULL;
	out = bar_run_argv(argv);

	/* Whatever it found is in the adapter's device list now, so the
	 * ordinary poll is what publishes it -- and the panel has to be
	 * told to look again, since it was built when the scan started. */
	bd->scanning = FALSE;
	bt_poll_async(plugin, bd);
	gowl_bar_plugin_request_panel_refresh(plugin);
}

static void
bt_action(GowlBarPlugin *plugin, gpointer data, const gchar *item_id,
          gint index, gdouble value, guint button)
{
	BtData           *bd = data;
	const gchar      *tool = bt_tool(plugin);
	g_autofree gchar *line = NULL;

	(void)value;
	(void)button;

	if (item_id == NULL || bd == NULL)
		return;

	if (g_strcmp0(item_id, "power") == 0) {
		line = g_strdup_printf("%s power %s", tool,
		                       bd->powered ? "off" : "on");
		gowl_bar_plugin_spawn(plugin, line);
		/* Optimistic, so the toggle moves under the finger rather
		   than at the next poll five seconds later. */
		bd->powered = !bd->powered;
		gowl_bar_plugin_request_panel_refresh(plugin);
		return;
	}

	if (g_strcmp0(item_id, "scan") == 0) {
		if (bd->scanning || bd->discovering) {
			/* Ours ends on its own timeout; this also ends one
			 * somebody else started, which is what the toggle
			 * showing "on" promised. */
			line = g_strdup_printf("%s scan off", tool);
			gowl_bar_plugin_spawn(plugin, line);
			bd->scanning = FALSE;
			bd->discovering = FALSE;
		} else {
			/* Optimistic, so the toggle moves under the finger
			 * rather than when the first listing comes back. */
			bd->scanning = TRUE;
			gowl_bar_plugin_queue_work(plugin, bt_scan_work,
			                           bd, NULL);
		}
		gowl_bar_plugin_request_panel_refresh(plugin);
		return;
	}

	if (g_str_has_prefix(item_id, "dev:")) {
		guint     idx = (guint)g_ascii_strtoull(item_id + 4, NULL, 10);
		BtDevice *d;

		g_mutex_lock(&bd->lock);
		if (idx >= bd->devices->len) {
			g_mutex_unlock(&bd->lock);
			return;
		}
		d = g_ptr_array_index(bd->devices, idx);

		/*
		 * A device a scan just turned up has never been paired, and
		 * `connect' on one of those fails: pairing is the step that
		 * has to happen first, and it is the whole reason to scan.
		 */
		if (!d->paired) {
			line = g_strdup_printf("%s pair %s", tool, d->mac);
		} else {
			line = g_strdup_printf("%s %s %s", tool,
				d->connected ? "disconnect" : "connect",
				d->mac);
		}
		{
			g_autofree gchar *name = g_strdup(d->name);
			gboolean was = d->connected;
			gboolean known = d->paired;

			if (known)
				d->connected = !d->connected;
			g_mutex_unlock(&bd->lock);

			gowl_bar_plugin_spawn(plugin, line);
			gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW,
				!known ? "Pairing"
				       : (was ? "Disconnecting" : "Connecting"),
				name);
		}
		gowl_bar_plugin_request_panel_refresh(plugin);
	}

	(void)index;
}

static const GowlBarPluginVTable bluetooth_vtable = {
	sizeof(GowlBarPluginVTable),
	bt_create, bt_destroy,
	NULL, NULL, NULL,
	bt_interval, bt_poll, bt_poll_async,
	NULL, NULL,
	NULL, NULL,
	bt_panel, bt_action,
	NULL, NULL,
	NULL,
	NULL
};


/**
 * bar_register_net_plugins:
 * @registry: the registry to populate
 */
void
bar_register_net_plugins(GowlBarRegistry *registry)
{
	gowl_bar_registry_register_vtable(registry, "network", "Network",
		"Link state, with a panel for Wi-Fi and DNS",
		&network_vtable);
	gowl_bar_registry_register_vtable(registry, "net", "Throughput",
		"Receive and transmit rates", &rate_vtable);
	gowl_bar_registry_register_vtable(registry, "ip", "IP address",
		"The interface's IPv4 address", &ip_vtable);
	gowl_bar_registry_register_vtable(registry, "wifi", "Wi-Fi signal",
		"Wireless signal strength", &wifi_vtable);
	gowl_bar_registry_register_vtable(registry, "vpn", "VPN",
		"Whether a tunnel interface is up", &vpn_vtable);
	gowl_bar_registry_register_vtable(registry, "bluetooth", "Bluetooth",
		"Adapter power and paired devices", &bluetooth_vtable);
	gowl_bar_registry_register_alias(registry, "bt", "bluetooth");

	gowl_bar_registry_register_vtable(registry, "tailscale", "Tailscale",
		"Tailnet state, peers and exit nodes", &tailscale_vtable);

	gowl_bar_registry_register_alias(registry, "wlan", "wifi");
	gowl_bar_registry_register_alias(registry, "ts", "tailscale");
}
