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
 * test-macro-targeted-key.c - Keys sent to one window, from its side.
 *
 * A macro that types into "the terminal" while you work in the browser
 * is only right if the terminal gets the keys, the browser gets none,
 * and the browser has the keyboard again afterwards -- with nothing a
 * unit test of the compositor can see.  So this is a real Wayland
 * client with two toplevels and a wl_keyboard, recording which surface
 * each key arrived on.
 *
 * Covers: a targeted tap reaches the target only; focus is given back
 * to the window that had it; text with shifted characters, Return and
 * characters the keymap has no key for; refusal while locked; the macro
 * API's key and text steps on a hostless context; find_client by
 * title/app-id; reorder_clients and the macro sort helper.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>

#include "macro-test-client.h"
#include "macro/gowl-macro-private.h"

static const gchar * const two_windows[] = {
	"alpha-term", "beta-browser", NULL
};

/* ── Tests ──────────────────────────────────────────────────────── */

static void
tap_reaches_only_the_target(void)
{
	Rig r;
	gint i;

	if (!rig_up(&r, two_windows, NULL, NULL)) {
		rig_down(&r);
		return;
	}
	focus_window(&r, 1);                   /* working in the browser */
	reset_keys(&r);

	g_assert_true(gowl_compositor_send_key_to_client(r.compositor, r.win[0],
	                                                 KEY_A, 0));
	g_assert_cmpint(await_keys(&r, 2), ==, 2);
	for (i = 0; i < 2; i++) {
		g_assert_cmpint(r.client.keys[i].window, ==, 0);
		g_assert_cmpuint(r.client.keys[i].key, ==, KEY_A);
	}
	g_assert_cmpuint(r.client.keys[0].state, ==, WL_KEYBOARD_KEY_STATE_PRESSED);
	g_assert_cmpuint(r.client.keys[1].state, ==,
	                 WL_KEYBOARD_KEY_STATE_RELEASED);
	/* the browser has the keyboard again, compositor-side too */
	g_assert_cmpint(focused_now(&r), ==, 1);
	g_assert_true(r.compositor->wlr_seat->keyboard_state.focused_surface
	              == gowl_client_get_wlr_surface(r.win[1]));
	g_assert_true(gowl_compositor_get_focused_client(r.compositor)
	              == r.win[1]);

	/* with a modifier held for the tap only */
	reset_keys(&r);
	g_assert_true(gowl_compositor_send_key_to_client(
		r.compositor, r.win[0], KEY_C, GOWL_KEY_MOD_CTRL));
	g_assert_cmpint(await_keys(&r, 2), ==, 2);
	g_assert_cmpuint(r.client.keys[0].mods, !=, 0);
	g_assert_cmpint(r.client.keys[0].window, ==, 0);
	g_assert_cmpint(focused_now(&r), ==, 1);
	g_assert_cmpuint(r.client.mods, ==, 0);

	/* sending to the focused window needs no focus dance */
	reset_keys(&r);
	g_assert_true(gowl_compositor_send_key_to_client(r.compositor, r.win[1],
	                                                 KEY_B, 0));
	g_assert_cmpint(await_keys(&r, 2), ==, 2);
	g_assert_cmpint(r.client.keys[0].window, ==, 1);
	rig_down(&r);
}

static void
text_reaches_the_target(void)
{
	Rig r;
	gint sent;
	gint n;

	if (!rig_up(&r, two_windows, NULL, NULL)) {
		rig_down(&r);
		return;
	}
	focus_window(&r, 1);
	reset_keys(&r);

	/* H needs shift; the snowman has no key in a us layout: skipped */
	sent = gowl_compositor_send_text_to_client(r.compositor, r.win[0],
	                                           "Hi\xe2\x98\x83\n");
	g_assert_cmpint(sent, ==, 3);
	n = await_keys(&r, 6);
	g_assert_cmpint(n, ==, 6);
	g_assert_cmpuint(r.client.keys[0].key, ==, KEY_H);
	g_assert_cmpuint(r.client.keys[0].mods, !=, 0);       /* shift */
	g_assert_cmpuint(r.client.keys[2].key, ==, KEY_I);
	g_assert_cmpuint(r.client.keys[2].mods, ==, 0);
	g_assert_cmpuint(r.client.keys[4].key, ==, KEY_ENTER);
	g_assert_cmpint(r.client.keys[4].window, ==, 0);
	g_assert_cmpint(focused_now(&r), ==, 1);

	g_assert_cmpint(gowl_compositor_send_text_to_client(r.compositor,
	                                                    r.win[0],
	                                                    "\xff\xfe"), ==, -1);
	rig_down(&r);
}

static void
refused_while_locked(void)
{
	Rig r;

	if (!rig_up(&r, two_windows, NULL, NULL)) {
		rig_down(&r);
		return;
	}
	focus_window(&r, 1);
	reset_keys(&r);
	r.compositor->locked = TRUE;
	g_assert_false(gowl_compositor_send_key_to_client(r.compositor,
	                                                  r.win[0], KEY_A, 0));
	g_assert_cmpint(gowl_compositor_send_text_to_client(r.compositor,
	                                                    r.win[0], "x"),
	                ==, -1);
	r.compositor->locked = FALSE;
	g_assert_cmpint(await_keys(&r, 1), ==, 0);
	g_assert_cmpint(focused_now(&r), ==, 1);
	rig_down(&r);
}

/* The macro API's steps, on a context with no host: executed at once. */
static void
macro_steps_and_helpers(void)
{
	Rig r;
	g_autoptr(GowlMacroContext) ctx = NULL;
	GowlClient *found;

	if (!rig_up(&r, two_windows, NULL, NULL)) {
		rig_down(&r);
		return;
	}
	focus_window(&r, 1);
	reset_keys(&r);
	ctx = gowl_macro_context_new(r.compositor, "test", NULL, NULL,
	                             GOWL_MACRO_TRIGGER_API, NULL, FALSE);

	found = gowl_macro_find_client(ctx, "title:alpha-term");
	g_assert_true(found == r.win[0]);
	g_assert_true(gowl_macro_find_client(ctx, "app-id:beta-browser")
	              == r.win[1]);
	g_assert_true(gowl_macro_find_client(ctx, "beta-browser") == r.win[1]);
	g_assert_null(gowl_macro_find_client(ctx, "title:nothing"));

	g_assert_true(gowl_macro_key(ctx, found, "Shift+x"));
	g_assert_true(gowl_macro_text(ctx, found, "ok"));
	{
		/* the parser warns about the bad combo; that is the point */
		GLogLevelFlags mask = g_log_set_always_fatal(G_LOG_LEVEL_ERROR);

		g_assert_false(gowl_macro_key(ctx, found, "Hyper+NoSuchKey"));
		g_log_set_always_fatal(mask);
	}
	g_assert_cmpint(await_keys(&r, 6), ==, 6);
	g_assert_cmpuint(r.client.keys[0].key, ==, KEY_X);
	g_assert_cmpuint(r.client.keys[0].mods, !=, 0);
	g_assert_cmpuint(r.client.keys[2].key, ==, KEY_O);
	g_assert_cmpuint(r.client.keys[4].key, ==, KEY_K);
	g_assert_cmpint(r.client.keys[5].window, ==, 0);
	g_assert_cmpint(focused_now(&r), ==, 1);
	rig_down(&r);
}

static gint
by_title_desc(
	gconstpointer a,
	gconstpointer b
){
	return -g_strcmp0(gowl_client_get_title((GowlClient *)a),
	                  gowl_client_get_title((GowlClient *)b));
}

static void
reorder_and_sort(void)
{
	Rig r;
	g_autoptr(GowlMacroContext) ctx = NULL;
	GList *order = NULL;
	GList *l;
	GowlClient *first;

	if (!rig_up(&r, two_windows, NULL, NULL)) {
		rig_down(&r);
		return;
	}
	/* whichever order they are in, swap them */
	l = gowl_compositor_get_clients(r.compositor);
	first = l->data;
	order = g_list_append(order, l->next->data);
	order = g_list_append(order, first);
	order = g_list_append(order, NULL);             /* ignored */
	order = g_list_append(order, first);            /* repeat ignored */
	gowl_compositor_reorder_clients(r.compositor, order);
	g_list_free(order);
	l = gowl_compositor_get_clients(r.compositor);
	g_assert_cmpuint(g_list_length(l), ==, 2);
	g_assert_true(l->next->data == first);

	/* the macro helper: descending by title puts beta first */
	ctx = gowl_macro_context_new(r.compositor, "sort", NULL, NULL,
	                             GOWL_MACRO_TRIGGER_API, NULL, FALSE);
	order = gowl_macro_list_clients(ctx, FALSE);
	g_assert_cmpuint(g_list_length(order), ==, 2);
	gowl_macro_sort_clients(ctx, order, by_title_desc);
	g_list_free(order);
	l = gowl_compositor_get_clients(r.compositor);
	g_assert_cmpstr(gowl_client_get_title(l->data), ==, "beta-browser");
	g_assert_cmpstr(gowl_client_get_title(l->next->data), ==, "alpha-term");
	pump(&r, 20);
	rig_down(&r);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/macro/targeted-key/tap-reaches-only-the-target",
	                tap_reaches_only_the_target);
	g_test_add_func("/macro/targeted-key/text-reaches-the-target",
	                text_reaches_the_target);
	g_test_add_func("/macro/targeted-key/refused-while-locked",
	                refused_while_locked);
	g_test_add_func("/macro/targeted-key/macro-steps-and-helpers",
	                macro_steps_and_helpers);
	g_test_add_func("/macro/targeted-key/reorder-and-sort",
	                reorder_and_sort);
	return g_test_run();
}
