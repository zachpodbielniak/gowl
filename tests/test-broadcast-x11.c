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
 * test-broadcast-x11.c - Keys into X11 windows that do not have focus.
 *
 * Games under Wine/Proton are X11 windows, and Xwayland gives every key
 * it receives to the window holding X INPUT focus, whatever wl_surface
 * the seat entered.  So seat delivery alone never reaches a background
 * X11 window -- this file shows that, as a fact the AUTO route is built
 * on -- and delivery has to move X focus, confirm the server agrees,
 * send, confirm the key was processed, and move focus back
 * (src/core/gowl-xwayland-sync.c).  Without the confirmations both
 * halves race: measured, activate-then-send loses the key to the old
 * window about half the time.
 *
 * Every assertion is about what the X SERVER delivered: KeyPress events
 * selected on each window, and GetInputFocus.
 */

#include <glib.h>
#include <linux/input-event-codes.h>

#ifdef GOWL_HAVE_XWAYLAND

#include <json-glib/json-glib.h>
#include <wlr/interfaces/wlr_keyboard.h>

#include "x11-rig.h"

#define N_WIN (3)

typedef struct {
	X11Rig               r;
	xcb_window_t         win[N_WIN];
	GowlClient          *c[N_WIN];
	struct wlr_keyboard *kb;
	gint                 presses[N_WIN];
	gint                 releases[N_WIN];
	gint                 last[N_WIN];   /* last event: 1 press, 0 release, -1 none */
} Scene;

static struct wlr_keyboard_impl fake_keyboard_impl;

static void
select_keys(
	X11Rig       *r,
	xcb_window_t  w
){
	uint32_t v = XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE
	             | XCB_EVENT_MASK_FOCUS_CHANGE
	             | XCB_EVENT_MASK_STRUCTURE_NOTIFY;

	xcb_change_window_attributes(r->conn, w, XCB_CW_EVENT_MASK, &v);
	xcb_flush(r->conn);
}

/* Counts key events per window for @ms, pumping the compositor. */
static void
collect(
	Scene *s,
	gint   ms
){
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;
	gint i;

	memset(s->presses, 0, sizeof s->presses);
	memset(s->releases, 0, sizeof s->releases);
	for (i = 0; i < N_WIN; i++)
		s->last[i] = -1;
	while (g_get_monotonic_time() < deadline) {
		xcb_generic_event_t *ev;

		x11_rig_pump(&s->r, 5);
		while ((ev = xcb_poll_for_event(s->r.conn)) != NULL) {
			guint t = ev->response_type & 0x7f;

			if (t == XCB_KEY_PRESS || t == XCB_KEY_RELEASE) {
				xcb_key_press_event_t *k = (xcb_key_press_event_t *)ev;

				for (i = 0; i < N_WIN; i++) {
					if (k->event != s->win[i])
						continue;
					if (t == XCB_KEY_PRESS)
						s->presses[i]++;
					else
						s->releases[i]++;
					s->last[i] = t == XCB_KEY_PRESS ? 1 : 0;
				}
			}
			free(ev);
		}
	}
}

static gboolean
scene_up(Scene *s)
{
	static const gchar * const titles[N_WIN] = {
		"EverQuest - Tank", "EverQuest - Cleric", "EverQuest - Wizard"
	};
	gint i;

	memset(s, 0, sizeof *s);
	if (!x11_rig_up(&s->r, "x11-broadcast"))
		return FALSE;
	for (i = 0; i < N_WIN; i++) {
		s->win[i] = x11_rig_create_toplevel(&s->r, titles[i],
		                                    0, 0, 200, 150);
		select_keys(&s->r, s->win[i]);
		s->c[i] = x11_rig_map_managed(&s->r, s->win[i]);
		if (s->c[i] == NULL) {
			g_test_skip("an X11 toplevel never mapped here");
			x11_rig_down(&s->r);
			return FALSE;
		}
	}
	gowl_compositor_focus_client(s->r.compositor, s->c[0], TRUE);
	if (!x11_rig_await_input_focus(&s->r, s->win[0])) {
		g_test_skip("the X server never granted focus");
		x11_rig_down(&s->r);
		return FALSE;
	}
	x11_rig_pump(&s->r, 100);
	x11_rig_drain(&s->r);
	return TRUE;
}

static void
scene_down(Scene *s)
{
	if (s->kb != NULL) {
		wlr_keyboard_finish(s->kb);
		g_free(s->kb);
		s->kb = NULL;
	}
	x11_rig_down(&s->r);
}

/* --- the primitive --- */

static void
seat_route_misses_x11(void)
{
	Scene s;

	if (!scene_up(&s))
		return;
	/*
	 * The fact the whole X11 path exists for: entering B's surface and
	 * sending a key, the Wayland way, puts it in A -- the window with X
	 * focus.  If this ever changes (an Xwayland that routes by surface),
	 * the activate dance becomes unnecessary; this test says so.
	 */
	g_assert_true(gowl_compositor_deliver_key(s.r.compositor, s.c[1],
	              KEY_W, TRUE, -1, GOWL_KEY_DELIVERY_SEAT));
	g_assert_true(gowl_compositor_deliver_key(s.r.compositor, s.c[1],
	              KEY_W, FALSE, -1, GOWL_KEY_DELIVERY_SEAT));
	collect(&s, 200);
	g_assert_cmpint(s.presses[1], ==, 0);
	g_assert_cmpint(s.presses[0], ==, 1);
	scene_down(&s);
}

static void
activate_route_reaches_x11(void)
{
	Scene s;
	gint round;

	if (!scene_up(&s))
		return;
	/* Several rounds: a race shows up as a key in the wrong window
	 * sooner or later, and one lucky pass proves nothing. */
	for (round = 0; round < 10; round++) {
		g_assert_true(gowl_compositor_deliver_key(s.r.compositor, s.c[1],
		              KEY_W, TRUE, -1, GOWL_KEY_DELIVERY_AUTO));
		g_assert_true(gowl_compositor_deliver_key(s.r.compositor, s.c[1],
		              KEY_W, FALSE, -1, GOWL_KEY_DELIVERY_AUTO));
		collect(&s, 60);
		/* At least one press, and exactly one release, in B.  An
		 * X server has one key state: the press is undone for the
		 * window focus returns to, so the release finds the key up
		 * and is preceded by a press -- autorepeat, to B. */
		g_assert_cmpint(s.presses[1], >=, 1);
		g_assert_cmpint(s.releases[1], ==, 1);
		g_assert_cmpint(s.last[1], ==, 0);
		g_assert_cmpint(s.presses[0], ==, 0);
		g_assert_cmpint(s.presses[2], ==, 0);
		/* and X focus is back where it was, confirmed */
		g_assert_cmpuint(x11_rig_input_focus(&s.r), ==, s.win[0]);
	}
	scene_down(&s);
}

static void
batch_restores_once(void)
{
	Scene s;

	if (!scene_up(&s))
		return;
	gowl_compositor_input_batch_begin(s.r.compositor);
	g_assert_true(gowl_compositor_deliver_key(s.r.compositor, s.c[1],
	              KEY_E, TRUE, -1, GOWL_KEY_DELIVERY_AUTO));
	g_assert_true(gowl_compositor_deliver_key(s.r.compositor, s.c[2],
	              KEY_E, TRUE, -1, GOWL_KEY_DELIVERY_AUTO));
	gowl_compositor_input_batch_end(s.r.compositor);
	gowl_compositor_input_batch_begin(s.r.compositor);
	g_assert_true(gowl_compositor_deliver_key(s.r.compositor, s.c[1],
	              KEY_E, FALSE, -1, GOWL_KEY_DELIVERY_AUTO));
	g_assert_true(gowl_compositor_deliver_key(s.r.compositor, s.c[2],
	              KEY_E, FALSE, -1, GOWL_KEY_DELIVERY_AUTO));
	gowl_compositor_input_batch_end(s.r.compositor);
	collect(&s, 150);
	/* Each got the key and ended with it released.  A target that
	 * finds the shared key already down is sent a release before its
	 * press, so the count of releases is not exact; the end state is
	 * what counts. */
	g_assert_cmpint(s.presses[1], >=, 1);
	g_assert_cmpint(s.presses[2], >=, 1);
	g_assert_cmpint(s.last[1], ==, 0);
	g_assert_cmpint(s.last[2], ==, 0);
	g_assert_cmpint(s.presses[0], ==, 0);
	g_assert_cmpuint(x11_rig_input_focus(&s.r), ==, s.win[0]);
	scene_down(&s);
}

static void
macro_text_reaches_x11(void)
{
	Scene s;

	if (!scene_up(&s))
		return;
	/* What the macro module's targeted steps call: before the X11
	 * route existed this typed into the FOCUSED window. */
	g_assert_cmpint(gowl_compositor_send_text_to_client(s.r.compositor,
	                s.c[2], "eq"), ==, 2);
	g_assert_true(gowl_compositor_send_key_to_client(s.r.compositor,
	              s.c[2], KEY_ENTER, 0));
	collect(&s, 150);
	g_assert_cmpint(s.presses[2], >=, 3);
	g_assert_cmpint(s.last[2], ==, 0);
	g_assert_cmpint(s.presses[0], ==, 0);
	g_assert_cmpint(s.presses[1], ==, 0);
	g_assert_cmpuint(x11_rig_input_focus(&s.r), ==, s.win[0]);
	scene_down(&s);
}

/* --- the module, end to end --- */

static gchar *
command(
	Scene       *s,
	const gchar *line
){
	gchar *reply = gowl_compositor_run_command(s->r.compositor, line);

	g_assert_nonnull(reply);
	return reply;
}

static void
key(
	Scene   *s,
	guint32  code,
	gboolean pressed
){
	struct wlr_keyboard_key_event ev;

	memset(&ev, 0, sizeof ev);
	ev.time_msec = 1;
	ev.keycode = code;
	ev.update_state = TRUE;
	ev.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
	                   : WL_KEYBOARD_KEY_STATE_RELEASED;
	wlr_keyboard_notify_key(s->kb, &ev);
}

static void
module_broadcasts_between_x11(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *team = NULL;
	g_autofree gchar *rule = NULL;
	g_autofree gchar *on = NULL;
	Scene s;
	gint round;

	if (!scene_up(&s))
		return;

	/* the real module, enabled after start, as cmacs enables it */
	if (!gowl_module_manager_load_module(s.r.modules,
	                                     GOWL_TEST_BROADCAST_MODULE,
	                                     &error)) {
		g_test_skip("broadcast.so did not load");
		scene_down(&s);
		return;
	}
	gowl_module_manager_activate_all(s.r.modules);
	gowl_module_manager_dispatch_startup(s.r.modules, s.r.compositor);

	fake_keyboard_impl.name = "gowl-test-keyboard";
	s.kb = g_new0(struct wlr_keyboard, 1);
	wlr_keyboard_init(s.kb, &fake_keyboard_impl, "test-kb");
	wl_signal_emit_mutable(&s.r.compositor->backend->events.new_input,
	                       &s.kb->base);
	x11_rig_pump(&s.r, 20);

	team = command(&s, "broadcast-add-team {name: eq, slots: ["
	                   "{name: tank, match: 'title ~ Tank'}, "
	                   "{name: cleric, match: 'title ~ Cleric'}, "
	                   "{name: wizard, match: 'title ~ Wizard'}]}");
	g_assert_true(g_str_has_prefix(team, "OK"));
	rule = command(&s, "broadcast-add-rule {name: follow, team: eq, "
	                   "keys: ['1-9']}");
	g_assert_true(g_str_has_prefix(rule, "OK"));
	on = command(&s, "broadcast-on");
	g_assert_true(g_str_has_prefix(on, "OK"));
	x11_rig_drain(&s.r);

	for (round = 0; round < 5; round++) {
		key(&s, KEY_1, TRUE);
		key(&s, KEY_1, FALSE);
		collect(&s, 100);
		/* the local window exactly once; both others got the key and
		 * ended with it released */
		g_assert_cmpint(s.presses[0], ==, 1);
		g_assert_cmpint(s.releases[0], ==, 1);
		g_assert_cmpint(s.last[0], ==, 0);
		g_assert_cmpint(s.presses[1], >=, 1);
		g_assert_cmpint(s.presses[2], >=, 1);
		g_assert_cmpint(s.last[1], ==, 0);
		g_assert_cmpint(s.last[2], ==, 0);
		g_assert_cmpuint(x11_rig_input_focus(&s.r), ==, s.win[0]);
	}
	gowl_module_manager_dispatch_shutdown(s.r.modules, s.r.compositor);
	scene_down(&s);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/broadcast/x11/seat-route-misses-x11",
	                seat_route_misses_x11);
	g_test_add_func("/broadcast/x11/activate-route-reaches-x11",
	                activate_route_reaches_x11);
	g_test_add_func("/broadcast/x11/batch-restores-once",
	                batch_restores_once);
	g_test_add_func("/broadcast/x11/macro-text-reaches-x11",
	                macro_text_reaches_x11);
	g_test_add_func("/broadcast/x11/module-broadcasts-between-x11",
	                module_broadcasts_between_x11);
	return g_test_run();
}

#else /* !GOWL_HAVE_XWAYLAND */

static void
skipped(void)
{
	g_test_skip("built without Xwayland");
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/broadcast/x11/skipped", skipped);
	return g_test_run();
}

#endif /* GOWL_HAVE_XWAYLAND */
