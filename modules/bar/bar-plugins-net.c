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

/* ----------------------------------------------------------------
 * Shared network state
 *
 * One struct per widget instance.  Everything in it is written by the
 * async poll and read by the panel builder, both of which the host
 * serialises: the async poll for an instance never overlaps itself,
 * and a panel is built on the dispatch thread between polls.
 * ---------------------------------------------------------------- */

typedef struct {
	gchar   *iface;
	gchar   *ipv4;
	gchar   *gateway;
	gchar   *ssid;
	gboolean wireless;
	gboolean online;
	gint     signal_dbm;
	gdouble  quality;
	gdouble  ping_ms;
	gint     packet_loss;
	glong    rx_total, tx_total;
	glong    rx_rate, tx_rate;

	/* Nearby and known networks, as `SSID\tSIGNAL\tSECURITY\tACTIVE'
	   rows straight out of nmcli's terse output. */
	GPtrArray *scan;
	gboolean   have_nmcli;
	gchar     *dns_mode;
	gboolean   busy;
} NetData;

static gpointer
net_create(GowlBarPlugin *plugin)
{
	NetData *nd;

	(void)plugin;
	nd = g_new0(NetData, 1);
	nd->scan = g_ptr_array_new_with_free_func(g_free);
	nd->ping_ms = -1.0;
	nd->packet_loss = -1;
	nd->have_nmcli = bar_have_command("nmcli");
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
	g_free(nd->dns_mode);
	g_ptr_array_unref(nd->scan);
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
   and a longer wait would hold a worker thread open. */
static void
net_measure_ping(NetData *nd, const gchar *host)
{
	const gchar *argv[] = { "ping", "-c", "1", "-W", "1", NULL, NULL };
	g_autofree gchar *out = NULL;
	const gchar *p;

	argv[5] = (host != NULL && host[0] != '\0') ? host : "1.1.1.1";

	nd->ping_ms     = -1.0;
	nd->packet_loss = 100;

	out = bar_run_argv(argv);
	if (out == NULL)
		return;

	p = strstr(out, "time=");
	if (p != NULL) {
		nd->ping_ms = g_ascii_strtod(p + 5, NULL);
		nd->packet_loss = 0;
	}
}

/* Pull the visible networks out of nmcli.  The terse output is
   colon-separated with escaped colons inside fields, so the split has
   to respect the backslash escape nmcli emits. */
static void
net_scan_wifi(NetData *nd)
{
	const gchar *argv[] = {
		"nmcli", "-t", "-f", "ACTIVE,SSID,SIGNAL,SECURITY",
		"device", "wifi", "list", NULL
	};
	g_autofree gchar *out = NULL;
	g_auto(GStrv) lines = NULL;
	gint i;

	g_ptr_array_set_size(nd->scan, 0);

	if (!nd->have_nmcli)
		return;

	out = bar_run_argv(argv);
	if (out == NULL)
		return;

	lines = g_strsplit(out, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		GString *field;
		GPtrArray *fields;
		const gchar *p;

		if (lines[i][0] == '\0')
			continue;

		fields = g_ptr_array_new_with_free_func(g_free);
		field  = g_string_new(NULL);
		for (p = lines[i]; *p != '\0'; p++) {
			if (*p == '\\' && p[1] != '\0') {
				p++;
				g_string_append_c(field, *p);
				continue;
			}
			if (*p == ':') {
				g_ptr_array_add(fields,
					g_string_free(field, FALSE));
				field = g_string_new(NULL);
				continue;
			}
			g_string_append_c(field, *p);
		}
		g_ptr_array_add(fields, g_string_free(field, FALSE));

		if (fields->len >= 4) {
			const gchar *ssid;

			ssid = g_ptr_array_index(fields, 1);
			if (ssid != NULL && ssid[0] != '\0') {
				g_ptr_array_add(nd->scan,
					g_strdup_printf("%s\t%s\t%s\t%s",
						ssid,
						(const gchar *)
							g_ptr_array_index(fields, 2),
						(const gchar *)
							g_ptr_array_index(fields, 3),
						(const gchar *)
							g_ptr_array_index(fields, 0)));
			}
		}
		g_ptr_array_unref(fields);

		/* A long scan list makes the panel unusable; the strongest
		   twenty is more than anyone picks from. */
		if (nd->scan->len >= 20)
			break;
	}
}

static void
net_poll_async(GowlBarPlugin *plugin, gpointer data)
{
	NetData *nd = data;
	BarSysinfo *info = net_info();
	g_autofree gchar *iface = NULL;
	const gchar *configured;
	gchar buf[128];
	gchar rx_buf[32], tx_buf[32];

	if (info == NULL)
		return;

	configured = gowl_bar_plugin_get_setting(plugin, "param");
	if (configured != NULL && configured[0] != '\0')
		iface = g_strdup(configured);
	else
		iface = bar_sysinfo_default_route_iface(info);

	g_free(nd->iface);
	nd->iface = g_strdup(iface);
	nd->online = (iface != NULL);

	if (iface != NULL) {
		nd->wireless = bar_sysinfo_iface_is_wireless(info, iface);
		g_free(nd->ipv4);
		nd->ipv4 = bar_sysinfo_ipv4(info, iface);
		g_free(nd->gateway);
		nd->gateway = bar_sysinfo_default_gateway(info);
		bar_sysinfo_iface_bytes(info, iface, &nd->rx_total,
		                        &nd->tx_total);
		bar_sysinfo_net(info, iface, &nd->rx_rate, &nd->tx_rate);
	}

	if (nd->wireless) {
		const gchar *wifi_iface = NULL;

		bar_sysinfo_wifi(info, &wifi_iface, &nd->signal_dbm,
		                 &nd->quality);
		if (nd->have_nmcli) {
			const gchar *argv[] = {
				"nmcli", "-t", "-f", "GENERAL.CONNECTION",
				"device", "show", NULL, NULL
			};
			g_autofree gchar *out = NULL;

			argv[6] = iface;
			out = bar_run_argv_line(argv);
			if (out != NULL) {
				const gchar *colon = strchr(out, ':');

				g_free(nd->ssid);
				nd->ssid = g_strdup((colon != NULL)
				                    ? colon + 1 : out);
			}
		}
	}

	net_measure_ping(nd,
		gowl_bar_plugin_get_setting(plugin, "ping-host"));

	/* Only rescan while the panel is showing: a background wifi scan
	   every five seconds disrupts the connection it is measuring. */
	if (gowl_bar_plugin_get_setting_bool(plugin, "panel-open", FALSE))
		net_scan_wifi(nd);

	if (!nd->online) {
		gowl_bar_plugin_set_label(plugin, "offline");
		gowl_bar_plugin_set_icon(plugin, "\xef\x87\xab");
		gowl_bar_plugin_set_color(plugin, GOWL_BAR_COLOR_RED);
		return;
	}

	if (gowl_bar_plugin_get_setting_bool(plugin, "show-rates", FALSE)) {
		bar_sysinfo_format_rate(nd->rx_rate, rx_buf, sizeof(rx_buf));
		bar_sysinfo_format_rate(nd->tx_rate, tx_buf, sizeof(tx_buf));
		g_snprintf(buf, sizeof(buf),
		           "\xe2\x86\x93%s \xe2\x86\x91%s", rx_buf, tx_buf);
	} else if (nd->wireless && nd->ssid != NULL) {
		g_snprintf(buf, sizeof(buf), "%s", nd->ssid);
	} else if (nd->ipv4 != NULL) {
		g_snprintf(buf, sizeof(buf), "%s", nd->ipv4);
	} else {
		g_snprintf(buf, sizeof(buf), "%s", nd->iface);
	}

	gowl_bar_plugin_set_label(plugin, buf);
	gowl_bar_plugin_set_icon(plugin,
		nd->wireless ? "\xef\x87\xab" : "\xef\x9b\xbf");
	gowl_bar_plugin_set_color(plugin,
		(nd->packet_loss == 0) ? GOWL_BAR_COLOR_TEXT
		                       : GOWL_BAR_COLOR_PEACH);
}

static void
net_panel_opened(GowlBarPlugin *plugin, gpointer data)
{
	NetData *nd = data;

	(void)nd;
	/* The async poll reads this to decide whether to scan; setting it
	   here rather than scanning inline keeps the scan off the
	   compositor thread. */
	gowl_bar_plugin_set_setting(plugin, "panel-open", "true");
	gowl_bar_plugin_request_redraw(plugin);
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

	/* Hero: what you are connected through, and its whimsical
	   subtitle --- the panel is a utility, and a little character in
	   the one line that never carries data costs nothing. */
	if (nd->wireless) {
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

	if (nd->ping_ms >= 0.0)
		g_snprintf(buf, sizeof(buf), "%.1f ms", nd->ping_ms);
	else
		g_strlcpy(buf, "Timeout", sizeof(buf));
	g_snprintf(buf2, sizeof(buf2), "%d%%",
	           (nd->packet_loss >= 0) ? nd->packet_loss : 100);
	item = gowl_bar_panel_add_field_pair(panel, "Ping", buf,
	                                     "Packet Loss", buf2);
	gowl_bar_panel_item_set_value_color(item,
		(nd->packet_loss == 0) ? GOWL_BAR_COLOR_SUBTEXT
		                       : GOWL_BAR_COLOR_RED);

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

	gowl_bar_panel_add_separator(panel);
	gowl_bar_panel_add_section(panel, "DNS provider");
	item = gowl_bar_panel_add_buttons(panel, "dns");
	gowl_bar_panel_add_button(item, "DHCP",
		g_strcmp0(nd->dns_mode, "dhcp") == 0 || nd->dns_mode == NULL);
	gowl_bar_panel_add_button(item, "Cloudflare",
		g_strcmp0(nd->dns_mode, "cloudflare") == 0);
	gowl_bar_panel_add_button(item, "Google",
		g_strcmp0(nd->dns_mode, "google") == 0);
	gowl_bar_panel_add_button(item, "Quad9",
		g_strcmp0(nd->dns_mode, "quad9") == 0);
	if (!nd->have_nmcli) {
		gowl_bar_panel_add_label(panel,
			"nmcli is not installed, so DNS cannot be changed "
			"from here");
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
		g_autofree gchar *line = NULL;

		if (index < 0 || index > 3 || !nd->have_nmcli ||
		    nd->iface == NULL)
			return;

		g_free(nd->dns_mode);
		nd->dns_mode = g_strdup(modes[index]);

		if (index == 0) {
			line = g_strdup_printf(
				"nmcli device modify %s ipv4.ignore-auto-dns no",
				nd->iface);
		} else {
			line = g_strdup_printf(
				"nmcli device modify %s ipv4.dns %s "
				"ipv4.ignore-auto-dns yes",
				nd->iface, servers[index]);
		}
		gowl_bar_plugin_spawn(plugin, line);
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_LOW,
			"DNS provider changed", modes[index]);
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
			gowl_bar_plugin_refresh_panel(plugin);
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
	net_panel_opened, NULL
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
}

static const GowlBarPluginVTable rate_vtable = {
	sizeof(GowlBarPluginVTable),
	net_create, net_destroy,
	NULL, NULL, NULL,
	rate_interval, rate_poll, NULL,
	NULL, NULL,
	NULL, NULL,
	net_panel, net_panel_action,
	NULL, NULL
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
	g_autofree gchar *addr = NULL;

	(void)data;

	if (info == NULL)
		return;
	addr = bar_sysinfo_ipv4(info,
		gowl_bar_plugin_get_setting(plugin, "param"));
	gowl_bar_plugin_set_label(plugin,
	                          (addr != NULL) ? addr : "no address");
	gowl_bar_plugin_set_color(plugin,
		(addr != NULL) ? GOWL_BAR_COLOR_TEXT : GOWL_BAR_COLOR_MUTED);
}

static const GowlBarPluginVTable ip_vtable = {
	sizeof(GowlBarPluginVTable),
	net_create, net_destroy,
	NULL, NULL, NULL,
	ip_interval, ip_poll, NULL,
	NULL, NULL,
	NULL, NULL,
	net_panel, net_panel_action,
	NULL, NULL
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
	gowl_bar_plugin_set_color(plugin,
		(quality > 0.6) ? GOWL_BAR_COLOR_TEXT
		: (quality > 0.3) ? GOWL_BAR_COLOR_YELLOW
		: GOWL_BAR_COLOR_RED);
}

static const GowlBarPluginVTable wifi_vtable = {
	sizeof(GowlBarPluginVTable),
	net_create, net_destroy,
	NULL, NULL, NULL,
	ip_interval, wifi_poll, NULL,
	NULL, NULL,
	NULL, NULL,
	net_panel, net_panel_action,
	NULL, NULL
};

/* ----------------------------------------------------------------
 * vpn
 * ---------------------------------------------------------------- */

static void
vpn_poll(GowlBarPlugin *plugin, gpointer data)
{
	gboolean up;

	(void)data;

	/* The kernel is the authority here: a tun or wireguard interface
	   exists or it does not, whatever created it. */
	up = g_file_test("/sys/class/net/tun0", G_FILE_TEST_IS_DIR) ||
	     g_file_test("/sys/class/net/wg0", G_FILE_TEST_IS_DIR) ||
	     g_file_test("/sys/class/net/proton0", G_FILE_TEST_IS_DIR);

	gowl_bar_plugin_set_label(plugin, up ? "VPN" : NULL);
	gowl_bar_plugin_set_icon(plugin, up ? "\xef\x82\xa3" : NULL);
	gowl_bar_plugin_set_color(plugin,
		up ? GOWL_BAR_COLOR_GREEN : GOWL_BAR_COLOR_MUTED);
}

static const GowlBarPluginVTable vpn_vtable = {
	sizeof(GowlBarPluginVTable),
	NULL, NULL, NULL, NULL, NULL,
	ip_interval, vpn_poll, NULL,
	NULL, NULL, NULL, NULL,
	NULL, NULL,
	NULL, NULL
};

/* ----------------------------------------------------------------
 * tailscale
 * ---------------------------------------------------------------- */

typedef struct {
	gboolean installed;
	gboolean active;
	gboolean needs_login;
	/* Whether this host has ever joined a tailnet.  Installed is not
	   the same question --- Immutablue ships Tailscale on every host,
	   so a widget keyed on the binary being present would sit grey
	   and permanent on machines that do not use it. */
	gboolean joined;
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
	g_free(td->exit_node);
	g_free(td->status_text);
	g_free(td->last_error);
	g_ptr_array_unref(td->peers);
	g_ptr_array_unref(td->exit_nodes);
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
static gboolean
ts_collect_peer(const gchar *obj, guint index, gpointer user_data)
{
	TailscaleData *td = user_data;
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

static void
ts_parse_peers(TailscaleData *td, const gchar *json)
{
	g_ptr_array_set_size(td->peers, 0);
	g_ptr_array_set_size(td->exit_nodes, 0);
	g_clear_pointer(&td->exit_node, g_free);

	gowl_bar_json_foreach_object(json, "Peer", ts_collect_peer, td);
}

/*
 * Whether the widget belongs in this bar at all.
 *
 * `show' is `auto' (the default), `always' or `never'.  Under `auto'
 * the widget appears only on a host that has actually joined a
 * tailnet, because the alternative --- keying on the binary being
 * installed --- puts a permanently grey icon on every Immutablue
 * machine, Tailscale being part of the image.
 *
 * A host that has joined keeps the widget whether the tailnet is up or
 * down: down is a state worth seeing on a machine that uses Tailscale,
 * and it is one click from the switch that fixes it.
 *
 * The plugin keeps polling while invisible, so a `tailscale up' on a
 * fresh host makes the widget appear on the next poll rather than at
 * the next login.
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
		visible = (td->installed && td->joined);

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

	td->installed = bar_have_command("tailscale");
	if (!td->installed) {
		td->joined = FALSE;
		ts_apply_visibility(plugin, td);
		return;
	}

	json = bar_run_argv(argv);
	if (json == NULL) {
		/* The daemon is not answering.  Whether that is worth a
		   widget depends on whether this host uses Tailscale at
		   all, which the last successful poll already told us. */
		td->active = FALSE;
		ts_apply_visibility(plugin, td);
		if (gowl_bar_plugin_get_visible(plugin)) {
			gowl_bar_plugin_set_icon(plugin, "\xef\x95\x82");
			gowl_bar_plugin_set_label(plugin,
				gowl_bar_plugin_get_setting_bool(plugin,
					"labels", FALSE)
				? "tailscale" : NULL);
			gowl_bar_plugin_set_color(plugin,
			                          GOWL_BAR_COLOR_MUTED);
		}
		return;
	}

	state = gowl_bar_json_string(json, "BackendState");
	td->active      = (g_strcmp0(state, "Running") == 0);
	td->needs_login = (g_strcmp0(state, "NeedsLogin") == 0);

	/* HaveNodeKey is the honest "is this a tailnet member" flag: it
	   stays true across `tailscale down' and goes false only on a
	   host that has never joined or has been logged out. */
	td->joined = gowl_bar_json_bool(json, "HaveNodeKey", FALSE) ||
	             td->active;

	ts_apply_visibility(plugin, td);
	if (!gowl_bar_plugin_get_visible(plugin))
		return;

	g_free(td->status_text);
	td->status_text = g_strdup((state != NULL) ? state : "Unknown");

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
			g_free(td->self_name);
			td->self_name = (dns != NULL && dns[0] != '\0')
				? g_strdup(dns) : g_strdup(host);

			g_free(td->self_ip);
			td->self_ip = NULL;
			ips = gowl_bar_json_string_array(self, "TailscaleIPs",
			                                 NULL);
			if (ips != NULL) {
				gint i;

				for (i = 0; ips[i] != NULL; i++) {
					if (g_str_has_prefix(ips[i], "100.")) {
						g_free(td->self_ip);
						td->self_ip =
							g_strdup(ips[i]);
						break;
					}
					if (td->self_ip == NULL)
						td->self_ip =
							g_strdup(ips[i]);
				}
			}
		}
	}

	ts_parse_peers(td, json);

	gowl_bar_plugin_set_icon(plugin, "\xef\x95\x82");
	if (gowl_bar_plugin_get_setting_bool(plugin, "labels", FALSE)) {
		gowl_bar_plugin_set_label(plugin,
			td->active ? (td->self_name != NULL ? td->self_name
			                                    : "tailscale")
			           : "off");
	} else {
		gowl_bar_plugin_set_label(plugin, NULL);
	}
	gowl_bar_plugin_set_color(plugin,
		td->active ? GOWL_BAR_COLOR_TEAL
		: td->needs_login ? GOWL_BAR_COLOR_YELLOW
		: GOWL_BAR_COLOR_MUTED);

	/* Say it once when a host that *is* a tailnet member has lapsed --
	   an expired session is actionable and easy to miss.  A host that
	   never joined gets nothing: the widget is not even shown there,
	   so a toast pointing at its panel would point at nothing. */
	if (td->needs_login && td->joined &&
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
	if (!td->joined) {
		/* Reachable only with `show: always' --- under `auto' the
		   widget is not in the bar to be clicked. */
		gowl_bar_panel_add_hero(panel, "\xef\x95\x82", "Tailscale",
		                        "Not on a tailnet");
		gowl_bar_panel_add_label(panel,
			"Run `tailscale up' to join one. The widget appears "
			"on its own once you have.");
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

	return panel;
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

	if (g_strcmp0(item_id, "login") == 0) {
		gowl_bar_plugin_spawn(plugin, "tailscale up");
		gowl_bar_plugin_notify(plugin, GOWL_BAR_TOAST_NORMAL,
			"Tailscale sign-in started",
			"Follow the link tailscale printed to finish "
			"joining the tailnet.");
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
			gowl_bar_plugin_refresh_panel(plugin);
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
	NULL, NULL
};

/* ----------------------------------------------------------------
 * Registration
 * ---------------------------------------------------------------- */

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
	gowl_bar_registry_register_vtable(registry, "tailscale", "Tailscale",
		"Tailnet state, peers and exit nodes", &tailscale_vtable);

	gowl_bar_registry_register_alias(registry, "wlan", "wifi");
	gowl_bar_registry_register_alias(registry, "ts", "tailscale");
}
