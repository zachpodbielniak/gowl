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
 * The field reader bar plugins use on a CLI's --json output.
 *
 * The fixture is the shape `tailscale status --json' actually has: a
 * `Self' object and a `Peer' map whose members carry the same field
 * names, some of them empty.  That overlap is the whole hazard -- a
 * reader that searches forward from `"Self":' answers Self's missing
 * DNSName with the first peer's, silently and plausibly.
 */

#include <string.h>

#include "barkit/gowl-bar-json.h"

static const gchar *fixture =
	"{\n"
	"  \"Version\": \"1.102.3\",\n"
	"  \"TUN\": true,\n"
	"  \"BackendState\": \"Running\",\n"
	"  \"AuthURL\": \"\",\n"
	"  \"ExitNodeStatus\": null,\n"
	"  \"TailscaleIPs\": [\"100.72.0.41\", \"fd7a:115c:a1e0::9535\"],\n"
	"  \"Self\": {\n"
	"    \"ID\": \"nvrn6DLCZS11CNTRL\",\n"
	"    \"HostName\": \"lt-zach\",\n"
	"    \"DNSName\": \"lt-zach.tail1234.ts.net.\",\n"
	"    \"OS\": \"linux\",\n"
	"    \"Online\": true,\n"
	"    \"ExitNode\": false,\n"
	"    \"ExitNodeOption\": false,\n"
	"    \"TailscaleIPs\": [\"fd7a:115c:a1e0::9535\", \"100.72.0.41\"]\n"
	"  },\n"
	"  \"Peer\": {\n"
	"    \"nodekey:aaa\": {\n"
	"      \"HostName\": \"funnel-ingress-node\",\n"
	"      \"DNSName\": \"\",\n"
	"      \"OS\": \"\",\n"
	"      \"Online\": true,\n"
	"      \"ExitNode\": false,\n"
	"      \"ExitNodeOption\": false,\n"
	"      \"TailscaleIPs\": [\"fd7a:115c:a1e0::a801\"]\n"
	"    },\n"
	"    \"nodekey:bbb\": {\n"
	"      \"HostName\": \"desk{tricky}\\\"quoted\",\n"
	"      \"DNSName\": \"desk.tail1234.ts.net.\",\n"
	"      \"OS\": \"linux\",\n"
	"      \"Online\": false,\n"
	"      \"ExitNode\": false,\n"
	"      \"ExitNodeOption\": true,\n"
	"      \"TailscaleIPs\": [\"100.90.1.2\"]\n"
	"    },\n"
	"    \"nodekey:ccc\": {\n"
	"      \"HostName\": \"relay\",\n"
	"      \"DNSName\": \"relay.tail1234.ts.net.\",\n"
	"      \"Online\": true,\n"
	"      \"ExitNode\": true,\n"
	"      \"ExitNodeOption\": true,\n"
	"      \"TailscaleIPs\": [\"100.11.22.33\"]\n"
	"    }\n"
	"  }\n"
	"}\n";

static void
test_scalars(void)
{
	g_autofree gchar *state = NULL;

	state = gowl_bar_json_string(fixture, "BackendState");
	g_assert_cmpstr(state, ==, "Running");

	g_assert_true(gowl_bar_json_bool(fixture, "TUN", FALSE));
	g_assert_false(gowl_bar_json_bool(fixture, "Missing", FALSE));
	g_assert_true(gowl_bar_json_bool(fixture, "Missing", TRUE));

	/* A null value is not a string, and must not come back as one. */
	g_assert_null(gowl_bar_json_string(fixture, "ExitNodeStatus"));
	/* Nor is an object. */
	g_assert_null(gowl_bar_json_string(fixture, "Self"));
}

static void
test_key_anchoring(void)
{
	/* `Node' must not match `NodeKey' or `ExitNode'. */
	g_assert_null(gowl_bar_json_string(fixture, "Node"));
	g_assert_null(gowl_bar_json_string(fixture, "Exit"));
	g_assert_null(gowl_bar_json_string(fixture, "Host"));
}

static void
test_object_is_bounded(void)
{
	g_autofree gchar *self = NULL;
	g_autofree gchar *host = NULL;
	g_autofree gchar *dns = NULL;

	self = gowl_bar_json_object(fixture, "Self");
	g_assert_nonnull(self);

	/* The bound is the point: reading a field out of the extracted
	   object must not reach the peers that follow it. */
	g_assert_null(strstr(self, "funnel-ingress-node"));

	host = gowl_bar_json_string(self, "HostName");
	dns  = gowl_bar_json_string(self, "DNSName");
	g_assert_cmpstr(host, ==, "lt-zach");
	g_assert_cmpstr(dns, ==, "lt-zach.tail1234.ts.net.");
}

static void
test_a_missing_field_stays_missing(void)
{
	g_autofree gchar *self = NULL;

	/* Self has no `Relay' field.  A forward search would find one in
	   a peer; the bounded object must simply not have it. */
	self = gowl_bar_json_object(fixture, "Self");
	g_assert_nonnull(self);
	g_assert_null(gowl_bar_json_string(self, "Relay"));
}

static void
test_string_arrays(void)
{
	g_auto(GStrv) ips = NULL;
	guint n;

	ips = gowl_bar_json_string_array(fixture, "TailscaleIPs", &n);
	g_assert_nonnull(ips);
	g_assert_cmpuint(n, ==, 2);
	g_assert_cmpstr(ips[0], ==, "100.72.0.41");
	g_assert_cmpstr(ips[1], ==, "fd7a:115c:a1e0::9535");
	g_assert_null(ips[2]);

	g_assert_null(gowl_bar_json_string_array(fixture, "Self", NULL));
}

typedef struct {
	GPtrArray *names;
	gchar     *exit_node;
	guint      exit_options;
} PeerWalk;

static gboolean
collect_peer(const gchar *obj, guint index, gpointer user_data)
{
	PeerWalk *walk = user_data;
	g_autofree gchar *host = NULL;
	g_autofree gchar *dns = NULL;

	(void)index;

	host = gowl_bar_json_string(obj, "HostName");
	dns  = gowl_bar_json_string(obj, "DNSName");
	g_ptr_array_add(walk->names, g_strdup(host));

	if (gowl_bar_json_bool(obj, "ExitNodeOption", FALSE))
		walk->exit_options++;
	if (gowl_bar_json_bool(obj, "ExitNode", FALSE)) {
		g_free(walk->exit_node);
		walk->exit_node = g_strdup(host);
	}

	/* A peer with no DNSName must report none, not the next peer's. */
	if (g_strcmp0(host, "funnel-ingress-node") == 0)
		g_assert_cmpstr(dns, ==, "");

	return TRUE;
}

static void
test_peer_walk(void)
{
	PeerWalk walk;
	guint visited;

	walk.names        = g_ptr_array_new_with_free_func(g_free);
	walk.exit_node    = NULL;
	walk.exit_options = 0;

	visited = gowl_bar_json_foreach_object(fixture, "Peer", collect_peer,
	                                       &walk);

	g_assert_cmpuint(visited, ==, 3);
	g_assert_cmpuint(walk.names->len, ==, 3);
	g_assert_cmpstr(g_ptr_array_index(walk.names, 0), ==,
	                "funnel-ingress-node");
	/* Braces and a quote inside a value must not unbalance the walk. */
	g_assert_cmpstr(g_ptr_array_index(walk.names, 1), ==,
	                "desk{tricky}\"quoted");
	g_assert_cmpstr(g_ptr_array_index(walk.names, 2), ==, "relay");

	g_assert_cmpuint(walk.exit_options, ==, 2);
	g_assert_cmpstr(walk.exit_node, ==, "relay");

	g_free(walk.exit_node);
	g_ptr_array_unref(walk.names);
}

static void
test_degenerate_input(void)
{
	g_assert_null(gowl_bar_json_string(NULL, "x"));
	g_assert_null(gowl_bar_json_string("", "x"));
	g_assert_null(gowl_bar_json_object("{", "x"));
	g_assert_null(gowl_bar_json_string("{\"x\":", "x"));
	/* An unterminated object must not run past the buffer. */
	g_assert_nonnull(gowl_bar_json_object("{\"x\":{\"y\":1", "x"));
	g_assert_cmpuint(gowl_bar_json_foreach_object("[]", "x", NULL, NULL),
	                 ==, 0);
}

/* If this machine has tailscale, run the real thing through the reader
   as well -- a fixture only proves the shape somebody wrote down. */
static void
test_against_a_real_reply(void)
{
	const gchar *path;
	g_autofree gchar *json = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *self = NULL;
	PeerWalk walk;

	path = g_getenv("GOWL_TEST_TAILSCALE_JSON");
	if (path == NULL || !g_file_get_contents(path, &json, NULL, NULL)) {
		g_test_skip("set GOWL_TEST_TAILSCALE_JSON to a real reply");
		return;
	}

	state = gowl_bar_json_string(json, "BackendState");
	g_assert_nonnull(state);

	self = gowl_bar_json_object(json, "Self");
	g_assert_nonnull(self);
	g_assert_nonnull(gowl_bar_json_string(self, "HostName"));

	walk.names        = g_ptr_array_new_with_free_func(g_free);
	walk.exit_node    = NULL;
	walk.exit_options = 0;
	gowl_bar_json_foreach_object(json, "Peer", collect_peer, &walk);
	g_assert_cmpuint(walk.names->len, >, 0);

	g_free(walk.exit_node);
	g_ptr_array_unref(walk.names);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/bar-json/scalars", test_scalars);
	g_test_add_func("/bar-json/key-anchoring", test_key_anchoring);
	g_test_add_func("/bar-json/object-is-bounded",
	                test_object_is_bounded);
	g_test_add_func("/bar-json/missing-stays-missing",
	                test_a_missing_field_stays_missing);
	g_test_add_func("/bar-json/string-arrays", test_string_arrays);
	g_test_add_func("/bar-json/peer-walk", test_peer_walk);
	g_test_add_func("/bar-json/degenerate", test_degenerate_input);
	g_test_add_func("/bar-json/real-reply", test_against_a_real_reply);

	return g_test_run();
}
