/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "core/gowl-core-private.h"

static guint protocol_closes;
static guint requests;

/* Record the protocol boundary without needing a connected client. */
void
wlr_xdg_toplevel_send_close(struct wlr_xdg_toplevel *toplevel)
{
	protocol_closes++;
}

static gboolean
close_requested(GowlCompositor *comp, GowlClient *client, gpointer data)
{
	requests++;
	return GPOINTER_TO_INT(data);
}

static void
test_close_policy(void)
{
	GowlCompositor *comp = gowl_compositor_new();
	GowlClient *client = g_object_new(GOWL_TYPE_CLIENT, NULL);
	struct wlr_xdg_toplevel toplevel = {0};
	gulong guard, observer;

	client->compositor = comp;
	client->xdg_toplevel = &toplevel;
	/* Standalone defaults retain the usual protocol close. */
	gowl_client_close(client);
	g_assert_cmpuint(protocol_closes, ==, 1);
	guard = g_signal_connect(comp, "client-close-request",
	                         G_CALLBACK(close_requested), GINT_TO_POINTER(TRUE));
	observer = g_signal_connect(comp, "client-close-request",
	                            G_CALLBACK(close_requested), GINT_TO_POINTER(FALSE));
	gowl_client_close(client);
	gowl_client_close(client);
	g_assert_cmpuint(protocol_closes, ==, 1);
	g_assert_cmpuint(requests, ==, 2); /* Veto stops the other handler too. */
	g_signal_handler_disconnect(comp, guard);
	gowl_client_close(client);
	g_assert_cmpuint(protocol_closes, ==, 2);
	g_assert_cmpuint(requests, ==, 3);
	g_signal_handler_disconnect(comp, observer);
	client->xdg_toplevel = NULL;
	client->compositor = NULL;
	g_object_unref(client);
	g_object_unref(comp);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/client/close-policy", test_close_policy);
	return g_test_run();
}
