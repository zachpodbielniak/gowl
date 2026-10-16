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
 * "Scan network": finding TVs without being told their addresses.
 *
 * Three sources, cheapest and most specific first:
 *
 *   SSDP     one multicast M-SEARCH per protocol; Roku, Samsung and LG
 *            all answer and name themselves.
 *   mDNS     `adb mdns services', for Android TVs advertising adb.
 *   probe    a TCP connect to the TV ports of every address in the
 *            local /24 -- the only way to find a Fire TV, which
 *            advertises nothing.  Bounded: a /24 at most, 64 attempts
 *            in flight, 400 ms each, so a whole scan is a few seconds
 *            on a worker and never a port scan of a corporate /16.
 *
 * All of it runs on one bar worker, with plain non-blocking sockets and
 * poll(): no GLib main context is involved at any point.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <gio/gio.h>

#include "bar-tvremote-backend.h"

#define SSDP_ADDR "239.255.255.250"
#define SSDP_PORT (1900)

/**
 * bar_tvremote_found_free:
 * @f: (nullable): a scan result
 */
void
bar_tvremote_found_free(BarTvremoteFound *f)
{
	if (f == NULL)
		return;
	g_free(f->host);
	g_free(f->how);
	g_free(f);
}

static BarTvremoteFound *
found_new(BarTvremoteKind kind, const gchar *host, gint port, const gchar *how)
{
	BarTvremoteFound *f;

	f = g_new0(BarTvremoteFound, 1);
	f->kind = kind;
	f->host = g_strdup(host);
	f->port = port;
	f->how = g_strdup(how);
	return f;
}

/**
 * bar_tvremote_detect_subnet:
 *
 * The /24 (or narrower) of the interface the default route uses: read
 * from /proc/net/route, the address and mask from getifaddrs().  With
 * no default route, the first non-loopback IPv4 interface.
 *
 * Returns: (transfer full) (nullable): a CIDR, or %NULL with no IPv4
 */
gchar *
bar_tvremote_detect_subnet(void)
{
	g_autofree gchar *route = NULL;
	g_autofree gchar *iface = NULL;
	struct ifaddrs *list = NULL, *ifa;
	gchar *cidr = NULL;
	guint pass;

	if (g_file_get_contents("/proc/net/route", &route, NULL, NULL))
		iface = bar_tvremote_route_iface(route);
	if (getifaddrs(&list) != 0)
		return NULL;

	/* Pass 0: the default-route interface.  Pass 1: anything. */
	for (pass = 0; pass < 2 && cidr == NULL; pass++) {
		for (ifa = list; ifa != NULL; ifa = ifa->ifa_next) {
			struct sockaddr_in *a, *m;

			if (ifa->ifa_addr == NULL || ifa->ifa_netmask == NULL ||
			    ifa->ifa_addr->sa_family != AF_INET ||
			    (ifa->ifa_flags & IFF_LOOPBACK) != 0)
				continue;
			if (pass == 0 && g_strcmp0(ifa->ifa_name, iface) != 0)
				continue;
			a = (struct sockaddr_in *)(gpointer)ifa->ifa_addr;
			m = (struct sockaddr_in *)(gpointer)ifa->ifa_netmask;
			cidr = bar_tvremote_subnet_for(ntohl(a->sin_addr.s_addr),
			                               ntohl(m->sin_addr.s_addr));
			break;
		}
	}
	freeifaddrs(list);
	return cidr;
}

typedef struct {
	gint         fd;
	const gchar *host;
	gint         port;
} Probe;

/* Finish one batch of in-flight connects: whatever said POLLOUT with
   no socket error within @timeout_ms is open. */
static void
probe_batch(Probe *batch, guint n, gint timeout_ms, GPtrArray *found)
{
	struct pollfd *fds;
	gint64 deadline;
	guint i, pending = n;
	gboolean *done;

	fds = g_new0(struct pollfd, n);
	done = g_new0(gboolean, n);
	for (i = 0; i < n; i++) {
		fds[i].fd = batch[i].fd;
		fds[i].events = POLLOUT;
		if (batch[i].fd < 0) {
			done[i] = TRUE;
			pending--;
		}
	}
	deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
	while (pending > 0) {
		gint64 left = (deadline - g_get_monotonic_time()) / 1000;
		gint r;

		if (left <= 0)
			break;
		for (i = 0; i < n; i++)
			fds[i].fd = done[i] ? -1 : batch[i].fd;
		r = poll(fds, n, (gint)left);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		for (i = 0; i < n; i++) {
			gint err = 0;
			socklen_t len = sizeof(err);

			if (done[i] || fds[i].revents == 0)
				continue;
			done[i] = TRUE;
			pending--;
			if (getsockopt(batch[i].fd, SOL_SOCKET, SO_ERROR, &err,
			               &len) == 0 && err == 0) {
				g_autofree gchar *how = NULL;

				how = g_strdup_printf("port %d", batch[i].port);
				g_ptr_array_add(found, found_new(
					bar_tvremote_kind_for_port(batch[i].port),
					batch[i].host, batch[i].port, how));
			}
		}
	}
	for (i = 0; i < n; i++)
		if (batch[i].fd >= 0)
			close(batch[i].fd);
	g_free(fds);
	g_free(done);
}

/**
 * bar_tvremote_scan_ports:
 * @hosts: (element-type utf8): IPv4 addresses
 * @ports: (array length=n_ports): the ports to try on each
 * @n_ports: how many
 * @timeout_ms: how long one connect may take
 * @concurrency: how many attempts are in flight at once
 *
 * Returns: (transfer full) (element-type BarTvremoteFound): every
 *   host:port that accepted a connection
 */
GPtrArray *
bar_tvremote_scan_ports(GPtrArray *hosts, const gint *ports, guint n_ports,
                        gint timeout_ms, guint concurrency)
{
	GPtrArray *found;
	Probe *batch;
	guint n = 0, h, p;

	found = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_tvremote_found_free);
	if (hosts == NULL || n_ports == 0)
		return found;
	concurrency = CLAMP(concurrency, 1, 256);
	batch = g_new0(Probe, concurrency);

	for (h = 0; h < hosts->len; h++) {
		for (p = 0; p < n_ports; p++) {
			struct sockaddr_in sa;
			gint fd;

			memset(&sa, 0, sizeof(sa));
			sa.sin_family = AF_INET;
			sa.sin_port = htons((guint16)ports[p]);
			if (inet_pton(AF_INET, g_ptr_array_index(hosts, h),
			              &sa.sin_addr) != 1)
				continue;
			fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK |
			                     SOCK_CLOEXEC, 0);
			if (fd >= 0 &&
			    connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 &&
			    errno != EINPROGRESS) {
				close(fd);
				fd = -1;
			}
			batch[n].fd = fd;
			batch[n].host = g_ptr_array_index(hosts, h);
			batch[n].port = ports[p];
			n++;
			if (n == concurrency) {
				probe_batch(batch, n, timeout_ms, found);
				n = 0;
			}
		}
	}
	if (n > 0)
		probe_batch(batch, n, timeout_ms, found);
	g_free(batch);
	return found;
}

/**
 * bar_tvremote_scan_ssdp:
 * @timeout_ms: how long to listen for answers
 *
 * Returns: (transfer full) (element-type BarTvremoteFound): the TVs
 *   that answered an M-SEARCH for Roku, Samsung or LG
 */
GPtrArray *
bar_tvremote_scan_ssdp(gint timeout_ms)
{
	static const gchar *targets[] = {
		"roku:ecp",
		"urn:samsung.com:device:RemoteControlReceiver:1",
		"urn:lge-com:service:webos-second-screen:1"
	};
	GPtrArray *found;
	struct sockaddr_in dst;
	gint64 deadline;
	gint fd, ttl = 2;
	guint i;

	found = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_tvremote_found_free);
	fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return found;
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons(SSDP_PORT);
	inet_pton(AF_INET, SSDP_ADDR, &dst.sin_addr);
	for (i = 0; i < G_N_ELEMENTS(targets); i++) {
		g_autofree gchar *req = bar_tvremote_ssdp_request(targets[i]);

		sendto(fd, req, strlen(req), 0, (struct sockaddr *)&dst,
		       sizeof(dst));
	}

	deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
	for (;;) {
		struct pollfd pfd;
		gchar buf[2048];
		gssize n;
		gint64 left = (deadline - g_get_monotonic_time()) / 1000;
		g_autofree gchar *location = NULL;
		g_autofree gchar *st = NULL;
		g_autofree gchar *server = NULL;
		g_autofree gchar *host = NULL;
		BarTvremoteKind kind;
		gint port;

		if (left <= 0)
			break;
		pfd.fd = fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, (gint)left) <= 0)
			continue;
		n = recv(fd, buf, sizeof(buf) - 1, 0);
		if (n <= 0)
			continue;
		buf[n] = '\0';
		if (!bar_tvremote_ssdp_parse(buf, &location, &st, &server))
			continue;
		kind = bar_tvremote_ssdp_kind(st, server);
		if (kind == BAR_TVREMOTE_KIND_UNKNOWN ||
		    !bar_tvremote_url_host_port(location, &host, &port))
			continue;
		/* LG's location is its DIAL port, not SSAP's. */
		g_ptr_array_add(found, found_new(kind, host,
			bar_tvremote_kind_default_port(kind), "SSDP"));
	}
	close(fd);
	return found;
}

/* Add @f to @out unless its host is already there; takes @f. */
static void
merge_found(GPtrArray *out, BarTvremoteFound *f)
{
	guint i;

	for (i = 0; i < out->len; i++) {
		BarTvremoteFound *o = g_ptr_array_index(out, i);

		if (g_strcmp0(o->host, f->host) == 0) {
			bar_tvremote_found_free(f);
			return;
		}
	}
	g_ptr_array_add(out, f);
}

/* Move every element of @from into @out (deduplicated by host). */
static void
merge_all(GPtrArray *out, GPtrArray *from)
{
	while (from->len > 0)
		merge_found(out, g_ptr_array_steal_index(from, 0));
	g_ptr_array_unref(from);
}

/**
 * bar_tvremote_scan:
 * @t: the tools (for `adb mdns')
 * @subnet: (nullable): the network to probe, or %NULL / "auto"
 * @error: return location for a #GError
 *
 * SSDP first, then mDNS, then the port probe; a host found by an
 * earlier source keeps that source's (more specific) kind.
 *
 * Returns: (transfer full) (nullable) (element-type BarTvremoteFound):
 *   what was found, or %NULL with @error when the subnet is unusable
 */
GPtrArray *
bar_tvremote_scan(const BarTvremoteTools *t, const gchar *subnet,
                  GError **error)
{
	static const gint ports[] = { 5555, 8060, 8002, 8001, 3001, 3000 };
	g_autofree gchar *cidr = NULL;
	g_autoptr(GPtrArray) hosts = NULL;
	GPtrArray *out;

	if (subnet == NULL || subnet[0] == '\0' ||
	    g_ascii_strcasecmp(subnet, "auto") == 0)
		cidr = bar_tvremote_detect_subnet();
	else
		cidr = g_strdup(subnet);
	if (cidr == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE,
		            "no IPv4 network to scan; set scan-subnet");
		return NULL;
	}
	hosts = bar_tvremote_subnet_hosts(cidr);
	if (hosts == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		            "scan-subnet %s is not an IPv4 /24 or narrower", cidr);
		return NULL;
	}

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)bar_tvremote_found_free);
	merge_all(out, bar_tvremote_scan_ssdp(2000));

	if (t != NULL && bar_tvremote_have(t->adb)) {
		const gchar *argv[] = { t->adb, "mdns", "services", NULL };
		g_autofree gchar *mdns = NULL;

		if (bar_tvremote_run(argv, 4000, &mdns, NULL, NULL, NULL)) {
			g_autoptr(GPtrArray) addrs = bar_tvremote_parse_mdns(mdns);
			guint i;

			for (i = 0; i < addrs->len; i++) {
				const gchar *addr = g_ptr_array_index(addrs, i);
				const gchar *colon = strrchr(addr, ':');
				g_autofree gchar *host = NULL;

				host = g_strndup(addr, (gsize)(colon - addr));
				merge_found(out, found_new(BAR_TVREMOTE_KIND_ANDROIDTV,
				                           host,
				                           (gint)g_ascii_strtoll(colon + 1,
				                                                 NULL, 10),
				                           "mDNS"));
			}
		}
	}

	merge_all(out, bar_tvremote_scan_ports(hosts, ports,
	                                       G_N_ELEMENTS(ports), 400, 64));
	return out;
}
